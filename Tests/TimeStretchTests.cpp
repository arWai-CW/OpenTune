#include "Inference/SoundTouchStretcher.h"
#include "Inference/RenderCache.h"
#include "Inference/TimeStretchCache.h"
#include "Reference/TimingAlignment.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <thread>

namespace {
using namespace OpenTune;

constexpr int sampleRate = 44100;
constexpr double pi = 3.14159265358979323846;

double estimatePitch(const juce::AudioBuffer<float>& audio, int channel, int start, int end)
{
    int first = -1;
    int last = -1;
    int crossings = 0;
    for (int i = start + 1; i < end; ++i) {
        if (audio.getSample(channel, i - 1) <= 0.0f && audio.getSample(channel, i) > 0.0f) {
            if (first < 0)
                first = i;
            last = i;
            ++crossings;
        }
    }
    return last > first ? (crossings - 1) * static_cast<double>(sampleRate) / (last - first) : 0.0;
}

bool testAnchorPlacementAndStereoPitch()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.7, 1.0, HandleKind::UserAdded},
        {3, 1.8, 1.6, HandleKind::UserAdded},
        {4, 2.4, 2.7, HandleKind::UserAdded},
        {5, 3.4, 3.4, HandleKind::ClipEnd}
    });
    if (!grid)
        return false;

    juce::AudioBuffer<float> input(2, sampleRate * 34 / 10);
    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < input.getNumSamples(); ++s)
            input.setSample(ch, s, 0.25f * static_cast<float>(
                std::sin(2.0 * pi * (220 + ch * 110) * s / sampleRate)));

    // Mark the explicit source anchors independently of the voiced tones.
    for (size_t i = 1; i + 1 < grid->handles().size(); ++i) {
        const int source = static_cast<int>(std::lround(grid->handles()[i].source_seconds * sampleRate));
        input.setSample(0, source, 0.95f);
        input.setSample(1, source, -0.85f);
    }

    SoundTouchStretcher stretcher(sampleRate, 2);
    const auto output = stretcher.stretch(input, *grid);
    if (output.getNumChannels() != 2 || output.getNumSamples() != input.getNumSamples())
        return false;

    for (size_t i = 1; i < grid->handles().size(); ++i) {
        const auto& handles = grid->handles();
        const int begin = static_cast<int>(std::lround(handles[i - 1].output_seconds * sampleRate));
        const int end = static_cast<int>(std::lround(handles[i].output_seconds * sampleRate));
        if (i > 1 && (output.getSample(0, begin) != 0.95f || output.getSample(1, begin) != -0.85f))
            return false;
        constexpr int edgeMargin = sampleRate / 10;
        for (int ch = 0; ch < 2; ++ch) {
            const double pitch = estimatePitch(output, ch, begin + edgeMargin, end - edgeMargin);
            if (std::abs(pitch - (220 + ch * 110)) > 1.0) {
                std::fprintf(stderr, "span %zu channel %d: measured %.2f Hz\n", i, ch, pitch);
                return false;
            }
        }
    }

    const auto identity = TimeGridSnapshot::makeIdentity(3.4);
    const auto unwarped = stretcher.stretch(input, *identity);
    return unwarped.getNumSamples() == input.getNumSamples()
        && unwarped.getSample(0, sampleRate) == input.getSample(0, sampleRate)
        && unwarped.getSample(1, sampleRate) == input.getSample(1, sampleRate);
}

