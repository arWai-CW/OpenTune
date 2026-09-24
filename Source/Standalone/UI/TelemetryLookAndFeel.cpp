#include "TelemetryLookAndFeel.h"
#include "TelemetryTheme.h"
#include "UIColors.h"
#include "UiAssets.h"
#include "../../Utils/CursorTheme.h"

namespace OpenTune {

// ============================================================================
// Tactical Telemetry & CRT Terminal — LookAndFeel 實作
//
// 機械剛性：零圓角、1px 結構線、零陰影 / 輝光。
// 視覺語彙：等寬字、校準刻度、危險紅、CRT 掃描線、ASCII 風格括標。
// ============================================================================

namespace
{
    using T = Telemetry::Colors;

    // 字級對齊原有主題的實際渲染值（OpenTune：Label / TextButton / ComboBox /
    // PopupMenu 皆為 16pt；見 UIColors::getUIFont 的 jmax(16, …) 下限）。
    // 等寬字面比 Roboto 小，數值對齊後視覺仍會略小一號，這是字型本身的差異。
    constexpr float kMonoDataSize    = 16.0f; // telemetry / 中繼資料字級
    constexpr float kMonoFieldSize   = 16.0f; // 輸入 / 下拉欄位
}

// ----------------------------------------------------------------------------
// 工業撥盤：校準刻度環 + 紅色指針 + 十字註冊標記（靜態，供元件複用）
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::drawDial(juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    float normalisedValue,
                                    bool highlighted,
                                    float rotaryStartAngle,
                                    float rotaryEndAngle,
                                    const juce::Slider* slider)
{
    const auto value = juce::jlimit(0.0f, 1.0f, normalisedValue);
    const auto inset = bounds.reduced(2.0f);
    const auto side = juce::jmin(inset.getWidth(), inset.getHeight());
    if (side < 8.0f)
        return;

    juce::Colour pointerColour { T::HazardRed };
    if (slider != nullptr && slider->getProperties().contains("arcColor"))
        pointerColour = juce::Colour(static_cast<juce::uint32>(static_cast<int>(slider->getProperties()["arcColor"])));

    const auto centre = inset.getCentre();
    const auto tickOuter = side * 0.48f;
    const auto bodyRadius = side * 0.36f;
    const auto angle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * value;

    // --- 儀表井（凹槽）：比任何卡片底色更深，確保撥盤永遠有邊界 ---
    // 舊版本體 #161616 與卡片 #1A1A1A 僅差 4 階 → 視覺上完全消失。
    // 改成「近黑井底 + PhosphorFaint 亮邊界」，與卡片底色無論深淺都能分離。
    const auto wellRadius = side * 0.48f;
    g.setColour(juce::Colour { T::WellTop });
    g.fillEllipse(centre.x - wellRadius, centre.y - wellRadius,
                  wellRadius * 2.0f, wellRadius * 2.0f);
    g.setColour(juce::Colour { T::PhosphorFaint }.withAlpha(0.55f));
    g.drawEllipse(centre.x - wellRadius, centre.y - wellRadius,
                  wellRadius * 2.0f, wellRadius * 2.0f, 1.0f);

    // --- 校準刻度環：已達值 = 紅，未達 = 暗灰；主刻度更長 ---
    static constexpr int kTickCount = 21;
    for (int i = 0; i < kTickCount; ++i)
    {
        const auto t = static_cast<float>(i) / static_cast<float>(kTickCount - 1);
        const auto tickAngle = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * t;
        const auto major = (i % 5) == 0;
        const auto inner = tickOuter - (major ? 7.0f : 4.0f);
        const auto passed = t <= value;

        const auto colour = passed ? pointerColour.withAlpha(major ? 0.95f : 0.75f)
                                   : juce::Colour { T::PhosphorFaint }.withAlpha(major ? 0.85f : 0.55f);

        g.setColour(colour);
        g.drawLine(centre.x + inner * std::sin(tickAngle),
                   centre.y - inner * std::cos(tickAngle),
                   centre.x + tickOuter * std::sin(tickAngle),
                   centre.y - tickOuter * std::cos(tickAngle),
                   major ? 1.5f : 1.0f);
    }

    // --- 撥盤本體（機械圓盤——方角規則的唯一豁免）---
    juce::Rectangle<float> body(centre.x - bodyRadius, centre.y - bodyRadius,
                                bodyRadius * 2.0f, bodyRadius * 2.0f);

    g.setColour(juce::Colour { T::ButtonHover });
    g.fillEllipse(body);
    g.setColour(juce::Colour { T::PhosphorFaint });
    g.drawEllipse(body, 1.25f);

    // --- 十字註冊標記（印刷套準十字）---
    g.setColour(juce::Colour { T::Phosphor }.withAlpha(0.30f));
    g.drawLine(centre.x - bodyRadius * 0.62f, centre.y,
               centre.x + bodyRadius * 0.62f, centre.y, 1.0f);
    g.drawLine(centre.x, centre.y - bodyRadius * 0.62f,
               centre.x, centre.y + bodyRadius * 0.62f, 1.0f);

    // --- 紅色指針 ---
    g.setColour(pointerColour);
    g.drawLine(centre.x + bodyRadius * 0.18f * std::sin(angle),
               centre.y - bodyRadius * 0.18f * std::cos(angle),
               centre.x + bodyRadius * 0.92f * std::sin(angle),
               centre.y - bodyRadius * 0.92f * std::cos(angle),
               highlighted ? 3.5f : 3.0f);

    // --- 中心軸銷 ---
    g.setColour(juce::Colour { T::PhosphorFaint });
    g.fillEllipse(centre.x - 2.0f, centre.y - 2.0f, 4.0f, 4.0f);

    // --- 懸停 / 拖拽：井緣外加一圈紅色警示環 ---
    if (highlighted)
    {
        // 夾在元件邊界內，避免極小尺寸時外環被裁切
        const auto maxRadius = side * 0.5f + 1.5f;
        const auto outerRadius = juce::jmin(wellRadius + 2.0f, maxRadius);
        g.setColour(pointerColour.withAlpha(0.85f));
        g.drawEllipse(centre.x - outerRadius, centre.y - outerRadius,
                      outerRadius * 2.0f, outerRadius * 2.0f, 1.0f);
    }
}

void TelemetryLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPosProportional, float rotaryStartAngle,
                                            float rotaryEndAngle, juce::Slider& slider)
{
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height));

    juce::Graphics::ScopedSaveState saveState(g);
    g.setOpacity(slider.isEnabled() ? 1.0f : 0.42f);

    drawDial(g, bounds, sliderPosProportional, slider.isMouseOverOrDragging(),
             rotaryStartAngle, rotaryEndAngle, &slider);

    // 懸停 / 拖拽時顯示數值讀數（等寬字）
    if (slider.isMouseOverOrDragging() && slider.isEnabled())
    {
        juce::String text;
        if (std::abs(slider.getValue()) < 10.0)
            text = juce::String(slider.getValue(), 1);
        else
            text = juce::String(static_cast<int>(slider.getValue()));

        g.setFont(Telemetry::monoFont(12.0f, true));
        g.setColour(juce::Colour { T::Phosphor });
        g.drawText(text, bounds.reduced(4.0f), juce::Justification::centred, false);
    }
}

