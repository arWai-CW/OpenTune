#include "ReferenceEditor.h"

#include <algorithm>
#include <cmath>

#include "ARA/OpenTuneDocumentController.h"
#include "Editor/ConfirmDialogContent.h"
#include "PluginProcessor.h"
#include "Reference/PitchReferenceSnapshot.h"
#include "Reference/TimingAlignment.h"
#include "UI/ParameterPanel.h"
#include "UI/PianoRollComponent.h"
#include "UI/ToolbarIcons.h"
#include "UI/TopBarComponent.h"
#include "Utils/AppLogger.h"
#include "Utils/LocalizationManager.h"
#include "Utils/ScreenReaderAnnouncer.h"
#include "Utils/TimeGridEditAction.h"

namespace OpenTune::PluginUI {

ReferenceEditor::ReferenceEditor(OpenTuneAudioProcessor& processor, juce::Component& contentRoot,
    ParameterPanel& parameterPanel, PianoRollComponent& pianoRoll, TopBarComponent& topBar,
    ResolveScope resolveScope, GetEditorView getEditorView, FocusedTrack focusedTrack, SyncPlacement syncPlacement)
    : processorRef_(processor), contentRoot_(contentRoot), parameterPanel_(parameterPanel),
      pianoRoll_(pianoRoll), topBar_(topBar), resolveScope_(std::move(resolveScope)),
      getEditorView_(std::move(getEditorView)), focusedTrack_(std::move(focusedTrack)),
      syncPlacement_(std::move(syncPlacement)),
      referenceTrackButton_(LOC(kReferenceTrack), ToolbarIcons::getTracksIcon()),
      tuneToReferenceButton_(LOC(kTuneToPitchReference), ToolbarIcons::getAutoTuneIcon()),
      alignTimingButton_(LOC(kAlignTimingToReference), ToolbarIcons::getTimeToolIcon())
{
    referenceTrackButton_.onClick = [this] {
        auto menu = createPitchReferenceMenu();
        if (menu.getNumItems() <= 1 && pitchReferenceTrack_ == nullptr) {
            menu.clear();
            menu.addItem(LOC_RAW("No other track with audio clips in this ARA document."), false, false, [] {});
        }
        auto options = juce::PopupMenu::Options().withTargetComponent(&referenceTrackButton_)
                           .withParentComponent(&contentRoot_);
        if (const int firstId = a11y::firstSelectableItemId(menu); firstId != 0)
            options = options.withInitiallySelectedItem(firstId);
        menu.showMenuAsync(options);
    };
    tuneToReferenceButton_.onClick = [this] { tuneToPitchReference(); };
    alignTimingButton_.onClick = [this] { alignTimingToPitchReference(); };
    for (auto* button : { &referenceTrackButton_, &tuneToReferenceButton_, &alignTimingButton_ })
        topBar_.addToolAction(*button);
    refreshControls();
}

ReferenceEditor::~ReferenceEditor()
{
    masterReference.clear();
    timingAlignmentPreview_.reset();
    timingAlignmentWorker_.reset();
    setPitchReferenceTrack(nullptr);
}

void ReferenceEditor::extendTransportMenu(int menuIndex, juce::PopupMenu& menu)
{
    const juce::WeakReference<ReferenceEditor> safeEditor(this);
    if (menuIndex == 2) {
        auto references = createPitchReferenceMenu();
        if (references.getNumItems() > 1 || pitchReferenceTrack_ != nullptr)
            menu.addSubMenu(LOC(kReferenceTrack), references);
    }
    else if (menuIndex == 1 && processorRef_.getDocumentController() != nullptr) {
        menu.addSeparator();
        menu.addItem(LOC(kTuneToPitchReference), pitchReferenceTrack_ != nullptr, false, [safeEditor] {
            if (safeEditor != nullptr)
                safeEditor->tuneToPitchReference();
        });
        menu.addItem(LOC(kAlignTimingToReference), canAlignTimingToReference(), false, [safeEditor] {
            if (safeEditor != nullptr)
                safeEditor->alignTimingToPitchReference();
        });
        menu.addItem(LOC_RAW("Restore Original Clip Timing"), true, false, [safeEditor] {
            if (safeEditor != nullptr)
                safeEditor->restoreOriginalClipTiming();
        });
    }
}

namespace {
juce::String referenceTrackName(const juce::ARARegionSequence& track)
{
    const auto name = juce::String::fromUTF8(track.getName());
    return name.isEmpty() ? LOC(kUnnamedReferenceTrack) : name;
}

std::vector<TimingOutputRange> protectedTimingRanges(
    const juce::ARAPlaybackRegion& placement, double sourceOrigin)
{
    std::vector<TimingOutputRange> ranges;
    for (const auto* sibling : placement.getAudioModification()->getPlaybackRegions()) {
        if (sibling != &placement)
            ranges.push_back({sibling->getStartInAudioModificationTime() - sourceOrigin,
                              sibling->getEndInAudioModificationTime() - sourceOrigin});
    }
    return ranges;
}

std::vector<PitchReferenceSnapshot> readPitchReferences(
    const OpenTuneDocumentController& dc, const juce::ARARegionSequence& track, ContentKey editedContent)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    std::vector<PitchReferenceSnapshot> references;
    for (const auto& region : dc.getPlaybackRegionProjectionsFor(track.getPlaybackRegions())) {
        if (!region.contentProjection.isValid() || region.contentKey == editedContent)
            continue;

        const auto content = dc.readContentSnapshot(region.contentKey);
        if (content == nullptr || content->timeGrid == nullptr
            || content->pitchCurve == nullptr || content->pitchCurve->isEmpty())
            continue;

        references.push_back({region.contentProjection, content->timeGrid, content->pitchCurve,
            static_cast<float>(content->pitchShiftSettings.getPitchRatio())});
    }
    return references;
}

} // namespace

