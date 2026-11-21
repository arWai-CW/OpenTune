#include "ProcessRenderRuntime.h"

#include "../DSP/AutoTunePeriodDetector.h"
#include "../DSP/AutoTunePitchShifter.h"
#include "../DSP/MelSpectrogram.h"
#include "../DSP/NoteEqProcessor.h"
#include "../Inference/ChunkRenderStrategy.h"
#include "../Inference/RenderCache.h"
#include "../Render/RenderChunkPlanner.h"
#include "../Utils/AppLogger.h"
#include "../Utils/AppPreferences.h"
#include "../Utils/ChannelLayoutLogger.h"
#include "../Utils/ModelPathResolver.h"
#include "../Utils/PitchCurve.h"
#include "../Utils/TimeCoordinate.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>

namespace OpenTune {

namespace {

// ==============================================================================
// Effective F0 Materialization
// ==============================================================================
// 将 contentSnap 的 forEachEffectiveF0Span 复制与 gain 应用封装为文件内函数，
// 消除 leading lookback、trailing lookahead、主 chunk 三处完全相同的逻辑。

std::vector<float> materializeEffectiveF0Range(
    const EditableContentSnapshot& contentSnap, int startFrame, int endFrame)
{
    const int rangeLength = std::max(0, endFrame - startFrame);
    std::vector<float> effectiveF0(static_cast<size_t>(rangeLength), 0.0f);
    contentSnap.forEachEffectiveF0Span(startFrame, endFrame,
        [&effectiveF0, startFrame](int frameIndex, const float* values, int spanLength, float gain)
        {
            const int offset = frameIndex - startFrame;
            for (int k = 0; k < spanLength; ++k)
                effectiveF0[static_cast<size_t>(offset + k)] = values[static_cast<size_t>(k)] * gain;
        });
    return effectiveF0;
}

// ==============================================================================
// F0 Gap Filling for Vocoder (Mel Frame Space)
// ==============================================================================
// 对齐训练代码 interp_uv=True（utils/wav2F0.interp_f0）：
//   在 log2 频域对所有 unvoiced（f0==0）帧做 np.interp 等价线性插值：
//   - 内部间隙：log2 域左右 voiced 帧线性插值（无大小限制）
//   - 前导零帧：flat clamp 到第一个 voiced 帧的 log2(F0)
//   - 尾部零帧：flat clamp 到最后一个 voiced 帧的 log2(F0)
//   - 全零输入：保持全零
//
// 旧实现有两大不一致：(1) 内部间隙 ≤50 帧限制（训练无此限制）；
// (2) 前导/尾部通过 contentSnap 边界查询 + 几何均值延伸（训练只做 flat clamp）。
// 这两处偏差导致 vocoder 拿到的 F0 与训练不一致，产生相位震荡（低频砰砰声）。
void fillF0GapsForVocoder(std::vector<float>& f0)
{
    if (f0.empty()) return;

    const int n = static_cast<int>(f0.size());

    // 收集所有 voiced 帧的索引和 log2(F0) 值
    std::vector<int> voicedIdx;
    std::vector<float> voicedLogF0;
    voicedIdx.reserve(n);
    voicedLogF0.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        if (f0[static_cast<size_t>(i)] > 0.0f)
        {
            voicedIdx.push_back(i);
            voicedLogF0.push_back(std::log2(f0[static_cast<size_t>(i)]));
        }
    }

    // 全部 unvoiced → 保持全零（对齐 interp_f0 中 uv.all() 分支）
    if (voicedIdx.empty()) return;

    // 对每个 unvoiced 帧在 log2 域做线性插值
    // 等价于 np.interp(x_unvoiced, x_voiced, f0_log2_voiced)
    for (int i = 0; i < n; ++i)
    {
        if (f0[static_cast<size_t>(i)] > 0.0f) continue;

        auto it = std::lower_bound(voicedIdx.begin(), voicedIdx.end(), i);

        if (it == voicedIdx.begin())
        {
            // 前导零帧：flat clamp 到第一个 voiced F0
            f0[static_cast<size_t>(i)] = std::pow(2.0f, voicedLogF0.front());
        }
        else if (it == voicedIdx.end())
        {
            // 尾部零帧：flat clamp 到最后一个 voiced F0
            f0[static_cast<size_t>(i)] = std::pow(2.0f, voicedLogF0.back());
        }
        else
        {
            // 内部零帧：log2 域线性插值
            const int rightIdx = *it;
            const int leftIdx = *(it - 1);
            const float rightLogF0 = voicedLogF0[static_cast<size_t>(it - voicedIdx.begin())];
            const float leftLogF0 = voicedLogF0[static_cast<size_t>(it - voicedIdx.begin() - 1)];
            const float t = static_cast<float>(i - leftIdx) / static_cast<float>(rightIdx - leftIdx);
            f0[static_cast<size_t>(i)] = std::pow(2.0f, leftLogF0 + t * (rightLogF0 - leftLogF0));
        }
    }
}

struct ContentSampleRange
{
    int64_t startSample{0};
    int64_t endSampleExclusive{0};

