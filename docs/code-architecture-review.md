# OpenTune 架构现状

本文只记录当前代码的架构、主要功能逻辑和未解决问题。历史迁移、审查过程和已完成重构不在本文展开。

## 1. 架构现状

### 1.1 构建边界

```text
OpenTune VST3 + ARA
  ├─ PluginProcessor
  ├─ ARA / Capture
  ├─ Content / Render / Runtime / Inference / DSP / Utils
  └─ VST3 wrapper

OpenTuneStandalone
  ├─ PluginProcessor
  ├─ StandaloneContent / SourceStore / Arrangement / Project
  ├─ Content / Render / Runtime / Inference / DSP / Utils
  └─ Standalone wrapper
```

VST3 和 Standalone 使用各自的 SharedCode target。公共源码分别编译到两个产品，ARA/Capture 只进入 VST3 target，Standalone 专属源码只进入 Standalone target。格式差异由编译目标和现有 adapter 表达，不在运行时保留第二套格式路径。

### 1.2 模块职责

| 模块 | 当前职责 |
| --- | --- |
| `PluginProcessor` | JUCE 生命周期、音訊處理、格式裝配、transport、content owner 查找與 owner mutation；不再建立 `RenderJob` 或解碼 Stage1/Stage2 gate |
| `ContentState` | Standalone、Capture、ARA 共用的 owner 資料狀態；包含 source window、PCM 引用、分析狀態、編輯狀態和 revision |
| Content owner | 持有 `ContentState`，執行域內 mutation 和生命週期；持久化由域外 adapter 讀寫其 content |
| `EditableContentSnapshot` | 跨執行緒和跨 module 的唯讀內容合同 |
| `ContentSnapshotProjection` | 集中完成 `ContentState` 與 snapshot 的雙向純資料投影 |
| `ContentRenderService` | owner mutation 後的單一 render interface；吸收 playback publication、local/full/TimeGrid invalidation、Stage1 planning、canonical settlement 與 Stage2 gate，caller 不再建立 `RenderJob` |
| `RenderWorker` | 執行 Stage1 identity queue 與 Stage2 queue；Stage1 以 `(RenderCache, chunk start)` index 做 O(log n) 查找與去重，adapter 仍由外部注入 |
| `ProcessF0Runtime` / `ProcessRenderRuntime` | 进程级 F0 与 render DSP orchestration；`ProcessRenderRuntime` 不再擁有 vocoder domain、generation 或 control worker |
| `VocoderExecutionCoordinator` | 可注入的 vocoder execution module；擁有 model/backend lifecycle、lazy initialization、generation-safe submit 與 reconfigure control worker，不處理 PCM 或 RenderCache |
| `OpenTuneDocumentController` | ARA document-scoped content、宿主通知、ARA mutation、CRS 和 ARA F0 生命周期 |
| `ReferenceTrackRegistry` | 进程级单例；跨 document 的 ARA instance 參考軌發布與群組偵測（共享任一 audio source persistent ID 的 owner 連通分量）。同 document 內的參考軌不走此路徑，見 §2.6 |
| `ContentRenderRouting` | 纯函数；把鋼琴卷簾的 content items 分流為「正在處理」與「唯讀參考軌」，並套用參考軌選單的隱藏集合。不得在此引入任何繪製或 ARA 依賴 |
| Editor / PianoRoll | UI 状态、交互、视图投影和格式特有接线；PianoRoll 不直接依赖 Processor |

`PluginProcessor` 仍是最大的協調點，但已退出 render transaction implementation。它保留 JUCE 生命周期、RT 音訊處理、format adapter 與 owner mutation；不得再承擔新的資料模型、`RenderJob` construction 或通用 Router 職責。

### 1.3 内容状态

`ContentState` 是三个 content owner 的唯一 owner schema：

- `sourceWindow`、`audioBuffer`、`sampleRate`、`audioRevision`
- `AnalysisState analysis`：pitch curve、Original F0、DetectedKey、silent gaps、reference features、`analysisRevision` 和分析状态
- notes、TimeGrid、pitch shift、volume envelope、note topology
- 分项 revision 和单一 `contentRevision`

