#pragma once

#include "../Content/EditableContentSnapshot.h"
#include "../Render/ContentRenderService.h"
#include "VocoderExecutionCoordinator.h"
#include "../Render/RenderJob.h"
#include "../Utils/VocoderModelWeight.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OpenTune {

class ProcessRenderRuntime
{
public:
    struct CompletionGate
    {
        std::mutex mutex;
        bool closed{false};
    };

    struct CompletionContext
    {
        std::shared_ptr<CompletionGate> gate;
        std::function<void(ContentKey,
                           std::shared_ptr<const EditableContentSnapshot>,
                           std::shared_ptr<const juce::AudioBuffer<float>>,
                           double)> chunkSettled;
        std::function<void(ContentKey)> chunkFailed;
    };

    static ProcessRenderRuntime& getInstance();

    void processChunkRenderJob(std::shared_ptr<ContentRenderService> crs,
                               RenderJob& job,
                               bool lightPitchEnabled,
                               CompletionContext completion);

    /**
     * 异步模型切换 / 后端重置 API（UI 线程调用）：
     * 只投递命令到进程寿命 control worker 并立即返回。control worker 串行执行
     * 耗时 Session 销毁、按当前配置重建、AccelerationDetector resetAndDetect。
     * completion 经 MessageManager::callAsync 投递回消息线程；调用方在其
     * completion gate 关闭后直接丢弃（不访问 owner）。
     */
    void setVocoderModelWeight(const VocoderModelWeight& weight, std::function<void()> completion);
    void resetVocoder(std::function<void()> completion);
    void resetInferenceBackend(bool forceCpu, std::function<void()> completion);

    /**
     * Vocoder submission / query entry points. All accesses to the underlying
     * VocoderDomain go through these (each locks vocoderMutex_), so a
     * concurrent control-worker reset/rebuild can never destroy the domain
     * while it is being used (no raw pointer escapes).
     *
     * submitVocoderJob requires expectedGeneration to match the current domain
     * generation: the caller captures the full configuration
     * (generation/conditioningBins/conditioningType/fMax) in a single locked
     * snapshot via acquireVocoderConfig(), so a job is never submitted to a
     * domain rebuilt since then with stale configuration.
     */
    bool submitVocoderJob(VocoderExecutionJob job, uint64_t expectedGeneration);
    bool isVocoderReady() const noexcept;
    bool isVocoderReconfiguring() const noexcept;

private:
    // 进程寿命 heap 单例（getInstance 显式 new、永不析构）：control worker
    // 与命令队列随进程存活，绝不在实例卸载路径 join 或销毁。
    ProcessRenderRuntime();
    ~ProcessRenderRuntime() = default;

    // Deferred retry list 只處理 render transaction；vocoder domain 的
    // generation 與 reconfigure lifecycle 由 VocoderExecutionCoordinator 擁有。
    struct DeferredRetry {
        std::weak_ptr<ContentRenderService> crs;
        RenderJob job;
    };
    mutable std::mutex deferredRetryMutex_;
    std::vector<DeferredRetry> deferredRetries_;

    std::unique_ptr<VocoderExecutionCoordinator> vocoderExecution_;

    void flushDeferredRetries(bool domainAvailable);
    void deferOrRequeue(std::shared_ptr<ContentRenderService> crs, RenderJob job);

    ProcessRenderRuntime(const ProcessRenderRuntime&) = delete;
    ProcessRenderRuntime& operator=(const ProcessRenderRuntime&) = delete;
};

} // namespace OpenTune
