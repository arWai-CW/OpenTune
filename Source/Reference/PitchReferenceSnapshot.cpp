#include "PitchReferenceSnapshot.h"

#include <algorithm>
#include <cmath>

namespace OpenTune {
namespace {

constexpr double kReferencePitchCenterWindowSeconds = 0.4;
constexpr double kReferencePitchOffsetJumpThresholdCents = 50.0;
constexpr double kReferencePitchOffsetMedianWindowSeconds = 0.12;
constexpr double kReferencePitchStabilitySlopeWindowSeconds = 0.05;
constexpr double kReferencePitchMaximumStableSlopeCentsPerSecond = 1640.0;
constexpr double kReferencePitchMinimumStableStretchSeconds = 0.08;
constexpr double kReferencePitchChangeToleranceCents = 0.5;
constexpr double kPi = 3.14159265358979323846;

bool isVoiced(float hz)
{
    return hz > 0.0f && std::isfinite(hz);
}

bool canSample(const PitchReferenceSnapshot& region)
{
    return region.projection.isValid() && region.timeGrid != nullptr && region.curve != nullptr
        && !region.curve->isEmpty() && region.curve->getHopSize() > 0
        && region.curve->getSampleRate() > 0.0;
}

float effectivePitch(const PitchReferenceSnapshot& region, int frame)
{
    float hz = 0.0f;
    region.curve->forEachEffectiveF0Span(frame, frame + 1, region.originalPitchRatio,
        [&](int, const float* data, int, float gain) { hz = *data * gain; });
    return hz;
}

} // namespace

PitchReferenceCorrection planPitchReferenceCorrection(
    const PitchReferenceSnapshot& target,
    const std::vector<PitchReferenceSnapshot>& references,
    const std::vector<F0FrameRange>& ranges)
{
    PitchReferenceCorrection result;
    if (!canSample(target))
        return result;

    struct ReferenceRegion
    {
        const PitchReferenceSnapshot& snapshot;
        F0Timeline timeline;
    };
    std::vector<ReferenceRegion> overlapping;
    const auto& placement = target.projection;
    for (const auto& reference : references) {
        const auto& refPlacement = reference.projection;
        if (canSample(reference) && halfOpenRangesOverlap(
                placement.timelineStartSeconds, placement.timelineEndSeconds(),
                refPlacement.timelineStartSeconds, refPlacement.timelineEndSeconds())) {
            overlapping.push_back({reference, {reference.curve->getHopSize(),
                reference.curve->getSampleRate(), static_cast<int>(reference.curve->size())}});
        }
    }
    if (overlapping.empty())
        return result;
    result.hasClipOverlap = true;

    const auto pitchAtTimelineTime = [&](double time) {
        for (const auto& [ref, timeline] : overlapping) {
            const auto& p = ref.projection;
            if (time < p.timelineStartSeconds || time >= p.timelineEndSeconds())
                continue;
            const double source = ref.timeGrid->tauInverse(p.projectTimelineTimeToContent(time));
            if (!(source >= 0.0 && source < timeline.timeAtFrame(timeline.endFrameExclusive())))
                continue;
            const int frame = std::min(timeline.nearestFrameBoundary(source), timeline.endFrameExclusive() - 1);
            if (const float hz = effectivePitch(ref, frame); isVoiced(hz))
                return hz;
        }
        return 0.0f;
    };

    const F0Timeline timeline{target.curve->getHopSize(), target.curve->getSampleRate(),
                              static_cast<int>(target.curve->size())};
    const double frameSeconds = timeline.timeAtFrame(1);
    int centerWindowFrames = std::max(1, static_cast<int>(std::lround(
        kReferencePitchCenterWindowSeconds / frameSeconds)));
    if (centerWindowFrames % 2 == 0)
        ++centerWindowFrames;
    const int centerWindowRadius = centerWindowFrames / 2;
    const int stabilitySlopeRadiusFrames = std::max(1, static_cast<int>(std::ceil(
        kReferencePitchStabilitySlopeWindowSeconds / (2.0 * frameSeconds))));
    const int minimumStableStretchFrames = std::max(1, static_cast<int>(std::ceil(
        kReferencePitchMinimumStableStretchSeconds / frameSeconds)));
    int medianWindowFrames = std::max(1, static_cast<int>(std::lround(
        kReferencePitchOffsetMedianWindowSeconds / frameSeconds)));
    if (medianWindowFrames % 2 == 0)
        ++medianWindowFrames;
    const int medianWindowRadius = medianWindowFrames / 2;
    const auto& originalF0 = target.curve->getOriginalF0();

    struct RunFrame
    {
        float baseHz;
        float referenceHz;
        float offsetCents;
    };

    for (const auto& range : ranges) {
        std::vector<RunFrame> run;
        int runStartFrame = 0;
        const auto finishRun = [&] {
            if (run.empty())
                return;
            const int count = static_cast<int>(run.size());
            for (int i = 1; i < count; ++i) {
                auto& offsetCents = run[static_cast<size_t>(i)].offsetCents;
                const auto& previous = run[static_cast<size_t>(i - 1)];
                const auto& current = run[static_cast<size_t>(i)];
                const auto pitchChangeCents = [](float previousHz, float currentHz) {
                    return std::abs(1200.0 * std::log2(
                        static_cast<double>(currentHz) / previousHz));
                };
                if (pitchChangeCents(previous.baseHz, current.baseHz)
                        > kReferencePitchOffsetJumpThresholdCents
                    || pitchChangeCents(previous.referenceHz, current.referenceHz)
                        > kReferencePitchOffsetJumpThresholdCents)
                    continue;
                const float previousOffsetCents = previous.offsetCents;
                while (offsetCents - previousOffsetCents > 600.0f)
                    offsetCents -= 1200.0f;
                while (offsetCents - previousOffsetCents < -600.0f)
                    offsetCents += 1200.0f;
            }

            std::vector<bool> stableCandidate(static_cast<size_t>(count), false);
            for (int i = stabilitySlopeRadiusFrames;
                 i + stabilitySlopeRadiusFrames < count; ++i) {
                const auto slopeCentsPerSecond = [&](float leftHz, float rightHz) {
                    return std::abs(1200.0 * std::log2(
                        static_cast<double>(rightHz) / leftHz))
                        / (2.0 * stabilitySlopeRadiusFrames * frameSeconds);
                };
                const auto& left = run[static_cast<size_t>(i - stabilitySlopeRadiusFrames)];
                const auto& right = run[static_cast<size_t>(i + stabilitySlopeRadiusFrames)];
                stableCandidate[static_cast<size_t>(i)] =
                    slopeCentsPerSecond(left.baseHz, right.baseHz)
                        <= kReferencePitchMaximumStableSlopeCentsPerSecond
                    && slopeCentsPerSecond(left.referenceHz, right.referenceHz)
                        <= kReferencePitchMaximumStableSlopeCentsPerSecond;
            }

            std::vector<std::pair<int, int>> stableStretches;
            for (int i = 0; i < count;) {
                if (!stableCandidate[static_cast<size_t>(i)]) {
                    ++i;
                    continue;
                }
                const int start = i;
                while (i < count && stableCandidate[static_cast<size_t>(i)])
                    ++i;
                if (i - start >= minimumStableStretchFrames)
                    stableStretches.emplace_back(start, i);
            }

            std::vector<int> noteBoundaries;
            for (const auto& note : target.notes) {
                const int noteStart = timeline.nearestFrameBoundary(note.startTime) - runStartFrame;
                const int noteEnd = timeline.nearestFrameBoundary(note.endTime) - runStartFrame;
                if (noteStart > 0 && noteStart < count)
                    noteBoundaries.push_back(noteStart);
                if (noteEnd > 0 && noteEnd < count)
                    noteBoundaries.push_back(noteEnd);
            }
            std::sort(noteBoundaries.begin(), noteBoundaries.end());
            noteBoundaries.erase(std::unique(noteBoundaries.begin(), noteBoundaries.end()),
                                 noteBoundaries.end());

            std::vector<float> filteredOffsets(static_cast<size_t>(count), 0.0f);
            std::vector<float> medianWindow;
            medianWindow.reserve(static_cast<size_t>(medianWindowFrames));
            for (const auto& [stretchStart, stretchEnd] : stableStretches) {
                for (int i = stretchStart; i < stretchEnd; ++i) {
                    const int radius = std::min(medianWindowRadius,
                        std::min(i - stretchStart, stretchEnd - 1 - i));
                    medianWindow.clear();
                    for (int j = i - radius; j <= i + radius; ++j)
                        medianWindow.push_back(run[static_cast<size_t>(j)].offsetCents);
                    std::sort(medianWindow.begin(), medianWindow.end());
                    const size_t middle = medianWindow.size() / 2;
                    float median = medianWindow[middle];
                    if (medianWindow.size() % 2 == 0)
                        median = (medianWindow[middle - 1] + medianWindow[middle]) * 0.5f;
                    filteredOffsets[static_cast<size_t>(i)] = median;
                }
            }

            struct StableOffset
            {
                int frame;
                float cents;
                int smoothingSegment;
            };
            std::vector<StableOffset> stableOffsets;
            int smoothingSegment = 0;
            for (size_t stretchIndex = 0; stretchIndex < stableStretches.size(); ++stretchIndex) {
                const auto [stretchStart, stretchEnd] = stableStretches[stretchIndex];
                if (stretchIndex > 0) {
                    const int previousEnd = stableStretches[stretchIndex - 1].second;
                    const bool noteBoundary = std::any_of(noteBoundaries.begin(), noteBoundaries.end(),
                        [previousEnd, stretchStart](int boundary) {
                            return boundary > previousEnd - 1 && boundary <= stretchStart;
                        });
                    const double offsetJump = std::abs(
                        static_cast<double>(filteredOffsets[static_cast<size_t>(stretchStart)])
                        - filteredOffsets[static_cast<size_t>(previousEnd - 1)]);
                    if (noteBoundary || offsetJump > kReferencePitchOffsetJumpThresholdCents)
                        ++smoothingSegment;
                }
                for (int frame = stretchStart; frame < stretchEnd; ++frame) {
                    if (frame > stretchStart
                        && std::binary_search(noteBoundaries.begin(), noteBoundaries.end(), frame))
                        ++smoothingSegment;
                    stableOffsets.push_back({frame, filteredOffsets[static_cast<size_t>(frame)],
                                             smoothingSegment});
                }
            }

            std::vector<float> smoothedOffsets(static_cast<size_t>(count), 0.0f);
            if (stableOffsets.empty()) {
                medianWindow.clear();
                for (const auto& frame : run)
                    medianWindow.push_back(frame.offsetCents);
                std::sort(medianWindow.begin(), medianWindow.end());
                const size_t middle = medianWindow.size() / 2;
                float median = medianWindow[middle];
                if (medianWindow.size() % 2 == 0)
                    median = (medianWindow[middle - 1] + medianWindow[middle]) * 0.5f;
                std::fill(smoothedOffsets.begin(), smoothedOffsets.end(), median);
            } else {
                std::vector<float> stableSmoothedOffsets(stableOffsets.size());
                for (size_t i = 0; i < stableOffsets.size(); ++i) {
                    double weightedOffsetSum = 0.0;
                    double weightedFrameOffsetSum = 0.0;
                    double weightSum = 0.0;
                    for (size_t j = i; j > 0;) {
                        --j;
                        if (stableOffsets[i].smoothingSegment != stableOffsets[j].smoothingSegment
                            || stableOffsets[i].frame - stableOffsets[j].frame > centerWindowRadius)
                            break;
                        const int lag = stableOffsets[i].frame - stableOffsets[j].frame;
                        const double weight = centerWindowRadius == 0 ? 1.0
                            : 0.5 * (1.0 + std::cos(kPi * lag / centerWindowRadius));
                        weightedOffsetSum += stableOffsets[j].cents * weight;
                        weightedFrameOffsetSum += (stableOffsets[j].frame - stableOffsets[i].frame) * weight;
                        weightSum += weight;
                    }
                    for (size_t j = i; j < stableOffsets.size(); ++j) {
                        if (stableOffsets[i].smoothingSegment != stableOffsets[j].smoothingSegment
                            || stableOffsets[j].frame - stableOffsets[i].frame > centerWindowRadius)
                            break;
                        const int lag = stableOffsets[j].frame - stableOffsets[i].frame;
                        const double weight = centerWindowRadius == 0 ? 1.0
                            : 0.5 * (1.0 + std::cos(kPi * lag / centerWindowRadius));
                        weightedOffsetSum += stableOffsets[j].cents * weight;
                        weightedFrameOffsetSum += (stableOffsets[j].frame - stableOffsets[i].frame) * weight;
                        weightSum += weight;
                    }
                    double localSlopeCentsPerFrame = 0.0;
                    const bool hasPrevious = i > 0
                        && stableOffsets[i - 1].smoothingSegment == stableOffsets[i].smoothingSegment;
                    const bool hasNext = i + 1 < stableOffsets.size()
                        && stableOffsets[i + 1].smoothingSegment == stableOffsets[i].smoothingSegment;
                    if (hasPrevious && hasNext) {
                        localSlopeCentsPerFrame = static_cast<double>(
                            stableOffsets[i + 1].cents - stableOffsets[i - 1].cents)
                            / (stableOffsets[i + 1].frame - stableOffsets[i - 1].frame);
                    } else if (hasNext) {
                        localSlopeCentsPerFrame = static_cast<double>(
                            stableOffsets[i + 1].cents - stableOffsets[i].cents)
                            / (stableOffsets[i + 1].frame - stableOffsets[i].frame);
                    } else if (hasPrevious) {
                        localSlopeCentsPerFrame = static_cast<double>(
                            stableOffsets[i].cents - stableOffsets[i - 1].cents)
                            / (stableOffsets[i].frame - stableOffsets[i - 1].frame);
                    }
                    stableSmoothedOffsets[i] = static_cast<float>(
                        weightedOffsetSum / weightSum
                        - localSlopeCentsPerFrame * weightedFrameOffsetSum / weightSum);
                }

                for (int frame = 0; frame < count; ++frame) {
                    const auto right = std::upper_bound(stableOffsets.begin(), stableOffsets.end(), frame,
                        [](int value, const StableOffset& offset) { return value < offset.frame; });
                    if (right == stableOffsets.begin()) {
                        smoothedOffsets[static_cast<size_t>(frame)] = stableSmoothedOffsets.front();
                    } else if (right == stableOffsets.end()) {
                        smoothedOffsets[static_cast<size_t>(frame)] = stableSmoothedOffsets.back();
                    } else {
                        const size_t rightIndex = static_cast<size_t>(right - stableOffsets.begin());
                        const size_t leftIndex = rightIndex - 1;
                        const int leftFrame = stableOffsets[leftIndex].frame;
                        const int rightFrame = stableOffsets[rightIndex].frame;
                        const double fraction = static_cast<double>(frame - leftFrame)
                            / (rightFrame - leftFrame);
                        smoothedOffsets[static_cast<size_t>(frame)] = static_cast<float>(
                            stableSmoothedOffsets[leftIndex]
                            + fraction * (stableSmoothedOffsets[rightIndex] - stableSmoothedOffsets[leftIndex]));
                    }
                }
            }

            PitchCorrectionSegment segment{runStartFrame, runStartFrame + count, {},
                                           PitchCorrectionSegment::Source::HandDraw};
            segment.f0Data.reserve(run.size());
            bool changed = false;
            for (int i = 0; i < count; ++i) {
                const float correctedHz = static_cast<float>(run[static_cast<size_t>(i)].baseHz
                    * std::exp2(smoothedOffsets[static_cast<size_t>(i)] / 1200.0));
                segment.f0Data.push_back(correctedHz);
                const float currentHz = effectivePitch(target, runStartFrame + i);
                if (!isVoiced(currentHz) || std::abs(1200.0 * std::log2(
                        static_cast<double>(correctedHz) / currentHz)) > kReferencePitchChangeToleranceCents)
                    changed = true;
            }
            if (changed)
                result.segments.push_back(std::move(segment));
            run.clear();
        };
        for (int frame = std::max(0, range.startFrame);
             frame < std::min(range.endFrameExclusive, timeline.endFrameExclusive()); ++frame) {
            float referenceHz = 0.0f;
            const float baseHz = originalF0[static_cast<size_t>(frame)] * target.originalPitchRatio;
            if (isVoiced(baseHz)) {
                const double time = placement.projectContentTimeToTimeline(
                    target.timeGrid->tauForward(timeline.timeAtFrame(frame)));
                if (time >= placement.timelineStartSeconds && time < placement.timelineEndSeconds())
                    referenceHz = pitchAtTimelineTime(time);
            }
            if (!isVoiced(referenceHz)) {
                finishRun();
                continue;
            }
            result.hasVoicedOverlap = true;
            if (run.empty())
                runStartFrame = frame;
            double offsetCents = 1200.0 * std::log2(
                static_cast<double>(referenceHz) / baseHz);
            offsetCents -= 1200.0 * std::round(offsetCents / 1200.0);
            run.push_back({baseHz, referenceHz, static_cast<float>(offsetCents)});
        }
        finishRun();
    }
    return result;
}

} // namespace OpenTune
