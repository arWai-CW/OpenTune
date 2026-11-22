#include "Stage2TimeStretchRebuilder.h"

#include "ContentRenderService.h"

#include "../Inference/SoundTouchStretcher.h"
#include "../Inference/TimeStretchCache.h"
#include "../Utils/AppLogger.h"
#include "../Utils/PlaybackAudioReader.h"

namespace OpenTune {

/**
 * Stage2 rebuild — consumes only the immutable snapshot/audio carried by the
 * request. Stage1 canonical PCM is read through a source-domain request that
 * carries only the audio and RenderCache needed by readCanonicalAudio.
 */
bool Stage2TimeStretchRebuilder::rebuild(ContentRenderService& crs,
                                          const Request& request)
{
    const auto contentKey = request.contentKey;
    if (!contentKey.isValid()) return false;

    if (!request.contentSnapshot
        || request.contentSnapshot->timeGrid == nullptr
        || request.contentSnapshot->timeGrid->empty()
        || !request.audioBuffer
        || request.audioBuffer->getNumChannels() <= 0
        || request.audioBuffer->getNumSamples() <= 0
        || request.audioSampleRate <= 0.0) {
        AppLogger::warn("Stage2: invalid request inputs for contentKey objectId="
                        + juce::String(static_cast<juce::int64>(contentKey.objectId)));
        return false;
    }

    const auto& snapshot = *request.contentSnapshot;

    // 同 revision 的合法 republish 会换 snapshot 指针，指针相等不是新鲜度判据。
    // 只比较 CRS 已发布 snapshot 的 revision；getPlaybackReadSource 成功即遵守
    // makePlaybackReadSource 合同（contentSnapshot/timeGrid 非空），不再重复设防。
    const auto isCurrentPublishedRequest = [&]() {
        PlaybackReadSource published;
        return crs.getPlaybackReadSource(contentKey, published)
            && published.contentSnapshot != nullptr
            && published.contentSnapshot->contentRevision == snapshot.contentRevision
            && published.contentSnapshot->timeGridRevision == snapshot.timeGridRevision
            && published.contentSnapshot->audioRevision == snapshot.audioRevision
            && published.canonicalBuffer == request.audioBuffer
            && published.canonicalSampleRate == request.audioSampleRate;
    };

    if (!isCurrentPublishedRequest())
        return false;

    // identity TimeGrid 不需要 Stage2；mutation 已在发布新 snapshot 时失效旧输出。
    if (snapshot.timeGrid->isIdentity()) {
        return true;
    }

    const double sampleRate = request.audioSampleRate;
    auto stage1 = crs.getRenderCache(contentKey);
    if (contentKey.domainKind == DomainKind::ARAAudioModification
        && (!stage1 || stage1->getChunkStats().hasActiveWork()))
        return false;
    const int channels = request.audioBuffer->getNumChannels();
    auto stretcher = crs.getStretcher(contentKey, sampleRate, channels);
    if (stretcher == nullptr)
        return false;

    const uint32_t buildGen = crs.getTimeStretchCache().beginBuild(contentKey);
    const int totalSamples = request.audioBuffer->getNumSamples();
    juce::AudioBuffer<float> input(channels, totalSamples);
    if (readCanonicalAudio({request.audioBuffer, std::move(stage1), 0, totalSamples},
                           input, 0) != totalSamples)
        return false;

    auto output = stretcher->stretch(input, *snapshot.timeGrid);
    if (output.getNumSamples() == 0 || !isCurrentPublishedRequest())
        return false;

    const int64_t firstOutput = TimeCoordinate::secondsToSamplesNearest(
        snapshot.timeGrid->handles().front().output_seconds, sampleRate);
    crs.getTimeStretchCache().storeBuffer(contentKey, std::move(output),
        snapshot.contentRevision, snapshot.timeGridRevision, sampleRate, buildGen, firstOutput);
    return true;
}

} // namespace OpenTune
