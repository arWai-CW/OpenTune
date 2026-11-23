#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../DSP/AutoTunePeriodDetector.h"
#include "../DSP/PitchShiftRateContract.h"

namespace OpenTune {

/// 判断 chunk 是否需要声码器（任一有声帧偏差 > 阈值）。
/// @param correctedF0    修正后 F0 数组（chunk 局部，100fps）
/// @param numF0Frames    correctedF0 帧数
/// @param originalF0     原始 F0 全局数组
/// @param f0StartFrame   chunk 在全局 F0 中的起始帧索引
/// @param thresholdCents 偏差阈值（默认 75 cents）
/// @return true = 需要声码器；false = 可用 AutoTune 周期重采样
inline bool chunkNeedsVocoder(
    const float* correctedF0,
    int numF0Frames,
    const std::vector<float>& originalF0,
    int f0StartFrame,
    float thresholdCents = kPitchShiftThresholdCents)
{
    for (int i = 0; i < numF0Frames; ++i) {
        const int globalFrame = f0StartFrame + i;
        if (globalFrame < 0 || globalFrame >= static_cast<int>(originalF0.size()))
            continue;

        const float corrected = correctedF0[i];
        const float original = originalF0[static_cast<size_t>(globalFrame)];

        // 跳过无声帧
        if (corrected <= 0.0f || original <= 0.0f) continue;

        const double resampleRate =
            static_cast<double>(corrected) / static_cast<double>(original);
        if (!isPitchShiftRateWithinCents(resampleRate, thresholdCents))
            return true;
    }
    return false;
}

/// Corrections closer than this to the source F0 are treated as no correction,
/// so a note moved back to where it was renders as the untouched source.
constexpr float kIdentityToleranceCents = 1.0f;
/// Out-of-contract frames are rendered by the vocoder; the light render keeps
/// following the target as closely as its contract allows so both renders
/// share a pitch trajectory at the seams.
constexpr float kLightClampCents = 70.0f;
/// Detector-invalid runs shorter than this pass through the light render
/// uncorrected (onsets, breath, consonant edges); longer runs go to the vocoder.
constexpr double kMaxLightInvalidRunSeconds = 0.040;
constexpr double kSpliceFadeSeconds = 0.020;
constexpr double kSpliceSearchSeconds = 0.015;
constexpr double kSpliceDilateSeconds = 0.005;
constexpr double kSpliceMergeGapSeconds = 0.050;

inline int chunkFrameAtSample(int sampleIndex, double samplesPerF0Frame,
                              double firstSampleFramePhase, int numF0Frames)
{
    return std::clamp(
        static_cast<int>(std::floor(firstSampleFramePhase
            + static_cast<double>(sampleIndex) / samplesPerF0Frame)),
        0, numF0Frames - 1);
}

/// Zeroes chunk-local correction frames that sit within
/// kIdentityToleranceCents of the source F0.
inline void clearIdentityCorrections(std::vector<float>& correctedF0,
                                     const std::vector<float>& originalF0,
                                     int f0StartFrame)
{
    const double tolerance = static_cast<double>(kIdentityToleranceCents);
    for (size_t i = 0; i < correctedF0.size(); ++i) {
        const int globalFrame = f0StartFrame + static_cast<int>(i);
        if (globalFrame < 0 || globalFrame >= static_cast<int>(originalF0.size()))
            continue;
        const float corrected = correctedF0[i];
        const float original = originalF0[static_cast<size_t>(globalFrame)];
        if (corrected <= 0.0f || original <= 0.0f)
            continue;
        const double cents = 1200.0 * std::log2(
            static_cast<double>(corrected) / static_cast<double>(original));
        if (std::fabs(cents) < tolerance)
            correctedF0[i] = 0.0f;
    }
}

struct LightRenderPlan
{
    /// Chunk-local target for the light render.
    std::vector<float> dspTarget;
    /// Chunk-local frames the vocoder must render.
    std::vector<uint8_t> vocoderFrames;
    bool needsVocoder = false;
};

/// Splits a chunk between the light render and the vocoder per frame instead
/// of sending the whole chunk to the vocoder when one frame needs it.
inline LightRenderPlan planLightRender(
    const std::vector<float>& correctedF0,
    const float* originalF0,
    int numF0Frames,
    const AutoTunePeriodDetector::DetectedPeriod* detectorPeriods,
    int numSamples,
    double sampleRate,
    double f0FrameRate,
    double firstSampleFramePhase)
{
    LightRenderPlan plan;
    const int frames = std::min(numF0Frames, static_cast<int>(correctedF0.size()));
    plan.dspTarget.assign(correctedF0.begin(), correctedF0.end());
    plan.vocoderFrames.assign(correctedF0.size(), 0);
    if (frames <= 0)
        return plan;

    const auto active = [&](int f) {
        return correctedF0[static_cast<size_t>(f)] > 0.0f && originalF0[f] > 0.0f;
    };
    const double clampRatio = std::exp2(static_cast<double>(kLightClampCents) / 1200.0);
    for (int f = 0; f < frames; ++f) {
        if (!active(f))
            continue;
        const double original = static_cast<double>(originalF0[f]);
        const double rate = static_cast<double>(correctedF0[static_cast<size_t>(f)]) / original;
        if (isPitchShiftRateWithinCents(rate, kPitchShiftThresholdCents))
            continue;
        plan.vocoderFrames[static_cast<size_t>(f)] = 1;
        plan.dspTarget[static_cast<size_t>(f)] = static_cast<float>(
            rate > 1.0 ? original * clampRatio : original / clampRatio);
    }

    if (detectorPeriods != nullptr && numSamples > 0) {
        const double samplesPerF0Frame = sampleRate / f0FrameRate;
        std::vector<uint8_t> invalid(static_cast<size_t>(frames), 0);
        for (int i = 0; i < numSamples; ++i) {
            const int f = chunkFrameAtSample(i, samplesPerF0Frame,
                                             firstSampleFramePhase, frames);
            const auto& det = detectorPeriods[i];
            if (active(f) && !(det.valid && det.periodSamples > 0.0f))
                invalid[static_cast<size_t>(f)] = 1;
        }
        const int maxLightRun = std::max(1, static_cast<int>(
            std::floor(kMaxLightInvalidRunSeconds * f0FrameRate)));
        for (int f = 0; f < frames;) {
            if (!invalid[static_cast<size_t>(f)]) { ++f; continue; }
            int end = f;
            while (end < frames && invalid[static_cast<size_t>(end)])
                ++end;
            for (int k = f; k < end; ++k) {
                plan.dspTarget[static_cast<size_t>(k)] = 0.0f;
                if (end - f >= maxLightRun)
                    plan.vocoderFrames[static_cast<size_t>(k)] = 1;
            }
            f = end;
        }
    }

    plan.needsVocoder = std::any_of(plan.vocoderFrames.begin(), plan.vocoderFrames.end(),
                                    [](uint8_t v) { return v != 0; });
    return plan;
}

/// Per-sample vocoder weight (0 = light render, 1 = vocoder). Regions are
/// merged across short gaps and widened slightly; each seam is a raised-cosine
/// crossfade outside the region, placed at the quietest nearby point of the
/// source so it lands in a breath or consonant when one is close.
inline std::vector<float> buildVocoderSpliceWeights(
    const std::vector<uint8_t>& vocoderFrames,
    int numSamples,
    double sampleRate,
    double f0FrameRate,
    double firstSampleFramePhase,
    const float* sourceAudio)
{
    std::vector<float> weights(static_cast<size_t>(std::max(0, numSamples)), 0.0f);
    const int frames = static_cast<int>(vocoderFrames.size());
    if (numSamples <= 0 || frames <= 0)
        return weights;

    const double samplesPerF0Frame = sampleRate / f0FrameRate;
    struct Region { int start; int end; };
    std::vector<Region> regions;
    for (int i = 0; i < numSamples; ++i) {
        const bool masked = vocoderFrames[static_cast<size_t>(chunkFrameAtSample(
            i, samplesPerF0Frame, firstSampleFramePhase, frames))] != 0;
        if (!masked)
            continue;
        if (!regions.empty() && regions.back().end == i)
            regions.back().end = i + 1;
        else
            regions.push_back({i, i + 1});
    }
    if (regions.empty())
        return weights;

    const int mergeGap = static_cast<int>(kSpliceMergeGapSeconds * sampleRate);
    const int dilate = static_cast<int>(kSpliceDilateSeconds * sampleRate);
    const int fade = std::max(2, static_cast<int>(kSpliceFadeSeconds * sampleRate));
    const int search = static_cast<int>(kSpliceSearchSeconds * sampleRate);
    std::vector<Region> merged;
    for (const auto& r : regions) {
        if (!merged.empty() && r.start - merged.back().end < mergeGap)
            merged.back().end = r.end;
        else
            merged.push_back(r);
    }

    std::vector<double> energyPrefix(static_cast<size_t>(numSamples) + 1, 0.0);
    for (int i = 0; i < numSamples; ++i) {
        const double x = sourceAudio != nullptr ? static_cast<double>(sourceAudio[i]) : 0.0;
        energyPrefix[static_cast<size_t>(i) + 1] = energyPrefix[static_cast<size_t>(i)] + x * x;
    }
    const auto energy = [&](int a, int b) {
        a = std::clamp(a, 0, numSamples);
        b = std::clamp(b, 0, numSamples);
        return energyPrefix[static_cast<size_t>(b)] - energyPrefix[static_cast<size_t>(a)];
    };
    const auto ramp = [&](int k) {
        const double x = (static_cast<double>(k) + 0.5) / static_cast<double>(fade);
        return static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979323846 * x));
    };
    const auto raise = [&](int i, float w) {
        if (i >= 0 && i < numSamples)
            weights[static_cast<size_t>(i)] = std::max(weights[static_cast<size_t>(i)], w);
    };
    constexpr int kSearchStep = 32;

    for (const auto& r : merged) {
        const int start = std::max(0, r.start - dilate);
        const int end = std::min(numSamples, r.end + dilate);
        for (int i = start; i < end; ++i)
            raise(i, 1.0f);

        if (start > 0) {
            int bestEnd = start;
            double bestEnergy = energy(start - fade, start);
            for (int fe = start - kSearchStep; fe >= start - search && fe - fade >= 0; fe -= kSearchStep) {
                const double e = energy(fe - fade, fe);
                if (e < bestEnergy) { bestEnergy = e; bestEnd = fe; }
            }
            for (int i = bestEnd; i < start; ++i)
                raise(i, 1.0f);
            for (int k = 0; k < fade; ++k)
                raise(bestEnd - fade + k, ramp(k));
        }
        if (end < numSamples) {
            int bestStart = end;
            double bestEnergy = energy(end, end + fade);
            for (int fs = end + kSearchStep; fs <= end + search && fs + fade <= numSamples; fs += kSearchStep) {
                const double e = energy(fs, fs + fade);
                if (e < bestEnergy) { bestEnergy = e; bestStart = fs; }
            }
            for (int i = end; i < bestStart; ++i)
                raise(i, 1.0f);
            for (int k = 0; k < fade; ++k)
                raise(bestStart + k, ramp(fade - 1 - k));
        }
    }
    return weights;
}

/// Light-render audio kept for splicing with the vocoder render of the same chunk.
struct VocoderSplicePayload
{
    /// Channel planes, numChannels * weights.size() samples.
    std::vector<float> lightAudio;
    std::vector<float> weights;
    int numChannels = 1;
};

/// Mixes the mono vocoder render into every light-render channel plane.
inline std::vector<float> spliceVocoderIntoLight(const VocoderSplicePayload& payload,
                                                 const std::vector<float>& vocoderAudio)
{
    const size_t frames = payload.weights.size();
    if (vocoderAudio.size() != frames
        || payload.lightAudio.size() != frames * static_cast<size_t>(payload.numChannels))
        return {};
    std::vector<float> out(payload.lightAudio.size());
    for (int ch = 0; ch < payload.numChannels; ++ch) {
        const size_t base = static_cast<size_t>(ch) * frames;
        for (size_t i = 0; i < frames; ++i) {
            const float w = payload.weights[i];
            out[base + i] = w == 0.0f ? payload.lightAudio[base + i]
                : w * vocoderAudio[i] + (1.0f - w) * payload.lightAudio[base + i];
        }
    }
    return out;
}

} // namespace OpenTune