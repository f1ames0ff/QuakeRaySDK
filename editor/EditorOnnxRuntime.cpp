// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "EditorOnnxRuntime.h"

#include <onnxruntime_c_api.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace
{

std::string ReadEnvVar(const char *name)
{
    if (name == nullptr)
    {
        return {};
    }
    const char *val = std::getenv(name);
    return val != nullptr ? std::string(val) : std::string{};
}

bool IsUsableRegularFile(const std::filesystem::path &path)
{
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::filesystem::path DefaultOnnxCacheDir()
{
#if defined(_WIN32)
    const std::string localApp = ReadEnvVar("LOCALAPPDATA");
    if (!localApp.empty())
    {
        return std::filesystem::path(localApp) / "QuakeRay" / "onnx" / "bin";
    }
    const std::string appData = ReadEnvVar("APPDATA");
    if (!appData.empty())
    {
        return std::filesystem::path(appData) / "QuakeRay" / "onnx" / "bin";
    }
    return std::filesystem::path("onnx") / "bin";
#else
    const std::string xdgCache = ReadEnvVar("XDG_CACHE_HOME");
    if (!xdgCache.empty())
    {
        return std::filesystem::path(xdgCache) / "quakeray" / "onnx" / "lib";
    }
    const std::string home = ReadEnvVar("HOME");
    if (!home.empty())
    {
        return std::filesystem::path(home) / ".cache" / "quakeray" / "onnx" / "lib";
    }
    return std::filesystem::path(".cache") / "quakeray" / "onnx" / "lib";
#endif
}

std::vector<std::filesystem::path> CandidateOnnxLibraryPaths(const std::string &exeDir)
{
    std::vector<std::filesystem::path> candidates;
    const std::string envLib = ReadEnvVar("QUAKERAY_ONNXRUNTIME_LIB");
    if (!envLib.empty())
    {
        candidates.emplace_back(envLib);
    }

#if defined(_WIN32)
    static constexpr const char *kNames[] = { "onnxruntime.dll" };
#else
    static constexpr const char *kNames[] = { "libonnxruntime.so", "libonnxruntime.so.1" };
#endif

    if (!exeDir.empty())
    {
        const std::filesystem::path base(exeDir);
        for (const char *name : kNames)
        {
            candidates.push_back(base / name);
        }
    }

#if defined(_WIN32)
    const std::string appData = ReadEnvVar("APPDATA");
    const std::filesystem::path cacheDir = DefaultOnnxCacheDir();
    candidates.push_back(cacheDir / "onnxruntime.dll");
    if (!appData.empty())
    {
        candidates.push_back(std::filesystem::path(appData) / "QuakeRay" / "onnx" / "bin" /
                             "onnxruntime.dll");
    }
#else
    const std::filesystem::path cacheDir = DefaultOnnxCacheDir();
    for (const char *name : kNames)
    {
        candidates.push_back(cacheDir / name);
    }
#endif
    return candidates;
}

template <typename T>
struct OrtScopedHandle
{
    using ReleaseFn = void(ORT_API_CALL *)(T *);
    T *ptr = nullptr;
    ReleaseFn release = nullptr;

    OrtScopedHandle() = default;
    OrtScopedHandle(const OrtScopedHandle &) = delete;
    OrtScopedHandle &operator=(const OrtScopedHandle &) = delete;

    ~OrtScopedHandle()
    {
        Reset();
    }

    void Reset()
    {
        if (ptr != nullptr && release != nullptr)
        {
            release(ptr);
        }
        ptr = nullptr;
    }
};

struct OrtScopedAllocString
{
    const OrtApi *api = nullptr;
    OrtAllocator *alloc = nullptr;
    char *ptr = nullptr;

    OrtScopedAllocString(const OrtApi *apiIn, OrtAllocator *allocIn)
        : api(apiIn)
        , alloc(allocIn)
    {
    }
    OrtScopedAllocString(const OrtScopedAllocString &) = delete;
    OrtScopedAllocString &operator=(const OrtScopedAllocString &) = delete;

    ~OrtScopedAllocString()
    {
        if (ptr != nullptr && api != nullptr && alloc != nullptr)
        {
            OrtStatus *status = api->AllocatorFree(alloc, ptr);
            if (status != nullptr)
            {
                api->ReleaseStatus(status);
            }
        }
    }
};

bool CheckOrtStatus(const OrtApi *api, OrtStatus *status, std::string &outError)
{
    if (status == nullptr)
    {
        return true;
    }
    const char *msg = api->GetErrorMessage(status);
    outError = msg != nullptr ? msg : "ONNX Runtime call failed";
    api->ReleaseStatus(status);
    return false;
}

struct OrtRuntimeState
{
    std::mutex sessionMutex;
    std::mutex runCancelMutex;

#if defined(_WIN32)
    HMODULE libHandle = nullptr;
#else
    void *libHandle = nullptr;
#endif
    std::filesystem::path libPath;
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    OrtSession *session = nullptr;
    std::filesystem::path modelPath;
    std::string providerLabel;
    const OrtApi *activeRunApi = nullptr;
    OrtRunOptions *activeRunOptions = nullptr;
};

OrtRuntimeState &GlobalOrtState()
{
    static OrtRuntimeState state;
    return state;
}

void UnloadOrtLibrary(OrtRuntimeState &state)
{
    if (state.libHandle == nullptr)
    {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(state.libHandle);
#else
    dlclose(state.libHandle);
#endif
    state.libHandle = nullptr;
    state.libPath.clear();
}

void ReleaseOrtSessionAndEnv(OrtRuntimeState &state)
{
    if (state.api != nullptr)
    {
        if (state.session != nullptr)
        {
            state.api->ReleaseSession(state.session);
            state.session = nullptr;
        }
        if (state.env != nullptr)
        {
            state.api->ReleaseEnv(state.env);
            state.env = nullptr;
        }
    }
    state.session = nullptr;
    state.env = nullptr;
    state.api = nullptr;
    state.modelPath.clear();
    state.providerLabel.clear();
    UnloadOrtLibrary(state);
}

using OrtGetApiBaseFn = const OrtApiBase *(ORT_API_CALL *)();

OrtGetApiBaseFn LoadOrtSymbol(const std::filesystem::path &path, OrtRuntimeState &state,
                              std::string &outError)
{
#if defined(_WIN32)
    state.libHandle = LoadLibraryW(path.wstring().c_str());
    if (state.libHandle == nullptr)
    {
        outError = "Failed loading onnxruntime.dll from " + path.string();
        return nullptr;
    }
    auto *proc = reinterpret_cast<void *>(GetProcAddress(state.libHandle, "OrtGetApiBase"));
#else
    state.libHandle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (state.libHandle == nullptr)
    {
        const char *dlErr = dlerror();
        outError = dlErr != nullptr ? dlErr : ("Failed loading " + path.string());
        return nullptr;
    }
    void *proc = dlsym(state.libHandle, "OrtGetApiBase");
#endif
    if (proc == nullptr)
    {
        outError = "Missing OrtGetApiBase symbol in " + path.string();
        UnloadOrtLibrary(state);
        return nullptr;
    }
    state.libPath = path;
    return reinterpret_cast<OrtGetApiBaseFn>(proc);
}

bool EnsureOrtEnvLoaded(OrtRuntimeState &state, const std::string &exeDir, std::string &outError)
{
    if (state.api != nullptr && state.env != nullptr)
    {
        return true;
    }

    const std::filesystem::path libPath = ResolveEditorOnnxRuntimeLibraryPath(exeDir);
    if (libPath.empty())
    {
        outError = "ONNX Runtime library not found. Expected libonnxruntime next to executable or "
                   "in " +
                   DefaultOnnxCacheDir().string();
        return false;
    }

    OrtGetApiBaseFn getApiBase = LoadOrtSymbol(libPath, state, outError);
    if (getApiBase == nullptr)
    {
        return false;
    }

    const OrtApiBase *base = getApiBase();
    if (base == nullptr)
    {
        outError = "OrtGetApiBase returned null.";
        UnloadOrtLibrary(state);
        return false;
    }

    static constexpr std::array<uint32_t, 4> kApiVersions = { ORT_API_VERSION, 24u, 22u, 20u };
    for (uint32_t ver : kApiVersions)
    {
        state.api = base->GetApi(ver);
        if (state.api != nullptr)
        {
            break;
        }
    }
    if (state.api == nullptr)
    {
        const char *rtVer = base->GetVersionString();
        outError = std::string("Incompatible ONNX Runtime ABI version: ") +
                   (rtVer != nullptr ? rtVer : "unknown");
        UnloadOrtLibrary(state);
        return false;
    }

    if (!CheckOrtStatus(state.api,
                        state.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "quakeray", &state.env),
                        outError))
    {
        ReleaseOrtSessionAndEnv(state);
        return false;
    }
    return true;
}

struct EpCandidate
{
    const char *name;
    const char *label;
};

bool AppendPreferredGpuProvider(const OrtApi *api, OrtSessionOptions *opts, std::string &outLabel)
{
#if defined(_WIN32)
    static constexpr std::array<EpCandidate, 3> kProviders = { {
        { "DML", "DirectML GPU" },
        { "WebGPU", "WebGPU Vulkan" },
        { "CUDA", "CUDA GPU" },
    } };
#else
    static constexpr std::array<EpCandidate, 3> kProviders = { {
        { "WebGPU", "WebGPU Vulkan" },
        { "MIGraphX", "MIGraphX GPU" },
        { "CUDA", "CUDA GPU" },
    } };
#endif

    for (const EpCandidate &ep : kProviders)
    {
        OrtStatus *status =
            api->SessionOptionsAppendExecutionProvider(opts, ep.name, nullptr, nullptr, 0);
        if (status == nullptr)
        {
            outLabel = ep.label;
            return true;
        }
        api->ReleaseStatus(status);
    }
    outLabel = "CPU";
    return false;
}

bool CreateSessionWithOptions(OrtRuntimeState &state, const std::filesystem::path &modelPath,
                              bool useGpu, std::string &outLabel, std::string &outError)
{
    OrtScopedHandle<OrtSessionOptions> opts;
    opts.release = state.api->ReleaseSessionOptions;
    if (!CheckOrtStatus(state.api, state.api->CreateSessionOptions(&opts.ptr), outError))
    {
        return false;
    }
    if (!CheckOrtStatus(state.api,
                        state.api->SetSessionGraphOptimizationLevel(opts.ptr, ORT_ENABLE_BASIC),
                        outError))
    {
        return false;
    }

    outLabel = "CPU";
    if (useGpu)
    {
        AppendPreferredGpuProvider(state.api, opts.ptr, outLabel);
    }

    OrtSession *created = nullptr;
#if defined(_WIN32)
    OrtStatus *status =
        state.api->CreateSession(state.env, modelPath.wstring().c_str(), opts.ptr, &created);
#else
    OrtStatus *status =
        state.api->CreateSession(state.env, modelPath.string().c_str(), opts.ptr, &created);
#endif
    if (!CheckOrtStatus(state.api, status, outError))
    {
        return false;
    }

    state.session = created;
    state.modelPath = modelPath;
    state.providerLabel = outLabel;
    return true;
}

bool EnsureOrtSession(OrtRuntimeState &state, const std::filesystem::path &modelPath,
                      const std::string &exeDir, std::string &outError)
{
    if (!EnsureOrtEnvLoaded(state, exeDir, outError))
    {
        return false;
    }
    if (!IsUsableRegularFile(modelPath))
    {
        outError = "ONNX model file not found: " + modelPath.string();
        return false;
    }

    const std::filesystem::path normModel = modelPath.lexically_normal();
    if (state.session != nullptr && state.modelPath == normModel)
    {
        return true;
    }

    if (state.session != nullptr)
    {
        state.api->ReleaseSession(state.session);
        state.session = nullptr;
        state.modelPath.clear();
        state.providerLabel.clear();
    }

    std::string gpuErr;
    std::string label;
    if (CreateSessionWithOptions(state, normModel, true, label, gpuErr))
    {
        return true;
    }

    std::string cpuErr;
    if (CreateSessionWithOptions(state, normModel, false, label, cpuErr))
    {
        return true;
    }

    outError = !gpuErr.empty() ? gpuErr : cpuErr;
    return false;
}

bool ExtractOutputTensorNchw(const OrtApi *api, OrtValue *outTensor, EditorOnnxTensorNchw &outData,
                             std::string &outError)
{
    OrtScopedHandle<OrtTensorTypeAndShapeInfo> info;
    info.release = api->ReleaseTensorTypeAndShapeInfo;
    if (!CheckOrtStatus(api, api->GetTensorTypeAndShape(outTensor, &info.ptr), outError))
    {
        return false;
    }

    size_t rank = 0;
    if (!CheckOrtStatus(api, api->GetDimensionsCount(info.ptr, &rank), outError))
    {
        return false;
    }
    if (rank != 4u)
    {
        outError = "Unexpected ONNX output tensor rank (expected 4D NCHW).";
        return false;
    }

    std::array<int64_t, 4> dims = {};
    if (!CheckOrtStatus(api, api->GetDimensions(info.ptr, dims.data(), dims.size()), outError))
    {
        return false;
    }
    if (dims[0] < 1 || dims[1] <= 0 || dims[2] <= 0 || dims[3] <= 0)
    {
        outError = "Invalid ONNX output tensor dimensions.";
        return false;
    }

    void *rawOut = nullptr;
    if (!CheckOrtStatus(api, api->GetTensorMutableData(outTensor, &rawOut), outError))
    {
        return false;
    }
    if (rawOut == nullptr)
    {
        outError = "ONNX output tensor data pointer is null.";
        return false;
    }

    const size_t totalFloats = size_t(dims[1]) * size_t(dims[2]) * size_t(dims[3]);
    outData.channels = dims[1];
    outData.height = dims[2];
    outData.width = dims[3];
    outData.data.resize(totalFloats);
    std::memcpy(outData.data.data(), rawOut, totalFloats * sizeof(float));
    return true;
}

class ActiveRunGuard
{
  public:
    ActiveRunGuard(OrtRuntimeState &stateIn, const OrtApi *apiIn, OrtRunOptions *runOpts)
        : m_state(stateIn)
    {
        std::lock_guard<std::mutex> lock(m_state.runCancelMutex);
        m_state.activeRunApi = apiIn;
        m_state.activeRunOptions = runOpts;
    }
    ActiveRunGuard(const ActiveRunGuard &) = delete;
    ActiveRunGuard &operator=(const ActiveRunGuard &) = delete;

    ~ActiveRunGuard()
    {
        std::lock_guard<std::mutex> lock(m_state.runCancelMutex);
        m_state.activeRunOptions = nullptr;
        m_state.activeRunApi = nullptr;
    }

  private:
    OrtRuntimeState &m_state;
};

bool ExecuteSessionRunNchw(OrtRuntimeState &state, const EditorOnnxTensorNchw &input,
                           EditorOnnxTensorNchw &output, const std::atomic<bool> *cancelRequested,
                           std::string &outError)
{
    const OrtApi *api = state.api;
    OrtAllocator *alloc = nullptr;
    if (!CheckOrtStatus(api, api->GetAllocatorWithDefaultOptions(&alloc), outError))
    {
        return false;
    }

    OrtScopedAllocString inputName(api, alloc);
    OrtScopedAllocString outputName(api, alloc);
    if (!CheckOrtStatus(api, api->SessionGetInputName(state.session, 0u, alloc, &inputName.ptr),
                        outError) ||
        !CheckOrtStatus(api, api->SessionGetOutputName(state.session, 0u, alloc, &outputName.ptr),
                        outError))
    {
        return false;
    }

    OrtScopedHandle<OrtMemoryInfo> memInfo;
    memInfo.release = api->ReleaseMemoryInfo;
    if (!CheckOrtStatus(
            api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo.ptr),
            outError))
    {
        return false;
    }

    const std::array<int64_t, 4> inShape = { 1, input.channels, input.height, input.width };
    std::vector<float> mutableInput = input.data;
    OrtScopedHandle<OrtValue> inTensor;
    inTensor.release = api->ReleaseValue;
    if (!CheckOrtStatus(
            api,
            api->CreateTensorWithDataAsOrtValue(memInfo.ptr, mutableInput.data(),
                                               mutableInput.size() * sizeof(float), inShape.data(),
                                               inShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                               &inTensor.ptr),
            outError))
    {
        return false;
    }

    OrtScopedHandle<OrtRunOptions> runOpts;
    runOpts.release = api->ReleaseRunOptions;
    if (!CheckOrtStatus(api, api->CreateRunOptions(&runOpts.ptr), outError))
    {
        return false;
    }

    ActiveRunGuard runGuard(state, api, runOpts.ptr);
    if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
    {
        OrtStatus *termStatus = api->RunOptionsSetTerminate(runOpts.ptr);
        if (termStatus != nullptr)
        {
            api->ReleaseStatus(termStatus);
        }
    }

    const char *inNames[] = { inputName.ptr };
    const char *outNames[] = { outputName.ptr };
    const OrtValue *inValues[] = { inTensor.ptr };
    OrtScopedHandle<OrtValue> outTensor;
    outTensor.release = api->ReleaseValue;

    if (!CheckOrtStatus(api,
                        api->Run(state.session, runOpts.ptr, inNames, inValues, 1u, outNames, 1u,
                                 &outTensor.ptr),
                        outError))
    {
        return false;
    }

    return ExtractOutputTensorNchw(api, outTensor.ptr, output, outError);
}

} // namespace

