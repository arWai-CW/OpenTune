#include "Content/PitchReferenceSnapshot.h"
#include "UI/PianoRoll/PianoRollTimeMap.h"
#include "Utils/EditorUiSync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
using namespace OpenTune;

bool nearPitch(float actual, float expected, double toleranceCents = 0.5)
{
    return actual > 0.0f && expected > 0.0f
        && std::abs(1200.0 * std::log2(static_cast<double>(actual) / expected)) <= toleranceCents;
}

bool segmentNearPitch(const PitchCorrectionSegment& segment, const std::vector<float>& expected,
                      double toleranceCents = 0.5)
{
    if (segment.f0Data.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!nearPitch(segment.f0Data[i], expected[i], toleranceCents))
            return false;
    }
    return true;
}

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

auto makeCurve(std::vector<float> original, std::vector<PitchCorrectionSegment> segments = {},
               int hopSize = 1, double sampleRate = 4.0)
{
    return std::make_shared<const PitchCurveSnapshot>(std::move(original), std::vector<float>{},
                                                      std::move(segments), hopSize, sampleRate);
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
    if (!plan.hasVoicedOverlap || plan.segments.size() != 1
        || !segmentNearPitch(plan.segments[0], {225, 225, 225, 225}, 0.1))
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
        && segmentNearPitch(plan.segments[0], {cSharp4, eFlat4}, 0.1)
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
        && plan.segments[0].startFrame == 0 && plan.segments[0].endFrame == 1
        && segmentNearPitch(plan.segments[0], {150}, 0.1)
        && plan.segments[1].startFrame == 3 && plan.segments[1].endFrame == 6
        && segmentNearPitch(plan.segments[1], {151.5f, 152.0f, 152.5f}, 0.1)
        && plan.segments[2].startFrame == 7 && plan.segments[2].endFrame == 8
        && segmentNearPitch(plan.segments[2], {153.5f}, 0.1);
}

