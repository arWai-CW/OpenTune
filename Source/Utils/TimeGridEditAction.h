#pragma once

#include "UndoManager.h"
#include "TimeGrid.h"
#include "Content/ContentKey.h"
#include <memory>

namespace OpenTune {

class ContentEditCommands;

struct TimeGridChange
{
    ContentKey key;
    std::shared_ptr<const TimeGridSnapshot> before;
    std::shared_ptr<const TimeGridSnapshot> after;
};

class TimeGridEditAction : public UndoAction {
public:
    TimeGridEditAction(std::shared_ptr<ContentEditCommands> commands,
                       juce::String description, std::vector<TimeGridChange> changes);

    void undo() override;
    void redo() override;
    juce::String getDescription() const override { return description_; }
    bool dependsOnContent(ContentKey key) const noexcept override;

private:
    std::shared_ptr<ContentEditCommands> commands_;
    juce::String                    description_;
    std::vector<TimeGridChange> changes_;
};

} // namespace OpenTune