bool testReferenceAlignment()
{
    const auto target = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 3.5, 3.48, HandleKind::UserAdded},
        {3, 4.0, 4.0, HandleKind::ClipEnd}
    });
    const auto guide = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 1.04, HandleKind::UserAdded},
        {3, 4.5, 4.5, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection targetPlacement{10.05, 4.0, 0, 4.0};
    const ContentTimelineProjection guidePlacement{9.8, 4.2, 0.3, 4.2};
    const std::vector<TimingOnset> targetOnsets{{0.5, false}, {1.1, true}, {1.97, false}, {2.8, true}};
    const std::vector<TimingOnset> guideOnsets{{1.10, false}, {1.68, true}, {2.56, false}};
    const auto align = [&](const auto& guides, double strength) {
        return alignTimingToReference(targetOnsets, *target, targetPlacement,
                                      guides, *guide, guidePlacement, strength);
    };
    const auto tight = align(guideOnsets, 1.0);
    const auto gentle = align(guideOnsets, 0.5);
    if (!tight.grid || !gentle.grid || tight.movedAnchors != 3 || gentle.movedAnchors != 3)
        return false;
    for (size_t i = 0; i < guideOnsets.size(); ++i) {
        const double source = targetOnsets[i].sourceSeconds;
        const double expected = targetPlacement.projectTimelineTimeToContent(
            guidePlacement.projectContentTimeToTimeline(guide->tauForward(guideOnsets[i].sourceSeconds)));
        if (std::abs(tight.grid->tauForward(source) - expected) > 1e-9
            || std::abs(gentle.grid->tauForward(source)
                - (target->tauForward(source) + expected) * 0.5) > 1e-9)
            return false;
    }
    const std::vector<TimingOnset> ambiguous{{0.96, false}, {1.052, false}, {1.68, true}, {2.56, false}};
    const auto repeated = alignTimingToReference(targetOnsets, *tight.grid, targetPlacement,
                                                  guideOnsets, *guide, guidePlacement, 1.0);
    const auto unreliable = align(ambiguous, 1.0);
    const auto& handles = tight.grid->handles();
    for (size_t i = 1; i < handles.size(); ++i)
        if (!TimeGridSnapshot::supportsStretchRatio(
            handles[i].source_seconds - handles[i - 1].source_seconds,
            handles[i].output_seconds - handles[i - 1].output_seconds))
            return false;
    return tight.grid->totalDurationSeconds() == 4.0
        && std::abs(tight.grid->tauForward(2.8) - target->tauForward(2.8)) < 1e-9
        && tight.grid->tauForward(3.5) == 3.48
        && repeated.alreadyAligned && !repeated.grid
        && !unreliable.alreadyAligned && !unreliable.grid
        && !align(guideOnsets, 0.0).grid;
}

bool testPooledSplitsCanAlignIndependently()
{
    const auto base = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 2.4, 2.4, HandleKind::InternalOnset},
        {3, 2.95, 2.95, HandleKind::InternalOnset},
        {4, 3.5, 3.5, HandleKind::InternalOnset},
        {5, 6.0, 6.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection lead{10.0, 6.0, 0.0, 6.0};
    const ContentTimelineProjection first{10.0, 2.0, 0.0, 2.0};
    const ContentTimelineProjection second{13.0, 2.0, 2.0, 2.0};
    const std::vector<TimingOnset> firstAttacks{{0.4, true}, {0.95, false}, {1.5, true}};
    const std::vector<TimingOnset> firstReference{{0.46, true}, {1.01, false}, {1.56, true}};
    const std::vector<TimingOnset> secondAttacks{{2.4, false}, {2.95, true}, {3.5, false}};
    const std::vector<TimingOnset> secondReference{{3.34, false}, {3.89, true}, {4.44, false}};

    const auto firstPlan = alignTimingToReference(firstAttacks, *base, first,
        firstReference, *base, lead, 1.0, {{2.0, 4.0}, {4.0, 6.0}});
    const auto copyConflict = alignTimingToReference(firstAttacks, *base, first,
        firstReference, *base, lead, 1.0, {{0.0, 2.0}});
    if (!firstPlan.grid || copyConflict.grid || !copyConflict.affectsProtectedTiming)
        return false;
    const auto alignSecond = [&](const TimeGridSnapshot& current) {
        return alignTimingToReference(secondAttacks, current, second,
            secondReference, *base, lead, 0.5, {{0.0, 2.0}, {4.0, 6.0}});
    };
    const auto preview = alignSecond(*base);
    const auto combined = alignSecond(*firstPlan.grid);
    if (!preview.grid || !combined.grid
        || !preservesTimingAudioInRange(*preview.grid, *combined.grid, {2.0, 4.0})
        || std::abs(combined.grid->tauForward(0.4) - 0.46) > 1e-9
        || std::abs(combined.grid->tauForward(2.4) - 2.37) > 1e-9)
        return false;

    juce::AudioBuffer<float> input(1, sampleRate * 6);
    for (int s = 0; s < input.getNumSamples(); ++s)
        input.setSample(0, s, static_cast<float>(std::sin(2.0 * pi * 221 * s / sampleRate)));
    SoundTouchStretcher stretcher(sampleRate, 1);
    const auto firstAudio = stretcher.stretch(input, *firstPlan.grid);
    const auto combinedAudio = stretcher.stretch(input, *combined.grid);
    const auto restored = restoreOriginalTimingInRange(*combined.grid, {0.0, 2.0}, {{2.0, 4.0}});
    if (!restored || !restored->isIdentityInOutputRange(0.0, 2.0))
        return false;
    const auto restoredAudio = stretcher.stretch(input, *restored);
    for (int s = 0; s < input.getNumSamples(); ++s) {
        if (s >= 2 * sampleRate && firstAudio.getSample(0, s) != input.getSample(0, s))
            return false;
        if ((s < 2 * sampleRate || s >= 4 * sampleRate)
            && combinedAudio.getSample(0, s) != firstAudio.getSample(0, s))
            return false;
        if (restoredAudio.getSample(0, s) != (s < 2 * sampleRate
            ? input.getSample(0, s) : combinedAudio.getSample(0, s)))
            return false;
    }
    return true;
}

