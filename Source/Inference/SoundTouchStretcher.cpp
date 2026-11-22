#include "SoundTouchStretcher.h"

#include <SoundTouch.h>
#include <algorithm>
#include <vector>
#include "../Utils/TimeCoordinate.h"

namespace OpenTune {

SoundTouchStretcher::SoundTouchStretcher(double sampleRate, int channels)
    : impl_(std::make_unique<soundtouch::SoundTouch>()), sampleRate_(sampleRate), channels_(channels)
{
    impl_->setSampleRate(static_cast<unsigned int>(sampleRate));
    impl_->setChannels(static_cast<unsigned int>(channels));
    impl_->setRate(1.0);
    impl_->setPitch(1.0);
    impl_->setSetting(SETTING_USE_AA_FILTER, 1);
    impl_->setSetting(SETTING_AA_FILTER_LENGTH, 64);
    impl_->setSetting(SETTING_USE_QUICKSEEK, 0);
    impl_->setSetting(SETTING_SEEKWINDOW_MS, 25);
    impl_->setSetting(SETTING_OVERLAP_MS, 8);
}

SoundTouchStretcher::~SoundTouchStretcher() = default;

juce::AudioBuffer<float> SoundTouchStretcher::stretch(const juce::AudioBuffer<float>& input,
                                                    const TimeGridSnapshot& grid)
{
    if (grid.empty() || input.getNumChannels() != channels_)
        return {};

    const auto toSample = [this](double seconds) {
        return static_cast<int>(TimeCoordinate::secondsToSamplesNearest(seconds, sampleRate_));
    };
    const auto& handles = grid.handles();
    const int firstOutput = toSample(handles.front().output_seconds);
    juce::AudioBuffer<float> output(channels_, toSample(handles.back().output_seconds) - firstOutput);
    output.clear();

    constexpr int blockSize = 4096;
    std::vector<float> interleaved(static_cast<size_t>(blockSize) * channels_);
    std::vector<float> stretched(interleaved.size());

    for (size_t i = 1; i < handles.size(); ++i) {
        const int sourceStart = std::clamp(toSample(handles[i - 1].source_seconds), 0, input.getNumSamples());
        const int sourceEnd = std::clamp(toSample(handles[i].source_seconds), sourceStart, input.getNumSamples());
        const int outputStart = toSample(handles[i - 1].output_seconds) - firstOutput;
        const int outputFrames = toSample(handles[i].output_seconds) - firstOutput - outputStart;
        const int sourceFrames = sourceEnd - sourceStart;

        if (sourceFrames == 0 || outputFrames == 0)
            continue;

        if (sourceFrames == outputFrames) {
            for (int ch = 0; ch < channels_; ++ch)
                output.copyFrom(ch, outputStart, input, ch, sourceStart, sourceFrames);
            continue;
        }

        // Flush each span separately so WSOLA's timing error cannot accumulate.
        impl_->clear();
        impl_->setTempo(static_cast<double>(sourceFrames) / outputFrames);
        int written = 0;
        const auto drain = [&] {
            while (written < outputFrames) {
                const int got = static_cast<int>(impl_->receiveSamples(stretched.data(),
                    static_cast<unsigned int>(std::min(blockSize, outputFrames - written))));
                if (got == 0)
                    break;
                for (int ch = 0; ch < channels_; ++ch) {
                    auto* out = output.getWritePointer(ch, outputStart + written);
                    for (int n = 0; n < got; ++n)
                        out[n] = stretched[static_cast<size_t>(n) * channels_ + ch];
                }
                written += got;
            }
        };

        for (int pos = sourceStart; pos < sourceEnd;) {
            const int count = std::min(blockSize, sourceEnd - pos);
            for (int ch = 0; ch < channels_; ++ch) {
                const auto* in = input.getReadPointer(ch, pos);
                for (int n = 0; n < count; ++n)
                    interleaved[static_cast<size_t>(n) * channels_ + ch] = in[n];
            }
            impl_->putSamples(interleaved.data(), static_cast<unsigned int>(count));
            drain();
            pos += count;
        }
        impl_->flush();
        drain();

        // Keep the anchor's waveform at its exact target, then blend into WSOLA.
        const int blend = std::min({toSample(0.008), sourceFrames / 2, outputFrames / 2});
        for (int ch = 0; ch < channels_; ++ch) {
            const auto* in = input.getReadPointer(ch, sourceStart);
            auto* out = output.getWritePointer(ch, outputStart);
            for (int n = 0; n < blend; ++n) {
                const float mix = static_cast<float>(std::max(0, n - blend / 2))
                    / static_cast<float>(blend - blend / 2);
                out[n] = in[n] + mix * (out[n] - in[n]);
                const int end = outputFrames - 1 - n;
                out[end] = in[sourceFrames - 1 - n]
                    + mix * (out[end] - in[sourceFrames - 1 - n]);
            }
        }
    }

    return output;
}

} // namespace OpenTune