// ----------------------------------------------------------------------------
// 線性滑杆：方角軌道 + 紅色填充 + 方形滑塊
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float minSliderPos, float maxSliderPos,
                                            const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::ignoreUnused(minSliderPos, maxSliderPos);

    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height));
    const auto hovered = slider.isMouseOverOrDragging();

    if (style == juce::Slider::LinearVertical)
    {
        constexpr float trackW = 4.0f;
        auto track = bounds.withWidth(trackW)
                          .withX(bounds.getCentreX() - trackW * 0.5f)
                          .reduced(0.0f, 4.0f);

        // 軌道（平面，方角）
        g.setColour(juce::Colour { T::GridGrey });
        g.fillRect(track);

        // 作用中填充 = 危險紅
        const auto fillTop = juce::jmax(track.getY(), sliderPos);
        if (fillTop < track.getBottom())
        {
            g.setColour(juce::Colour { T::HazardRed });
            g.fillRect(track.getX(), fillTop, track.getWidth(), track.getBottom() - fillTop);
        }

        // 方形滑塊（磷光白，懸停轉紅）
        constexpr float thumbW = 24.0f;
        constexpr float thumbH = 10.0f;
        auto thumb = juce::Rectangle<float>(bounds.getCentreX() - thumbW * 0.5f,
                                            sliderPos - thumbH * 0.5f,
                                            thumbW, thumbH);

        g.setColour(hovered ? juce::Colour { T::HazardLift } : juce::Colour { T::Bone });
        g.fillRect(thumb);
        g.setColour(juce::Colour { T::Ink });
        g.drawRect(thumb, 1.0f);
    }
    else if (style == juce::Slider::LinearHorizontal)
    {
        constexpr float trackH = 4.0f;
        auto track = bounds.withHeight(trackH)
                          .withY(bounds.getCentreY() - trackH * 0.5f)
                          .reduced(4.0f, 0.0f);

        g.setColour(juce::Colour { T::GridGrey });
        g.fillRect(track);

        const auto fillRight = juce::jmin(track.getRight(), sliderPos);
        if (fillRight > track.getX())
        {
            g.setColour(juce::Colour { T::HazardRed });
            g.fillRect(track.getX(), track.getY(), fillRight - track.getX(), track.getHeight());
        }

        constexpr float thumbW = 10.0f;
        constexpr float thumbH = 24.0f;
        auto thumb = juce::Rectangle<float>(sliderPos - thumbW * 0.5f,
                                            bounds.getCentreY() - thumbH * 0.5f,
                                            thumbW, thumbH);

        g.setColour(hovered ? juce::Colour { T::HazardLift } : juce::Colour { T::Bone });
        g.fillRect(thumb);
        g.setColour(juce::Colour { T::Ink });
        g.drawRect(thumb, 1.0f);
    }
    else
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                               minSliderPos, maxSliderPos, style, slider);
    }
}