bool testSameTimeMapCanStillChangeStretchedSiblingAudio()
{
    const auto before = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 1.2, HandleKind::UserAdded},
        {3, 3.0, 3.0, HandleKind::ClipEnd}
    });
    auto handles = before->handles();
    handles.insert(handles.begin() + 2, {4, 2.0, before->tauForward(2.0), HandleKind::InternalOnset});
    const auto after = TimeGridSnapshot::makeFromHandles(std::move(handles));
    return after && preservesTimingAudioInRange(*before, *after, {0.0, 1.2})
        && !preservesTimingAudioInRange(*before, *after, {1.2, 2.1});
}

bool testRestoreOriginalClipTiming()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.5, 0.6, HandleKind::OnsetVoiced},
        {3, 1.0, 1.0, HandleKind::InternalOnset},
        {4, 1.5, 1.4, HandleKind::UserAdded},
        {5, 2.0, 2.0, HandleKind::InternalOnset},
        {6, 3.0, 3.0, HandleKind::ClipEnd}
    });
    const auto restored = restoreOriginalTimingInRange(*grid, {0.0, 1.0}, {{1.0, 2.0}});
    if (!restored || restored->tauForward(0.5) != 0.5 || restored->tauForward(1.5) != 1.4
        || !preservesTimingAudioInRange(*grid, *restored, {1.0, 3.0})
        || restoreOriginalTimingInRange(*grid, {0.0, 1.0}, {{0.25, 0.75}})
        || restoreOriginalTimingInRange(*grid, {0.0, 0.8})
        || restoreOriginalTimingInRange(*grid, {2.0, 3.0}))
        return false;
    const auto whole = restoreOriginalTimingInRange(*grid, {-0.05, 3.05});
    return whole && whole->isIdentity() && whole->totalDurationSeconds() == 3.0;
}

bool testDetectAttacksAcrossPitch()
{
    constexpr int rate = 16000;
    const std::vector<double> attacks{0.35, 0.82, 1.44};
    for (const int pitch : {180, 320}) {
        juce::AudioBuffer<float> audio(2, 2 * rate);
        audio.clear();
        for (const double onset : attacks) {
            for (int i = 0; i < rate / 8; ++i) {
                const float value = static_cast<float>(std::min(1.0, i / (rate * 0.01))
                    * std::sin(2.0 * pi * pitch * i / rate));
                const int sample = static_cast<int>(onset * rate) + i;
                audio.setSample(0, sample, value);
                audio.setSample(1, sample, -value);
            }
        }
        const auto onsets = detectTimingOnsets(audio, rate);
        if (onsets.size() != attacks.size())
            return false;
        for (size_t i = 0; i < attacks.size(); ++i)
            if (std::abs(onsets[i].sourceSeconds - attacks[i]) > 0.01)
                return false;
    }
    return true;
}

bool testIsolatedAttackStaysPut()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.5, 0.5, HandleKind::InternalOnset},
        {3, 1.0, 1.0, HandleKind::InternalOnset},
        {4, 1.8, 1.8, HandleKind::InternalOnset},
        {5, 5.0, 5.0, HandleKind::UserAdded},
        {6, 6.0, 6.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection placement{0.0, 6.0, 0.0, 6.0};
    const std::vector<TimingOnset> target{{0.5, true}, {1.0, false}, {1.8, true}, {4.1, false}};
    const std::vector<TimingOnset> guide{{0.6, true}, {1.1, false}, {1.9, true}, {4.2, false}};
    const auto result = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0);
    return result.grid && result.movedAnchors == 3
        && std::abs(result.grid->tauForward(4.1) - 4.1) < 1e-9
        && std::any_of(result.grid->handles().begin(), result.grid->handles().end(), [](const auto& handle) {
            return handle.id == 5 && handle.output_seconds == 5.0;
        });
}

