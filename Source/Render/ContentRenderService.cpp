#include "ContentRenderService.h"

#include "PlaybackReadSource.h"
#include "PlaybackSourcePublisher.h"
#include "RenderCacheRegistry.h"
#include "RenderChunkPlanner.h"
#include "RenderJob.h"
#include "RenderWorker.h"
#include "Stage2TimeStretchRebuilder.h"
#include "StretcherPool.h"
#include "../Inference/RenderCache.h"
#include "../Inference/SoundTouchStretcher.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/TimeCoordinate.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace OpenTune {

struct ContentRenderService::Impl
{
    struct ExecutorState
    {
        std::mutex mutex;
        void* owner{nullptr};
        std::function<void(RenderJob&)> stage1Callback;
    };

    ~Impl()
    {
        // 所有 async job 以 shared_ptr 持有 service；同步 worker callback 必須在
        // Impl 的 cache / stretcher / publisher 銷毀前完全退出。
        renderWorker.detachExecutionLease(this);
    }

    ExecutorState executor;
    PlaybackSourcePublisher playbackSources;
    RenderCacheRegistry renderCaches;
    RenderWorker renderWorker;
    StretcherPool stretchers;
    TimeStretchCache timeStretchCache;

    std::atomic<uint64_t> invalidationsReceived{0};
    std::atomic<uint64_t> invalidationsRejected{0};
    std::atomic<uint64_t> playbackPublications{0};
    std::atomic<uint64_t> stage1Batches{0};
    std::atomic<uint64_t> stage1AffectedChunks{0};
    std::atomic<uint64_t> stage1QueuedChunks{0};
    std::atomic<uint64_t> stage2Requests{0};
    std::atomic<uint64_t> canonicalSettlements{0};
    std::atomic<uint64_t> requeuedChunks{0};
    std::atomic<uint64_t> timeStretchInvalidations{0};
    std::atomic<uint64_t> asyncRenderStarted{0};
    std::atomic<uint64_t> asyncRenderCompleted{0};
};

ContentRenderService::ContentRenderService()
    : impl_(std::make_unique<Impl>())
{
}

ContentRenderService::~ContentRenderService() = default;