juce::PopupMenu ReferenceEditor::createPitchReferenceMenu()
{
    juce::PopupMenu references;
    auto* dc = processorRef_.getDocumentController();
    auto* document = dc != nullptr ? dc->getDocument() : nullptr;
    if (document == nullptr)
        return references;

    const auto focused = focusedTrack_();
    const juce::WeakReference<ReferenceEditor> safeThis(this);
    references.addItem(LOC(kOff), true, pitchReferenceTrack_ == nullptr, [safeThis] {
        if (safeThis != nullptr)
            safeThis->setPitchReferenceTrack(nullptr);
    });
    int index = 0;
    for (auto* track : document->getRegionSequences()) {
        ++index;
        if (track == focused || track->getPlaybackRegions().empty())
            continue;

        references.addItem(juce::String(index) + ". " + referenceTrackName(*track), true, track == pitchReferenceTrack_,
            [safeThis, track] {
                if (safeThis == nullptr)
                    return;
                auto* current = safeThis->processorRef_.getDocumentController();
                if (auto* document = current != nullptr ? current->getDocument() : nullptr) {
                    const auto& tracks = document->getRegionSequences();
                    if (std::find(tracks.begin(), tracks.end(), track) != tracks.end())
                        safeThis->setPitchReferenceTrack(track);
                }
            });
    }
    return references;
}

bool ReferenceEditor::canAlignTimingToReference() const
{
    return pitchReferenceTrack_ != nullptr && !timingAlignmentRunning_ && timingAlignmentPreview_ == nullptr;
}

