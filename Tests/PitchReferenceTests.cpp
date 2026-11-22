#include "Content/PitchReferenceSnapshot.h"
#include "UI/PianoRoll/PianoRollTimeMap.h"
#include "Utils/EditorUiSync.h"

#include <cmath>
#include <cstdio>

namespace {
using namespace OpenTune;

bool testTrimmedReferenceUsesWarpAndPlacement()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 1.5, HandleKind::UserAdded},
        {3, 3.0, 3.0, HandleKind::ClipEnd},
    });
    if (!grid)
        return false;
    const PitchReferenceSnapshot reference{{10.0, 2.0, 0.5, 2.0}, grid, nullptr};
    ViewMapper view;
    view.visibleStartSeconds = 10.0;
    view.pixelsPerSecond = 100.0;
    view.contentStartX = 40;
    const PianoRollTimeMap map(reference.projection, *reference.timeGrid, view);
    return map.sourceToX(1.0) == 140 && std::abs(map.xToSource(140) - 1.0) < 1.0e-9;
}

bool testEffectivePitchAndChangeDetection()
{
    const auto curve = std::make_shared<const PitchCurveSnapshot>(
        std::vector<float>{100.0f, 110.0f, 0.0f, 120.0f}, std::vector<float>{},
        std::vector<PitchCorrectionSegment>{{1, 2, {180.0f}}}, 160, 16000.0);
    const PitchReferenceSnapshot reference{{2.0, 1.0, 0.0, 1.0}, nullptr, curve, 2.0f};
    std::vector<float> effective;
    curve->forEachEffectiveF0Span(0, 10, reference.originalPitchRatio,
        [&](int, const float* data, int count, float gain) {
            for (int i = 0; i < count; ++i)
                effective.push_back(data[i] * gain);
        });
    auto next = reference;
    if (!(reference == next) || effective != std::vector<float>{200.0f, 180.0f, 0.0f, 240.0f})
        return false;

    next.projection.timelineStartSeconds += 1.0;
    if (reference == next)
        return false;
    next = reference;
    next.curve = std::make_shared<const PitchCurveSnapshot>(*curve);
    if (reference == next)
        return false;
    next = reference;
    next.originalPitchRatio = 1.0f;
    return !(reference == next);
}

auto makeCurve(std::vector<float> original, std::vector<PitchCorrectionSegment> segments = {}, int hopSize = 1)
{
    return std::make_shared<const PitchCurveSnapshot>(std::move(original), std::vector<float>{},
                                                      std::move(segments), hopSize, 4.0);
}

bool testReferenceFirstSelectionStillFocusesTheSelectedTarget()
{
    const auto grid = TimeGridSnapshot::makeIdentity(1.0);
    const PitchReferenceSnapshot earlier{{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({200, 200, 200, 200})};
    const PitchReferenceSnapshot target{{2.0, 1.0, 0.0, 1.0}, grid, makeCurve({200, 200, 200, 200})};
    const PitchReferenceSnapshot guide{{2.0, 1.0, 0.0, 1.0}, grid, makeCurve({225, 225, 225, 225})};
    const std::vector assigned{&earlier, &target};
    const auto scope = resolveEditorPlaybackScope(assigned, {&guide, &target});
    if (scope != std::vector{&target, &earlier}
        || resolveEditorPlaybackScope(assigned, {&guide}) != assigned
        || resolveEditorPlaybackScope<PitchReferenceSnapshot const>({}, {&target}) != std::vector{&target})
        return false;

    const auto plan = planPitchReferenceCorrection(*scope.front(), {guide}, {{0, 4}});
    if (!plan.hasVoicedOverlap || plan.segments.size() != 1)
        return false;

    auto corrected = target;
    corrected.curve = makeCurve({200, 200, 200, 200}, plan.segments);
    const auto repeated = planPitchReferenceCorrection(corrected, {guide}, {{0, 4}});
    return repeated.hasVoicedOverlap && repeated.segments.empty();
}

bool testTuneMatchesChromaticPitchAcrossTrimAndWarp()
{
    const auto targetGrid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 2.0, HandleKind::UserAdded},
        {3, 4.0, 4.0, HandleKind::ClipEnd},
    });
    const auto referenceGrid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 2.0, HandleKind::UserAdded},
        {3, 3.0, 3.0, HandleKind::ClipEnd},
    });
    const float cSharp4 = 277.18263f;
    const float eFlat4 = 311.12698f;
    const PitchReferenceSnapshot target{{10.0, 2.0, 1.0, 2.0}, targetGrid,
        makeCurve({260, 260, 260, 260, 260, 260, 260, 260}, {}, 2)};
    const std::vector<PitchReferenceSnapshot> references{
        {{10.0, 1.25, 1.0, 1.25}, referenceGrid,
         makeCurve({180, 170, cSharp4 / 2.0f, 210, 100, 190}, {{4, 5, {eFlat4}}}), 2.0f},
    };
    const auto plan = planPitchReferenceCorrection(target, references, {{0, 8}});
    return plan.hasVoicedOverlap && plan.segments.size() == 1
        && plan.segments[0].startFrame == 1 && plan.segments[0].endFrame == 3
        && plan.segments[0].f0Data == std::vector<float>{cSharp4, eFlat4}
        && target.curve->getCorrectionSegments().empty()
        && references[0].curve->getCorrectionSegments()[0].f0Data == std::vector<float>{eFlat4};
}

