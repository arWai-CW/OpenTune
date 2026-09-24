#pragma once

#include "RenderJob.h"
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace OpenTune {

/**
 * RenderExecutionLease — 渲染执行租约。
 * 
 * 处理器桥接短租约，RenderWorker 不长期持有 callback。
 * Worker 调用 callback 时传入完整的 RenderJob。
 * 析构时必须 detach，防止 dangling lambda。
 */
struct RenderExecutionLease
{
    void* owner{nullptr};
    std::function<void(RenderJob&)> renderJobCallback;

    bool isValid() const noexcept
    {
        return owner != nullptr && renderJobCallback != nullptr;
    }
};

/**
 * RenderWorker — 异步渲染队列和工作线程。
 * 
 * 管理带身份的 Stage1 chunk 队列、Stage2 队列、worker thread 生命周期和 execution lease。
 */
class RenderWorker
{
public:
    RenderWorker();
    ~RenderWorker();

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    void attachExecutionLease(RenderExecutionLease lease);
    /**
     * detachExecutionLease — 终止执行租约（析构路径专用）。
     * 立即清空 lease 与排队 job，并等待正在执行的 renderJobCallback 完成。
     * 不等待 asyncInFlight_：vocoder 推理不可取消，其完成不依赖本 worker 存活
     * （onComplete 经 shared_ptr 持有 ContentRenderService，必然回调）。
     */
    void detachExecutionLease(void* owner);

    // Reconcile 缓存计划与物理 Stage1 队列的同步必须在同一队列锁临界区内完成：
    // worker loop 也在同一 mutex 下 claim，避免出现「旧 queued job + reconcile
    // 后新 targetRevision」的跨 revision 组合。reconcile 回调在锁内执行。
    void reconcileAndSyncStage1Queue(const RenderJob& templateJob,
                                     const std::function<void()>& reconcile);
    void discardStage1Queue(RenderCache* cache);
    void discardAllStage1Queue();
    void discardStage2Queue(ContentKey key);
    void discardAllStage2Queue();
    void enqueue(RenderJob job);
    std::size_t getStage1QueueSize() const;
    void beginAsyncJob();
    void completeAsyncJob();

    /**
     * pause() 暂停取新同步 job 并等待正在执行的 render callback 完成
     * （inFlight_ == 0），绝不等待异步 vocoder 推理（重置推理后端 /
     * 切换 vocoder 模型前调用）：卡住的推理由调用方 resetVocoder /
     * setVocoderModelWeight 的 SetTerminate + join 终止，onComplete 回调归还计数。
     * drain() 完整合同：等待 queue_ 空且 inFlight_ == 0 && asyncInFlight_ == 0。
     * 导出路径在读取渲染结果前必须调用，保证最新完整数据已落盘。
     * 析构路径不得使用 drain()/pause() 编排：vocoder 推理不可取消，
     * 其完成不依赖本 worker 存活，析构只调 detachExecutionLease。
     */
    void pause();
    void resume();
    void drain();
    void stop();

private:
    using JobQueue = std::list<RenderJob>;
    using JobQueueIterator = JobQueue::iterator;
    using Stage1Index = std::map<int64_t, JobQueueIterator>;

    void loop();
    void insertStage1Locked(RenderJob job);
    void eraseQueueLocked(JobQueueIterator iterator);
    void forgetStage1IdentityLocked(const RenderJob& job);

    // 物理 Stage1 queue 與 cache pending chunk 集合同步。只在 mutex_ 持有時呼叫。
    // 每個 (RenderCache, chunk start) identity 只允許一個排隊項。
    void syncStage1QueueLocked(const RenderJob& templateJob);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    JobQueue queue_;
    std::map<RenderCache*, Stage1Index> stage1QueueIndex_;

    RenderExecutionLease lease_;

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    bool paused_{false};
    int inFlight_{0};
    int asyncInFlight_{0};
};

} // namespace OpenTune
