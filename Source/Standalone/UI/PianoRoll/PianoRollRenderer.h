#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include "UI/UIColors.h"
#include "UI/WaveformMipmap.h"
#include "Utils/F0Timeline.h"
#include "Utils/ContentTimelineProjection.h"
#include "Content/ContentKey.h"
#include "Utils/PitchCurve.h"
#include "Utils/PianoRollVisualPreferences.h"
#include "Utils/Note.h"
#include "Utils/TimeGrid.h"   // ⚡️ vocal-time-stretch §8.5 — TimeGrid handles
#include "UI/ToolIds.h"
#include "UI/ViewMapper.h"
#include "Content/EditableContentSnapshot.h"
#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace OpenTune {

struct TimelineContentPlacement
{
    ContentKey contentKey;
    ContentTimelineProjection projection;
    juce::Colour displayColour{0xFF4A90D9}; // 轨道主题色/音符主色
    /// 所屬 Region Sequence 名（≈ host track 名），僅供 editor 的參考軌選單顯示。
    /// 不參與任何繪製或命中測試；Standalone 恆為空。
    juce::String sequenceName;

    bool isValid() const noexcept
    {
        return contentKey.isValid() && projection.isValid();
    }
};

/**
 * 钢琴卷帘渲染器
 * 负责绘制钢琴卷帘界面的所有元素，包括背景、琴键、音符、波形和音高曲线
 */
class PianoRollRenderer
{
public:
    PianoRollRenderer() = default;

    // OpenDyne blob 音量预览：拖拽期间由组件注入（指向 InteractionState::volumePreviewEnvelope）。
    // 非拖拽时为 nullptr，drawNotes 回退到 ownerSnapshot->volumeEnvelope。
    const AutomationLane* volumePreviewEnvelope_ = nullptr;
    void setVolumePreviewEnvelope(const AutomationLane* env) noexcept { volumePreviewEnvelope_ = env; }

    /**
     * 渲染上下文结构体
     * 包含渲染所需的所有参数
     */
    struct ContentRenderItem
    {
        ContentKey contentKey;
        ContentTimelineProjection projection;
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
        std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
        std::shared_ptr<const PitchCurveSnapshot> pitchSnapshot;
        F0Timeline f0Timeline;
        std::shared_ptr<const EditableContentSnapshot> ownerSnapshot;
        const std::vector<Note>* displayNotes = nullptr;
        bool active = false;
        juce::Colour displayColour{0xFF4A90D9}; // 从 placement 注入，Renderer 不查 owner
        bool notesPrimaryScheme = false;        // OpenDyne：波形 blob 模式

        bool isValid() const noexcept
        {
            return contentKey.isValid() && projection.isValid() && static_cast<bool>(timeGrid);
        }
    };

    struct ReferenceOverlay
    {
        std::vector<Note> ghostNotes;               // reference content 的 derived notes（content-local source time）
        float ghostOpacity{0.20f};                   // 透明度
        juce::Colour ghostColour;                    // ghost 颜色（不同于当前轨）
        bool enabled{false};                         // 是否启用 overlay

        // source content → timeline 的投影值
        ContentTimelineProjection sourceProjection;
        // reference content 的 TimeGrid（source↔output 转换）
        std::shared_ptr<const TimeGridSnapshot> timeGrid;
    };

    /// 跨軌參考曲線疊加（Melodyne-style reference）：由 VST3 editor 從
    /// ReferenceTrackRegistry 的發布記錄建構。所有時間皆為 playback-time 秒數
    /// （已透過 RegionMapping 由 modification-time 轉換），可直接投入時間軸繪製。
    ///
    /// 顏色刻意不由呼叫端指定：renderer 一律使用 UIColors::referenceCurve 統一灰，
    /// 確保參考軌與主動 track（紅/藍）視覺上明確區隔。
    struct ReferenceTrackCurve
    {
        juce::String name;                   // 顯示標籤（Region Sequence / source name）
        // 平行陣列（按 F0 frame 逐幀）：
        std::vector<float> playbackTimes;    // 秒；< 0 表示對不到任何 region（無效幀）
        std::vector<float> originalHz;       // Original F0（Hz）；<= 0 為無聲/無效幀
        std::vector<float> correctedHz;      // 校正 F0（Hz）；<= 0 表示該幀無校正覆蓋
        bool hasCorrections{false};

