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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"
#include "EditorDtalPipeline.h"
#include "EditorMaterial.h"
#include "EditorOverlayPipeline.h"
#include "EditorSession.h"
#include "EditorSettings.h"
#include "Scene.h"

using EditorRuntimeLogFn = std::function<void(const char *)>;

struct EditorDynamicMaterialResult
{
    EditorMaterialParams params;
    QrMaterial surfaceMaterial = QR_NO_MATERIAL;
    QrMaterial lightMaterial = QR_NO_MATERIAL;
    EditorMaterialEmission emission;
    bool hasAuthoredOverride = false;
    bool suppressBaseEmission = false;
    bool emitRayOnly = false;
};

struct EditorLevelApplyResult
{
    bool hasSky = false;
    bool hasFog = false;
    bool hasLights = false;
    bool replacedStandardLights = false;
    EditorLevelDocument::ReadResult read;
};

struct EditorClusterLightUploadExtras
{
    const std::vector<QrClusterLightSource> *extraQuakeUnitSources = nullptr;
    const std::vector<EditorEmissiveLight> *extraDynamicEmissiveLights = nullptr;
};

void ApplyEditorResolutionParams(QrInstance instance, const EditorRenderSettings &settings,
                                 uint32_t clientWidth, uint32_t clientHeight,
                                 QrDrawFrameRenderResolutionParams &out);

class EditorSceneRuntime
{
public:
    EditorScene scene;
    EditorMaterialTable materials;
    EditorMaterialBakes materialBakes;
    EditorMaterialBakes dynamicMaterialBakes;
    EditorRenderSettings settings;
    EditorSunState sun;
    double celestialClockSeconds = 0.0;
    std::vector<EditorLight> customLights;
    std::vector<EditorEmissiveLight> emissiveLights;
    std::vector<EditorGeneratedLightBase> generatedLightBases;
    std::vector<EditorEmitterLightOverride> emitterLightOverrides;
    std::vector<QrMaterial> textureMaterials;
    std::vector<EditorMaterialEmission> textureEmissions;
    std::unordered_map<std::string, int32_t> textureIndexByName;
    std::unordered_set<std::string> missingExternalTextures;
    EditorOverlayCache overlayCache;
    QrMaterial skySolidMaterial = QR_NO_MATERIAL;
    QrMaterial skyAlphaMaterial = QR_NO_MATERIAL;
    QrMaterial skyboxMaterials[6] = {
        QR_NO_MATERIAL, QR_NO_MATERIAL, QR_NO_MATERIAL,
        QR_NO_MATERIAL, QR_NO_MATERIAL, QR_NO_MATERIAL,
    };
    uint32_t skyboxWidths[6] = {};
    uint32_t skyboxHeights[6] = {};
    bool hasSkybox = false;
    uint32_t clusterGeneration = 0;

    bool LoadBspSceneMemory(const std::vector<uint8_t> &bytes, const std::string &sourceName,
                            const uint8_t *palette768, const EditorSceneLoadOptions &options,
                            std::string &error);

    void ApplySceneRenderDefaults();
    EditorMaterialParams DefaultMaterialParamsFor(const std::string &name) const;
    void ApplyMaterialsDocuments(const EditorMaterialsDocument &activeDoc,
                                 const EditorMaterialsDocument *baseDoc,
                                 size_t *outLoaded = nullptr, size_t *outInherited = nullptr);
    int ApplyConfigText(const std::string &text);
    EditorLevelApplyResult ApplyLevelDocument(const EditorLevelDocument &levelDoc,
                                              const std::string &levelKey);
    void ReapplyGeneratedLights();

    static bool CreatePoolMaterial(QrInstance instance, const EditorTexture &texture,
                                   QrMaterial &outMaterial, EditorMaterialEmission &outEmission);
    static QrGeometryPassThroughType PassThroughTypeForSurface(EditorSurfaceKind kind);

