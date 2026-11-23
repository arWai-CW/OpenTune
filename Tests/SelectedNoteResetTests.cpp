#include "ContentEditCommandsStub.h"
#include "Content/ContentPatchGeometry.h"
#include "Content/EditableContentSnapshot.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using namespace OpenTune;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

constexpr ContentKey key{DomainKind::StandaloneClip, 1};

EditableContentSnapshot makeSnapshot()
{
    EditableContentSnapshot snapshot;
    snapshot.notesRevision = 7;
    Note note;
    note.originalPitch = 216.0f;
    note.pitch = 220.0f;
    note.pitchOffset = 2.0f;
    note.retuneSpeed = 0.8f;
    note.vibratoDepth = 8.0f;
    note.vibratoRate = 5.0f;
    note.pitchDriftScale = 0.2f;
    note.eq = EqSettings{};
    for (const auto& range : std::vector<NoteRangeSeconds>{{0.02, 0.054}, {0.054, 0.10}, {0.10, 0.16}}) {
        note.startTime = range.startSeconds;
        note.endTime = range.endSeconds;
        snapshot.notes.push_back(note);
    }

    PitchCorrectionSegment segment{0, 20, std::vector<float>(20, 250.0f),
                                   PitchCorrectionSegment::Source::HandDraw};
    for (int f = 0; f < 20; ++f)
        segment.baseF0Data.push_back(220.0f + static_cast<float>(f));
    snapshot.pitchCurve = std::make_shared<PitchCurveSnapshot>(
        std::vector<float>(20, 216.0f), std::vector<float>{}, std::vector<PitchCorrectionSegment>{segment}, 1, 100.0);
    snapshot.volumeEnvelope = AutomationLane::fromSnapshot({{0.0, 2.0f}, {0.08, -3.0f}, {0.19, 4.0f}});
    return snapshot;
}

std::vector<PitchCorrectionSegment> patchSegments(std::vector<PitchCorrectionSegment> segments,
                                                 const std::vector<PitchCorrectionSegment>& replacement,
                                                 const std::vector<ContentEditRangeFrames>& ranges)
{
    for (const auto& range : ranges)
        segments = replaceSegmentsInFrameRange(
            segments, clipSegmentsToFrameRange(replacement, range.startFrame, range.endFrameExclusive),
            range.startFrame, range.endFrameExclusive);
    return segments;
}

class RecordingCommands final : public TestSupport::ContentEditCommandsStub
{
public:
    explicit RecordingCommands(EditableContentSnapshot snapshot) : state(std::move(snapshot)) {}

    ContentCommitSnapshot commitNotesAndSegmentsInRanges(
        ContentKey content, std::vector<Note> notes, std::vector<PitchCorrectionSegment> segments,
        const std::vector<ContentEditRangeFrames>& ranges) override
    {
        require(content == key, "reset replay targeted the wrong content");
        auto curve = PitchCurve::fromSnapshot(state.pitchCurve);
        curve->replaceCorrectionSegments(
            patchSegments(state.pitchCurve->getCorrectionSegments(), segments, ranges));
        state.pitchCurve = curve->getSnapshot();
        state.notes = std::move(notes);
        return std::make_shared<EditableContentSnapshot>(state);
    }

    ContentCommitSnapshot commitVolumeEnvelope(ContentKey content, AutomationLane envelope) override
    {
        require(content == key, "gain replay targeted the wrong content");
        state.volumeEnvelope = std::move(envelope);
        return std::make_shared<EditableContentSnapshot>(state);
    }

    EditableContentSnapshot state;
};