bool testTuneSkipsUnselectedAndUnvoicedFrames()
{
    const auto grid = TimeGridSnapshot::makeIdentity(2.0);
    const PitchReferenceSnapshot target{{0.0, 2.0, 0.0, 2.0}, grid,
        makeCurve({200, 200, 200, 200, 200, 200, 0, 200}, {{1, 7, {201, 202, 203, 204, 205, 206}}})};
    const std::vector<PitchReferenceSnapshot> references{
        {{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({300, 0, 302, 303, 999})},
        {{1.0, 1.0, 0.0, 1.0}, grid, makeCurve({304, 305, 306, 307})},
    };
    const auto plan = planPitchReferenceCorrection(target, references, {{0, 2}, {3, 8}});
    return plan.hasVoicedOverlap && plan.segments.size() == 3
        && plan.segments[0].startFrame == 0 && plan.segments[0].f0Data == std::vector<float>{300}
        && plan.segments[1].startFrame == 3 && plan.segments[1].f0Data == std::vector<float>{303, 304, 305}
        && plan.segments[2].startFrame == 7 && plan.segments[2].f0Data == std::vector<float>{307};
}

bool testTuneNoOpWithoutVoicedOverlapOrPitchChanges()
{
    const auto grid = TimeGridSnapshot::makeIdentity(1.0);
    const PitchReferenceSnapshot target{{1.0, 1.0, 0.0, 1.0}, grid, makeCurve({150, 150, 0, 150}), 2.0f};
    const PitchReferenceSnapshot adjacent{{2.0, 1.0, 0.0, 1.0}, grid, makeCurve({500, 500, 500, 500})};
    const PitchReferenceSnapshot matched{{1.0, 1.0, 0.0, 1.0}, grid, makeCurve({300, 0, 400, 300})};
    const PitchReferenceSnapshot unvoiced{{1.0, 1.0, 0.0, 1.0}, grid, makeCurve({0, 0, 0, 0})};
    const auto none = planPitchReferenceCorrection(target, {}, {{0, 4}});
    const auto noOverlap = planPitchReferenceCorrection(target, {adjacent}, {{0, 4}});
    const auto noVoicedOverlap = planPitchReferenceCorrection(target, {unvoiced}, {{0, 4}});
    const auto equal = planPitchReferenceCorrection(target, {matched}, {{0, 4}});
    return !none.hasClipOverlap && !none.hasVoicedOverlap && none.segments.empty()
        && !noOverlap.hasClipOverlap && !noOverlap.hasVoicedOverlap && noOverlap.segments.empty()
        && noVoicedOverlap.hasClipOverlap && !noVoicedOverlap.hasVoicedOverlap && noVoicedOverlap.segments.empty()
        && equal.hasClipOverlap && equal.hasVoicedOverlap && equal.segments.empty();
}
} // namespace

int main()
{
    if (!testTrimmedReferenceUsesWarpAndPlacement()) {
        std::fputs("FAIL: Reference source-to-timeline mapping\n", stderr);
        return 1;
    }
    if (!testEffectivePitchAndChangeDetection()) {
        std::fputs("FAIL: Reference effective pitch and snapshot changes\n", stderr);
        return 1;
    }
    if (!testTuneMatchesChromaticPitchAcrossTrimAndWarp()) {
        std::fputs("FAIL: Reference tuning preserves chromatic pitch through trim and warp\n", stderr);
        return 1;
    }
    if (!testTuneSkipsUnselectedAndUnvoicedFrames()) {
        std::fputs("FAIL: Reference tuning respects selection, voicing and region boundaries\n", stderr);
        return 1;
    }
    if (!testTuneNoOpWithoutVoicedOverlapOrPitchChanges()) {
        std::fputs("FAIL: Reference tuning does not edit without a different voiced target\n", stderr);
        return 1;
    }
    if (!testReferenceFirstSelectionStillFocusesTheSelectedTarget()) {
        std::fputs("FAIL: A reference selected first must not steal focus from the selected target\n", stderr);
        return 1;
    }
    std::puts("PitchReference tests passed.");
}