    bool isValid() const noexcept
    {
        return endSampleExclusive > startSample;
    }
};

struct FrozenRenderBoundaries
{
    int64_t trueStartSample{0};
    int64_t trueEndSample{0};
    int64_t synthEndSample{0};
    int64_t publishSampleCount{0};
    int64_t synthSampleCount{0};
    int frameCount{0};
    int hopSize{0};
};

bool freezeRenderBoundaries(const ContentSampleRange& contentRange,
                            int64_t startSample,
                            int64_t endSampleExclusive,
                            int hopSize,
                            FrozenRenderBoundaries& out)
{
    out = FrozenRenderBoundaries{};

    if (!contentRange.isValid() || hopSize <= 0)
        return false;

    out.trueStartSample = juce::jlimit(contentRange.startSample, contentRange.endSampleExclusive, startSample);
    out.trueEndSample = juce::jlimit(out.trueStartSample, contentRange.endSampleExclusive, endSampleExclusive);
    out.publishSampleCount = out.trueEndSample - out.trueStartSample;
    if (out.publishSampleCount <= 0)
    {
        out = FrozenRenderBoundaries{};
        return false;
    }

    const bool isLastChunk = out.trueEndSample == contentRange.endSampleExclusive;
    if (!isLastChunk && (out.publishSampleCount % hopSize) != 0)
    {
        out = FrozenRenderBoundaries{};
        return false;
    }

    out.frameCount = juce::jmax(1, static_cast<int>((out.publishSampleCount + hopSize - 1) / hopSize));
    out.synthSampleCount = isLastChunk ? static_cast<int64_t>(out.frameCount) * hopSize
                                       : out.publishSampleCount;
    out.synthEndSample = out.trueStartSample + out.synthSampleCount;
    out.hopSize = hopSize;
    return true;
}

bool preparePublishedAudioFromSynthesis(const FrozenRenderBoundaries& boundaries,
                                        const std::vector<float>& synthesizedAudio,
                                        std::vector<float>& publishedAudio)
{
    publishedAudio.clear();

    if (boundaries.publishSampleCount <= 0 || boundaries.synthSampleCount <= 0)
        return false;

    if (synthesizedAudio.size() != static_cast<size_t>(boundaries.synthSampleCount))
        return false;

    publishedAudio.assign(synthesizedAudio.begin(),
                          synthesizedAudio.begin() + static_cast<size_t>(boundaries.publishSampleCount));
    return true;
}

void notifyChunkSettled(const ProcessRenderRuntime::CompletionContext& completion,
                        const RenderJob& job)
{
    if (!completion.gate || !completion.chunkSettled)
        return;
    std::lock_guard<std::mutex> lk(completion.gate->mutex);
    if (!completion.gate->closed)
        completion.chunkSettled(job.contentKey,
                                job.contentSnapshot,
                                job.audioBuffer,
                                job.audioSampleRate);
}

void notifyChunkSettled(const ProcessRenderRuntime::CompletionContext& completion,
                        ContentKey key,
                        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
                        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
                        double audioSampleRate)
{
    if (!completion.gate || !completion.chunkSettled)
        return;
    std::lock_guard<std::mutex> lk(completion.gate->mutex);
    if (!completion.gate->closed)
        completion.chunkSettled(key,
                                std::move(contentSnapshot),
                                std::move(audioBuffer),
                                audioSampleRate);
}

void notifyChunkFailed(const ProcessRenderRuntime::CompletionContext& completion,
                       ContentKey key)
{
    if (!completion.gate || !completion.chunkFailed)
        return;
    std::lock_guard<std::mutex> lk(completion.gate->mutex);
    if (!completion.gate->closed)
        completion.chunkFailed(key);
}

void failChunk(const ProcessRenderRuntime::CompletionContext& completion,
               RenderCache* cache,
               int64_t startSample,
               uint64_t revision,
               uint64_t generation,
               ContentKey key)
{
    if (cache != nullptr && cache->completeChunkRenderFailure(
            startSample, revision, generation))
        notifyChunkFailed(completion, key);
}

// ==============================================================================
// Per-note EQ 唯一发布（契约 §4/§6/§7）
// ==============================================================================
// 所有 Stage1 最终 Note 音频结果（轻量、原始、Vocoder）在写入缓存前统一经
// 应用 per-note EQ，位于音量包络前（包络只在播放端
// applyAutomationGain 应用）。notes 只在本调用栈内读取，不复制、不存储。
// 每个 active Note 使用局部 NoteEqProcessor，prepare 按固定渲染率计算系数并从
// Note 起点重置状态，只处理该 Note 在完整 chunk 内的精确样本范围。
// RenderChunkPlanner 已保证 active-EQ Note 不跨 chunk；Stage1 音频固定 44.1kHz。
constexpr int kNoteEqBoundaryFadeSamples = 12;

// chunk 发布范围 [trueStartSample, trueEndSample) 是否与任何 active-EQ Note
// 相交（用于原始路径 Blank/失败条件中的 EQ 排除，契约 §6）。
// 秒域边界按 RenderCache::kSampleRate 换算，与接入音频的样本域一致。
bool chunkIntersectsActiveEqNote(const std::vector<Note>& notes,
                                 const FrozenRenderBoundaries& boundaries)
{
    for (const auto& note : notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;

        const int64_t noteStartSample = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        if (noteStartSample >= boundaries.trueEndSample)
            break;  // notes 按起点升序

        const int64_t noteEndSample = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        if (noteEndSample > boundaries.trueStartSample)
            return true;
    }
    return false;
}

RenderCache::ChunkRenderResult publishChunkWithPerNoteEq(
    RenderCache& renderCache,
    const FrozenRenderBoundaries& boundaries,
    std::vector<float> audio,
    uint64_t revision,
    uint64_t generation,
    const std::vector<Note>& notes)
{
    for (const auto& note : notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;

        const int64_t noteStartSample = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        if (noteStartSample >= boundaries.trueEndSample)
            break;  // notes 按起点升序

        const int64_t noteEndSample = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        const int64_t rangeStart = std::max(noteStartSample, boundaries.trueStartSample);
        const int64_t rangeEnd = std::min(noteEndSample, boundaries.trueEndSample);
        if (rangeEnd <= rangeStart)
            continue;

        // planner 保证 active-EQ Note 不跨 chunk，因此处理器仅在本 Note 内存在，
        // 不需要共享状态或跨 chunk 缓存。
        NoteEqProcessor processor;
        processor.prepare(RenderCache::kSampleRate, *note.eq);

        const size_t audioOffset = static_cast<size_t>(rangeStart - boundaries.trueStartSample);
        const int noteSampleCount = static_cast<int>(rangeEnd - rangeStart);
        const bool fadeIn = rangeStart == noteStartSample;
        const bool fadeOut = rangeEnd == noteEndSample;

        if (!fadeIn && !fadeOut)
        {
            float* channelData[1] = { audio.data() + audioOffset };
            juce::AudioBuffer<float> noteBuffer(channelData, 1, noteSampleCount);
            processor.process(noteBuffer);
            continue;
        }

        std::vector<float> filtered(static_cast<size_t>(noteSampleCount));
        std::copy_n(audio.data() + audioOffset, noteSampleCount, filtered.data());
        float* filteredData[1] = { filtered.data() };
        juce::AudioBuffer<float> filteredBuffer(filteredData, 1, noteSampleCount);
        processor.process(filteredBuffer);

        const int fadeInSamples = std::min(kNoteEqBoundaryFadeSamples, noteSampleCount);
        const int fadeOutSamples = std::min(kNoteEqBoundaryFadeSamples, noteSampleCount);
        for (int i = 0; i < noteSampleCount; ++i)
        {
            float wetMix = 1.0f;
            if (fadeIn)
            {
                const float weight = fadeInSamples > 1
                    ? static_cast<float>(i) / static_cast<float>(fadeInSamples - 1)
                    : 0.0f;
                wetMix = std::min(wetMix, weight);
            }
            if (fadeOut)
            {
                const int distanceFromEnd = noteSampleCount - 1 - i;
                const float weight = fadeOutSamples > 1
                    ? static_cast<float>(distanceFromEnd) / static_cast<float>(fadeOutSamples - 1)
                    : 0.0f;
                wetMix = std::min(wetMix, weight);
            }

            const float dry = audio[audioOffset + static_cast<size_t>(i)];
            const float wet = filtered[static_cast<size_t>(i)];
            audio[audioOffset + static_cast<size_t>(i)] = dry + (wet - dry) * wetMix;
        }
    }

    return renderCache.completeChunkRenderWithAudio(
        boundaries.trueStartSample, boundaries.trueEndSample,
        std::move(audio), revision, generation);
}

} // namespace

