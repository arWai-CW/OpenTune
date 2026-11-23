#include "../Source/Utils/PlayHeadState.h"

#include <cstdio>
#include <array>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

juce::Optional<juce::AudioPlayHead::PositionInfo> makePosition(double seconds, bool playing)
{
    juce::AudioPlayHead::PositionInfo position;
    position.setTimeInSeconds(seconds);
    position.setIsPlaying(playing);
    return juce::Optional<juce::AudioPlayHead::PositionInfo>(position);
}

void testConcurrentDuplicateObservationCommitsOnce()
{
    OpenTune::PlayHeadState state;
    std::atomic<bool> start{false};
    std::array<std::thread, 8> observers;
    std::array<bool, 8> committed{};
    for (size_t i = 0; i < observers.size(); ++i)
        observers[i] = std::thread([&, i] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            committed[i] = state.update(makePosition(42.0, false)).timeChanged;
        });
    start.store(true, std::memory_order_release);
    for (auto& observer : observers)
        observer.join();
    int commits = 0;
    for (bool value : committed)
        commits += value ? 1 : 0;
    expect(commits == 1, "concurrent duplicate times have one committing observer");
    expect(state.presentationEpoch.load() == 1, "duplicate observers advance one epoch");
    expect(!state.update(juce::nullopt).timeChanged,
           "absent transport information preserves the observed state");
}

void testContiguousBlocksKeepPresentationClock()
{
    OpenTune::PlayHeadState state;
    state.isPlaying.store(true);
    auto& projection = state.presentationProjection;
    expect(!projection.load().valid, "empty projection has no anchor");
    expect(state.getPresentedPositionAt(10.0) == 0.0, "empty projection uses canonical time");
    projection.publish(0.0, 10.0, 0.02, 0);

    double previous = 0.0;
    for (int block = 1; block <= 100; ++block)
    {
        const double position = block * 0.02;
        const double callbackClock = 10.0 + position - (block % 2 ? 0.008 : 0.002);
        const double before = state.getPresentedPositionAt(callbackClock);
        projection.publish(position, callbackClock, position + 0.02, 0);
        const double after = state.getPresentedPositionAt(callbackClock);
        expect(std::abs(after - before) < 1.0e-9, "early callback does not jump presentation forward");
        expect(after >= previous, "jittered callbacks preserve monotonic playback");
        expect(std::abs(after - (callbackClock - 10.0)) < 1.0e-9,
               "presentation advances at wall-clock speed, independent of callbacks");
        previous = after;
    }
    expect(state.getPresentedPositionAt(100.0) == projection.load().horizon,
           "missing callbacks cannot project beyond committed audio");

    projection.publish(5.0, 20.0, 5.02, 0);
    expect(state.getPresentedPositionAt(20.0) == 5.0, "discontinuous position resets anchor");
    state.presentationEpoch.store(1);
    state.timeInSeconds.store(1.0);
    expect(state.getPresentedPositionAt(20.0) == 1.0, "stale epoch falls back to canonical seek");
    projection.publish(5.02, 30.0, 5.04, 1);
    expect(state.getPresentedPositionAt(30.0) == 5.02, "new epoch resets even a contiguous anchor");
    state.isPlaying.store(false);
    expect(state.getPresentedPositionAt(31.0) == 1.0, "stopped transport does not extrapolate");
}

} // namespace

int main()
{
    testConcurrentDuplicateObservationCommitsOnce();
    testContiguousBlocksKeepPresentationClock();
    if (failures == 0)
        std::puts("PlayHeadStateTests: PASS (duplicate writers, jittered blocks, horizon, seek, stop)");
    return failures == 0 ? 0 : 1;
}
