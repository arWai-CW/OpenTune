#include "RenderWorker.h"
#include "../Runtime/ProcessRenderRuntime.h"
#include <algorithm>
#include <set>
#include <vector>

namespace OpenTune {

RenderWorker::RenderWorker()
{
    thread_ = std::thread([this] { loop(); });
}

RenderWorker::~RenderWorker()
{
    stop();
}

void RenderWorker::stop()
{
    if (!thread_.joinable())
        return;

    {
        // 與 loop 的 condition-variable predicate 使用同一把 mutex，避免
        // worker 完成 predicate 檢查、尚未进入 wait 時发生 lost wakeup。
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_.store(true);
    }
    cv_.notify_all();
    thread_.join();
}

// ============================================================
// 執行租約
// ============================================================

void RenderWorker::attachExecutionLease(RenderExecutionLease lease)
{
    drain();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        lease_ = std::move(lease);
    }
    resume();
}

void RenderWorker::detachExecutionLease(void* owner)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (lease_.owner != owner)
            return;

        // 立即清 lease：worker 之後取出的 leaseCopy 無效，只會離開 inFlight_，
        // 不執行任何可能引用已析構 owner 的 callback。
        lease_ = RenderExecutionLease{};
        queue_.clear();
        stage1QueueIndex_.clear();
    }
    cv_.notify_all();

    // 等待正在執行的 renderJobCallback 完成；其 callback 仍可安全存取 owner。
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] { return inFlight_ == 0; });
}

// ============================================================
// Stage1 identity queue
// ============================================================

void RenderWorker::reconcileAndSyncStage1Queue(
    const RenderJob& templateJob,
    const std::function<void()>& reconcile)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 與 worker claim 共用同一臨界區：reconcile 產生的 revision 不可能
        // 和舊 queued identity 組成跨 revision job。
        reconcile();
        syncStage1QueueLocked(templateJob);
    }
    cv_.notify_all();
}

void RenderWorker::insertStage1Locked(RenderJob job)
{
    jassert(job.kind == RenderJob::Kind::Stage1Render);
    jassert(job.renderCache != nullptr);
    if (job.renderCache == nullptr || job.queuedChunkStartSample < 0)
        return;

    auto& index = stage1QueueIndex_[job.renderCache.get()];
    const auto startSample = job.queuedChunkStartSample;
    if (index.find(startSample) != index.end())
        return;

    JobQueueIterator insertBefore = queue_.end();
    const auto next = index.upper_bound(startSample);
    if (next != index.end())
        insertBefore = next->second;

    auto inserted = queue_.insert(insertBefore, std::move(job));
    index.emplace(startSample, inserted);
}

void RenderWorker::eraseQueueLocked(JobQueueIterator iterator)
{
    if (iterator == queue_.end())
        return;

    if (iterator->kind == RenderJob::Kind::Stage1Render
        && iterator->renderCache != nullptr)
    {
        auto cacheIt = stage1QueueIndex_.find(iterator->renderCache.get());
        if (cacheIt != stage1QueueIndex_.end())
        {
            auto indexIt = cacheIt->second.find(iterator->queuedChunkStartSample);
            if (indexIt != cacheIt->second.end()
                && indexIt->second == iterator)
                cacheIt->second.erase(indexIt);

            if (cacheIt->second.empty())
                stage1QueueIndex_.erase(cacheIt);
        }
    }

    queue_.erase(iterator);
}

void RenderWorker::forgetStage1IdentityLocked(const RenderJob& job)
{
    if (job.kind != RenderJob::Kind::Stage1Render || job.renderCache == nullptr)
        return;

    auto cacheIt = stage1QueueIndex_.find(job.renderCache.get());
    if (cacheIt == stage1QueueIndex_.end())
        return;

    cacheIt->second.erase(job.queuedChunkStartSample);
    if (cacheIt->second.empty())
        stage1QueueIndex_.erase(cacheIt);
}

void RenderWorker::syncStage1QueueLocked(const RenderJob& templateJob)
{
    jassert(templateJob.kind == RenderJob::Kind::Stage1Render);
    jassert(templateJob.renderCache != nullptr);
    if (templateJob.renderCache == nullptr)
        return;

    auto* cache = templateJob.renderCache.get();
    const auto pendingChunkStarts = templateJob.renderCache->getPendingChunkStarts();
    const std::set<int64_t> desired(pendingChunkStarts.begin(), pendingChunkStarts.end());

    std::vector<int64_t> staleStarts;
    if (auto cacheIt = stage1QueueIndex_.find(cache);
        cacheIt != stage1QueueIndex_.end())
    {
        for (const auto& [startSample, iterator] : cacheIt->second)
        {
            juce::ignoreUnused(iterator);
            if (desired.count(startSample) == 0)
                staleStarts.push_back(startSample);
        }
    }

    for (const auto startSample : staleStarts)
    {
        auto cacheIt = stage1QueueIndex_.find(cache);
        if (cacheIt == stage1QueueIndex_.end())
            break;
        auto queuedIt = cacheIt->second.find(startSample);
        if (queuedIt != cacheIt->second.end())
            eraseQueueLocked(queuedIt->second);
    }

    for (const auto startSample : pendingChunkStarts)
    {
        auto cacheIt = stage1QueueIndex_.find(cache);
        if (cacheIt != stage1QueueIndex_.end())
        {
            auto queuedIt = cacheIt->second.find(startSample);
            if (queuedIt != cacheIt->second.end())
            {
                queuedIt->second->contentSnapshot = templateJob.contentSnapshot;
                queuedIt->second->audioBuffer = templateJob.audioBuffer;
                queuedIt->second->audioSampleRate = templateJob.audioSampleRate;
                continue;
            }
        }

        RenderJob queued = templateJob;
        queued.queuedChunkStartSample = startSample;
        insertStage1Locked(std::move(queued));
    }
}

