cmake_minimum_required(VERSION 3.22)
project(OpenTuneOrtBenchmark LANGUAGES C CXX)

get_filename_component(ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(JUCE_ROOT "${ROOT}/JUCE-master" CACHE PATH "Directory containing pinned JUCE")
add_subdirectory("${JUCE_ROOT}" juce)
add_executable(ort_benchmark
    Benchmark.cpp
    "${ROOT}/Source/DSP/ResamplingManager.cpp"
    "${ROOT}/Source/Inference/FCPEExtractor.cpp"
    "${ROOT}/Source/Inference/OnnxVocoderBase.cpp"
    "${ROOT}/Source/Inference/PCNSFHifiGANVocoder.cpp"
    "${ROOT}/Source/Utils/CpuBudgetManager.cpp")
target_compile_features(ort_benchmark PRIVATE cxx_std_17)
target_compile_definitions(ort_benchmark PRIVATE JUCE_USE_CURL=0 JUCE_USE_FLAC=0 JUCE_USE_OGGVORBIS=0)
target_include_directories(ort_benchmark PRIVATE
    "${ORT_INCLUDE}" "${ROOT}/Source" "${ROOT}/ThirdParty/r8brain-free-src-master")
target_link_libraries(ort_benchmark PRIVATE "${ORT_LIBRARY}" juce::juce_dsp)
get_filename_component(ORT_LIB_DIRECTORY "${ORT_LIBRARY}" DIRECTORY)
set_target_properties(ort_benchmark PROPERTIES BUILD_RPATH "${ORT_LIB_DIRECTORY}")
