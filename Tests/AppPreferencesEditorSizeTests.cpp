#include "Utils/AppPreferences.h"

#include <cstdio>

namespace {

bool hasSize(const OpenTune::PluginEditorSize& actual, int width, int height)
{
    return actual.width == width && actual.height == height;
}

bool testOutOfScaleDragPreference()
{
    const auto directory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneAppPreferences-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions options;
    options.settingsDirectory = directory;
    bool passed = true;
    {
        OpenTune::AppPreferences preferences(options);
        passed = !preferences.getOutOfScaleDragEnabled();
        preferences.setOutOfScaleDragEnabled(true);
    }
    {
        OpenTune::AppPreferences preferences(options);
        passed = passed && preferences.getOutOfScaleDragEnabled();
        preferences.setOutOfScaleDragEnabled(false);
    }
    {
        OpenTune::AppPreferences preferences(options);
        passed = passed && !preferences.getOutOfScaleDragEnabled();
    }
    directory.deleteRecursively();
    return passed;
}

bool testSharedPluginEditorSize()
{
    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneAppPreferences-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions options;
    options.settingsDirectory = temporaryDirectory;

    bool sharedInstancesReadLatestSize = false;
    {
        OpenTune::AppPreferences instanceA(options);
        OpenTune::AppPreferences instanceB(options);
        OpenTune::AppPreferences staleInstance(options);
        const auto initialSize = instanceB.getDefaultPluginEditorSize();

        instanceA.setDefaultPluginEditorSize({ 1280, 760 });
        const auto refreshedSize = instanceB.getDefaultPluginEditorSize();
        staleInstance.setTheme(OpenTune::ThemeId::DarkBlueGrey);
        sharedInstancesReadLatestSize = hasSize(initialSize, 1000, 900)
            && hasSize(refreshedSize, 1280, 760);
    }

    OpenTune::AppPreferences reopenedInstance(options);
    const auto persistedSize = reopenedInstance.getDefaultPluginEditorSize();
    temporaryDirectory.deleteRecursively();
    return sharedInstancesReadLatestSize && hasSize(persistedSize, 1280, 760);
}

bool testSharedModelsDirectory()
{
    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneAppPreferences-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions options;
    options.settingsDirectory = temporaryDirectory;
    const auto legacyPath = temporaryDirectory.getChildFile("Legacy Models").getFullPathName();
    const auto chosenPath = temporaryDirectory.getChildFile("Chosen Models").getFullPathName();
    bool passed = false;
    {
        OpenTune::AppPreferences noOverride(options);
        if (noOverride.getCustomModelsDirectory().isEmpty()) {
            juce::PropertiesFile::Options legacyOptions;
            legacyOptions.applicationName = "app-preferences";
            legacyOptions.filenameSuffix = ".settings";
            legacyOptions.folderName = temporaryDirectory.getFullPathName();
            legacyOptions.storageFormat = juce::PropertiesFile::storeAsXML;
            {
                juce::PropertiesFile legacy(legacyOptions);
                legacy.setValue("shared.models.customDirectory", legacyPath);
                passed = legacy.saveIfNeeded();
            }
            OpenTune::AppPreferences writer(options);
            passed = passed && writer.getCustomModelsDirectory() == legacyPath
                && writer.setCustomModelsDirectory(chosenPath);
            noOverride.setTheme(OpenTune::ThemeId::DarkBlueGrey);

            OpenTune::AppPreferences reopened(options);
            passed = passed && reopened.getCustomModelsDirectory() == chosenPath
                && !reopened.setCustomModelsDirectory("relative/models")
                && reopened.getCustomModelsDirectory() == chosenPath
                && reopened.setCustomModelsDirectory({})
                && writer.getCustomModelsDirectory().isEmpty();
        }
    }
    temporaryDirectory.deleteRecursively();
    return passed;
}

bool testTraditionalChinesePreference()
{
    using namespace OpenTune;
    const auto directory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneAppPreferences-" + juce::Uuid().toString());
    AppPreferences::StorageOptions options;
    options.settingsDirectory = directory;

    bool passed = true;
    for (const auto language : { Language::Chinese, Language::TraditionalChinese }) {
        {
            AppPreferences writer(options);
            writer.setLanguage(language);
        }
        AppPreferences reader(options);
        passed = passed && reader.getState().shared.language == language;
    }
    directory.deleteRecursively();
    return passed
        && getLanguageNativeName(Language::TraditionalChinese) == juce::String::fromUTF8("繁體中文")
        && Loc::get(Language::TraditionalChinese, Loc::Keys::kEdit) == juce::String::fromUTF8("編輯")
        && Loc::get(Language::Chinese, Loc::Keys::kEdit) == juce::String::fromUTF8("编辑")
        && Loc::get(Language::TraditionalChinese, "Untranslated string") == "Untranslated string";
}

bool testShortcutScopesAndConflicts()
{
    using namespace OpenTune::KeyShortcutConfig;
    const auto pluginScopes = kPluginEditorGeneralScope
        | kPianoRollOpenTuneScope | kPianoRollOpenDyneScope;
    if (!scopesIntersect(kPianoRollOpenTuneScope, kPianoRollOpenTuneScope)
        || scopesIntersect(kPianoRollOpenTuneScope, kPianoRollOpenDyneScope)
        || scopesIntersect(kStandaloneArrangementScope, kPianoRollOpenTuneScope))
        return false;

    const ShortcutId pluginDispatchedTools[] = {
        ShortcutId::ToolDrawNote, ShortcutId::ToolSelect, ShortcutId::ToolLineAnchor,
        ShortcutId::ToolHandDraw, ShortcutId::ToolTimeTool, ShortcutId::ToolODSelect,
        ShortcutId::ToolODPitch, ShortcutId::ToolODPitchModulation, ShortcutId::ToolODPitchDrift,
        ShortcutId::ToolODVolumeEnvelope, ShortcutId::ToolODScissors,
        ShortcutId::ToolAutoTune, ShortcutId::Eq, ShortcutId::SelectAll,
        ShortcutId::Delete, ShortcutId::CancelSelection,
    };
    for (const auto id : pluginDispatchedTools) {
        if (!scopesIntersect(getShortcutInfo(id).scopes, pluginScopes))
            return false;
    }

    auto settings = KeyShortcutSettings::getDefault();
    const KeyBinding sharedBinding('K', juce::ModifierKeys::commandModifier);
    setShortcutBinding(settings, ShortcutId::Cut, sharedBinding);
    setShortcutBinding(settings, ShortcutId::Copy, sharedBinding);
    setShortcutBinding(settings, ShortcutId::Paste, sharedBinding);
    setShortcutBinding(settings, ShortcutId::ToolODSelect, sharedBinding);

    const std::vector<ShortcutId> pluginCandidates = {
        ShortcutId::Copy, ShortcutId::Paste, ShortcutId::ToolODSelect
    };
    const auto pluginConflicts = findConflictingShortcuts(
        settings, ShortcutId::Cut, sharedBinding, pluginCandidates, pluginScopes);
    if (pluginConflicts != std::vector<ShortcutId>{
            ShortcutId::Copy, ShortcutId::Paste, ShortcutId::ToolODSelect })
        return false;

    const KeyBinding modeBinding('Q', {});
    setShortcutBinding(settings, ShortcutId::ToolDrawNote, modeBinding);
    setShortcutBinding(settings, ShortcutId::ToolODSelect, modeBinding);
    const auto openTuneConflicts = findConflictingShortcuts(
        settings, ShortcutId::ToolDrawNote, modeBinding,
        { ShortcutId::ToolODSelect }, pluginScopes);
    return openTuneConflicts.empty()
        && findShortcutForBinding(settings, modeBinding, kPianoRollOpenTuneScope) == ShortcutId::ToolDrawNote
        && findShortcutForBinding(settings, modeBinding, kPianoRollOpenDyneScope) == ShortcutId::ToolODSelect;
}

bool testShortcutDefaults()
{
    using namespace OpenTune::KeyShortcutConfig;
    const auto settings = KeyShortcutSettings::getDefault();
    for (const auto& info : kShortcutInfos) {
        if (info.scopes == 0)
            return false;
    }

    const auto& deleteBindings = getShortcutBinding(settings, ShortcutId::Delete);
    const auto& handDraw = getShortcutBinding(settings, ShortcutId::ToolHandDraw);
    const auto& autoTune = getShortcutBinding(settings, ShortcutId::ToolAutoTune);
#if JUCE_MAC
    const auto transportModifiers = juce::ModifierKeys::commandModifier
        | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
    const KeyBinding playPauseBinding(juce::KeyPress::spaceKey, {});
    if (!getShortcutBinding(settings, ShortcutId::PlayPause).hasBinding(
            playPauseBinding)
        || !getShortcutBinding(settings, ShortcutId::Stop).hasBinding(KeyBinding('.', transportModifiers))
        || !getShortcutBinding(settings, ShortcutId::PlayFromStart).hasBinding(
            KeyBinding(juce::KeyPress::returnKey, transportModifiers))
        || toCanonicalString(playPauseBinding) != "Space")
        return false;

    if (deleteBindings.hasBinding(KeyBinding('1', {}))
        || !deleteBindings.hasBinding(KeyBinding(juce::KeyPress::deleteKey, {}))
        || !deleteBindings.hasBinding(KeyBinding(juce::KeyPress::backspaceKey, {}))
        || !handDraw.hasBinding(KeyBinding('H', {}))
        || !autoTune.hasBinding(KeyBinding('A', {})))
        return false;

    const auto zoomModifiers = juce::ModifierKeys::commandModifier
        | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
    if (!getShortcutBinding(settings, ShortcutId::ZoomHorizontalIn).hasBinding(
            KeyBinding(juce::KeyPress::rightKey, zoomModifiers))
        || !getShortcutBinding(settings, ShortcutId::ZoomHorizontalIn).hasBinding(
            { BindingType::WheelUp, juce::ModifierKeys::ctrlModifier })
        || !getShortcutBinding(settings, ShortcutId::ZoomHorizontalOut).hasBinding(
            KeyBinding(juce::KeyPress::leftKey, zoomModifiers))
        || !getShortcutBinding(settings, ShortcutId::ZoomHorizontalOut).hasBinding(
            { BindingType::WheelDown, juce::ModifierKeys::ctrlModifier })
        || !getShortcutBinding(settings, ShortcutId::ZoomVerticalIn).hasBinding(
            KeyBinding(juce::KeyPress::upKey, zoomModifiers))
        || !getShortcutBinding(settings, ShortcutId::ZoomVerticalIn).hasBinding(
            { BindingType::WheelUp, juce::ModifierKeys::shiftModifier })
        || !getShortcutBinding(settings, ShortcutId::ZoomVerticalOut).hasBinding(
            KeyBinding(juce::KeyPress::downKey, zoomModifiers))
        || !getShortcutBinding(settings, ShortcutId::ZoomVerticalOut).hasBinding(
            { BindingType::WheelDown, juce::ModifierKeys::shiftModifier }))
        return false;

    for (int key = '1'; key <= '6'; ++key) {
        const auto id = static_cast<ShortcutId>(static_cast<int>(ShortcutId::ToolODSelect) + key - '1');
        if (!getShortcutBinding(settings, id).hasBinding(KeyBinding(key, {})))
            return false;
    }
    return true;
#else
    if (!deleteBindings.hasBinding(KeyBinding('1', {}))
        || !handDraw.hasBinding(KeyBinding('5', {}))
        || !autoTune.hasBinding(KeyBinding('6', {}))
        || !getShortcutBinding(settings, ShortcutId::PlayPause).hasBinding(
            KeyBinding(juce::KeyPress::spaceKey, {}))
        || !getShortcutBinding(settings, ShortcutId::Stop).hasBinding(
            KeyBinding(juce::KeyPress::returnKey, {}))
        || !getShortcutBinding(settings, ShortcutId::PlayFromStart).hasBinding(KeyBinding('A', {}))
        || !getShortcutBinding(settings, ShortcutId::ToolODSelect).hasBinding(
            KeyBinding(juce::KeyPress::F1Key, {}))
        || !getShortcutBinding(settings, ShortcutId::ToolODPitch).hasBinding(
            KeyBinding(juce::KeyPress::F2Key, {}))
        || !getShortcutBinding(settings, ShortcutId::ToolODVolumeEnvelope).hasBinding(
            KeyBinding(juce::KeyPress::F4Key, {}))
        || !getShortcutBinding(settings, ShortcutId::ToolODScissors).hasBinding(
            KeyBinding(juce::KeyPress::F6Key, {})))
        return false;

    const auto zoomModifiers = juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier;
    return getShortcutBinding(settings, ShortcutId::ZoomHorizontalIn).hasBinding(
               KeyBinding(juce::KeyPress::rightKey, zoomModifiers))
        && getShortcutBinding(settings, ShortcutId::ZoomHorizontalIn).hasBinding(
               { BindingType::WheelUp, juce::ModifierKeys::ctrlModifier })
        && getShortcutBinding(settings, ShortcutId::ZoomHorizontalOut).hasBinding(
               KeyBinding(juce::KeyPress::leftKey, zoomModifiers))
        && getShortcutBinding(settings, ShortcutId::ZoomHorizontalOut).hasBinding(
               { BindingType::WheelDown, juce::ModifierKeys::ctrlModifier })
        && getShortcutBinding(settings, ShortcutId::ZoomVerticalIn).hasBinding(
               KeyBinding(juce::KeyPress::upKey, zoomModifiers))
        && getShortcutBinding(settings, ShortcutId::ZoomVerticalIn).hasBinding(
               { BindingType::WheelUp, juce::ModifierKeys::shiftModifier })
        && getShortcutBinding(settings, ShortcutId::ZoomVerticalOut).hasBinding(
               KeyBinding(juce::KeyPress::downKey, zoomModifiers))
        && getShortcutBinding(settings, ShortcutId::ZoomVerticalOut).hasBinding(
               { BindingType::WheelDown, juce::ModifierKeys::shiftModifier });
#endif
}

bool testLegacyShortcutMigration()
{
    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneShortcutMigration-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions storageOptions;
    storageOptions.settingsDirectory = temporaryDirectory;
    temporaryDirectory.createDirectory();

    auto settingsFile = temporaryDirectory.getChildFile(storageOptions.fileName);
    juce::PropertiesFile::Options propertiesOptions;
    propertiesOptions.applicationName = settingsFile.getFileNameWithoutExtension();
    propertiesOptions.filenameSuffix = settingsFile.getFileExtension();
    propertiesOptions.folderName = temporaryDirectory.getFullPathName();
    propertiesOptions.storageFormat = juce::PropertiesFile::storeAsXML;
    propertiesOptions.millisecondsBeforeSaving = 0;
    bool legacySaved = false;
    {
        juce::PropertiesFile legacySettings(propertiesOptions);
        legacySettings.setValue("shared.shortcuts.zoomHorizontalIn", "Ctrl+K");
        legacySettings.setValue("shared.shortcuts.zoomVerticalOut", "");
        legacySaved = legacySettings.save();
    }
    if (!legacySaved) {
        temporaryDirectory.deleteRecursively();
        return false;
    }

    const auto horizontalZoomId = OpenTune::KeyShortcutConfig::ShortcutId::ZoomHorizontalIn;
    const auto verticalZoomId = OpenTune::KeyShortcutConfig::ShortcutId::ZoomVerticalOut;
    const auto verticalZoomInId = OpenTune::KeyShortcutConfig::ShortcutId::ZoomVerticalIn;
    const OpenTune::KeyShortcutConfig::KeyBinding customKey(
        'K', juce::ModifierKeys::ctrlModifier);
    const OpenTune::KeyShortcutConfig::KeyBinding wheelBinding(
        OpenTune::KeyShortcutConfig::BindingType::WheelUp,
        juce::ModifierKeys::ctrlModifier);

    bool migrated = false;
    {
        OpenTune::AppPreferences preferences(storageOptions);
        const auto shortcuts = preferences.getState().shared.shortcuts;
        migrated = OpenTune::KeyShortcutConfig::getShortcutBinding(shortcuts, horizontalZoomId)
                       .hasBinding(customKey)
            && OpenTune::KeyShortcutConfig::getShortcutBinding(shortcuts, horizontalZoomId)
                   .hasBinding(wheelBinding)
            && OpenTune::KeyShortcutConfig::getShortcutBinding(shortcuts, verticalZoomId)
                   .bindings.empty()
            && OpenTune::KeyShortcutConfig::getShortcutBinding(shortcuts, verticalZoomInId)
                   .hasBinding({ OpenTune::KeyShortcutConfig::BindingType::WheelUp,
                                 juce::ModifierKeys::shiftModifier });
    }

    bool persistedMigration = false;
    {
        juce::PropertiesFile migratedSettings(propertiesOptions);
        OpenTune::KeyShortcutConfig::ShortcutBinding persistedHorizontalZoom;
        OpenTune::KeyShortcutConfig::ShortcutBinding persistedVerticalZoomIn;
        persistedMigration = migratedSettings.getIntValue("shared.shortcuts.schemaVersion", 0) == 5  // 5: Shift+Alt wheel zoom
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomHorizontalIn", {}),
                persistedHorizontalZoom)
            && persistedHorizontalZoom.hasBinding(customKey)
            && persistedHorizontalZoom.hasBinding(wheelBinding)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomVerticalIn", {}),
                persistedVerticalZoomIn)
            && persistedVerticalZoomIn.hasBinding({ OpenTune::KeyShortcutConfig::BindingType::WheelUp,
                                                    juce::ModifierKeys::shiftModifier })
            && migratedSettings.containsKey("shared.shortcuts.zoomVerticalOut")
            && migratedSettings.getValue("shared.shortcuts.zoomVerticalOut", {}).isEmpty();
    }
    temporaryDirectory.deleteRecursively();
    return migrated && persistedMigration;
}

