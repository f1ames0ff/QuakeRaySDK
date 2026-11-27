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

#include "EditorAiModelDownload.h"
#include "EditorAiTexture.h"
#include "EditorAiWorkerClient.h"
#include "EditorApp.h"
#include "EditorGui.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

enum AiTabIndex
{
    kTabUpscale = 0,
    kTabDelight = 1,
    kTabNormals = 2,
    kTabDepth = 3,
    kTabEmissive = 4,
    kTabCount = 5,
};

enum DepthTarget
{
    kDepthTargetHeight = 0,
    kDepthTargetNormals = 1,
    kDepthTargetGloss = 2,
};

struct AiTabData
{
    bool hasGeneratedOnce = false;
    bool hasResult = false;
    uint32_t rawTexW = 0;
    uint32_t rawTexH = 0;
    uint32_t resultWidth = 0;
    uint32_t resultHeight = 0;
    std::vector<uint8_t> rawNormalRgba;
    std::vector<float> rawHeight;
    std::vector<uint8_t> rawColorRgba;
    std::vector<uint8_t> finalRgba;
    std::vector<uint8_t> displayPreviewRgba;
    QrMaterial resultPreviewMat = QR_NO_MATERIAL;

    int32_t viewMode = 0;
    float lightAngleDeg = -125.0f;
    std::string providerBadge;
    std::string appliedRelPath;

    EditorAiNormalParams normalParams;
    EditorAiToneParams toneParams;
    EditorAiDelightParams delightParams;
    EditorAiUpscaleParams upscaleParams;
    float preSmooth = 0.0f;
    bool useUpscaledDiffuse = true;
    bool useDelitDiffuse = true;
    int depthTarget = kDepthTargetHeight;
    bool microDetailEdited = false;
};

} // namespace

struct EditorAiDialogState
{
    bool open = false;
    bool emissiveOnly = false;
    int32_t materialIndex = -1;
    std::string materialName;
    std::string diffuseName;

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> diffuseRgba;
    QrMaterial leftPreviewMat = QR_NO_MATERIAL;
    uint32_t leftPreviewWidth = 0;
    uint32_t leftPreviewHeight = 0;
    bool leftPreviewUsesUpscaled = false;
    bool leftPreviewUsesPreprocessed = false;
    bool leftPreviewUsesDelit = false;

    EditorAiLocalConfig localConfig;
    std::string configStatus;

    int activeTab = kTabNormals;
    int pendingSelectTab = -1;
    std::array<AiTabData, kTabCount> tabs;

    int workerTargetTab = kTabNormals;
    bool generating = false;
    std::atomic<bool> cancelRequested{ false };
    std::atomic<bool> workerDone{ false };
    std::thread workerThread;
    std::chrono::steady_clock::time_point generateStart;
    std::string errorMessage;

    bool downloading = false;
    std::atomic<bool> downloadCancelRequested{ false };
    std::atomic<bool> downloadDone{ false };
    std::thread downloadThread;
    std::chrono::steady_clock::time_point downloadStart;
    EditorAiDownloadProgress downloadProgress;
    EditorAiBackend downloadBackend = EditorAiBackend::LotusNormalD;

    std::mutex workerMutex;
    EditorAiInferenceResult workerOutput;
    mutable std::mutex progressMutex;
    int32_t progressPercent = 0;
    std::string progressStage;
    std::mutex downloadMutex;
    EditorAiDownloadResult downloadOutput;
};

namespace
{

const char *SlotLabelForAi(EditorApp::EditorTextureSlot slot)
{
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Base:
        return "texture_base";
    case EditorApp::EditorTextureSlot::Normals:
        return "texture_normals";
    case EditorApp::EditorTextureSlot::Height:
        return "texture_height";
    case EditorApp::EditorTextureSlot::Gloss:
        return "texture_gloss";
    case EditorApp::EditorTextureSlot::Emissive:
        return "texture_emissive";
    default:
        return "texture";
    }
}

EditorApp::EditorTextureSlot TargetSlotForTab(const EditorAiDialogState &state, int tabIdx)
{
    switch (tabIdx)
    {
    case kTabUpscale:
    case kTabDelight:
        return EditorApp::EditorTextureSlot::Base;
    case kTabNormals:
        return EditorApp::EditorTextureSlot::Normals;
    case kTabDepth:
        if (state.tabs[size_t(tabIdx)].depthTarget == kDepthTargetHeight)
        {
            return EditorApp::EditorTextureSlot::Height;
        }
        return state.tabs[size_t(tabIdx)].depthTarget == kDepthTargetNormals
                   ? EditorApp::EditorTextureSlot::Normals
                   : EditorApp::EditorTextureSlot::Gloss;
    case kTabEmissive:
        return EditorApp::EditorTextureSlot::Emissive;
    default:
        return EditorApp::EditorTextureSlot::Normals;
    }
}

EditorAiMapKind TargetMapKindForTab(const EditorAiDialogState &state, int tabIdx)
{
    switch (tabIdx)
    {
    case kTabUpscale:
        return EditorAiMapKind::Upscale;
    case kTabDelight:
        return EditorAiMapKind::Delight;
    case kTabNormals:
        return EditorAiMapKind::Normals;
    case kTabDepth:
        if (state.tabs[size_t(tabIdx)].depthTarget == kDepthTargetHeight)
        {
            return EditorAiMapKind::Height;
        }
        return state.tabs[size_t(tabIdx)].depthTarget == kDepthTargetNormals
                   ? EditorAiMapKind::Normals
                   : EditorAiMapKind::Gloss;
    case kTabEmissive:
        return EditorAiMapKind::Emissive;
    default:
        return EditorAiMapKind::Normals;
    }
}

EditorAiBackend ActiveBackendForTab(const EditorAiDialogState &, int tabIdx)
{
    switch (tabIdx)
    {
    case kTabUpscale:
        return EditorAiBackend::RealPlksrPbrify;
    case kTabDelight:
        return EditorAiBackend::StableDelight;
    case kTabNormals:
        return EditorAiBackend::DeepBump;
    case kTabDepth:
        return EditorAiBackend::LotusDepthD;
    default:
        return EditorAiBackend::DeepBump;
    }
}

struct SelectedSourceInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool usedUpscaled = false;
    bool usedDelit = false;
    bool usedPreprocessed = false;
};

bool HasReadyTabOutput(const AiTabData &tab)
{
    return tab.hasResult && !tab.finalRgba.empty() && tab.resultWidth > 0u &&
           tab.resultHeight > 0u;
}

const std::vector<uint8_t> &SelectTabSourceDiffuse(const EditorAiDialogState &state, int tabIdx,
                                                   SelectedSourceInfo &outInfo)
{
    outInfo = { state.width, state.height, false, false, false };
    const AiTabData &upscale = state.tabs[kTabUpscale];
    const AiTabData &delight = state.tabs[kTabDelight];
    const AiTabData &tab = state.tabs[size_t(tabIdx)];

    const bool canUseDelit = (tabIdx == kTabNormals || tabIdx == kTabDepth) &&
                             tab.useDelitDiffuse && HasReadyTabOutput(delight);
    if (canUseDelit)
    {
        outInfo.width = delight.resultWidth;
        outInfo.height = delight.resultHeight;
        outInfo.usedDelit = true;
        outInfo.usedUpscaled =
            (delight.resultWidth != state.width || delight.resultHeight != state.height);
        return delight.finalRgba;
    }

    const bool allowUpscaled =
        (tabIdx == kTabDelight || tabIdx == kTabNormals || tabIdx == kTabDepth) &&
        tab.useUpscaledDiffuse && HasReadyTabOutput(upscale);
    if (allowUpscaled)
    {
        outInfo.width = upscale.resultWidth;
        outInfo.height = upscale.resultHeight;
        outInfo.usedUpscaled = true;
        return upscale.finalRgba;
    }
    return state.diffuseRgba;
}

void DestroyDialogMaterial(QrInstance instance, QrMaterial &mat)
{
    if (instance != nullptr && mat != QR_NO_MATERIAL)
    {
        qrDestroyMaterial(instance, mat);
    }
    mat = QR_NO_MATERIAL;
}