ARA 的 `audioBuffer` 保持为空，PCM 由 `AudioSource` 提供。Standalone 与 Capture 的 retire/revive 状态属于各自 owner，不放进共享 `ContentState`。Render cache、worker、stretcher 和 playback publisher 也不属于 `ContentState`。

owner snapshot 统一通过 `makeContentSnapshot()` 生成；split、clone 等需要重建 owner state 的路径使用 `contentStateFromSnapshot()`。空 `timeGrid` 在投影时使用 `TimeGridSnapshot::bootstrapIdentity()`。ARA 只在共同投影后补充缓存的 AudioSource shape 元数据。

### 1.4 线程与所有权

```text
消息线程
  ├─ owner mutation、持久化、ARA SDK 对象、宿主通知
  └─ render/F0 job 调度

音频线程
  ├─ processBlock / ARA renderer
  └─ 只读取 atomic snapshot、prepared PCM 和已发布 cache/source

Render worker
  └─ 消費 Stage1/Stage2 job 與不可變 input；以 identity index 同步 Stage1 queue，不直接解參考 ARA DC object

F0 / Reference worker
  └─ 异步分析，通过 owner revision 和 completion gate 回写
```

当前设计优先使用不可变 snapshot 和 atomic `shared_ptr`，实时路径不新增锁和动态状态迁移。

## 2. 功能逻辑

### 2.1 Standalone 导入与播放

```text
文件导入
  -> SourceStore 保存源音频和 identity
  -> StandaloneContentRepository 创建 content owner
  -> ContentState 保存 source window、PCM、编辑和分析状态
  -> ContentRenderService 发布 PlaybackReadSource
  -> F0 分析
  -> owner mutation / snapshot
  -> ContentRenderService::invalidateContent
  -> Stage1 render
  -> canonical settlement
  -> Standalone 非恆等 TimeGrid 進 Stage2；ARA 僅接受 identity TimeGrid
  -> playback snapshot / 音訊讀取
```

Arrangement 只保存 placement 的几何与混音/引用参数、track 状态和 timeline 几何。placement 通过 `ContentKey` 找到 content snapshot，不复制内容字段。

### 2.2 Regular VST3 Capture

```text
宿主音频 block
  -> CaptureSession / ring buffer
  -> CaptureSegment
  -> CaptureSegmentContent
  -> F0ExtractionService
  -> commitSegmentF0Result
  -> ContentRenderService::invalidateContent
  -> RenderWorker / canonical settlement
  -> PlaybackReadSource
```

F0 成功提交使用一次性 `applyOriginalF0()`：同时写入 curve、Ready、`analysisRevision`、`pitchRevision` 和 `contentRevision`。render gate 以 `hasUsableOriginalF0()` 为准，不单独相信状态枚举；Capture 直接检查 `PitchCurve`，其他 render 路径检查 snapshot helper。Capture persistence restore 对 Ready 且有曲线的数据复用同一提交路径。

### 2.3 ARA2

```text
ARA host
  -> OpenTuneDocumentController
  -> AudioSource / AudioModification / PlaybackRegion
  -> ContentKey 和 document-scoped ContentState
  -> Processor / owner mutation 完成後通知 document CRS
  -> ContentRenderService::invalidateContent
  -> PlaybackReadSource / canonical Stage1
  -> OpenTunePlaybackRenderer
```

ARA SDK 对象和宿主 placement 只在 DC 消息线程访问。`PlaybackRegion` 只保存 host pointer；projection 只保存宿主 placement 属性和 `ContentKey`，不复制 `SourceWindow` 或其他 content state。可渲染 placement 要求 playback duration 与 modification duration 相等。渲染和编辑视图需要 source window 时，从对应 CRS `PlaybackReadSource.contentSnapshot` 读取。

ARA 只接受 identity TimeGrid。`applyTimeGrid()` 对非恒等 TimeGrid 直接返回失败；归档恢复遇到非恒等 TimeGrid 时返回 `nullopt`，不替换为 identity。

AudioModification clone 分为四步：

