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
    const char* rejection = nullptr;
    std::size_t targetAttacks = 0;
    std::size_t referenceAttacks = 0;
    std::size_t matchedAttacks = 0;
    bool affectsProtectedTiming = false;
};

inline constexpr double minimumTimingOverlapSeconds = 0.5;

struct TimingOutputRange
{
    double begin;
    double end;
};

bool preservesTimingAudioInRange(const TimeGridSnapshot& before, const TimeGridSnapshot& after,
                                 TimingOutputRange range) noexcept;

std::shared_ptr<const TimeGridSnapshot> restoreOriginalTimingInRange(
    const TimeGridSnapshot& grid, TimingOutputRange range,
    const std::vector<TimingOutputRange>& protectedRanges = {});

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
                                       double strength,
                                       const std::vector<TimingOutputRange>& protectedRanges = {});

} // namespace OpenTune