ProcessRenderRuntime& ProcessRenderRuntime::getInstance()
{
    // 进程寿命 heap singleton：不注册静态析构（DLL detach 持 loader lock，
    // 不得在静态析构中 join 工作线程）。VST3 构建中模块已被 pin
    // （Vst3ModulePin.cpp），domain 与 control worker 存活到进程退出；
    // vocoderDomain_ 只经 setVocoderModelWeight() / resetVocoder() /
    // resetInferenceBackend() 在 control worker 上显式重建。
    static auto* instance = new ProcessRenderRuntime;
    return *instance;
}

ProcessRenderRuntime::ProcessRenderRuntime()
{
    // 启动前从持久化配置读取 vocoder 模型权重，确保首次 lazy 加载使用用户
    // 实际选择的权重（而非硬编码默认值）。
    const auto prefs = AppPreferences().getState().shared.vocoderModelWeight;
    const auto modelsDir = ModelPathResolver::getModelsDirectory();
    auto initialWeight = prefs;

    // 持久化权重可能已被新版本停止打包；只回退到项目保证提供的内置默认权重。
    // 默认权重也缺失时保留原值，让 coordinator 报出真实错误。
    const auto weightPath = [&modelsDir](const VocoderModelWeight& weight)
    {
        const juce::File preferred = juce::File(modelsDir)
            .getChildFile("vocoder_weights")
            .getChildFile(juce::String(weight));
        if (preferred.existsAsFile())
            return preferred.getFullPathName().toStdString();
        return modelsDir + "/" + weight;
    };

    const auto resolvedPath = weightPath(initialWeight);
    if (!juce::File(resolvedPath).existsAsFile()) {
        AppLogger::warn("[ProcessRenderRuntime] Persisted vocoder weight not found: "
            + juce::String(initialWeight) + " (" + resolvedPath + ")");

        if (initialWeight != kDefaultVocoderWeight) {
            const auto defaultPath = weightPath(kDefaultVocoderWeight);
            if (juce::File(defaultPath).existsAsFile()) {
                AppLogger::warn("[ProcessRenderRuntime] Falling back to default weight: "
                    + juce::String(kDefaultVocoderWeight));
                initialWeight = kDefaultVocoderWeight;
            }
        }
    }

    // Domain lifecycle、generation 與 control worker 由 coordinator 擁有；
    // ProcessRenderRuntime 只在 reconfigure 完成時接收 retry flush callback。
    vocoderExecution_ = std::make_unique<VocoderExecutionCoordinator>(
        std::move(initialWeight), modelsDir);
    vocoderExecution_->setReconfigureObserver(
        [this](bool domainAvailable)
        {
            flushDeferredRetries(domainAvailable);
        });
}

