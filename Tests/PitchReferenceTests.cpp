#include "Content/PitchReferenceSnapshot.h"
#include "UI/PianoRoll/PianoRollTimeMap.h"

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
    std::puts("PitchReference tests passed.");
}
