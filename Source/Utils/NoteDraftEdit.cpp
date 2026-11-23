#include "NoteDraftEdit.h"

#include "../Content/ContentPatchGeometry.h"
#include <algorithm>

namespace OpenTune {

namespace {

template <typename AddRange>
void forEachChangedNoteRange(const std::vector<Note>& baselineNotes,
                            const std::vector<Note>& workingNotes,
                            AddRange addRange)
{
    const auto notesContentEqual = [](const Note& left, const Note& right) {
        return left.endTime == right.endTime
            && left.pitch == right.pitch
            && left.pitchOffset == right.pitchOffset
            && left.retuneSpeed == right.retuneSpeed
            && left.pitchDriftScale == right.pitchDriftScale
            && left.vibratoDepth == right.vibratoDepth
            && left.vibratoRate == right.vibratoRate
            && left.eq == right.eq;
    };

    std::size_t beforeIndex = 0;
    std::size_t workingIndex = 0;
    while (beforeIndex < baselineNotes.size() || workingIndex < workingNotes.size()) {
        const bool hasBefore = beforeIndex < baselineNotes.size();
        const bool hasWorking = workingIndex < workingNotes.size();
        if (hasBefore && hasWorking
            && baselineNotes[beforeIndex].startTime == workingNotes[workingIndex].startTime) {
            if (!notesContentEqual(baselineNotes[beforeIndex], workingNotes[workingIndex]))
                addRange(baselineNotes[beforeIndex].startTime,
                         std::max(baselineNotes[beforeIndex].endTime, workingNotes[workingIndex].endTime));
            ++beforeIndex;
            ++workingIndex;
        } else if (!hasWorking
                   || (hasBefore && baselineNotes[beforeIndex].startTime
                       < workingNotes[workingIndex].startTime)) {
            addRange(baselineNotes[beforeIndex].startTime, baselineNotes[beforeIndex].endTime);
            ++beforeIndex;
        } else {
            addRange(workingNotes[workingIndex].startTime, workingNotes[workingIndex].endTime);
            ++workingIndex;
        }
    }
}

} // namespace

std::vector<F0FrameRange> changedNoteFrameRanges(const std::vector<Note>& baselineNotes,
                                                const std::vector<Note>& workingNotes,
                                                const F0Timeline& timeline)
{
    std::vector<F0FrameRange> ranges;
    if (!timeline.isEmpty())
        forEachChangedNoteRange(baselineNotes, workingNotes, [&](double start, double end) {
            const F0FrameRange range{timeline.exclusiveFrameAt(start), timeline.exclusiveFrameAt(end)};
            if (!range.isEmpty())
                ranges.push_back(range);
        });
    return ranges;
}

NoteDraftEditPlan planNoteDraftEdit(const std::vector<Note>& baselineNotes,
                                    const std::vector<Note>& workingNotes)
{
    NoteDraftEditPlan plan;
    double dirtyStartTime = 1e30;
    double dirtyEndTime = -1e30;
    forEachChangedNoteRange(baselineNotes, workingNotes, [&](double start, double end) {
        dirtyStartTime = std::min(dirtyStartTime, start);
        dirtyEndTime = std::max(dirtyEndTime, end);
    });

    if (dirtyEndTime <= dirtyStartTime)
        return plan;

    plan.status = NoteDraftEditStatus::Ready;
    plan.beforePatch.affectedRange = { dirtyStartTime, dirtyEndTime };
    plan.afterPatch.affectedRange = plan.beforePatch.affectedRange;
    plan.beforePatch.afterNotesInRange = filterNotesToRange(
        baselineNotes, dirtyStartTime, dirtyEndTime);
    plan.afterPatch.afterNotesInRange = filterNotesToRange(
        workingNotes, dirtyStartTime, dirtyEndTime);
    return plan;
}

} // namespace OpenTune
