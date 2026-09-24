# OpenTune

OpenTune 的領域涵蓋音訊內容、編輯狀態、跨格式 persistence，以及可供 Standalone、VST3 與 ARA 播放的算繪結果。

## Language

**Content owner**:
某一格式範圍內持有 `ContentState` 並獨佔其 mutation 與 lifecycle 的權威對象。
_Avoid_: renderer、editor、cache

**ContentKey**:
內容在目前 owner 範圍內的穩定識別；它不表示來源音訊、track 或 placement 本身。
_Avoid_: source id、placement id

**Content snapshot**:
某個已完成 revision 的唯讀編輯內容視圖；它不是 owner 的 live state。
_Avoid_: live state、mutable copy

**Render invalidation**:
宣告部分或全部內容已偏離目前可播放的算繪結果；它描述需要重新驗證的範圍，不指定排程 implementation。
_Avoid_: render request、cache clear

**Playback publication**:
發布一個已通過內容 revision 與算繪完整性條件的最新可讀取結果。
_Avoid_: raw render、partial result
