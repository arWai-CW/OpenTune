#undef JucePlugin_Enable_ARA

#include "../Source/Content/ContentSnapshotProjection.h"
#include "../Source/Content/ContentState.h"
#include "../Source/Render/ContentRenderService.h"
#include "../Source/Render/PlaybackReadSource.h"
#include "../Source/Render/RenderJob.h"
#include "../Source/Utils/PlaybackAudioReader.h"
#include "../Source/Utils/PitchCurve.h"
#include "../Source/Utils/TimeGrid.h"
#include "../Source/Utils/TimeCoordinate.h"
#include "../Source/Inference/TimeStretchCache.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using namespace OpenTune;

struct RenderFixture
{
    ContentKey key;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    std::shared_ptr<const EditableContentSnapshot> snapshot;
};

std::shared_ptr<const TimeGridSnapshot> makeIdentityGrid(double durationSeconds)
{
    return TimeGridSnapshot::makeIdentity(durationSeconds);
}

std::shared_ptr<const TimeGridSnapshot> makeWarpedGrid(double durationSeconds)
{
    auto identity = makeIdentityGrid(durationSeconds);
    if (identity == nullptr)
        return nullptr;

    auto handles = identity->handles();
    handles.insert(handles.begin() + 1,
                   TimeHandle{ 1, durationSeconds * 0.5,
                                durationSeconds * 0.6,
                                HandleKind::UserAdded, Confidence::Default });
    return TimeGridSnapshot::makeFromHandles(std::move(handles));
}

RenderFixture makeFixture(DomainKind domain,
                          double durationSeconds,
                          uint64_t contentRevision,
                          bool usableF0,
                          std::shared_ptr<const TimeGridSnapshot> timeGrid = nullptr)
{
    RenderFixture fixture;
    fixture.key.domainKind = domain;
    fixture.key.objectId = 17;

    const int sampleCount = juce::jmax(1, juce::roundToInt(
        durationSeconds * TimeCoordinate::kRenderSampleRate));
    auto audio = std::make_shared<juce::AudioBuffer<float>>(1, sampleCount);
    audio->clear();
    fixture.audio = std::move(audio);

    ContentState state;
    state.sourceWindow = SourceWindow{ 1, {}, 0.0, durationSeconds };
    state.audioBuffer = fixture.audio;
    state.sampleRate = TimeCoordinate::kRenderSampleRate;
    state.timeGrid = timeGrid != nullptr
        ? std::move(timeGrid)
        : makeIdentityGrid(durationSeconds);
    state.contentRevision = contentRevision;

    if (usableF0)
    {
        auto curve = std::make_shared<PitchCurve>();
        curve->setHopSize(256);
        curve->setSampleRate(16000.0);
        const int frameCount = juce::jmax(
            8, juce::roundToInt(durationSeconds * 16000.0 / 256.0) + 2);
        curve->setOriginalF0(std::vector<float>(
            static_cast<size_t>(frameCount), 220.0f));
        state.analysis.originalF0State = OriginalF0State::Ready;
        state.analysis.pitchCurve = std::move(curve);
    }

    fixture.snapshot = std::make_shared<const EditableContentSnapshot>(
        makeContentSnapshot(state));
    return fixture;
}

ContentRenderService::InvalidationRequest fullInvalidation(const RenderFixture& fixture)
{
    ContentRenderService::InvalidationRequest request;
    request.contentKey = fixture.key;
    request.contentSnapshot = fixture.snapshot;
    request.kind = ContentRenderService::InvalidationKind::FullContent;
    return request;
}

bool testServiceShutdownDoesNotLoseWorkerWakeup()
{
    // stop() 必須在 worker condition-variable predicate 使用的 mutex 下更新
    // stopping state；反覆建立/銷毀可覆蓋先前的 lost-wakeup race。
    for (int i = 0; i < 64; ++i)
    {
        ContentRenderService service;
    }
    return true;
}