1. SDK 调用 clone hook，创建新 SDK modification 并按新 host pointer 注册 DTO。
2. clone hook 通过 `makeContentSnapshot()` / `contentStateFromSnapshot()` 复制可持久化 content；已完成的 F0 重建为独立可变 `PitchCurve`，不复制未完成的异步分析状态。
3. hook 返回，不读取 PCM、不启动分析、不通知宿主。
4. SDK 写入新 persistent ID 后回调 property update；DC 在此绑定 ContentKey、attach source、刷新 projection/CRS 并通知宿主。

### 2.4 ARA 多 instance 與參考軌

宿主（實測 REAPER）讓**同一 project 內的多個 ARA instance 綁定同一份 ARA document**。實測結果是每個 instance 會有兩個 processor（editor 與 playback 分開建立），而兩者、以及不同 instance 之間，**共用同一個 `OpenTuneDocumentController`**。

由此得到的架構事實（皆為實測，非推測）：

1. `EditorView::getEffectivePlaybackRegions()` 回傳**整份 document** 的 playback regions，不是當前 instance 的。ARA 規格即如此規定。
2. `OpenTunePlaybackRenderer` 的 region 指派集合（`addPlaybackRegion`）**確實是 per-instance**，每個 renderer 只拿到自己那一條 track。
3. 但開啟 GUI 的 processor **沒有 playback renderer**（`playbackRenderer=false`），而它的 `ARAEditorRenderer::getPlaybackRegions()` 在此 host 實測為**空**。因此**沒有任何 ARA 訊號能讓 editor 知道自己 instance 的 region**。
4. 焦點解析本身是**正確的**：`getFocusedEditorPlaybackRegionProjection()` 取自宿主 editor view selection，是 per-instance 的；`projections.front()` 僅為無 focus 時的 fallback，不是正常路徑。

因此編輯器的範圍與繪製必須遵守以下規則：

- **顯示範圍 = 整份 document**。不可嘗試用 ARA region 指派收窄到單一 instance（已驗證無此訊號，收窄會導致 editor 全空）。
- **正在處理的 track** = `contentKey == editedContentKey_`（來自宿主 focus），完整渲染。
- **其餘 track = 唯讀參考軌**，只畫灰色 F0 曲線（`drawReferenceF0Curve`），不畫音符方塊、波形、unvoiced band、time grid anchor。顏色固定為 `UIColors::referenceCurve`，不接受呼叫端指定，以保證與紅/藍主動曲線明確區隔。
- **同 document 與 registry 兩種參考來源必須去重**。同一 modification 不得同時出現在兩個來源，否則會重複繪製（典型觸發情境：複製一軌改成和聲，該軌同時符合「同 document 其他 track」與「共享音訊素材的 registry 群組」）。去重 key 為 modification persistent ID，優先保留同 document 路徑。
- **同一份 ARA document 內的多條 clip 屬於同一 track**，Melodyne 單 clip 語意：只有 focused clip 完整渲染，其餘 clip 降級為參考軌。

分流決策集中在 `ContentRenderRouting`（純函式 + 單元測試），不得在繪製迴圈內重新推導。該邏輯曾因未受保護而退化成「全部完整渲染」，造成多 instance 視覺混淆且無任何測試可察覺。

`ReferenceTrackRegistry` 保留給**真正獨立的 document**（非本 DC 所能見者）；在 REAPER 的共用 document 模型下，同 document 內的參考軌不會走 registry，因此該單例在常見情境下無實際作用，但不可移除——其他 host 仍可能每 instance 獨立 document。

### 2.5 时间和渲染数据

项目使用多个明确的时间域：

- timeline seconds：宿主或 Standalone 时间轴
- content/source seconds：Note、PitchCurve 和 SourceWindow 使用的内容时间
- output seconds：TimeGrid 变换后的播放输出时间
- sample index：明确 sample rate 下的整数样本
- F0 frame：由 hop size 和 F0 sample rate 定义的帧索引
- pixel：UI 显示坐标

绝对时间是上游事实来源。不同时间域比较前必须经过显式映射；sample/frame/hop 只作为离散访问、上下文覆盖或模型输入坐标，不得反向修改绝对时间、内容边界或持久化数据。UI pixel 不得反推新的绝对时间；音频线程只消费已准备好的整数 sample range。

