#pragma once

#include <juce_graphics/juce_graphics.h>
#include "FontScale.h"

namespace OpenTune {
namespace Telemetry {

    // ===========================================================================
    // Tactical Telemetry & CRT Terminal — 純暗色基底（#0A0A0A），禁止混用亮色
    //
    // 規則（industrial-brutalist-ui）：
    //   1. 背景 = 停機 CRT 玻璃（#0A0A0A / #121212），禁止純黑 #000000。
    //   2. 前景 = 白色磷光 #EAEAEA，等寬字主導。
    //   3. 航空危險紅是【唯一】強調色（#C62828——比原始 #E61919 降亮約 22%，
    //      在近黑 CRT 底上不再刺眼，同時保留 3.5:1 以上的辨識度）。
    //   4. 終端綠 #4AF626 只出現一次：StatusReady（單一狀態 LED）。
    //   5. 禁止漸變、柔和投影、圓角（radius = 0，旋钮除外——它是機械撥盤）。
    // ===========================================================================
    struct Colors
    {
        // --- Substrate（基底）---
        static const juce::uint32 Void            = 0xFF0A0A0A; // 停機 CRT 玻璃
        static const juce::uint32 Surface         = 0xFF121212; // 抬升的面板表面
        static const juce::uint32 SurfaceRaised   = 0xFF1A1A1A; // 抬升的控件面
        static const juce::uint32 SurfaceHigh     = 0xFF1F1F1F; // 懸停控件面
        static const juce::uint32 Ink             = 0xFF050505; // 井底（近黑）
        static const juce::uint32 LaneLift        = 0xFF141414; // Piano roll C 音階列
        static const juce::uint32 LaneDeep        = 0xFF0E0E0E; // Piano roll 其他音階列
        static const juce::uint32 TrackPanel      = 0xFF101010; // 音軌面板底色

        // --- Structural（結構線）---
        static const juce::uint32 GridGrey        = 0xFF262626; // 1px 網格線
        static const juce::uint32 Divider         = 0xFF2E2E2E; // 分隔區劃線
        static const juce::uint32 Edge            = 0xFF3A3A3A; // 抬升邊緣 / 輪緣
        static const juce::uint32 TickGrey        = 0xFF333333; // 拍點標記 / 次級線

        // --- Phosphor（磷光前景）---
        static const juce::uint32 Phosphor        = 0xFFEAEAEA; // 白色磷光——主要文字
        static const juce::uint32 PhosphorDim     = 0xFF9E9E9E; // 次級中繼資料
        static const juce::uint32 PhosphorFaint   = 0xFF5A5A5A; // 停用 / 非作用中
        static const juce::uint32 Bone            = 0xFFD0D0D0; // 實心亮塊（音符、滑塊）

        // --- Hazard red —— 唯一的強調色 ---
        // 亮度經過一次下修：#E61919 在 #0A0A0A 上高飽和純紅會產生刺眼暈動。
        // #C62828 相對亮度約低 22%、飽和度約低 23%，仍維持純紅相。
        static const juce::uint32 HazardRed       = 0xFFC62828;
        static const juce::uint32 HazardLift      = 0xFFE04B4B; // 熱狀態 / 削波（僅略亮於基準）
        static const juce::uint32 HazardDeep      = 0xFF7E1414; // 深紅

        // --- 唯一許可的綠——僅限狀態 LED，絕不作為一般文字 ---
        static const juce::uint32 SignalGreen     = 0xFF4AF626;

        // --- Controls（控件）---
        static const juce::uint32 ButtonFace      = 0xFF141414;
        static const juce::uint32 ButtonHover     = 0xFF1F1F1F;
        static const juce::uint32 ButtonOff       = 0xFF1A1A1A;
        static const juce::uint32 KnobFace        = 0xFF161616;

        // --- Display wells（儀表井 / 時間碼）---
        static const juce::uint32 WellTop         = 0xFF060606;
        static const juce::uint32 WellBottom      = 0xFF0A0A0A;
        static const juce::uint32 DisplayDim      = 0x66EAEAEA;

        // --- Keybed（鍵床）---
        static const juce::uint32 KeyWhite        = 0xFFEAEAEA;
        static const juce::uint32 KeyBlack        = 0xFF141414;

        // --- Waveform / pitch data ---
        static const juce::uint32 WaveFill        = 0x4CEAEAEA; // 30% 磷光填充
        static const juce::uint32 PitchShadow     = 0x40EAEAEA;

        // --- Sidebar shell（平面分隔，無光暈）---
        static const juce::uint32 ShellTop        = 0xFF141414;
        static const juce::uint32 ShellMid        = 0xFF111111;
        static const juce::uint32 ShellBottom     = 0xFF0D0D0D;

        // --- Transparent（停用的玻璃 / 輝光 token，保持介面相容）---
        // 注意：不可命名為 None——X11 標頭定義了 None 巨集（Linux 建置會炸）。
        static const juce::uint32 Clear           = 0x00000000;
    };

    struct Style
    {
        // 絕對拒絕圓角：面板 / 控件 / 輸入框全部 0。
        // 唯一例外是旋钮——它是圓形機械撥盤（999 = 由幾何自然約束）。
        static constexpr float PanelRadius        = 0.0f;
        static constexpr float ControlRadius      = 0.0f;
        static constexpr float FieldRadius        = 0.0f;
        static constexpr float KnobRadius         = 999.0f;

