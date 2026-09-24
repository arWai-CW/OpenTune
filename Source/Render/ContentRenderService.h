#pragma once

#include "../Content/ContentKey.h"
#include "../Content/EditableContentSnapshot.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace OpenTune {

class ProcessRenderRuntime;
class RenderCache;
struct RenderJob;
class SoundTouchStretcher;
class TimeStretchCache;
struct PlaybackReadSource;
struct Stage2TimeStretchRebuilder;

/**
 * Content render 的深 module。
 *
 * owner 先完成 mutation，再以不可變 snapshot 宣告 local、full 或 TimeGrid
 * invalidation；本 module 負責 playback publication、Stage1 planning、queue
 * reconciliation、canonical settlement 與 Stage2 gate。caller 不再建立
 * RenderJob，也不再理解低階 cache/queue 實作。
 */
class ContentRenderService
{
public:
    /** Stage1 執行 adapter；Stage2 與 settlement 由本 module 內部處理。 */
    struct ExecutionLease
    {
        void* owner{nullptr};
        std::function<void(RenderJob&)> renderJobCallback;

        bool isValid() const noexcept
        {
            return owner != nullptr && renderJobCallback != nullptr;
        }
    };

    struct ChunkStats
    {
        int idle{0};
        int pending{0};
        int running{0};
        int blank{0};
        int failed{0};

        int total() const noexcept
        {
            return idle + pending + running + blank + failed;
        }

        bool hasActiveWork() const noexcept
        {
            return pending > 0 || running > 0;
        }
    };

    struct StateSnapshot
    {
        ChunkStats chunkStats;
        bool hasPublishedAudio{false};
        bool hasContent{false};
    };

    enum class InvalidationKind : uint8_t
    {
        LocalRange,
        FullContent,
        TimeGridChange
    };

    struct InvalidationRequest
    {
        ContentKey contentKey;
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot;
        InvalidationKind kind{InvalidationKind::FullContent};
        double startSeconds{0.0};
        double endSeconds{0.0};
    };

    enum class InvalidationResult : uint8_t
    {
        Rejected,
        PublishedWithoutRender,
        Enqueued
    };

    /** 僅供診斷與效能基線；不會進 audio-thread read path。 */
    struct Metrics
    {
        uint64_t invalidationsReceived{0};
        uint64_t invalidationsRejected{0};
        uint64_t playbackPublications{0};
        uint64_t stage1Batches{0};
        uint64_t stage1AffectedChunks{0};
        uint64_t stage1QueuedChunks{0};
        uint64_t stage2Requests{0};
        uint64_t canonicalSettlements{0};
        uint64_t requeuedChunks{0};
        uint64_t timeStretchInvalidations{0};
        uint64_t asyncRenderStarted{0};
        uint64_t asyncRenderCompleted{0};
    };

    ContentRenderService();
    ~ContentRenderService();

    ContentRenderService(const ContentRenderService&) = delete;
    ContentRenderService& operator=(const ContentRenderService&) = delete;

    /** 發布 owner 的 canonical PCM 與最新不可變內容 snapshot。 */
    bool publishCanonicalSource(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);

    /** 只替換已發布 playback source 的 editable snapshot，不啟動 Stage1。 */
    bool republishPlaybackSource(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot);

    bool getPlaybackReadSource(ContentKey key, PlaybackReadSource& out) const;

    /** owner mutation 完成後的唯一 render scheduling interface。 */
    InvalidationResult invalidateContent(InvalidationRequest request);

    /**
     * Stage1 chunk 成功發布後由 executor adapter 回報。
     * @return true 表示該 content 已 canonical settled。
     */
    bool isCanonicalSettled(ContentKey key) const;
    bool handleStage1ChunkSettled(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);

    StateSnapshot getStateSnapshot(ContentKey key) const;
    ChunkStats getChunkStats(ContentKey key) const;

    /** 保留 playback source，清除並重建其 derived render artifacts。 */
    void invalidateContentArtifacts(ContentKey key);
    void invalidateAllContentArtifacts();

    /** 一次移除 publication、Stage1/Stage2 與 cache。 */
    void removeContent(ContentKey key);

    void attachExecutionLease(ExecutionLease lease);
    void detachExecutionLease(void* owner);

    void pauseRenderWorker();
    void resumeRenderWorker();
    void drainRenderWorker();

    void clearAll();
    void preparePlaybackSampleRate(double targetSampleRate);
    Metrics getMetrics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void executeRenderJob(RenderJob& job);
    bool enqueueStage2RebuildWhenCanonicalSettled(
        ContentKey key,
        std::shared_ptr<const EditableContentSnapshot> contentSnapshot,
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer,
        double audioSampleRate);

    // Stage2TimeStretchRebuilder 是本 module implementation 的一部分。
    std::shared_ptr<RenderCache> getRenderCacheForStage2(ContentKey key) const;
    std::shared_ptr<SoundTouchStretcher> getStretcherForStage2(
        ContentKey key, double sampleRate, int channels);
    TimeStretchCache& getTimeStretchCacheForStage2() noexcept;

    // ProcessRenderRuntime 只透過窄化的 retry / async lifetime interface 操作。
    void requeueRenderChunk(const RenderJob& job);
    void beginAsyncRenderJob();
    void completeAsyncRenderJob();

    friend struct Stage2TimeStretchRebuilder;
    friend class ProcessRenderRuntime;
};

} // namespace OpenTune