bool testLoudAudioOutsideOverlapDoesNotHideAttacks()
{
    static constexpr int rate = 16000;
    juce::AudioBuffer<float> targetAudio(1, 5 * rate), guideAudio(1, 5 * rate);
    targetAudio.clear();
    guideAudio.clear();
    const auto pulse = [](auto& audio, double time, double amplitude) {
        for (int i = 0; i < rate / 8; ++i)
            audio.setSample(0, static_cast<int>(std::lround(time * rate)) + i,
                static_cast<float>(amplitude * std::min(1.0, i / (rate * 0.01))
                    * std::sin(2.0 * pi * 220.0 * i / rate)));
    };

    const auto grid = TimeGridSnapshot::makeIdentity(5.0);
    const auto guideGrid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 1.0, 2.0, HandleKind::UserAdded},
        {3, 5.0, 5.0, HandleKind::ClipEnd}
    });
    if (!guideGrid)
        return false;

    // The guide starts inside its source, and its existing timing grid maps that
    // cropped region to a different source range.
    const ContentTimelineProjection targetPlacement{10.0, 5.0, 0.0, 5.0};
    const ContentTimelineProjection guidePlacement{11.0, 3.0, 2.0, 3.0};
    pulse(targetAudio, 0.35, 1.0);
    pulse(guideAudio, 0.35, 1.0);
    for (const double time : {11.35, 11.8, 12.45, 13.05}) {
        pulse(targetAudio, targetPlacement.projectTimelineTimeToContent(time) + 0.04, 0.12);
        pulse(guideAudio, guideGrid->tauInverse(guidePlacement.projectTimelineTimeToContent(time)), 0.12);
    }

    const auto targets = detectTimingOnsetsInOverlap(targetAudio, rate, *grid, targetPlacement, guidePlacement);
    const auto guides = detectTimingOnsetsInOverlap(guideAudio, rate, *guideGrid, guidePlacement, targetPlacement);
    const auto plan = alignTimingToReference(targets, *grid, targetPlacement, guides, *guideGrid, guidePlacement, 1.0);
    return targets.size() == 4 && guides.size() == 4 && plan.grid && plan.movedAnchors == 4
        && std::abs(plan.grid->tauForward(0.35) - 0.35) < 1e-9;
}

bool testTimingRejectionReportsStageAndCounts()
{
    const auto grid = TimeGridSnapshot::makeIdentity(3.0);
    const ContentTimelineProjection placement{0.0, 3.0, 0.0, 3.0};
    const std::vector<TimingOnset> targets{{0.4, true}, {1.0, false}, {1.6, true}};
    const std::vector<TimingOnset> guides{{0.44, true}, {1.04, false}, {2.1, true}};
    const auto plan = [&](const std::vector<TimingOnset>& guide) {
        return alignTimingToReference(targets, *grid, placement, guide, *grid, placement, 1.0);
    };
    const auto noAttacks = plan({});
    const auto notEnoughMatches = plan(guides);
    return !noAttacks.grid && noAttacks.rejection
        && std::string_view(noAttacks.rejection).find("detected attacks") != std::string_view::npos
        && noAttacks.targetAttacks == 3 && noAttacks.referenceAttacks == 0 && noAttacks.matchedAttacks == 0
        && !notEnoughMatches.grid && notEnoughMatches.rejection
        && std::string_view(notEnoughMatches.rejection).find("reliable matches") != std::string_view::npos
        && notEnoughMatches.targetAttacks == 3 && notEnoughMatches.referenceAttacks == 3
        && notEnoughMatches.matchedAttacks == 2;
}

bool testLastFramesOfOverlapCanProvideTheThirdMatch()
{
    static constexpr int rate = 16000;
    juce::AudioBuffer<float> targetAudio(1, 3 * rate), guideAudio(1, 3 * rate);
    targetAudio.clear();
    guideAudio.clear();
    const auto attack = [](auto& audio, double time) {
        const int start = static_cast<int>(std::lround(time * rate));
        for (int i = 0; i < rate / 8; ++i)
            audio.setSample(0, start + i, static_cast<float>(std::sin(2.0 * pi * 200.0 * i / rate)));
    };
    for (const double time : {0.49, 1.14, 1.99}) {
        attack(guideAudio, time);
        attack(targetAudio, time - 0.165);
    }
    const auto grid = TimeGridSnapshot::makeIdentity(3.0);
    const ContentTimelineProjection targetPlacement{0.0, 3.0, 0.0, 3.0};
    const ContentTimelineProjection guidePlacement{0.0, 2.0, 0.0, 2.0};
    const auto targets = detectTimingOnsetsInOverlap(targetAudio, rate, *grid, targetPlacement, guidePlacement);
    const auto guides = detectTimingOnsetsInOverlap(guideAudio, rate, *grid, guidePlacement, targetPlacement);
    const auto plan = alignTimingToReference(targets, *grid, targetPlacement, guides, *grid, guidePlacement, 0.5);
    return guides.size() == 3 && guides.back().sourceSeconds >= 1.985
        && guides.back().sourceSeconds < 2.0 && plan.grid && plan.matchedAttacks == 3;
}

