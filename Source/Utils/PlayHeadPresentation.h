#pragma once

#include "PlayHeadState.h"

#include <optional>

namespace OpenTune {

struct PlayHeadPresentation
{
    bool playing = false;
    double seconds = 0.0;
};

inline PlayHeadPresentation resolvePlayHeadPresentation(
    const PlayHeadState& state, double nowClockSeconds,
    std::optional<PlayHeadPresentation> hostTransport = std::nullopt)
{
    if (hostTransport && std::isfinite(hostTransport->seconds))
        return *hostTransport;

    return {state.isPlaying.load(std::memory_order_acquire),
            state.getPresentedPositionAt(nowClockSeconds)};
}

} // namespace OpenTune
