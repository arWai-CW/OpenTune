#pragma once

/**
 * ModelPathResolver - 模型路径解析器
 * 
 * 负责解析 ONNX Runtime 动态库路径和模型目录路径。
 * 解析顺序：用户自选目录（AppPreferences）> 程序目录 > Program Files >
 * AppData > 注册表 > 当前工作目录。
 *
 * 用户自选目录以进程内缓存保存（由 AppPreferences::setCustomModelsDirectory
 * 在持久化时同步更新，加载时从磁盘播种），getModelsDirectory() 因此可在
 * 音频/推理工作线程频繁调用而不触碰磁盘。
 *
 * 实现位于 ModelPathResolver.cpp。
 */

#include <string>
#include <juce_core/juce_core.h>

namespace OpenTune {

class ModelPathResolver {
public:
    static bool ensureOnnxRuntimeLoaded();

    /// 更新进程内用户自选模型目录缓存（AppPreferences 持久化时调用）。
    static void setUserConfiguredModelsDirectory(const juce::String& directory);

    static std::string getModelsDirectory();

private:
    static juce::File getCurrentModuleFile();
};

} // namespace OpenTune