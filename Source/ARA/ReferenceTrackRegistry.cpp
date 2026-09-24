#include "ReferenceTrackRegistry.h"

#include <utility>

namespace OpenTune {

ReferenceTrackRegistry& ReferenceTrackRegistry::getInstance()
{
    static ReferenceTrackRegistry instance;
    return instance;
}

std::string ReferenceTrackRegistry::registerOwner(const std::string& ownerId,
                                                  std::set<std::string> sourceIds)
{
    std::unique_lock<std::mutex> lock(mutex_);
    OwnerState state;
    state.sourceIds = std::move(sourceIds);
    owners_[ownerId] = std::move(state);
    recomputeGroups(lock);
    const auto* group = findOwnerGroup(ownerId);
    return group != nullptr ? *group : std::string();
}

std::string ReferenceTrackRegistry::updateOwnerSources(const std::string& ownerId,
                                                       std::set<std::string> sourceIds)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = owners_.find(ownerId);
    if (it == owners_.end())
    {
        OwnerState state;
        state.sourceIds = std::move(sourceIds);
        owners_[ownerId] = std::move(state);
    }
    else
    {
        it->second.sourceIds = std::move(sourceIds);
    }

    recomputeGroups(lock);
    const auto* group = findOwnerGroup(ownerId);
    return group != nullptr ? *group : std::string();
}

void ReferenceTrackRegistry::unregisterOwner(const std::string& ownerId)
{
    std::unique_lock<std::mutex> lock(mutex_);
    owners_.erase(ownerId);
    recomputeGroups(lock);
}

void ReferenceTrackRegistry::publishRecord(const std::string& ownerId,
                                           PublishedReferenceRecord record)
{
    if (record.modificationPersistentId.empty() || record.sourcePersistentId.empty())
        return;

    std::unique_lock<std::mutex> lock(mutex_);
    auto it = owners_.find(ownerId);
    if (it == owners_.end())
        return; // 未註冊的 owner 不接受發布

    record.publishStamp = nextPublishStamp_++;
    it->second.records[record.modificationPersistentId] = std::move(record);
}

void ReferenceTrackRegistry::removeRecord(const std::string& ownerId,
                                          const std::string& modificationPersistentId)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = owners_.find(ownerId);
    if (it == owners_.end())
        return;
    it->second.records.erase(modificationPersistentId);
}

void ReferenceTrackRegistry::clearOwnerRecords(const std::string& ownerId)
{
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = owners_.find(ownerId);
    if (it == owners_.end())
        return;
    it->second.records.clear();
}

std::vector<ReferenceTrackSummary> ReferenceTrackRegistry::listReferenceTracks(
    const std::string& ownerId,
    const std::string& excludeModificationPersistentId) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    std::vector<ReferenceTrackSummary> result;

    const auto* group = findOwnerGroup(ownerId);
    if (group == nullptr)
        return result;

    for (const auto& ownerId2 : collectGroupOwners(ownerId))
    {
        const auto it = owners_.find(ownerId2);
        if (it == owners_.end())
            continue;

        for (const auto& [modId, record] : it->second.records)
        {
            if (!excludeModificationPersistentId.empty()
                && modId == excludeModificationPersistentId)
                continue;

            ReferenceTrackSummary summary;
            summary.modificationPersistentId = modId;
            summary.sourceName = record.sourceName;
            summary.sequenceName = record.sequenceName;
            summary.contentVersion = record.contentVersion;
            summary.publishStamp = record.publishStamp;
            summary.hasCurve = record.hasUsableCurve();
            summary.regionMappingCount = static_cast<int>(record.regionMappings.size());
            result.push_back(std::move(summary));
        }
    }

    return result;
}

std::shared_ptr<const PublishedReferenceRecord> ReferenceTrackRegistry::queryRecord(
    const std::string& ownerId,
    const std::string& modificationPersistentId) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    const auto* group = findOwnerGroup(ownerId);
    if (group == nullptr)
        return nullptr;

    for (const auto& ownerId2 : collectGroupOwners(ownerId))
    {
        const auto it = owners_.find(ownerId2);
        if (it == owners_.end())
            continue;

        const auto recIt = it->second.records.find(modificationPersistentId);
        if (recIt != it->second.records.end())
            return std::make_shared<const PublishedReferenceRecord>(recIt->second);
    }

    return nullptr;
}

std::vector<std::string> ReferenceTrackRegistry::getGroupMemberOwnerIds(
    const std::string& ownerId) const
{
    std::unique_lock<std::mutex> lock(mutex_);
    return collectGroupOwners(ownerId);
}

void ReferenceTrackRegistry::resetForTesting()
{
    std::unique_lock<std::mutex> lock(mutex_);
    owners_.clear();
    nextPublishStamp_ = 1;
}

void ReferenceTrackRegistry::recomputeGroups(std::unique_lock<std::mutex>&)
{
    // union-find：共享任一 source id 的 owner 必須同群（傳遞閉包合併）。
    std::map<std::string, std::string> parent;

    auto find = [&parent](const std::string& id) -> std::string
    {
        auto it = parent.find(id);
        if (it == parent.end())
        {
            parent[id] = id;
            return id;
        }

        std::string root = id;
        while (parent[root] != root)
            root = parent[root];

        // path compression
        std::string current = id;
        while (parent[current] != root)
        {
            const auto next = parent[current];
            parent[current] = root;
            current = next;
        }
        return root;
    };

    auto unite = [&parent, &find](const std::string& a, const std::string& b)
    {
        const auto ra = find(a);
        const auto rb = find(b);
        if (ra != rb)
            parent[ra] = rb;
    };

    // source id → 包含它的 owner 們
    std::map<std::string, std::vector<std::string>> sourceOwners;
    for (const auto& kv : owners_)
        for (const auto& sourceId : kv.second.sourceIds)
            sourceOwners[sourceId].push_back(kv.first);

    for (const auto& kv : sourceOwners)
    {
        const auto& ownerList = kv.second;
        if (ownerList.empty())
            continue;
        for (size_t i = 1; i < ownerList.size(); ++i)
            unite(ownerList.front(), ownerList[i]);
    }

    // 群組 id = 該連通分量中字典序最小的 ownerId（核心不變時穩定）。
    std::map<std::string, std::string> rootMin;
    for (const auto& kv : owners_)
    {
        const auto root = find(kv.first);
        auto it = rootMin.find(root);
        if (it == rootMin.end() || kv.first < it->second)
            rootMin[root] = kv.first;
    }

    for (auto& kv : owners_)
        kv.second.groupId = rootMin[find(kv.first)];
}

const std::string* ReferenceTrackRegistry::findOwnerGroup(const std::string& ownerId) const
{
    const auto it = owners_.find(ownerId);
    if (it == owners_.end() || it->second.groupId.empty())
        return nullptr;
    return &it->second.groupId;
}

std::vector<std::string> ReferenceTrackRegistry::collectGroupOwners(
    const std::string& ownerId) const
{
    const auto* group = findOwnerGroup(ownerId);
    if (group == nullptr)
        return {};

    std::vector<std::string> members;
    for (const auto& kv : owners_)
    {
        if (kv.second.groupId == *group)
            members.push_back(kv.first);
    }
    return members;
}

} // namespace OpenTune