bool testPeriodicAttacksDoNotSlipByASyllable()
{
    const auto grid = TimeGridSnapshot::makeIdentity(3.0);
    const ContentTimelineProjection placement{0.0, 3.0, 0.0, 3.0};
    for (const auto& [spacing, delay] : {std::pair{0.24, 0.13}, std::pair{0.18, 0.125}, std::pair{0.18, 0.162}}) {
        std::vector<TimingOnset> target, guide;
        for (int i = 0; i < 7; ++i) {
            const auto guideTime = 0.4 + i * spacing;
            guide.push_back({guideTime, false});
            target.push_back({guideTime + delay, false});
        }
        if (alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0).grid)
            return false;
    }
    return true;
}

bool testDenseAttacksPreferClearlyCloserSyllables()
{
    const auto grid = TimeGridSnapshot::makeIdentity(3.0);
    const ContentTimelineProjection placement{0.0, 3.0, 0.0, 3.0};
    for (const double delay : {0.025, 0.040}) {
        std::vector<TimingOnset> target, guide;
        for (int i = 0; i < 7; ++i) {
            const auto guideTime = 0.4 + i * 0.18;
            guide.push_back({guideTime, false});
            target.push_back({guideTime + delay, false});
        }
        const auto result = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0);
        if (!result.grid || result.movedAnchors != static_cast<int>(target.size())
            || std::abs(result.grid->tauForward(target[3].sourceSeconds) - guide[3].sourceSeconds) >= 1e-9)
            return false;
    }
    return true;
}

bool testExtraLeadingAttackDoesNotHidePhaseAmbiguity()
{
    const auto grid = TimeGridSnapshot::makeIdentity(2.5);
    const ContentTimelineProjection placement{0.0, 2.5, 0.0, 2.5};
    std::vector<TimingOnset> target{{0.345, false}}, guide;
    for (int i = 0; i < 7; ++i) {
        const double onset = 0.4 + i * 0.18;
        guide.push_back({onset, false});
        if (i < 6)
            target.push_back({onset + 0.125, false});
    }
    return !alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0).grid;
}

bool testBlockedAnchorDoesNotDriftAway()
{
    const auto grid = TimeGridSnapshot::makeIdentity(2.5);
    const ContentTimelineProjection placement{0.0, 2.5, 0.0, 2.5};
    const std::vector<TimingOnset> target{{0.50, true}, {0.63, false}, {1.25, true}, {1.85, false}};
    const std::vector<TimingOnset> guide{{0.46, true}, {0.69, false}, {1.25, true}, {1.85, false}};
    return !alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0).grid;
}

bool testBlockedAttackDoesNotDiscardSafeEdits()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.35, 0.35, HandleKind::UserAdded},
        {3, 0.95, 0.95, HandleKind::UserAdded},
        {4, 3.0, 3.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection placement{0.0, 3.0, 0.0, 3.0};
    const std::vector<TimingOnset> target{{0.60, true}, {0.73, false},
        {1.35, true}, {1.90, false}, {2.45, true}};
    const std::vector<TimingOnset> guide{{0.552, true}, {0.763, false},
        {1.39, true}, {1.94, false}, {2.49, true}};
    for (const double strength : {0.5, 1.0}) {
        const auto plan = alignTimingToReference(target, *grid, placement, guide, *grid, placement, strength);
        if (!plan.grid) {
            std::fprintf(stderr, "Safe attacks discarded: %s\n", plan.rejection);
            return false;
        }
        if (plan.matchedAttacks != 5 || plan.movedAnchors != 3
            || std::abs(plan.maxMoveSeconds - 0.04 * strength) > 1e-9)
            return false;
        for (size_t i = 0; i < target.size(); ++i) {
            const auto before = target[i].sourceSeconds;
            const auto expected = i < 2 ? before : before + 0.04 * strength;
            if (std::abs(plan.grid->tauForward(before) - expected) > 1e-9)
                return false;
        }
    }
    return true;
}

bool testBlockedAttackKeepsSafeNeighbor()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.34, 0.34, HandleKind::UserAdded},
        {3, 2.0, 2.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection placement{0.0, 2.0, 0.0, 2.0};
    const std::vector<TimingOnset> target{{0.50, true}, {0.63, false}, {0.90, true}};
    const std::vector<TimingOnset> guide{{0.56, true}, {0.643, false}, {0.915, true}};
    const auto plan = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0);
    return plan.grid && plan.movedAnchors == 1 && plan.matchedAttacks == 3
        && std::abs(plan.grid->tauForward(0.90) - 0.915) < 1e-9
        && std::abs(plan.grid->tauForward(0.63) - 0.643) < 0.013;
}

