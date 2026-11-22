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
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
        commands_->replaceTimeGridIfCurrent(it->key, it->after, it->before);
}

void TimeGridEditAction::redo()
{
    for (const auto& change : changes_)
        commands_->replaceTimeGridIfCurrent(change.key, change.before, change.after);
}

bool TimeGridEditAction::dependsOnContent(ContentKey key) const noexcept
{
    return std::any_of(changes_.begin(), changes_.end(), [key](const auto& change) {
        return change.key == key;
    });
}

} // namespace OpenTune
