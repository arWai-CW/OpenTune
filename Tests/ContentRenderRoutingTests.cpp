// ContentRenderRouting 單元測試（純邏輯，無 JUCE / ARA / host 依賴）
//
// 保護的行為：鋼琴卷簾必須把「正在處理的 track」與「同 document 其他 track」
// 分流，後者只畫灰色 F0 曲線且可由參考軌選單開關。此分流若退化成全部完整
// 渲染，多 ARA instance 會視覺混淆且無任何測試可察覺。

#include "Standalone/UI/ContentRenderRouting.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace OpenTune;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

ContentKey makeKey(std::uint64_t objectId)
{
    ContentKey key;
    key.domainKind = DomainKind::ARAAudioModification;
    key.objectId = objectId;
    return key;
}

// 驗證三組索引互斥且完整覆蓋所有輸入位置（每個 index 恰好出現一次）。
void checkPartitionIsExact(const std::vector<ContentKey>& keys,
                           const ContentRenderRouting& routing,
                           const char* what)
{
    std::vector<int> seen(keys.size(), 0);
    for (std::size_t i : routing.active)          { seen[i] += 1; }
    for (std::size_t i : routing.visibleReference){ seen[i] += 1; }
    for (std::size_t i : routing.hiddenReference) { seen[i] += 1; }

    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        if (seen[i] != 1)
        {
            std::printf("FAIL: %s — index %zu 出現 %d 次（應為 1）\n",
                        what, i, seen[i]);
            ++g_failures;
        }
    }
}

void testSingleMatchingItemIsActive()
{
    const std::vector<ContentKey> keys{ makeKey(1) };
    const auto routing = routeContentRenderItems(keys, makeKey(1), {});

    check(routing.active.size() == 1 && routing.active[0] == 0,
          "單一且匹配 → 應為 active");
    check(routing.visibleReference.empty(), "不應有可見參考軌");
    check(routing.hiddenReference.empty(), "不應有隱藏參考軌");
    checkPartitionIsExact(keys, routing, "singleActive");
}

void testTwoItemsSplitsActiveAndReference()
{
    const std::vector<ContentKey> keys{ makeKey(1), makeKey(2) };
    const auto routing = routeContentRenderItems(keys, makeKey(1), {});

    check(routing.active.size() == 1 && routing.active[0] == 0,
          "track 1 應為 active");
    check(routing.visibleReference.size() == 1 && routing.visibleReference[0] == 1,
          "track 2 應為可見參考軌");
    checkPartitionIsExact(keys, routing, "twoItems");
}

void testHiddenReferenceIsNotDrawn()
{
    const std::vector<ContentKey> keys{ makeKey(1), makeKey(2) };
    const HiddenReferenceSet hidden{ makeKey(2) };
    const auto routing = routeContentRenderItems(keys, makeKey(1), hidden);

    check(routing.active.size() == 1 && routing.active[0] == 0,
          "active 不受隱藏集合影響");
    check(routing.visibleReference.empty(), "被選單關閉的參考軌不應可見");
    check(routing.hiddenReference.size() == 1 && routing.hiddenReference[0] == 1,
          "被關閉的參考軌應歸入 hiddenReference");
    checkPartitionIsExact(keys, routing, "hiddenReference");
}

void testHiddenSetCannotHideEditedTrack()
{
    // 使用者正在編輯的那一軌，即使誤入 hiddenReferences 仍必須是 active ——
    // 否則會出現「焦點在 A、但 A 不顯示且無法編輯」的狀態。
    const std::vector<ContentKey> keys{ makeKey(1), makeKey(2) };
    const HiddenReferenceSet hidden{ makeKey(1), makeKey(2) };
    const auto routing = routeContentRenderItems(keys, makeKey(1), hidden);

    check(routing.active.size() == 1 && routing.active[0] == 0,
          "正在編輯的 track 必須維持 active");
    check(routing.hiddenReference.size() == 1 && routing.hiddenReference[0] == 1,
          "非 active 的 track 仍依選單隱藏");
    checkPartitionIsExact(keys, routing, "cannotHideEdited");
}

void testMultiClipOnSameTrack()
{
    // 同一軌多 clip（Melodyne 單 clip 模式）：只有 focus 的那個是 active，
    // 其餘 clip 為參考軌，可由選單整軌開關。
    const std::vector<ContentKey> keys{ makeKey(10), makeKey(11), makeKey(12) };
    const auto routing = routeContentRenderItems(keys, makeKey(11), {});

    check(routing.active.size() == 1 && routing.active[0] == 1,
          "只有 focus 的 clip 是 active");
    check(routing.visibleReference.size() == 2,
          "同軌其餘 clip 應為參考軌");
    check(routing.visibleReference[0] == 0 && routing.visibleReference[1] == 2,
          "參考軌索引順序應穩定");
    checkPartitionIsExact(keys, routing, "multiClip");
}

void testInvalidEditedKeyMakesEverythingReference()
{
    // 沒有焦點時不得把第一個 item 當 active（那會造成「誤改別軌」）。
    const std::vector<ContentKey> keys{ makeKey(1), makeKey(2) };
    const auto routing = routeContentRenderItems(keys, ContentKey{}, {});

    check(routing.active.empty(), "edited key 無效時不應有 active");
    check(routing.visibleReference.size() == 2, "全部應為參考軌");
    checkPartitionIsExact(keys, routing, "invalidEditedKey");
}

void testEditedKeyNotPresentMakesEverythingReference()
{
    const std::vector<ContentKey> keys{ makeKey(1), makeKey(2) };
    const auto routing = routeContentRenderItems(keys, makeKey(99), {});

    check(routing.active.empty(), "焦點不在清單中時不應有 active");
    check(routing.visibleReference.size() == 2, "全部應為參考軌");
    checkPartitionIsExact(keys, routing, "editedKeyAbsent");
}

void testEmptyInput()
{
    const auto routing = routeContentRenderItems({}, makeKey(1), {});
    check(routing.active.empty() && routing.visibleReference.empty()
              && routing.hiddenReference.empty(),
          "空輸入應回傳空分流");
}

void testIsActiveContentItem()
{
    check(isActiveContentItem(makeKey(1), makeKey(1)), "相同 key → active");
    check(!isActiveContentItem(makeKey(1), makeKey(2)), "不同 key → 非 active");
    check(!isActiveContentItem(makeKey(1), ContentKey{}),
          "edited 無效 → 非 active");
    check(!isActiveContentItem(ContentKey{}, makeKey(1)),
          "item 無效 → 非 active");
}

} // namespace

int main()
{
    testSingleMatchingItemIsActive();
    testTwoItemsSplitsActiveAndReference();
    testHiddenReferenceIsNotDrawn();
    testHiddenSetCannotHideEditedTrack();
    testMultiClipOnSameTrack();
    testInvalidEditedKeyMakesEverythingReference();
    testEditedKeyNotPresentMakesEverythingReference();
    testEmptyInput();
    testIsActiveContentItem();

    if (g_failures == 0)
    {
        std::printf("ContentRenderRoutingTests: all passed\n");
        return 0;
    }

    std::printf("ContentRenderRoutingTests: %d failure(s)\n", g_failures);
    return 1;
}