`TimeCoordinate` 中，`secondsToSamples` 是 point 的向零截断，`secondsToSamplesNearest` 用于模型/窗口中心的最近 sample，`secondsToSamplesFloor` 与 `secondsToSamplesCeil` 成对表示 interval 覆盖，`sampleRateProject` 用于跨采样率的离散边界投影。投影结果不回写秒域真相。

PianoRoll 的 source、output、timeline、pixel 映射集中在 `PianoRollTimeMap`。播放头命中音符时，先按 placement 和 TimeGrid 将 timeline 映射到 source，再与 Note 时间比较。

### 2.6 持久化和测试

Standalone、Capture、ARA 保留各自的 archive/container 格式，但内容字段都恢复到对应 owner 的 `ContentState`。`contentRevision` 和 `audioRevision` 不从归档恢复，新内容身份从 1 开始；ARA 另外保存并恢复 `notesRevision`、`pitchRevision`、`timeGridRevision` 及分析相关 revision，Standalone 也恢复 `referenceFeatures.analysisRevision`。不额外生成运行时 revision 账本。跨采样率 buffer 长度由源 sample 数和源/目标采样率一次投影得到；不得从投影后的 buffer 长度反算源内容秒数。

當前啟用測試預設關閉；打開 `OPENTUNE_BUILD_TESTS` 後有 11 個測試目標：`AutoTunePitchShifterTests`、`PitchParameterContractTests`、`PitchLaneVisualPolicyTests`、`Vst3ProcessorStateCodecTests`、`StandaloneProcessorStateCodecTests`、`CapturePersistenceTests`、`ContentSnapshotProjectionTests`、`ReferenceTrackRegistryTests`、`ContentRenderRoutingTests`、`ContentRenderServiceTests` 和 `VocoderExecutionCoordinatorTests`。`ContentRenderServiceTests` 覆蓋無 usable F0 時只發布 playback source、playback-only republish 不啟動 Stage1、TimeGrid derived-output invalidation、local invalidation 的實體 queue 邊界、canonical settlement、只有非 ARA content 會啟動 Stage2、stale generation/blank completion、TimeStretch clear ABA，以及 service shutdown wakeup；`VocoderExecutionCoordinatorTests` 使用 fake domain 覆蓋 generation-safe submit 與 model creation failure 後同 weight retry；`ReferenceTrackRegistryTests` 與 `ContentRenderRoutingTests` 維持既有 reference routing 契約。最近一次 CMake/Ninja 建置中，VST3/Standalone target 和 11/11 CTest 均通過。

## 3. 已完成的边界收敛

当前代码已具备以下边界：

