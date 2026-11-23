#pragma once

#include "PluginProcessor.h"
#include "UI/TransportBarComponent.h"

#include <functional>

namespace OpenTune {

class ParameterPanel;
class PianoRollComponent;
class TopBarComponent;

namespace PluginUI {

class ReferenceEditor : private juce::ARARegionSequenceListener
{
public:
    struct ActivePlacement
    {
        const juce::ARARegionSequence* focusedAraTrack;
        std::optional<PianoRollPlacementIdentity> activePlacementIdentity;
    };

    using ResolveScope = std::function<std::vector<juce::ARAPlaybackRegion*>()>;
    using GetEditorView = std::function<const juce::ARAEditorView*()>;
    using FocusedTrack = std::function<const juce::ARARegionSequence*()>;
    using SyncPlacement = std::function<ActivePlacement()>;

    ReferenceEditor(OpenTuneAudioProcessor&, juce::Component&, ParameterPanel&, PianoRollComponent&,
                    TopBarComponent&, ResolveScope, GetEditorView, FocusedTrack, SyncPlacement);
    ~ReferenceEditor() override;

    void extendTransportMenu(int menuIndex, juce::PopupMenu&);
    void refreshControls();
    void syncOverlay(const juce::ARARegionSequence* focusedTrack, ContentKey activeContent);

private:
    juce::PopupMenu createPitchReferenceMenu();
    bool canAlignTimingToReference() const;
    void setPitchReferenceTrack(juce::ARARegionSequence*);
    void tuneToPitchReference();
    void alignTimingToPitchReference();
    void restoreOriginalClipTiming();
    void didUpdateRegionSequenceProperties(juce::ARARegionSequence*) override;
    void willDestroyRegionSequence(juce::ARARegionSequence*) override;

    OpenTuneAudioProcessor& processorRef_;
    juce::Component& contentRoot_;
    ParameterPanel& parameterPanel_;
    PianoRollComponent& pianoRoll_;
    TopBarComponent& topBar_;
    ResolveScope resolveScope_;
    GetEditorView getEditorView_;
    FocusedTrack focusedTrack_;
    SyncPlacement syncPlacement_;

    UnifiedToolbarButton referenceTrackButton_, tuneToReferenceButton_, alignTimingButton_;
    juce::ARARegionSequence* pitchReferenceTrack_ = nullptr;
    std::unique_ptr<juce::ThreadPool> timingAlignmentWorker_;
    std::unique_ptr<juce::AlertWindow> timingAlignmentPreview_;
    bool timingAlignmentRunning_ = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(ReferenceEditor)
};

} // namespace PluginUI
} // namespace OpenTune