// ----------------------------------------------------------------------------
// 按鈕：平面方角。常態 = 控制面；懸停 = 抬升；按下 = 危險斜紋；開啟 = 實心紅
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                const juce::Colour& backgroundColour,
                                                bool shouldDrawButtonAsHighlighted,
                                                bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(backgroundColour);

    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    if (bounds.isEmpty())
        return;

    const auto enabled = button.isEnabled();
    const auto on = button.getToggleState();
    const auto down = shouldDrawButtonAsDown;
    const auto hover = shouldDrawButtonAsHighlighted && enabled;

    if (down)
    {
        // 按下 = 危險斜紋（機械警示帶）
        g.setColour(juce::Colour { T::Ink });
        g.fillRect(bounds);
        Telemetry::drawHazardStripes(g, bounds, 7.0f);
    }
    else if (on)
    {
        g.setColour(juce::Colour { T::HazardRed });
        g.fillRect(bounds);
    }
    else
    {
        g.setColour(juce::Colour { T::ButtonFace });
        g.fillRect(bounds);
        if (hover)
        {
            g.setColour(juce::Colour { T::ButtonHover });
            g.fillRect(bounds);
        }
    }

    // 1px 結構邊線
    auto border = juce::Colour { T::Edge };
    if (on && !down)
        border = juce::Colour { T::Phosphor }.withAlpha(0.85f);
    else if (hover)
        border = juce::Colour { T::Phosphor }.withAlpha(0.55f);

    g.setColour(border.withMultipliedAlpha(enabled ? 1.0f : 0.45f));
    g.drawRect(bounds, 1.0f);
}

// 按鈕文字：等寬字 + 全大寫（結構衝擊）
void TelemetryLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                          bool shouldDrawButtonAsHighlighted,
                                          bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

    auto colour = button.getToggleState()
                      ? button.findColour(juce::TextButton::textColourOnId)
                      : button.findColour(juce::TextButton::textColourOffId);

    if (!button.isEnabled())
        colour = juce::Colour { T::PhosphorFaint };

    g.setColour(colour);
    g.setFont(Telemetry::monoFont(kMonoDataSize, true));
    g.drawFittedText(button.getButtonText().toUpperCase(),
                     button.getLocalBounds(),
                     juce::Justification::centred, 1, 0.8f);
}