1. `Stage2TimeStretchRebuilder` 不再構造缺少 snapshot 的臨時 `PlaybackReadSource`；canonical Stage1 讀取只接收 job 固定的 source PCM 和 `RenderCache`。
2. Stage2 job 帶入 `RenderCache` generation；invalidate／clear／remove 會丟棄待處理 Stage2 queue，worker 執行前與 rebuild 完成前都會重新驗證 generation、canonical settled、published revisions 和 source identity。`TimeStretchCache` 仍以 `(contentRevision, timeGridRevision)` 為版本 key，並以 invalidation generation 拒絕舊 build。
3. 局部 Stage1 编辑会为未受影响的 pending/running chunk 继承新的 `contentRevision`，避免 prepared overlay 跳过该 chunk 后回退到 dry。
4. Standalone、Capture 的非恒等 TimeGrid 经过 canonical-settled gate 进入 Stage2；ARA 只接受 identity TimeGrid，对非恒等 TimeGrid 显式拒绝，且不进入 Stage2。
5. `PluginProcessor` 的 ARA/Capture 分支在最终输出后发布频谱；Standalone placement 的 PianoRoll projection、ARA 非零 source window projection 和 TrimLeft output/source 映射已按各自坐标合同修正。
6. 删除了未消费的 ARA content accessor、cycle-range wrapper、播放采样率 getter 和 renderer CRS setter。worker 不再携带冗余的 `startSeconds`，只使用 sample range。
7. 删除未消费的 `mappingRevision`、`pitchShiftRevision`、`outputGainRevision` 和双账本 analysis lifecycle；VST3 旧状态的字节布局 skip 常量仍保留，仅用于读取已发布的历史布局。
8. `PianoRollComponent` 和 `PianoRollToolHandler` 的 scratch curve mutation 直接返回新的 `shared_ptr<const PitchCurveSnapshot>`；业务层不再通过旧 `getSnapshot()` 或 `clone()` 回读编辑结果。
9. Standalone、Capture 的 Stage1 completion 直接携带当次 snapshot/audio 进入 Stage2 settle gate；ARA 不建立 Stage2 completion chain。
10. `PlaybackRegionProjection` 只保留宿主 placement 属性和 `ContentKey`；ARA renderer 从发布的 `PlaybackReadSource.contentSnapshot` 取得 `SourceWindow` 和 TimeGrid。
11. `ContentSnapshotProjection` 对空 `timeGrid` 使用 bootstrap identity，snapshot 与 `ContentState` 的投影不再要求调用方预先填充 identity grid。
12. 采样率投影、TimeGrid spacing、F0 区间投影和 placement fade 已分别收敛到命名明确的离散坐标或绝对 seconds 路径；这些派生坐标不回写内容时间。
13. split 收敛为单一 F0 帧边界锚：父 source window 必须已 sample-aligned，切点取 `F0Timeline::nearestFrameBoundary` 的帧时间并校验其落在音频 sample 网格上，两段按半开帧区间 `[0, b)` / `[b, N)` 切分；非 identity TimeGrid 内容显式拒绝 split。`volumeEnvelope` 与 notes/silent gaps 使用同一 local 秒区间切片；merge 与 `copyContentRange` 复用 `AutomationLane` 的切片/拼接，不再静默丢弃音量包络。
14. 钢琴卷帘的「正在處理 track / 唯讀參考轨」分流收敛到 `ContentRenderRouting` 纯函数，并由 `ContentRenderRoutingTests` 锁定；绘製迴圈不再自行判断 `item.active`。分流曾退化为「全部完整渲染」而无声通过，正是因为它原先内嵌在绘製路径、没有测试 seam。
15. 參考軌呈现收敛为统一灰色：`UIColors::referenceCurve` 由 renderer 层强制指定，`ReferenceTrackCurve` 不再暴露 colour/opacity 字段，呼叫端无法重新引入 per-source 配色，也确保与主動曲线的红/蓝明确区隔。
16. 同 document 與 registry 兩种參考來源以 modification persistent ID 去重，同 document 路径优先；「複製一軌改成和聲」不再被重複繪製成錯位重影。
17. 參考軌選單成为參考轨显隐的唯一权威：同 document 来源经 `setHiddenReferenceContentKeys` 真正停止绘製（而非降低透明度），registry 来源经曲线过滤；正在编辑的 track 不受選單隱藏影响。
18. OpenTune 的 AUTO 與「吸附到音階」開關共用同一策略：開啟時套用目前音階；關閉時不套用音階投影，沿用音符生成器既有的最近半音量化，讓經過音與和弦外音可被保留並人工繪製。
19. Standalone、Capture、ARA 的 owner 都先完成 mutation，再以不可變 snapshot 通知 `ContentRenderService`；`PluginProcessor` 與 `OpenTuneDocumentController` 不再自行建立 `RenderJob`。
20. `ContentRenderService` 內部處理 playback publication、usable-F0 gate、Stage1 reconcile、canonical settlement、Stage2 domain policy 與 derived cache lifecycle；Stage1 executor 是唯一外部 execution seam。
21. `RenderWorker` 的 Stage1 queue 以 `(RenderCache*, chunk start)` index 查找、排序與去重，local revision 更新不再對整個 deque 做巢狀線性掃描。
22. `ContentRenderService::Metrics` 提供 invalidation、affected/queued chunks、Stage2、settlement、retry 與 async completion 基線；新增 `ContentRenderServiceTests` 鎖定 interface 行為。
23. `ProcessRenderRuntime` 只有在 `RenderCache::markChunkAsBlank()` 實際接受 revision/generation 後才回報 settled；completion gate 缺失時安全忽略 callback。相同 vocoder weight 在前次 model creation 失敗後仍允許重試。
24. `ContentRenderService::requeueRenderChunk()` 以 published source 的 `RenderCache` identity 和 cache state machine 判定 job；local disjoint edit 不會因 snapshot revision 差異而把 chunk 永久留在 `Running`。