bool ContentRenderService::publishCanonicalSource(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (!key.isValid()
        || contentSnapshot == nullptr
        || contentSnapshot->timeGrid == nullptr
        || audioBuffer == nullptr
        || audioBuffer->getNumChannels() <= 0
        || audioBuffer->getNumSamples() <= 0
        || audioSampleRate <= 0.0)
        return false;

    auto readSource = makePlaybackReadSource(
        key,
        std::move(contentSnapshot),
        std::move(audioBuffer),
        audioSampleRate,
        impl_->renderCaches.getOrCreate(key),
        impl_->timeStretchCache);
    impl_->playbackSources.publish(key, std::move(readSource));
    impl_->playbackPublications.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ContentRenderService::republishPlaybackSource(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot)
{
    if (!key.isValid()
        || contentSnapshot == nullptr
        || contentSnapshot->timeGrid == nullptr)
        return false;

    PlaybackReadSource current;
    if (!impl_->playbackSources.get(key, current)
        || current.audioBuffer == nullptr
        || current.audioSampleRate <= 0.0)
        return false;

    impl_->playbackSources.publish(
        key,
        makePlaybackReadSource(
            key,
            std::move(contentSnapshot),
            std::move(current.audioBuffer),
            current.audioSampleRate,
            impl_->renderCaches.getOrCreate(key),
            impl_->timeStretchCache));
    impl_->playbackPublications.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ContentRenderService::getPlaybackReadSource(
    ContentKey key, PlaybackReadSource& out) const
{
    return impl_->playbackSources.get(key, out);
}

ContentRenderService::InvalidationResult ContentRenderService::invalidateContent(
    InvalidationRequest request)
{
    impl_->invalidationsReceived.fetch_add(1, std::memory_order_relaxed);

    if (!request.contentKey.isValid()
        || request.contentSnapshot == nullptr
        || request.contentSnapshot->timeGrid == nullptr)
    {
        impl_->invalidationsRejected.fetch_add(1, std::memory_order_relaxed);
        return InvalidationResult::Rejected;
    }

    PlaybackReadSource current;
    if (!impl_->playbackSources.get(request.contentKey, current)
        || current.audioBuffer == nullptr
        || current.audioBuffer->getNumChannels() <= 0
        || current.audioBuffer->getNumSamples() <= 0
        || current.audioSampleRate <= 0.0)
    {
        impl_->invalidationsRejected.fetch_add(1, std::memory_order_relaxed);
        return InvalidationResult::Rejected;
    }

    if (request.kind == InvalidationKind::TimeGridChange)
    {
        impl_->timeStretchCache.invalidate(request.contentKey);
        impl_->timeStretchInvalidations.fetch_add(1, std::memory_order_relaxed);
    }

    auto contentSnapshot = request.contentSnapshot;
    auto audioBuffer = std::move(current.audioBuffer);
    const double audioSampleRate = current.audioSampleRate;
    const int64_t totalSamples = audioBuffer->getNumSamples();

    int64_t startSample = 0;
    int64_t endSampleExclusive = totalSamples;
    if (request.kind == InvalidationKind::LocalRange)
    {
        if (!std::isfinite(request.startSeconds)
            || !std::isfinite(request.endSeconds))
        {
            impl_->invalidationsRejected.fetch_add(1, std::memory_order_relaxed);
            return InvalidationResult::Rejected;
        }

        startSample = juce::jlimit<int64_t>(
            0, totalSamples,
            TimeCoordinate::secondsToSamplesFloor(
                request.startSeconds, audioSampleRate));
        endSampleExclusive = juce::jlimit<int64_t>(
            0, totalSamples,
            TimeCoordinate::secondsToSamplesCeil(
                request.endSeconds, audioSampleRate));
    }

    // 每個 owner mutation 只發布一次最新 snapshot。Stage1 queue 之後會持有
    // 同一份 shared_ptr，不再回查 owner。
    impl_->playbackSources.publish(
        request.contentKey,
        makePlaybackReadSource(
            request.contentKey,
            contentSnapshot,
            audioBuffer,
            audioSampleRate,
            impl_->renderCaches.getOrCreate(request.contentKey),
            impl_->timeStretchCache));
    impl_->playbackPublications.fetch_add(1, std::memory_order_relaxed);

    // usable OriginalF0 是 Stage1 的 gate；analysis 尚未完成時仍先發布可讀
    // playback source，F0 完成後由 owner 再送一次 full invalidation。
    if (!contentSnapshot->hasUsableOriginalF0()
        || endSampleExclusive <= startSample)
        return InvalidationResult::PublishedWithoutRender;

    std::vector<RenderChunkPlanner::ProtectedRange> protectedRanges;
    for (const auto& note : contentSnapshot->notes)
    {
        if (!note.eq.has_value() || !note.eq->active)
            continue;

        const int64_t protectedStart = TimeCoordinate::secondsToSamplesFloor(
            note.startTime, RenderCache::kSampleRate);
        const int64_t protectedEnd = TimeCoordinate::secondsToSamplesCeil(
            note.endTime, RenderCache::kSampleRate);
        if (protectedEnd > protectedStart)
            protectedRanges.push_back({protectedStart, protectedEnd});
    }

    const auto chunkRanges = RenderChunkPlanner::selectChunksIntersectingRange(
        totalSamples,
        contentSnapshot->silentGaps,
        protectedRanges,
        0,
        totalSamples,
        RenderChunkPlanner::kRenderHopSize);
    if (chunkRanges.empty())
        return InvalidationResult::PublishedWithoutRender;

    std::vector<RenderCache::PlannedChunk> fullPlan;
    fullPlan.reserve(chunkRanges.size());
    for (const auto& range : chunkRanges)
        fullPlan.push_back({range.startSample, range.endSampleExclusive});

    RenderJob job;
    job.kind = RenderJob::Kind::Stage1Render;
    job.contentKey = request.contentKey;
    job.contentSnapshot = std::move(contentSnapshot);
    job.renderCache = impl_->renderCaches.getOrCreate(request.contentKey);
    job.cacheGeneration = job.renderCache->getGeneration();
    job.audioBuffer = std::move(audioBuffer);
    job.audioSampleRate = audioSampleRate;
    job.startSample = startSample;
    job.endSampleExclusive = endSampleExclusive;

    RenderCache::ReconcileResult reconcileResult;
    impl_->renderWorker.reconcileAndSyncStage1Queue(job, [&]() {
        reconcileResult = job.renderCache->reconcileFullPlanAndRequest(
            fullPlan,
            job.startSample,
            job.endSampleExclusive,
            job.contentSnapshot->contentRevision);
        if (reconcileResult.stateChanged)
        {
            impl_->timeStretchCache.invalidate(job.contentKey);
            impl_->timeStretchInvalidations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    impl_->stage1Batches.fetch_add(1, std::memory_order_relaxed);
    impl_->stage1AffectedChunks.fetch_add(
        reconcileResult.requestedChunkCount, std::memory_order_relaxed);
    impl_->stage1QueuedChunks.store(
        impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);

    // TimeGrid 只改 Stage2。若 Stage1 已 canonical settled，不应等待一個
    // 不會改變 contentRevision 的新 Stage1 job；直接以最新 published source 排 Stage2。
    if (request.kind == InvalidationKind::TimeGridChange
        && job.renderCache->isCanonicalSettled())
    {
        PlaybackReadSource current;
        if (impl_->playbackSources.get(request.contentKey, current)
            && current.contentSnapshot != nullptr
            && current.audioBuffer != nullptr)
        {
            enqueueStage2RebuildWhenCanonicalSettled(
                request.contentKey,
                std::move(current.contentSnapshot),
                std::move(current.audioBuffer),
                current.audioSampleRate);
        }
    }

    return InvalidationResult::Enqueued;
}

bool ContentRenderService::isCanonicalSettled(ContentKey key) const
{
    auto renderCache = impl_->renderCaches.get(key);
    return renderCache != nullptr && renderCache->isCanonicalSettled();
}

bool ContentRenderService::handleStage1ChunkSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot>,
    std::shared_ptr<const juce::AudioBuffer<float>>,
    double)
{
    auto renderCache = impl_->renderCaches.get(key);
    if (renderCache == nullptr || !renderCache->isCanonicalSettled())
        return false;

    // completion 可能來自較早的 job；Stage2 一律使用目前已發布的 source，
    // 不可把舊 snapshot 重新送進 freshness gate。
    PlaybackReadSource current;
    if (!impl_->playbackSources.get(key, current)
        || current.contentSnapshot == nullptr
        || current.audioBuffer == nullptr)
        return false;

    impl_->canonicalSettlements.fetch_add(1, std::memory_order_relaxed);
    enqueueStage2RebuildWhenCanonicalSettled(
        key,
        std::move(current.contentSnapshot),
        std::move(current.audioBuffer),
        current.audioSampleRate);
    return true;
}

ContentRenderService::StateSnapshot ContentRenderService::getStateSnapshot(
    ContentKey key) const
{
    StateSnapshot result;
    auto renderCache = impl_->renderCaches.get(key);
    if (renderCache == nullptr)
        return result;

    const auto cacheState = renderCache->getStateSnapshot();
    result.chunkStats.idle = cacheState.chunkStats.idle;
    result.chunkStats.pending = cacheState.chunkStats.pending;
    result.chunkStats.running = cacheState.chunkStats.running;
    result.chunkStats.blank = cacheState.chunkStats.blank;
    result.chunkStats.failed = cacheState.chunkStats.failed;
    result.hasPublishedAudio = cacheState.hasPublishedAudio;
    result.hasContent = cacheState.hasNonBlankChunks;
    return result;
}

ContentRenderService::ChunkStats ContentRenderService::getChunkStats(
    ContentKey key) const
{
    auto renderCache = impl_->renderCaches.get(key);
    if (renderCache == nullptr)
        return {};

    const auto stats = renderCache->getChunkStats();
    ChunkStats result;
    result.idle = stats.idle;
    result.pending = stats.pending;
    result.running = stats.running;
    result.blank = stats.blank;
    result.failed = stats.failed;
    return result;
}

void ContentRenderService::invalidateContentArtifacts(ContentKey key)
{
    if (auto renderCache = impl_->renderCaches.get(key))
    {
        impl_->renderWorker.discardStage1Queue(renderCache.get());
        renderCache->clear();
    }
    impl_->timeStretchCache.invalidate(key);
    impl_->timeStretchInvalidations.fetch_add(1, std::memory_order_relaxed);
    impl_->stage1QueuedChunks.store(
        impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);
}

void ContentRenderService::invalidateAllContentArtifacts()
{
    impl_->renderWorker.discardAllStage1Queue();
    impl_->renderCaches.invalidateAll();
    impl_->timeStretchCache.clear();
    impl_->stage1QueuedChunks.store(0, std::memory_order_relaxed);
    impl_->timeStretchInvalidations.fetch_add(1, std::memory_order_relaxed);
}

void ContentRenderService::removeContent(ContentKey key)
{
    if (auto renderCache = impl_->renderCaches.get(key))
        impl_->renderWorker.discardStage1Queue(renderCache.get());
    impl_->stage1QueuedChunks.store(
        impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);
    impl_->playbackSources.remove(key);
    impl_->renderCaches.remove(key);
    impl_->stretchers.remove(key);
    impl_->timeStretchCache.invalidate(key);
}

void ContentRenderService::attachExecutionLease(ExecutionLease lease)
{
    if (!lease.isValid())
        return;

    // 同一 service 同時只允許一個 Stage1 executor。先在 executor lock 外
    // 完整 detach 舊 owner；正在執行的舊 callback 不會取得新 executor。
    bool hadExecutor = false;
    {
        std::lock_guard<std::mutex> lock(impl_->executor.mutex);
        hadExecutor = impl_->executor.owner != nullptr;
        impl_->executor.owner = nullptr;
        impl_->executor.stage1Callback = nullptr;
    }
    if (hadExecutor)
        impl_->renderWorker.detachExecutionLease(impl_.get());

    {
        std::lock_guard<std::mutex> lock(impl_->executor.mutex);
        impl_->executor.owner = lease.owner;
        impl_->executor.stage1Callback = std::move(lease.renderJobCallback);
    }

    RenderExecutionLease internalLease;
    internalLease.owner = impl_.get();
    internalLease.renderJobCallback = [this](RenderJob& job) {
        executeRenderJob(job);
    };
    impl_->renderWorker.attachExecutionLease(std::move(internalLease));
}

void ContentRenderService::detachExecutionLease(void* owner)
{
    {
        std::lock_guard<std::mutex> lock(impl_->executor.mutex);
        if (impl_->executor.owner != owner)
            return;
        impl_->executor.owner = nullptr;
        impl_->executor.stage1Callback = nullptr;
    }
    impl_->renderWorker.detachExecutionLease(impl_.get());
    impl_->stage1QueuedChunks.store(
        impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);
}

void ContentRenderService::executeRenderJob(RenderJob& job)
{
    impl_->stage1QueuedChunks.store(
        impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);

    if (job.kind == RenderJob::Kind::Stage2Rebuild)
    {
        if (job.contentKey.domainKind == DomainKind::ARAAudioModification
            || job.contentSnapshot == nullptr
            || job.audioBuffer == nullptr)
            return;

        Stage2TimeStretchRebuilder::Request request;
        request.contentKey = job.contentKey;
        request.contentSnapshot = std::move(job.contentSnapshot);
        request.audioBuffer = std::move(job.audioBuffer);
        request.audioSampleRate = job.audioSampleRate;
        Stage2TimeStretchRebuilder::rebuild(*this, request);
        return;
    }

    std::function<void(RenderJob&)> callback;
    {
        std::lock_guard<std::mutex> lock(impl_->executor.mutex);
        callback = impl_->executor.stage1Callback;
    }

    if (callback)
    {
        callback(job);
        return;
    }

    if (job.renderCache != nullptr)
        job.renderCache->completeChunkRenderFailure(
            job.startSample, job.targetRevision, job.cacheGeneration);
}

bool ContentRenderService::enqueueStage2RebuildWhenCanonicalSettled(
    ContentKey key,
    std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
    double audioSampleRate)
{
    if (key.domainKind == DomainKind::ARAAudioModification
        || !key.isValid()
        || contentSnapshot == nullptr
        || contentSnapshot->timeGrid == nullptr
        || contentSnapshot->timeGrid->isIdentity()
        || audioBuffer == nullptr
        || audioSampleRate <= 0.0)
        return false;

    auto renderCache = impl_->renderCaches.get(key);
    if (renderCache == nullptr || !renderCache->isCanonicalSettled())
        return false;

    RenderJob job;
    job.kind = RenderJob::Kind::Stage2Rebuild;
    job.contentKey = key;
    job.contentSnapshot = std::move(contentSnapshot);
    job.audioBuffer = std::move(audioBuffer);
    job.audioSampleRate = audioSampleRate;
    impl_->renderWorker.enqueue(std::move(job));
    impl_->stage2Requests.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ContentRenderService::pauseRenderWorker()
{
    impl_->renderWorker.pause();
}

void ContentRenderService::resumeRenderWorker()
{
    impl_->renderWorker.resume();
}

void ContentRenderService::drainRenderWorker()
{
    impl_->renderWorker.drain();
}

void ContentRenderService::requeueRenderChunk(const RenderJob& job)
{
    jassert(job.kind == RenderJob::Kind::Stage1Render
            && job.renderCache != nullptr);

    PlaybackReadSource current;
    if (!impl_->playbackSources.get(job.contentKey, current)
        || current.contentSnapshot == nullptr
        || current.audioBuffer == nullptr
        || job.contentSnapshot == nullptr
        || current.contentSnapshot->contentRevision
               != job.contentSnapshot->contentRevision)
        return;

    if (job.renderCache->requeueRunningChunk(
            job.startSample, job.targetRevision, job.cacheGeneration))
    {
        RenderJob queued = job;
        queued.queuedChunkStartSample = job.startSample;
        queued.contentSnapshot = std::move(current.contentSnapshot);
        queued.audioBuffer = std::move(current.audioBuffer);
        queued.audioSampleRate = current.audioSampleRate;
        impl_->renderWorker.enqueue(std::move(queued));
        impl_->requeuedChunks.fetch_add(1, std::memory_order_relaxed);
        impl_->stage1QueuedChunks.store(
            impl_->renderWorker.getStage1QueueSize(), std::memory_order_relaxed);
    }
}

void ContentRenderService::beginAsyncRenderJob()
{
    impl_->renderWorker.beginAsyncJob();
    impl_->asyncRenderStarted.fetch_add(1, std::memory_order_relaxed);
}

void ContentRenderService::completeAsyncRenderJob()
{
    impl_->renderWorker.completeAsyncJob();
    impl_->asyncRenderCompleted.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<RenderCache> ContentRenderService::getRenderCacheForStage2(
    ContentKey key) const
{
    return impl_->renderCaches.get(key);
}

std::shared_ptr<SoundTouchStretcher> ContentRenderService::getStretcherForStage2(
    ContentKey key, double sampleRate, int channels)
{
    return impl_->stretchers.getOrCreate(key, sampleRate, channels);
}

TimeStretchCache& ContentRenderService::getTimeStretchCacheForStage2() noexcept
{
    return impl_->timeStretchCache;
}

void ContentRenderService::clearAll()
{
    impl_->renderWorker.discardAllStage1Queue();
    impl_->playbackSources.clear();
    impl_->renderCaches.clear();
    impl_->stretchers.clear();
    impl_->timeStretchCache.clear();
    impl_->stage1QueuedChunks.store(0, std::memory_order_relaxed);
}

void ContentRenderService::preparePlaybackSampleRate(double targetSampleRate)
{
    if (targetSampleRate <= 0.0)
        return;

    impl_->playbackSources.setPlaybackSampleRate(targetSampleRate);
    impl_->renderCaches.preparePlaybackSampleRate(targetSampleRate);
    impl_->timeStretchCache.prepareForPlaybackSampleRate(targetSampleRate);
}

ContentRenderService::Metrics ContentRenderService::getMetrics() const noexcept
{
    Metrics metrics;
    metrics.invalidationsReceived = impl_->invalidationsReceived.load(std::memory_order_relaxed);
    metrics.invalidationsRejected = impl_->invalidationsRejected.load(std::memory_order_relaxed);
    metrics.playbackPublications = impl_->playbackPublications.load(std::memory_order_relaxed);
    metrics.stage1Batches = impl_->stage1Batches.load(std::memory_order_relaxed);
    metrics.stage1AffectedChunks = impl_->stage1AffectedChunks.load(std::memory_order_relaxed);
    metrics.stage1QueuedChunks = impl_->stage1QueuedChunks.load(std::memory_order_relaxed);
    metrics.stage2Requests = impl_->stage2Requests.load(std::memory_order_relaxed);
    metrics.canonicalSettlements = impl_->canonicalSettlements.load(std::memory_order_relaxed);
    metrics.requeuedChunks = impl_->requeuedChunks.load(std::memory_order_relaxed);
    metrics.timeStretchInvalidations = impl_->timeStretchInvalidations.load(std::memory_order_relaxed);
    metrics.asyncRenderStarted = impl_->asyncRenderStarted.load(std::memory_order_relaxed);
    metrics.asyncRenderCompleted = impl_->asyncRenderCompleted.load(std::memory_order_relaxed);
    return metrics;
}

} // namespace OpenTune
