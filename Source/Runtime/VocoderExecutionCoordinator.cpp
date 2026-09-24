#include "VocoderExecutionCoordinator.h"

#include "ProcessF0Runtime.h"
#include "../Inference/VocoderDomain.h"
#include "../Utils/AppLogger.h"
#include "../Utils/ModelPathResolver.h"
#include <juce_events/juce_events.h>

namespace OpenTune {

namespace {

class VocoderDomainAdapter final : public VocoderDomainPort
{
public:
    explicit VocoderDomainAdapter(const std::string& modelPath)
    {
        auto& f0Runtime = ProcessF0Runtime::getInstance();
        const auto modelsDirectory = ModelPathResolver::getModelsDirectory();
        if (!f0Runtime.initialize(modelsDirectory))
            return;

        auto env = f0Runtime.getOrtEnv();
        if (env == nullptr)
            return;

        domain_ = std::make_unique<VocoderDomain>(std::move(env));
        AppLogger::info("VocoderDomain: loading weight modelPath=" + modelPath);
        initialized_ = domain_ != nullptr && domain_->initialize(modelPath);
    }

    ~VocoderDomainAdapter() override = default;

    bool submit(VocoderExecutionJob job) override
    {
        if (!initialized_ || domain_ == nullptr)
            return false;

        VocoderDomain::Job domainJob;
        domainJob.f0 = std::move(job.f0);
        domainJob.uv = std::move(job.uv);
        domainJob.conditioning = std::move(job.conditioning);
        domainJob.onComplete = [callback = std::move(job.onComplete)](
            VocoderDomain::JobResult result,
            const juce::String& error,
            const std::vector<float>& audio)
        {
            if (callback)
                callback(static_cast<VocoderExecutionJobResult>(result), error, audio);
        };
        return domain_->submit(std::move(domainJob));
    }

    int getConditioningBins() const override
    {
        return domain_ != nullptr ? domain_->getConditioningBins() : 0;
    }

    VocoderConditioningType getConditioningType() const override
    {
        return domain_ != nullptr
            ? domain_->getConditioningType()
            : VocoderConditioningType::LogMel;
    }

    float getFMax() const override
    {
        return domain_ != nullptr ? domain_->getFMax() : 16000.0f;
    }

private:
    std::unique_ptr<VocoderDomain> domain_;
    bool initialized_{false};
};

} // namespace

VocoderExecutionCoordinator::VocoderExecutionCoordinator(
    VocoderModelWeight initialWeight,
    std::string modelDirectory,
    DomainFactory domainFactory,
    BackendReset backendReset,
    CompletionDispatcher completionDispatcher)
    : modelDirectory_(std::move(modelDirectory)),
      domainFactory_(std::move(domainFactory)),
      backendReset_(std::move(backendReset)),
      completionDispatcher_(std::move(completionDispatcher)),
      currentWeight_(std::move(initialWeight))
{
    if (!completionDispatcher_)
    {
        completionDispatcher_ = [](std::function<void()> completion)
        {
            juce::MessageManager::callAsync(std::move(completion));
        };
    }

    controlWorker_ = std::thread([this] { controlWorkerLoop(); });
}

VocoderExecutionCoordinator::~VocoderExecutionCoordinator()
{
    stop();
}

void VocoderExecutionCoordinator::setReconfigureObserver(ReconfigureObserver observer)
{
    std::lock_guard<std::mutex> lock(observerMutex_);
    reconfigureObserver_ = std::move(observer);
}

std::string VocoderExecutionCoordinator::modelPathForWeight(
    const VocoderModelWeight& weight) const
{
    const juce::File weightDirectory = juce::File(modelDirectory_)
        .getChildFile("vocoder_weights");
    const juce::File preferred = weightDirectory.getChildFile(juce::String(weight));
    if (preferred.existsAsFile())
        return preferred.getFullPathName().toStdString();

    return modelDirectory_ + "/" + weight;
}

std::unique_ptr<VocoderDomainPort> VocoderExecutionCoordinator::createDomain(
    const VocoderModelWeight& weight) const
{
    const auto path = modelPathForWeight(weight);
    if (domainFactory_)
        return domainFactory_(path);

    auto domain = std::make_unique<VocoderDomainAdapter>(path);
    return domain->getConditioningBins() > 0 ? std::move(domain) : nullptr;
}

void VocoderExecutionCoordinator::postCommand(Command command)
{
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (stopping_.load())
            return;
        commandQueue_.push_back(std::move(command));
    }
    controlCv_.notify_all();
}

void VocoderExecutionCoordinator::notifyCompletion(
    std::function<void()> completion) const
{
    if (completion)
        completionDispatcher_(std::move(completion));
}

void VocoderExecutionCoordinator::controlWorkerLoop()
{
    while (true)
    {
        Command command;
        {
            std::unique_lock<std::mutex> lock(controlMutex_);
            controlCv_.wait(lock, [this] {
                return stopping_.load() || !commandQueue_.empty();
            });

            if (stopping_.load())
                return;

            command = std::move(commandQueue_.front());
            commandQueue_.pop_front();
        }

        reconfigure(command);
        notifyCompletion(std::move(command.completion));
    }
}

