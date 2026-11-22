#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "../Utils/TimeGrid.h"

namespace soundtouch { class SoundTouch; }

namespace OpenTune {

class SoundTouchStretcher {
public:
    SoundTouchStretcher(double sampleRate, int channels);
    ~SoundTouchStretcher();

    juce::AudioBuffer<float> stretch(const juce::AudioBuffer<float>& input,
                                    const TimeGridSnapshot& grid);

private:
    std::unique_ptr<soundtouch::SoundTouch> impl_;
    double sampleRate_;
    int channels_;
};

} // namespace OpenTune
