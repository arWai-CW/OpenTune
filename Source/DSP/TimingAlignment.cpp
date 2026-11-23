#include "TimingAlignment.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace OpenTune {
namespace {

constexpr double hopSeconds = 0.005;
constexpr double maxShift = 0.18;
constexpr double ambiguityMargin = 0.018;
constexpr double boundaryEpsilon = 1.0e-9;

struct ProjectedOnset
{
    TimingOnset onset;
    double timeline;
};

std::vector<ProjectedOnset> projectOnsets(const std::vector<TimingOnset>& onsets,
                                         const TimeGridSnapshot& grid,
                                         const ContentTimelineProjection& placement,
                                         double begin, double end)
{
    std::vector<ProjectedOnset> visible;
    for (const auto& onset : onsets) {
        if (onset.sourceSeconds <= 0.0 || onset.sourceSeconds >= grid.totalDurationSeconds())
            continue;
        const double time = placement.projectContentTimeToTimeline(grid.tauForward(onset.sourceSeconds));
        if (time >= begin && time < end)
            visible.push_back({onset, time});
    }
    return visible;
}

int uniqueMatch(const ProjectedOnset& target, const std::vector<ProjectedOnset>& candidates)
{
    auto it = std::lower_bound(candidates.begin(), candidates.end(), target.timeline - maxShift,
        [](const ProjectedOnset& onset, double time) { return onset.timeline < time; });
    int bestIndex = -1;
    double best = std::numeric_limits<double>::infinity();
    double runnerUp = best;
    for (; it != candidates.end() && it->timeline <= target.timeline + maxShift; ++it) {
        const double score = std::abs(it->timeline - target.timeline)
            + (it->onset.highFrequencyDominant == target.onset.highFrequencyDominant ? 0.0 : 0.035);
        if (score < best) {
            runnerUp = best;
            best = score;
            bestIndex = static_cast<int>(it - candidates.begin());
        } else {
            runnerUp = std::min(runnerUp, score);
        }
    }
    return runnerUp - best >= ambiguityMargin ? bestIndex : -1;
}

bool hasCompetingSyllablePhase(const std::vector<ProjectedOnset>& targets,
                              const std::vector<ProjectedOnset>& guides,
                              const std::vector<int>& matches, size_t matched)
{
    // Equal counts can hide an extra attack at one end and a missing one at the other.
    const bool fullCoverage = matched == std::min(targets.size(), guides.size());
    for (const int direction : {-1, 1}) {
        int run = 0;
        double firstOffset = 0.0;
        int previousGuide = -1;
        for (size_t i = 0; i < matches.size(); ++i) {
            const int guide = matches[i];
            const int neighbor = guide + direction;
            if (guide < 0 || neighbor < 0 || neighbor >= static_cast<int>(guides.size())) {
                run = 0;
                continue;
            }
            const auto& target = targets[i];
            const auto& candidate = guides[static_cast<size_t>(neighbor)];
            const double offset = guides[static_cast<size_t>(guide)].timeline - target.timeline;
            const double alternative = candidate.timeline - target.timeline;
            if (offset * alternative >= 0.0
                || std::abs(alternative) > maxShift
                || (fullCoverage && std::abs(alternative) > 2.5 * std::abs(offset))
                || candidate.onset.highFrequencyDominant != target.onset.highFrequencyDominant) {
                run = 0;
                continue;
            }
            if (run == 0 || guide != previousGuide + 1 || std::abs(alternative - firstOffset) > ambiguityMargin) {
                run = 0;
                firstOffset = alternative;
            }
            previousGuide = guide;
            if (++run >= 3)
                return true;
        }
    }
    return false;
}

bool validSpan(const TimeHandle& first, const TimeHandle& second)
{
    return TimeGridSnapshot::hasMinimumSourceSpacing(first.source_seconds, second.source_seconds)
        && TimeGridSnapshot::supportsStretchRatio(second.source_seconds - first.source_seconds,
                                                  second.output_seconds - first.output_seconds);
}

auto anchorInsertionPoint(std::vector<TimeHandle>& handles, const TimeHandle& anchor)
{
    const auto after = std::lower_bound(handles.begin(), handles.end(), anchor.source_seconds,
        [](const TimeHandle& handle, double time) { return handle.source_seconds < time; });
    if (after == handles.begin() || after == handles.end()
        || !validSpan(*(after - 1), anchor) || !validSpan(anchor, *after))
        return handles.end();
    return after;
}

bool insertAnchor(std::vector<TimeHandle>& handles, const TimeHandle& anchor)
{
    const auto after = anchorInsertionPoint(handles, anchor);
    if (after == handles.end())
        return false;
    handles.insert(after, anchor);
    return true;
}

std::vector<TimingOnset> detectTimingOnsets(const juce::AudioBuffer<float>& audio, double sampleRate,
                                          double sourceBegin, double sourceEnd)
{
    if (!std::isfinite(sampleRate) || sampleRate < 1000.0 || sampleRate > 768000.0
        || audio.getNumChannels() == 0)
        return {};

    const auto sampleAt = [&](double seconds) {
        return static_cast<int>(std::ceil(std::clamp(seconds * sampleRate, 0.0,
                                                    static_cast<double>(audio.getNumSamples()))));
    };
    const int firstSample = sampleAt(sourceBegin);
    const int hop = static_cast<int>(std::lround(sampleRate * hopSeconds));
    const int frames = (sampleAt(sourceEnd) - firstSample) / hop;
    if (frames < 7)
        return {};
    struct Energy { double full = 0.0; double high = 0.0; };
    std::vector<Energy> envelope(static_cast<size_t>(frames));
    Energy peak;
    for (int frame = 0; frame < frames; ++frame) {
        auto& energy = envelope[static_cast<size_t>(frame)];
        const int start = firstSample + frame * hop;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch) {
            const auto* samples = audio.getReadPointer(ch);
            float previous = start > firstSample ? samples[start - 1] : samples[start];
            for (int i = start; i < start + hop; ++i) {
                const float sample = samples[i];
                const float diff = sample - previous;
                energy.full += sample * sample;
                energy.high += diff * diff;
                previous = sample;
            }
        }
        energy.full = std::sqrt(energy.full);
        energy.high = std::sqrt(energy.high);
        peak.full = std::max(peak.full, energy.full);
        peak.high = std::max(peak.high, energy.high);
    }
    if (peak.full == 0.0 || peak.high == 0.0)
        return {};

