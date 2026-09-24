#include "ContentRenderRouting.h"

namespace OpenTune {

bool isActiveContentItem(const ContentKey& itemContentKey,
                         const ContentKey& editedContentKey)
{
    // 兩者皆須有效：edited key 無效代表沒有焦點（host 尚未提供 selection），
    // 此時不應把任何 item 當成 active，否則會誤判第一個 item 為處理軌。
    return editedContentKey.isValid() && itemContentKey.isValid()
        && itemContentKey == editedContentKey;
}

ContentRenderRouting routeContentRenderItems(
    const std::vector<ContentKey>& itemContentKeys,
    const ContentKey& editedContentKey,
    const HiddenReferenceSet& hiddenReferences)
{
    ContentRenderRouting routing;
    routing.active.reserve(itemContentKeys.size());

    for (std::size_t i = 0; i < itemContentKeys.size(); ++i)
    {
        const auto& key = itemContentKeys[i];

        if (isActiveContentItem(key, editedContentKey))
        {
            // active 優先於 hidden：選單不能關閉正在編輯的那一軌。
            routing.active.push_back(i);
            continue;
        }

        if (hiddenReferences.count(key) != 0)
            routing.hiddenReference.push_back(i);
        else
            routing.visibleReference.push_back(i);
    }

    return routing;
}

} // namespace OpenTune
