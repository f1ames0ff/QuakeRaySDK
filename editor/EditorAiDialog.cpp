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
#include "EditorApp.h"
#include "EditorGui.h"
#include "EditorOnnxRuntime.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct EditorAiDialogState
{
    bool open = false;
    int32_t materialIndex = -1;
    EditorApp::EditorTextureSlot slot = EditorApp::EditorTextureSlot::Normals;
    EditorAiMapKind mapKind = EditorAiMapKind::Normals;
    std::string materialName;
    std::string diffuseName;

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> diffuseRgba;
    QrMaterial diffusePreviewMat = QR_NO_MATERIAL;

    EditorAiLocalConfig localConfig;
    std::string configStatus;

    EditorAiNormalParams normalParams;
    bool microDetailEdited = false;
    EditorAiToneParams toneParams;
    int32_t viewMode = 2;
    float lightAngleDeg = -125.0f;

    bool hasGeneratedOnce = false;
    bool hasResult = false;
    std::vector<uint8_t> rawNormalRgba;
    std::vector<float> rawHeight;
    std::vector<uint8_t> rawColorRgba;
    std::vector<uint8_t> finalRgba;
    std::vector<uint8_t> displayPreviewRgba;
    QrMaterial resultPreviewMat = QR_NO_MATERIAL;

    bool generating = false;
    std::atomic<bool> cancelRequested{ false };
    std::atomic<bool> workerDone{ false };
    std::thread workerThread;
    std::chrono::steady_clock::time_point generateStart;
    std::string errorMessage;
    std::string activeModelLabel;
    std::string providerBadge;

    bool downloading = false;
    std::atomic<bool> downloadCancelRequested{ false };
    std::atomic<bool> downloadDone{ false };
    std::thread downloadThread;
    std::chrono::steady_clock::time_point downloadStart;
    EditorAiDownloadProgress downloadProgress;
    EditorAiBackend downloadBackend = EditorAiBackend::LotusNormalD;

    std::mutex workerMutex;
    EditorAiInferenceResult workerOutput;
    std::mutex downloadMutex;
    EditorAiDownloadResult downloadOutput;
};

namespace
{

EditorAiMapKind MapKindForSlot(EditorApp::EditorTextureSlot slot, bool &outValid)
{
    outValid = true;
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Normals:
        return EditorAiMapKind::Normals;
    case EditorApp::EditorTextureSlot::Gloss:
        return EditorAiMapKind::Gloss;
    case EditorApp::EditorTextureSlot::Emissive:
        return EditorAiMapKind::Emissive;
    default:
        outValid = false;
        return EditorAiMapKind::Normals;
    }
}

const char *SlotLabelForAi(EditorApp::EditorTextureSlot slot)
{
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Normals:
        return "texture_normals";
    case EditorApp::EditorTextureSlot::Gloss:
        return "texture_gloss";
    case EditorApp::EditorTextureSlot::Emissive:
        return "texture_emissive";
    default:
        return "texture";
    }
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

void RebuildProcessedResultAndPreview(QrInstance instance, EditorAiDialogState &state)
{
    if (!state.hasResult || state.width == 0 || state.height == 0)
    {
        return;
    }

    if (state.mapKind == EditorAiMapKind::Normals)
    {
        if (!state.rawNormalRgba.empty())
        {
            AdjustEditorNormalMap(state.rawNormalRgba, state.width, state.height,
                                  &state.diffuseRgba, state.normalParams, state.finalRgba);
        }
        else
        {
            BuildEditorNormalMapFromHeight(state.rawHeight, state.width, state.height,
                                           &state.diffuseRgba, state.normalParams, state.finalRgba);
        }

        if (state.viewMode == 1 && !state.rawHeight.empty())
        {
            BuildEditorHeightPreviewRgba(state.rawHeight, state.width, state.height,
                                         state.normalParams.invertHeight,
                                         state.displayPreviewRgba);
        }
        else if (state.viewMode == 2)
        {
            BuildEditorNormalLitPreviewRgba(state.diffuseRgba, state.finalRgba, state.width,
                                            state.height, state.lightAngleDeg,
                                            state.displayPreviewRgba);
        }
        else
        {
            state.displayPreviewRgba = state.finalRgba;
        }
    }
    else if (state.mapKind == EditorAiMapKind::Gloss)
    {
        ApplyEditorGlossAdjustments(state.rawColorRgba, state.width, state.height, state.toneParams,
                                    state.finalRgba);
        if (state.viewMode == 1)
        {
            BuildEditorGlossSpecularPreviewRgba(state.diffuseRgba, state.finalRgba, state.width,
                                                state.height, state.lightAngleDeg,
                                                state.displayPreviewRgba);
        }
        else
        {
            state.displayPreviewRgba = state.finalRgba;
        }
    }
    else
    {
        ApplyEditorEmissiveAdjustments(state.rawColorRgba, state.width, state.height,
                                       state.toneParams, state.finalRgba);
        if (state.viewMode == 1)
        {
            BuildEditorEmissiveCompositePreviewRgba(state.diffuseRgba, state.finalRgba, state.width,
                                                    state.height, state.displayPreviewRgba);
        }
        else
        {
            state.displayPreviewRgba = state.finalRgba;
        }
    }

    DestroyDialogMaterial(instance, state.resultPreviewMat);
    state.resultPreviewMat = CreatePreviewMaterialFromRgba(instance, state.width, state.height,
                                                           state.displayPreviewRgba);
}

