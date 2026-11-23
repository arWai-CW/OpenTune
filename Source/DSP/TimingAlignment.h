#pragma once

#include "../Utils/ContentTimelineProjection.h"
#include "../Utils/TimeGrid.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace OpenTune {

struct TimingOnset
{
    double sourceSeconds;
    bool highFrequencyDominant;
};

struct TimingAlignment
{
    std::shared_ptr<const TimeGridSnapshot> grid;
    int movedAnchors = 0;
    double maxMoveSeconds = 0.0;
    bool alreadyAligned = false;
};

std::vector<TimingOnset> detectTimingOnsets(const juce::AudioBuffer<float>& audio, double sampleRate);

std::vector<TimingOnset> detectTimingOnsetsInOverlap(const juce::AudioBuffer<float>& audio,
                                                   double sampleRate,
                                                   const TimeGridSnapshot& grid,
                                                   const ContentTimelineProjection& placement,
                                                   const ContentTimelineProjection& otherPlacement);

TimingAlignment alignTimingToReference(const std::vector<TimingOnset>& target,
                                       const TimeGridSnapshot& targetGrid,
                                       const ContentTimelineProjection& targetPlacement,
                                       const std::vector<TimingOnset>& guide,
                                       const TimeGridSnapshot& guideGrid,
                                       const ContentTimelineProjection& guidePlacement,
                                       double strength);

} // namespace OpenTune
