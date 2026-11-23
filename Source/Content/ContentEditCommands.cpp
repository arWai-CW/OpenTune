#include "ContentEditCommands.h"
#include "EditableContentSnapshot.h"
#include "../Utils/LegacyNoteGenerator.h"
#include "../Utils/PitchUtils.h"
#include "../Utils/TuningConfig.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace OpenTune {

SelectedNoteResetPlan planSelectedNoteReset(const EditableContentSnapshot& snapshot,
                                            const std::vector<int>& selectedIndices,
                                            uint64_t expectedNotesRevision)
{
    SelectedNoteResetPlan plan;
    if (snapshot.notesRevision != expectedNotesRevision || selectedIndices.empty()
        || !snapshot.hasUsableOriginalF0())
        return plan;

    auto indices = selectedIndices;
    std::sort(indices.begin(), indices.end());
    if (indices.front() < 0 || static_cast<size_t>(indices.back()) >= snapshot.notes.size())
        return plan;
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    const auto& curve = *snapshot.pitchCurve;
    const F0Timeline timeline(curve.getHopSize(), curve.getSampleRate(), static_cast<int>(curve.size()));
    const auto firstFrameAtOrAfter = [&timeline](double seconds) {
        const int nearest = timeline.nearestFrameBoundary(seconds);
        return nearest < timeline.endFrameExclusive() && timeline.timeAtFrame(nearest) + 1e-9 < seconds
            ? nearest + 1 : nearest;
    };

    struct GainRange {
        double startSeconds;
        double endSeconds;
        double rampSeconds;
    };
    std::vector<GainRange> gainRanges;
    const Note defaults;
    for (const int index : indices) {
        const auto& note = snapshot.notes[static_cast<size_t>(index)];
        if (!std::isfinite(note.startTime) || !std::isfinite(note.endTime) || note.endTime <= note.startTime)
            return {};

        const double ramp = std::min(0.005, (note.endTime - note.startTime) * 0.25);
        if (!gainRanges.empty() && gainRanges.back().endSeconds >= note.startTime) {
            gainRanges.back().endSeconds = std::max(gainRanges.back().endSeconds, note.endTime);
            gainRanges.back().rampSeconds = std::min(gainRanges.back().rampSeconds, ramp);
        } else {
            gainRanges.push_back({note.startTime, note.endTime, ramp});
        }

        const int start = firstFrameAtOrAfter(note.startTime);
        const int end = firstFrameAtOrAfter(note.endTime);
        if (end <= start)
            continue;
        const float sungPitch = note.originalPitch > 0.0f ? note.originalPitch : note.pitch;
        if (note.pitch == sungPitch && note.pitchOffset == 0.0f && note.pitchDriftScale == defaults.pitchDriftScale
            && note.retuneSpeed == defaults.retuneSpeed && note.vibratoDepth == defaults.vibratoDepth
            && note.vibratoRate == defaults.vibratoRate && !note.eq
            && !curve.hasCorrectionInRange(start, end))
            continue;

        if (plan.notes.empty())
            plan.notes = snapshot.notes;
        auto& reset = plan.notes[static_cast<size_t>(index)];
        reset.pitch = sungPitch;
        reset.pitchOffset = 0.0f;
        reset.pitchDriftScale = defaults.pitchDriftScale;
        reset.retuneSpeed = defaults.retuneSpeed;
        reset.vibratoDepth = defaults.vibratoDepth;
        reset.vibratoRate = defaults.vibratoRate;
        reset.eq.reset();
        reset.dirty = true;
        if (!plan.pitchRanges.empty() && plan.pitchRanges.back().endFrameExclusive >= start)
            plan.pitchRanges.back().endFrameExclusive = std::max(plan.pitchRanges.back().endFrameExclusive, end);
        else
            plan.pitchRanges.push_back({start, end});
    }

    auto envelope = snapshot.volumeEnvelope;
    if (!envelope.empty()) {
        for (const auto& range : gainRanges) {
            const auto& points = envelope.points();
            const auto first = std::upper_bound(points.begin(), points.end(), range.startSeconds,
                [](double time, const AutomationPoint& point) { return time < point.timeSeconds; });
            const auto last = std::lower_bound(first, points.end(), range.endSeconds,
                [](const AutomationPoint& point, double time) { return point.timeSeconds < time; });
            if (envelope.evalAt(range.startSeconds) == 0.0f && envelope.evalAt(range.endSeconds) == 0.0f
                && std::all_of(first, last, [](const auto& point) { return point.gainDb == 0.0f; }))
                continue;
            // Preserve neighboring gain and leave every selected note's midpoint on the plateau.
            const double ramp = range.rampSeconds;
            envelope.setRegionGain(range.startSeconds + ramp, range.endSeconds - ramp, 0.0f, ramp);
        }
        if (!(envelope == snapshot.volumeEnvelope))
            plan.volumeEnvelope = std::move(envelope);
    }
    return plan;
}