bool testProtectedSiblingDoesNotDiscardIndependentMove()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.34, 0.34, HandleKind::UserAdded},
        {3, 1.17, 1.17, HandleKind::UserAdded},
        {4, 2.0, 2.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection placement{0.0, 2.0, 0.0, 2.0};
    const std::vector<TimingOnset> target{{0.50, true}, {0.63, false}, {0.90, true}, {1.50, false}};
    const std::vector<TimingOnset> guide{{0.56, true}, {0.643, false}, {0.915, true}, {1.54, false}};
    const TimingOutputRange sibling{1.0, 1.17};
    const auto plan = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0, {sibling});
    return plan.grid && plan.movedAnchors == 1 && plan.matchedAttacks == 4
        && std::abs(plan.grid->tauForward(1.50) - 1.54) < 1e-9
        && preservesTimingAudioInRange(*grid, *plan.grid, sibling);
}

bool testUnpoolHintRequiresSafeUnpooledMoves()
{
    const auto grid = TimeGridSnapshot::makeFromHandles({
        {1, 0.0, 0.0, HandleKind::ClipStart},
        {2, 0.35, 0.35, HandleKind::UserAdded},
        {3, 0.95, 0.95, HandleKind::UserAdded},
        {4, 2.0, 2.0, HandleKind::ClipEnd}
    });
    const ContentTimelineProjection placement{0.0, 2.0, 0.0, 2.0};
    const std::vector<TimingOnset> target{{0.60, true}, {0.73, false}, {1.35, true}};
    const std::vector<TimingOnset> guide{{0.552, true}, {0.763, false}, {1.35, true}};
    const auto unpooled = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0);
    const auto pooled = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0, {{0.4, 0.5}});
    return !unpooled.grid && !pooled.grid && pooled.matchedAttacks == 3 && !pooled.affectsProtectedTiming
        && juce::String(pooled.rejection) == "stretch would worsen a reliable match";
}
}

namespace {
bool testStereoRenderCache()
{
    using OpenTune::RenderCache;
    using OpenTune::PlaybackSampleGeometry::projectToSample;
    constexpr int frames = 128;
    constexpr int origin = 64;
    constexpr int nativeFrames = 384;
    constexpr double targetRate = 48000.0;
    constexpr double sourceRate = RenderCache::kSampleRate;
    auto native = std::make_shared<juce::AudioBuffer<float>>(2, nativeFrames);
    auto canonical = std::make_shared<juce::AudioBuffer<float>>(2, 2 * frames);
    for (int channel = 0; channel < 2; ++channel) {
        std::fill_n(native->getWritePointer(channel), nativeFrames, channel == 0 ? 0.1f : 0.4f);
        canonical->copyFrom(channel, 0, *native, channel, origin, 2 * frames);
    }

    RenderCache cache;
    cache.setCanonicalSource(canonical, origin, {native, sourceRate, 0});
    if (!cache.prepareForPlaybackSampleRate(sourceRate))
        return false;
    cache.reconcileFullPlanAndRequest({{0, frames}, {frames, 2 * frames}}, {{0, 2 * frames}}, 1);
    const auto jobs = cache.getPendingJobs();
    if (jobs.size() != 2 || !cache.claimPendingJob(jobs[0]) || !cache.claimPendingJob(jobs[1]))
        return false;
    std::vector<float> stereo(2 * frames, 0.25f);
    std::fill_n(stereo.begin() + frames, frames, -0.75f);
    if (cache.completeChunkRenderWithAudio(0, frames, std::move(stereo), jobs[0].targetRevision, 2)
            != RenderCache::ChunkRenderResult::Published
        || cache.completeChunkRenderWithAudio(frames, 2 * frames,
               std::vector<float>(frames, 0.5f), jobs[1].targetRevision)
            != RenderCache::ChunkRenderResult::Published)
        return false;

    juce::AudioBuffer<float> output(2, 2 * frames);
    for (bool prepared : {false, true}) {
        output.clear();
        if (prepared)
            cache.overlayPreparedAudio(output, 0, 2 * frames, origin, sourceRate, 1);
        else
            cache.overlayCanonicalAudio(output, 0, 2 * frames, 0);
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < 2 * frames; ++frame)
                if (output.getSample(channel, frame) != (frame < frames ? (channel == 0 ? 0.25f : -0.75f) : 0.5f))
                    return false;
    }