    // Positive energy flux finds syllable attacks without using note pitch.
    std::vector<double> flux(envelope.size());
    for (size_t i = 3; i < envelope.size(); ++i) {
        const auto& now = envelope[i];
        const auto& earlier = envelope[i - 3];
        const double full = (now.full - earlier.full) / peak.full;
        const double high = (now.high - earlier.high) / peak.high;
        flux[i] = std::max(full, 0.7 * high);
    }

    std::vector<TimingOnset> onsets;
    for (size_t i = 3; i < flux.size(); ++i) {
        if (flux[i] < 0.16 || flux[i] <= flux[i - 1])
            continue;
        const auto lookAheadEnd = flux.begin() + std::min(i + 4, flux.size());
        if (std::any_of(flux.begin() + i + 1, lookAheadEnd, [&](double next) { return next > flux[i]; }))
            continue;
        size_t attack = i;
        while (attack > i - 3 && flux[attack - 1] >= 0.4 * flux[i])
            --attack;
        const double time = static_cast<double>(firstSample + attack * hop) / sampleRate;
        if (!onsets.empty() && time - onsets.back().sourceSeconds < 0.105)
            continue;
        const auto& energy = envelope[i];
        onsets.push_back({time, energy.high / peak.high > 1.5 * energy.full / peak.full});
    }
    return onsets;
}

} // namespace

std::vector<TimingOnset> detectTimingOnsets(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    return detectTimingOnsets(audio, sampleRate, 0.0, std::numeric_limits<double>::infinity());
}

std::vector<TimingOnset> detectTimingOnsetsInOverlap(const juce::AudioBuffer<float>& audio,
                                                   double sampleRate,
                                                   const TimeGridSnapshot& grid,
                                                   const ContentTimelineProjection& placement,
                                                   const ContentTimelineProjection& otherPlacement)
{
    const auto begin = std::max(placement.timelineStartSeconds, otherPlacement.timelineStartSeconds);
    const auto end = std::min(placement.timelineEndSeconds(), otherPlacement.timelineEndSeconds());
    if (!placement.isValid() || !otherPlacement.isValid() || grid.empty()
        || !std::isfinite(begin) || !std::isfinite(end) || end <= begin)
        return {};
    const auto contentBegin = placement.projectTimelineTimeToContent(begin);
    const auto contentEnd = placement.projectTimelineTimeToContent(end);
    if (!std::isfinite(contentBegin) || !std::isfinite(contentEnd))
        return {};
    return detectTimingOnsets(audio, sampleRate,
                             grid.tauInverse(contentBegin), grid.tauInverse(contentEnd));
}

