#include "PitchReferenceSnapshot.h"

namespace OpenTune {
namespace {

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
    const auto& originalF0 = target.curve->getOriginalF0();
    for (const auto& range : ranges) {
        PitchCorrectionSegment run{0, 0, {}, PitchCorrectionSegment::Source::HandDraw};
        bool changed = false;
        const auto finishRun = [&] {
            if (changed)
                result.segments.push_back(std::move(run));
            run = {0, 0, {}, PitchCorrectionSegment::Source::HandDraw};
            changed = false;
        };
        for (int frame = std::max(0, range.startFrame);
             frame < std::min(range.endFrameExclusive, timeline.endFrameExclusive()); ++frame) {
            float hz = 0.0f;
            if (isVoiced(originalF0[static_cast<size_t>(frame)])) {
                const double time = placement.projectContentTimeToTimeline(
                    target.timeGrid->tauForward(timeline.timeAtFrame(frame)));
                if (time >= placement.timelineStartSeconds && time < placement.timelineEndSeconds())
                    hz = pitchAtTimelineTime(time);
            }
            if (!isVoiced(hz)) {
                if (!run.f0Data.empty())
                    finishRun();
                continue;
            }
            result.hasVoicedOverlap = true;
            if (run.f0Data.empty())
                run.startFrame = frame;
            run.endFrame = frame + 1;
            run.f0Data.push_back(hz);
            changed = changed || hz != effectivePitch(target, frame);
        }
        if (changed)
            result.segments.push_back(std::move(run));
    }
    return result;
}

} // namespace OpenTune
