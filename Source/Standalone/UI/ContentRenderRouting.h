#pragma once

#include "Content/ContentKey.h"

#include <cstddef>
#include <set>
#include <vector>

namespace OpenTune {

/**
 * 被「參考軌選單」關閉的 reference track。
 *
 * 語意：以 modification content 為單位；不含正在處理的 track（選單無法關閉
 * 自己正在編輯的那一軌，見 routeContentRenderItems）。
 */
using HiddenReferenceSet = std::set<ContentKey>;

/**
 * Piano Roll content 渲染分流結果。
 *
 * 背景：同一份 ARA document 可能同時含多個 instance 的 track（REAPER 讓同
 * project 的多個 instance 共用一份 document），因此鋼琴卷簾會拿到多個
 * content item。使用者只能編輯其中一個（focused），其餘應降級為「唯讀灰色
 * 參考曲線」——只畫 F0，不畫音符方塊 / 波形 / time grid anchor。
 *
 * 這兩個索引清單是 drawContent 的唯一依據，抽出成純函式以便單元測試：
 * 該分流邏輯曾因未受保護而退化成「全部完整渲染」，造成多 instance 視覺混淆
 * 且無法察覺。
 */
struct ContentRenderRouting
{
    /// 正在處理的 track：完整渲染（波形 / unvoiced / 音符方塊 / 彩色 F0 / anchor）
    std::vector<std::size_t> active;
    /// 參考軌且選單為開啟：只畫灰色 F0 曲線
    std::vector<std::size_t> visibleReference;
    /// 參考軌但選單已關閉：不畫任何內容
    std::vector<std::size_t> hiddenReference;
};

/**
 * 判定某個 content item 是否為「正在處理的 track」。
 *
 * 判定條件是 contentKey == editedContentKey_（焦點由 host 的 editor view
 * selection 決定，非依文件順序猜測）。焦點無效時沒有 active item。
 */
bool isActiveContentItem(const ContentKey& itemContentKey,
                         const ContentKey& editedContentKey);

/**
 * 依 edited key 與選單隱藏集合，把 content items 分流為 active / 可見參考 /
 * 隱藏參考三組。索引對應輸入 vector 的位置，順序穩定（便於 diff 與測試）。
 *
 * 不變條件：
 *  - 同一個 index 只會出現在其中一組
 *  - edited key 對應的 item 永遠是 active，即使它出現在 hiddenReferences
 *    （選單不能把使用者正在編輯的那一軌關掉）
 *  - hiddenReferences 內非 active 的 item 歸入 hiddenReference
 */
ContentRenderRouting routeContentRenderItems(
    const std::vector<ContentKey>& itemContentKeys,
    const ContentKey& editedContentKey,
    const HiddenReferenceSet& hiddenReferences);

} // namespace OpenTune