bool testLegacyVocoderWeightPreferenceRemoved()
{
    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneVocoderPreferenceMigration-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions storageOptions;
    storageOptions.settingsDirectory = temporaryDirectory;
    temporaryDirectory.createDirectory();

    const auto settingsFile = temporaryDirectory.getChildFile(storageOptions.fileName);
    juce::PropertiesFile::Options propertiesOptions;
    propertiesOptions.applicationName = settingsFile.getFileNameWithoutExtension();
    propertiesOptions.filenameSuffix = settingsFile.getFileExtension();
    propertiesOptions.folderName = temporaryDirectory.getFullPathName();
    propertiesOptions.storageFormat = juce::PropertiesFile::storeAsXML;
    propertiesOptions.millisecondsBeforeSaving = 0;

    bool legacySaved = false;
    {
        juce::PropertiesFile legacySettings(propertiesOptions);
        legacySettings.setValue("shared.rendering.vocoderWeight", "hifigan_coulin9.onnx");
        legacySettings.setValue("shared.rendering.priority", "cpu-first");
        legacySaved = legacySettings.save();
    }
    if (!legacySaved) {
        temporaryDirectory.deleteRecursively();
        return false;
    }

    bool priorityPreserved = false;
    {
        OpenTune::AppPreferences preferences(storageOptions);
        priorityPreserved = preferences.getState().shared.renderingPriority
            == OpenTune::RenderingPriority::CpuFirst;
    }

    bool legacyKeyRemoved = false;
    {
        juce::PropertiesFile migratedSettings(propertiesOptions);
        legacyKeyRemoved = !migratedSettings.containsKey("shared.rendering.vocoderWeight")
            && migratedSettings.getValue("shared.rendering.priority") == "cpu-first";
    }

    temporaryDirectory.deleteRecursively();
    return priorityPreserved && legacyKeyRemoved;
}