// 核取方塊：方角 + 紅底白色 X（終端標記風格）
void TelemetryLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                            bool shouldDrawButtonAsHighlighted,
                                            bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(shouldDrawButtonAsDown);

    // 字級先乘使用者字級係數，再以控件高度做上限（避免文字溢出控件）
    const auto fontSize = juce::jmin(FontScale::scale(15.0f),
                                     static_cast<float>(button.getHeight()) * 0.75f);
    const auto boxSize = fontSize * 1.1f;

    auto bounds = button.getLocalBounds().toFloat();
    auto tickBounds = juce::Rectangle<float>(4.0f,
                                             (bounds.getHeight() - boxSize) * 0.5f,
                                             boxSize, boxSize);

    // 方角框體
    g.setColour(juce::Colour { T::Ink });
    g.fillRect(tickBounds);

    const auto border = (shouldDrawButtonAsHighlighted || button.isMouseOver())
                            ? juce::Colour { T::Phosphor }
                            : juce::Colour { T::PhosphorFaint };
    g.setColour(border);
    g.drawRect(tickBounds, 1.0f);

    if (button.getToggleState())
    {
        // 紅底 + 白色 X
        g.setColour(juce::Colour { T::HazardRed });
        g.fillRect(tickBounds);

        const auto inset = tickBounds.reduced(3.5f);
        g.setColour(juce::Colour { T::Phosphor });
        g.drawLine(inset.getX(), inset.getY(), inset.getRight(), inset.getBottom(), 2.0f);
        g.drawLine(inset.getRight(), inset.getY(), inset.getX(), inset.getBottom(), 2.0f);
    }

    if (!button.getButtonText().isEmpty())
    {
        g.setColour(button.isEnabled() ? juce::Colour { T::Phosphor }
                                       : juce::Colour { T::PhosphorFaint });
        // fontSize 已由 jmin(FontScale::scale(15), height * 0.75) 夾過，跳過 16pt 下限，
        // 才能與原有主題的 toggle 字級（同為 15pt）對齊。
        g.setFont(Telemetry::monoFont(fontSize, false, false));
        g.drawFittedText(button.getButtonText().toUpperCase(),
                         button.getLocalBounds()
                              .withTrimmedLeft(juce::roundToInt(boxSize) + 10)
                              .withTrimmedRight(2),
                         juce::Justification::centredLeft, 10);
    }
}

// ----------------------------------------------------------------------------
// 文字欄位：深井 + 1px 邊線；聚焦 = 紅邊 + 四角括標
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height,
                                                    juce::TextEditor& textEditor)
{
    juce::ignoreUnused(textEditor);
    g.setColour(juce::Colour { T::Ink });
    g.fillRect(0, 0, width, height);
    Telemetry::drawScanlines(g, juce::Rectangle<float>(0.0f, 0.0f,
                                                       static_cast<float>(width),
                                                       static_cast<float>(height)),
                             juce::Colour { 0x06FFFFFF });
}

void TelemetryLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height,
                                                 juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width),
                                         static_cast<float>(height));

    g.setColour(juce::Colour { T::Divider });
    g.drawRect(bounds.reduced(0.5f), 1.0f);

    const auto focused = textEditor.hasKeyboardFocus(true) && !textEditor.isReadOnly();
    if (focused)
    {
        g.setColour(juce::Colour { T::HazardRed });
        g.drawRect(bounds.reduced(0.5f), 2.0f);
        Telemetry::drawCornerBrackets(g, bounds.reduced(3.0f),
                                      juce::Colour { T::HazardRed }, 9.0f, 2.0f);
    }
}