    if (!cache.prepareForPlaybackSampleRate(targetRate))
        return false;
    const auto start = projectToSample(origin * targetRate / sourceRate);
    const int count = static_cast<int>(projectToSample((origin + 2 * frames) * targetRate / sourceRate) - start);
    output.setSize(2, count);
    output.clear();
    cache.overlayPreparedAudio(output, 0, count, start, targetRate, 1);
    OpenTune::ResamplingManager resampler;
    for (int channel = 0; channel < 2; ++channel) {
        // Both sides of the mixed-channel boundary must match continuous resampling.
        const auto* dry = native->getReadPointer(channel);
        std::vector<float> continuous(dry, dry + nativeFrames);
        std::fill_n(continuous.begin() + origin, frames, channel == 0 ? 0.25f : -0.75f);
        std::fill_n(continuous.begin() + origin + frames, frames, 0.5f);
        const auto expected = resampler.resampleRange(continuous.data(), continuous.size(),
            sourceRate, targetRate, 0, start, count);
        for (int frame = 0; frame < count; ++frame)
            if (!(std::abs(output.getSample(channel, frame) - expected[frame]) <= 1e-5f))
                return false;
    }
    return true;
}

bool testClearRejectsOldTimeStretchBuild()
{
    OpenTune::TimeStretchCache cache;
    const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2401, 0};
    const uint32_t oldGeneration = cache.beginBuild(key);
    cache.clear();
    const uint32_t newGeneration = cache.beginBuild(key);

    cache.store(key, {1.0f, 1.0f, 1.0f, 1.0f}, 1, 1, 44100.0, oldGeneration);
    juce::AudioBuffer<float> destination(1, 4);
    if (cache.sliceCanonicalForOutputRange(key, 1, 1, 0, destination, 0, 4) != 0)
        return false;

    cache.store(key, {2.0f, 2.0f, 2.0f, 2.0f}, 2, 1, 44100.0, newGeneration);
    destination.clear();
    return cache.sliceCanonicalForOutputRange(key, 2, 1, 0, destination, 0, 4) == 4
        && destination.getSample(0, 0) == 2.0f
        && destination.getSample(0, 3) == 2.0f;
}

bool testConcurrentTimeStretchPublicationAndClear()
{
    const auto baseline = OpenTune::RenderCache::globalCacheCurrentBytes().load();
    bool intact = true;
    {
        OpenTune::TimeStretchCache cache;
        cache.prepareForPlaybackSampleRate(44100.0);
        const OpenTune::ContentKey key{OpenTune::DomainKind::StandaloneClip, 2402, 0};
        std::atomic<bool> ready{false};
        std::atomic<bool> done{false};
        std::thread audio([&] {
            juce::AudioBuffer<float> destination(2, 256);
            ready.store(true, std::memory_order_release);
            do
            {
                const int copied = cache.sliceForOutputRange(
                    key, 1, 1, 0, destination, 0, 256, 44100.0);
                if (copied != 0 && copied != 256)
                    intact = false;
                if (copied == 256)
                {
                    const float value = destination.getSample(0, 0);
                    intact = intact && (value == 0.25f || value == 0.75f);
                    for (int channel = 0; channel < 2; ++channel)
                        for (int sample = 0; sample < 256; ++sample)
                            intact = intact && destination.getSample(channel, sample) == value;
                }
            }
            while (!done.load(std::memory_order_acquire));
        });
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int publication = 0; publication < 2048; ++publication)
        {
            if (publication % 8 == 0)
                cache.clear();
            const auto generation = cache.beginBuild(key);
            cache.store(key, std::vector<float>(256, publication % 2 == 0 ? 0.25f : 0.75f),
                        1, 1, 44100.0, generation);
        }
        done.store(true, std::memory_order_release);
        audio.join();
        cache.collectPlaybackSnapshots();
        juce::AudioBuffer<float> finalOutput(1, 256);
        intact = intact && cache.sliceForOutputRange(
            key, 1, 1, 0, finalOutput, 0, 256, 44100.0) == 256;
        intact = intact && finalOutput.getSample(0, 255) == 0.75f;
    }
    return intact && OpenTune::RenderCache::globalCacheCurrentBytes().load() == baseline;
}

