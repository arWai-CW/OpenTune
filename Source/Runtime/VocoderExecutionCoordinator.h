#pragma once

#include "../Inference/VocoderInterface.h"
#include "../Utils/VocoderModelWeight.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OpenTune {

enum class VocoderExecutionJobResult : uint8_t
{
    Succeeded,
    Cancelled,
    Failed
};

struct VocoderExecutionJob
{
    std::vector<float> f0;
    std::vector<float> uv;
    std::vector<float> conditioning;
    std::function<void(VocoderExecutionJobResult,
                       const juce::String&,
                       const std::vector<float>&)> onComplete;
};

/**
 * 可替換的 vocoder domain interface。
 *
 * Production 使用 VocoderDomainAdapter；測試可提供不需要 ONNX 的 fake domain，
 * 讓 generation、submit 與 reconfigure 行為能在沒有模型檔案的環境驗證。
 */
class VocoderDomainPort
{
public:
    virtual ~VocoderDomainPort() = default;

    virtual bool submit(VocoderExecutionJob job) = 0;
    virtual int getConditioningBins() const = 0;
    virtual VocoderConditioningType getConditioningType() const = 0;
    virtual float getFMax() const = 0;
};

/**
 * Process-level vocoder execution module。
 *
 * 這個 module 隱藏 model/backend 的建立、破壞、generation、lazy initialization
 * 與 control worker；caller 只取得一次 config snapshot，然後以該 generation
 * submit。module 不處理 PCM、RenderCache 或 ContentRenderService retry。
 */
class VocoderExecutionCoordinator
{
public:
    struct Config
    {
        uint64_t generation{0};
        int conditioningBins{0};
        VocoderConditioningType conditioningType{VocoderConditioningType::LogMel};
        float fMax{16000.0f};
    };

    using DomainFactory = std::function<std::unique_ptr<VocoderDomainPort>(
        const std::string& modelPath)>;
    using BackendReset = std::function<void(bool forceCpu)>;
    using CompletionDispatcher = std::function<void(std::function<void()>)>;
    using ReconfigureObserver = std::function<void(bool domainAvailable)>;

    VocoderExecutionCoordinator(
        VocoderModelWeight initialWeight,
        std::string modelDirectory,
        DomainFactory domainFactory = {},
        BackendReset backendReset = {},
        CompletionDispatcher completionDispatcher = {});
    ~VocoderExecutionCoordinator();

    VocoderExecutionCoordinator(const VocoderExecutionCoordinator&) = delete;
    VocoderExecutionCoordinator& operator=(const VocoderExecutionCoordinator&) = delete;

    void setReconfigureObserver(ReconfigureObserver observer);

    void setVocoderModelWeight(
        VocoderModelWeight weight, std::function<void()> completion = {});
    void resetVocoder(std::function<void()> completion = {});
    void resetInferenceBackend(
        bool forceCpu, std::function<void()> completion = {});

    bool acquireConfig(Config& out);
    bool submit(VocoderExecutionJob job, uint64_t expectedGeneration);
    bool isReady() const noexcept;
    bool isReconfiguring() const noexcept;

    void stop() noexcept;

private:
    enum class CommandType : uint8_t
    {
        SetVocoderWeight,
        ResetVocoder,
        ResetInferenceBackend
    };

    struct Command
    {
        CommandType type{CommandType::ResetVocoder};
        VocoderModelWeight weight{kDefaultVocoderWeight};
        bool forceCpu{false};
        std::function<void()> completion;
    };

    std::unique_ptr<VocoderDomainPort> createDomain(
        const VocoderModelWeight& weight) const;
    std::string modelPathForWeight(const VocoderModelWeight& weight) const;

    void controlWorkerLoop();
    void postCommand(Command command);
    void reconfigure(const Command& command);
    void notifyCompletion(std::function<void()> completion) const;

    std::string modelDirectory_;
    DomainFactory domainFactory_;
    BackendReset backendReset_;
    CompletionDispatcher completionDispatcher_;

    mutable std::mutex controlMutex_;
    std::condition_variable controlCv_;
    std::deque<Command> commandQueue_;
    std::thread controlWorker_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex observerMutex_;
    ReconfigureObserver reconfigureObserver_;

    mutable std::mutex vocoderMutex_;
    std::condition_variable vocoderStateCv_;
    std::unique_ptr<VocoderDomainPort> domain_;
    VocoderModelWeight currentWeight_{kDefaultVocoderWeight};
    bool reconfiguring_{false};
    bool initializing_{false};
    uint64_t generation_{0};
};

} // namespace OpenTune