QrMaterial CreatePreviewMaterialFromRgba(QrInstance instance, uint32_t width, uint32_t height,
                                         const std::vector<uint8_t> &rgba)
{
    if (instance == nullptr || width == 0 || height == 0 ||
        rgba.size() != size_t(width) * size_t(height) * 4u)
    {
        return QR_NO_MATERIAL;
    }
    QrMaterialCreateInfo info = {};
    info.size = { width, height };
    info.textures.pDataAlbedoAlpha = rgba.data();
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    QrMaterial created = QR_NO_MATERIAL;
    if (qrCreateMaterial(instance, &info, &created) != QR_SUCCESS)
    {
        return QR_NO_MATERIAL;
    }
    return created;
}

void BuildActiveInputRgba(const EditorAiDialogState &state, int tabIdx, std::vector<uint8_t> &out,
                          SelectedSourceInfo &outInfo)
{
    const std::vector<uint8_t> &baseSrc = SelectTabSourceDiffuse(state, tabIdx, outInfo);
    const AiTabData &tab = state.tabs[size_t(tabIdx)];
    outInfo.usedPreprocessed = false;
    if ((tabIdx == kTabNormals || tabIdx == kTabDepth) && tab.preSmooth > 1e-4f)
    {
        outInfo.usedPreprocessed = true;
        const EditorAiDiffusePrepParams prep{ 0.0f, tab.preSmooth, tab.normalParams.seamless };
        PreprocessEditorAiDiffuse(baseSrc, outInfo.width, outInfo.height, prep, out);
        return;
    }
    out = baseSrc;
}

void RefreshLeftPreviewForActiveTab(QrInstance instance, EditorAiDialogState &state)
{
    DestroyDialogMaterial(instance, state.leftPreviewMat);
    std::vector<uint8_t> inputRgba;
    SelectedSourceInfo info;
    BuildActiveInputRgba(state, state.activeTab, inputRgba, info);
    state.leftPreviewWidth = info.width;
    state.leftPreviewHeight = info.height;
    state.leftPreviewUsesUpscaled = info.usedUpscaled;
    state.leftPreviewUsesDelit = info.usedDelit;
    state.leftPreviewUsesPreprocessed = info.usedPreprocessed;
    state.leftPreviewMat = CreatePreviewMaterialFromRgba(instance, state.leftPreviewWidth,
                                                         state.leftPreviewHeight, inputRgba);
}

void RebuildNormalTabResult(uint32_t texW, uint32_t texH, const std::vector<uint8_t> &srcDiffuse,
                            AiTabData &tab)
{
    if (!tab.rawNormalRgba.empty())
    {
        AdjustEditorNormalMap(tab.rawNormalRgba, texW, texH, &srcDiffuse, tab.normalParams,
                              tab.finalRgba);
    }
    else
    {
        BuildEditorNormalMapFromHeight(tab.rawHeight, texW, texH, &srcDiffuse, tab.normalParams,
                                       tab.finalRgba);
    }

    if (tab.viewMode == 1 && !tab.rawHeight.empty())
    {
        BuildEditorHeightPreviewRgba(tab.rawHeight, texW, texH, tab.normalParams.invertHeight,
                                     tab.displayPreviewRgba);
    }
    else if (tab.viewMode == 2)
    {
        BuildEditorNormalLitPreviewRgba(srcDiffuse, tab.finalRgba, texW, texH, tab.lightAngleDeg,
                                        tab.displayPreviewRgba);
    }
    else
    {
        tab.displayPreviewRgba = tab.finalRgba;
    }
}

void BuildAdjustedHeightFieldAndRgba(const std::vector<float> &rawHeight, uint32_t width,
                                     uint32_t height, bool invertHeight,
                                     const EditorAiToneParams &tone,
                                     std::vector<float> &outAdjHeight,
                                     std::vector<uint8_t> &outRgba)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || rawHeight.size() != pixels)
    {
        outAdjHeight.clear();
        outRgba.clear();
        return;
    }
    const float gain = std::clamp(tone.contrastGain, 0.2f, 3.0f);
    const float bias = std::clamp(tone.biasThreshold, -0.5f, 0.8f);
    outAdjHeight.resize(pixels);
    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float baseH = invertHeight ? (1.0f - rawHeight[i]) : rawHeight[i];
        const float adj = std::clamp((baseH - 0.5f) * gain + 0.5f + bias, 0.0f, 1.0f);
        outAdjHeight[i] = adj;
        const uint8_t v = uint8_t(std::clamp(std::lround(adj * 255.0f), 0l, 255l));
        outRgba[i * 4u + 0u] = v;
        outRgba[i * 4u + 1u] = v;
        outRgba[i * 4u + 2u] = v;
        outRgba[i * 4u + 3u] = 255u;
    }
}

void RebuildDepthTabResult(uint32_t texW, uint32_t texH, const std::vector<uint8_t> &srcDiffuse,
                           AiTabData &tab)
{
    if (tab.depthTarget == kDepthTargetHeight)
    {
        std::vector<float> adjHeight;
        BuildAdjustedHeightFieldAndRgba(tab.rawHeight, texW, texH, tab.normalParams.invertHeight,
                                        tab.toneParams, adjHeight, tab.finalRgba);
        if (tab.viewMode == 1 && !adjHeight.empty())
        {
            EditorAiNormalParams previewNorm = tab.normalParams;
            previewNorm.invertHeight = false;
            std::vector<uint8_t> tmpNormal;
            BuildEditorNormalMapFromHeight(adjHeight, texW, texH, &srcDiffuse, previewNorm,
                                           tmpNormal);
            BuildEditorNormalLitPreviewRgba(srcDiffuse, tmpNormal, texW, texH, tab.lightAngleDeg,
                                            tab.displayPreviewRgba);
        }
        else
        {
            tab.displayPreviewRgba = tab.finalRgba;
        }
        return;
    }

    if (tab.depthTarget == kDepthTargetNormals)
    {
        BuildEditorNormalMapFromHeight(tab.rawHeight, texW, texH, &srcDiffuse, tab.normalParams,
                                       tab.finalRgba);
        if (tab.viewMode == 0)
        {
            BuildEditorHeightPreviewRgba(tab.rawHeight, texW, texH, tab.normalParams.invertHeight,
                                         tab.displayPreviewRgba);
        }
        else if (tab.viewMode == 2)
        {
            BuildEditorNormalLitPreviewRgba(srcDiffuse, tab.finalRgba, texW, texH, tab.lightAngleDeg,
                                            tab.displayPreviewRgba);
        }
        else
        {
            tab.displayPreviewRgba = tab.finalRgba;
        }
        return;
    }

    if (tab.rawColorRgba.empty() && !tab.rawHeight.empty())
    {
        SynthesizeEditorGlossFromDiffuseAndHeight(srcDiffuse, tab.rawHeight, texW, texH,
                                                  tab.rawColorRgba);
    }
    ApplyEditorGlossAdjustments(tab.rawColorRgba, texW, texH, tab.toneParams, tab.finalRgba);
    if (tab.viewMode == 0 && !tab.rawHeight.empty())
    {
        BuildEditorHeightPreviewRgba(tab.rawHeight, texW, texH, false, tab.displayPreviewRgba);
    }
    else if (tab.viewMode == 2)
    {
        BuildEditorGlossSpecularPreviewRgba(srcDiffuse, tab.finalRgba, texW, texH, tab.lightAngleDeg,
                                            tab.displayPreviewRgba);
    }
    else
    {
        tab.displayPreviewRgba = tab.finalRgba;
    }
}

