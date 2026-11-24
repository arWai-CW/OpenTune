#pragma once

#include "PlayHeadState.h"

#include <algorithm>

namespace OpenTune {

struct PlayHeadPresentation
{
    bool playing = false;
    double seconds = 0.0;
};

class PlayHeadPresenter
{
public:
    PlayHeadPresentation present(const PlayHeadState& state, double nowClockSeconds)
    {
        const bool playing = state.isPlaying.load(std::memory_order_acquire);
        const double canonical = state.timeInSeconds.load(std::memory_order_relaxed);
        if (!playing)
        {
            initialized_ = false;
            holding_ = false;
            return {false, canonical};
        }

        const auto epoch = state.presentationEpoch.load(std::memory_order_acquire);
        const auto snapshot = state.presentationProjection.load();
        double target = canonical;
        if (snapshot.valid && snapshot.epoch == epoch)
            target = std::min(snapshot.projectUncappedAt(nowClockSeconds), snapshot.horizon + maxOvershoot);
        if (!std::isfinite(target))
            target = canonical;

        const bool transportJump = initialized_ && epoch != lastEpoch_
            && std::abs(canonical - lastHorizon_) > seekThreshold;
        if (initialized_ && target < lastDisplayed_)
        {
            if (!holding_)
                holdStartedAt_ = nowClockSeconds;
            holding_ = true;
        }
        else
            holding_ = false;

        if (!initialized_ || transportJump || target < lastDisplayed_ - seekThreshold
            || (holding_ && nowClockSeconds - holdStartedAt_ >= maxHoldDuration))
        {
            lastDisplayed_ = target;
            holding_ = false;
        }
        else
            lastDisplayed_ = std::max(lastDisplayed_, target);

        initialized_ = true;
        lastEpoch_ = epoch;
        lastHorizon_ = snapshot.valid ? snapshot.horizon : canonical;
        return {true, lastDisplayed_};
    }

    static constexpr double maxOvershoot = 0.5;
    static constexpr double seekThreshold = 0.25;
    static constexpr double maxHoldDuration = 0.1;

private:
    bool initialized_ = false;
    bool holding_ = false;
    double holdStartedAt_ = 0.0;
    double lastDisplayed_ = 0.0;
    double lastHorizon_ = 0.0;
    uint64_t lastEpoch_ = 0;
};

} // namespace OpenTune
