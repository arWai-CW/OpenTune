#pragma once

#include "Utils/ContentTimelineProjection.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"

namespace OpenTune {

struct PitchReferenceSnapshot
{
    ContentTimelineProjection projection;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    std::shared_ptr<const PitchCurveSnapshot> curve;
    float originalPitchRatio = 1.0f;

    bool operator==(const PitchReferenceSnapshot& other) const noexcept
    {
        return timeGrid == other.timeGrid && curve == other.curve
            && originalPitchRatio == other.originalPitchRatio
            && projection.timelineStartSeconds == other.projection.timelineStartSeconds
            && projection.timelineDurationSeconds == other.projection.timelineDurationSeconds
            && projection.contentStartSeconds == other.projection.contentStartSeconds
            && projection.contentDurationSeconds == other.projection.contentDurationSeconds;
    }
};

} // namespace OpenTune
