#include "UI/PianoRollComponent.h"

#include "Content/ContentEditCommands.h"
#include "Reference/PitchReferenceSnapshot.h"
#include "UI/PianoRoll/PianoRollToolHandler.h"
#include "Utils/LocalizationManager.h"

namespace OpenTune {

PianoRollComponent::AutoTuneApplyResult PianoRollComponent::tuneToPitchReference()
{
    if (contentCommands_ == nullptr || editedContentKey_.domainKind != DomainKind::ARAAudioModification)
        return { AutoTuneApplyStatus::NoContent };

    const auto snapshot = readEditedSnapshot();
    if (snapshot == nullptr || snapshot->originalF0State != OriginalF0State::Ready
        || !snapshot->hasUsableOriginalF0())
        return { AutoTuneApplyStatus::OriginalF0NotReady };

    if (pitchReferences_.empty())
        return { AutoTuneApplyStatus::NoAnalyzedReferencePitch };

    std::vector<F0FrameRange> ranges;
    const auto selection = toolHandler_->collectSelectedFrameRanges();
    for (const auto& [start, end] : selection.ranges)
        ranges.push_back({start, end});
    if (ranges.empty())
        ranges.push_back({0, static_cast<int>(snapshot->pitchCurve->size())});

    auto plan = planPitchReferenceCorrection(
        {activeContentProjection(), snapshot->timeGrid, snapshot->pitchCurve,
         static_cast<float>(snapshot->pitchShiftSettings.getPitchRatio()), snapshot->notes},
        pitchReferences_, ranges);
    if (!plan.hasClipOverlap)
        return { AutoTuneApplyStatus::NoReferenceClipOverlap };
    if (!plan.hasVoicedOverlap)
        return { AutoTuneApplyStatus::NoVoicedReferenceOverlap };
    if (plan.segments.empty())
        return { AutoTuneApplyStatus::NoChange };

    std::vector<ContentEditRangeFrames> affected;
    affected.reserve(plan.segments.size());
    for (const auto& segment : plan.segments)
        affected.push_back({segment.startFrame, segment.endFrame});
    auto edit = contentCommands_->commitNotesAndSegmentsUndoableInRanges(
        editedContentKey_, snapshot->notes, std::move(plan.segments), affected, LOC(kTuneToPitchReference));
    if (edit.snapshot == nullptr)
        return { AutoTuneApplyStatus::NoContent };

    lastKnownNotesRevision_ = edit.snapshot->notesRevision;
    lastKnownPitchRevision_ = edit.snapshot->pitchRevision;
    setEditedContent(editedContentKey_, edit.snapshot->pitchCurve, audioBuffer_, audioBufferSampleRate_);
    pendingUndoDescription_ = {};
    return { AutoTuneApplyStatus::Applied };
}

} // namespace OpenTune
