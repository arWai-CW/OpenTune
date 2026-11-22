#include "TimeGridEditAction.h"
#include "Content/ContentEditCommands.h"
#include <algorithm>

namespace OpenTune {

TimeGridEditAction::TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                                       juce::String description, std::vector<TimeGridChange> changes)
    : commands_(std::move(commands)), description_(std::move(description)), changes_(std::move(changes))
{
    jassert(!changes_.empty());
    jassert(std::all_of(changes_.begin(), changes_.end(), [](const auto& change) {
        return change.key.isValid() && change.before != nullptr && change.after != nullptr;
    }));
}

void TimeGridEditAction::undo()
{
    tryReplay(true);
}

void TimeGridEditAction::redo()
{
    tryReplay(false);
}

bool TimeGridEditAction::tryReplay(bool isUndo)
{
    const auto changeAt = [&](std::size_t index) -> const TimeGridChange& {
        return changes_[isUndo ? changes_.size() - 1 - index : index];
    };
    const auto replace = [&](const TimeGridChange& change, bool reverse) {
        return commands_->replaceTimeGridIfCurrent(change.key,
            reverse ? change.after : change.before,
            reverse ? change.before : change.after);
    };
    for (std::size_t i = 0; i < changes_.size(); ++i) {
        if (replace(changeAt(i), isUndo) == TimeGridReplayResult::Conflict) {
            while (i > 0)
                replace(changeAt(--i), !isUndo);
            return false;
        }
    }
    return true;
}

bool TimeGridEditAction::dependsOnContent(ContentKey key) const noexcept
{
    return std::any_of(changes_.begin(), changes_.end(), [key](const auto& change) {
        return change.key == key;
    });
}

} // namespace OpenTune