        bool isValid() const noexcept
        {
            return !name.isEmpty()
                && playbackTimes.size() == originalHz.size()
                && originalHz.size() == correctedHz.size();
        }
    };

    /// Render context — includes stable content state and transient UI fields.
    struct RenderContext
    {
        int width = 0;
        int height = 0;
        int pianoKeyWidth = 60;
        int contentStartX = 60;
        juce::Rectangle<int> rasterBounds;  // Image-local rect to rasterize; full image bounds when empty
        int rulerHeight = 30;
        double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
        float pixelsPerSemitone = 15.0f;
        float minMidi = 24.0f;
        float maxMidi = 108.0f;
        std::vector<ContentRenderItem> contents;
        int scaleRootNote = 0;
        int scaleType = 1;
        NoteNameMode noteNameMode = NoteNameMode::COnly;
        bool labelsOnly = false;
        bool showUnvoicedFrames = false;
        bool showOriginalF0 = true;
        bool showCorrectedF0 = true;

        bool hasF0Selection = false;
        std::vector<std::pair<int, int>> f0SelectionRanges;  // {startFrame, endFrameExclusive}，排序归并

        ViewMapper coords;

        ToolId currentTool = ToolId::Select;
        bool isTimeView() const { return currentTool == ToolId::TimeTool; }
        uint64_t timeGridHoveredHandleId = 0;
        uint64_t timeGridSelectedHandleId = 0;
        std::vector<uint64_t> additionalSelectedHandleIds;
    };

    void drawUnvoicedFrameBands(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawWaveform(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item,
                      const WaveformMipmap::Level& wfLevel, int wfLevelIndex);

    /// Draw published TimeGrid anchors as cache-friendly neutral lines.
    /// No hover/selected/drag affordances — those are painted by drawTimeGridHandles in overlay.
    void drawTimeGridAnchors(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    void drawPianoKeys(juce::Graphics& g, const RenderContext& ctx);
    void drawNotes(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawSelectedNoteHighlights(juce::Graphics& g,
                                    const RenderContext& ctx,
                                    const std::vector<Note>& notes,
                                    const std::vector<int>& selectedNoteIndices,
                                    const ContentRenderItem& item);
    void drawF0Curve(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);
    void drawF0SelectionHighlight(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    // ⚡️ §8.5 — paint TimeGrid handles as vertical guide lines.
    void drawTimeGridHandles(juce::Graphics& g, const RenderContext& ctx, const ContentRenderItem& item);

    void drawGhostNotes(juce::Graphics& g, const RenderContext& ctx, const ReferenceOverlay& overlay);

    /** 跨軌參考曲線疊加：半透明薄線畫在主動內容曲線之下。 */
    void drawReferenceTrackCurves(juce::Graphics& g,
                                  const RenderContext& ctx,
                                  const std::vector<ReferenceTrackCurve>& curves);

    /**
     * 參考軌（同一 ARA document 內的非焦點 track）只畫 F0 曲線，且統一灰色。
     *
     * 與 drawF0Curve 的差異（刻意為之）：
     *  - 不畫音符方塊、波形、unvoiced band、time grid anchor（由 drawContent 跳過）
     *  - 不使用 originalF0 紅 / correctedF0 藍，改用 UIColors::referenceCurve 統一灰
     *  - 無 glow / innerGlow，僅單純線條，避免與主動曲線搶視覺
     *  - Original 以粗半透明「帶」呈現、Corrected 以細實線疊在其上，
     *    如此即使同色仍可看出參考轨是否已被校正（線偏離帶）
     */
    void drawReferenceF0Curve(juce::Graphics& g,
                              const RenderContext& ctx,
                              const ContentRenderItem& item);


};

} // namespace OpenTune