void RebuildUpscaleTabResult(QrInstance instance, const EditorAiDialogState &state, AiTabData &tab)
{
    if (state.width == 0u || state.height == 0u)
    {
        return;
    }
    const EditorAiUpscaleInput in{
        state.diffuseRgba, state.width, state.height, tab.rawColorRgba, tab.rawTexW, tab.rawTexH,
    };
    ApplyEditorUpscaleAdjustments(in, tab.upscaleParams, tab.finalRgba, tab.resultWidth,
                                  tab.resultHeight);
    if (tab.viewMode == 1)
    {
        BuildEditorUpscaleComparePreviewRgba(state.diffuseRgba, state.width, state.height,
                                             tab.finalRgba, tab.resultWidth, tab.resultHeight,
                                             tab.displayPreviewRgba);
    }
    else if (tab.viewMode == 2)
    {
        BuildEditorUpscaleDetailPreviewRgba(state.diffuseRgba, state.width, state.height,
                                            tab.finalRgba, tab.resultWidth, tab.resultHeight,
                                            tab.displayPreviewRgba);
    }
    else
    {
        tab.displayPreviewRgba = tab.finalRgba;
    }
    DestroyDialogMaterial(instance, tab.resultPreviewMat);
    tab.resultPreviewMat = CreatePreviewMaterialFromRgba(instance, tab.resultWidth,
                                                         tab.resultHeight, tab.displayPreviewRgba);
}

void RebuildDelightOrEmissiveTabResult(int tabIdx, uint32_t texW, uint32_t texH,
                                       const std::vector<uint8_t> &srcRgba, AiTabData &tab)
{
    if (tabIdx == kTabDelight)
    {
        ApplyEditorDelightAdjustments(srcRgba, tab.rawColorRgba, texW, texH, tab.delightParams,
                                      tab.finalRgba);
        if (tab.viewMode == 1)
        {
            BuildEditorDelightRemovedGlarePreviewRgba(srcRgba, tab.finalRgba, texW, texH,
                                                      tab.displayPreviewRgba);
        }
        else
        {
            tab.displayPreviewRgba = tab.finalRgba;
        }
        return;
    }

    ApplyEditorEmissiveAdjustments(tab.rawColorRgba, texW, texH, tab.toneParams, tab.finalRgba);
    if (tab.viewMode == 1)
    {
        BuildEditorEmissiveCompositePreviewRgba(srcRgba, tab.finalRgba, texW, texH,
                                                tab.displayPreviewRgba);
    }
    else
    {
        tab.displayPreviewRgba = tab.finalRgba;
    }
}

void RebuildTabResultAndPreview(QrInstance instance, EditorAiDialogState &state, int tabIdx)
{
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    if (!tab.hasResult || tab.rawTexW == 0u || tab.rawTexH == 0u)
    {
        return;
    }
    if (tabIdx == kTabUpscale)
    {
        RebuildUpscaleTabResult(instance, state, tab);
        return;
    }

    const uint32_t texW = tab.rawTexW;
    const uint32_t texH = tab.rawTexH;
    tab.resultWidth = texW;
    tab.resultHeight = texH;

    SelectedSourceInfo srcInfo;
    const std::vector<uint8_t> &rawSrc = SelectTabSourceDiffuse(state, tabIdx, srcInfo);
    std::vector<uint8_t> resizedSrc;
    const std::vector<uint8_t> *srcPtr = &rawSrc;
    if (srcInfo.width != texW || srcInfo.height != texH)
    {
        ResizeEditorRgbaBilinear(rawSrc, srcInfo.width, srcInfo.height, texW, texH, resizedSrc);
        srcPtr = &resizedSrc;
    }

    if (tabIdx == kTabNormals)
    {
        RebuildNormalTabResult(texW, texH, *srcPtr, tab);
    }
    else if (tabIdx == kTabDepth)
    {
        RebuildDepthTabResult(texW, texH, *srcPtr, tab);
    }
    else
    {
        RebuildDelightOrEmissiveTabResult(tabIdx, texW, texH, *srcPtr, tab);
    }

    DestroyDialogMaterial(instance, tab.resultPreviewMat);
    tab.resultPreviewMat =
        CreatePreviewMaterialFromRgba(instance, texW, texH, tab.displayPreviewRgba);
}

bool ResolveAndValidateUniqueAiDiskPath(const std::filesystem::path &outRoot,
                                        const std::string &relPath,
                                        std::filesystem::path &outDiskPath,
                                        std::string &outError)
{
    if (relPath.empty())
    {
        outError = "Failed allocating non-overwriting AI output filename.";
        return false;
    }
    outDiskPath = outRoot / relPath;
    std::error_code ec;
    if (std::filesystem::exists(outDiskPath, ec))
    {
        outError = "Refusing to overwrite existing texture " + relPath;
        return false;
    }
    return true;
}

void JoinAiWorkerIfRunning(EditorAiDialogState &state, bool cancelIfRunning)
{
    if (cancelIfRunning)
    {
        state.cancelRequested.store(true, std::memory_order_relaxed);
        CancelActiveEditorAiWorker();
    }
    if (state.workerThread.joinable())
    {
        state.workerThread.join();
    }
    state.generating = false;
    state.workerDone.store(false, std::memory_order_relaxed);
    state.cancelRequested.store(false, std::memory_order_relaxed);
}

void JoinAiDownloadIfRunning(EditorAiDialogState &state, bool cancelIfRunning)
{
    if (cancelIfRunning)
    {
        state.downloadCancelRequested.store(true, std::memory_order_relaxed);
    }
    if (state.downloadThread.joinable())
    {
        state.downloadThread.join();
    }
    state.downloading = false;
    state.downloadDone.store(false, std::memory_order_relaxed);
    state.downloadCancelRequested.store(false, std::memory_order_relaxed);
}

std::string ResolveAiOutputRootDir(const std::string &gamePath, const std::string &materialsPath,
                                   const std::string &scenePath)
{
    if (!gamePath.empty())
    {
        return gamePath;
    }
    std::string matDir = EditorDirectoryOf(materialsPath);
    if (!matDir.empty())
    {
        return matDir;
    }
    std::string sceneDir = EditorDirectoryOf(scenePath);
    if (!sceneDir.empty())
    {
        return sceneDir;
    }
    return ".";
}

void AssignMaterialSlotTexture(EditorMaterialParams &params, EditorApp::EditorTextureSlot slot,
                               const std::string &relPath)
{
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Base:
        params.baseTexture = relPath;
        break;
    case EditorApp::EditorTextureSlot::Normals:
        params.normalTexture = relPath;
        break;
    case EditorApp::EditorTextureSlot::Height:
        params.heightTexture = relPath;
        break;
    case EditorApp::EditorTextureSlot::Gloss:
        params.glossTexture = relPath;
        break;
    case EditorApp::EditorTextureSlot::Emissive:
        params.emissiveTexture = relPath;
        break;
    default:
        break;
    }
    if (!relPath.empty())
    {
        EditorApp::AutoEnableMaterialSlot(params, slot);
    }
}

void SaveLocalConfigQuietly(EditorAiDialogState &state)
{
    std::string err;
    if (!SaveEditorAiLocalConfig(DefaultEditorAiLocalConfigPath(), state.localConfig, err))
    {
        state.configStatus = err;
    }
    else
    {
        state.configStatus.clear();
    }
}

int32_t UpscaleIndexForResolution(uint32_t processingRes)
{
    if (processingRes <= 576u)
    {
        return 0;
    }
    if (processingRes >= 896u)
    {
        return 2;
    }
    return 1;
}

bool CheckTabModelAndRuntimeReady(const EditorAiDialogState &state, int tabIdx,
                                    bool &outMissingModelOnly, std::string &outMissingHint)
{
    outMissingModelOnly = false;
    outMissingHint.clear();
    if (tabIdx == kTabEmissive)
    {
        return true;
    }
    const std::string exeDir = EditorExecutableDirectory();
    if (!IsEditorAiWorkerAvailable(exeDir))
    {
        outMissingHint = "AI worker missing (expected quakeray_ai_worker next to quakerayeditor)";
        return false;
    }
    const EditorAiBackend backend = ActiveBackendForTab(state, tabIdx);
    if (ResolveEditorAiModelPath(backend, exeDir, state.localConfig.modelsDir).empty())
    {
        outMissingModelOnly = true;
        outMissingHint = std::string("Model missing: ") +
                         (DefaultEditorAiModelsDir() / EditorAiBackendModelFilename(backend)).string();
        return false;
    }
    return true;
}

