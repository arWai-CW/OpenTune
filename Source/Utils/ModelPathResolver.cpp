#include "ModelPathResolver.h"
#include "AppPreferences.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#include <mutex>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

namespace OpenTune {

namespace {

// 进程内用户自选模型目录缓存。setUserConfiguredModelsDirectory 在
// AppPreferences 持久化时同步更新；getModelsDirectory 首次调用时从磁盘
// 播种。读取路径只查缓存，不触碰磁盘。
std::mutex& userModelsDirectoryMutex()
{
    static std::mutex mutex;
    return mutex;
}

juce::String& userModelsDirectoryCache()
{
    static juce::String directory;
    return directory;
}

std::atomic<bool>& userModelsDirectorySeeded()
{
    static std::atomic<bool> seeded{false};
    return seeded;
}

} // namespace

void ModelPathResolver::setUserConfiguredModelsDirectory(const juce::String& directory)
{
    std::lock_guard<std::mutex> lock(userModelsDirectoryMutex());
    userModelsDirectorySeeded().store(true, std::memory_order_relaxed);
    userModelsDirectoryCache() = directory;
}

bool ModelPathResolver::ensureOnnxRuntimeLoaded()
{
#if defined(_WIN32)
    if (::GetModuleHandleW(L"OpenTuneOnnxRuntime_1_24_4.dll") != nullptr) {
        return true;
    }

    const juce::File moduleFile = getCurrentModuleFile();
    const juce::File programFilesRoot = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
        .getChildFile("OpenTune");
    const juce::File programDataRoot = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
        .getChildFile("OpenTune");

    // 唯一 DLL 名 OpenTuneOnnxRuntime_1_24_4.dll：带版本后缀，绝不装载宿主的同名裸 DLL
    const juce::File candidates[] = {
        programFilesRoot.getChildFile("OpenTuneOnnxRuntime_1_24_4.dll"),
        programDataRoot.getChildFile("OpenTuneOnnxRuntime_1_24_4.dll"),
        moduleFile.getParentDirectory().getChildFile("OpenTuneOnnxRuntime_1_24_4.dll")
    };

    for (const auto& candidate : candidates) {
        if (!candidate.existsAsFile()) {
            continue;
        }
        const auto handle = ::LoadLibraryExW(
            candidate.getFullPathName().toWideCharPointer(),
            nullptr,
            LOAD_WITH_ALTERED_SEARCH_PATH
        );
        if (handle != nullptr) {
            return true;
        }
    }
    return false;
#else
    return true;
#endif
}

std::string ModelPathResolver::getModelsDirectory()
{
    juce::String custom;
    {
        std::lock_guard<std::mutex> lock(userModelsDirectoryMutex());
        if (userModelsDirectorySeeded().load(std::memory_order_acquire)) {
            custom = userModelsDirectoryCache();
        }
    }

    if (!userModelsDirectorySeeded().load(std::memory_order_acquire)) {
        // 首个调用：锁外从磁盘读取用户自选目录一次。构造 AppPreferences 会
        // 经 load()/setUserConfiguredModelsDirectory 写回本缓存，因此在锁外
        // 读取避免与该写锁嵌套死锁。之后所有调用只读缓存。
        custom = AppPreferences().getState().shared.customModelsDirectory;
        std::lock_guard<std::mutex> lock(userModelsDirectoryMutex());
        userModelsDirectorySeeded().store(true, std::memory_order_release);
        userModelsDirectoryCache() = custom;
    }

    // 用户自选目录优先：无论是否存在都返回，让下游（模型可用性检查、
    // 错误信息）基于用户配置的真实路径报告，避免静默回退到自动侦测目录
    // 造成「改了路径却仍加载旧模型」的困惑。
    if (custom.isNotEmpty()) {
        return custom.toStdString();
    }

    const juce::File programFilesModelsDir = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
        .getChildFile("OpenTune")
        .getChildFile("models");
    if (programFilesModelsDir.isDirectory()) {
        return programFilesModelsDir.getFullPathName().toStdString();
    }

    const juce::File programDataModelsDir = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
        .getChildFile("OpenTune")
        .getChildFile("models");
    if (programDataModelsDir.isDirectory()) {
        return programDataModelsDir.getFullPathName().toStdString();
    }

    const juce::File moduleFile = getCurrentModuleFile();
    juce::File modelsDir = moduleFile.getParentDirectory().getChildFile("models");

    if (modelsDir.exists() && modelsDir.isDirectory()) {
        return modelsDir.getFullPathName().toStdString();
    }

    modelsDir = moduleFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("models");

    if (modelsDir.exists() && modelsDir.isDirectory()) {
        return modelsDir.getFullPathName().toStdString();
    }

    #ifdef _WIN32
    juce::String regPath = juce::WindowsRegistry::getValue(
        "HKEY_LOCAL_MACHINE\\Software\\MakediffVST\\OpenTune\\ModelsPath"
    );
    if (regPath.isNotEmpty()) {
        juce::File regModelsDir(regPath);
        if (regModelsDir.exists() && regModelsDir.isDirectory()) {
            return regPath.toStdString();
        }
    }
    #endif

    modelsDir = juce::File::getCurrentWorkingDirectory().getChildFile("models");
    if (modelsDir.exists() && modelsDir.isDirectory()) {
        return modelsDir.getFullPathName().toStdString();
    }

    modelsDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile("models");
    if (modelsDir.exists() && modelsDir.isDirectory()) {
        return modelsDir.getFullPathName().toStdString();
    }

    return "./models";
}

juce::File ModelPathResolver::getCurrentModuleFile()
{
#if defined(_WIN32)
    HMODULE moduleHandle = nullptr;
    const BOOL ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModelPathResolver::getModelsDirectory),
        &moduleHandle
    );

    if (ok != 0 && moduleHandle != nullptr) {
        std::wstring path;
        path.resize(32768);
        const DWORD len = GetModuleFileNameW(moduleHandle, path.data(), static_cast<DWORD>(path.size()));
        if (len > 0 && len < path.size()) {
            path.resize(len);
            return juce::File(juce::String(path.c_str()));
        }
    }
#elif defined(__APPLE__) || defined(__linux__)
    // macOS/Linux：解析本 symbol 所在模組（plugin bundle 或 app）的實際路徑，
    // 而非宿主可執行檔。對 VST3 bundle，dli_fname 指向
    // <Plugin>.vst3/Contents/MacOS/<Name>，向上兩層即可到達 Contents/Resources/models，
    // 使打包進 bundle 的模型可被自動偵測。
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&ModelPathResolver::getModelsDirectory), &info) != 0
        && info.dli_fname != nullptr) {
        const juce::File moduleFile(juce::String(info.dli_fname));
        if (moduleFile.exists())
            return moduleFile;
    }
#endif
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile);
}

} // namespace OpenTune