std::filesystem::path ResolveEditorOnnxRuntimeLibraryPath(const std::string &exeDir)
{
    for (const std::filesystem::path &candidate : CandidateOnnxLibraryPaths(exeDir))
    {
        if (IsUsableRegularFile(candidate))
        {
            return candidate.lexically_normal();
        }
    }
    return {};
}

bool IsEditorOnnxRuntimeAvailable(const std::string &exeDir)
{
    return !ResolveEditorOnnxRuntimeLibraryPath(exeDir).empty();
}

bool RunEditorOnnxModelNchw(const std::filesystem::path &modelPath, const std::string &exeDir,
                            const EditorOnnxTensorNchw &input, EditorOnnxTensorNchw &output,
                            const std::atomic<bool> *cancelRequested, std::string &outProviderLabel,
                            std::string &outError)
{
    output = {};
    outProviderLabel.clear();
    outError.clear();

    if (input.channels <= 0 || input.height <= 0 || input.width <= 0)
    {
        outError = "Invalid input tensor dimensions for ONNX Runtime.";
        return false;
    }
    const size_t expectedFloats =
        size_t(input.channels) * size_t(input.height) * size_t(input.width);
    if (input.data.size() != expectedFloats)
    {
        outError = "ONNX input tensor buffer size mismatch.";
        return false;
    }

    OrtRuntimeState &state = GlobalOrtState();
    std::lock_guard<std::mutex> sessionLock(state.sessionMutex);

    if (cancelRequested != nullptr && cancelRequested->load(std::memory_order_relaxed))
    {
        outError = "Cancelled";
        return false;
    }
    if (!EnsureOrtSession(state, modelPath, exeDir, outError))
    {
        return false;
    }
    outProviderLabel = state.providerLabel;
    return ExecuteSessionRunNchw(state, input, output, cancelRequested, outError);
}

void CancelActiveEditorOnnxRun()
{
    OrtRuntimeState &state = GlobalOrtState();
    std::lock_guard<std::mutex> lock(state.runCancelMutex);
    if (state.activeRunApi != nullptr && state.activeRunOptions != nullptr)
    {
        OrtStatus *status = state.activeRunApi->RunOptionsSetTerminate(state.activeRunOptions);
        if (status != nullptr)
        {
            state.activeRunApi->ReleaseStatus(status);
        }
    }
}

void ShutdownEditorOnnxRuntime()
{
    CancelActiveEditorOnnxRun();
    OrtRuntimeState &state = GlobalOrtState();
    std::lock_guard<std::mutex> lock(state.sessionMutex);
    ReleaseOrtSessionAndEnv(state);
}