    bool CreateTextureMaterials(QrInstance instance, const EditorAssetSource &assets,
                                const EditorRuntimeLogFn &log = {});
    int32_t TextureIndexByName(const std::string &name) const;
    int32_t ResolveOrLoadTexture(QrInstance instance, const EditorAssetSource &assets,
                                 const std::string &name);
    QrMaterial SurfaceMaterial(const EditorSurface &surface, QrMaterial fallbackMaterial) const;
    EditorMaterialEmission MaterialEmissionOf(QrMaterial material) const;
    QrMaterial SurfaceMaterialWithOverride(QrInstance instance, const EditorAssetSource &assets,
                                           const EditorSurface &surface,
                                           const EditorMaterialParams &params,
                                           QrMaterial fallbackMaterial);
    EditorDynamicMaterialResult ResolveDynamicMaterial(QrInstance instance,
                                                       const EditorAssetSource &assets,
                                                       const std::string &shaderName,
                                                       const EditorMaterialsDocument *activeDoc,
                                                       const EditorMaterialsDocument *baseDoc,
                                                       bool forceEnvBase = false);

    bool UploadStaticGeometry(QrInstance instance, const EditorAssetSource &assets,
                              QrMaterial fallbackMaterial, const EditorRuntimeLogFn &log = {});
    void BuildEmissiveLights(QrInstance instance, const EditorAssetSource &assets,
                             QrMaterial fallbackMaterial, const EditorRuntimeLogFn &log = {});
    bool UploadWorldClusters(QrInstance instance, const EditorRuntimeLogFn &log = {});

    void CreateSkyboxMaterials(QrInstance instance, const EditorAssetSource &assets,
                               const EditorRuntimeLogFn &log = {});
    void CreateScrollingSkyMaterials(QrInstance instance, const EditorAssetSource &assets,
                                     const EditorRuntimeLogFn &log = {});
    void UploadSky(QrInstance instance, const float cameraOrigin[3], double timeSeconds,
                   float skyDistance) const;
    void UploadWorldOverlays(
        QrInstance instance, const EditorAssetSource &assets, const EditorCameraBasis &cam,
        float timeSeconds,
        const std::vector<const std::vector<EditorTexture> *> *extraTextureLists = nullptr);

    void AdvanceCelestialClock(float deltaSeconds);
    void UploadSun(QrInstance instance, const EditorRuntimeLogFn &log = {}) const;
    static QrResult UploadSingleLight(QrInstance instance, const EditorLight &light,
                                      uint64_t uniqueID, float styleScale = 1.0f);
    QrResult UploadLights(QrInstance instance, double timeSeconds, bool animateLightStyles,
                          const EditorRuntimeLogFn &log = {}) const;
    QrResult UploadEmissiveLights(
        QrInstance instance, float timeSeconds,
        const std::vector<EditorEmissiveLight> *extraDynamicLights = nullptr,
        const EditorRuntimeLogFn &log = {}) const;
    QrResult UploadClusterLightSources(QrInstance instance,
                                       const EditorClusterLightUploadExtras *extras = nullptr,
                                       const EditorRuntimeLogFn &log = {}) const;

    void PopulateDrawFrame(QrInstance instance, const float viewerPos[3], uint32_t clientWidth,
                           uint32_t clientHeight, EditorFrameParams &outFrameParams,
                           QrDrawFrameInfo &ioDraw) const;

    void DestroyPoolMaterials(QrInstance instance);
    void DestroySkyMaterials(QrInstance instance);
    void DestroyOverlayMaterials(QrInstance instance);
    void DestroyGpuResources(QrInstance instance);
    void ResetGpuHandlesOnly();

private:
    bool UploadSurfaceGeometry(QrInstance instance, const EditorAssetSource &assets,
                               size_t surfaceIndex, QrMaterial fallbackMaterial,
                               std::vector<QrVertex> &meterVertices,
                               const EditorRuntimeLogFn &log);

    struct DocumentNameIndex
    {
        std::unordered_map<std::string, std::string> exact;
        std::unordered_map<std::string, std::string> stem;
    };

    DocumentNameIndex m_activeDocIndex;
    DocumentNameIndex m_baseDocIndex;
    std::unordered_map<std::string, EditorDynamicMaterialResult> m_dynamicMaterialCache;
};