void LaunchGenerationWorker(EditorAiDialogState &state)
{
    JoinAiWorkerIfRunning(state, true);
    JoinAiDownloadIfRunning(state, true);
    state.errorMessage.clear();
    {
        std::lock_guard<std::mutex> lock(state.progressMutex);
        state.progressPercent = 0;
        state.progressStage = "Starting AI worker...";
    }

    const int tabIdx = state.activeTab;
    const AiTabData &tab = state.tabs[size_t(tabIdx)];
    SelectedSourceInfo srcInfo;
    EditorAiInferenceRequest req;
    req.backend = ActiveBackendForTab(state, tabIdx);
    req.mapKind = TargetMapKindForTab(state, tabIdx);
    req.diffuseRgba = SelectTabSourceDiffuse(state, tabIdx, srcInfo);
    req.width = srcInfo.width;
    req.height = srcInfo.height;
    req.processingRes = std::max<uint32_t>(512u, state.localConfig.processingRes);
    if (tabIdx == kTabUpscale)
    {
        req.seamless = tab.upscaleParams.seamless;
    }
    else if (tabIdx == kTabDelight)
    {
        req.seamless = tab.delightParams.seamless;
    }
    else
    {
        req.seamless = tab.normalParams.seamless;
    }
    req.flattenLighting = 0.0f;
    req.preSmooth = (tabIdx == kTabNormals || tabIdx == kTabDepth) ? tab.preSmooth : 0.0f;
    req.modelsDir = state.localConfig.modelsDir;
    req.exeDir = EditorExecutableDirectory();
    req.progressCb = [&state](int32_t pct, const std::string &stage) {
        std::lock_guard<std::mutex> lock(state.progressMutex);
        state.progressPercent = std::clamp(pct, 0, 100);
        state.progressStage = stage;
    };

    state.workerTargetTab = tabIdx;
    state.generating = true;
    state.cancelRequested.store(false, std::memory_order_relaxed);
    state.workerDone.store(false, std::memory_order_relaxed);
    state.generateStart = std::chrono::steady_clock::now();

    state.workerThread = std::thread([&state, req = std::move(req)]() {
        EditorAiInferenceResult res = RunEditorAiTextureInference(req, &state.cancelRequested);
        {
            std::lock_guard<std::mutex> lock(state.workerMutex);
            state.workerOutput = std::move(res);
        }
        state.workerDone.store(true, std::memory_order_release);
    });
}

void LaunchModelDownloadWorker(EditorAiDialogState &state)
{
    JoinAiWorkerIfRunning(state, true);
    JoinAiDownloadIfRunning(state, true);
    state.errorMessage.clear();

    const EditorAiBackend backend = ActiveBackendForTab(state, state.activeTab);
    const EditorAiModelDownloadSpec spec = GetEditorAiModelDownloadSpec(backend);
    const std::filesystem::path targetDir = state.localConfig.modelsDir.empty()
                                                ? DefaultEditorAiModelsDir()
                                                : std::filesystem::path(state.localConfig.modelsDir);

    state.downloadBackend = backend;
    state.downloadProgress.bytesDownloaded.store(0u, std::memory_order_relaxed);
    state.downloadProgress.bytesTotal.store(spec.expectedBytes, std::memory_order_relaxed);
    state.downloading = true;
    state.downloadCancelRequested.store(false, std::memory_order_relaxed);
    state.downloadDone.store(false, std::memory_order_relaxed);
    state.downloadStart = std::chrono::steady_clock::now();

    state.downloadThread = std::thread([&state, backend, targetDir]() {
        EditorAiDownloadResult res = DownloadEditorAiBackendModel(
            backend, targetDir, &state.downloadProgress, &state.downloadCancelRequested);
        {
            std::lock_guard<std::mutex> lock(state.downloadMutex);
            state.downloadOutput = std::move(res);
        }
        state.downloadDone.store(true, std::memory_order_release);
    });
}

void DrawMissingModelPrompt(const EditorAiDialogState &state, int tabIdx, bool &outTriggerDownload)
{
    bool missingModelOnly = false;
    std::string missingHint;
    if (state.downloading ||
        CheckTabModelAndRuntimeReady(state, tabIdx, missingModelOnly, missingHint))
    {
        return;
    }
    if (missingModelOnly)
    {
        const EditorAiBackend backend = ActiveBackendForTab(state, tabIdx);
        const EditorAiModelDownloadSpec spec = GetEditorAiModelDownloadSpec(backend);
        const double sizeMb = double(spec.expectedBytes) / (1024.0 * 1024.0);
        char btnLabel[96] = {};
        if (sizeMb >= 1024.0)
        {
            std::snprintf(btnLabel, sizeof(btnLabel), "Download Model (%.2f GB)", sizeMb / 1024.0);
        }
        else
        {
            std::snprintf(btnLabel, sizeof(btnLabel), "Download Model (%.1f MB)", sizeMb);
        }
        ImGui::BeginDisabled(state.generating);
        if (ImGui::Button(btnLabel, ImVec2(EditorGuiFitButtonWidth(btnLabel), 0.0f)))
        {
            outTriggerDownload = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.28f, 1.0f));
    ImGui::TextWrapped("%s", missingHint.c_str());
    ImGui::PopStyleColor();
}

void DrawUpscaleResolutionCombo(EditorAiDialogState &state)
{
    static constexpr const char *kUpscaleItems[] = {
        "512 px",
        "768 px (Default)",
        "1024 px",
    };
    static constexpr uint32_t kUpscaleValues[] = { 512u, 768u, 1024u };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Upscale (Max Side):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(EditorGuiComboWidth(kUpscaleItems, 3, 160.0f));
    int32_t resIdx = UpscaleIndexForResolution(state.localConfig.processingRes);
    if (ImGui::Combo("##ai_upscale", &resIdx, kUpscaleItems, 3))
    {
        state.localConfig.processingRes = kUpscaleValues[std::clamp(resIdx, 0, 2)];
        SaveLocalConfigQuietly(state);
    }

    const uint32_t srcW =
        state.leftPreviewWidth > 0 ? state.leftPreviewWidth : state.width;
    const uint32_t srcH =
        state.leftPreviewHeight > 0 ? state.leftPreviewHeight : state.height;
    uint32_t fitW = 0;
    uint32_t fitH = 0;
    if (ActiveBackendForTab(state, state.activeTab) == EditorAiBackend::LotusDepthD)
    {
        EditorAiLotusDepthDims dims;
        const bool seamless = state.tabs[size_t(state.activeTab)].normalParams.seamless;
        ComputeEditorAiLotusDepthDimensions(srcW, srcH, state.localConfig.processingRes, seamless,
                                            dims);
        const uint32_t workW = dims.fitW + 2u * dims.haloX;
        const uint32_t workH = dims.fitH + 2u * dims.haloY;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Fit: %ux%u -> %ux%u (ctx %ux%u) -> %ux%u", srcW, srcH, dims.fitW,
                            dims.fitH, workW, workH, srcW, srcH);
        return;
    }
    ComputeEditorAiUpscaleFitDimensions(srcW, srcH, state.localConfig.processingRes, fitW, fitH);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Fit: %ux%u -> %ux%u -> %ux%u", srcW, srcH, fitW, fitH, srcW, srcH);
}

void DrawModelHeaderRow(EditorAiDialogState &state, int tabIdx)
{
    const EditorAiBackend activeBackend = ActiveBackendForTab(state, tabIdx);
    const AiTabData &tab = state.tabs[size_t(tabIdx)];

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model:");
    ImGui::SameLine();
    ImGui::TextUnformatted(EditorAiBackendDisplayName(activeBackend));

    ImGui::SameLine();
    if (!tab.providerBadge.empty())
    {
        ImGui::TextDisabled("GGUF (%s)", tab.providerBadge.c_str());
    }
    else
    {
        ImGui::TextDisabled("GGUF (%s)", EditorAiBackendModelFilename(activeBackend));
    }
}

bool DrawWrappedRadioRow(const char *const *labels, int32_t count, float maxRowW, int32_t &value)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const float radioChrome = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
    const float spacingX = style.ItemSpacing.x;
    bool changed = false;
    float lineUsed = 0.0f;

    for (int32_t i = 0; i < count; ++i)
    {
        const float itemW = ImGui::CalcTextSize(labels[i], nullptr, true).x + radioChrome;
        if (i > 0 && lineUsed + spacingX + itemW <= maxRowW)
        {
            ImGui::SameLine();
            lineUsed += spacingX + itemW;
        }
        else
        {
            lineUsed = itemW;
        }
        changed |= ImGui::RadioButton(labels[i], &value, i);
    }
    return changed;
}