bool preservesTimingAudioInRange(const TimeGridSnapshot& before, const TimeGridSnapshot& after,
                                 TimingOutputRange range) noexcept
{
    if (before.empty() || after.empty() || !std::isfinite(range.begin) || !std::isfinite(range.end)
        || range.end <= range.begin)
        return false;

    const auto& a = before.handles();
    const auto& b = after.handles();
    if (a.back().output_seconds != b.back().output_seconds)
        return false;

    const auto samePosition = [](const TimeHandle& first, const TimeHandle& second) {
        return first.source_seconds == second.source_seconds && first.output_seconds == second.output_seconds;
    };
    const auto isIdentity = [](const TimeHandle& handle) { return handle.source_seconds == handle.output_seconds; };
    for (size_t i = 1, j = 1; i < a.size() && j < b.size();) {
        const auto begin = std::max(a[i - 1].output_seconds, b[j - 1].output_seconds);
        const auto end = std::min(a[i].output_seconds, b[j].output_seconds);
        if (begin >= range.end)
            break;
        if (std::min(range.end, end) - std::max(range.begin, begin) > boundaryEpsilon) {
            // Stage 2 stretches whole anchor spans independently. Splitting a stretched
            // span can change its waveform even when the time map there stays the same.
            const bool sameSpan = samePosition(a[i - 1], b[j - 1]) && samePosition(a[i], b[j]);
            const bool bothDry = isIdentity(a[i - 1]) && isIdentity(a[i]) && isIdentity(b[j - 1]) && isIdentity(b[j]);
            if (!sameSpan && !bothDry)
                return false;
        }
        if (a[i].output_seconds == b[j].output_seconds) {
            ++i;
            ++j;
        } else if (a[i].output_seconds < b[j].output_seconds) {
            ++i;
        } else {
            ++j;
        }
    }
    return true;
}

std::shared_ptr<const TimeGridSnapshot> restoreOriginalTimingInRange(
    const TimeGridSnapshot& grid, TimingOutputRange range,
    const std::vector<TimingOutputRange>& protectedRanges)
{
    if (grid.empty() || !std::isfinite(range.begin) || !std::isfinite(range.end))
        return {};
    range.begin = std::max(0.0, range.begin);
    range.end = std::min(grid.handles().back().output_seconds, range.end);
    if (range.end <= range.begin || grid.isIdentityInOutputRange(range.begin, range.end))
        return {};

    auto handles = grid.handles();
    handles.erase(std::remove_if(handles.begin(), handles.end(), [&](const auto& handle) {
        return handle.source_seconds != handle.output_seconds
            && handle.output_seconds > range.begin + boundaryEpsilon
            && handle.output_seconds < range.end - boundaryEpsilon;
    }), handles.end());
    auto restored = TimeGridSnapshot::makeFromHandles(std::move(handles));
    if (!restored || !restored->isIdentityInOutputRange(range.begin, range.end))
        return {};

    const auto preserves = [&](TimingOutputRange outside) {
        return preservesTimingAudioInRange(grid, *restored, outside);
    };
    // Never reset hidden audio outside the focused clip, including other ARA placements.
    const double end = grid.handles().back().output_seconds;
    if ((range.begin > 0.0 && !preserves({0.0, range.begin}))
        || (range.end < end && !preserves({range.end, end}))
        || !std::all_of(protectedRanges.begin(), protectedRanges.end(), preserves))
        return {};
    return restored;
}

