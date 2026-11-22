#include "Inference/SoundTouchStretcher.h"
#include "DSP/TimingAlignment.h"

#include <cmath>
#include <cstdio>

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
    const auto& handles = tight.grid->handles();
    for (size_t i = 1; i < handles.size(); ++i)
        if (!TimeGridSnapshot::supportsStretchRatio(
            handles[i].source_seconds - handles[i - 1].source_seconds,
            handles[i].output_seconds - handles[i - 1].output_seconds))
            return false;
    return tight.grid->totalDurationSeconds() == 4.0
        && std::abs(tight.grid->tauForward(2.8) - target->tauForward(2.8)) < 1e-9
        && tight.grid->tauForward(3.5) == 3.48
        && !align(ambiguous, 1.0).grid
        && !align(guideOnsets, 0.0).grid;
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
    const auto grid = TimeGridSnapshot::makeIdentity(6.0);
    const ContentTimelineProjection placement{0.0, 6.0, 0.0, 6.0};
    const std::vector<TimingOnset> target{{0.5, true}, {1.0, false}, {1.8, true}, {4.1, false}};
    const std::vector<TimingOnset> guide{{0.6, true}, {1.1, false}, {1.9, true}, {4.2, false}};
    const auto result = alignTimingToReference(target, *grid, placement, guide, *grid, placement, 1.0);
    return result.grid && result.movedAnchors == 3
        && std::abs(result.grid->tauForward(4.1) - 4.1) < 1e-9;
}
}

int main()
{
    if (!testAnchorPlacementAndStereoPitch()) {
        std::fputs("Time stretch failed anchor, stereo or pitch preservation\n", stderr);
        return 1;
    }
    if (!testReferenceAlignment()) {
        std::fputs("Reference timing failed placement, strength, or preservation\n", stderr);
        return 1;
    }
    if (!testDetectAttacksAcrossPitch()) {
        std::fputs("Timing onset detection did not find synthetic attacks\n", stderr);
        return 1;
    }
    if (!testIsolatedAttackStaysPut()) {
        std::fputs("Reference timing moved an isolated attack\n", stderr);
        return 1;
    }
    return 0;
}
