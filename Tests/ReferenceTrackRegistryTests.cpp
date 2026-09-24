// ReferenceTrackRegistry 純邏輯單元測試。
// 不依賴 ARA host / JUCE GUI，只驗證群組偵測、發布/查詢、版本戳記與隔離語意。

#include "../Source/ARA/ReferenceTrackRegistry.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace OpenTune;

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

ReferenceTrackRegistry& reg()
{
    return ReferenceTrackRegistry::getInstance();
}

std::shared_ptr<const PitchCurveSnapshot> makeCurve(const std::vector<float>& f0)
{
    if (f0.empty())
        return std::make_shared<const PitchCurveSnapshot>(
            std::vector<float>{}, std::vector<float>{},
            std::vector<PitchCorrectionSegment>{}, 256, 16000.0, 1);
    return std::make_shared<const PitchCurveSnapshot>(
        f0, f0, std::vector<PitchCorrectionSegment>{}, 256, 16000.0, 1);
}

PublishedReferenceRecord makeRecord(const std::string& modId,
                                    const std::string& sourceId,
                                    uint64_t contentVersion,
                                    bool withCurve)
{
    PublishedReferenceRecord record;
    record.modificationPersistentId = modId;
    record.sourcePersistentId = sourceId;
    record.sourceName = "Source-" + sourceId;
    record.sequenceName = "Track-" + modId;
    if (withCurve)
        record.curve = makeCurve({440.0f, 442.0f, 441.0f});
    record.contentVersion = contentVersion;
    return record;
}

// ── 群組偵測 ─────────────────────────────────────────────────

bool testGroupDetectionBySharedSource()
{
    reg().resetForTesting();

    const auto groupA = reg().registerOwner("ownerA", {"src1"});
    const auto groupB = reg().registerOwner("ownerB", {"src1", "src2"}); // 共享 src1
    const auto groupC = reg().registerOwner("ownerC", {"src3"});          // 無交集

    check(groupA == groupB, "共享 source 的 owner 應同群");
    check(groupA != groupC, "無共享 source 的 owner 應不同群");

    check(reg().getGroupMemberOwnerIds("ownerA").size() == 2,
          "ownerA 群組應含兩個 owner");

    return g_failures == 0;
}

bool testTransitiveClosureMerging()
{
    reg().resetForTesting();

    // A 與 B 共享 src1；B 與 C 共享 src2 ⇒ A,B,C 同群
    reg().registerOwner("ownerA", {"src1"});
    reg().registerOwner("ownerB", {"src1", "src2"});
    const auto groupC = reg().registerOwner("ownerC", {"src2"});

    const auto groupA = reg().getGroupMemberOwnerIds("ownerA");
    check(groupA.size() == 3, "傳遞閉包：A,B,C 應同群");
    check(reg().getGroupMemberOwnerIds("ownerC") == groupA, "C 群組應與 A 一致");
    check(!groupC.empty(), "registerOwner 應回傳非空群組");

    return g_failures == 0;
}

bool testGroupMergeOnUpdate()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});
    const auto groupA = reg().registerOwner("ownerB", {"src2"});
    check(reg().getGroupMemberOwnerIds("ownerA") != reg().getGroupMemberOwnerIds("ownerB"),
          "初始應不同群");

    // B 之後 gain src1 ⇒ 併入 A 群
    const auto merged = reg().updateOwnerSources("ownerB", {"src2", "src1"});
    check(reg().getGroupMemberOwnerIds("ownerA").size() == 2, "更新後應合併為同一群");
    check(reg().getGroupMemberOwnerIds("ownerA") == reg().getGroupMemberOwnerIds("ownerB"),
          "A/B 群組 id 應一致");
    check(!merged.empty(), "updateOwnerSources 應回傳群組");

    return g_failures == 0;
}

bool testUnregisterRemovesFromGroup()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});
    reg().registerOwner("ownerB", {"src1"});
    check(reg().getGroupMemberOwnerIds("ownerA").size() == 2, "註冊後同群");

    reg().unregisterOwner("ownerB");
    const auto members = reg().getGroupMemberOwnerIds("ownerA");
    check(members.size() == 1 && members.front() == "ownerA",
          "註銷後群組應只含 ownerA");

    return g_failures == 0;
}

// ── 發布 / 查詢 / 隔離 ───────────────────────────────────────