bool testTrimmedStereoTimeStretchRead()
{
    using namespace OpenTune;
    const ContentKey key{DomainKind::ARAAudioModification, 4721, 0};
    TimeStretchCache cache;
    cache.prepareForPlaybackSampleRate(48000.0);
    juce::AudioBuffer<float> input(2, 4410);
    input.clear();
    for (int i = 0; i < input.getNumSamples(); ++i) {
        input.setSample(0, i, 0.25f);
        input.setSample(1, i, -0.5f);
    }
    cache.storeBuffer(key, std::move(input), 17, 2, 44100.0, cache.beginBuild(key));

    juce::AudioBuffer<float> playback(2, 128);
    return cache.sliceForOutputRange(key, 17, 1, 1200, playback, 0, 128, 48000.0) == 0
        && cache.sliceForOutputRange(key, 17, 2, 1200, playback, 0, 128, 48000.0) == 128
        && std::abs(playback.getSample(0, 64) - 0.25f) < 0.001f
        && std::abs(playback.getSample(1, 64) + 0.5f) < 0.001f;
}
} // namespace

int main()
{
    if (!testStereoRenderCache()) {
        std::fputs("FAIL: canonical and prepared render cache preserve stereo planes\n", stderr);
        return 1;
    }
    if (!testClearRejectsOldTimeStretchBuild()) {
        std::fputs("FAIL: clear rejects an old TimeStretchCache build\n", stderr);
        return 1;
    }
    if (!testConcurrentTimeStretchPublicationAndClear()) {
        std::fputs("FAIL: concurrent time-stretch reads survive publication and clear\n", stderr);
        return 1;
    }
    if (!testTrimmedStereoTimeStretchRead()) {
        std::fputs("FAIL: time-stretch reads preserve cropped stereo planes\n", stderr);
        return 1;
    }
    if (!testAnchorPlacementAndStereoPitch()) {
        std::fputs("Time stretch failed anchor, stereo or pitch preservation\n", stderr);
        return 1;
    }
    if (!testReferenceAlignment()) {
        std::fputs("Reference timing failed placement, strength, or preservation\n", stderr);
        return 1;
    }
    if (!testBlockedAttackDoesNotDiscardSafeEdits()) {
        std::fputs("A blocked attack discarded independent safe timing edits\n", stderr);
        return 1;
    }
    if (!testBlockedAttackKeepsSafeNeighbor()) {
        std::fputs("A blocked attack discarded a safe neighboring timing move\n", stderr);
        return 1;
    }
    if (!testProtectedSiblingDoesNotDiscardIndependentMove()) {
        std::fputs("A move affecting a pooled sibling discarded an independent timing move\n", stderr);
        return 1;
    }
    if (!testUnpoolHintRequiresSafeUnpooledMoves()) {
        std::fputs("Timing alignment suggested unpooling even though an independent clip cannot align\n", stderr);
        return 1;
    }
    if (!testPooledSplitsCanAlignIndependently() || !testSameTimeMapCanStillChangeStretchedSiblingAudio()) {
        std::fputs("Pooled timing edits failed to preserve sibling splits\n", stderr);
        return 1;
    }
    if (!testRestoreOriginalClipTiming()) {
        std::fputs("Restoring clip timing failed or affected audio outside the clip\n", stderr);
        return 1;
    }
    if (!testDetectAttacksAcrossPitch()) {
        std::fputs("Timing onset detection did not find synthetic attacks\n", stderr);
        return 1;
    }
    if (!testLoudAudioOutsideOverlapDoesNotHideAttacks()) {
        std::fputs("Loud audio outside the overlap hid matching timing attacks\n", stderr);
        return 1;
    }
    if (!testTimingRejectionReportsStageAndCounts()) {
        std::fputs("Timing alignment did not report the rejection stage and counts\n", stderr);
        return 1;
    }
    if (!testLastFramesOfOverlapCanProvideTheThirdMatch()) {
        std::fputs("Timing alignment missed an attack in the last frames of the overlap\n", stderr);
        return 1;
    }
    if (!testIsolatedAttackStaysPut()) {
        std::fputs("Reference timing did not align a seeded grid while protecting isolated and manual anchors\n", stderr);
        return 1;
    }
    if (!testPeriodicAttacksDoNotSlipByASyllable()) {
        std::fputs("Reference timing incorrectly paired periodic attacks with the next syllable\n", stderr);
        return 1;
    }
    if (!testDenseAttacksPreferClearlyCloserSyllables()) {
        std::fputs("Reference timing rejected clearly closer attacks in a dense phrase\n", stderr);
        return 1;
    }
    if (!testExtraLeadingAttackDoesNotHidePhaseAmbiguity()) {
        std::fputs("An extra leading attack hid a competing timing phase\n", stderr);
        return 1;
    }
    if (!testBlockedAnchorDoesNotDriftAway()) {
        std::fputs("Reference timing moved an unanchored attack away from its guide\n", stderr);
        return 1;
    }
    return 0;
}