bool ShouldRebuildAfterSeamlessToggle(const AiTabData &tab, int tabIdx)
{
    if (!tab.hasResult)
    {
        return false;
    }
    return (tabIdx == kTabUpscale && tab.upscaleParams.sharpen > 1e-4f) ||
           tabIdx == kTabNormals ||
           (tabIdx == kTabDepth && tab.depthTarget == kDepthTargetNormals) ||
           (tabIdx == kTabDelight && tab.delightParams.detailPreserve > 1e-4f);
}

void DrawUpscaleScaleControls(const EditorAiDialogState &state, AiTabData &tab,
                              bool &needResultRebuild)
{
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scale:");
    ImGui::SameLine();
    static constexpr const char *kScaleLabels[] = { "4x (Native)", "2x" };
    int32_t scaleIdx = (tab.upscaleParams.outputScale == 2) ? 1 : 0;
    if (DrawWrappedRadioRow(kScaleLabels, 2, ImGui::GetContentRegionAvail().x, scaleIdx))
    {
        tab.upscaleParams.outputScale = (scaleIdx == 1) ? 2 : 4;
        needResultRebuild = tab.hasResult;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Output: %ux%u -> %ux%u", state.width, state.height,
                        state.width * uint32_t(tab.upscaleParams.outputScale),
                        state.height * uint32_t(tab.upscaleParams.outputScale));
}

bool ToggleUpstreamCheckbox(QrInstance instance, EditorAiDialogState &state, int tabIdx,
                            const char *label, bool &value)
{
    ImGui::SameLine();
    if (!ImGui::Checkbox(label, &value))
    {
        return false;
    }
    RefreshLeftPreviewForActiveTab(instance, state);
    if (tabIdx == kTabDepth)
    {
        state.tabs[size_t(tabIdx)].rawColorRgba.clear();
    }
    return state.tabs[size_t(tabIdx)].hasResult;
}

void DrawNormalsOrDepthInputControls(QrInstance instance, EditorAiDialogState &state, int tabIdx,
                                     bool inputChanged, bool &needResultRebuild)
{
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f * EditorGuiScale());
    const bool smoothChanged =
        ImGui::SliderFloat("Pre-smooth", &tab.preSmooth, 0.0f, 1.0f, "%.2f");
    if (smoothChanged || (inputChanged && tab.preSmooth > 1e-4f))
    {
        RefreshLeftPreviewForActiveTab(instance, state);
    }

    const bool hasUpscaled = HasReadyTabOutput(state.tabs[kTabUpscale]);
    const bool hasDelit = HasReadyTabOutput(state.tabs[kTabDelight]);
    if (hasDelit)
    {
        needResultRebuild |=
            ToggleUpstreamCheckbox(instance, state, tabIdx, "Use De-lit diffuse", tab.useDelitDiffuse);
    }
    if (hasUpscaled && (!hasDelit || !tab.useDelitDiffuse))
    {
        needResultRebuild |= ToggleUpstreamCheckbox(instance, state, tabIdx, "Use Upscaled diffuse",
                                                    tab.useUpscaledDiffuse);
    }
}

void DrawUseDelitAndPreSmoothControls(QrInstance instance, EditorAiDialogState &state, int tabIdx)
{
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    bool *seamlessPtr = &tab.normalParams.seamless;
    if (tabIdx == kTabUpscale)
    {
        seamlessPtr = &tab.upscaleParams.seamless;
    }
    else if (tabIdx == kTabDelight)
    {
        seamlessPtr = &tab.delightParams.seamless;
    }
    const bool inputChanged = ImGui::Checkbox("Seamless", seamlessPtr);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Wrap borders for tiling wall/floor textures; disable for character/weapon UV atlases.");
    }

    bool needResultRebuild = inputChanged && ShouldRebuildAfterSeamlessToggle(tab, tabIdx);
    const bool hasUpscaled = HasReadyTabOutput(state.tabs[kTabUpscale]);

    if (tabIdx == kTabUpscale)
    {
        DrawUpscaleScaleControls(state, tab, needResultRebuild);
    }
    else if (tabIdx == kTabDelight && hasUpscaled)
    {
        needResultRebuild |= ToggleUpstreamCheckbox(instance, state, tabIdx, "Use Upscaled diffuse",
                                                    tab.useUpscaledDiffuse);
    }
    else if (tabIdx == kTabNormals || tabIdx == kTabDepth)
    {
        DrawNormalsOrDepthInputControls(instance, state, tabIdx, inputChanged, needResultRebuild);
    }

    if (tabIdx == kTabDepth)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Apply to:");
        static constexpr const char *kDepthTargetLabels[] = {
            "Height / Parallax (texture_height)",
            "Normals (texture_normals)",
            "Gloss (texture_gloss)",
        };
        int32_t targetMode = tab.depthTarget;
        if (DrawWrappedRadioRow(kDepthTargetLabels, 3, ImGui::GetContentRegionAvail().x, targetMode))
        {
            tab.depthTarget = targetMode;
            tab.viewMode = (targetMode == kDepthTargetHeight) ? 0 : 1;
            needResultRebuild = tab.hasResult;
        }
    }

    if (needResultRebuild && tab.hasResult)
    {
        RebuildTabResultAndPreview(instance, state, tabIdx);
    }
}

void DrawTabConfigBar(QrInstance instance, EditorAiDialogState &state, int tabIdx,
                      bool &outTriggerDownload)
{
    if (tabIdx == kTabEmissive)
    {
        ImGui::TextDisabled("Local luminance/saturation extraction -> texture_emissive");
        return;
    }

    ImGui::BeginDisabled(state.generating || state.downloading);
    DrawModelHeaderRow(state, tabIdx);
    if (IsEditorAiDiffusionBackend(ActiveBackendForTab(state, tabIdx)))
    {
        DrawUpscaleResolutionCombo(state);
    }
    DrawUseDelitAndPreSmoothControls(instance, state, tabIdx);
    ImGui::EndDisabled();

    DrawMissingModelPrompt(state, tabIdx, outTriggerDownload);
    if (!state.configStatus.empty())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.62f, 0.30f, 1.0f), "%s", state.configStatus.c_str());
    }
}

void DrawPreviewImageBox(const char *title, QrMaterial mat, uint32_t texW, uint32_t texH,
                         float boxSize, const char *placeholder)
{
    ImGui::BeginGroup();
    const std::string shownTitle = EditorGuiEllipsize(title, boxSize);
    ImGui::TextUnformatted(shownTitle.c_str());
    if (title != nullptr && shownTitle != title && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", title);
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 boxMax(pos.x + boxSize, pos.y + boxSize);
    ImGui::Dummy(ImVec2(boxSize, boxSize));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, boxMax, IM_COL32(24, 26, 30, 255));

    if (mat != QR_NO_MATERIAL && texW > 0 && texH > 0)
    {
        const float aspect = float(texW) / float(texH);
        const float drawW = aspect >= 1.0f ? boxSize : (boxSize * aspect);
        const float drawH = aspect >= 1.0f ? (boxSize / aspect) : boxSize;
        const float ox = pos.x + 0.5f * (boxSize - drawW);
        const float oy = pos.y + 0.5f * (boxSize - drawH);
        dl->AddImage(ImTextureRef(ImTextureID(uintptr_t(mat))), ImVec2(ox, oy),
                     ImVec2(ox + drawW, oy + drawH));
    }
    else
    {
        const ImVec2 txtSz = ImGui::CalcTextSize(placeholder);
        const ImVec2 txtPos(
            pos.x + std::max(8.0f * EditorGuiScale(), 0.5f * (boxSize - txtSz.x)),
            pos.y + 0.5f * (boxSize - txtSz.y));
        dl->PushClipRect(pos, boxMax, true);
        dl->AddText(txtPos, IM_COL32(140, 146, 158, 255), placeholder);
        dl->PopClipRect();
    }
    dl->AddRect(pos, boxMax, IM_COL32(72, 82, 96, 255), 0.0f, 0, 1.0f);
    ImGui::EndGroup();
}