bool testPublishAndQueryWithinGroup()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});
    reg().registerOwner("ownerB", {"src1"});

    reg().publishRecord("ownerB", makeRecord("modB1", "src1", 7, true));

    const auto tracks = reg().listReferenceTracks("ownerA");
    check(tracks.size() == 1, "ownerA 應能看到 ownerB 發布的參考軌");
    if (!tracks.empty())
    {
        check(tracks[0].modificationPersistentId == "modB1", "摘要 id 正確");
        check(tracks[0].hasCurve, "摘要應回報 hasCurve");
        check(tracks[0].contentVersion == 7, "摘要應帶 contentVersion");
        check(tracks[0].sequenceName == "Track-modB1", "摘要應帶 sequenceName");
    }

    const auto record = reg().queryRecord("ownerA", "modB1");
    check(record != nullptr, "ownerA 應能 query 到 ownerB 的記錄");
    if (record != nullptr)
    {
        check(record->hasUsableCurve(), "記錄應有可用曲線");
        check(record->curve->getOriginalF0()[1] == 442.0f, "曲線資料正確");
    }

    return g_failures == 0;
}

bool testCrossGroupIsolation()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});
    reg().registerOwner("ownerB", {"src2"}); // 不同群

    reg().publishRecord("ownerB", makeRecord("modB1", "src2", 1, true));

    check(reg().listReferenceTracks("ownerA").empty(),
          "不同群組的記錄不可見");
    check(reg().queryRecord("ownerA", "modB1") == nullptr,
          "跨群組 query 應為 null");

    return g_failures == 0;
}

bool testExcludeSelfModification()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});
    reg().registerOwner("ownerB", {"src1"});
    reg().publishRecord("ownerA", makeRecord("modA1", "src1", 1, true));
    reg().publishRecord("ownerB", makeRecord("modB1", "src1", 2, true));

    const auto withoutExclude = reg().listReferenceTracks("ownerA");
    check(withoutExclude.size() == 2, "未排除應列兩軌");

    const auto excluded = reg().listReferenceTracks("ownerA", "modA1");
    check(excluded.size() == 1 && excluded.front().modificationPersistentId == "modB1",
          "排除自己的 modification 後應只剩另一軌");

    return g_failures == 0;
}

bool testPublishStampAndVersionUpdate()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});

    reg().publishRecord("ownerA", makeRecord("modA1", "src1", 1, true));
    const auto first = reg().queryRecord("ownerA", "modA1");
    check(first != nullptr && first->publishStamp != 0, "首次發布應有 publishStamp");

    // 覆寫同 id（版本提升）⇒ publishStamp 遞增
    reg().publishRecord("ownerA", makeRecord("modA1", "src1", 2, true));
    const auto second = reg().queryRecord("ownerA", "modA1");
    check(second != nullptr && second->contentVersion == 2, "覆寫應更新 contentVersion");
    check(first != nullptr && second != nullptr
              && second->publishStamp > first->publishStamp,
          "覆寫應取得更大 publishStamp");

    // removeRecord
    reg().removeRecord("ownerA", "modA1");
    check(reg().queryRecord("ownerA", "modA1") == nullptr, "removeRecord 後查無");

    return g_failures == 0;
}

bool testInvalidRecordsRejected()
{
    reg().resetForTesting();

    reg().registerOwner("ownerA", {"src1"});

    PublishedReferenceRecord empty;
    reg().publishRecord("ownerA", empty);
    check(reg().listReferenceTracks("ownerA").empty(), "空記錄不應被發布");

    // 未註冊的 owner 發布應被忽略
    reg().publishRecord("ghostOwner", makeRecord("modG", "src1", 1, true));
    check(reg().listReferenceTracks("ownerA").empty(), "未註冊 owner 發布應被忽略");

    // 未分析（無 curve）記錄仍可發布並列出，但回報 hasCurve=false
    reg().publishRecord("ownerA", makeRecord("modA1", "src1", 1, false));
    const auto tracks = reg().listReferenceTracks("ownerA");
    check(tracks.size() == 1 && !tracks[0].hasCurve, "無 curve 記錄應列出且 hasCurve=false");
    const auto record = reg().queryRecord("ownerA", "modA1");
    check(record != nullptr && !record->hasUsableCurve(), "無 curve 記錄 query 應回報不可用");

    return g_failures == 0;
}

} // namespace

int main()
{
    check(testGroupDetectionBySharedSource(), "testGroupDetectionBySharedSource");
    check(testTransitiveClosureMerging(), "testTransitiveClosureMerging");
    check(testGroupMergeOnUpdate(), "testGroupMergeOnUpdate");
    check(testUnregisterRemovesFromGroup(), "testUnregisterRemovesFromGroup");
    check(testPublishAndQueryWithinGroup(), "testPublishAndQueryWithinGroup");
    check(testCrossGroupIsolation(), "testCrossGroupIsolation");
    check(testExcludeSelfModification(), "testExcludeSelfModification");
    check(testPublishStampAndVersionUpdate(), "testPublishStampAndVersionUpdate");
    check(testInvalidRecordsRejected(), "testInvalidRecordsRejected");

    if (g_failures == 0)
    {
        std::printf("ReferenceTrackRegistryTests: all passed\n");
        return 0;
    }

    std::printf("ReferenceTrackRegistryTests: %d failure(s)\n", g_failures);
    return 1;
}