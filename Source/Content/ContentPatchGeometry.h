#pragma once

/**
 * Pure range-scoped geometry for content note/segment patches.
 *
 * Operates only on notes / PitchCorrectionSegment / frame or second ranges:
 * it knows nothing about ContentKey, DomainKind, owners, revisions, render or
 * Undo. The processor remains the orchestrator that reads the snapshot, calls
 * these helpers, dispatches to the owner by DomainKind and completes the local
 * mutation.
 */

#include "../Utils/Note.h"
#include "../Utils/PitchCurve.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace OpenTune {

// Notes overlapping the affected range in the seconds domain: a note is kept
// when [startTime, endTime) strictly intersects [rangeStartSeconds, rangeEndSeconds).
inline std::vector<Note> filterNotesToRange(const std::vector<Note>& notes,
                                            double rangeStartSeconds,
                                            double rangeEndSeconds)
{
    std::vector<Note> filtered;
    for (const auto& note : notes) {
        if (note.endTime > rangeStartSeconds && note.startTime < rangeEndSeconds)
            filtered.push_back(note);
    }
    return filtered;
}

// Filters incoming segments to the affected frame range and clips boundary
// crossings, maintaining the f0Data offset so each clipped segment keeps the
// samples that belong to its retained frame span. Segments fully outside the
// range, empty after clipping, or inconsistent with f0Data are dropped.
inline std::vector<PitchCorrectionSegment> clipSegmentsToFrameRange(
    const std::vector<PitchCorrectionSegment>& segments,
    int rangeStartFrame,
    int rangeEndFrameExclusive)
{
    std::vector<PitchCorrectionSegment> filtered;
    for (const auto& seg : segments)
        if (auto clipped = clipPitchCorrectionSegment(seg, rangeStartFrame, rangeEndFrameExclusive))
            filtered.push_back(std::move(*clipped));
    return filtered;
}

// Replaces the segments inside the affected frame range with newSegments:
// segments entirely outside the range are kept as-is, boundary-crossing old
// segments are split so their outside parts are preserved (f0Data offsets
// maintained), then newSegments are inserted and the result is sorted by
// startFrame.
inline std::vector<PitchCorrectionSegment> replaceSegmentsInFrameRange(
    const std::vector<PitchCorrectionSegment>& existingSegments,
    const std::vector<PitchCorrectionSegment>& newSegments,
    int rangeStartFrame,
    int rangeEndFrameExclusive)
{
    std::vector<PitchCorrectionSegment> mergedSegments;
    mergedSegments.reserve(existingSegments.size() + newSegments.size());

    for (const auto& seg : existingSegments) {
        // Segment entirely outside range -> keep as-is
        if (seg.endFrame <= rangeStartFrame || seg.startFrame >= rangeEndFrameExclusive) {
            mergedSegments.push_back(seg);
            continue;
        }

        if (auto left = clipPitchCorrectionSegment(seg, seg.startFrame, rangeStartFrame))
            mergedSegments.push_back(std::move(*left));
        if (auto right = clipPitchCorrectionSegment(seg, rangeEndFrameExclusive, seg.endFrame))
            mergedSegments.push_back(std::move(*right));
    }

    // Callers already filtered the new segments to range-overlapping only.
    mergedSegments.insert(mergedSegments.end(), newSegments.begin(), newSegments.end());

    std::sort(mergedSegments.begin(), mergedSegments.end(),
              [](const PitchCorrectionSegment& a, const PitchCorrectionSegment& b) {
                  return a.startFrame < b.startFrame;
              });
    return mergedSegments;
}

} // namespace OpenTune