## 4. 剩余问题

### P1：宿主行为和回归覆盖

1. **ARA placement、clone 和多 placement PianoRoll 仍缺 DAW/ARA 宿主回归。** projection 不再携带 content state；仍需宿主确认 placement 更新、clone 独立性、通知时序和视觉行为。
2. **Capture 非恒等 TimeGrid 需要真实宿主回归。** 代码路径已接入单一 Stage2 settle gate，但录制、编辑、播放和宿主 block 边界的时序仍没有 VST3 宿主测试。
3. **多 ARA instance 场景只完成 REAPER 实测，其他 host 未知。** §2.4 记录的四条事实（共用 document、renderer per-instance 指派、editor processor 无 renderer、焦點靠 host selection）均来自单一 host 实测。若其他 host 为每 instance 独立 document，`ReferenceTrackRegistry` 才是主要路径，但其群組条件（共享 audio source persistent ID）未在其他 host 验证过。当前无任何自动化测试能覆盖 ARA 多 instance 行为——纯逻辑测试只锁住分流决策本身，真实绑定仍需人工宿主回归。

### P1：时间域和状态合同

1. **离散投影与绝对时间的边界合同仍需宿主和数据回归。** placement fade、RenderCache 跨率长度、TimeGrid spacing、F0 区间投影已分别使用明确的 seconds/point/interval 语义；剩余问题集中在 ARA source window 的 fractional sample 对齐和模型输入边界，不能凭经验改绝对时间公式。
2. **F0 frame 与模型窗中心的合同未锁定。** FCPE padding、F0Timeline 和 mel/F0 插值需要训练侧定义和回归样本确认。
3. **舊 cache 拒絕、非恒等 TimeGrid 的 cache miss、F0Timeline/SourceWindow 採樣網格與 split 邊界數學、AutomationLane slice/merge、跨 instance 群組偵測、鋼琴卷帘 active/reference 分流已有純邏輯測試。** ARA 非零 source window、局部渲染 revision、導出一致性、placement fractional sample 邊界以及 `splitPlacementAtSeconds` 端到端集成目前只有代碼合同；相關 ARA/Capture 行為仍缺宿主級驗證。當前啟用測試目標為 11 個，CTest 全部通過。

### P2：维护成本

1. `PluginProcessor` 已退出 `RenderJob` construction 與 Stage1/Stage2 gate，但仍承載較多 owner mutation、transport、import/export 與 inference 接線；後續只能沿已確認的 owner/invariant 收斂，不能按檔案大小抽 helper。
2. F0/Reference 使用 detached worker，RenderWorker 使用可 join 的常驻线程，Vocoder control worker 依附进程寿命单例；各自拥有去重、shutdown 和 completion 机制。业务不能强行合并，但关闭、generation 和 completion gate 应建立共同测试合同。
3. 三种持久化容器仍各自解释 EQ/Note migration；格式边界应保留，纯数据迁移规则可以共享。
4. Beat 计算、MIDI/Hz 与 lane-center 转换、cents math 等仍有重复或同名 API，后续应在纯数学边界单点化，不能把 UI 坐标规则混入音高真相。
5. 測試預設仍為關閉選項；本次驗證顯式打開 `OPENTUNE_BUILD_TESTS` 後，VST3/Standalone target 和 11/11 CTest 均通過。

### 不属于当前架构问题的事项

- `OPENTUNE_COMMON_SOURCES` 在两个 SharedCode target 各编译一次是两个产品生成独立二进制的正常结果。
- ARA wrapper、document-scoped `AudioModification.content` 和 CRS snapshot 是不同生命周期的合法边界，不应因“存在多个对象”而重新合并。
- 不应恢复运行时 `wrapperType` 双格式分流，也不应新增 ContentDomainRouter、锁、COW 或 CloneManager 来掩盖尚未解决的 owner/时间域问题。