// ----------------------------------------------------------------------------
// ComboBox：方角井 + 三角箭頭（作用中紅邊）
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown,
                                        int buttonX, int buttonY, int buttonW, int buttonH,
                                        juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width),
                                         static_cast<float>(height)).reduced(2.0f);

    const auto active = isButtonDown || box.isPopupActive();
    const auto hover = box.isMouseOver();

    g.setColour(active ? juce::Colour { T::Void } : juce::Colour { T::ButtonFace });
    g.fillRect(bounds);

    auto border = juce::Colour { T::Edge };
    if (active)
        border = juce::Colour { T::HazardRed };
    else if (hover)
        border = juce::Colour { T::Phosphor }.withAlpha(0.55f);

    g.setColour(border);
    g.drawRect(bounds.reduced(0.5f), 1.0f);

    // 箭頭：實心下三角
    if (buttonW > 0 && buttonH > 0)
    {
        const auto arrowSize = 4.5f;
        const auto cx = static_cast<float>(buttonX) + static_cast<float>(buttonW) * 0.5f;
        const auto cy = static_cast<float>(buttonY) + static_cast<float>(buttonH) * 0.5f;

        juce::Path arrow;
        arrow.addTriangle(cx - arrowSize, cy - arrowSize * 0.5f,
                          cx + arrowSize, cy - arrowSize * 0.5f,
                          cx, cy + arrowSize * 0.6f);

        g.setColour(active ? juce::Colour { T::HazardRed } : juce::Colour { T::Phosphor });
        g.fillPath(arrow);
    }
}

// ----------------------------------------------------------------------------
// ScrollBar：方角軌道 + 磷光滑塊（懸停轉紅）
// ----------------------------------------------------------------------------
int TelemetryLookAndFeel::getDefaultScrollbarWidth()
{
    return UIColors::scrollBarThickness;
}

void TelemetryLookAndFeel::drawScrollbar(juce::Graphics& g,
                                         juce::ScrollBar& scrollBar,
                                         int x, int y, int width, int height,
                                         bool isScrollbarVertical,
                                         int thumbStartPosition, int thumbSize,
                                         bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused(scrollBar);

    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(width), static_cast<float>(height));
    if (bounds.isEmpty())
        return;

    const auto availableThickness = isScrollbarVertical ? bounds.getWidth() : bounds.getHeight();
    const auto trackThickness = juce::jmin(UIColors::scrollBarThumbThickness, availableThickness * 0.55f);
    auto track = isScrollbarVertical
        ? bounds.withWidth(trackThickness).withX(bounds.getCentreX() - trackThickness * 0.5f).reduced(0.0f, 8.0f)
        : bounds.withHeight(trackThickness).withY(bounds.getCentreY() - trackThickness * 0.5f).reduced(8.0f, 0.0f);

    // 軌道（平面深井）
    g.setColour(juce::Colour { T::Ink });
    g.fillRect(track);

    if (thumbSize <= 0)
        return;

    auto thumb = isScrollbarVertical
        ? juce::Rectangle<float>(bounds.getCentreX() - trackThickness * 0.5f,
                                 static_cast<float>(thumbStartPosition),
                                 trackThickness,
                                 static_cast<float>(thumbSize)).reduced(0.0f, 2.0f)
        : juce::Rectangle<float>(static_cast<float>(thumbStartPosition),
                                 bounds.getCentreY() - trackThickness * 0.5f,
                                 static_cast<float>(thumbSize),
                                 trackThickness).reduced(2.0f, 0.0f);

    thumb = thumb.getIntersection(bounds.reduced(3.0f));
    if (thumb.isEmpty())
        return;

    // 滑塊 = 作用中位置資料 → 磷光白，懸停 / 拖拽 = 危險紅
    g.setColour((isMouseDown || isMouseOver) ? juce::Colour { T::HazardRed }
                                             : juce::Colour { T::Bone });
    g.fillRect(thumb);

    g.setColour(juce::Colour { T::Ink });
    g.drawRect(thumb, 1.0f);
}

// ----------------------------------------------------------------------------
// PopupMenu：掃描線暗井 + 黑字紅底高亮（危險標牌反白）
// ----------------------------------------------------------------------------
void TelemetryLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<float>(0.0f, 0.0f,
                                         static_cast<float>(width),
                                         static_cast<float>(height));

    g.setColour(juce::Colour { T::Void });
    g.fillAll();
    Telemetry::drawScanlines(g, bounds);

    g.setColour(juce::Colour { T::Divider });
    g.drawRect(bounds.reduced(0.5f), 1.0f);
}

void TelemetryLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                             bool isSeparator, bool isActive, bool isHighlighted,
                                             bool isTicked, bool hasSubMenu, const juce::String& text,
                                             const juce::String& shortcutKeyText,
                                             const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused(shortcutKeyText, textColour);

    if (isSeparator)
    {
        auto r = area.reduced(6, 0);
        g.setColour(juce::Colour { T::Divider });
        g.fillRect(r.removeFromTop(1));
        return;
    }

    juce::ignoreUnused(isActive);

    if (isHighlighted)
    {
        // 危險標牌：紅底 + 磷光白字。
        // 注意：降紅後黑字（Ink）對比僅約 3.6:1，改用磷光白可維持 4.6:1 以上。
        g.setColour(juce::Colour { T::HazardRed });
        g.fillRect(area);
        g.setColour(juce::Colour { T::Phosphor });
    }
    else
    {
        g.setColour(juce::Colour { T::Phosphor });
    }

    g.setFont(Telemetry::monoFont(kMonoDataSize));

    auto r = area.toFloat().reduced(10.0f, 0.0f);

    if (isTicked)
    {
        auto tickArea = r.removeFromRight(20.0f);
        g.setColour(isHighlighted ? juce::Colour { T::Ink } : juce::Colour { T::HazardRed });
        g.fillRect(tickArea.getCentreX() - 3.5f, tickArea.getCentreY() - 3.5f, 7.0f, 7.0f);
    }

    if (hasSubMenu)
    {
        auto subArea = r.removeFromRight(16.0f);
        g.drawText(">>", subArea, juce::Justification::centredLeft, false);
    }

    if (icon != nullptr)
    {
        auto iconArea = r.removeFromLeft(juce::jmin(24.0f, r.getHeight()));
        icon->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
    }

    g.drawFittedText(text, r.toNearestInt(), juce::Justification::centredLeft, 1);
}

// ----------------------------------------------------------------------------
// Fonts：等寬字主導
// ----------------------------------------------------------------------------
juce::Font TelemetryLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return Telemetry::monoFont(kMonoDataSize, true);
}

juce::Font TelemetryLookAndFeel::getLabelFont(juce::Label&)
{
    return Telemetry::monoFont(kMonoDataSize);
}

juce::Font TelemetryLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    if (box.getProperties().contains("fontHeight"))
        return Telemetry::monoFont(
            static_cast<float>(static_cast<double>(box.getProperties()["fontHeight"])));

    return Telemetry::monoFont(kMonoFieldSize);
}

juce::Font TelemetryLookAndFeel::getPopupMenuFont()
{
    // 選單會顯示本地化字串（含中文）；等寬 ASCII 字型無 CJK 字形會渲染成方框。
    // 選單文字改用內嵌的 Honor Sans CN；等寬字型保留給數值/程式碼類面板。
    return juce::Font(juce::FontOptions(UiAssets::createHonorSansTypeface())
                          .withHeight(FontScale::scale(kMonoDataSize)));
}

juce::Label* TelemetryLookAndFeel::createComboBoxTextBox(juce::ComboBox& box)
{
    auto* label = new juce::Label();
    label->setFont(getComboBoxFont(box));
    label->setMinimumHorizontalScale(1.0f);

    if (box.getProperties().contains("noArrow"))
        label->setJustificationType(juce::Justification::centred);
    else
        label->setJustificationType(juce::Justification::centredLeft);

    return label;
}

void TelemetryLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setFont(getComboBoxFont(box));
    label.setMinimumHorizontalScale(1.0f);

    if (box.getProperties().contains("noArrow"))
        label.setBounds(0, 0, box.getWidth(), box.getHeight());
    else
        label.setBounds(6, 1, box.getWidth() - 36, box.getHeight() - 2);
}