void ReferenceEditor::refreshControls()
{
    const bool visible = processorRef_.getDocumentController() != nullptr;
    topBar_.setToolActionsVisible(visible);
    if (visible) {
        const bool hasReference = pitchReferenceTrack_ != nullptr;
        const bool canAlign = canAlignTimingToReference();
        const auto referenceTitle = LOC(kReferenceTrack) + ": "
            + (hasReference ? referenceTrackName(*pitchReferenceTrack_) : LOC(kOff));
        const auto tuneTitle = LOC(kTuneToPitchReference);
        const auto alignTitle = LOC(kAlignTimingToReference);
        referenceTrackButton_.setToolbarLabel(LOC(kReferenceToolbarLabel));
        tuneToReferenceButton_.setToolbarLabel(LOC(kTuneReferenceToolbarLabel));
        alignTimingButton_.setToolbarLabel(LOC(kAlignReferenceToolbarLabel));
        referenceTrackButton_.setTitle(referenceTitle);
        tuneToReferenceButton_.setTitle(tuneTitle);
        alignTimingButton_.setTitle(alignTitle);

        const auto chooseReference = LOC_RAW("Choose a reference track first.");
        referenceTrackButton_.setTooltip(referenceTitle);
        referenceTrackButton_.setToggleState(hasReference, juce::dontSendNotification);
        tuneToReferenceButton_.setEnabled(hasReference);
        tuneToReferenceButton_.setAlpha(hasReference ? 1.0f : 0.55f);
        tuneToReferenceButton_.setTooltip(hasReference ? tuneTitle : chooseReference);
        alignTimingButton_.setEnabled(canAlign);
        alignTimingButton_.setAlpha(canAlign ? 1.0f : 0.55f);
        alignTimingButton_.setTooltip(!hasReference ? chooseReference
            : timingAlignmentRunning_ ? LOC_RAW("Analyzing timing") : alignTitle);
    }
}

void ReferenceEditor::setPitchReferenceTrack(juce::ARARegionSequence* track)
{
    if (pitchReferenceTrack_ == track)
        return;
    if (pitchReferenceTrack_ != nullptr)
        pitchReferenceTrack_->removeListener(this);
    pitchReferenceTrack_ = track;
    if (pitchReferenceTrack_ != nullptr)
        pitchReferenceTrack_->addListener(this);
    pianoRoll_.setPitchReferences({});
    refreshControls();
    if (auto* dc = processorRef_.getDocumentController())
        dc->presentationChanges().sendChangeMessage();
}

void ReferenceEditor::tuneToPitchReference()
{
    parameterPanel_.commitPendingEdits();
    const auto sync = syncPlacement_();
    const auto title = LOC(kTuneToPitchReference);
    if (pitchReferenceTrack_ != nullptr && pitchReferenceTrack_ == sync.focusedAraTrack) {
        OpenTune::ConfirmDialogContent::showMessage(&contentRoot_, title,
            LOC_RAW("Select the clip you want to tune in the DAW, not the reference track."));
        return;
    }
    const auto result = pianoRoll_.tuneToPitchReference();
    if (result.applied())
        a11y::announce(pianoRoll_, title);
    else if (result.status == PianoRollComponent::AutoTuneApplyStatus::NoChange)
        OpenTune::ConfirmDialogContent::showMessage(&contentRoot_, title,
            LOC_RAW("The selected pitch already matches the reference."));
    else {
        auto detail = result.message();
        if (sync.focusedAraTrack != nullptr && sync.activePlacementIdentity) {
            detail += "\n\n" + Loc::format(LOC_RAW("Active clip: {0} @ {1} s"),
                referenceTrackName(*sync.focusedAraTrack),
                juce::String(sync.activePlacementIdentity->projection.timelineStartSeconds, 2));
        }
        OpenTune::ConfirmDialogContent::showMessage(&contentRoot_, title, detail);
    }
}

