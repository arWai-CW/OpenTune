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

void testAraEpochChangesAndBursts()
{
    OpenTune::PlayHeadState state;
    auto publishBlock = [&](double position, double clock) {
        const auto update = state.update(makePosition(position, true));
        expect(state.presentationProjection.tryPublish(position, clock, position + 0.02, update.epoch),
               "ARA block publishes its new epoch");
        expect(state.presentationProjection.load().epoch == update.epoch,
               "retained ARA anchor carries the new epoch");
    };
    publishBlock(0.0, 10.0);
    double previous = 0.0;
    for (int block = 1; block <= 100; ++block)
    {
        const double position = block * 0.02;
        const double clock = 10.0 + position - (block % 2 ? 0.008 : 0.002);
        const double before = state.getPresentedPositionAt(clock);
        publishBlock(position, clock);
        const double after = state.getPresentedPositionAt(clock);
        expect(std::abs(after - before) < 1.0e-9, "ARA epoch bump does not jump forward");
        expect(after >= previous, "ARA jitter remains monotonic");
        previous = after;
    }

    OpenTune::PlayHeadState burstState;
    for (int block = 0; block < 8; ++block)
    {
        const double position = block * 0.02;
        const double before = burstState.getPresentedPositionAt(20.0);
        const auto update = burstState.update(makePosition(position, true));
        expect(burstState.presentationProjection.tryPublish(position, 20.0, position + 0.02, update.epoch),
               "ARA burst block publishes");
        expect(std::abs(burstState.getPresentedPositionAt(20.0) - before) < 1.0e-9,
               "burst delivery does not jump ahead of wall clock");
    }
    previous = burstState.getPresentedPositionAt(20.0);
    for (int frame = 1; frame <= 15; ++frame)
    {
        const double position = burstState.getPresentedPositionAt(20.0 + frame * 0.01);
        expect(std::abs(position - previous - 0.01) < 1.0e-9,
               "burst gap advances one frame interval without holds or jumps");
        previous = position;
    }
    const auto epoch = burstState.presentationEpoch.load();
    expect(!burstState.presentationProjection.tryPublish(9.0, 21.0, 9.02, epoch - 1),
           "stale ARA publisher cannot replace the current anchor");
    burstState.presentationProjection.sequence.fetch_add(1);
    expect(!burstState.presentationProjection.tryPublish(9.0, 21.0, 9.02, epoch),
           "contended ARA publisher returns without spinning");
    burstState.presentationProjection.sequence.fetch_add(1);
}

void testLateCallbackAndTransportResets()
{
    for (bool ara : {false, true})
    {
        OpenTune::PlayHeadPresentationProjection projection;
        auto publish = [&](double position, double clock, uint64_t epoch) {
            if (ara)
                expect(projection.tryPublish(position, clock, position + 0.02, epoch),
                       "ARA reset publishes");
            else
                projection.publish(position, clock, position + 0.02, 0);
        };
        publish(0.0, 9.995, 0);
        publish(0.02, 10.02, 1);
        expect(std::abs(projection.load().anchorClock - 10.02) < 1.0e-9,
               "regular callback replaces the early clock origin");
        double previous = projection.load().projectAt(10.02);
        for (int frame = 1; frame <= 200; ++frame)
        {
            const double clock = 10.02 + frame * 0.001;
            if (frame % 20 == 0)
                publish(0.02 + frame * 0.001, clock, 1 + frame / 20);
            const double position = projection.load().projectAt(clock);
            expect(std::abs(position - previous - 0.001) < 1.0e-9,
                   "after late callback each frame advances without hold-then-jump");
            previous = position;
        }
        publish(0.24, 11.0, 20);
        expect(projection.load().anchorClock == 11.0,
               "underrun or resume after stop resets a stale contiguous anchor");
        publish(5.0, 11.01, 21);
        expect(projection.load().projectAt(11.01) == 5.0, "seek resets the anchor");
        publish(0.0, 11.02, 22);
        expect(projection.load().projectAt(11.02) == 0.0, "loop backwards resets the anchor");
        for (int block = 1; block <= 30; ++block)
            publish(block * 0.02, 11.02, 22 + block);
        expect(projection.load().anchorPosition >= 0.5,
               "excessive render ahead resets instead of accumulating unbounded lag");
    }
}

void testConcurrentAraAnchorRetention()
{
    OpenTune::PlayHeadState state;
    auto initial = state.update(makePosition(-0.02, true));
    state.presentationProjection.tryPublish(-0.02, 9.98, 0.0, initial.epoch);
    for (int block = 0; block < 16; ++block)
    {
        const double position = block * 0.02;
        const double clock = 10.0 + position - 0.005;
        const double before = state.getPresentedPositionAt(clock);
        std::atomic<bool> start{false};
        std::atomic<int> published{0};
        std::array<std::thread, 4> writers;
        for (auto& writer : writers)
            writer = std::thread([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                const auto update = state.update(makePosition(position, true));
                if (update.timeChanged
                    && state.presentationProjection.tryPublish(position, clock, position + 0.02, update.epoch))
                    published.fetch_add(1);
            });
        start.store(true, std::memory_order_release);
        for (auto& writer : writers)
            writer.join();
        expect(published.load() == 1, "duplicate ARA writers publish one anchor per block");
        expect(std::abs(state.getPresentedPositionAt(clock) - before) < 1.0e-9,
               "concurrent ARA epoch changes retain a continuous clock");
    }
}

} // namespace

int main()
{
    testConcurrentDuplicateObservationCommitsOnce();
    testContiguousBlocksKeepPresentationClock();
    testAraEpochChangesAndBursts();
    testLateCallbackAndTransportResets();
    testConcurrentAraAnchorRetention();
    if (failures == 0)
        std::puts("PlayHeadStateTests: PASS (duplicate writers, standalone/ARA jitter, epoch changes, bursts, late callbacks, underrun/resume, seek/loop, bounded lead, horizon, stop)");
    return failures == 0 ? 0 : 1;
}