void checkReset()
{
    const auto before = makeSnapshot();
    auto plan = planSelectedNoteReset(before, {2, 0, 2}, 7);
    require(plan.pitchRanges.size() == 2 && plan.volumeEnvelope.has_value(), "reset needs two disjoint patches");
    require(plan.pitchRanges[0].startFrame == 2 && plan.pitchRanges[0].endFrameExclusive == 6
            && plan.pitchRanges[1].startFrame == 10 && plan.pitchRanges[1].endFrameExclusive == 16,
            "frame ranges must contain only the selected notes' frame timestamps");
    const Note defaults;
    for (auto i : {0, 2}) {
        const auto& note = plan.notes[static_cast<size_t>(i)];
        require(note.pitch == note.originalPitch && note.pitchOffset == 0.0f && !note.eq
                && note.pitchDriftScale == defaults.pitchDriftScale && note.retuneSpeed == defaults.retuneSpeed
                && note.vibratoDepth == defaults.vibratoDepth && note.vibratoRate == defaults.vibratoRate
                && note.startTime == before.notes[static_cast<size_t>(i)].startTime
                && note.endTime == before.notes[static_cast<size_t>(i)].endTime, "selected note did not reset");
    }
    require(plan.notes[1].pitchOffset == before.notes[1].pitchOffset && plan.notes[1].eq.has_value(),
            "reset changed an unselected note's parameters");
    require(std::abs(plan.volumeEnvelope->evalAt(0.035)) < 1e-5f
            && std::abs(plan.volumeEnvelope->evalAt(0.135)) < 1e-5f, "selected gain did not reset");
    for (const double t : {0.01, 0.054, 0.06, 0.075, 0.098, 0.10, 0.19})
        require(std::abs(plan.volumeEnvelope->evalAt(t) - before.volumeEnvelope.evalAt(t)) < 1e-5f,
                "reset changed gain outside selection");

    const auto& beforeSegments = before.pitchCurve->getCorrectionSegments();
    auto commands = std::make_shared<RecordingCommands>(before);
    auto action = std::make_unique<PianoRollEditAction>(
        commands, key, "Restore Selected Notes", before.notes, plan.notes,
        beforeSegments, std::vector<PitchCorrectionSegment>{}, plan.pitchRanges);
    action->setVolumeEnvelopePatch(before.volumeEnvelope, *plan.volumeEnvelope);
    action->redo();
    UndoManager manager;
    manager.addAction(std::move(action));

    const auto& curve = *commands->state.pitchCurve;
    require(!curve.hasCorrectionInRange(2, 6) && !curve.hasCorrectionInRange(10, 16)
            && curve.getOriginalF0() == before.pitchCurve->getOriginalF0(),
            "corrected frames should fall back to the original contour");
    const auto& remaining = curve.getCorrectionSegments();
    require(remaining.size() == 3 && remaining[1].startFrame == 6 && remaining[1].endFrame == 10
            && remaining[1].f0Data == std::vector<float>(4, 250.0f)
            && remaining[1].baseF0Data == std::vector<float>{226.0f, 227.0f, 228.0f, 229.0f},
            "reset changed neighboring correction data when splitting a segment");
    const auto again = planSelectedNoteReset(commands->state, {0, 2}, 7);
    require(again.pitchRanges.empty() && !again.volumeEnvelope, "repeat reset should be a no-op");

    require(manager.undo() != nullptr && !manager.canUndo(), "reset should undo in one step");
    require(commands->state.notes[0].pitchOffset == before.notes[0].pitchOffset
            && commands->state.pitchCurve->hasCorrectionInRange(2, 6)
            && commands->state.volumeEnvelope == before.volumeEnvelope, "undo did not restore the previous edits");
    require(manager.redo() != nullptr && commands->state.notes[2].pitchOffset == 0.0f
            && !commands->state.pitchCurve->hasCorrectionInRange(10, 16)
            && commands->state.volumeEnvelope == *plan.volumeEnvelope, "redo did not restore the reset");
}

void checkSelectionGuards()
{
    auto snapshot = makeSnapshot();
    for (const auto& plan : {planSelectedNoteReset(snapshot, {}, 7),
                             planSelectedNoteReset(snapshot, {0, 99}, 7),
                             planSelectedNoteReset(snapshot, {-1, 0}, 7),
                             planSelectedNoteReset(snapshot, {0, 2}, 6)})
        require(plan.pitchRanges.empty() && !plan.volumeEnvelope, "reset accepted a stale or invalid selection");

    snapshot.pitchCurve.reset();
    const auto unanalyzed = planSelectedNoteReset(snapshot, {0}, 7);
    require(unanalyzed.pitchRanges.empty() && !unanalyzed.volumeEnvelope, "reset accepted unanalyzed audio");
}

} // namespace

int main()
{
    checkReset();
    checkSelectionGuards();
}