void DrawRightPreviewModeSelector(QrInstance instance, EditorAiDialogState &state, int tabIdx,
                                  float boxSize)
{
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    bool changed = false;
    bool showAngle = false;

    if (tabIdx == kTabUpscale)
    {
        static constexpr const char *kLabels[] = { "Upscaled", "Split Compare", "Added Detail" };
        changed |= DrawWrappedRadioRow(kLabels, 3, boxSize, tab.viewMode);
    }
    else if (tabIdx == kTabDelight)
    {
        static constexpr const char *kLabels[] = { "De-lit", "Removed Glare" };
        changed |= DrawWrappedRadioRow(kLabels, 2, boxSize, tab.viewMode);
    }
    else if (tabIdx == kTabNormals)
    {
        static constexpr const char *kLabels[] = { "Normal RGB", "Height", "Lit" };
        changed |= DrawWrappedRadioRow(kLabels, 3, boxSize, tab.viewMode);
        showAngle = (tab.viewMode == 2);
    }
    else if (tabIdx == kTabDepth)
    {
        if (tab.depthTarget == kDepthTargetHeight)
        {
            static constexpr const char *kLabels[] = { "Height", "Lit" };
            changed |= DrawWrappedRadioRow(kLabels, 2, boxSize, tab.viewMode);
            showAngle = (tab.viewMode == 1);
        }
        else if (tab.depthTarget == kDepthTargetNormals)
        {
            static constexpr const char *kLabels[] = { "Depth", "Normal RGB", "Lit" };
            changed |= DrawWrappedRadioRow(kLabels, 3, boxSize, tab.viewMode);
            showAngle = (tab.viewMode == 2);
        }
        else
        {
            static constexpr const char *kLabels[] = { "Depth", "Gloss", "Specular" };
            changed |= DrawWrappedRadioRow(kLabels, 3, boxSize, tab.viewMode);
            showAngle = (tab.viewMode == 2);
        }
    }
    else
    {
        static constexpr const char *kLabels[] = { "Emissive", "Composite" };
        changed |= DrawWrappedRadioRow(kLabels, 2, boxSize, tab.viewMode);
    }

    if (showAngle)
    {
        const float scale = EditorGuiScale();
        const float angleLabelW =
            ImGui::CalcTextSize("Light Angle").x + ImGui::GetStyle().ItemInnerSpacing.x;
        const float angleSliderW =
            std::max(80.0f * scale, std::min(220.0f * scale, boxSize - angleLabelW));
        ImGui::SetNextItemWidth(angleSliderW);
        changed |= ImGui::SliderFloat("Light Angle", &tab.lightAngleDeg, -180.0f, 180.0f,
                                      "%.0f deg");
    }

    if (changed && tab.hasResult)
    {
        RebuildTabResultAndPreview(instance, state, tabIdx);
    }
}

const char *DescribeLeftPreviewHeader(const EditorAiDialogState &state)
{
    static constexpr const char *kHeaders[8] = {
        "Diffuse",
        "Upscaled Diffuse",
        "De-lit Diffuse",
        "Upscaled De-lit Diffuse",
        "AI Input (pre-smoothed)",
        "AI Input (upscaled + smoothed)",
        "AI Input (de-lit + smoothed)",
        "AI Input (upscaled + de-lit + smoothed)",
    };
    const int idx = (state.leftPreviewUsesUpscaled ? 1 : 0) |
                    (state.leftPreviewUsesDelit ? 2 : 0) |
                    (state.leftPreviewUsesPreprocessed ? 4 : 0);
    return kHeaders[idx];
}

void DrawDualPreviewSection(QrInstance instance, EditorAiDialogState &state, int tabIdx)
{
    const float scale = EditorGuiScale();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float gap = 16.0f * scale;
    const float boxSize = std::clamp((availW - gap) * 0.5f, 180.0f * scale, 296.0f * scale);
    const AiTabData &tab = state.tabs[size_t(tabIdx)];

    char leftHeader[192] = {};
    const uint32_t leftW =
        state.leftPreviewWidth > 0u ? state.leftPreviewWidth : state.width;
    const uint32_t leftH =
        state.leftPreviewHeight > 0u ? state.leftPreviewHeight : state.height;
    std::snprintf(leftHeader, sizeof(leftHeader), "%s (%ux%u)", DescribeLeftPreviewHeader(state),
                  leftW, leftH);
    DrawPreviewImageBox(leftHeader, state.leftPreviewMat, leftW, leftH, boxSize, "No diffuse");

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginGroup();
    const bool activeGenerating = state.generating && state.workerTargetTab == tabIdx;
    const char *placeholder = activeGenerating ? "Generating..." : "Click Generate to preview";
    char rightHeader[96] = {};
    if (tab.hasResult && tab.resultWidth > 0u && tab.resultHeight > 0u)
    {
        std::snprintf(rightHeader, sizeof(rightHeader), "Generated Preview (%ux%u)",
                      tab.resultWidth, tab.resultHeight);
    }
    else
    {
        std::snprintf(rightHeader, sizeof(rightHeader), "Generated Preview");
    }
    const uint32_t rightW = tab.resultWidth > 0u ? tab.resultWidth : leftW;
    const uint32_t rightH = tab.resultHeight > 0u ? tab.resultHeight : leftH;
    DrawPreviewImageBox(rightHeader, tab.resultPreviewMat, rightW, rightH, boxSize, placeholder);
    ImGui::Spacing();
    DrawRightPreviewModeSelector(instance, state, tabIdx, boxSize);
    ImGui::EndGroup();
}

struct PairedSliderSpec
{
    const char *label;
    float *value;
    float minVal;
    float maxVal;
    const char *format;
};

bool DrawPairedSliders(const PairedSliderSpec &left, const PairedSliderSpec &right)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const float scale = EditorGuiScale();
    const float startX = ImGui::GetCursorPosX();
    const float availW = ImGui::GetContentRegionAvail().x;
    const float spacing = style.ItemSpacing.x;
    const float halfW = std::max(120.0f * scale, (availW - spacing) * 0.5f);
    const float minSliderW = 100.0f * scale;

    const auto colSliderW = [&](const char *lbl) {
        const float lw = ImGui::CalcTextSize(lbl, nullptr, true).x + style.ItemInnerSpacing.x;
        return std::max(minSliderW, halfW - lw);
    };

    bool changed = false;
    ImGui::SetNextItemWidth(colSliderW(left.label));
    changed |= ImGui::SliderFloat(left.label, left.value, left.minVal, left.maxVal, left.format);

    ImGui::SameLine(startX + halfW + spacing);
    ImGui::SetNextItemWidth(colSliderW(right.label));
    changed |=
        ImGui::SliderFloat(right.label, right.value, right.minVal, right.maxVal, right.format);
    return changed;
}

void DrawNormalAdjustmentControls(AiTabData &tab, bool &outChanged)
{
    outChanged |= ImGui::Checkbox("Invert Height", &tab.normalParams.invertHeight);
    ImGui::SameLine();
    outChanged |= ImGui::Checkbox("Invert Y", &tab.normalParams.invertY);
    const float prevMicro = tab.normalParams.microDetail;
    const PairedSliderSpec left{ "Strength", &tab.normalParams.strength, 0.2f, 3.0f, "%.2f" };
    const PairedSliderSpec right{ "Micro-detail", &tab.normalParams.microDetail, 0.0f, 0.5f,
                                  "%.2f" };
    if (DrawPairedSliders(left, right))
    {
        if (tab.normalParams.microDetail != prevMicro)
        {
            tab.microDetailEdited = true;
        }
        outChanged = true;
    }
}