        static constexpr float StrokeThin         = 1.0f;
        static constexpr float StrokeThick        = 2.0f;
        static constexpr float FocusRingThickness = 2.0f;

        // 機械介面：零陰影、零輝光、零斜面——層次由 1px 結構線表達。
        static constexpr float ShadowAlpha        = 0.0f;
        static constexpr int   ShadowRadius       = 1;
        static constexpr int   ShadowOffsetX      = 0;
        static constexpr int   ShadowOffsetY      = 1;

        static constexpr float GlowAlpha          = 0.0f;
        static constexpr float GlowRadius         = 0.0f;

        static constexpr float BevelWidth         = 0.0f;
        static constexpr float BevelIntensity     = 0.0f;

        static constexpr float AnimationDurationMs = 80.0f; // 快速機械响应
        static constexpr float HoverGlowIntensity  = 0.0f;

        static constexpr float CornerRadius        = 0.0f;
    };

    // ===========================================================================
    // 繪製基元 —— 供 LookAndFeel 與各元件複用
    // ===========================================================================

    // 等寬字體（telemetry / 資料讀數的唯一字體）
    //
    // 字級套用使用者全域字級係數（FontScale.h），並比照 UIColors::getUIFont 保留
    // 16pt 下限——「先取下限（100% 基準），再乘係數」，所以 80% 仍會真的縮小。
    //
    // clampToMinimum = false 供【呼叫方已自行做高度夾限】的路徑使用，例如
    // drawToggleButton 的 jmin(FontScale::scale(15), height * 0.75)：原有主題在該處
    // 直接 g.setFont(fontSize) 而不經過 UIColors，渲染值就是 15pt；這裡若再墊到 16
    // 反而會與原有主題脫鉤。
    inline juce::Font monoFont(float height, bool bold = false, bool clampToMinimum = true)
    {
        const float h = clampToMinimum ? juce::jmax(16.0f, height) : height;
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                             bold ? "Bold" : "Regular",
                                             FontScale::scale(h)));
    }

    // CRT 掃描線：畫在暗色基底上的磷光刷新帶（淺色細線）。
    inline void drawScanlines(juce::Graphics& g,
                              juce::Rectangle<float> bounds,
                              juce::Colour lineColour = juce::Colour { 0x08FFFFFF },
                              float period = 3.0f)
    {
        if (bounds.isEmpty() || period < 2.0f)
            return;

        juce::Graphics::ScopedSaveState saveState(g);
        g.reduceClipRegion(bounds.toNearestInt());

        g.setColour(lineColour);
        for (float y = bounds.getY(); y < bounds.getBottom(); y += period)
            g.fillRect(bounds.getX(), y, bounds.getWidth(), 1.0f);
    }

    // 危險斜紋：紅 / 虛空黑交替的警示帶（須由呼叫方先填滿背景）。
    inline void drawHazardStripes(juce::Graphics& g,
                                  juce::Rectangle<float> bounds,
                                  float stripeWidth = 7.0f,
                                  juce::Colour stripeColour = juce::Colour { Colors::HazardRed })
    {
        if (bounds.isEmpty() || stripeWidth <= 0.0f)
            return;

        juce::Graphics::ScopedSaveState saveState(g);
        g.reduceClipRegion(bounds.toNearestInt());

        g.setColour(stripeColour);

        const float h = bounds.getHeight();
        const float period = stripeWidth * 2.0f;

        for (float x = bounds.getX() - h; x < bounds.getRight() + h; x += period)
        {
            juce::Path stripe;
            stripe.startNewSubPath(x, bounds.getBottom());
            stripe.lineTo(x + stripeWidth, bounds.getBottom());
            stripe.lineTo(x + stripeWidth + h, bounds.getY());
            stripe.lineTo(x + h, bounds.getY());
            stripe.closeSubPath();
            g.fillPath(stripe);
        }
    }

    // 技術取景框：聚焦元素四角的紅色括標（取代圓角焦點環）。
    inline void drawCornerBrackets(juce::Graphics& g,
                                   juce::Rectangle<float> bounds,
                                   juce::Colour colour,
                                   float arm = 10.0f,
                                   float thickness = 2.0f)
    {
        if (bounds.isEmpty())
            return;

        const float a = juce::jmin(arm, bounds.getWidth() * 0.4f, bounds.getHeight() * 0.4f);
        const float l = bounds.getX();
        const float t = bounds.getY();
        const float r = bounds.getRight() - thickness;
        const float b = bounds.getBottom() - thickness;

        g.setColour(colour);

        // 左上
        g.fillRect(l, t, a, thickness);
        g.fillRect(l, t, thickness, a);
        // 右上
        g.fillRect(l + bounds.getWidth() - a, t, a, thickness);
        g.fillRect(r, t, thickness, a);
        // 左下
        g.fillRect(l, b, a, thickness);
        g.fillRect(l, bounds.getBottom() - a, thickness, a);
        // 右下
        g.fillRect(l + bounds.getWidth() - a, b, a, thickness);
        g.fillRect(r, bounds.getBottom() - a, thickness, a);
    }

} // namespace Telemetry
} // namespace OpenTune
