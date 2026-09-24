#include "../Source/Runtime/VocoderExecutionCoordinator.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>

using namespace OpenTune;

namespace {

struct FakeState
{
    std::mutex mutex;
    int createCalls{0};
    int submitCalls{0};
    bool createSucceeds{true};
};

class FakeDomain final : public VocoderDomainPort
{
public:
    explicit FakeDomain(FakeState& state)
        : state_(state)
    {
    }

    bool submit(VocoderExecutionJob) override
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        ++state_.submitCalls;
        return true;
    }

    int getConditioningBins() const override { return 80; }
    VocoderConditioningType getConditioningType() const override
    {
        return VocoderConditioningType::LogMel;
    }
    float getFMax() const override { return 16000.0f; }

private:
    FakeState& state_;
};

VocoderExecutionCoordinator::DomainFactory makeFactory(FakeState& state)
{
    return [&state](const std::string&)
        -> std::unique_ptr<VocoderDomainPort>
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.createCalls;
        if (!state.createSucceeds)
            return std::unique_ptr<VocoderDomainPort>();
        return std::make_unique<FakeDomain>(state);
    };
}

bool waitFor(const std::future<void>& completion)
{
    return completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
}

bool testGenerationRejectsStaleSubmit()
{
    FakeState state;
    VocoderExecutionCoordinator coordinator(
        "first.onnx", "/tmp", makeFactory(state), {},
        [](std::function<void()> completion) { completion(); });

    VocoderExecutionCoordinator::Config firstConfig;
    if (!coordinator.acquireConfig(firstConfig))
        return false;

    VocoderExecutionJob job;
    if (!coordinator.submit(std::move(job), firstConfig.generation))
        return false;

    std::promise<void> reconfigDone;
    auto reconfigFuture = reconfigDone.get_future();
    coordinator.setVocoderModelWeight(
        "second.onnx", [&] { reconfigDone.set_value(); });
    if (!waitFor(reconfigFuture))
        return false;

    VocoderExecutionCoordinator::Config secondConfig;
    if (!coordinator.acquireConfig(secondConfig)
        || secondConfig.generation == firstConfig.generation)
        return false;

    if (coordinator.submit(VocoderExecutionJob{}, firstConfig.generation))
        return false;
    if (!coordinator.submit(VocoderExecutionJob{}, secondConfig.generation))
        return false;

    std::lock_guard<std::mutex> lock(state.mutex);
    return state.createCalls == 2 && state.submitCalls == 2;
}

bool testSameWeightCanRetryAfterCreateFailure()
{
    FakeState state;
    state.createSucceeds = false;
    VocoderExecutionCoordinator coordinator(
        "model.onnx", "/tmp", makeFactory(state), {},
        [](std::function<void()> completion) { completion(); });

    std::promise<void> firstDone;
    auto firstFuture = firstDone.get_future();
    coordinator.setVocoderModelWeight(
        "model.onnx", [&] { firstDone.set_value(); });
    if (!waitFor(firstFuture))
        return false;

    state.createSucceeds = true;
    std::promise<void> secondDone;
    auto secondFuture = secondDone.get_future();
    coordinator.setVocoderModelWeight(
        "model.onnx", [&] { secondDone.set_value(); });
    if (!waitFor(secondFuture))
        return false;

    VocoderExecutionCoordinator::Config config;
    if (!coordinator.acquireConfig(config))
        return false;

    std::lock_guard<std::mutex> lock(state.mutex);
    return state.createCalls == 2;
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
    ok &= check("generation rejects stale submit",
                testGenerationRejectsStaleSubmit());
    ok &= check("same weight retries after create failure",
                testSameWeightCanRetryAfterCreateFailure());
    return ok ? 0 : 1;
}
