#pragma once

#include "Utils/ContentTimelineProjection.h"
#include "Utils/Note.h"
#include "Utils/PitchCurve.h"
#include "Utils/TimeGrid.h"

namespace OpenTune {

struct PitchReferenceSnapshot
{
    ContentTimelineProjection projection;
    std::shared_ptr<const TimeGridSnapshot> timeGrid;
    std::shared_ptr<const PitchCurveSnapshot> curve;
    float originalPitchRatio = 1.0f;
    std::vector<Note> notes;

    bool operator==(const PitchReferenceSnapshot& other) const noexcept
    {
        if (!(timeGrid == other.timeGrid && curve == other.curve
            && originalPitchRatio == other.originalPitchRatio
            && projection.timelineStartSeconds == other.projection.timelineStartSeconds
            && projection.timelineDurationSeconds == other.projection.timelineDurationSeconds
            && projection.contentStartSeconds == other.projection.contentStartSeconds
            && projection.contentDurationSeconds == other.projection.contentDurationSeconds)
            || notes.size() != other.notes.size())
            return false;
        for (size_t i = 0; i < notes.size(); ++i) {
            if (notes[i].startTime != other.notes[i].startTime
                || notes[i].endTime != other.notes[i].endTime)
                return false;
        }
        return true;
    }
};

struct PitchReferenceCorrection
{
    std::vector<PitchCorrectionSegment> segments;
    bool hasClipOverlap = false;
    bool hasVoicedOverlap = false;
};

PitchReferenceCorrection planPitchReferenceCorrection(
    const PitchReferenceSnapshot& target,
    const std::vector<PitchReferenceSnapshot>& references,
    const std::vector<F0FrameRange>& ranges);

} // namespace OpenTune
