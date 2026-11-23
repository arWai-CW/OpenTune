#include "PitchReferenceSnapshot.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {
namespace {

constexpr double kReferencePitchCenterWindowSeconds = 0.4;
constexpr double kReferencePitchOffsetJumpThresholdCents = 50.0;
constexpr double kReferencePitchOffsetMedianWindowSeconds = 0.12;
constexpr double kReferencePitchChangeToleranceCents = 0.5;
constexpr double kPi = 3.14159265358979323846;

bool isVoiced(float hz)
{
    return hz > 0.0f && std::isfinite(hz);
}

bool canSample(const PitchReferenceSnapshot& region)
{
    return region.projection.isValid() && region.timeGrid != nullptr && region.curve != nullptr
        && !region.curve->isEmpty() && region.curve->getHopSize() > 0
        && region.curve->getSampleRate() > 0.0;
}

float effectivePitch(const PitchReferenceSnapshot& region, int frame)
{
    float hz = 0.0f;
    region.curve->forEachEffectiveF0Span(frame, frame + 1, region.originalPitchRatio,
        [&](int, const float* data, int, float gain) { hz = *data * gain; });
    return hz;
}

} // namespace

PitchReferenceCorrection planPitchReferenceCorrection(
    const PitchReferenceSnapshot& target,
    const std::vector<PitchReferenceSnapshot>& references,
    const std::vector<F0FrameRange>& ranges)
{
    PitchReferenceCorrection result;
    if (!canSample(target))
        return result;

    struct ReferenceRegion
    {
        const PitchReferenceSnapshot& snapshot;
        F0Timeline timeline;
    };
    std::vector<ReferenceRegion> overlapping;
    const auto& placement = target.projection;
    for (const auto& reference : references) {
        const auto& refPlacement = reference.projection;
        if (canSample(reference) && halfOpenRangesOverlap(
                placement.timelineStartSeconds, placement.timelineEndSeconds(),
                refPlacement.timelineStartSeconds, refPlacement.timelineEndSeconds())) {
            overlapping.push_back({reference, {reference.curve->getHopSize(),
                reference.curve->getSampleRate(), static_cast<int>(reference.curve->size())}});
        }
    }
    if (overlapping.empty())
        return result;
    result.hasClipOverlap = true;

    const auto pitchAtTimelineTime = [&](double time) {
        for (const auto& [ref, timeline] : overlapping) {
            const auto& p = ref.projection;
            if (time < p.timelineStartSeconds || time >= p.timelineEndSeconds())
                continue;
            const double source = ref.timeGrid->tauInverse(p.projectTimelineTimeToContent(time));
            if (!(source >= 0.0 && source < timeline.timeAtFrame(timeline.endFrameExclusive())))
                continue;
            const int frame = std::min(timeline.nearestFrameBoundary(source), timeline.endFrameExclusive() - 1);
            if (const float hz = effectivePitch(ref, frame); isVoiced(hz))
                return hz;
        }
        return 0.0f;
    };

    const F0Timeline timeline{target.curve->getHopSize(), target.curve->getSampleRate(),
                              static_cast<int>(target.curve->size())};
    const double frameSeconds = timeline.timeAtFrame(1);
    int centerWindowFrames = std::max(1, static_cast<int>(std::lround(
        kReferencePitchCenterWindowSeconds / frameSeconds)));
    if (centerWindowFrames % 2 == 0)
        ++centerWindowFrames;
    const int centerWindowRadius = centerWindowFrames / 2;
    int medianWindowFrames = std::max(1, static_cast<int>(std::lround(
        kReferencePitchOffsetMedianWindowSeconds / frameSeconds)));
    if (medianWindowFrames % 2 == 0)
        ++medianWindowFrames;
    const int medianWindowRadius = medianWindowFrames / 2;
    const auto& originalF0 = target.curve->getOriginalF0();

    struct RunFrame
    {
        float baseHz;
        float offsetCents;
    };

    for (const auto& range : ranges) {
        std::vector<RunFrame> run;
        int runStartFrame = 0;
        const auto finishRun = [&] {
            if (run.empty())
                return;
            const int count = static_cast<int>(run.size());
            std::vector<float> filteredOffsets(static_cast<size_t>(count));
            std::vector<float> medianWindow;
            medianWindow.reserve(static_cast<size_t>(medianWindowFrames));
            for (int i = 0; i < count; ++i) {
                const int begin = std::max(0, i - medianWindowRadius);
                const int end = std::min(count, i + medianWindowRadius + 1);
                medianWindow.clear();
                for (int j = begin; j < end; ++j)
                    medianWindow.push_back(run[static_cast<size_t>(j)].offsetCents);
                std::sort(medianWindow.begin(), medianWindow.end());
                const size_t middle = medianWindow.size() / 2;
                float median = medianWindow[middle];
                if (medianWindow.size() % 2 == 0)
                    median = (medianWindow[middle - 1] + medianWindow[middle]) * 0.5f;
                filteredOffsets[static_cast<size_t>(i)] = median;
            }

            std::vector<int> boundaries{0, count};
            for (const auto& note : target.notes) {
                const int noteStart = timeline.nearestFrameBoundary(note.startTime) - runStartFrame;
                const int noteEnd = timeline.nearestFrameBoundary(note.endTime) - runStartFrame;
                if (noteStart > 0 && noteStart < count)
                    boundaries.push_back(noteStart);
                if (noteEnd > 0 && noteEnd < count)
                    boundaries.push_back(noteEnd);
            }
            for (int i = 1; i < count; ++i) {
                if (std::abs(static_cast<double>(filteredOffsets[static_cast<size_t>(i)])
                        - filteredOffsets[static_cast<size_t>(i - 1)]) > kReferencePitchOffsetJumpThresholdCents)
                    boundaries.push_back(i);
            }
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

            PitchCorrectionSegment segment{runStartFrame, runStartFrame + count, {},
                                           PitchCorrectionSegment::Source::HandDraw};
            segment.f0Data.reserve(run.size());
            bool changed = false;
            for (int i = 0; i < count; ++i) {
                const auto endBoundary = std::upper_bound(boundaries.begin(), boundaries.end(), i);
                const int smoothingEnd = *endBoundary;
                const int smoothingStart = *(endBoundary - 1);
                const int begin = std::max(smoothingStart, i - centerWindowRadius);
                const int end = std::min(smoothingEnd, i + centerWindowRadius + 1);
                double weightedOffsetSum = 0.0;
                double weightSum = 0.0;
                for (int j = begin; j < end; ++j) {
                    const int lag = j > i ? j - i : i - j;
                    const double weight = centerWindowRadius == 0 ? 1.0
                        : 0.5 * (1.0 + std::cos(kPi * lag / centerWindowRadius));
                    weightedOffsetSum += filteredOffsets[static_cast<size_t>(j)] * weight;
                    weightSum += weight;
                }
                const double smoothedCents = weightedOffsetSum / weightSum;
                const float correctedHz = static_cast<float>(run[static_cast<size_t>(i)].baseHz
                    * std::exp2(smoothedCents / 1200.0));
                segment.f0Data.push_back(correctedHz);
                const float currentHz = effectivePitch(target, runStartFrame + i);
                if (!isVoiced(currentHz) || std::abs(1200.0 * std::log2(
                        static_cast<double>(correctedHz) / currentHz)) > kReferencePitchChangeToleranceCents)
                    changed = true;
            }
            if (changed)
                result.segments.push_back(std::move(segment));
            run.clear();
        };
        for (int frame = std::max(0, range.startFrame);
             frame < std::min(range.endFrameExclusive, timeline.endFrameExclusive()); ++frame) {
            float referenceHz = 0.0f;
            const float baseHz = originalF0[static_cast<size_t>(frame)] * target.originalPitchRatio;
            if (isVoiced(baseHz)) {
                const double time = placement.projectContentTimeToTimeline(
                    target.timeGrid->tauForward(timeline.timeAtFrame(frame)));
                if (time >= placement.timelineStartSeconds && time < placement.timelineEndSeconds())
                    referenceHz = pitchAtTimelineTime(time);
            }
            if (!isVoiced(referenceHz)) {
                finishRun();
                continue;
            }
            result.hasVoicedOverlap = true;
            if (run.empty())
                runStartFrame = frame;
            double offsetCents = 1200.0 * std::log2(
                static_cast<double>(referenceHz) / baseHz);
            offsetCents -= 1200.0 * std::round(offsetCents / 1200.0);
            run.push_back({baseHz, static_cast<float>(offsetCents)});
        }
        finishRun();
    }
    return result;
}

} // namespace OpenTune