bool testConstantTargetMovesPitchCenterWithoutChangingRegister()
{
    const auto grid = TimeGridSnapshot::makeIdentity(1.0);
    const PitchReferenceSnapshot target{{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({200, 200, 200, 200})};
    const PitchReferenceSnapshot reference{{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({220, 220, 220, 220})};
    const PitchReferenceSnapshot octaveReference{{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({410, 410, 410, 410})};
    const PitchReferenceSnapshot exactOctave{{0.0, 1.0, 0.0, 1.0}, grid, makeCurve({400, 400, 400, 400})};

    const auto musical = planPitchReferenceCorrection(target, {reference}, {{0, 4}});
    if (!musical.hasVoicedOverlap || musical.segments.size() != 1
        || !segmentNearPitch(musical.segments[0], {220, 220, 220, 220}, 0.1))
        return false;

    const auto octave = planPitchReferenceCorrection(target, {octaveReference}, {{0, 4}});
    if (!octave.hasVoicedOverlap || octave.segments.size() != 1
        || !segmentNearPitch(octave.segments[0], {205, 205, 205, 205}, 0.1))
        return false;

    const auto alreadyPitchClass = planPitchReferenceCorrection(target, {exactOctave}, {{0, 4}});
    return alreadyPitchClass.hasVoicedOverlap && alreadyPitchClass.segments.empty();
}

bool testLegatoNotesKeepIndependentReferenceOffsets()
{
    constexpr int shortNoteFrames = 15;
    constexpr int longNoteFrames = 50;
    constexpr int frameCount = shortNoteFrames + longNoteFrames;
    constexpr double frameSeconds = 0.01;
    constexpr double meanHz = 200.0;
    const float firstReferenceHz = static_cast<float>(meanHz * std::exp2(80.0 / 1200.0));
    const float secondReferenceHz = static_cast<float>(meanHz * std::exp2(-40.0 / 1200.0));

    std::vector<float> referenceF0(frameCount);
    for (int i = 0; i < frameCount; ++i)
        referenceF0[static_cast<size_t>(i)] = i < shortNoteFrames ? firstReferenceHz : secondReferenceHz;
    const double duration = frameCount * frameSeconds;
    const auto grid = TimeGridSnapshot::makeIdentity(duration);
    PitchReferenceSnapshot target{{0.0, duration, 0.0, duration}, grid,
        makeCurve(std::vector<float>(frameCount, static_cast<float>(meanHz)), {}, 1, 100.0)};
    target.notes = {
        {0.0, shortNoteFrames * frameSeconds, static_cast<float>(meanHz)},
        {shortNoteFrames * frameSeconds, duration, static_cast<float>(meanHz)},
    };
    const PitchReferenceSnapshot reference{{0.0, duration, 0.0, duration}, grid,
        makeCurve(std::move(referenceF0), {}, 1, 100.0)};

    const auto plan = planPitchReferenceCorrection(target, {reference}, {{0, frameCount}});
    if (!plan.hasVoicedOverlap || plan.segments.size() != 1
        || plan.segments[0].startFrame != 0 || plan.segments[0].endFrame != frameCount
        || plan.segments[0].f0Data.size() != static_cast<size_t>(frameCount))
        return false;

    const auto& corrected = plan.segments[0].f0Data;
    return nearPitch(corrected[2], firstReferenceHz, 2.0)
        && nearPitch(corrected[shortNoteFrames - 3], firstReferenceHz, 2.0)
        && nearPitch(corrected[shortNoteFrames + 5], secondReferenceHz, 2.0)
        && nearPitch(corrected[frameCount - 1], secondReferenceHz, 2.0);
}

bool vibratoRateIsPreserved(double vibratoRateHz)
{
    constexpr int frameCount = 200;
    constexpr double meanHz = 200.0;
    constexpr double referenceHz = 220.0;
    constexpr double vibratoCents = 30.0;
    constexpr double pi = 3.14159265358979323846;
    constexpr double frameSeconds = 0.01;

    std::vector<float> targetF0;
    targetF0.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const double phase = 2.0 * pi * vibratoRateHz * i * frameSeconds;
        targetF0.push_back(static_cast<float>(meanHz
            * std::exp2(vibratoCents * std::sin(phase) / 1200.0)));
    }
    const std::vector<float> referenceF0(frameCount, static_cast<float>(referenceHz));
    const double duration = frameCount * frameSeconds;
    const auto grid = TimeGridSnapshot::makeIdentity(duration);
    const PitchReferenceSnapshot target{{0.0, duration, 0.0, duration}, grid,
        makeCurve(std::move(targetF0), {}, 1, 100.0)};
    const PitchReferenceSnapshot reference{{0.0, duration, 0.0, duration}, grid,
        makeCurve(referenceF0, {}, 1, 100.0)};

    const auto plan = planPitchReferenceCorrection(target, {reference}, {{0, frameCount}});
    if (!plan.hasVoicedOverlap || plan.segments.size() != 1
        || plan.segments[0].startFrame != 0 || plan.segments[0].endFrame != frameCount
        || plan.segments[0].f0Data.size() != static_cast<size_t>(frameCount))
        return false;

    constexpr int begin = 40;
    constexpr int end = 160;
    double inputMean = 0.0;
    double outputMean = 0.0;
    const auto& targetOriginal = target.curve->getOriginalF0();
    const auto& corrected = plan.segments[0].f0Data;
    for (int i = begin; i < end; ++i) {
        inputMean += 1200.0 * std::log2(static_cast<double>(targetOriginal[i]) / meanHz);
        outputMean += 1200.0 * std::log2(static_cast<double>(corrected[i]) / referenceHz);
    }
    inputMean /= static_cast<double>(end - begin);
    outputMean /= static_cast<double>(end - begin);
    if (std::abs(outputMean) > 0.25)
        return false;

    double inputEnergy = 0.0;
    double correlation = 0.0;
    for (int i = begin; i < end; ++i) {
        const double inputDeviation = 1200.0 * std::log2(static_cast<double>(targetOriginal[i]) / meanHz) - inputMean;
        const double outputDeviation = 1200.0 * std::log2(static_cast<double>(corrected[i]) / referenceHz) - outputMean;
        inputEnergy += inputDeviation * inputDeviation;
        correlation += inputDeviation * outputDeviation;
    }
    if (inputEnergy <= 0.0)
        return false;
    const double amplitudeRatio = correlation / inputEnergy;
    return amplitudeRatio >= 0.95 && amplitudeRatio <= 1.05;
}

bool testVibratoIsPreservedWhileCenterFollowsReference()
{
    return vibratoRateIsPreserved(5.0) && vibratoRateIsPreserved(7.0);
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
    if (!testConstantTargetMovesPitchCenterWithoutChangingRegister()) {
        std::fputs("FAIL: Reference tuning moves the pitch center without changing register\n", stderr);
        return 1;
    }
    if (!testLegatoNotesKeepIndependentReferenceOffsets()) {
        std::fputs("FAIL: Reference tuning keeps legato note offsets independent\n", stderr);
        return 1;
    }
    if (!testVibratoIsPreservedWhileCenterFollowsReference()) {
        std::fputs("FAIL: Reference tuning preserves vibrato while moving the pitch center\n", stderr);
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