void ReferenceEditor::restoreOriginalClipTiming()
{
    const auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr)
        return;
    parameterPanel_.commitPendingEdits();
    const auto regions = dc->getPlaybackRegionProjectionsFor(resolveScope_());
    const auto focused = std::find_if(regions.begin(), regions.end(), [](const auto& region) {
        return region.contentKey.isValid() && region.contentProjection.isValid();
    });
    const auto title = LOC_RAW("Restore Original Clip Timing");
    const auto current = focused != regions.end() && focused->isPlaybackRenderable()
        ? dc->readContentSnapshot(focused->contentKey) : nullptr;
    if (!current || !current->timeGrid) {
        ConfirmDialogContent::showMessage(&contentRoot_, title, LOC_RAW("Select a clip with loaded audio."));
        return;
    }

    const auto& projection = focused->contentProjection;
    const TimingOutputRange range{projection.contentStartSeconds,
                                  projection.contentStartSeconds + projection.contentDurationSeconds};
    if (current->timeGrid->isIdentityInOutputRange(range.begin, range.end)) {
        ConfirmDialogContent::showMessage(&contentRoot_, title, LOC_RAW("This clip is already at its original timing."));
        return;
    }
    const auto restored = restoreOriginalTimingInRange(*current->timeGrid, range,
        protectedTimingRanges(*focused->playbackRegion, current->sourceWindow.sourceStartSeconds));
    if (!restored || !processorRef_.applyTimeGridEdit(focused->contentKey, current->timeGrid, restored, title)) {
        ConfirmDialogContent::showMessage(&contentRoot_, title,
            LOC_RAW("This clip's timing cannot be restored without changing audio outside it or in another clip."));
        return;
    }

    pianoRoll_.onTimeGridRevisionChanged();
    a11y::announce(pianoRoll_, title);
}