void JoinAiWorkerIfRunning(EditorAiDialogState &state, bool cancelIfRunning)
{
    if (cancelIfRunning)
    {
        state.cancelRequested.store(true, std::memory_order_relaxed);
        CancelActiveEditorOnnxRun();
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
    case EditorApp::EditorTextureSlot::Normals:
        params.normalTexture = relPath;
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

bool CheckModelAndRuntimeReady(const EditorAiDialogState &state, bool &outMissingModelOnly,
                                 std::string &outMissingHint)
{
    outMissingModelOnly = false;
    outMissingHint.clear();
    if (state.mapKind == EditorAiMapKind::Emissive)
    {
        return true;
    }
    const std::string exeDir = EditorExecutableDirectory();
    if (!IsEditorOnnxRuntimeAvailable(exeDir))
    {
        outMissingHint = "ONNX Runtime library missing next to quakerayeditor or in "
                         "~/.cache/quakeray/onnx/lib";
        return false;
    }
    if (ResolveEditorAiModelPath(state.localConfig.activeBackend, exeDir,
                                 state.localConfig.modelsDir)
            .empty())
    {
        outMissingModelOnly = true;
        outMissingHint = std::string("Model missing: ") +
                         (DefaultEditorAiModelsDir() /
                          EditorAiBackendModelFilename(state.localConfig.activeBackend))
                             .string();
        return false;
    }
    return true;
}

bool CanAutoRunActiveBackend(const EditorAiDialogState &state, std::string &outMissingHint)
{
    bool missingModelOnly = false;
    return CheckModelAndRuntimeReady(state, missingModelOnly, outMissingHint);
}

void LaunchGenerationWorker(EditorAiDialogState &state)
{
    JoinAiWorkerIfRunning(state, true);
    JoinAiDownloadIfRunning(state, true);
    state.errorMessage.clear();

    EditorAiInferenceRequest req;
    req.backend = state.localConfig.activeBackend;
    req.mapKind = state.mapKind;
    req.width = state.width;
    req.height = state.height;
    req.processingRes = std::max<uint32_t>(512u, state.localConfig.processingRes);
    req.seamless = state.normalParams.seamless;
    req.modelsDir = state.localConfig.modelsDir;
    req.exeDir = EditorExecutableDirectory();
    req.diffuseRgba = state.diffuseRgba;

    state.activeModelLabel = EditorAiBackendDisplayName(req.backend);
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

    const EditorAiBackend backend = state.localConfig.activeBackend;
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

void DrawMissingModelPrompt(const EditorAiDialogState &state, bool &outTriggerDownload)
{
    bool missingModelOnly = false;
    std::string missingHint;
    if (state.downloading || CheckModelAndRuntimeReady(state, missingModelOnly, missingHint))
    {
        return;
    }
    if (missingModelOnly)
    {
        const EditorAiModelDownloadSpec spec =
            GetEditorAiModelDownloadSpec(state.localConfig.activeBackend);
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
        if (ImGui::Button(btnLabel))
        {
            outTriggerDownload = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.28f, 1.0f), "%s", missingHint.c_str());
}

void DrawLocalBackendBar(EditorAiDialogState &state, bool &outTriggerGen, bool &outTriggerDownload)
{
    static constexpr const char *kBackendItems[] = {
        "Lotus Normal (ONNX)",
        "Lotus Depth (ONNX)",
        "DeepBump (Fast ONNX)",
    };
    static constexpr const char *kUpscaleItems[] = {
        "512 px",
        "768 px (Default)",
        "1024 px",
    };
    static constexpr uint32_t kUpscaleValues[] = { 512u, 768u, 1024u };

    ImGui::BeginDisabled(state.generating || state.downloading);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f * EditorGuiScale());
    int32_t selectedBackend = int32_t(state.localConfig.activeBackend);
    if (ImGui::Combo("##ai_backend", &selectedBackend, kBackendItems, 3))
    {
        state.localConfig.activeBackend = EditorAiBackend(std::clamp(selectedBackend, 0, 2));
        if (!state.microDetailEdited)
        {
            state.normalParams.microDetail =
                IsEditorAiLotusBackend(state.localConfig.activeBackend) ? 0.0f : 0.15f;
        }
        SaveLocalConfigQuietly(state);
        std::string hint;
        if (CanAutoRunActiveBackend(state, hint))
        {
            outTriggerGen = true;
        }
    }

    ImGui::SameLine();
    if (!state.providerBadge.empty())
    {
        ImGui::TextDisabled("C++ ONNX (%s)", state.providerBadge.c_str());
    }
    else
    {
        ImGui::TextDisabled("C++ ONNX (%s)",
                            EditorAiBackendModelFilename(state.localConfig.activeBackend));
    }

    if (IsEditorAiLotusBackend(state.localConfig.activeBackend))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Upscale (Max Side):");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f * EditorGuiScale());
        int32_t resIdx = UpscaleIndexForResolution(state.localConfig.processingRes);
        if (ImGui::Combo("##ai_upscale", &resIdx, kUpscaleItems, 3))
        {
            state.localConfig.processingRes = kUpscaleValues[std::clamp(resIdx, 0, 2)];
            SaveLocalConfigQuietly(state);
            std::string hint;
            if (CanAutoRunActiveBackend(state, hint))
            {
                outTriggerGen = true;
            }
        }

        uint32_t fitW = 0;
        uint32_t fitH = 0;
        ComputeEditorAiUpscaleFitDimensions(state.width, state.height,
                                            state.localConfig.processingRes, fitW, fitH);
        ImGui::SameLine();
        ImGui::TextDisabled("Fit: %ux%u -> %ux%u -> %ux%u", state.width, state.height, fitW, fitH,
                            state.width, state.height);
    }
    ImGui::EndDisabled();

    DrawMissingModelPrompt(state, outTriggerDownload);
    if (!state.configStatus.empty())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.62f, 0.30f, 1.0f), "%s", state.configStatus.c_str());
    }
}

