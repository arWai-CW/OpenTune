#include "DSP/ResamplingManager.h"
#include "Inference/FCPEExtractor.h"
#include "Inference/PCNSFHifiGANVocoder.h"
#include "Utils/AppLogger.h"
#include "Utils/CpuBudgetManager.h"

#if defined(__APPLE__) && ORT_API_VERSION < 24
#include "Inference/OnnxRuntimeProviderCompat.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>

namespace OpenTune {
void AppLogger::debug(const juce::String&) {}
void AppLogger::info(const juce::String&) {}
void AppLogger::warn(const juce::String&) {}
void AppLogger::error(const juce::String&) {}
}

namespace {
using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Ort::SessionOptions options(bool analysis, bool coreml)
{
    Ort::SessionOptions result;
    result.SetExecutionMode(ORT_SEQUENTIAL);
    result.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    if (analysis)
    {
        const auto budget = OpenTune::CpuBudgetManager::buildConfig(coreml);
        result.SetIntraOpNumThreads(budget.onnxIntra);
        result.SetInterOpNumThreads(budget.onnxInter);
        result.AddConfigEntry("session.intra_op.allow_spinning", "0");
        result.AddConfigEntry("session.inter_op.allow_spinning", "0");
    }
    if (coreml)
    {
#if defined(__APPLE__) && ORT_API_VERSION < 24
        OpenTune::appendCoreMlExecutionProvider(result);
#elif defined(__APPLE__)
        result.AppendExecutionProvider("CoreML", {{"ModelFormat", "MLProgram"}, {"MLComputeUnits", "ALL"}});
#else
        throw std::runtime_error("CoreML requires macOS");
#endif
    }
    return result;
}

template <typename Run>
void measure(const char* label, double seconds, Run run)
{
    constexpr auto measured = 9;
    std::vector<double> samples;
    size_t count = 0;
    double absMean = 0.0;
    for (int i = -2; i < measured; ++i)
    {
        const auto start = Clock::now();
        auto result = run();
        const double time = elapsedMs(start);
        if (result.empty() || !std::all_of(result.begin(), result.end(), [](float x) { return std::isfinite(x); }))
            throw std::runtime_error(std::string(label) + " returned invalid output");
        count = result.size();
        absMean = std::accumulate(result.begin(), result.end(), 0.0,
                                 [](double total, float x) { return total + std::abs(x); }) / count;
        if (i >= 0)
            samples.push_back(time);
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "- " << label << ' ' << seconds << " s: median " << samples[measured/2]
              << " ms, range " << samples.front() << "–" << samples.back()
              << " ms, output " << count << ", mean |output| " << absMean << '\n';
}
}

int main(int argc, char** argv) try
{
    if (argc != 5)
        throw std::runtime_error("usage: ort_benchmark VERSION cpu|coreml FCPE_MODEL VOCODER_MODEL");
    const std::string version = OrtGetApiBase()->GetVersionString();
    if (version != argv[1])
        throw std::runtime_error("Unexpected loaded ORT version: " + version);
    const bool coreml = std::string(argv[2]) == "coreml";
    std::cout << "\n### ONNX Runtime " << version << ' ' << argv[2] << '\n' << std::flush
              << std::fixed << std::setprecision(3);
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "OpenTuneOrtBenchmark");
    Ort::RunOptions runOptions;

    auto start = Clock::now();
    OpenTune::FCPEExtractor extractor(std::make_unique<Ort::Session>(env, argv[3], options(true, coreml)),
                                      std::make_shared<OpenTune::ResamplingManager>());
    std::cout << "- FCPE session and extractor init: " << elapsedMs(start) << " ms\n";
    for (const int seconds : {5, 20})
    {
        constexpr int sampleRate = 48000;
        std::vector<float> wave(sampleRate * seconds);
        double phase = 0.0;
        for (size_t i = 0; i < wave.size(); ++i)
        {
            const double t = static_cast<double>(i) / sampleRate;
            phase += (160.0 + 45.0 * std::sin(1.19 * t) + 4.0 * std::sin(29.0 * t)) * 6.283185307179586 / sampleRate;
            const double amplitude = std::pow(std::abs(std::sin(4.0 * t)), 0.5);
            wave[i] = static_cast<float>(amplitude * (0.23 * std::sin(phase) + 0.10 * std::sin(2 * phase)
                                                     + 0.06 * std::sin(3 * phase)));
        }
        measure("FCPE analysis", seconds, [&] {
            return extractor.extractF0(wave.data(), wave.size(), sampleRate, runOptions);
        });
    }

    start = Clock::now();
    OpenTune::PCNSFHifiGANVocoder vocoder(std::make_unique<Ort::Session>(env, argv[4], options(false, coreml)));
    std::cout << "- HiFiGAN session and vocoder init: " << elapsedMs(start) << " ms\n";
    for (const double seconds : {0.5, 3.0})
    {
        const size_t frames = static_cast<size_t>(std::ceil(seconds * vocoder.getSampleRate() / vocoder.getHopSize()));
        const size_t bins = static_cast<size_t>(vocoder.getConditioningBins());
        std::vector<float> pitch(frames), conditioning(frames * bins);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            pitch[frame] = static_cast<float>(190.0 + 20.0 * std::sin(frame * 0.06));
            for (size_t bin = 0; bin < bins; ++bin)
                conditioning[bin * frames + frame] = static_cast<float>(
                    -2.0 - 6.5 * static_cast<double>(bin)/bins + 0.6 * std::sin(frame * 0.15 + bin * 0.6));
        }
        measure("HiFiGAN render synthesis", seconds, [&] {
            return vocoder.synthesize(pitch, {}, conditioning.data(), conditioning.size(), runOptions);
        });
    }
}
catch (const std::exception& ex)
{
    std::cerr << ex.what() << '\n';
    return 1;
}