bool testMacShortcutDefaultMigration()
{
    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneShortcutDefaultMigration-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions storageOptions;
    storageOptions.settingsDirectory = temporaryDirectory;
    temporaryDirectory.createDirectory();
#if JUCE_MAC
    const auto transportModifiers = juce::ModifierKeys::commandModifier
        | juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
#endif

    auto settingsFile = temporaryDirectory.getChildFile(storageOptions.fileName);
    juce::PropertiesFile::Options propertiesOptions;
    propertiesOptions.applicationName = settingsFile.getFileNameWithoutExtension();
    propertiesOptions.filenameSuffix = settingsFile.getFileExtension();
    propertiesOptions.folderName = temporaryDirectory.getFullPathName();
    propertiesOptions.storageFormat = juce::PropertiesFile::storeAsXML;
    propertiesOptions.millisecondsBeforeSaving = 0;
    bool legacySaved = false;
    {
        juce::PropertiesFile legacySettings(propertiesOptions);
        legacySettings.setValue("shared.shortcuts.schemaVersion", 1);
        legacySettings.setValue("shared.shortcuts.delete", "Delete|Backspace|1");
        legacySettings.setValue("shared.shortcuts.toolHandDraw", "5");
        legacySettings.setValue("shared.shortcuts.toolAutoTune", "6");
        legacySettings.setValue("shared.shortcuts.playPause", "Space");
#if JUCE_MAC
        legacySettings.setValue("shared.shortcuts.stop", ".");
        legacySettings.setValue("shared.shortcuts.playFromStart", "Command+.");
        legacySettings.setValue("shared.shortcuts.zoomHorizontalIn", "Command+Alt+Right|Ctrl+WheelUp");
        legacySettings.setValue("shared.shortcuts.zoomHorizontalOut", "Command+Alt+Left|Ctrl+WheelDown");
        legacySettings.setValue("shared.shortcuts.zoomVerticalIn", "Command+Alt+Up|Shift+WheelUp");
        legacySettings.setValue("shared.shortcuts.zoomVerticalOut", "Command+Alt+Down|Shift+WheelDown");
#else
        legacySettings.setValue("shared.shortcuts.stop", "Return");
        legacySettings.setValue("shared.shortcuts.playFromStart", "A");
        legacySettings.setValue("shared.shortcuts.zoomHorizontalIn", "Command+Alt+Right|Ctrl+WheelUp");
        legacySettings.setValue("shared.shortcuts.zoomHorizontalOut", "Command+Alt+Left|Ctrl+WheelDown");
        legacySettings.setValue("shared.shortcuts.zoomVerticalIn", "Command+Alt+Up|Shift+WheelUp");
        legacySettings.setValue("shared.shortcuts.zoomVerticalOut", "Command+Alt+Down|Shift+WheelDown");
#endif
        legacySettings.setValue("shared.shortcuts.toolSelect", "Q");
        legacySaved = legacySettings.save();
    }
    if (!legacySaved) {
        temporaryDirectory.deleteRecursively();
        return false;
    }

    {
        OpenTune::AppPreferences preferences(storageOptions);
    }

    bool migrated = false;
    {
        juce::PropertiesFile migratedSettings(propertiesOptions);
        OpenTune::KeyShortcutConfig::ShortcutBinding deleteBindings;
        OpenTune::KeyShortcutConfig::ShortcutBinding handDraw;
        OpenTune::KeyShortcutConfig::ShortcutBinding autoTune;
        OpenTune::KeyShortcutConfig::ShortcutBinding playPause;
        OpenTune::KeyShortcutConfig::ShortcutBinding stop;
        OpenTune::KeyShortcutConfig::ShortcutBinding playFromStart;
        OpenTune::KeyShortcutConfig::ShortcutBinding horizontalZoomIn;
        OpenTune::KeyShortcutConfig::ShortcutBinding horizontalZoomOut;
        OpenTune::KeyShortcutConfig::ShortcutBinding verticalZoomIn;
        OpenTune::KeyShortcutConfig::ShortcutBinding verticalZoomOut;
        OpenTune::KeyShortcutConfig::ShortcutBinding customSelect;
        migrated = migratedSettings.getIntValue("shared.shortcuts.schemaVersion", 0) == 5  // 5: Shift+Alt wheel zoom
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.delete", {}), deleteBindings)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.toolHandDraw", {}), handDraw)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.toolAutoTune", {}), autoTune)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.playPause", {}), playPause)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.stop", {}), stop)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.playFromStart", {}), playFromStart)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomHorizontalIn", {}), horizontalZoomIn)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomHorizontalOut", {}), horizontalZoomOut)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomVerticalIn", {}), verticalZoomIn)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.zoomVerticalOut", {}), verticalZoomOut)
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedSettings.getValue("shared.shortcuts.toolSelect", {}), customSelect);
        const auto hasExactly = [](const OpenTune::KeyShortcutConfig::ShortcutBinding& actual,
                                   std::initializer_list<OpenTune::KeyShortcutConfig::KeyBinding> expected) {
            if (actual.bindings.size() != expected.size())
                return false;
            for (const auto& binding : expected) {
                if (!actual.hasBinding(binding))
                    return false;
            }
            return true;
        };