void ProcessRenderRuntime::setVocoderModelWeight(
    const VocoderModelWeight& weight, std::function<void()> completion)
{
    vocoderExecution_->setVocoderModelWeight(weight, std::move(completion));
}

void ProcessRenderRuntime::resetVocoder(std::function<void()> completion)
{
    vocoderExecution_->resetVocoder(std::move(completion));
}

void ProcessRenderRuntime::resetInferenceBackend(
    bool forceCpu, std::function<void()> completion)
{
    vocoderExecution_->resetInferenceBackend(forceCpu, std::move(completion));
}


bool ProcessRenderRuntime::submitVocoderJob(
    VocoderExecutionJob job, uint64_t expectedGeneration)
{
    return vocoderExecution_->submit(std::move(job), expectedGeneration);
}

bool ProcessRenderRuntime::isVocoderReady() const noexcept
{
    return vocoderExecution_->isReady();
}

bool ProcessRenderRuntime::isVocoderReconfiguring() const noexcept
{
    return vocoderExecution_->isReconfiguring();
}

void ProcessRenderRuntime::flushDeferredRetries(bool domainAvailable)
{
    std::vector<DeferredRetry> readyRetries;
    {
        std::lock_guard<std::mutex> lock(deferredRetryMutex_);
        readyRetries.swap(deferredRetries_);
    }

    for (auto& retry : readyRetries)
    {
        auto crsShared = retry.crs.lock();
        if (!crsShared)
            continue;

        if (domainAvailable)
        {
            crsShared->requeueRenderChunk(retry.job);
        }
        else if (retry.job.renderCache != nullptr)
        {
            retry.job.renderCache->completeChunkRenderFailure(
                retry.job.startSample, retry.job.targetRevision,
                retry.job.cacheGeneration);
        }
    }
}

void ProcessRenderRuntime::deferOrRequeue(
    std::shared_ptr<ContentRenderService> crs, RenderJob job)
{
    bool shouldDefer = false;
    {
        std::lock_guard<std::mutex> lock(deferredRetryMutex_);
        shouldDefer = vocoderExecution_->isReconfiguring();
        if (shouldDefer)
            deferredRetries_.push_back({crs, std::move(job)});
    }
    if (!shouldDefer)
        crs->requeueRenderChunk(std::move(job));
}