bool testPublicationWaitsForUsableF0()
{
    const auto fixture = makeFixture(
        DomainKind::StandaloneClip, 1.0, 1, false);
    ContentRenderService service;

    if (!service.publishCanonicalSource(
            fixture.key, fixture.snapshot, fixture.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;

    const auto result = service.invalidateContent(fullInvalidation(fixture));
    PlaybackReadSource published;
    const auto metrics = service.getMetrics();

    return result == ContentRenderService::InvalidationResult::PublishedWithoutRender
        && service.getPlaybackReadSource(fixture.key, published)
        && published.contentSnapshot == fixture.snapshot
        && published.audioBuffer == fixture.audio
        && metrics.invalidationsReceived == 1
        && metrics.playbackPublications == 2
        && metrics.stage1Batches == 0
        && service.getStateSnapshot(fixture.key).chunkStats.total() == 0;
}

bool testPlaybackOnlyRepublishDoesNotInvalidateStage1()
{
    const auto fixture = makeFixture(
        DomainKind::StandaloneClip, 1.0, 1, true);
    ContentRenderService service;
    if (!service.publishCanonicalSource(
            fixture.key, fixture.snapshot, fixture.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;

    EditableContentSnapshot snapshotData = *fixture.snapshot;
    ++snapshotData.contentRevision;
    auto next = std::make_shared<const EditableContentSnapshot>(snapshotData);
    if (!service.republishPlaybackSource(fixture.key, next))
        return false;

    const auto metrics = service.getMetrics();
    return metrics.playbackPublications == 2
        && metrics.stage1Batches == 0
        && metrics.timeStretchInvalidations == 0;
}

bool testTimeGridChangeInvalidatesDerivedOutputWithoutF0()
{
    const auto fixture = makeFixture(
        DomainKind::StandaloneClip, 1.0, 1, false);
    ContentRenderService service;
    if (!service.publishCanonicalSource(
            fixture.key, fixture.snapshot, fixture.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;

    ContentRenderService::InvalidationRequest request;
    request.contentKey = fixture.key;
    request.contentSnapshot = fixture.snapshot;
    request.kind = ContentRenderService::InvalidationKind::TimeGridChange;

    const auto result = service.invalidateContent(std::move(request));
    const auto metrics = service.getMetrics();
    return result == ContentRenderService::InvalidationResult::PublishedWithoutRender
        && metrics.playbackPublications == 2
        && metrics.timeStretchInvalidations == 1
        && metrics.stage1Batches == 0
        && metrics.stage2Requests == 0;
}

bool testLocalInvalidationKeepsPhysicalQueueBounded()
{
    const auto first = makeFixture(
        DomainKind::StandaloneClip, 20.0, 1, true);
    const auto second = makeFixture(
        DomainKind::StandaloneClip, 20.0, 2, true);
    ContentRenderService service;
    int ownerToken = 0;
    std::atomic<int> unexpectedExecutions{ 0 };

    ContentRenderService::ExecutionLease lease;
    lease.owner = &ownerToken;
    lease.renderJobCallback = [&unexpectedExecutions](RenderJob&) {
        ++unexpectedExecutions;
    };
    service.attachExecutionLease(std::move(lease));
    service.pauseRenderWorker();

    if (!service.publishCanonicalSource(
            first.key, first.snapshot, first.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;
    if (service.invalidateContent(fullInvalidation(first))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;

    const auto fullMetrics = service.getMetrics();
    const auto fullPending = service.getChunkStats(first.key).pending;
    if (fullPending <= 1
        || fullMetrics.stage1QueuedChunks != static_cast<uint64_t>(fullPending))
        return false;

    ContentRenderService::InvalidationRequest local;
    local.contentKey = second.key;
    local.contentSnapshot = second.snapshot;
    local.kind = ContentRenderService::InvalidationKind::LocalRange;
    local.startSeconds = 5.0;
    local.endSeconds = 5.1;
    if (service.invalidateContent(std::move(local))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;

    auto localMetrics = service.getMetrics();
    const auto localAffected =
        localMetrics.stage1AffectedChunks - fullMetrics.stage1AffectedChunks;

    // 相同 local range 連續 supersede：實體 queue identity 不得隨 revision 增長。
    constexpr int kSupersedeCount = 100;
    for (int revision = 3; revision < 3 + kSupersedeCount; ++revision)
    {
        EditableContentSnapshot snapshotData = *second.snapshot;
        snapshotData.contentRevision = static_cast<uint64_t>(revision);

        ContentRenderService::InvalidationRequest repeated;
        repeated.contentKey = second.key;
        repeated.contentSnapshot = std::make_shared<const EditableContentSnapshot>(
            snapshotData);
        repeated.kind = ContentRenderService::InvalidationKind::LocalRange;
        repeated.startSeconds = 5.0;
        repeated.endSeconds = 5.1;
        if (service.invalidateContent(std::move(repeated))
            != ContentRenderService::InvalidationResult::Enqueued)
            return false;
    }

    localMetrics = service.getMetrics();
    const auto repeatedAffected = localMetrics.stage1AffectedChunks
        - fullMetrics.stage1AffectedChunks;
    const auto repeatedAverage = repeatedAffected
        / static_cast<uint64_t>(kSupersedeCount + 1);

    service.detachExecutionLease(&ownerToken);
    return unexpectedExecutions.load() == 0
        && localAffected > 0
        && localAffected < fullMetrics.stage1AffectedChunks
        && repeatedAverage == localAffected
        && localMetrics.stage1QueuedChunks == fullMetrics.stage1QueuedChunks
        && service.getChunkStats(first.key).pending == fullPending;
}

bool runSettlementCase(DomainKind domain, bool expectStage2)
{
    auto grid = makeWarpedGrid(1.0);
    if (grid == nullptr || grid->isIdentity())
        return false;

    const auto fixture = makeFixture(
        domain, 1.0, 1, true, std::move(grid));
    ContentRenderService service;
    int ownerToken = 0;
    std::atomic<int> stage1Executions{ 0 };
    ContentRenderService* servicePtr = &service;

    ContentRenderService::ExecutionLease lease;
    lease.owner = &ownerToken;
    lease.renderJobCallback = [servicePtr, &stage1Executions](RenderJob& job) {
        ++stage1Executions;
        if (job.kind != RenderJob::Kind::Stage1Render
            || job.renderCache == nullptr)
            return;

        std::vector<float> audio(
            static_cast<size_t>(job.endSampleExclusive - job.startSample),
            0.0f);
        const auto result = job.renderCache->completeChunkRenderWithAudio(
            job.startSample, job.endSampleExclusive,
            std::move(audio), job.targetRevision, job.cacheGeneration);
        if (result == RenderCache::ChunkRenderResult::Published)
        {
            servicePtr->handleStage1ChunkSettled(
                job.contentKey,
                job.contentSnapshot,
                job.audioBuffer,
                job.audioSampleRate);
        }
    };
    service.attachExecutionLease(std::move(lease));

    if (!service.publishCanonicalSource(
            fixture.key, fixture.snapshot, fixture.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;
    if (service.invalidateContent(fullInvalidation(fixture))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;

    service.drainRenderWorker();
    const auto metrics = service.getMetrics();
    const bool settled = service.getStateSnapshot(fixture.key).chunkStats.total() > 0
        && service.getStateSnapshot(fixture.key).chunkStats.pending == 0
        && service.getStateSnapshot(fixture.key).chunkStats.running == 0;
    service.detachExecutionLease(&ownerToken);

    return stage1Executions.load() > 0
        && settled
        && (metrics.stage2Requests == (expectStage2 ? 1u : 0u));
}

bool testSettlementEnqueuesStage2OnlyForNonAraContent()
{
    return runSettlementCase(DomainKind::StandaloneClip, true)
        && runSettlementCase(DomainKind::ARAAudioModification, false);
}

bool testStaleCompletionCannotPassClearedCacheGeneration()
{
    RenderCache cache;
    const std::vector<RenderCache::PlannedChunk> plan{
        { 0, 512 }
    };

    if (!cache.reconcileFullPlanAndRequest(plan, 0, 512, 1).stateChanged)
        return false;
    RenderCache::PendingJob oldPending;
    if (!cache.claimPendingJob(0, oldPending)
        || oldPending.targetRevision != 1)
        return false;

    cache.clear();
    if (!cache.reconcileFullPlanAndRequest(plan, 0, 512, 1).stateChanged)
        return false;
    RenderCache::PendingJob newPending;
    if (!cache.claimPendingJob(0, newPending)
        || newPending.targetRevision != 1)
        return false;
    if (cache.requeueRunningChunk(
            0, oldPending.targetRevision, oldPending.generation))
        return false;
    if (cache.markChunkAsBlank(
            0, oldPending.targetRevision, oldPending.generation))
        return false;

    const auto result = cache.completeChunkRenderWithAudio(
        0, 512, std::vector<float>(512, 0.0f),
        oldPending.targetRevision, oldPending.generation);
    return result == RenderCache::ChunkRenderResult::Stale
        && !cache.getStateSnapshot().hasPublishedAudio;
}

bool testTimeGridChangeReusesSettledStage1ForStage2()
{
    auto firstGrid = makeWarpedGrid(1.0);
    const auto first = makeFixture(
        DomainKind::StandaloneClip, 1.0, 1, true, std::move(firstGrid));
    ContentRenderService service;
    int ownerToken = 0;
    ContentRenderService* servicePtr = &service;

    ContentRenderService::ExecutionLease lease;
    lease.owner = &ownerToken;
    lease.renderJobCallback = [servicePtr](RenderJob& job) {
        if (job.kind != RenderJob::Kind::Stage1Render
            || job.renderCache == nullptr)
            return;
        std::vector<float> audio(
            static_cast<size_t>(job.endSampleExclusive - job.startSample),
            0.0f);
        if (job.renderCache->completeChunkRenderWithAudio(
                job.startSample, job.endSampleExclusive,
                std::move(audio), job.targetRevision, job.cacheGeneration)
            == RenderCache::ChunkRenderResult::Published)
        {
            servicePtr->handleStage1ChunkSettled(
                job.contentKey, job.contentSnapshot,
                job.audioBuffer, job.audioSampleRate);
        }
    };
    service.attachExecutionLease(std::move(lease));

    if (!service.publishCanonicalSource(
            first.key, first.snapshot, first.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;
    if (service.invalidateContent(fullInvalidation(first))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;
    service.drainRenderWorker();
    const auto afterFirst = service.getMetrics();
    if (afterFirst.stage2Requests != 1)
        return false;

    EditableContentSnapshot secondData = *first.snapshot;
    secondData.timeGridRevision = first.snapshot->timeGridRevision + 1;
    auto second = std::make_shared<const EditableContentSnapshot>(secondData);
    ContentRenderService::InvalidationRequest timeGridChange;
    timeGridChange.contentKey = first.key;
    timeGridChange.contentSnapshot = std::move(second);
    timeGridChange.kind = ContentRenderService::InvalidationKind::TimeGridChange;
    if (service.invalidateContent(std::move(timeGridChange))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;
    service.drainRenderWorker();
    service.detachExecutionLease(&ownerToken);

    const auto afterSecond = service.getMetrics();
    return afterSecond.stage2Requests == 2;
}

bool testTimeStretchClearRejectsOldBuild()
{
    ContentKey key;
    key.domainKind = DomainKind::StandaloneClip;
    key.objectId = 17;

    TimeStretchCache cache;
    const auto oldGeneration = cache.beginBuild(key);
    cache.clear();
    const auto newGeneration = cache.beginBuild(key);
    if (oldGeneration == newGeneration)
        return false;

    cache.store(key, std::vector<float>(128, 0.25f),
                1, 1, TimeCoordinate::kRenderSampleRate, oldGeneration);

    juce::AudioBuffer<float> output(1, 128);
    output.clear();
    return cache.sliceForOutputRange(
               key, 1, 1, 0, output, 0, output.getNumSamples(),
               static_cast<int>(TimeCoordinate::kRenderSampleRate)) == 0;
}

bool testRenderQueueProducesContinuousPlayback()
{
    const auto fixture = makeFixture(
        DomainKind::StandaloneClip, 1.0, 1, true);
    ContentRenderService service;
    int ownerToken = 0;
    ContentRenderService* servicePtr = &service;

    ContentRenderService::ExecutionLease lease;
    lease.owner = &ownerToken;
    lease.renderJobCallback = [servicePtr](RenderJob& job) {
        if (job.kind != RenderJob::Kind::Stage1Render
            || job.renderCache == nullptr)
            return;

        const auto sampleCount = static_cast<size_t>(
            job.endSampleExclusive - job.startSample);
        std::vector<float> audio(sampleCount);
        for (size_t i = 0; i < sampleCount; ++i)
        {
            const auto absoluteSample = static_cast<double>(job.startSample)
                + static_cast<double>(i);
            audio[i] = 0.2f * std::sin(
                2.0 * 3.14159265358979323846 * 440.0
                    * absoluteSample / TimeCoordinate::kRenderSampleRate);
        }

        if (job.renderCache->completeChunkRenderWithAudio(
                job.startSample, job.endSampleExclusive,
                std::move(audio), job.targetRevision, job.cacheGeneration)
            == RenderCache::ChunkRenderResult::Published)
        {
            servicePtr->handleStage1ChunkSettled(
                job.contentKey, job.contentSnapshot,
                job.audioBuffer, job.audioSampleRate);
        }
    };
    service.attachExecutionLease(std::move(lease));

    if (!service.publishCanonicalSource(
            fixture.key, fixture.snapshot, fixture.audio,
            TimeCoordinate::kRenderSampleRate))
        return false;
    if (service.invalidateContent(fullInvalidation(fixture))
        != ContentRenderService::InvalidationResult::Enqueued)
        return false;

    service.drainRenderWorker();
    service.detachExecutionLease(&ownerToken);

    PlaybackReadSource source;
    if (!service.getPlaybackReadSource(fixture.key, source))
        return false;

    juce::AudioBuffer<float> output(1, fixture.audio->getNumSamples());
    const auto read = readPlaybackAudio(
        PlaybackReadRequest(source, 0,
                            TimeCoordinate::kRenderSampleRate,
                            output.getNumSamples()),
        output, 0);
    if (read != output.getNumSamples())
        return false;

    const auto* samples = output.getReadPointer(0);
    float energy = 0.0f;
    for (int i = 0; i < read; ++i)
        energy += samples[i] * samples[i];
    if (energy <= 1.0f)
        return false;

    for (int i = 1; i < read; ++i)
    {
        if (std::abs(samples[i] - samples[i - 1]) > 0.05f)
            return false;
    }
    return true;
}

bool check(const char* name, bool result)
{
    std::printf("%s: %s\n", name, result ? "PASS" : "FAIL");
    return result;
}

} // namespace

int main()
{
    bool ok = true;
    ok &= check("service shutdown does not lose worker wakeup",
                testServiceShutdownDoesNotLoseWorkerWakeup());
    ok &= check("publication waits for usable F0",
                testPublicationWaitsForUsableF0());
    ok &= check("playback-only republish does not invalidate Stage1",
                testPlaybackOnlyRepublishDoesNotInvalidateStage1());
    ok &= check("TimeGrid change invalidates derived output without F0",
                testTimeGridChangeInvalidatesDerivedOutputWithoutF0());
    ok &= check("local invalidation keeps physical queue bounded",
                testLocalInvalidationKeepsPhysicalQueueBounded());
    ok &= check("settlement enqueues Stage2 only for non-ARA content",
                testSettlementEnqueuesStage2OnlyForNonAraContent());
    ok &= check("stale completion cannot pass cleared cache generation",
                testStaleCompletionCannotPassClearedCacheGeneration());
    ok &= check("TimeGrid change reuses settled Stage1 for Stage2",
                testTimeGridChangeReusesSettledStage1ForStage2());
    ok &= check("TimeStretch clear rejects old build",
                testTimeStretchClearRejectsOldBuild());
    ok &= check("render queue produces continuous playback",
                testRenderQueueProducesContinuousPlayback());
    return ok ? 0 : 1;
}