#if JUCE_MAC
        migrated = migrated
            && hasExactly(deleteBindings, {
                { juce::KeyPress::deleteKey, {} }, { juce::KeyPress::backspaceKey, {} }
            })
            && hasExactly(handDraw, { { 'H', {} } })
            && hasExactly(autoTune, { { 'A', {} } })
            && hasExactly(playPause, { { juce::KeyPress::spaceKey, {} } })
            && hasExactly(stop, { { '.', transportModifiers } })
            && hasExactly(playFromStart, { { juce::KeyPress::returnKey, transportModifiers } })
            && hasExactly(horizontalZoomIn, {
                { juce::KeyPress::rightKey, transportModifiers },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp, juce::ModifierKeys::ctrlModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp,
                  juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(horizontalZoomOut, {
                { juce::KeyPress::leftKey, transportModifiers },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown, juce::ModifierKeys::ctrlModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown,
                  juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(verticalZoomIn, {
                { juce::KeyPress::upKey, transportModifiers },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp, juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(verticalZoomOut, {
                { juce::KeyPress::downKey, transportModifiers },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown, juce::ModifierKeys::shiftModifier }
            });
#else
        migrated = migrated
            && hasExactly(deleteBindings, {
                { juce::KeyPress::deleteKey, {} },
                { juce::KeyPress::backspaceKey, {} },
                { '1', {} }
            })
            && hasExactly(handDraw, { { '5', {} } })
            && hasExactly(autoTune, { { '6', {} } })
            && hasExactly(playPause, { { juce::KeyPress::spaceKey, {} } })
            && hasExactly(stop, { { juce::KeyPress::returnKey, {} } })
            && hasExactly(playFromStart, { { 'A', {} } })
            && hasExactly(horizontalZoomIn, {
                { juce::KeyPress::rightKey,
                  juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp, juce::ModifierKeys::ctrlModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp,
                  juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(horizontalZoomOut, {
                { juce::KeyPress::leftKey,
                  juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown, juce::ModifierKeys::ctrlModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown,
                  juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(verticalZoomIn, {
                { juce::KeyPress::upKey,
                  juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelUp, juce::ModifierKeys::shiftModifier }
            })
            && hasExactly(verticalZoomOut, {
                { juce::KeyPress::downKey,
                  juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier },
                { OpenTune::KeyShortcutConfig::BindingType::WheelDown, juce::ModifierKeys::shiftModifier }
            });
#endif
        migrated = migrated && hasExactly(customSelect, { { 'Q', {} } });
    }
#if JUCE_MAC
    bool preservedCustomBinding = false;
    {
        juce::PropertiesFile customSettings(propertiesOptions);
        customSettings.setValue("shared.shortcuts.schemaVersion", 1);
        customSettings.setValue("shared.shortcuts.toolHandDraw", "5|Q");
        preservedCustomBinding = customSettings.save();
    }
    if (preservedCustomBinding) {
        {
            OpenTune::AppPreferences preferences(storageOptions);
        }
        juce::PropertiesFile migratedCustomSettings(propertiesOptions);
        OpenTune::KeyShortcutConfig::ShortcutBinding handDraw;
        preservedCustomBinding = OpenTune::KeyShortcutConfig::parseShortcutBinding(
                migratedCustomSettings.getValue("shared.shortcuts.toolHandDraw", {}), handDraw)
            && handDraw.bindings.size() == 2
            && handDraw.hasBinding({ 'H', {} })
            && handDraw.hasBinding({ 'Q', {} })
            && !handDraw.hasBinding({ '5', {} });
    }
    migrated = migrated && preservedCustomBinding;

    bool restoredSpacePlay = false;
    {
        juce::PropertiesFile previousDefault(propertiesOptions);
        previousDefault.setValue("shared.shortcuts.schemaVersion", 2);
        previousDefault.setValue("shared.shortcuts.playPause", "Cmd+Shift+Alt+Space");
        previousDefault.setValue("shared.shortcuts.stop", "Ctrl+S");
        restoredSpacePlay = previousDefault.save();
    }
    if (restoredSpacePlay) {
        {
            OpenTune::AppPreferences preferences(storageOptions);
        }
        juce::PropertiesFile restoredSettings(propertiesOptions);
        OpenTune::KeyShortcutConfig::ShortcutBinding playPause;
        OpenTune::KeyShortcutConfig::ShortcutBinding stop;
        restoredSpacePlay = restoredSettings.getIntValue("shared.shortcuts.schemaVersion", 0) == 5
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                restoredSettings.getValue("shared.shortcuts.playPause", {}), playPause)
            && playPause.bindings.size() == 1
            && playPause.hasBinding({ juce::KeyPress::spaceKey, {} })
            && OpenTune::KeyShortcutConfig::parseShortcutBinding(
                restoredSettings.getValue("shared.shortcuts.stop", {}), stop)
            && stop.bindings.size() == 1
            && stop.hasBinding({ 'S', juce::ModifierKeys::ctrlModifier });
    }
    migrated = migrated && restoredSpacePlay;
#endif
    temporaryDirectory.deleteRecursively();
    return migrated;
}

bool testAltShiftWheelZoomShortcut()
{
    using namespace OpenTune::KeyShortcutConfig;
    const auto altShift = juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier;
    const KeyBinding zoomInWheel(BindingType::WheelUp, altShift);
    const KeyBinding zoomOutWheel(BindingType::WheelDown, altShift);
    const auto defaults = KeyShortcutSettings::getDefault();
    if (findShortcutForBinding(defaults, zoomInWheel, kPianoRollOpenTuneScope) != ShortcutId::ZoomHorizontalIn
        || findShortcutForBinding(defaults, zoomOutWheel, kPianoRollOpenDyneScope) != ShortcutId::ZoomHorizontalOut)
        return false;

    const auto temporaryDirectory = juce::File::getCurrentWorkingDirectory()
        .getChildFile("OpenTuneAltShiftWheelMigration-" + juce::Uuid().toString());
    OpenTune::AppPreferences::StorageOptions storageOptions;
    storageOptions.settingsDirectory = temporaryDirectory;
    temporaryDirectory.createDirectory();

    auto settingsFile = temporaryDirectory.getChildFile(storageOptions.fileName);
    juce::PropertiesFile::Options propertiesOptions;
    propertiesOptions.applicationName = settingsFile.getFileNameWithoutExtension();
    propertiesOptions.filenameSuffix = settingsFile.getFileExtension();
    propertiesOptions.folderName = temporaryDirectory.getFullPathName();
    propertiesOptions.storageFormat = juce::PropertiesFile::storeAsXML;
    propertiesOptions.millisecondsBeforeSaving = 0;
    const KeyBinding ctrlWheelUp(BindingType::WheelUp, juce::ModifierKeys::ctrlModifier);
    const KeyBinding ctrlWheelDown(BindingType::WheelDown, juce::ModifierKeys::ctrlModifier);
    bool saved = false;
    {
        juce::PropertiesFile storedSettings(propertiesOptions);
        storedSettings.setValue("shared.shortcuts.schemaVersion", 4);
        storedSettings.setValue("shared.shortcuts.zoomHorizontalIn", toCanonicalString(ctrlWheelUp));
        storedSettings.setValue("shared.shortcuts.zoomHorizontalOut", toCanonicalString(ctrlWheelDown));
        storedSettings.setValue("shared.shortcuts.scrollHorizontalRight", toCanonicalString(zoomOutWheel));
        saved = storedSettings.save();
    }

    bool migrated = false;
    if (saved) {
        OpenTune::AppPreferences preferences(storageOptions);
        const auto shortcuts = preferences.getState().shared.shortcuts;
        const auto& zoomIn = getShortcutBinding(shortcuts, ShortcutId::ZoomHorizontalIn);
        const auto& zoomOut = getShortcutBinding(shortcuts, ShortcutId::ZoomHorizontalOut);
        migrated = zoomIn.hasBinding(ctrlWheelUp) && zoomIn.hasBinding(zoomInWheel)
            && zoomOut.hasBinding(ctrlWheelDown) && !zoomOut.hasBinding(zoomOutWheel)
            && getShortcutBinding(shortcuts, ShortcutId::ScrollHorizontalRight).hasBinding(zoomOutWheel);
    }
    temporaryDirectory.deleteRecursively();
    return migrated;
}

} // namespace

int main()
{
    if (!testOutOfScaleDragPreference()) {
        std::fputs("FAIL: out-of-scale drag preference default and persistence\n", stderr);
        return 1;
    }

    if (!testSharedPluginEditorSize()) {
        std::fputs("FAIL: shared plugin editor size preferences\n", stderr);
        return 1;
    }

    if (!testSharedModelsDirectory()) {
        std::fputs("FAIL: shared models directory preference\n", stderr);
        return 1;
    }

    if (!testTraditionalChinesePreference()) {
        std::fputs("FAIL: Traditional Chinese selection and localization\n", stderr);
        return 1;
    }

    if (!testShortcutScopesAndConflicts()) {
        std::fputs("FAIL: shortcut scopes and conflicts\n", stderr);
        return 1;
    }

    if (!testAltShiftWheelZoomShortcut()) {
        std::fputs("FAIL: Shift+Alt wheel horizontal zoom shortcut\n", stderr);
        return 1;
    }

    if (!testShortcutDefaults()) {
        std::fputs("FAIL: shortcut defaults\n", stderr);
        return 1;
    }

    if (!testLegacyShortcutMigration()) {
        std::fputs("FAIL: legacy shortcut schema migration\n", stderr);
        return 1;
    }

    if (!testLegacyVocoderWeightPreferenceRemoved()) {
        std::fputs("FAIL: legacy vocoder weight preference removal\n", stderr);
        return 1;
    }

    if (!testMacShortcutDefaultMigration()) {
        std::fputs("FAIL: targeted shortcut default migration\n", stderr);
        return 1;
    }

    std::puts("AppPreferences tests passed.");
    return 0;
}