bool ContentEditCommands::canGenerateNotesOnly(const EditableContentSnapshot& snapshot)
{
    return snapshot.originalF0State == OriginalF0State::Ready && snapshot.hasUsableOriginalF0();
}

std::optional<ContentNoteRangePatch> ContentEditCommands::planGenerateNotesOnly(
    const EditableContentSnapshot& snapshot, const NoteGeneratorParams& params)
{
    if (!canGenerateNotesOnly(snapshot))
        return std::nullopt;

    const auto& curve = *snapshot.pitchCurve;
    const auto& f0 = curve.getOriginalF0();
    const int count = static_cast<int>(f0.size());
    auto generated = LegacyNoteGenerator::generate(
        f0.data(), count, nullptr, 0, count, curve.getHopSize(), curve.getSampleRate(), params);
    if (!LegacyNoteGenerator::validate(generated))
        return std::nullopt;
    for (auto& note : generated)
        if (note.originalPitch > 0.0f)
            note.pitch = note.originalPitch;

    const F0Timeline timeline{curve.getHopSize(), curve.getSampleRate(), count};
    ContentNoteRangePatch patch;
    patch.affectedRange = {0.0, timeline.timeAtFrame(count)};
    for (const auto& note : generated) {
        patch.affectedRange.startSeconds = std::min(patch.affectedRange.startSeconds, note.startTime);
        patch.affectedRange.endSeconds = std::max(patch.affectedRange.endSeconds, note.endTime);
    }
    patch.afterNotesInRange = std::move(generated);
    return patch;
}

std::optional<ContentNoteRangePatch> ContentEditCommands::planInitialNotesOnly(
    const EditableContentSnapshot& snapshot, const NoteGeneratorParams& params)
{
    if (snapshot.noteTopologyInitialized)
        return std::nullopt;
    return planGenerateNotesOnly(snapshot, params);
}

ContentNoteValueEditPlan ContentEditCommands::planNoteValueEdit(
    const std::vector<Note>& notes,
    const ContentNoteValueEditRequest& request) const
{
    ContentNoteValueEditPlan plan;
    plan.notes = notes;
    const float tuningHz = TuningConfig::currentTuningHz();

    const auto setParameter = [](float& field, const std::optional<double>& value) {
        if (!value)
            return false;
        const float target = static_cast<float>(*value);
        if (std::abs(static_cast<double>(field) - target) <= 1.0e-6)
            return false;
        field = target;
        return true;
    };

    for (const auto& edit : request.edits) {
        const auto position = edit.position;
        if (position >= plan.notes.size())
            continue;
        auto& note = plan.notes[position];
        bool changed = false;
        switch (edit.kind) {
            case ContentNoteValueEdit::Kind::TargetPitch: {
                const float pitch = PitchUtils::midiToFreq(static_cast<float>(edit.targetMidi), tuningHz);
                const double currentMidi = PitchUtils::freqToMidi(note.getAdjustedPitch(), tuningHz);
                const double targetMidi = PitchUtils::freqToMidi(pitch, tuningHz);
                if (note.pitchOffset != 0.0f || std::abs(currentMidi - targetMidi) >= 1.0e-4) {
                    note.pitch = pitch;
                    note.pitchOffset = 0.0f;
                    changed = true;
                }
                break;
            }
            case ContentNoteValueEdit::Kind::ShiftCents:
                if (std::abs(edit.deltaCents) > 1.0e-6) {
                    note.pitchOffset += static_cast<float>(edit.deltaCents / 100.0);
                    changed = true;
                }
                break;
            case ContentNoteValueEdit::Kind::SetPitchOffset:
                if (std::abs(note.pitchOffset - edit.pitchOffset) > 1.0e-3) {
                    note.pitchOffset = static_cast<float>(edit.pitchOffset);
                    changed = true;
                }
                break;
            case ContentNoteValueEdit::Kind::Parameters: {
                const auto retune = edit.retuneSpeed
                    ? std::optional<double>(std::clamp(*edit.retuneSpeed, 0.0, 1.0))
                    : std::nullopt;
                const auto depth = edit.vibratoDepth
                    ? std::optional<double>(std::clamp(*edit.vibratoDepth, 0.0, 100.0))
                    : std::nullopt;
                const auto rate = edit.vibratoRate
                    ? std::optional<double>(std::clamp(*edit.vibratoRate, 3.0, 12.0))
                    : std::nullopt;
                changed = setParameter(note.retuneSpeed, retune) || changed;
                changed = setParameter(note.vibratoDepth, depth) || changed;
                changed = setParameter(note.vibratoRate, rate) || changed;
                changed = setParameter(note.pitchDriftScale, edit.pitchDriftScale) || changed;
                break;
            }
        }
        if (changed) {
            note.dirty = true;
            plan.changedPositions.push_back(position);
        }
    }
    return plan;
}

} // namespace OpenTune