void ReferenceEditor::alignTimingToPitchReference()
{
    auto* dc = processorRef_.getDocumentController();
    if (dc == nullptr || !canAlignTimingToReference())
        return;

    using Region = OpenTuneDocumentController::PlaybackRegionProjection;
    struct Input
    {
        Region region;
        std::shared_ptr<const EditableContentSnapshot> snapshot;
    };
    struct Target
    {
        Input input;
        size_t guideIndex;
        juce::String label;
        std::vector<TimingOutputRange> protectedRanges;
        std::vector<TimingOnset> onsets, guideOnsets;
        TimingAlignment gentle, tight;
    };

    const auto protectedRangesFor = [](const Input& input) {
        return protectedTimingRanges(*input.region.playbackRegion, input.snapshot->sourceWindow.sourceStartSeconds);
    };
    const auto makePlan = [](const Target& target, const Input& guide, const TimeGridSnapshot& grid,
                             double strength, const std::vector<TimingOutputRange>& protectedRanges) {
        return alignTimingToReference(target.onsets, grid, target.input.region.contentProjection,
            target.guideOnsets, *guide.snapshot->timeGrid, guide.region.contentProjection, strength, protectedRanges);
    };

    parameterPanel_.commitPendingEdits();
    const auto load = [dc](const Region& region, juce::String& rejection) -> Input {
        if (!region.contentProjection.isValid()) {
            rejection = "no content projection (audio not loaded or invalid placement)";
            return {};
        }
        if (!region.isPlaybackRenderable()) {
            rejection = "unsupported playback placement (timeline " + juce::String(region.durationInPlaybackTime, 3)
                + " s, source " + juce::String(region.durationInModificationTime, 3) + " s)";
            return {};
        }
        auto snapshot = dc->readContentSnapshot(region.contentKey);
        if (!snapshot || !snapshot->audioBuffer || !snapshot->timeGrid || snapshot->timeGrid->empty()) {
            rejection = "audio or timing grid not loaded";
            return {};
        }
        return {region, std::move(snapshot)};
    };
    const auto guideRegions = dc->getPlaybackRegionProjectionsFor(pitchReferenceTrack_->getPlaybackRegions());
    std::vector<juce::ARAPlaybackRegion*> selected;
    if (const auto* view = getEditorView_())
        selected = view->getViewSelection().getPlaybackRegions<juce::ARAPlaybackRegion>();
    if (selected.empty()) {
        const auto scoped = resolveScope_();
        if (!scoped.empty())
            selected.push_back(scoped.front());
    }
    selected.erase(std::remove_if(selected.begin(), selected.end(), [this](auto* region) {
        return region == nullptr || region->getRegionSequence() == pitchReferenceTrack_;
    }), selected.end());

    const auto title = LOC(kAlignTimingToReference);
    if (selected.empty() || selected.size() > 6) {
        const auto reason = "Selection: " + juce::String(static_cast<int>(selected.size())) + " target clips (need 1–6)";
        ConfirmDialogContent::showDiagnostic(&contentRoot_, title,
            LOC_RAW("Select one to six target clips in the DAW, and choose a separate reference track.") + "\n" + reason,
            reason);
        return;
    }

    juce::StringArray diagnostics;
    std::vector<Input> guides;
    std::vector<Target> targets;
    const auto targetRegions = dc->getPlaybackRegionProjectionsFor(selected);
    if (targetRegions.size() != selected.size())
        diagnostics.add("Placement: " + juce::String(static_cast<int>(targetRegions.size())) + "/"
            + juce::String(static_cast<int>(selected.size())) + " selected targets have a valid DAW placement");
    for (const auto& region : targetRegions) {
        const auto position = "Target @ " + juce::String(region.startInPlaybackTime, 2) + " s: ";
        juce::String rejection;
        const auto input = load(region, rejection);
        if (!input.snapshot) {
            diagnostics.add(position + rejection);
            continue;
        }
        Input bestGuide;
        double bestOverlap = 0.0;
        double maxOverlap = 0.0;
        juce::String referenceRejection;
        constexpr double maxTimingAnalysisSeconds = 180.0;
        for (const auto& guide : guideRegions) {
            const double overlap = std::min(region.endInPlaybackTime, guide.endInPlaybackTime)
                - std::max(region.startInPlaybackTime, guide.startInPlaybackTime);
            if (guide.contentKey == region.contentKey)
                continue;
            const auto previousMax = maxOverlap;
            maxOverlap = std::max(maxOverlap, overlap);
            if (overlap < minimumTimingOverlapSeconds || overlap <= bestOverlap)
                continue;
            if (overlap > maxTimingAnalysisSeconds) {
                if (overlap >= previousMax)
                    referenceRejection = "overlap exceeds the " + juce::String(static_cast<int>(maxTimingAnalysisSeconds))
                        + " s timing analysis limit";
                continue;
            }
            if (auto candidate = load(guide, rejection); candidate.snapshot) {
                bestGuide = std::move(candidate);
                bestOverlap = overlap;
            } else if (overlap >= previousMax) {
                referenceRejection = rejection;
            }
        }
        if (!bestGuide.snapshot) {
            const auto reason = referenceRejection.isNotEmpty() ? referenceRejection
                : juce::String("no eligible reference clip with enough overlap");
            diagnostics.add(position + "reference: " + reason + " (max overlap "
                + juce::String(maxOverlap, 3) + " s; need "
                + juce::String(minimumTimingOverlapSeconds, 1) + " s; "
                + juce::String(static_cast<int>(guideRegions.size())) + " reference clips)");
            continue;
        }
        const auto existing = std::find_if(guides.begin(), guides.end(), [&](const auto& guide) {
            return guide.region.playbackRegion == bestGuide.region.playbackRegion;
        });
        const auto guideIndex = static_cast<size_t>(existing - guides.begin());
        if (existing == guides.end())
            guides.push_back(std::move(bestGuide));
        const auto* track = region.playbackRegion->getRegionSequence();
        auto name = juce::String::fromUTF8(track != nullptr ? static_cast<const char*>(track->getName()) : nullptr);
        if (name.isEmpty())
            name = LOC(kUnnamedReferenceTrack);
        name += " @ " + juce::String(region.startInPlaybackTime, 2) + " s";
        targets.push_back({input, guideIndex, std::move(name), protectedRangesFor(input), {}, {}, {}, {}});
    }
    const auto sharedTargetHelp = juce::PluginHostType{}.isReaper()
        ? Loc::format(LOC_RAW("Some targets share ARA edits. Select those items in REAPER, run this action, then retry: {0}"),
                      juce::String("Item: Remove active takes from ARA edit pool (unpool)"))
        : LOC_RAW("Some targets share ARA edits. Make those clips independent in the DAW, then retry.");
    const auto noAlignment = LOC_RAW("No reliable timing changes.");
    const auto showFailure = [title, noAlignment, sharedTargetHelp](juce::Component& parent,
                                                                 const juce::StringArray& details, bool sharedConflict) {
        auto summary = details.isEmpty() ? noAlignment : noAlignment + "\n" + details[0];
        auto copied = details.joinIntoString("\n");
        if (sharedConflict) {
            summary += "\n\n" + sharedTargetHelp;
            copied += "\n" + sharedTargetHelp;
        }
        ConfirmDialogContent::showDiagnostic(&parent, title, summary, copied);
    };
    if (targets.empty()) {
        showFailure(contentRoot_, diagnostics, false);
        return;
    }

    if (!timingAlignmentWorker_)
        timingAlignmentWorker_ = std::make_unique<juce::ThreadPool>(1);
    timingAlignmentRunning_ = true;
    refreshControls();
    a11y::announce(pianoRoll_, LOC_RAW("Analyzing timing"));
    const juce::WeakReference<ReferenceEditor> safe(this);
    timingAlignmentWorker_->addJob([safe, guideTrack = pitchReferenceTrack_, count = selected.size(),
        guides = std::move(guides), targets = std::move(targets), title, showFailure, sharedTargetHelp,
        protectedRangesFor, makePlan,
        diagnostics = std::move(diagnostics)]() mutable {
        const auto detect = [](const Input& input, const Input& other) {
            return detectTimingOnsetsInOverlap(*input.snapshot->audioBuffer, input.snapshot->audioSampleRate,
                *input.snapshot->timeGrid, input.region.contentProjection, other.region.contentProjection);
        };
        bool sharedConflict = false;
        try {
            for (auto& target : targets) {
                const auto& guide = guides[target.guideIndex];
                target.onsets = detect(target.input, guide);
                target.guideOnsets = detect(guide, target.input);
                const auto plan = [&](double strength) {
                    return makePlan(target, guide, *target.input.snapshot->timeGrid, strength, target.protectedRanges);
                };
                target.gentle = plan(0.5);
                target.tight = plan(1.0);
                if (!target.gentle.grid && !target.tight.grid) {
                    const auto& a = target.tight;
                    sharedConflict = sharedConflict || a.affectsProtectedTiming || target.gentle.affectsProtectedTiming;
                    const auto& targetRegion = target.input.region;
                    const double overlap = std::min(targetRegion.endInPlaybackTime, guide.region.endInPlaybackTime)
                        - std::max(targetRegion.startInPlaybackTime, guide.region.startInPlaybackTime);
                    const auto stage = [](const TimingAlignment& result) {
                        return juce::String(result.alreadyAligned ? "already aligned" : result.rejection);
                    };
                    const auto tight = stage(a);
                    const auto gentle = stage(target.gentle);
                    const auto reason = tight == gentle ? tight : tight + " (tight), " + gentle + " (gentle)";
                    diagnostics.add("Target @ " + juce::String(targetRegion.startInPlaybackTime, 2)
                        + " s: " + reason + "; overlap "
                        + juce::String(overlap, 3) + " s, attacks (target/ref) "
                        + juce::String(static_cast<int>(a.targetAttacks)) + "/"
                        + juce::String(static_cast<int>(a.referenceAttacks)) + ", matched "
                        + juce::String(static_cast<int>(a.matchedAttacks)));
                }
            }
        } catch (const std::exception& error) {
            AppLogger::warn("Timing alignment: " + juce::String(error.what()));
            diagnostics.add("Analysis could not finish; see the OpenTune log.");
            targets.clear();
        }
        const bool alreadyAligned = targets.size() == count
            && std::all_of(targets.begin(), targets.end(), [](const auto& target) {
                return target.tight.alreadyAligned;
            });
        targets.erase(std::remove_if(targets.begin(), targets.end(), [](const auto& target) {
            return !target.gentle.grid && !target.tight.grid;
        }), targets.end());
        juce::MessageManager::callAsync([safe, guideTrack, count, guides = std::move(guides),
                targets = std::move(targets), title, showFailure, sharedTargetHelp,
                protectedRangesFor, makePlan, diagnostics = std::move(diagnostics), alreadyAligned, sharedConflict]() mutable {
            if (safe == nullptr)
                return;
            safe->timingAlignmentRunning_ = false;
            if (safe->pitchReferenceTrack_ != guideTrack) {
                safe->refreshControls();
                return;
            }
            if (targets.empty()) {
                safe->refreshControls();
                if (alreadyAligned)
                    ConfirmDialogContent::showMessage(&safe->contentRoot_, title,
                        LOC_RAW("The matched attacks are already aligned. No timing change was applied."));
                else
                    showFailure(safe->contentRoot_, diagnostics, sharedConflict);
                return;
            }

            auto detail = LOC_RAW("Choose alignment strength per clip. Pitch and the reference track are not edited.");
            if (count != targets.size())
                detail += "\n" + Loc::format(LOC_RAW("{0} selected clips have no safe alignment and will be skipped."),
                                             juce::String(static_cast<int>(count - targets.size())));
            if (sharedConflict)
                detail += "\n" + sharedTargetHelp;
            auto preview = std::make_unique<juce::AlertWindow>(title, detail,
                juce::MessageBoxIconType::NoIcon, &safe->contentRoot_);
            const auto label = [](const juce::String& mode, const TimingAlignment& plan) {
                return Loc::format(LOC_RAW("{0} — {1} anchors, {2} ms max"), mode, juce::String(plan.movedAnchors),
                    juce::String(static_cast<int>(std::lround(plan.maxMoveSeconds * 1000))));
            };
            for (size_t i = 0; i < targets.size(); ++i) {
                const auto id = juce::String(static_cast<int>(i));
                preview->addComboBox(id, {}, targets[i].label);
                auto* box = preview->getComboBoxComponent(id);
                box->addItem(LOC_RAW("Skip"), 1);
                if (targets[i].gentle.grid)
                    box->addItem(label(LOC(kTimingGentle), targets[i].gentle), 2);
                if (targets[i].tight.grid)
                    box->addItem(label(LOC(kTimingTight), targets[i].tight), 3);
                box->setSelectedId(targets[i].gentle.grid ? 2 : 1, juce::dontSendNotification);
            }
            preview->addButton(LOC_RAW("Apply"), 1, juce::KeyPress(juce::KeyPress::returnKey));
            preview->addButton(LOC(kCancel), 0, juce::KeyPress(juce::KeyPress::escapeKey));
            safe->timingAlignmentPreview_ = std::move(preview);
            safe->refreshControls();
            auto& window = *safe->timingAlignmentPreview_;
            const auto bounds = safe->contentRoot_.getLocalBounds();
            window.setCentrePosition(bounds.getCentreX(), bounds.getCentreY());
            safe->contentRoot_.addChildComponent(window);
            window.enterModalState(true, juce::ModalCallbackFunction::create(
                [safe, guideTrack, guides = std::move(guides), targets = std::move(targets), title,
                 showFailure, protectedRangesFor, makePlan](int choice) {
                    if (safe == nullptr)
                        return;
                    const auto dialog = std::move(safe->timingAlignmentPreview_);
                    safe->refreshControls();
                    if (choice != 1 || !dialog)
                        return;
                    auto* dc = safe->processorRef_.getDocumentController();
                    const auto unchanged = [dc](const Input& input) {
                        if (dc == nullptr)
                            return false;
                        const auto current = dc->getPlaybackRegionProjectionsFor({input.region.playbackRegion});
                        const PianoRollPlacementIdentity identity{input.region.contentKey, input.region.contentProjection};
                        if (current.size() != 1
                            || !(identity == PianoRollPlacementIdentity{current[0].contentKey, current[0].contentProjection}))
                            return false;
                        const auto snapshot = dc->readContentSnapshot(input.region.contentKey);
                        return snapshot && snapshot->audioBuffer == input.snapshot->audioBuffer
                            && snapshot->timeGridRevision == input.snapshot->timeGridRevision;
                    };
                    const auto stale = [&] {
                        ConfirmDialogContent::showMessage(&safe->contentRoot_, title,
                            LOC_RAW("A clip or the reference changed. Run timing alignment again."));
                    };
                    std::vector<TimeGridChange> changes;
                    for (size_t i = 0; i < targets.size(); ++i) {
                        const auto* box = dialog->getComboBoxComponent(juce::String(static_cast<int>(i)));
                        const auto& target = targets[i];
                        const auto mode = box->getSelectedId();
                        const auto previewGrid = mode == 2 ? target.gentle.grid
                            : mode == 3 ? target.tight.grid : nullptr;
                        if (!previewGrid)
                            continue;
                        const auto& input = target.input;
                        if (safe->pitchReferenceTrack_ != guideTrack || !unchanged(input)
                            || !unchanged(guides[target.guideIndex])) {
                            stale();
                            return;
                        }
                        const auto ranges = protectedRangesFor(input);
                        const auto previous = std::find_if(changes.begin(), changes.end(), [&](const auto& change) {
                            return change.key == input.region.contentKey;
                        });
                        if (previous == changes.end()) {
                            if (std::any_of(ranges.begin(), ranges.end(), [&](const auto& range) {
                                    return !preservesTimingAudioInRange(*input.snapshot->timeGrid, *previewGrid, range);
                                })) {
                                showFailure(safe->contentRoot_, {}, true);
                                return;
                            }
                            changes.push_back({input.region.contentKey, input.snapshot->timeGrid, previewGrid});
                            continue;
                        }
                        // Build one undoable grid per modification, with each split limited to its own audio.
                        auto combined = makePlan(target, guides[target.guideIndex], *previous->after,
                                                 mode == 2 ? 0.5 : 1.0, ranges);
                        const auto& projection = input.region.contentProjection;
                        if (!combined.grid || !preservesTimingAudioInRange(*previewGrid, *combined.grid,
                                {projection.contentStartSeconds, projection.contentStartSeconds + projection.contentDurationSeconds})) {
                            ConfirmDialogContent::showMessage(&safe->contentRoot_, title,
                                LOC_RAW("These clip edits could not be combined safely. Try aligning fewer clips at once."));
                            return;
                        }
                        previous->after = std::move(combined.grid);
                    }
                    if (changes.empty())
                        return;

                    if (safe->processorRef_.commitTimeGridEdits(changes, title))
                        a11y::announce(safe->pianoRoll_, title);
                    else
                        stale();
                }));
        });
    });
}

void ReferenceEditor::didUpdateRegionSequenceProperties(juce::ARARegionSequence* track)
{
    if (pitchReferenceTrack_ == track)
        refreshControls();
}

void ReferenceEditor::willDestroyRegionSequence(juce::ARARegionSequence* track)
{
    if (pitchReferenceTrack_ == track)
        setPitchReferenceTrack(nullptr);
}

void ReferenceEditor::syncOverlay(const juce::ARARegionSequence* focusedTrack, ContentKey activeContent)
{
    const auto* dc = processorRef_.getDocumentController();
    if (dc != nullptr && activeContent.isValid() && pitchReferenceTrack_ != nullptr
        && pitchReferenceTrack_ != focusedTrack)
        pianoRoll_.setPitchReferences(readPitchReferences(*dc, *pitchReferenceTrack_, activeContent));
    else
        pianoRoll_.setPitchReferences({});
}

} // namespace OpenTune::PluginUI