void DrawPreviewImageBox(const char *title, QrMaterial mat, uint32_t texW, uint32_t texH,
                         float boxSize, const char *placeholder)
{
    ImGui::BeginGroup();
    ImGui::TextUnformatted(title);
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
        dl->AddText(ImVec2(pos.x + 12.0f, pos.y + boxSize * 0.45f), IM_COL32(140, 146, 158, 255),
                    placeholder);
    }
    dl->AddRect(pos, boxMax, IM_COL32(72, 82, 96, 255), 0.0f, 0, 1.0f);
    ImGui::EndGroup();
}

void DrawRightPreviewModeSelector(QrInstance instance, EditorAiDialogState &state)
{
    bool changed = false;
    if (state.mapKind == EditorAiMapKind::Normals)
    {
        changed |= ImGui::RadioButton("Normal RGB", &state.viewMode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Height Map", &state.viewMode, 1);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Lit Preview", &state.viewMode, 2);
        if (state.viewMode == 2)
        {
            ImGui::SetNextItemWidth(220.0f * EditorGuiScale());
            changed |= ImGui::SliderFloat("Light Angle", &state.lightAngleDeg, -180.0f, 180.0f,
                                          "%.0f deg");
        }
    }
    else if (state.mapKind == EditorAiMapKind::Gloss)
    {
        changed |= ImGui::RadioButton("Gloss", &state.viewMode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Specular Preview", &state.viewMode, 1);
        if (state.viewMode == 1)
        {
            ImGui::SetNextItemWidth(220.0f * EditorGuiScale());
            changed |= ImGui::SliderFloat("Light Angle", &state.lightAngleDeg, -180.0f, 180.0f,
                                          "%.0f deg");
        }
    }
    else
    {
        changed |= ImGui::RadioButton("Emissive", &state.viewMode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Composite on Diffuse", &state.viewMode, 1);
    }

    if (changed && state.hasResult)
    {
        RebuildProcessedResultAndPreview(instance, state);
    }
}

void DrawDualPreviewSection(QrInstance instance, EditorAiDialogState &state)
{
    const float availW = ImGui::GetContentRegionAvail().x;
    const float boxSize = std::clamp((availW - 24.0f) * 0.5f, 170.0f, 272.0f * EditorGuiScale());

    char leftHeader[192] = {};
    std::snprintf(leftHeader, sizeof(leftHeader), "Diffuse (%ux%u) - %s", state.width, state.height,
                  SlotLabelForAi(state.slot));
    DrawPreviewImageBox(leftHeader, state.diffusePreviewMat, state.width, state.height, boxSize,
                        "No diffuse");

    ImGui::SameLine(0.0f, 18.0f);
    ImGui::BeginGroup();
    const char *placeholder = state.generating ? "Running ONNX..." : "Click Generate to preview";
    DrawPreviewImageBox("Generated Preview", state.resultPreviewMat, state.width, state.height,
                        boxSize, placeholder);
    DrawRightPreviewModeSelector(instance, state);
    ImGui::EndGroup();
}

void DrawInstantAdjustmentsSection(QrInstance instance, EditorAiDialogState &state)
{
    ImGui::Separator();
    bool changed = false;
    const float sliderW = 210.0f * EditorGuiScale();
    if (state.mapKind == EditorAiMapKind::Normals)
    {
        changed |= ImGui::Checkbox("Invert Height", &state.normalParams.invertHeight);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Invert Y", &state.normalParams.invertY);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Seamless", &state.normalParams.seamless);
        ImGui::SetNextItemWidth(sliderW);
        changed |= ImGui::SliderFloat("Strength", &state.normalParams.strength, 0.2f, 3.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderW);
        if (ImGui::SliderFloat("Micro-detail", &state.normalParams.microDetail, 0.0f, 0.5f, "%.2f"))
        {
            state.microDetailEdited = true;
            changed = true;
        }
    }
    else
    {
        ImGui::SetNextItemWidth(sliderW);
        changed |= ImGui::SliderFloat("Contrast / Gain", &state.toneParams.contrastGain, 0.2f,
                                      3.0f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sliderW);
        changed |= ImGui::SliderFloat("Bias / Threshold", &state.toneParams.biasThreshold, -0.5f,
                                      0.8f, "%.2f");
    }

    if (changed && state.hasResult)
    {
        RebuildProcessedResultAndPreview(instance, state);
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
            res.errorMessage.empty() ? "ONNX inference failed." : res.errorMessage;
        return;
    }

    state.providerBadge = std::move(res.providerLabel);
    state.rawNormalRgba = std::move(res.rawNormalRgba);
    state.rawHeight = std::move(res.rawHeight);
    state.rawColorRgba = std::move(res.rawColorRgba);
    state.hasGeneratedOnce = true;
    state.hasResult = true;
    RebuildProcessedResultAndPreview(instance, state);
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
    if (state.localConfig.activeBackend == state.downloadBackend)
    {
        LaunchGenerationWorker(state);
    }
}

void DrawGenerationStatusAndErrors(const EditorAiDialogState &state)
{
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
        const auto elapsed = std::chrono::duration<float>(
                                 std::chrono::steady_clock::now() - state.generateStart)
                                 .count();
        ImGui::TextColored(ImVec4(0.45f, 0.78f, 1.0f, 1.0f),
                           "Running %s natively in C++ ONNX... (%.1fs)",
                           state.activeModelLabel.c_str(), elapsed);
    }
    else if (!state.errorMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.42f, 0.36f, 1.0f));
        ImGui::TextWrapped("%s", state.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
}