void DrawInstantAdjustmentsSection(QrInstance instance, EditorAiDialogState &state, int tabIdx)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    bool changed = false;

    if (tabIdx == kTabUpscale)
    {
        changed |= DrawPairedSliders(
            { "Strength", &tab.upscaleParams.strength, 0.0f, 1.0f, "%.2f" },
            { "Sharpen", &tab.upscaleParams.sharpen, 0.0f, 1.0f, "%.2f" });
    }
    else if (tabIdx == kTabDelight)
    {
        changed |= DrawPairedSliders(
            { "Delight Strength", &tab.delightParams.strength, 0.0f, 1.0f, "%.2f" },
            { "Detail Preserve", &tab.delightParams.detailPreserve, 0.0f, 1.0f, "%.2f" });
    }
    else if (tabIdx == kTabNormals ||
             (tabIdx == kTabDepth && tab.depthTarget == kDepthTargetNormals))
    {
        DrawNormalAdjustmentControls(tab, changed);
    }
    else
    {
        if (tabIdx == kTabDepth && tab.depthTarget == kDepthTargetHeight)
        {
            changed |= ImGui::Checkbox("Invert Height", &tab.normalParams.invertHeight);
        }
        changed |= DrawPairedSliders(
            { "Contrast / Gain", &tab.toneParams.contrastGain, 0.2f, 3.0f, "%.2f" },
            { "Bias / Threshold", &tab.toneParams.biasThreshold, -0.5f, 0.8f, "%.2f" });
    }

    if (changed && tab.hasResult)
    {
        RebuildTabResultAndPreview(instance, state, tabIdx);
    }
}

void PollGenerationWorkerCompletion(QrInstance instance, EditorAiDialogState &state)
{
    if (!state.generating || !state.workerDone.load(std::memory_order_acquire))
    {
        return;
    }

    if (state.workerThread.joinable())
    {
        state.workerThread.join();
    }
    state.generating = false;
    state.workerDone.store(false, std::memory_order_relaxed);

    EditorAiInferenceResult res;
    {
        std::lock_guard<std::mutex> lock(state.workerMutex);
        res = std::move(state.workerOutput);
    }

    if (res.cancelled)
    {
        return;
    }
    if (!res.ok)
    {
        state.errorMessage =
            res.errorMessage.empty() ? "AI worker inference failed." : res.errorMessage;
        return;
    }

    const int tabIdx = std::clamp(state.workerTargetTab, 0, kTabCount - 1);
    AiTabData &tab = state.tabs[size_t(tabIdx)];
    tab.rawTexW = res.outputWidth > 0u ? res.outputWidth : state.width;
    tab.rawTexH = res.outputHeight > 0u ? res.outputHeight : state.height;
    tab.providerBadge = std::move(res.providerLabel);
    tab.rawNormalRgba = std::move(res.rawNormalRgba);
    tab.rawHeight = std::move(res.rawHeight);
    tab.rawColorRgba = std::move(res.rawColorRgba);
    tab.hasGeneratedOnce = true;
    tab.hasResult = true;
    tab.appliedRelPath.clear();
    RebuildTabResultAndPreview(instance, state, tabIdx);
}

void PollModelDownloadCompletion(EditorAiDialogState &state)
{
    if (!state.downloading || !state.downloadDone.load(std::memory_order_acquire))
    {
        return;
    }

    if (state.downloadThread.joinable())
    {
        state.downloadThread.join();
    }
    state.downloading = false;
    state.downloadDone.store(false, std::memory_order_relaxed);

    EditorAiDownloadResult res;
    {
        std::lock_guard<std::mutex> lock(state.downloadMutex);
        res = std::move(state.downloadOutput);
    }

    if (res.cancelled)
    {
        return;
    }
    if (!res.ok)
    {
        state.errorMessage = res.errorMessage.empty() ? "Model download failed." : res.errorMessage;
        return;
    }

    state.errorMessage.clear();
}

void DrawGenerationStatusAndErrors(const EditorAiDialogState &state, int tabIdx)
{
    const AiTabData &tab = state.tabs[size_t(tabIdx)];
    if (state.downloading)
    {
        const uint64_t dl = state.downloadProgress.bytesDownloaded.load(std::memory_order_relaxed);
        const uint64_t tot = state.downloadProgress.bytesTotal.load(std::memory_order_relaxed);
        const float frac =
            tot > 0u ? std::clamp(float(double(dl) / double(tot)), 0.0f, 1.0f) : 0.0f;
        const double dlMb = double(dl) / (1024.0 * 1024.0);
        const double totMb = double(tot) / (1024.0 * 1024.0);
        char overlay[160] = {};
        if (tot > 0u)
        {
            std::snprintf(overlay, sizeof(overlay), "Downloading %s: %.1f / %.1f MB (%.0f%%)",
                          EditorAiBackendModelFilename(state.downloadBackend), dlMb, totMb,
                          double(frac) * 100.0);
        }
        else
        {
            std::snprintf(overlay, sizeof(overlay), "Downloading %s...",
                          EditorAiBackendModelFilename(state.downloadBackend));
        }
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
    }
    else if (state.generating)
    {
        int32_t pct = 0;
        std::string stage;
        {
            std::lock_guard<std::mutex> lock(state.progressMutex);
            pct = state.progressPercent;
            stage = state.progressStage;
        }
        const auto elapsed = std::chrono::duration<float>(
                                 std::chrono::steady_clock::now() - state.generateStart)
                                 .count();
        const float frac = std::clamp(float(pct) / 100.0f, 0.0f, 1.0f);
        char overlay[192] = {};
        std::snprintf(overlay, sizeof(overlay), "%d%% - %s (%.1fs)", pct,
                      stage.empty() ? "Generating..." : stage.c_str(), elapsed);
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);
    }
    else if (!state.errorMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.42f, 0.36f, 1.0f));
        ImGui::TextWrapped("%s", state.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
    else if (!tab.appliedRelPath.empty())
    {
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.52f, 1.0f), "Applied override: %s",
                           tab.appliedRelPath.c_str());
    }
}

void DrawDialogFooterButtons(const EditorAiDialogState &state, int tabIdx, bool &outGenerate,
                             bool &outApply, bool &outClose)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const AiTabData &tab = state.tabs[size_t(tabIdx)];
    const char *genLabel = tab.hasGeneratedOnce ? "Regenerate" : "Generate";
    const bool busy = state.generating || state.downloading;
    const float scale = EditorGuiScale();
    const float btnH = std::max(30.0f * scale, ImGui::GetFrameHeight() + 4.0f * scale);

    ImGui::BeginDisabled(busy);
    if (ImGui::Button(genLabel, ImVec2(EditorGuiFitButtonWidth(genLabel, 116.0f), btnH)))
    {
        outGenerate = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !tab.hasResult);
    if (ImGui::Button("Apply", ImVec2(EditorGuiFitButtonWidth("Apply", 104.0f), btnH)))
    {
        outApply = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const char *closeLabel = busy ? "Cancel" : "Close";
    if (ImGui::Button(closeLabel, ImVec2(EditorGuiFitButtonWidth(closeLabel, 100.0f), btnH)))
    {
        outClose = true;
    }
}

} // namespace

bool EditorApp::IsAiTextureDialogOpen() const
{
    return m_aiDialog != nullptr && m_aiDialog->open;
}

void EditorAiDialogDeleter::operator()(EditorAiDialogState *ptr) const
{
    delete ptr;
}

bool EditorApp::CancelAiTextureGeneratorDialog()
{
    if (!IsAiTextureDialogOpen())
    {
        return false;
    }
    ShutdownAiTextureGeneratorDialog();
    return true;
}

void EditorApp::ShutdownAiTextureGeneratorDialog()
{
    if (!m_aiDialog)
    {
        return;
    }
    JoinAiWorkerIfRunning(*m_aiDialog, true);
    JoinAiDownloadIfRunning(*m_aiDialog, true);
    DestroyDialogMaterial(m_instance, m_aiDialog->leftPreviewMat);
    for (AiTabData &tab : m_aiDialog->tabs)
    {
        DestroyDialogMaterial(m_instance, tab.resultPreviewMat);
    }
    m_aiDialog->open = false;
    m_aiDialog.reset();
}

