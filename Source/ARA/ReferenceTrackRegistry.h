#pragma once

#include "../Utils/PitchCurve.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace OpenTune {

// 每個 playback region 的 modification-time ↔ playback-time 對映。
// 用於把其他 track 的曲線疊到主 editor 的 playback-time 時間軸上。
// 注意：ARA 規格只有 AudioSource / AudioModification 有 persistent ID，
// playback region 沒有 —— 故對映以「同一 modification 的多個 region 時間對」
// 表達即可，region 本身無需 id。
struct ReferenceRegionMapping
{
    double startModificationTime{0.0};
    double endModificationTime{0.0};
    double startPlaybackTime{0.0};
    double endPlaybackTime{0.0};

    bool isValid() const noexcept
    {
        return startModificationTime >= 0.0
            && endModificationTime > startModificationTime + 1e-12
            && startPlaybackTime >= 0.0
            && endPlaybackTime > startPlaybackTime + 1e-12;
    }
};

// 由 OpenTuneDocumentController 發布的跨 instance 參考資料。
// curve 為 immutable PitchCurveSnapshot（shared_ptr 零複製共享）。
struct PublishedReferenceRecord
{
    std::string modificationPersistentId;   // document 內唯一；同群組內唯一 key 後半
    std::string sourcePersistentId;         // 群組偵測與 source 比對用
    std::string sourceName;                 // 顯示標籤
    std::string sequenceName;               // 顯示標籤（Region Sequence ≈ track）
    std::vector<ReferenceRegionMapping> regionMappings;

    std::shared_ptr<const PitchCurveSnapshot> curve;
    // 源頭版本戳記：content/pitch revision 提升時遞增；UI 依此判斷是否重取。
    uint64_t contentVersion{0};
    // registry 層級發布戳記：每次 publishRecord 全域遞增。
    uint64_t publishStamp{0};

    bool isValid() const noexcept { return !modificationPersistentId.empty() && !sourcePersistentId.empty(); }
    bool hasUsableCurve() const noexcept
    {
        return curve != nullptr && !curve->isEmpty() && curve->hasOriginalF0Data();
    }
};

// 輕量摘要（不帶 curve），供 UI 輪詢版本變化。
struct ReferenceTrackSummary
{
    std::string modificationPersistentId;
    std::string sourceName;
    std::string sequenceName;
    uint64_t contentVersion{0};
    uint64_t publishStamp{0};
    bool hasCurve{false};
    int regionMappingCount{0};
};

// process 全域單例：跨 ARA instance（同 document / 同專案）發布與讀取參考曲線。
// 群組 =「共享任一 audio source persistent ID」的 owner 連通分量（REAPER pooled
// edits 語意與 ARA「document 跨 instance 共享」設計一致）。
class ReferenceTrackRegistry
{
public:
    static ReferenceTrackRegistry& getInstance();

    // 註冊 / 更新 owner（DocumentController）的 audio source persistent ID 集合，
    // 並重算跨 owner 群組。回傳該 owner 目前所屬的群組 id（不保證跨更新穩定，
    // 但群組核心不變時以最小 ownerId 為群組 id，保持穩定）。
    std::string registerOwner(const std::string& ownerId, std::set<std::string> sourceIds);
    std::string updateOwnerSources(const std::string& ownerId, std::set<std::string> sourceIds);
    void unregisterOwner(const std::string& ownerId);

    // 發布一筆記錄（owner 自己的 modification）。modificationPersistentId 在 owner 內唯一；
    // 重複發布同 id 以新記錄覆寫並取得新 publishStamp。
    void publishRecord(const std::string& ownerId, PublishedReferenceRecord record);
    void removeRecord(const std::string& ownerId, const std::string& modificationPersistentId);
    void clearOwnerRecords(const std::string& ownerId);

    // 查詢側：列舉與 owner 同群組（含自己）的參考軌摘要；exclude 為目前主軌的
    // modificationPersistentId（不把自己列為參考）。
    std::vector<ReferenceTrackSummary> listReferenceTracks(
        const std::string& ownerId,
        const std::string& excludeModificationPersistentId = {}) const;

    // 取完整記錄（含 curve）；查無回傳 nullptr。
    std::shared_ptr<const PublishedReferenceRecord> queryRecord(
        const std::string& ownerId,
        const std::string& modificationPersistentId) const;

    std::vector<std::string> getGroupMemberOwnerIds(const std::string& ownerId) const;

    // 測試輔助：清除全部狀態。不應在生產路徑使用。
    void resetForTesting();

private:
    ReferenceTrackRegistry() = default;

    void recomputeGroups(std::unique_lock<std::mutex>& lock);
    const std::string* findOwnerGroup(const std::string& ownerId) const;
    std::vector<std::string> collectGroupOwners(const std::string& ownerId) const;

    struct OwnerState
    {
        std::set<std::string> sourceIds;
        std::map<std::string, PublishedReferenceRecord> records;
        std::string groupId;
    };

    std::map<std::string, OwnerState> owners_;
    uint64_t nextPublishStamp_{1};
    mutable std::mutex mutex_;
};

} // namespace OpenTune