void ProcessRenderRuntime::processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                                                  RenderJob& job,
                                                  bool lightPitchEnabled,
                                                  CompletionContext completion)
{
    if (crs == nullptr || job.renderCache == nullptr || !job.contentSnapshot || !job.audioBuffer)
    {
        failChunk(completion, job.renderCache.get(), job.startSample,
                  job.targetRevision, job.cacheGeneration, job.contentKey);
        return;
    }

    const auto contentSnap = job.contentSnapshot;

    auto pitchCurve = contentSnap->pitchCurve;

    std::vector<float> monoAudio;
    std::vector<float> effectiveF0;
    std::vector<float> vocoderF0;
    std::vector<float> vocoderUv;

    const double relChunkStartSec = TimeCoordinate::samplesToSeconds(
        job.startSample, RenderCache::kSampleRate);
    auto coreJob = std::move(job);
    FrozenRenderBoundaries boundaries;
    int numFrames = 0;
    bool clipFound = false;
    bool boundariesFrozen = false;
    VocoderExecutionCoordinator::Config vocoderCfg;

    if (coreJob.audioBuffer != nullptr)
    {
        // 边界冻结使用唯一渲染 hop（RenderChunkPlanner::kRenderHopSize），与
        // Vocoder domain 配置无关；vocoder 配置延后到真实 mel/vocoder 分支前获取。
        const int audioNumSamples = coreJob.audioBuffer->getNumSamples();
        const int audioNumChannels = coreJob.audioBuffer->getNumChannels();

        ContentSampleRange contentRange{0, audioNumSamples};
        if (freezeRenderBoundaries(contentRange,
                                   coreJob.startSample,
                                   coreJob.endSampleExclusive,
                                   RenderChunkPlanner::kRenderHopSize,
                                   boundaries))
        {
            boundariesFrozen = true;
            if (audioNumChannels > 0)
            {
                numFrames = boundaries.frameCount;
                monoAudio.resize(static_cast<size_t>(boundaries.synthSampleCount), 0.0f);
                const float* ch0 = coreJob.audioBuffer->getReadPointer(0);
                for (int64_t i = 0; i < boundaries.publishSampleCount; ++i)
                    monoAudio[static_cast<size_t>(i)] = ch0[static_cast<int>(boundaries.trueStartSample + i)];
                ChannelLayoutLog::logChunkRender(
                    static_cast<juce::int64>(coreJob.contentKey.objectId),
                    audioNumChannels);
                clipFound = true;
            }
        }
    }

    // 契约 §6：边界冻结后立即计算当前 chunk 是否与 active-EQ Note 相交。
    // 带 active EQ 的 Note 不得走 Blank：原 Blank/失败条件中若相交则发布原始
    // monoAudio 的 publishSampleCount 部分并应用 EQ。
    const bool intersectsActiveEqNote = chunkIntersectsActiveEqNote(
        contentSnap->notes, boundaries);

    // 原始路径发布：monoAudio 的 publishSampleCount 部分 + per-note EQ（契约 §6）
    const auto publishRawWithEq = [&]() {
        std::vector<float> rawAudio(
            monoAudio.begin(),
            monoAudio.begin() + static_cast<size_t>(boundaries.publishSampleCount));
        const auto result = publishChunkWithPerNoteEq(
            *coreJob.renderCache, boundaries, std::move(rawAudio),
            coreJob.targetRevision, coreJob.cacheGeneration, contentSnap->notes);
        if (result == RenderCache::ChunkRenderResult::InvalidInput) {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        } else if (result == RenderCache::ChunkRenderResult::Published)
            notifyChunkSettled(completion, coreJob);
    };

    if (!clipFound || monoAudio.empty() || numFrames <= 0 || !boundariesFrozen)
    {
        // 无原始音频可发布：保持既有失败语义
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        return;
    }

    if (!pitchCurve)
    {
        if (!intersectsActiveEqNote)
        {
            failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                      coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
            return;
        }
        publishRawWithEq();
        return;
    }

    const double trueStartSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueStartSample,
                                                                     TimeCoordinate::kRenderSampleRate);
    const double trueEndSeconds = TimeCoordinate::samplesToSeconds(boundaries.trueEndSample,
                                                                   TimeCoordinate::kRenderSampleRate);
    const double hopDuration = static_cast<double>(boundaries.hopSize) / RenderCache::kSampleRate;

    if (!pitchCurve->hasOriginalF0Data())
    {
        if (!intersectsActiveEqNote)
        {
            if (coreJob.renderCache->markChunkAsBlank(
                    coreJob.startSample, coreJob.targetRevision, coreJob.cacheGeneration))
                notifyChunkSettled(completion, coreJob);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }

    const int f0HopSize = pitchCurve->getHopSize();
    const double f0SampleRate = pitchCurve->getSampleRate();
    if (f0HopSize <= 0 || f0SampleRate <= 0.0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        return;
    }

    const double f0FrameRate = f0SampleRate / static_cast<double>(f0HopSize);
    const F0Timeline f0Timeline(f0HopSize,
                                f0SampleRate,
                                static_cast<int>(pitchCurve->getOriginalF0().size()));
    const double f0TimelineEndSeconds = f0Timeline.timeAtFrame(
        static_cast<int>(pitchCurve->getOriginalF0().size()));
    if (trueEndSeconds <= 0.0 || trueStartSeconds >= f0TimelineEndSeconds)
    {
        if (!intersectsActiveEqNote)
        {
            if (coreJob.renderCache->markChunkAsBlank(
                    coreJob.startSample, coreJob.targetRevision, coreJob.cacheGeneration))
                notifyChunkSettled(completion, coreJob);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }
    const auto contentF0Range = f0Timeline.rangeForTimes(trueStartSeconds, trueEndSeconds);
    const int f0StartFrame = contentF0Range.startFrame;
    // One extra frame is operational interpolation context. It is not part of
    // the chunk's absolute content range or published output length.
    const int f0EndFrame = std::min(
        static_cast<int>(pitchCurve->getOriginalF0().size()),
        contentF0Range.endFrameExclusive + 1);
    const int numF0Frames = std::max(1, f0EndFrame - f0StartFrame);
    const double firstSampleFramePhase = trueStartSeconds * f0FrameRate - static_cast<double>(f0StartFrame);

    // Blank 判定：仅当全局移调为恒等（effectiveF0 与 originalF0 一致，无差异可
    // 合成）且该 chunk 帧范围内无任何 correction segment 时，才 Blank 回退原始
    // 音频缓存播放。非恒等全局移调即使无 correction 也必须进入 vocoder 全量渲染，
    // 否则移调不生效。带 active EQ 的 Note 不得走 Blank：改为发布原始音频并应用 EQ。
    if (contentSnap->pitchShiftSettings.isIdentity() && !pitchCurve->hasCorrectionInRange(f0StartFrame, f0EndFrame))
    {
        if (!intersectsActiveEqNote)
        {
            if (coreJob.renderCache->markChunkAsBlank(
                    coreJob.startSample, coreJob.targetRevision, coreJob.cacheGeneration))
                notifyChunkSettled(completion, coreJob);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }

    effectiveF0 = materializeEffectiveF0Range(*contentSnap, f0StartFrame, f0EndFrame);

    bool hasValidF0 = false;
    for (float f : effectiveF0)
    {
        if (f > 0.0f)
        {
            hasValidF0 = true;
            break;
        }
    }

    if (!hasValidF0)
    {
        if (!intersectsActiveEqNote)
        {
            if (coreJob.renderCache->markChunkAsBlank(
                    coreJob.startSample, coreJob.targetRevision, coreJob.cacheGeneration))
                notifyChunkSettled(completion, coreJob);
        }
        else
        {
            publishRawWithEq();
        }
        return;
    }

    if (lightPitchEnabled && contentSnap->pitchShiftSettings.isIdentity())
    {
        const auto& originalF0Full = pitchCurve->getOriginalF0();
        const int originalF0Size = static_cast<int>(originalF0Full.size());
        if (f0StartFrame >= 0 && f0StartFrame < originalF0Size)
        {
            const int safeNumF0Frames = std::min(numF0Frames, originalF0Size - f0StartFrame);
            std::vector<float> autoTuneTargetF0(static_cast<size_t>(safeNumF0Frames), 0.0f);
            pitchCurve->forEachCorrectionF0Span(
                f0StartFrame, f0StartFrame + safeNumF0Frames,
                [&autoTuneTargetF0, f0StartFrame](int frameIndex, const float* values, int spanLength)
                {
                    if (values == nullptr)
                        return;
                    const int offset = frameIndex - f0StartFrame;
                    for (int k = 0; k < spanLength; ++k)
                        autoTuneTargetF0[static_cast<size_t>(offset + k)] = values[static_cast<size_t>(k)];
                });
            const bool needsVocoder = chunkNeedsVocoder(
                autoTuneTargetF0.data(), safeNumF0Frames, originalF0Full, f0StartFrame);
            if (!needsVocoder)
            {
                AutoTunePitchShifter autoTuneShifter(RenderCache::kSampleRate);
                // Supply the source context needed by the pitch resampler:
                // preceding waveform history for cycle transitions and future
                // samples for aligned band-limited interpolation.
                float shifterLookahead[AutoTunePitchShifter::kLookaheadSamples] = {};
                int shifterLookaheadSamples = 0;
                const float* shifterLookbehind = nullptr;
                int shifterLookbehindSamples = 0;
                {
                    const int64_t clipNumSamples = coreJob.audioBuffer->getNumSamples();
                    const int64_t tailStart = boundaries.trueStartSample + boundaries.publishSampleCount;
                    const float* clipCh0 = coreJob.audioBuffer->getReadPointer(0);
                    const int64_t avail = std::clamp<int64_t>(
                        clipNumSamples - tailStart, 0, AutoTunePitchShifter::kLookaheadSamples);
                    shifterLookaheadSamples = static_cast<int>(avail);
                    for (int64_t i = 0; i < avail; ++i)
                        shifterLookahead[i] = clipCh0[tailStart + i];

                    // 周期检测器需要完整 coarse window 及其因果 FIR 历史，
                    // 其固定上下文长度由 detector 自身声明。
                    const int lookbehindCapacity =
                        AutoTunePeriodDetector::kRequiredLookbehindSamples
                        + AutoTunePitchShifter::kLookaheadSamples + 2;
                    const int64_t lookbehindStart = std::max<int64_t>(
                        0, boundaries.trueStartSample - lookbehindCapacity);
                    shifterLookbehind = clipCh0 + lookbehindStart;
                    shifterLookbehindSamples = static_cast<int>(
                        boundaries.trueStartSample - lookbehindStart);
                }

                // Chunk-local 双阶段周期检测：downsampled lag 粗搜 + full-rate
                // V=E-2H 局部精搜，逐样本输出 period/valid 影子源交给 shifter。
                // Locate the chunk and its lookbehind on the global F0 hint grid.
                const int detectorNumSamples =
                    static_cast<int>(boundaries.publishSampleCount);
                // UV/V is decided by the waveform detector itself.  RMVPE's
                // originalF0 remains the editable source/target track, but it
                // must not gate the AutoTune detector's periodicity analysis.
                const std::vector<AutoTunePeriodDetector::DetectedPeriod>
                    detectorFrames = AutoTunePeriodDetector::analyze(
                        shifterLookbehind, shifterLookbehindSamples,
                        monoAudio.data(), detectorNumSamples,
                        RenderCache::kSampleRate,
                        originalF0Full.data(), originalF0Size, f0FrameRate,
                        static_cast<double>(f0StartFrame) + firstSampleFramePhase);

                // snapMode：所有音符 retuneSpeed 均为 1.0 时，DSP 跳过 EMA 直接锁定
                bool snapMode = true;
                for (const auto& n : contentSnap->notes) {
                    if (n.retuneSpeed < 1.0f) { snapMode = false; break; }
                }

                auto repair = autoTuneShifter.shiftChunkWithOctaveRepair(
                    monoAudio.data(),
                    static_cast<int>(boundaries.publishSampleCount),
                    originalF0Full.data() + f0StartFrame,
                    autoTuneTargetF0.data(),
                    safeNumF0Frames,
                    f0FrameRate,
                    firstSampleFramePhase,
                    shifterLookahead,
                    shifterLookaheadSamples,
                    shifterLookbehind,
                    shifterLookbehindSamples,
                    detectorFrames.data(),
                    detectorNumSamples,
                    snapMode);

                if (!repair.success
                    || repair.audio.size() != static_cast<size_t>(boundaries.publishSampleCount))
                {
                    AppLogger::warn("RenderWorker: AutoTune pitch-shift contract not met, falling through to vocoder (startSample="
                        + juce::String(boundaries.trueStartSample)
                        + ", bestOctave=" + juce::String(repair.octave)
                        + ", targetErrorCents=" + juce::String(repair.targetErrorCents, 3)
                        + ", continuityCents=" + juce::String(repair.continuityCents, 3) + ")");
                }
                else
                {
                    const uint64_t objectId = coreJob.contentKey.objectId;
                    const auto result = publishChunkWithPerNoteEq(
                        *coreJob.renderCache, boundaries, std::move(repair.audio),
                        coreJob.targetRevision, coreJob.cacheGeneration,
                        contentSnap->notes);

                    if (result == RenderCache::ChunkRenderResult::InvalidInput)
                    {
                        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
                    }
                    else if (result == RenderCache::ChunkRenderResult::Published)
                    {
                        notifyChunkSettled(completion, coreJob);
                    }

                    AppLogger::debug("RenderWorker: AutoTune pitch-shift chunk objId="
                        + juce::String(static_cast<juce::int64>(objectId))
                        + " start=" + juce::String(relChunkStartSec, 3));
                    return;
                }
            }
        }
    }

    // 真实条件谱/vocoder 分支前唯一一次锁内获取 domain 配置
    // （generation/conditioningBins/conditioningType/fMax）：raw 四分支与 light
    // 路径已在上方早退，绝不触发模型加载。配置与提交校验的 generation 同属一个
    // domain，不存在跨域混用。
    if (!vocoderExecution_->acquireConfig(vocoderCfg))
    {
        if (isVocoderReconfiguring())
        {
            RenderJob requeueJob;
            requeueJob.kind = RenderJob::Kind::Stage1Render;
            requeueJob.contentKey = coreJob.contentKey;
            requeueJob.renderCache = coreJob.renderCache;
            requeueJob.contentSnapshot = contentSnap;
            requeueJob.audioBuffer = coreJob.audioBuffer;
            requeueJob.audioSampleRate = coreJob.audioSampleRate;
            requeueJob.startSample = coreJob.startSample;
            requeueJob.targetRevision = coreJob.targetRevision;
            requeueJob.cacheGeneration = coreJob.cacheGeneration;
            deferOrRequeue(crs, std::move(requeueJob));
            return;
        }
        AppLogger::log("RenderWorker: acquireVocoderConfig FAILED");
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        return;
    }

    const int conditioningBins = vocoderCfg.conditioningBins;
    const float fMax = vocoderCfg.fMax;
    if (conditioningBins <= 0)
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        return;
    }

    MelSpectrogramConfig conditioningConfig;
    conditioningConfig.sampleRate = static_cast<int>(RenderCache::kSampleRate);
    conditioningConfig.nMels = conditioningBins;
    conditioningConfig.fMax = fMax;

    const bool linearSpec = vocoderCfg.conditioningType == VocoderConditioningType::LogLinearSpec;
    auto conditioningResult = linearSpec
        ? computeLogLinearSpectrogram(monoAudio.data(),
                                      static_cast<int>(monoAudio.size()),
                                      numFrames,
                                      conditioningConfig)
        : computeLogMelSpectrogram(monoAudio.data(),
                                   static_cast<int>(monoAudio.size()),
                                   numFrames,
                                   conditioningConfig);
    if (!conditioningResult.ok() || conditioningResult.value().empty())
    {
        failChunk(completion, coreJob.renderCache.get(), coreJob.startSample,
                  coreJob.targetRevision, coreJob.cacheGeneration, coreJob.contentKey);
        return;
    }

    auto conditioning = std::move(conditioningResult).value();
    const int actualFrames = static_cast<int>(conditioning.size() / conditioningConfig.nMels);

    // Training applies interp_uv=True to the complete F0 timeline before the
    // vocoder sees it.  Interpolating only this render chunk misses voiced
    // frames on the other side of an all-unvoiced chunk.
    const auto& originalF0 = pitchCurve->getOriginalF0();
    auto vocoderSourceF0 = materializeEffectiveF0Range(
        *contentSnap, 0, static_cast<int>(originalF0.size()));
    // UV 必须在 gap-fill 之前采样：fillF0GapsForVocoder 按训练 interp_uv=True
    // 语义填满所有 unvoiced 帧，填完后无法再区分浊音/清音。线性谱声码器需要
    // 显式 UV 才能在清音段抑制谐波源。
    const auto preFillF0 = vocoderSourceF0;
    fillF0GapsForVocoder(vocoderSourceF0);

    vocoderF0.assign(static_cast<size_t>(actualFrames), 0.0f);
    vocoderUv.assign(static_cast<size_t>(actualFrames), 0.0f);
    for (int i = 0; i < actualFrames; ++i)
    {
        const double melTimeSec = trueStartSeconds + i * hopDuration;
        const double srcPos = melTimeSec * f0FrameRate - static_cast<double>(f0StartFrame);
        if (srcPos < 0.0)
            continue;
        const int srcIdx0 = static_cast<int>(srcPos);
        if (srcIdx0 >= numF0Frames)
            continue;
        const int srcIdx1 = std::min(srcIdx0 + 1, numF0Frames - 1);
        const double frac = srcPos - static_cast<double>(srcIdx0);
        const int globalIdx0 = f0StartFrame + srcIdx0;
        const int globalIdx1 = f0StartFrame + srcIdx1;
        const float f0_0 = globalIdx0 >= 0
            && globalIdx0 < static_cast<int>(vocoderSourceF0.size())
            ? vocoderSourceF0[static_cast<size_t>(globalIdx0)] : 0.0f;
        const float f0_1 = globalIdx1 >= 0
            && globalIdx1 < static_cast<int>(vocoderSourceF0.size())
            ? vocoderSourceF0[static_cast<size_t>(globalIdx1)] : 0.0f;
        if (f0_0 > 0.0f && f0_1 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] =
                static_cast<float>(std::exp(std::log(f0_0) * (1.0 - frac) + std::log(f0_1) * frac));
        else if (f0_0 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] = f0_0;
        else if (f0_1 > 0.0f)
            vocoderF0[static_cast<size_t>(i)] = f0_1;

        const bool voiced0 = globalIdx0 >= 0
            && globalIdx0 < static_cast<int>(preFillF0.size())
            && preFillF0[static_cast<size_t>(globalIdx0)] > 0.0f;
        const bool voiced1 = globalIdx1 >= 0
            && globalIdx1 < static_cast<int>(preFillF0.size())
            && preFillF0[static_cast<size_t>(globalIdx1)] > 0.0f;
        vocoderUv[static_cast<size_t>(i)] = (voiced0 || voiced1) ? 1.0f : 0.0f;
    }

    VocoderExecutionJob vocoderJob;
    vocoderJob.f0 = std::move(vocoderF0);
    vocoderJob.uv = std::move(vocoderUv);
    vocoderJob.conditioning = std::move(conditioning);

    auto renderCache = coreJob.renderCache;
    auto targetRevision = coreJob.targetRevision;
    const auto cacheGeneration = coreJob.cacheGeneration;
    auto requeueAudioBuffer = coreJob.audioBuffer;
    const double requeueAudioSampleRate = coreJob.audioSampleRate;
    const ContentKey captureContentKey = coreJob.contentKey;
    const uint64_t chunkObjId = captureContentKey.objectId;
    const int64_t jobStartSample = boundaries.trueStartSample;
    const FrozenRenderBoundaries frozenBoundaries = boundaries;

    vocoderJob.onComplete = [this, crs, renderCache, targetRevision,
                             cacheGeneration, requeueAudioBuffer, requeueAudioSampleRate,
                             captureContentKey, chunkObjId,
                             jobStartSample, frozenBoundaries, contentSnap,
                             completion](
                                 VocoderExecutionJobResult result,
                                 const juce::String& error,
                                 const std::vector<float>& audio)
    {
        struct AsyncRenderCompletion final
        {
            ContentRenderService& service;
            ~AsyncRenderCompletion() { service.completeAsyncRenderJob(); }
        } asyncCompletion{*crs};

        const auto& boundaries = frozenBoundaries;

        if (result == VocoderExecutionJobResult::Succeeded)
        {
            std::vector<float> publishedAudio;
            if (!preparePublishedAudioFromSynthesis(boundaries, audio, publishedAudio))
            {
                AppLogger::error("ChunkRender: synthesis length mismatch for RenderCache publish objId="
                    + juce::String(static_cast<juce::int64>(chunkObjId)));
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, cacheGeneration, captureContentKey);
                return;
            }

            const auto publishResult = publishChunkWithPerNoteEq(
                *renderCache, boundaries, std::move(publishedAudio), targetRevision,
                cacheGeneration, contentSnap->notes);

            if (publishResult == RenderCache::ChunkRenderResult::InvalidInput)
            {
                failChunk(completion, renderCache.get(), jobStartSample,
                          targetRevision, cacheGeneration, captureContentKey);
                return;
            }

            if (publishResult == RenderCache::ChunkRenderResult::Stale)
            {
                return;
            }

            notifyChunkSettled(completion,
                               captureContentKey,
                               contentSnap,
                               requeueAudioBuffer,
                               requeueAudioSampleRate);
        }
        else if (result == VocoderExecutionJobResult::Cancelled)
        {
            // Cancelled (scheduler shutdown, superseded, queue overflow): requeue or defer
            RenderJob requeueJob;
            requeueJob.kind = RenderJob::Kind::Stage1Render;
            requeueJob.contentKey = captureContentKey;
            requeueJob.renderCache = renderCache;
            requeueJob.contentSnapshot = contentSnap;
            requeueJob.audioBuffer = requeueAudioBuffer;
            requeueJob.audioSampleRate = requeueAudioSampleRate;
            requeueJob.startSample = jobStartSample;
            requeueJob.targetRevision = targetRevision;
            requeueJob.cacheGeneration = cacheGeneration;
            deferOrRequeue(crs, std::move(requeueJob));
        }
        else
        {
            // Failed: real inference error
            AppLogger::error("ChunkRender: vocoder failed objId="
                + juce::String(static_cast<juce::int64>(chunkObjId))
                + " error=" + error);
            failChunk(completion, renderCache.get(), jobStartSample,
                      targetRevision, cacheGeneration, captureContentKey);
        }
    };

    crs->beginAsyncRenderJob();
    if (!submitVocoderJob(std::move(vocoderJob), vocoderCfg.generation))
    {
        // stale generation or reconfiguring: deferOrRequeue handles both paths
        deferOrRequeue(crs, std::move(coreJob));
        crs->completeAsyncRenderJob();
        return;
    }
}

} // namespace OpenTune
