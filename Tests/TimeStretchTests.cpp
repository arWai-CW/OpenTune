#include "Inference/SoundTouchStretcher.h"

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
}

int main()
{
    if (!testAnchorPlacementAndStereoPitch()) {
        std::fputs("Time stretch failed anchor, stereo or pitch preservation\n", stderr);
        return 1;
    }
    return 0;
}