void RenderWorker::discardStage1Queue(RenderCache* cache)
{
    std::lock_guard<std::mutex> lk(mutex_);
    while (true)
    {
        auto cacheIt = stage1QueueIndex_.find(cache);
        if (cacheIt == stage1QueueIndex_.end())
            break;
        eraseQueueLocked(cacheIt->second.begin()->second);
    }
    cv_.notify_all();
}

void RenderWorker::discardAllStage1Queue()
{
    std::lock_guard<std::mutex> lk(mutex_);
    while (!stage1QueueIndex_.empty())
        eraseQueueLocked(stage1QueueIndex_.begin()->second.begin()->second);
    cv_.notify_all();
}

void RenderWorker::enqueue(RenderJob job)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (job.kind == RenderJob::Kind::Stage1Render)
        {
            if (job.renderCache == nullptr || job.queuedChunkStartSample < 0)
                return;
            insertStage1Locked(std::move(job));
        }
        else
        {
            queue_.push_back(std::move(job));
        }
    }
    // queue 狀態改變可能同時喚醒 worker 與 drain() waiter；只 notify_one
    // 可能讓另一個等待者維持睡眠。
    cv_.notify_all();
}

std::size_t RenderWorker::getStage1QueueSize() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::size_t count = 0;
    for (const auto& [cache, index] : stage1QueueIndex_)
    {
        juce::ignoreUnused(cache);
        count += index.size();
    }
    return count;
}

void RenderWorker::beginAsyncJob()
{
    std::lock_guard<std::mutex> lk(mutex_);
    ++asyncInFlight_;
}

void RenderWorker::completeAsyncJob()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        jassert(asyncInFlight_ > 0);
        --asyncInFlight_;
    }
    // drain() 與 worker loop 共用 condition variable；喚醒 worker 不代表
    // drain waiter 已被喚醒，因此 async counter 歸零必須通知所有等待者。
    cv_.notify_all();
}

// ============================================================
// 暫停 / 恢復 / 排空
// ============================================================

void RenderWorker::pause()
{
    std::unique_lock<std::mutex> lk(mutex_);
    paused_ = true;
    cv_.wait(lk, [this] { return inFlight_ == 0; });
}

void RenderWorker::resume()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        paused_ = false;
    }
    cv_.notify_all();
}

void RenderWorker::drain()
{
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] {
        return queue_.empty() && inFlight_ == 0 && asyncInFlight_ == 0;
    });
}

// ============================================================
// 工作循環
// ============================================================

void RenderWorker::loop()
{
    while (!stopping_.load())
    {
        RenderJob job;
        RenderExecutionLease leaseCopy;
        bool hasJob = false;

        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] {
                return stopping_.load()
                    || (!paused_ && asyncInFlight_ == 0 && !queue_.empty());
            });

            if (stopping_.load())
                break;

            if (!paused_ && asyncInFlight_ == 0 && !queue_.empty())
            {
                job = std::move(queue_.front());
                forgetStage1IdentityLocked(job);
                queue_.pop_front();
                leaseCopy = lease_;
                ++inFlight_;

                if (leaseCopy.isValid())
                {
                    if (job.kind == RenderJob::Kind::Stage1Render
                        && job.renderCache != nullptr)
                    {
                        RenderCache::PendingJob pendingJob;
                        if (job.renderCache->claimPendingJob(
                                job.queuedChunkStartSample, pendingJob))
                        {
                            job.startSample = pendingJob.startSample;
                            job.endSampleExclusive = pendingJob.endSampleExclusive;
                            job.targetRevision = pendingJob.targetRevision;
                            job.cacheGeneration = pendingJob.generation;
                            hasJob = true;
                        }
                    }
                    else if (job.kind == RenderJob::Kind::Stage2Rebuild)
                    {
                        hasJob = true;
                    }
                }
            }
        }

        if (hasJob)
            leaseCopy.renderJobCallback(job);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            --inFlight_;
        }
        cv_.notify_all();
    }
}

} // namespace OpenTune
