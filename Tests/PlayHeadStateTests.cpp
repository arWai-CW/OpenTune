#include "../Source/Utils/PlayHeadState.h"
#include "../Source/Utils/PlayHeadPresentation.h"

#include <cstdio>
#include <array>
#include <thread>
#include <limits>

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

void testPresenterDuringCallbackGaps()
{
    OpenTune::PlayHeadState state;
    OpenTune::PlayHeadPresenter presenter;
    auto update = state.update(makePosition(0.0, true));
    state.presentationProjection.tryPublish(0.0, 10.0, 0.02, update.epoch);
    double previous = 0.0;
    presenter.present(state, 10.0);
    int nextPublish = 10;
    int burstIndex = 0;
    for (int frame = 1; frame <= 300; ++frame)
    {
        const double seconds = frame * 0.01;
        if (frame == nextPublish)
        {
            update = state.update(makePosition(seconds, true));
            state.presentationProjection.tryPublish(seconds, 10.0 + seconds, seconds + 0.02, update.epoch);
            nextPublish += (++burstIndex % 3 + 1) * 10;
            const double beforeBurst = presenter.present(state, 10.0 + seconds).seconds;
            for (int block = 1; block < 4; ++block)
            {
                const double position = seconds + block * 0.02;
                update = state.update(makePosition(position, true));
                state.presentationProjection.tryPublish(position, 10.0 + seconds, position + 0.02, update.epoch);
            }
            expect(std::abs(presenter.present(state, 10.0 + seconds).seconds - beforeBurst) < 1.0e-9,
                   "presenter does not jump during a contiguous burst");
        }
        const auto presentation = presenter.present(state, 10.0 + seconds);
        expect(presentation.playing, "presenter stays playing during callback gaps");
        expect(std::abs(presentation.seconds - previous - 0.01) < 1.0e-9,
               "presenter crosses sparse callback gaps at one frame delta");
        expect(presentation.seconds >= previous, "sparse playback never steps backwards");
        previous = presentation.seconds;
    }
    const auto snapshot = state.presentationProjection.load();
    const auto capped = presenter.present(state, 100.0);
    expect(std::abs(capped.seconds - snapshot.horizon - OpenTune::PlayHeadPresenter::maxOvershoot) < 1.0e-9,
           "presenter bounds extrapolation after long callback starvation");
    expect(state.getPresentedPositionAt(100.0) == snapshot.horizon,
           "other callers retain the committed horizon cap");
}

void testPresenterOffsetsAndTransportChanges()
{
    OpenTune::PlayHeadState state;
    OpenTune::PlayHeadPresenter presenter;
    double previous = 0.0;
    for (int frame = 0; frame <= 100; ++frame)
    {
        const double seconds = 2.0 + frame * 0.01;
        const double callbackPosition = seconds + (frame % 2 ? 0.02 : -0.02);
        const auto update = state.update(makePosition(callbackPosition, true));
        state.presentationProjection.tryPublish(callbackPosition, 10.0 + seconds,
                                                callbackPosition + 0.02, update.epoch);
        const auto presentation = presenter.present(state, 10.0 + seconds);
        expect(presentation.seconds >= previous, "presenter suppresses backwards flicker from offset writers");
        previous = presentation.seconds;
    }
    auto update = state.update(makePosition(1.0, true));
    state.presentationProjection.tryPublish(1.0, 14.0, 1.02, update.epoch);
    expect(presenter.present(state, 14.0).seconds == 1.0, "presenter snaps on backwards seek");
    update = state.update(makePosition(0.0, true));
    state.presentationProjection.tryPublish(0.0, 14.02, 0.02, update.epoch);
    expect(presenter.present(state, 14.02).seconds == 0.0, "presenter snaps on loop wrap");
    state.update(makePosition(0.1, false));
    const auto stopped = presenter.present(state, 15.0);
    expect(!stopped.playing && stopped.seconds == 0.1, "stop presents canonical time and resets state");
    update = state.update(makePosition(0.0, true));
    state.presentationProjection.tryPublish(0.0, 16.0, 0.02, update.epoch);
    expect(presenter.present(state, 16.0).seconds == 0.0, "restart does not inherit the previous display floor");
    expect(std::abs(presenter.present(state, 16.4).seconds - 0.4) < 1.0e-9,
           "presenter can bridge a gap before an epoch-changing seek");
    update = state.update(makePosition(0.3, true));
    state.presentationProjection.tryPublish(0.3, 16.4, 0.32, update.epoch);
    expect(presenter.present(state, 16.4).seconds == 0.3,
           "non-contiguous epoch change snaps even a small displayed backwards move");
    state.presentationProjection.tryPublish(0.0, 16.5, 0.02, update.epoch);
    expect(presenter.present(state, 16.5).seconds == 0.0,
           "large backwards target snaps even without an epoch change");

    OpenTune::PlayHeadState empty;
    const auto initial = presenter.present(empty, 17.0);
    expect(!initial.playing && initial.seconds == 0.0, "empty transport stays stopped");
    empty.update(makePosition(0.2, true));
    expect(presenter.present(empty, 17.0).seconds == 0.2, "missing anchor falls back to canonical time");
    empty.presentationProjection.publish(std::numeric_limits<double>::quiet_NaN(), 17.0, 0.22,
                                        empty.presentationEpoch.load());
    expect(presenter.present(empty, 17.0).seconds == 0.2, "invalid anchor falls back to canonical time");
}