namespace
{
bool IsSupportedAiSlot(EditorApp::EditorTextureSlot slot)
{
    return slot == EditorApp::EditorTextureSlot::Base ||
           slot == EditorApp::EditorTextureSlot::Normals ||
           slot == EditorApp::EditorTextureSlot::Height ||
           slot == EditorApp::EditorTextureSlot::Gloss ||
           slot == EditorApp::EditorTextureSlot::Emissive;
}

void ConfigureInitialDialogTabs(EditorApp::EditorTextureSlot slot, EditorAiDialogState &state)
{
    state.tabs[kTabNormals].viewMode = 2; // Lit Preview
    state.tabs[kTabNormals].normalParams.microDetail = 0.15f;
    state.tabs[kTabDepth].depthTarget =
        (slot == EditorApp::EditorTextureSlot::Gloss) ? kDepthTargetGloss : kDepthTargetHeight;
    state.tabs[kTabDepth].viewMode = (slot == EditorApp::EditorTextureSlot::Gloss) ? 1 : 0;

    if (slot == EditorApp::EditorTextureSlot::Base)
    {
        state.activeTab = kTabUpscale;
    }
    else if (slot == EditorApp::EditorTextureSlot::Normals)
    {
        state.activeTab = kTabNormals;
    }
    else if (slot == EditorApp::EditorTextureSlot::Height ||
             slot == EditorApp::EditorTextureSlot::Gloss)
    {
        state.activeTab = kTabDepth;
    }
    else
    {
        state.activeTab = kTabEmissive;
    }
    state.pendingSelectTab = state.emissiveOnly ? -1 : state.activeTab;
}

void DrawAiGeneratorTabs(QrInstance instance, EditorAiDialogState &state)
{
    if (state.emissiveOnly)
    {
        return;
    }
    static constexpr std::array<const char *, 4> kTabLabels = {
        "Upscale",
        "De-lighting",
        "Normals",
        "Depth",
    };
    const bool busy = state.generating || state.downloading;
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginTabBar("##ai_pbr_tabs"))
    {
        for (int tabIdx = 0; tabIdx < 4; tabIdx++)
        {
            ImGuiTabItemFlags flags =
                (state.pendingSelectTab == tabIdx) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(kTabLabels[size_t(tabIdx)], nullptr, flags))
            {
                if (state.activeTab != tabIdx)
                {
                    state.activeTab = tabIdx;
                    RefreshLeftPreviewForActiveTab(instance, state);
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
        state.pendingSelectTab = -1;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
}
} // namespace

void EditorApp::OpenAiTextureGeneratorDialog(int32_t materialIndex, EditorTextureSlot slot)
{
    if (materialIndex < 0 || size_t(materialIndex) >= m_materials.Count() ||
        !IsSupportedAiSlot(slot))
    {
        return;
    }

    const std::string &matName = m_materials.NameAt(size_t(materialIndex));
    const EditorMaterialParams &params = m_materials.At(size_t(materialIndex));
    int32_t diffuseIdx = ResolveOrLoadTexture(params.baseTexture);
    if (diffuseIdx < 0)
    {
        diffuseIdx = ResolveOrLoadTexture(matName);
    }
    if (diffuseIdx < 0 || size_t(diffuseIdx) >= m_scene.textures.size() ||
        m_scene.textures[size_t(diffuseIdx)].rgba.empty())
    {
        Notify("No diffuse texture loaded for " + matName);
        return;
    }

    ShutdownAiTextureGeneratorDialog();
    m_aiDialog.reset(new EditorAiDialogState());
    EditorAiDialogState &state = *m_aiDialog;
    state.open = true;
    state.emissiveOnly = (slot == EditorTextureSlot::Emissive);
    state.materialIndex = materialIndex;
    state.materialName = matName;
    state.diffuseName = m_scene.textures[size_t(diffuseIdx)].name;
    state.width = m_scene.textures[size_t(diffuseIdx)].width;
    state.height = m_scene.textures[size_t(diffuseIdx)].height;
    state.leftPreviewWidth = state.width;
    state.leftPreviewHeight = state.height;
    state.diffuseRgba = m_scene.textures[size_t(diffuseIdx)].rgba;

    std::string cfgErr;
    if (!LoadEditorAiLocalConfig(DefaultEditorAiLocalConfigPath(), state.localConfig, cfgErr))
    {
        state.configStatus = cfgErr;
    }

    ConfigureInitialDialogTabs(slot, state);
    RefreshLeftPreviewForActiveTab(m_instance, state);
}

void EditorApp::ApplyGeneratedAiTextureToMaterial(EditorAiDialogState &state)
{
    AiTabData &tab = state.tabs[size_t(state.activeTab)];
    const EditorAiMapKind outKind = TargetMapKindForTab(state, state.activeTab);
    const EditorTextureSlot outSlot = TargetSlotForTab(state, state.activeTab);
    const std::string outRoot =
        ResolveAiOutputRootDir(m_gamePath, m_sessionPaths.materials, m_scenePath);
    const std::string relPath = BuildUniqueGeneratedTextureRelativePath(
        outRoot, state.materialName, outKind, [this](const std::string &candidate) {
            return TextureIndexByName(candidate) >= 0 || m_assets.Exists(candidate);
        });
    std::filesystem::path diskPath;
    if (!ResolveAndValidateUniqueAiDiskPath(outRoot, relPath, diskPath, state.errorMessage))
    {
        return;
    }

    const uint32_t outW = tab.resultWidth > 0u ? tab.resultWidth : state.width;
    const uint32_t outH = tab.resultHeight > 0u ? tab.resultHeight : state.height;
    if (!WriteEditorRgbaTga(diskPath, outW, outH, tab.finalRgba))
    {
        state.errorMessage = "Failed writing TGA output to " + diskPath.string();
        return;
    }
    m_assets.AddDirectory(outRoot);
    RegisterOrUpdateGeneratedTexture(relPath, outW, outH, tab.finalRgba);

    if (state.materialIndex >= 0 && size_t(state.materialIndex) < m_materials.Count())
    {
        AssignMaterialSlotTexture(m_materials.At(size_t(state.materialIndex)), outSlot, relPath);
        MarkMaterialEdited(state.materialIndex);
    }
    PrintMessage(("ai texture applied: " + state.materialName + " -> " + relPath).c_str(), this);
    Notify("AI generated " + relPath);
    tab.appliedRelPath = relPath;
    if (state.emissiveOnly)
    {
        ShutdownAiTextureGeneratorDialog();
    }
}

void EditorApp::DrawAiTextureGeneratorDialog()
{
    if (!IsAiTextureDialogOpen())
    {
        return;
    }
    EditorAiDialogState &state = *m_aiDialog;
    PollModelDownloadCompletion(state);
    PollGenerationWorkerCompletion(m_instance, state);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    const float targetW = std::min(vp->Size.x - 32.0f * scale, 760.0f * scale);
    EditorGuiBackdrop(vp->Size.x, vp->Size.y, 0.58f);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(targetW, 0.0f), ImGuiCond_Appearing);

    bool windowOpen = true;
    bool triggerGen = false;
    bool triggerDownload = false;
    bool triggerApply = false;
    bool triggerClose = false;

    if (ImGui::Begin("AI Texture Generator", &windowOpen,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        const EditorTextureSlot curSlot = TargetSlotForTab(state, state.activeTab);
        ImGui::TextDisabled("Material: %s  |  Target: %s", state.materialName.c_str(),
                            SlotLabelForAi(curSlot));
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        DrawAiGeneratorTabs(m_instance, state);
        DrawTabConfigBar(m_instance, state, state.activeTab, triggerDownload);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        DrawDualPreviewSection(m_instance, state, state.activeTab);
        DrawInstantAdjustmentsSection(m_instance, state, state.activeTab);
        DrawGenerationStatusAndErrors(state, state.activeTab);
        DrawDialogFooterButtons(state, state.activeTab, triggerGen, triggerApply, triggerClose);
    }
    ImGui::End();

    if (!windowOpen || triggerClose)
    {
        ShutdownAiTextureGeneratorDialog();
    }
    else if (triggerDownload)
    {
        LaunchModelDownloadWorker(state);
    }
    else if (triggerGen)
    {
        LaunchGenerationWorker(state);
    }
    else if (triggerApply && state.tabs[size_t(state.activeTab)].hasResult)
    {
        ApplyGeneratedAiTextureToMaterial(state);
    }
}
