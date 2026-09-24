#pragma once

#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ThemeTokens.h"

namespace OpenTune {

/// Tactical Telemetry / CRT 終端 LookAndFeel。
/// 方角（radius = 0）、1px 結構線、等寬字主導、危險紅唯一強調色、
/// 掃描線與危險斜紋等工業標記。對應 ThemeId::Telemetry。
class TelemetryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TelemetryLookAndFeel();

    /// 工業撥盤繪製（校準刻度 + 紅色指針 + 十字註冊標記）。
    /// 供 drawRotarySlider 與 TrackPanel 的音量旋钮複用。
    static void drawDial(juce::Graphics& g,
                         juce::Rectangle<float> bounds,
                         float normalisedValue,
                         bool highlighted,
                         float rotaryStartAngle,
                         float rotaryEndAngle,
                         const juce::Slider* slider);

    // --- Sliders ---
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle style, juce::Slider& slider) override;

    // --- Buttons ---
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // --- Text fields ---
    void fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                  juce::TextEditor& textEditor) override;

    void drawTextEditorOutline(juce::Graphics& g, int width, int height,
                               juce::TextEditor& textEditor) override;

    // --- ComboBox ---
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;

    // --- ScrollBar ---
    int getDefaultScrollbarWidth() override;

    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollBar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override;

    // --- PopupMenu ---
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;

    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;

    // --- Misc chrome ---
    juce::MouseCursor getMouseCursorFor(juce::Component& component) override;

    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g,
                                    int w, int h, int titleSpaceX, int titleSpaceW,
                                    const juce::Image* icon, bool drawTitleTextOnLeft) override;

    // --- Fonts：等寬字主導（資料與遙測）---
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox& box) override;
    juce::Font getPopupMenuFont() override;
    juce::Label* createComboBoxTextBox(juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;

    /// 安裝為 JUCE 進程級預設 LookAndFeel（獨立 DialogWindow 與普通控件使用）。
    static void installAsDefault();

private:
    static TelemetryLookAndFeel& getDefaultInstance();
};

} // namespace OpenTune