// ----------------------------------------------------------------------------
// 建構：顯式色彩表（不依賴 LookAndFeel_V4 預設值）
// ----------------------------------------------------------------------------
TelemetryLookAndFeel::TelemetryLookAndFeel()
{
    setColour(juce::TextButton::buttonColourId, juce::Colour { T::ButtonFace });
    setColour(juce::TextButton::buttonOnColourId, juce::Colour { T::HazardRed });
    setColour(juce::TextButton::textColourOffId, juce::Colour { T::Phosphor });
    setColour(juce::TextButton::textColourOnId, juce::Colour { T::Phosphor });

    setColour(juce::Label::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::Label::backgroundColourId, juce::Colour { T::Clear });
    setColour(juce::Label::outlineColourId, juce::Colour { T::Clear });

    setColour(juce::TextEditor::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::TextEditor::backgroundColourId, juce::Colour { T::Ink });
    setColour(juce::TextEditor::outlineColourId, juce::Colour { T::Divider });
    setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour { T::HazardRed });

    setColour(juce::ComboBox::backgroundColourId, juce::Colour { T::ButtonFace });
    setColour(juce::ComboBox::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::ComboBox::outlineColourId, juce::Colour { T::Edge });
    setColour(juce::ComboBox::buttonColourId, juce::Colour { T::ButtonFace });
    setColour(juce::ComboBox::arrowColourId, juce::Colour { T::Phosphor });

    setColour(juce::PopupMenu::backgroundColourId, juce::Colour { T::Void });
    setColour(juce::PopupMenu::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour { T::HazardRed });
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour { T::Ink });
    setColour(juce::PopupMenu::headerTextColourId, juce::Colour { T::PhosphorDim });

    setColour(juce::ScrollBar::trackColourId, juce::Colour { T::Ink });
    setColour(juce::ScrollBar::thumbColourId, juce::Colour { T::Bone });

    setColour(juce::Slider::thumbColourId, juce::Colour { T::Bone });
    setColour(juce::Slider::trackColourId, juce::Colour { T::HazardRed });
    setColour(juce::Slider::backgroundColourId, juce::Colour { T::GridGrey });
    setColour(juce::Slider::textBoxTextColourId, juce::Colour { T::Phosphor });
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colour { T::Divider });

    setColour(juce::ToggleButton::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::ToggleButton::tickColourId, juce::Colour { T::HazardRed });
    setColour(juce::ToggleButton::tickDisabledColourId, juce::Colour { T::PhosphorFaint });

    setColour(juce::ListBox::backgroundColourId, juce::Colour { T::Void });
    setColour(juce::ListBox::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::ListBox::outlineColourId, juce::Colour { T::Divider });

    setColour(juce::ProgressBar::backgroundColourId, juce::Colour { T::Ink });
    setColour(juce::ProgressBar::foregroundColourId, juce::Colour { T::HazardRed });

    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour { T::Surface });
    setColour(juce::TooltipWindow::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::TooltipWindow::outlineColourId, juce::Colour { T::Divider });

    setColour(juce::BubbleComponent::backgroundColourId, juce::Colour { T::Surface });
    setColour(juce::BubbleComponent::outlineColourId, juce::Colour { T::Divider });

    setColour(juce::AlertWindow::backgroundColourId, juce::Colour { T::Void });
    setColour(juce::AlertWindow::textColourId, juce::Colour { T::Phosphor });
    setColour(juce::AlertWindow::outlineColourId, juce::Colour { T::Divider });

    setColour(juce::ResizableWindow::backgroundColourId, juce::Colour { T::Void });
    setColour(juce::DocumentWindow::backgroundColourId, juce::Colour { T::Void });
}

// ----------------------------------------------------------------------------
// 滑鼠游標（與其他主題一致，交由 CursorThemeManager 解析）
// ----------------------------------------------------------------------------
juce::MouseCursor TelemetryLookAndFeel::getMouseCursorFor(juce::Component& component)
{
    return CursorThemeManager::getInstance().resolveCursor(
        LookAndFeel_V4::getMouseCursorFor(component));
}

// ----------------------------------------------------------------------------
// 進程級預設 LookAndFeel
// ----------------------------------------------------------------------------

void TelemetryLookAndFeel::installAsDefault()
{
    auto& instance = getDefaultInstance();
    juce::LookAndFeel::setDefaultLookAndFeel(&instance);
}

TelemetryLookAndFeel& TelemetryLookAndFeel::getDefaultInstance()
{
    static TelemetryLookAndFeel instance;
    return instance;
}

} // namespace OpenTune