TimingAlignment alignTimingToReference(const std::vector<TimingOnset>& target,
                                       const TimeGridSnapshot& targetGrid,
                                       const ContentTimelineProjection& targetPlacement,
                                       const std::vector<TimingOnset>& guide,
                                       const TimeGridSnapshot& guideGrid,
                                       const ContentTimelineProjection& guidePlacement,
                                       double strength,
                                       const std::vector<TimingOutputRange>& protectedRanges)
{
    TimingAlignment result;
    const auto reject = [&](const char* stage) {
        result.grid.reset();
        result.movedAnchors = 0;
        result.maxMoveSeconds = 0.0;
        result.rejection = stage;
        return result;
    };

    if (targetGrid.empty() || guideGrid.empty() || !targetPlacement.isValid()
        || !guidePlacement.isValid() || !(strength > 0.0 && strength <= 1.0))
        return reject("invalid timing grid or placement");
    const double begin = std::max(targetPlacement.timelineStartSeconds, guidePlacement.timelineStartSeconds);
    const double end = std::min(targetPlacement.timelineEndSeconds(), guidePlacement.timelineEndSeconds());
    if (end - begin < minimumTimingOverlapSeconds)
        return reject("insufficient overlap");

    const auto targets = projectOnsets(target, targetGrid, targetPlacement, begin, end);
    const auto guides = projectOnsets(guide, guideGrid, guidePlacement, begin, end);
    result.targetAttacks = targets.size();
    result.referenceAttacks = guides.size();
    if (targets.size() < 3 || guides.size() < 3)
        return reject("too few detected attacks (need 3 on each clip)");
    std::vector<int> matches(targets.size(), -1);
    int previous = -1;
    for (size_t i = 0; i < targets.size(); ++i) {
        const int index = uniqueMatch(targets[i], guides);
        if (index <= previous || uniqueMatch(guides[static_cast<size_t>(index)], targets) != static_cast<int>(i))
            continue;
        matches[i] = previous = index;
    }
    // An isolated coincidence after a break isn't evidence of the same phrase.
    std::vector<bool> corroborated(targets.size());
    size_t lastMatch = targets.size();
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i] < 0)
            continue;
        if (lastMatch != targets.size()) {
            const double targetGap = targets[i].timeline - targets[lastMatch].timeline;
            const double guideGap = guides[matches[i]].timeline - guides[matches[lastMatch]].timeline;
            if (std::max(targetGap, guideGap) <= 1.0 && std::abs(targetGap - guideGap) <= 0.12)
                corroborated[lastMatch] = corroborated[i] = true;
        }
        lastMatch = i;
    }
    size_t matched = 0;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (!corroborated[i])
            matches[i] = -1;
        else
            ++matched;
    }
    result.matchedAttacks = matched;
    if (matched < 3 || matched * 2 < targets.size())
        return reject("too few reliable matches (need 3 and at least half of target attacks)");
    if (hasCompetingSyllablePhase(targets, guides, matches, matched))
        return reject("ambiguous syllable matches");

    constexpr double minMove = 0.012;
    const auto offsetFor = [&](size_t i) {
        return matches[i] < 0 ? 0.0 : guides[static_cast<size_t>(matches[i])].timeline - targets[i].timeline;
    };
    bool alreadyAligned = true;
    for (size_t i = 0; i < targets.size(); ++i)
        alreadyAligned = alreadyAligned && std::abs(offsetFor(i)) < minMove;
    if (alreadyAligned) {
        result.alreadyAligned = true;
        return result;
    }

    auto handles = targetGrid.handles();
    const double firstOutput = std::max(0.0, targetPlacement.projectTimelineTimeToContent(begin));
    const double lastOutput = std::min(targetGrid.totalDurationSeconds(), targetPlacement.projectTimelineTimeToContent(end));
    // Opening the time tool can seed an untouched identity grid. Those seeds
    // aren't manual edits and shouldn't block detected attacks in the overlap.
    if (targetGrid.isIdentityInOutputRange(firstOutput, lastOutput)) {
        handles.erase(std::remove_if(handles.begin(), handles.end(), [&](const auto& handle) {
            return handle.kind == HandleKind::InternalOnset
                && handle.output_seconds > firstOutput + boundaryEpsilon
                && handle.output_seconds < lastOutput - boundaryEpsilon;
        }), handles.end());
    }
    uint64_t id = 0;
    for (const auto& handle : targetGrid.handles())
        id = std::max(id, handle.id);

    // Freeze the map outside the overlap and at attacks without a reliable guide.
    const auto lockTime = [&](double output) {
        constexpr double epsilon = 1.0e-6;
        if (std::any_of(handles.begin(), handles.end(), [&](const auto& anchor) {
                return std::abs(anchor.output_seconds - output) < epsilon;
            }))
            return true;
        return insertAnchor(handles, {++id, targetGrid.tauInverse(output), output, HandleKind::InternalOnset});
    };
    const auto lockBoundary = [&](double output, double step) -> std::optional<double> {
        for (int attempt = 0; attempt <= 12 && output >= firstOutput && output <= lastOutput;
             ++attempt, output += step)
            if (lockTime(output))
                return output;
        return std::nullopt;
    };
    const auto firstLock = lockBoundary(firstOutput, 0.025);
    const auto lastLock = lockBoundary(lastOutput, -0.025);
    if (!firstLock || !lastLock || *firstLock >= *lastLock)
        return reject("could not lock overlap boundaries");
    const auto moveFor = [&](size_t i) {
        return offsetFor(i) * strength;
    };
    for (size_t i = 0; i < targets.size(); ++i)
        if (std::abs(moveFor(i)) < minMove)
            lockTime(targetGrid.tauForward(targets[i].onset.sourceSeconds));

    const auto lastLockedId = id;
    bool skippedProtectedMove = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        const double time = targets[i].timeline;
        const double source = targets[i].onset.sourceSeconds;
        const double oldOutput = targetGrid.tauForward(source);
        const double move = moveFor(i);
        if (std::abs(move) < minMove)
            continue;

        // Keep manual handles intact and avoid extreme stretches between anchors.
        const double output = targetPlacement.projectTimelineTimeToContent(time + move);
        if (oldOutput <= *firstLock || oldOutput >= *lastLock || output <= *firstLock || output >= *lastLock)
            continue;
        const auto kind = targets[i].onset.highFrequencyDominant ? HandleKind::OnsetSibilant : HandleKind::OnsetVoiced;
        const TimeHandle anchor{++id, source, output, kind, Confidence::High};
        const auto after = anchorInsertionPoint(handles, anchor);
        if (after == handles.end())
            continue;

        // Adding a moving anchor changes both halves of its stretch span.
        const auto& left = *(after - 1);
        if (std::any_of(protectedRanges.begin(), protectedRanges.end(), [&](const auto& range) {
                return std::min(range.end, after->output_seconds)
                    - std::max(range.begin, left.output_seconds) > boundaryEpsilon;
            })) {
            skippedProtectedMove = true;
            continue;
        }
        handles.insert(after, anchor);
    }

    const auto unsafeMove = [&](size_t i, double output) -> const char* {
        const double after = targetPlacement.projectContentTimeToTimeline(output);
        const double before = targets[i].timeline;
        const double reference = before + offsetFor(i);
        if (std::abs(moveFor(i)) < minMove && std::abs(after - before) > 0.008)
            return "stretch would move an unedited attack";
        if (matches[i] >= 0 && std::abs(after - reference) > std::abs(before - reference) + 1e-6)
            return "stretch would worsen a reliable match";
        return nullptr;
    };

    // A blocked attack can still move indirectly between new anchors. Test
    // the span made by removing either neighbor before discarding them both.
    const char* rejection = "no anchors can move within the timing limits";
    for (;;) {
        result.grid = TimeGridSnapshot::makeFromHandles(handles);
        if (!result.grid)
            return reject("stretch would create an invalid timing grid");

        bool removed = false;
        for (size_t i = 0; i < targets.size(); ++i) {
            const double source = targets[i].onset.sourceSeconds;
            const auto* reason = unsafeMove(i, result.grid->tauForward(source));
            if (!reason)
                continue;

            rejection = reason;
            const auto right = std::lower_bound(handles.begin(), handles.end(), source,
                [](const TimeHandle& handle, double time) { return handle.source_seconds < time; });
            const auto left = right - 1;
            const bool canDropLeft = source < right->source_seconds && left->id > lastLockedId;
            const bool canDropRight = right->id > lastLockedId;
            if (!canDropLeft && !canDropRight)
                return reject(rejection);
            const auto spanIsSafe = [&](const TimeHandle& a, const TimeHandle& b) {
                const double ratio = (source - a.source_seconds) / (b.source_seconds - a.source_seconds);
                const double output = a.output_seconds + ratio * (b.output_seconds - a.output_seconds);
                return unsafeMove(i, output) == nullptr;
            };
            if (canDropLeft && canDropRight) {
                if (spanIsSafe(*left, *(right + 1)))
                    handles.erase(right);
                else if (spanIsSafe(*(left - 1), *right))
                    handles.erase(left);
                else
                    handles.erase(left, right + 1);
            } else {
                handles.erase(canDropLeft ? left : right);
            }
            removed = true;
            break;
        }
        if (!removed)
            break;
    }
    for (const auto& handle : handles) {
        if (handle.id <= lastLockedId)
            continue;
        ++result.movedAnchors;
        result.maxMoveSeconds = std::max(result.maxMoveSeconds,
            std::abs(handle.output_seconds - targetGrid.tauForward(handle.source_seconds)));
    }
    const auto rejectProtectedTiming = [&] {
        result.affectsProtectedTiming = true;
        return reject("timing would affect another placement that shares these ARA edits");
    };
    if (result.movedAnchors == 0) {
        if (skippedProtectedMove)
            return rejectProtectedTiming();
        return reject(rejection);
    }
    if (std::any_of(protectedRanges.begin(), protectedRanges.end(), [&](const auto& range) {
            return !preservesTimingAudioInRange(targetGrid, *result.grid, range);
        }))
        return rejectProtectedTiming();
    return result;
}

} // namespace OpenTune