void testPresenterShortLoopsAndSmallSeeks()
{
    OpenTune::PlayHeadState state;
    OpenTune::PlayHeadPresenter presenter;
    for (int frame = 0; frame < 120; ++frame)
    {
        const double clock = 10.0 + frame * 0.01;
        const double target = (frame % 20) * 0.01;
        const auto update = state.update(makePosition(target, true));
        state.presentationProjection.tryPublish(target, clock, target + 0.01, update.epoch);
        const double displayed = presenter.present(state, clock).seconds;
        if (frame % 20 >= 11)
            expect(std::abs(displayed - target) < 1.0e-9,
                   "short loop follows target within hold duration plus a frame");
    }

    OpenTune::PlayHeadState seekState;
    OpenTune::PlayHeadPresenter seekPresenter;
    auto update = seekState.update(makePosition(1.0, true));
    seekState.presentationProjection.tryPublish(1.0, 20.0, 1.01, update.epoch);
    seekPresenter.present(seekState, 20.0);
    for (int frame = 0; frame <= 12; ++frame)
    {
        const double target = 0.9 + frame * 0.005;
        const double clock = 20.01 + frame * 0.01;
        update = seekState.update(makePosition(target, true));
        seekState.presentationProjection.tryPublish(target, clock, target + 0.005, update.epoch);
        const double displayed = seekPresenter.present(seekState, clock).seconds;
        if (frame == 0)
            expect(displayed == 1.0, "small backwards target initially holds to suppress flicker");
        if (frame >= 11)
            expect(std::abs(displayed - target) < 1.0e-9,
                   "small seek snaps within hold duration plus a frame");
    }
}

void testPresenterDenseCallbacks()
{
    OpenTune::PlayHeadState state;
    OpenTune::PlayHeadPresenter presenter;
    for (int block = 0; block < 100; ++block)
    {
        const double seconds = block * 0.02;
        const auto update = state.update(makePosition(seconds, true));
        state.presentationProjection.tryPublish(seconds, 10.0 + seconds, seconds + 0.02, update.epoch);
        for (int frame = 0; frame < 4; ++frame)
        {
            const double clock = 10.0 + seconds + frame * 0.005;
            expect(std::abs(presenter.present(state, clock).seconds - state.getPresentedPositionAt(clock)) < 1.0e-9,
                   "dense callbacks match the existing projection");
        }
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
    testPresenterDuringCallbackGaps();
    testPresenterOffsetsAndTransportChanges();
    testPresenterDenseCallbacks();
    testPresenterShortLoopsAndSmallSeeks();
    if (failures == 0)
        std::puts("PlayHeadStateTests: PASS (earlier projection regressions, sparse bursts, offset writers, seek/loop, short loops, small seeks, stop/reset, bounded overshoot, dense callbacks, missing/invalid anchors)");
    return failures == 0 ? 0 : 1;
}
