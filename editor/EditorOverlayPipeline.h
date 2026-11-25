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

#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
#include "EditorMaterial.h"
#include "Scene.h"

struct EditorWaveDeformParams
{
    EditorWaveType wave = EditorWaveType::None;
    float spread = 0.0f;
    float base = 0.0f;
    float amplitude = 0.0f;
    float phase = 0.0f;
    float frequency = 0.0f;
};

struct EditorSurfaceEmissionUploadMode
{
    bool suppressBaseSurfaceEmission = false;
    bool emitRayOnly = false;
};

struct EditorOverlayUploadInput
{
    bool cutoutUnderlay = false;
    bool tracedBase = false;
    bool metallicEnvBase = false;
    bool suppressEnvironmentStages = false;
    bool promotedOverlayToTraced = false;
    bool isDtalLight = false;
    bool hasAnimatedEmissiveStage = false;
    bool twoSided = false;
    float surfaceLight = 0.0f;
    float colorScale = 1.0f;
    float alphaScale = 1.0f;
    float emissiveScale = 1.0f;
    int32_t explicitEmissiveStage = -1;
    uint32_t disabledStagesMask = 0;
    size_t overlaySurfaceIndex = (std::numeric_limits<size_t>::max)();
    std::string defaultEmissiveTexture;
    std::string emissiveTextureOverride;
    size_t baseStageIndex = (std::numeric_limits<size_t>::max)();
    size_t firstStage = 0u;
    size_t endStage = 0u;
    const std::vector<EditorOverlayStage> *stages = nullptr;
    const std::vector<QrVertex> *vertices = nullptr;
    const std::vector<uint32_t> *indices = nullptr;
};

struct EditorOverlayMaterialRequest
{
    std::string imageName;
    bool clampMap = false;
    bool cleanAdditiveBlack = false;
    bool addScreenEmission = false;
    const std::vector<const std::vector<EditorTexture> *> *extraTextureLists = nullptr;
};

struct EditorOverlaySceneUploadOptions
{
    const EditorScene *scene = nullptr;
    const EditorMaterialTable *materials = nullptr;
    const EditorAssetSource *assets = nullptr;
    const EditorCameraBasis *camera = nullptr;
    bool truelightEnabled = false;
    bool emissiveNee = false;
    float timeSeconds = 0.0f;
    const std::vector<const std::vector<EditorTexture> *> *extraTextureLists = nullptr;
};

class EditorOverlayCache
{
public:
    void Destroy(QrInstance instance);
    void ResetHandlesOnly();

    QrMaterial GetCutoutBackingMaterial(QrInstance instance);
    QrMaterial GetOverlayStageMaterial(QrInstance instance, const EditorAssetSource &assets,
                                       const EditorScene &scene,
                                       const EditorOverlayMaterialRequest &request);

private:
    QrMaterial m_cutoutBackingMaterial = QR_NO_MATERIAL;
    std::unordered_map<std::string, QrMaterial> m_overlayMaterials;
};

bool LoadEditorExternalImageRgba(const EditorAssetSource &assets, const std::string &key,
                                 uint32_t &outWidth, uint32_t &outHeight,
                                 std::vector<uint8_t> &outRgba);

bool IsEditorShaderStageAdditive(const EditorOverlayStage &stage);
bool IsEditorTextureStageAnimated(const EditorOverlayStage &stage);
bool EditorTextureHasAnimatedEmissiveStage(const EditorTexture &tex, uint32_t disabledStagesMask);
const EditorTexture *FindEditorSceneTextureByName(const EditorScene &scene,
                                                 const std::string &shaderName);
const EditorTexture *FindEditorSurfaceTexture(const EditorScene &scene,
                                              const EditorSurface &surface);
bool EditorTextureHasAutoEmissiveSource(const EditorTexture *tex,
                                        const EditorMaterialParams &params);
bool EditorSurfaceHasAutoEmissiveSource(const EditorScene &scene, const EditorSurface &surface,
                                        const EditorMaterialParams &params);
int32_t FindEditorMatchingStageIndexInTexture(const EditorTexture *tex, const std::string &texRef);
bool EditorSurfaceEmissiveBelongsToOverlayStage(const EditorScene &scene,
                                                const EditorSurface &surface,
                                                const EditorMaterialParams &params);
float ComputeEditorSurfaceEmissiveNormalOffset(const EditorScene &scene,
                                               const EditorSurface &surface,
                                               const EditorMaterialParams &params);
bool EditorSurfaceUsesDtalLighting(float truelight, const EditorMaterialParams &params);
EditorSurfaceEmissionUploadMode ResolveEditorSurfaceEmissionUploadMode(
    const EditorScene &scene, const EditorSurface &surface, const EditorMaterialParams &params,
    bool truelightEnabled, bool emissiveNee);

void DeformEditorWaveVertices(std::vector<QrVertex> &verts, const EditorWaveDeformParams &deform,
                              float timeSeconds);
void DeformEditorAutoSpriteVertices(std::vector<QrVertex> &verts, std::vector<uint32_t> &indices,
                                    bool autoSprite, bool autoSprite2,
                                    const EditorCameraBasis &cam);

void UploadEditorOverlayStageSet(
    QrInstance instance, const EditorOverlayUploadInput &input, const EditorCameraBasis &cam,
    float timeSeconds,
    const std::function<QrMaterial(const std::string &, bool, bool, bool)> &resolveMat);

void UploadEditorOverlaySurfaces(QrInstance instance,
                                 const EditorOverlaySceneUploadOptions &options,
                                 EditorOverlayCache &cache);
