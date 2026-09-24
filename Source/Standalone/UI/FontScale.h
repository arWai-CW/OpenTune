#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace OpenTune {

// ============================================================================
// 全域字級縮放係數（使用者偏好設定「字級」）
//
// 獨立小模組的原因：UIColors.h 會包含 ThemeTokens.h，而 ThemeTokens.h 又包含
// 各主題標頭（含 TelemetryTheme.h）。因此主題標頭無法回頭引用 UIColors.h，
// 字級係數必須放在一個雙方都能包含、且依賴極輕的檔案裡。
//
// 唯一真相存放在 atomic，偏好設定載入時寫入（AppPreferences::load() 與
// setFontSizePercent()），所有字型工廠與畫布繪製在建立 Font 的瞬間讀取。
// ============================================================================

struct FontScale {
    /// 使用者字級百分比，100 = 100%（原始字級）。範圍 50–300。
    static int percent() noexcept
    {
        return percent_.load(std::memory_order_relaxed);
    }

    static void setPercent(int value) noexcept
    {
        percent_.store(std::clamp(value, 50, 300), std::memory_order_relaxed);
    }

    /// 縮放係數（1.0f = 100%）。
    static float factor() noexcept
    {
        return static_cast<float>(percent()) * 0.01f;
    }

    /// 將基準字級乘上使用者字級係數。
    ///
    /// 字型工廠一律「先取原本的下限（jmax），再乘係數」，因此 80% 會真的縮小，
    /// 而不會被下限擋住；150% 也會一致放大。
    static float scale(float baseHeight) noexcept
    {
        return baseHeight * factor();
    }

    /// 整數版：用於跟著字級一起縮放的版面高度（如選單列高度）。
    static int scaleInt(int basePixels) noexcept
    {
        return static_cast<int>(std::lround(scale(static_cast<float>(basePixels))));
    }

private:
    static inline std::atomic<int> percent_ { 100 };
};

} // namespace OpenTune
