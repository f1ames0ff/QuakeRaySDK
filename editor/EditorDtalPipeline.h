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
#include <string>
#include <vector>

#include <qray/qray.h>

#include "EditorMaterial.h"
#include "EditorSession.h"
#include "EditorSettings.h"
#include "Scene.h"

// RT_LIGHT_Emit numbers map lights from UINT16_MAX upwards and keeps world geometry IDs (1..N)
// out of the way. Custom lights start above map lights; emissive area lights start above torch.
inline constexpr uint64_t kEditorFirstLightUniqueID = uint64_t(UINT16_MAX);
inline constexpr uint64_t kEditorFirstCustomLightUniqueID = kEditorFirstLightUniqueID + 0x10000;
inline constexpr uint64_t kEditorTorchUniqueID = kEditorFirstCustomLightUniqueID + 0x10000;
inline constexpr uint64_t kEditorPreviewLightUniqueID = kEditorFirstCustomLightUniqueID + 0x10001;
inline constexpr uint64_t kEditorFirstEmissiveLightUniqueID =
    kEditorFirstCustomLightUniqueID + 0x20000;

inline constexpr float kEditorLightReachUnits = 1000.0f;
inline constexpr float kEditorDynamicLightReachUnits = 400.0f;
inline constexpr float kEditorLightTopUpReachUnits = 1000.0f;

// RT_EMIS_INTENSITY_TO_RAW(rt_emis_light_intensity) * RT_FIXUP_LIGHT_INTENSITY (10 * 1600 * 0.01).
inline constexpr float kEditorEmissionLightScale = 10.0f * 1600.0f * 0.01f;

inline constexpr uint64_t EditorLightUniqueID(size_t index)
{
    return kEditorFirstLightUniqueID + uint64_t(index);
}

inline constexpr uint64_t EditorCustomLightUniqueID(size_t index)
{
    return kEditorFirstCustomLightUniqueID + uint64_t(index);
}

// One clipped UV sub-polygon representing a single glowing repetition of an emissive texture.
struct EditorClippedGlowPoly
{
    QrFloat2D uvVerts[MAX_TEXTURED_AREA_LIGHT_VERTS] = {};
    int numVerts = 0;
    float area = 0.0f;
    float center[3] = {};
};

struct EditorGeneratedLightBase
{
    std::string name;
    uint64_t uniqueID = 0;
    float position[3] = {};
    float baseColor[3] = { 1.0f, 1.0f, 1.0f };
    float baseGain = 1.0f;
    float defaultRadiusMeters = 0.1f;
    float modelSizeMeters = 0.0f;
    bool spot = false;
    float direction[3] = { 0.0f, 0.0f, 1.0f };
    float angleInner = 0.0f;
    float angleOuter = 30.0f;
    bool lightShafts = false;
    float lightShaftsPower = 1.0f;
    float lightShaftsDistance = 0.0f;
    float lightShaftsFalloff = 1.0f;
    bool isAliasModel = false;
};

struct EditorEmissiveLight
{
    QrTexturedAreaLightUploadInfo info = {};
    size_t surfaceIndex = 0;
    float power = 0.0f;
    float center[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    bool isAliasModel = false;
};

struct EditorDtalBuildCounts
{
    uint32_t fitted = 0;
    uint32_t glowClipped = 0;
    uint32_t squared = 0;
};

struct EditorDtalBuildResult
{
    std::vector<EditorEmissiveLight> emissiveLights;
    std::vector<EditorGeneratedLightBase> generatedLightBases;
    EditorDtalBuildCounts counts;
};

struct EditorDtalMaterialLookup
{
    std::function<QrMaterial(const EditorSurface &, const EditorMaterialParams &)> surfaceMaterial;
    std::function<EditorMaterialEmission(QrMaterial)> materialEmission;
};

struct EditorDynamicMeshDtalRequest
{
    const EditorRenderSettings *settings = nullptr;
    const EditorMaterialParams *params = nullptr;
    QrMaterial lightMaterial = QR_NO_MATERIAL;
    EditorMaterialEmission emission;
    const std::vector<QrVertex> *vertices = nullptr;
    const std::vector<uint32_t> *indices = nullptr;
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    int32_t *budgetRemaining = nullptr;
    uint64_t *nextUniqueId = nullptr;
};

bool ClipFaceUvToGlowPolygons(const std::vector<QrFloat2D> &surfUv, const float A[3],
                               const float B[3], const float C[3],
                               const EditorMaterialEmission &emission,
                               std::vector<EditorClippedGlowPoly> &outPolys);

void BuildEditorSceneEmissiveLights(const EditorScene &scene,
                                    const EditorMaterialTable &materials,
                                    const EditorRenderSettings &settings,
                                    const EditorDtalMaterialLookup &lookup,
                                    EditorDtalBuildResult &out);

QrResult UploadEditorEmissiveLights(
    QrInstance instance, const EditorScene &scene, const EditorMaterialTable &materials,
    const std::vector<EditorEmissiveLight> &emissiveLights, float timeSeconds,
    const std::vector<EditorEmissiveLight> *extraDynamicLights = nullptr);

void BuildEditorDynamicMeshAreaLights(const EditorDynamicMeshDtalRequest &request,
                                      std::vector<EditorEmissiveLight> &outLights);

const EditorEmitterLightOverride *FindEditorEmitterOverride(
    const std::vector<EditorEmitterLightOverride> &overrides, const std::string &name,
    uint64_t uniqueID);

EditorLight ResolveEditorSingleGeneratedLight(const EditorGeneratedLightBase &base,
                                              const EditorEmitterLightOverride *ov,
                                              const EditorRenderSettings &settings,
                                              const EditorScene &scene);