void DrawDialogFooterButtons(const EditorAiDialogState &state, bool &outGenerate, bool &outApply,
                             bool &outCancel)
{
    ImGui::Separator();
    const char *genLabel = state.hasGeneratedOnce ? "Regenerate" : "Generate";
    const bool busy = state.generating || state.downloading;

    ImGui::BeginDisabled(busy);
    if (ImGui::Button(genLabel, ImVec2(120.0f, 30.0f)))
    {
        outGenerate = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !state.hasResult);
    if (ImGui::Button("Apply", ImVec2(110.0f, 30.0f)))
    {
        outApply = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f, 30.0f)))
    {
        outCancel = true;
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
    DestroyDialogMaterial(m_instance, m_aiDialog->diffusePreviewMat);
    DestroyDialogMaterial(m_instance, m_aiDialog->resultPreviewMat);
    m_aiDialog->open = false;
    m_aiDialog.reset();
}

void EditorApp::OpenAiTextureGeneratorDialog(int32_t materialIndex, EditorTextureSlot slot)
{
    bool validSlot = false;
    const EditorAiMapKind kind = MapKindForSlot(slot, validSlot);
    if (!validSlot || materialIndex < 0 || size_t(materialIndex) >= m_materials.Count())
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
    state.materialIndex = materialIndex;
    state.slot = slot;
    state.mapKind = kind;
    state.materialName = matName;
    state.diffuseName = m_scene.textures[size_t(diffuseIdx)].name;
    state.width = m_scene.textures[size_t(diffuseIdx)].width;
    state.height = m_scene.textures[size_t(diffuseIdx)].height;
    state.diffuseRgba = m_scene.textures[size_t(diffuseIdx)].rgba;
    state.diffusePreviewMat =
        CreatePreviewMaterialFromRgba(m_instance, state.width, state.height, state.diffuseRgba);
    state.viewMode = (kind == EditorAiMapKind::Normals) ? 2 : 0;

    std::string cfgErr;
    if (!LoadEditorAiLocalConfig(DefaultEditorAiLocalConfigPath(), state.localConfig, cfgErr))
    {
        state.configStatus = cfgErr;
    }
    state.normalParams.microDetail =
        IsEditorAiLotusBackend(state.localConfig.activeBackend) ? 0.0f : 0.15f;

    std::string missingHint;
    if (CanAutoRunActiveBackend(state, missingHint))
    {
        LaunchGenerationWorker(state);
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
    EditorGuiBackdrop(vp->Size.x, vp->Size.y, 0.58f);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f * EditorGuiScale(), 0.0f), ImGuiCond_Appearing);

    bool windowOpen = true;
    bool triggerGen = false;
    bool triggerDownload = false;
    bool triggerApply = false;
    bool triggerCancel = false;
    if (ImGui::Begin("AI Texture Generator", &windowOpen,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("Material: %s  |  Slot: %s", state.materialName.c_str(),
                            SlotLabelForAi(state.slot));
        ImGui::Separator();
        DrawLocalBackendBar(state, triggerGen, triggerDownload);
        ImGui::Separator();
        DrawDualPreviewSection(m_instance, state);
        DrawInstantAdjustmentsSection(m_instance, state);
        DrawGenerationStatusAndErrors(state);
        DrawDialogFooterButtons(state, triggerGen, triggerApply, triggerCancel);
    }
    ImGui::End();

    if (!windowOpen || triggerCancel)
    {
        ShutdownAiTextureGeneratorDialog();
        return;
    }
    if (triggerDownload)
    {
        LaunchModelDownloadWorker(state);
        return;
    }
    if (triggerGen)
    {
        LaunchGenerationWorker(state);
        return;
    }
    if (triggerApply && state.hasResult)
    {
        const std::string relPath =
            BuildGeneratedTextureRelativePath(state.materialName, state.mapKind);
        const std::string outRoot =
            ResolveAiOutputRootDir(m_gamePath, m_sessionPaths.materials, m_scenePath);
        const std::filesystem::path diskPath = std::filesystem::path(outRoot) / relPath;

        if (!WriteEditorRgbaTga(diskPath, state.width, state.height, state.finalRgba))
        {
            state.errorMessage = "Failed writing TGA output to " + diskPath.string();
            return;
        }
        m_assets.AddDirectory(outRoot);
        RegisterOrUpdateGeneratedTexture(relPath, state.width, state.height, state.finalRgba);

        if (state.materialIndex >= 0 && size_t(state.materialIndex) < m_materials.Count())
        {
            AssignMaterialSlotTexture(m_materials.At(size_t(state.materialIndex)), state.slot,
                                      relPath);
            MarkMaterialEdited(state.materialIndex);
        }
        PrintMessage(("ai texture applied: " + state.materialName + " -> " + relPath).c_str(),
                     this);
        Notify("AI generated " + relPath);
        ShutdownAiTextureGeneratorDialog();
    }
}