void VocoderExecutionCoordinator::reconfigure(const Command& command)
{
    std::unique_ptr<VocoderDomainPort> retiredDomain;
    VocoderModelWeight targetWeight;
    bool noOp = false;

    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);

        if (command.type == CommandType::SetVocoderWeight
            && currentWeight_ == command.weight
            && domain_ != nullptr
            && !reconfiguring_)
        {
            noOp = true;
        }
        else
        {
            if (command.type == CommandType::SetVocoderWeight)
                currentWeight_ = command.weight;

            targetWeight = currentWeight_;
            reconfiguring_ = true;
            retiredDomain = std::move(domain_);
            ++generation_;

            // 若 RenderWorker 正在做第一次 lazy initialization，等它離開
            // 临界區後再開始重建，避免同時建立兩個 ONNX session。
            vocoderStateCv_.wait(lock, [this] { return !initializing_; });
        }
    }

    if (noOp)
    {
        ReconfigureObserver observer;
        {
            std::lock_guard<std::mutex> lock(observerMutex_);
            observer = reconfigureObserver_;
        }
        if (observer)
            observer(isReady());
        return;
    }

    // 破壞舊 session 與建立新 session 都不持有 vocoderMutex_。
    retiredDomain.reset();

    if (command.type == CommandType::ResetInferenceBackend && backendReset_)
        backendReset_(command.forceCpu);

    auto newDomain = createDomain(targetWeight);

    bool domainAvailable = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        if (currentWeight_ == targetWeight)
        {
            domain_ = std::move(newDomain);
            if (domain_ != nullptr)
                ++generation_;
        }
        reconfiguring_ = false;
        domainAvailable = domain_ != nullptr;
    }
    vocoderStateCv_.notify_all();

    ReconfigureObserver observer;
    {
        std::lock_guard<std::mutex> lock(observerMutex_);
        observer = reconfigureObserver_;
    }
    if (observer)
        observer(domainAvailable);
}

bool VocoderExecutionCoordinator::acquireConfig(Config& out)
{
    VocoderModelWeight targetWeight;
    uint64_t creationGeneration = 0;

    {
        std::unique_lock<std::mutex> lock(vocoderMutex_);
        if (domain_ != nullptr)
        {
            out.generation = generation_;
            out.conditioningBins = domain_->getConditioningBins();
            out.conditioningType = domain_->getConditioningType();
            out.fMax = domain_->getFMax();
            return true;
        }

        if (initializing_)
        {
            vocoderStateCv_.wait(lock, [this] {
                return !initializing_ || reconfiguring_;
            });

            if (domain_ != nullptr)
            {
                out.generation = generation_;
                out.conditioningBins = domain_->getConditioningBins();
                out.conditioningType = domain_->getConditioningType();
                out.fMax = domain_->getFMax();
                return true;
            }
        }

        if (reconfiguring_)
            return false;

        initializing_ = true;
        targetWeight = currentWeight_;
        creationGeneration = generation_;
    }

    auto newDomain = createDomain(targetWeight);

    bool mayPublish = false;
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        mayPublish = !reconfiguring_
            && initializing_
            && generation_ == creationGeneration
            && currentWeight_ == targetWeight;

        if (mayPublish && newDomain != nullptr)
        {
            domain_ = std::move(newDomain);
            ++generation_;
            out.generation = generation_;
            out.conditioningBins = domain_->getConditioningBins();
            out.conditioningType = domain_->getConditioningType();
            out.fMax = domain_->getFMax();
            published = true;
        }
    }

    newDomain.reset();
    {
        std::lock_guard<std::mutex> lock(vocoderMutex_);
        initializing_ = false;
    }
    vocoderStateCv_.notify_all();
    return published;
}

bool VocoderExecutionCoordinator::submit(
    VocoderExecutionJob job, uint64_t expectedGeneration)
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    if (domain_ == nullptr || generation_ != expectedGeneration)
        return false;

    return domain_->submit(std::move(job));
}

bool VocoderExecutionCoordinator::isReady() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return domain_ != nullptr;
}

bool VocoderExecutionCoordinator::isReconfiguring() const noexcept
{
    std::lock_guard<std::mutex> lock(vocoderMutex_);
    return reconfiguring_;
}

void VocoderExecutionCoordinator::setVocoderModelWeight(
    VocoderModelWeight weight, std::function<void()> completion)
{
    Command command;
    command.type = CommandType::SetVocoderWeight;
    command.weight = std::move(weight);
    command.completion = std::move(completion);
    postCommand(std::move(command));
}

void VocoderExecutionCoordinator::resetVocoder(
    std::function<void()> completion)
{
    Command command;
    command.type = CommandType::ResetVocoder;
    command.completion = std::move(completion);
    postCommand(std::move(command));
}

void VocoderExecutionCoordinator::resetInferenceBackend(
    bool forceCpu, std::function<void()> completion)
{
    Command command;
    command.type = CommandType::ResetInferenceBackend;
    command.forceCpu = forceCpu;
    command.completion = std::move(completion);
    postCommand(std::move(command));
}

void VocoderExecutionCoordinator::stop() noexcept
{
    if (!controlWorker_.joinable())
        return;

    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        stopping_.store(true);
        commandQueue_.clear();
    }
    controlCv_.notify_all();
    controlWorker_.join();
}

} // namespace OpenTune
