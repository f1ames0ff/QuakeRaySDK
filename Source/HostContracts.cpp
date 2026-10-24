// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "qray/host_contract.h"

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qray
{

namespace
{

enum QrayContractDiagnosticCode : uint32_t
{
    QRAY_DIAG_NONE = 0,
    QRAY_DIAG_NULL_POINTER,
    QRAY_DIAG_BAD_HEADER,
    QRAY_DIAG_BAD_KEY,
    QRAY_DIAG_DUPLICATE_KEY,
    QRAY_DIAG_DUPLICATE_RUNTIME_ID,
    QRAY_DIAG_BAD_ARRAY,
    QRAY_DIAG_BAD_GEOMETRY,
    QRAY_DIAG_BAD_MATERIAL,
    QRAY_DIAG_BAD_LIGHT,
    QRAY_DIAG_BAD_DTAL,
    QRAY_DIAG_BAD_VIEW,
    QRAY_DIAG_UNSUPPORTED,
    QRAY_DIAG_BAD_SIDECAR,
};

constexpr uint32_t kMaterialIntentMask =
    QRAY_MATERIAL_INTENT_ROUGHNESS |
    QRAY_MATERIAL_INTENT_METALLIC |
    QRAY_MATERIAL_INTENT_EMISSION |
    QRAY_MATERIAL_INTENT_LAYER_COLORS |
    QRAY_MATERIAL_INTENT_ALBEDO_TEXTURE |
    QRAY_MATERIAL_INTENT_RME_TEXTURE |
    QRAY_MATERIAL_INTENT_NORMAL_TEXTURE;

constexpr uint32_t kLightIntentMask =
    QRAY_LIGHT_INTENT_COLOR |
    QRAY_LIGHT_INTENT_POSITION |
    QRAY_LIGHT_INTENT_DIRECTION |
    QRAY_LIGHT_INTENT_RADIUS |
    QRAY_LIGHT_INTENT_CONE |
    QRAY_LIGHT_INTENT_POWER;

constexpr uint32_t kDtalIntentMask =
    QRAY_DTAL_INTENT_MATERIAL |
    QRAY_DTAL_INTENT_COLOR |
    QRAY_DTAL_INTENT_REACH |
    QRAY_DTAL_INTENT_POWER;

struct KeyHash
{
    size_t operator()(const QrayStableKey &key) const noexcept
    {
        uint64_t value = key.value;
        value ^= uint64_t(key.domain) << 56;
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        return static_cast<size_t>(value);
    }
};

struct UUIDHash
{
    size_t operator()(const QrayAuthoringUUID &id) const noexcept
    {
        uint64_t value = id.high ^ (id.low + 0x9e3779b97f4a7c15ull + (id.high << 6) + (id.high >> 2));
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        return static_cast<size_t>(value);
    }
};

struct OwnedLight
{
    QrayStableKey key;
    QraySceneLightKind kind = QraySceneLightKind::Spherical;
    QrMaterial areaMaterial = QR_NO_MATERIAL;
    QrDirectionalLightUploadInfo directional = {};
    QrSphericalLightUploadInfo spherical = {};
    QrSpotLightUploadInfo spot = {};
    QrPolygonalLightUploadInfo polygonal = {};
    QrTexturedAreaLightUploadInfo texturedArea = {};
};

struct OwnedDtal
{
    std::vector<QrDtalGroupUploadInfo> groups;
    std::vector<QrDtalMemberUpload> members;
};

struct SurfaceBinding
{
    uint64_t runtimeId = 0;
    uint32_t vertexCount = 0;
};

QrResult Fail(QrayContractDiagnostic *pDiagnostic, QrResult result, uint32_t code,
              uint32_t index = UINT32_MAX) noexcept
{
    if (pDiagnostic != nullptr)
    {
        pDiagnostic->result = result;
        pDiagnostic->code = code;
        pDiagnostic->index = index;
    }
    return result;
}

QrResult Success(QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pDiagnostic != nullptr)
    {
        pDiagnostic->result = QR_SUCCESS;
        pDiagnostic->code = QRAY_DIAG_NONE;
        pDiagnostic->index = UINT32_MAX;
    }
    return QR_SUCCESS;
}

template<typename T>
bool HeaderValid(const QrayContractHeader &header) noexcept
{
    return header.size >= sizeof(T) && (header.version >> 16) == kHostContractMajor;
}

template<typename T>
bool ArrayValid(const QrayArrayView<T> &array) noexcept
{
    return array.count == 0 || array.pData != nullptr;
}

bool KeyValid(const QrayStableKey &key, QrayStableKeyDomain domain) noexcept
{
    return key.value != 0 && key.domain == uint32_t(domain) && key.reserved == 0;
}

bool KeyIsOptionalMaterial(const QrayStableKey &key) noexcept
{
    return key.value == 0 || KeyValid(key, QRAY_KEY_MATERIAL);
}

bool UuidValid(const QrayAuthoringUUID &id) noexcept
{
    return id.high != 0 || id.low != 0;
}

bool Float3Finite(const QrFloat3D &value) noexcept
{
    return std::isfinite(value.data[0]) && std::isfinite(value.data[1]) &&
           std::isfinite(value.data[2]);
}

bool Float4Finite(const QrFloat4D &value) noexcept
{
    return std::isfinite(value.data[0]) && std::isfinite(value.data[1]) &&
           std::isfinite(value.data[2]) && std::isfinite(value.data[3]);
}

bool TexturePathValid(const char *path) noexcept
{
    return path != nullptr && path[0] != '\0';
}

bool MaterialIntentValid(const QrayMaterialIntent &intent) noexcept
{
    if (!KeyValid(intent.surface, QRAY_KEY_SURFACE) || intent.flags == 0 ||
        (intent.flags & ~kMaterialIntentMask) != 0)
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_ROUGHNESS) != 0 &&
        (!std::isfinite(intent.roughness) || intent.roughness < 0.0f || intent.roughness > 1.0f))
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_METALLIC) != 0 &&
        (!std::isfinite(intent.metallic) || intent.metallic < 0.0f || intent.metallic > 1.0f))
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_EMISSION) != 0 &&
        (!std::isfinite(intent.emission) || intent.emission < 0.0f))
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_LAYER_COLORS) != 0)
    {
        for (const QrFloat4D &color : intent.layerColors)
            if (!Float4Finite(color))
                return false;
    }
    if ((intent.flags & QRAY_MATERIAL_INTENT_ALBEDO_TEXTURE) != 0 &&
        !TexturePathValid(intent.pAlbedoTexture))
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_RME_TEXTURE) != 0 &&
        !TexturePathValid(intent.pRoughnessMetallicEmissionTexture))
        return false;
    if ((intent.flags & QRAY_MATERIAL_INTENT_NORMAL_TEXTURE) != 0 &&
        !TexturePathValid(intent.pNormalTexture))
        return false;
    return true;
}

bool LightIntentValid(const QrayLightIntent &intent) noexcept
{
    if (!KeyValid(intent.light, QRAY_KEY_LIGHT) || !KeyIsOptionalMaterial(intent.material) ||
        intent.flags == 0 || (intent.flags & ~kLightIntentMask) != 0)
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_COLOR) != 0 && !Float3Finite(intent.color))
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_POSITION) != 0 && !Float3Finite(intent.position))
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_DIRECTION) != 0 && !Float3Finite(intent.direction))
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_RADIUS) != 0 &&
        (!std::isfinite(intent.radius) || intent.radius < 0.0f))
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_CONE) != 0 &&
        (!std::isfinite(intent.angleInner) || !std::isfinite(intent.angleOuter) ||
         intent.angleInner < 0.0f || intent.angleOuter < intent.angleInner))
        return false;
    if ((intent.flags & QRAY_LIGHT_INTENT_POWER) != 0 &&
        (!std::isfinite(intent.power) || intent.power < 0.0f))
        return false;
    return true;
}

bool DtalIntentValid(const QrayDtalIntent &intent) noexcept
{
    if (!KeyValid(intent.group, QRAY_KEY_DTAL) || !KeyIsOptionalMaterial(intent.material) ||
        intent.flags == 0 || (intent.flags & ~kDtalIntentMask) != 0)
        return false;
    if ((intent.flags & QRAY_DTAL_INTENT_MATERIAL) != 0 && intent.material.value == 0)
        return false;
    if ((intent.flags & QRAY_DTAL_INTENT_COLOR) != 0 && !Float3Finite(intent.color))
        return false;
    if ((intent.flags & QRAY_DTAL_INTENT_REACH) != 0 &&
        (!std::isfinite(intent.reach) || intent.reach < 0.0f))
        return false;
    if ((intent.flags & QRAY_DTAL_INTENT_POWER) != 0 &&
        (!std::isfinite(intent.estimatedPower) || intent.estimatedPower < 0.0f))
        return false;
    return true;
}

bool MaterialCreateValid(const QraySceneMaterial &material) noexcept
{
    if (!KeyValid(material.key, QRAY_KEY_MATERIAL))
        return false;

    const bool hasStatic = material.pCreateInfo != nullptr;
    const bool hasAnimated = material.pAnimatedCreateInfo != nullptr;
    if (hasStatic == hasAnimated)
        return false;

    if (hasAnimated)
    {
        return material.pAnimatedCreateInfo->frameCount != 0 &&
               material.pAnimatedCreateInfo->pFrames != nullptr;
    }

    const QrMaterialCreateInfo &info = *material.pCreateInfo;
    if (info.size.width == 0 || info.size.height == 0)
        return false;

    return info.pRelativePath != nullptr || info.textures.pDataAlbedoAlpha != nullptr ||
           info.textures.pDataRoughnessMetallicEmission != nullptr ||
           info.textures.pDataNormal != nullptr;
}

bool GeometryValid(const QraySceneGeometry &geometry, bool staticOnly) noexcept
{
    if (!KeyValid(geometry.key, QRAY_KEY_SURFACE) || geometry.geometry.uniqueID == 0)
        return false;
    if (staticOnly && geometry.geometry.geomType != QR_GEOMETRY_TYPE_STATIC &&
        geometry.geometry.geomType != QR_GEOMETRY_TYPE_STATIC_MOVABLE)
        return false;
    if (geometry.geometry.vertexCount == 0 || geometry.geometry.pVertices == nullptr)
        return false;
    if (geometry.geometry.indexCount != 0 && geometry.geometry.pIndices == nullptr)
        return false;
    for (const QrayStableKey &key : geometry.materialKeys)
    {
        if (!KeyIsOptionalMaterial(key))
            return false;
    }
    for (const QrMaterial material : geometry.geometry.geomMaterial.layerMaterials)
    {
        if (material != QR_NO_MATERIAL)
            return false;
    }
    return true;
}

bool LightValid(const QraySceneLight &light) noexcept
{
    if (!KeyValid(light.key, QRAY_KEY_LIGHT) || !KeyIsOptionalMaterial(light.areaMaterial))
        return false;

    switch (light.kind)
    {
    case QraySceneLightKind::Directional:
        return light.pDirectional != nullptr && light.pDirectional->uniqueID != 0 &&
               Float3Finite(light.pDirectional->color) &&
               Float3Finite(light.pDirectional->direction);
    case QraySceneLightKind::Spherical:
        return light.pSpherical != nullptr && light.pSpherical->uniqueID != 0 &&
               Float3Finite(light.pSpherical->color) &&
               Float3Finite(light.pSpherical->position) &&
               std::isfinite(light.pSpherical->radius) && light.pSpherical->radius >= 0.0f;
    case QraySceneLightKind::Spot:
        return light.pSpot != nullptr && light.pSpot->uniqueID != 0 &&
               Float3Finite(light.pSpot->color) && Float3Finite(light.pSpot->position) &&
               Float3Finite(light.pSpot->direction) && std::isfinite(light.pSpot->radius) &&
               std::isfinite(light.pSpot->angleInner) &&
               std::isfinite(light.pSpot->angleOuter);
    case QraySceneLightKind::Polygonal:
        return light.pPolygonal != nullptr && light.pPolygonal->uniqueID != 0 &&
               Float3Finite(light.pPolygonal->color);
    case QraySceneLightKind::TexturedArea:
        return light.pTexturedArea != nullptr && light.pTexturedArea->uniqueID != 0 &&
               Float3Finite(light.pTexturedArea->color) &&
               Float3Finite(light.pTexturedArea->normal) &&
               std::isfinite(light.pTexturedArea->area) && light.pTexturedArea->area > 0.0f &&
               light.pTexturedArea->material == QR_NO_MATERIAL;
    }
    return false;
}

bool DtalValid(const QraySceneDtalBatch &dtal) noexcept
{
    if (dtal.groupCount == 0 || dtal.pGroups == nullptr)
        return false;
    if (dtal.memberCount > QR_DTAL_MAX_UPLOAD_MEMBERS)
        return false;
    if (dtal.memberCount != 0 && dtal.pMembers == nullptr)
        return false;
    if (dtal.groupMaterialKeys.count != dtal.groupCount ||
        dtal.groupMaterialKeys.pData == nullptr)
        return false;

    for (uint32_t i = 0; i < dtal.groupCount; ++i)
    {
        const QrDtalGroupUploadInfo &group = dtal.pGroups[i];
        if (group.uniqueID == 0 || group.memberCount == 0 ||
            group.memberBase > dtal.memberCount ||
            group.memberCount > dtal.memberCount - group.memberBase ||
            group.material != QR_NO_MATERIAL ||
            !KeyIsOptionalMaterial(dtal.groupMaterialKeys.pData[i]))
        {
            return false;
        }
    }
    return true;
}

bool WorldLightsValid(const QraySceneWorldLights &world) noexcept
{
    if (world.pUpload == nullptr)
        return world.faceMaterialKeys.count == 0 && world.faceMaterialKeys.pData == nullptr;

    const QrWorldLightsUploadInfo &upload = *world.pUpload;
    if (upload.numClusters != 0 &&
        (upload.pClusterMins == nullptr || upload.pClusterMaxs == nullptr))
        return false;
    if (upload.numFaces != 0)
    {
        if (upload.pFaces == nullptr)
            return false;
        if (world.faceMaterialKeys.count != upload.numFaces ||
            world.faceMaterialKeys.pData == nullptr)
            return false;
        for (uint32_t i = 0; i < upload.numFaces; ++i)
        {
            if (upload.pFaces[i].material != QR_NO_MATERIAL ||
                !KeyIsOptionalMaterial(world.faceMaterialKeys.pData[i]))
                return false;
        }
    }
    if (upload.numFaceVertices != 0 && upload.pFaceVertices == nullptr)
        return false;
    if (upload.visDataSize != 0 &&
        (upload.pVisData == nullptr || upload.pVisOffsets == nullptr || upload.pvsRowBytes == 0))
        return false;
    return true;
}

bool MaterialContentUpdateValid(const QrayMaterialContentUpdate &update) noexcept
{
    return KeyValid(update.material, QRAY_KEY_MATERIAL) &&
           (update.textures.pDataAlbedoAlpha != nullptr ||
            update.textures.pDataRoughnessMetallicEmission != nullptr ||
            update.textures.pDataNormal != nullptr);
}

bool RasterizedUpdateValid(const QrayRasterizedUpdate &update) noexcept
{
    return KeyIsOptionalMaterial(update.material) && update.upload.vertexCount != 0 &&
           update.upload.pVertices != nullptr && update.upload.material == QR_NO_MATERIAL &&
           update.pViewProjection != nullptr;
}

bool ParticleUpdateValid(const QrayParticleUpdate &update) noexcept
{
    return KeyIsOptionalMaterial(update.material) && update.upload.count != 0 &&
           update.upload.pPoints != nullptr && update.upload.material == QR_NO_MATERIAL;
}

bool DecalUpdateValid(const QrayDecalUpdate &update) noexcept
{
    return KeyIsOptionalMaterial(update.material) &&
           update.upload.material == QR_NO_MATERIAL;
}

bool ClusterSourcesValid(const QrClusterLightSourcesUploadInfo &sources) noexcept
{
    if (sources.numLights != 0 && sources.pLights == nullptr)
        return false;
    if (sources.numLights > QR_CLUSTER_MAX_REGISTERED_LIGHTS)
        return false;
    std::unordered_set<uint64_t> ids;
    for (uint32_t i = 0; i < sources.numLights; ++i)
    {
        const QrClusterLightSource &light = sources.pLights[i];
        if (light.uniqueID == 0 ||
            (light.clusterCount != 0 && light.pClusters == nullptr) ||
            !ids.insert(light.uniqueID).second)
            return false;
    }
    return true;
}

QrResult ApplyLight(QrInstance instance, const QraySceneLight &light,
                    const std::unordered_map<QrayStableKey, QrMaterial, KeyHash> &materials) noexcept
{
    QrMaterial areaMaterial = QR_NO_MATERIAL;
    if (light.areaMaterial.value != 0)
    {
        const auto it = materials.find(light.areaMaterial);
        if (it == materials.end())
            return QR_WRONG_ARGUMENT;
        areaMaterial = it->second;
    }

    switch (light.kind)
    {
    case QraySceneLightKind::Directional:
        return qrUploadDirectionalLight(instance, light.pDirectional);
    case QraySceneLightKind::Spherical:
        return qrUploadSphericalLight(instance, light.pSpherical);
    case QraySceneLightKind::Spot:
        return qrUploadSpotLight(instance, light.pSpot);
    case QraySceneLightKind::Polygonal:
        return qrUploadPolygonalLight(instance, light.pPolygonal);
    case QraySceneLightKind::TexturedArea:
    {
        QrTexturedAreaLightUploadInfo upload = *light.pTexturedArea;
        upload.material = areaMaterial;
        return qrUploadTexturedAreaLight(instance, &upload);
    }
    }
    return QR_WRONG_ARGUMENT;
}

QrResult ApplyOwnedLight(QrInstance instance, const OwnedLight &light) noexcept
{
    switch (light.kind)
    {
    case QraySceneLightKind::Directional:
        return qrUploadDirectionalLight(instance, &light.directional);
    case QraySceneLightKind::Spherical:
        return qrUploadSphericalLight(instance, &light.spherical);
    case QraySceneLightKind::Spot:
        return qrUploadSpotLight(instance, &light.spot);
    case QraySceneLightKind::Polygonal:
        return qrUploadPolygonalLight(instance, &light.polygonal);
    case QraySceneLightKind::TexturedArea:
    {
        QrTexturedAreaLightUploadInfo upload = light.texturedArea;
        upload.material = light.areaMaterial;
        return qrUploadTexturedAreaLight(instance, &upload);
    }
    }
    return QR_WRONG_ARGUMENT;
}

QrResult ApplyDtal(QrInstance instance, const QraySceneDtalBatch &dtal,
                   const std::unordered_map<QrayStableKey, QrMaterial, KeyHash> &materials) noexcept
{
    std::vector<QrDtalGroupUploadInfo> groups(dtal.pGroups, dtal.pGroups + dtal.groupCount);
    for (uint32_t i = 0; i < dtal.groupCount; ++i)
    {
        const QrayStableKey &key = dtal.groupMaterialKeys.pData[i];
        if (key.value == 0)
            continue;
        const auto it = materials.find(key);
        if (it == materials.end())
            return QR_WRONG_ARGUMENT;
        groups[i].material = it->second;
    }

    const QrDtalGroupUploadBatch batch = {
        dtal.groupCount,
        groups.data(),
        dtal.memberCount,
        dtal.pMembers,
    };
    return qrUploadDtalGroups(instance, &batch);
}

QrResult ApplyOwnedDtal(QrInstance instance, const OwnedDtal &dtal) noexcept
{
    const QrDtalGroupUploadBatch batch = {
        static_cast<uint32_t>(dtal.groups.size()),
        dtal.groups.data(),
        static_cast<uint32_t>(dtal.members.size()),
        dtal.members.data(),
    };
    return qrUploadDtalGroups(instance, &batch);
}

OwnedLight OwnLight(const QraySceneLight &light, QrMaterial areaMaterial)
{
    OwnedLight owned;
    owned.key = light.key;
    owned.kind = light.kind;
    owned.areaMaterial = areaMaterial;
    switch (light.kind)
    {
    case QraySceneLightKind::Directional:
        owned.directional = *light.pDirectional;
        break;
    case QraySceneLightKind::Spherical:
        owned.spherical = *light.pSpherical;
        break;
    case QraySceneLightKind::Spot:
        owned.spot = *light.pSpot;
        break;
    case QraySceneLightKind::Polygonal:
        owned.polygonal = *light.pPolygonal;
        break;
    case QraySceneLightKind::TexturedArea:
        owned.texturedArea = *light.pTexturedArea;
        break;
    }
    return owned;
}

OwnedDtal OwnDtal(const QraySceneDtalBatch &dtal,
                  const std::unordered_map<QrayStableKey, QrMaterial, KeyHash> &materials) noexcept
{
    OwnedDtal owned;
    owned.groups.assign(dtal.pGroups, dtal.pGroups + dtal.groupCount);
    owned.members.assign(dtal.pMembers, dtal.pMembers + dtal.memberCount);
    for (uint32_t i = 0; i < dtal.groupCount; ++i)
    {
        const QrayStableKey &key = dtal.groupMaterialKeys.pData[i];
        if (key.value == 0)
            continue;
        const auto it = materials.find(key);
        if (it == materials.end())
            return {};
        owned.groups[i].material = it->second;
    }
    return owned;
}

QrMaterial MaterialFor(const std::unordered_map<QrayStableKey, QrMaterial, KeyHash> &materials,
                       const QrayStableKey &key) noexcept
{
    if (key.value == 0)
        return QR_NO_MATERIAL;
    const auto it = materials.find(key);
    return it == materials.end() ? QR_NO_MATERIAL : it->second;
}

} // namespace

QrResult IQrayHost::Attach(QrInstance) noexcept
{
    return QR_SUCCESS;
}

void IQrayHost::Detach() noexcept {}

QrResult IQrayHost::SurfaceKey(uint64_t, QrayStableKey *) const noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::ResolveSurface(const QrayStableKey &, uint64_t *) const noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::ResolveMaterial(const QrayStableKey &, QrMaterial *) const noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::LightKey(uint64_t, QrayStableKey *) const noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::Refresh(const QrayFrameView &, QrayFrameUpdates *) noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::EvaluateStyles(double, QrayArrayView<const uint8_t>,
                                   QrayMutableArrayView<float>) noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::ApplySidecar(const QraySidecar &, QrayMutableArrayView<QraySidecarEntryResult>,
                                 uint32_t *) noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::LoadAuthoredLights(QrayMutableArrayView<QraySceneLight>, uint32_t *) noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

QrResult IQrayHost::Pick(const QrayPickRequest &, QrayPickResult *) noexcept
{
    return QR_FEATURE_UNAVAILABLE;
}

uint32_t qrayGetHostContractVersion() noexcept
{
    return kHostContractVersion;
}

QrResult qrayValidateSceneAsset(const QraySceneAsset *pAsset,
                                QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pAsset == nullptr)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_NULL_POINTER);
    if (!HeaderValid<QraySceneAsset>(pAsset->header))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_HEADER);
    if (!KeyValid(pAsset->sceneKey, QRAY_KEY_SCENE))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_KEY);
    if (pAsset->fingerprint.reserved != 0)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_HEADER);
    if (!ArrayValid(pAsset->materials) || !ArrayValid(pAsset->geometry) ||
        !ArrayValid(pAsset->lights) || !ArrayValid(pAsset->dtalBatches) ||
        !ArrayValid(pAsset->fogVolumes))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_ARRAY);

    std::unordered_set<QrayStableKey, KeyHash> materialKeys;
    std::unordered_set<QrayStableKey, KeyHash> surfaceKeys;
    std::unordered_set<QrayStableKey, KeyHash> lightKeys;
    std::unordered_set<uint64_t> geometryIds;
    std::unordered_set<uint64_t> lightIds;

    for (uint32_t i = 0; i < pAsset->materials.count; ++i)
    {
        const QraySceneMaterial &material = pAsset->materials.pData[i];
        if (!MaterialCreateValid(material))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
        if (!materialKeys.insert(material.key).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_DUPLICATE_KEY, i);
    }
    for (uint32_t i = 0; i < pAsset->geometry.count; ++i)
    {
        const QraySceneGeometry &geometry = pAsset->geometry.pData[i];
        if (!GeometryValid(geometry, true))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY, i);
        if (!surfaceKeys.insert(geometry.key).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_DUPLICATE_KEY, i);
        if (!geometryIds.insert(geometry.geometry.uniqueID).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_DUPLICATE_RUNTIME_ID, i);
        for (const QrayStableKey &key : geometry.materialKeys)
        {
            if (key.value != 0 && materialKeys.find(key) == materialKeys.end())
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
        }
    }
    for (uint32_t i = 0; i < pAsset->lights.count; ++i)
    {
        const QraySceneLight &light = pAsset->lights.pData[i];
        if (!LightValid(light))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_LIGHT, i);
        if (!lightKeys.insert(light.key).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_DUPLICATE_KEY, i);
        const uint64_t id = light.kind == QraySceneLightKind::Directional ? light.pDirectional->uniqueID :
            light.kind == QraySceneLightKind::Spherical ? light.pSpherical->uniqueID :
            light.kind == QraySceneLightKind::Spot ? light.pSpot->uniqueID :
            light.kind == QraySceneLightKind::Polygonal ? light.pPolygonal->uniqueID :
            light.pTexturedArea->uniqueID;
        if (!lightIds.insert(id).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_DUPLICATE_RUNTIME_ID, i);
        if (light.areaMaterial.value != 0 &&
            materialKeys.find(light.areaMaterial) == materialKeys.end())
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
    }
    for (uint32_t i = 0; i < pAsset->dtalBatches.count; ++i)
    {
        const QraySceneDtalBatch &dtal = pAsset->dtalBatches.pData[i];
        if (!DtalValid(dtal))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_DTAL, i);
        for (uint32_t j = 0; j < dtal.groupCount; ++j)
        {
            const QrayStableKey &key = dtal.groupMaterialKeys.pData[j];
            if (key.value != 0 && materialKeys.find(key) == materialKeys.end())
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
        }
    }
    if (!WorldLightsValid(pAsset->worldLights))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY);
    if (pAsset->worldLights.pUpload != nullptr)
    {
        for (uint32_t i = 0; i < pAsset->worldLights.faceMaterialKeys.count; ++i)
        {
            const QrayStableKey &key = pAsset->worldLights.faceMaterialKeys.pData[i];
            if (key.value != 0 && materialKeys.find(key) == materialKeys.end())
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
        }
    }
    return Success(pDiagnostic);
}

QrResult qrayValidateFrameView(const QrayFrameView *pView,
                               QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pView == nullptr)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_NULL_POINTER);
    if (!HeaderValid<QrayFrameView>(pView->header))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_HEADER);
    if (pView->viewId == 0 || pView->reserved != 0 || pView->pStartInfo == nullptr ||
        pView->pDrawInfo == nullptr)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_VIEW);
    if (pView->flags & QRAY_FRAME_VIEW_RESET_HISTORY)
        return Fail(pDiagnostic, QR_FEATURE_UNAVAILABLE, QRAY_DIAG_UNSUPPORTED);
    if ((pView->flags & ~QRAY_FRAME_VIEW_RESET_HISTORY) != 0)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_VIEW);
    return Success(pDiagnostic);
}

QrResult qrayValidateFrameUpdates(const QrayFrameUpdates *pUpdates,
                                  QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pUpdates == nullptr)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_NULL_POINTER);
    if (!HeaderValid<QrayFrameUpdates>(pUpdates->header))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_HEADER);
    if (!ArrayValid(pUpdates->transforms) || !ArrayValid(pUpdates->texCoords) ||
        !ArrayValid(pUpdates->materialContents) || !ArrayValid(pUpdates->dynamicGeometry) ||
        !ArrayValid(pUpdates->rasterized) || !ArrayValid(pUpdates->particles) ||
        !ArrayValid(pUpdates->decals) || !ArrayValid(pUpdates->portals) ||
        !ArrayValid(pUpdates->lights) || !ArrayValid(pUpdates->dtalBatches) ||
        !ArrayValid(pUpdates->fogVolumes))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_ARRAY);

    for (uint32_t i = 0; i < pUpdates->transforms.count; ++i)
        if (!KeyValid(pUpdates->transforms.pData[i].geometry, QRAY_KEY_SURFACE))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_KEY, i);
    for (uint32_t i = 0; i < pUpdates->texCoords.count; ++i)
    {
        const QrayTexCoordUpdate &update = pUpdates->texCoords.pData[i];
        if (!KeyValid(update.geometry, QRAY_KEY_SURFACE) || update.vertexCount == 0 ||
            (update.pTexCoordLayerData[0] == nullptr &&
             update.pTexCoordLayerData[1] == nullptr &&
             update.pTexCoordLayerData[2] == nullptr))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_KEY, i);
    }
    for (uint32_t i = 0; i < pUpdates->materialContents.count; ++i)
        if (!MaterialContentUpdateValid(pUpdates->materialContents.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_MATERIAL, i);
    for (uint32_t i = 0; i < pUpdates->dynamicGeometry.count; ++i)
    {
        const QraySceneGeometry &geometry = pUpdates->dynamicGeometry.pData[i];
        if (!GeometryValid(geometry, false) ||
            geometry.geometry.geomType != QR_GEOMETRY_TYPE_DYNAMIC)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY, i);
    }
    for (uint32_t i = 0; i < pUpdates->rasterized.count; ++i)
        if (!RasterizedUpdateValid(pUpdates->rasterized.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY, i);
    for (uint32_t i = 0; i < pUpdates->particles.count; ++i)
        if (!ParticleUpdateValid(pUpdates->particles.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY, i);
    for (uint32_t i = 0; i < pUpdates->decals.count; ++i)
        if (!DecalUpdateValid(pUpdates->decals.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_GEOMETRY, i);
    for (uint32_t i = 0; i < pUpdates->lights.count; ++i)
        if (!LightValid(pUpdates->lights.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_LIGHT, i);
    for (uint32_t i = 0; i < pUpdates->dtalBatches.count; ++i)
        if (!DtalValid(pUpdates->dtalBatches.pData[i]))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_DTAL, i);
    if (pUpdates->pClusterSources != nullptr &&
        !ClusterSourcesValid(*pUpdates->pClusterSources))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_ARRAY);
    return Success(pDiagnostic);
}

QrResult qrayValidateSidecar(const QraySidecar *pSidecar,
                             QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pSidecar == nullptr)
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_NULL_POINTER);
    if (!HeaderValid<QraySidecar>(pSidecar->header))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_HEADER);
    if (pSidecar->schemaVersion != 1 || pSidecar->reserved != 0 ||
        !KeyValid(pSidecar->sceneKey, QRAY_KEY_SCENE) ||
        pSidecar->fingerprint.reserved != 0 || !ArrayValid(pSidecar->entries))
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_SIDECAR);

    std::unordered_set<QrayAuthoringUUID, UUIDHash> ids;
    for (uint32_t i = 0; i < pSidecar->entries.count; ++i)
    {
        const QraySidecarEntry &entry = pSidecar->entries.pData[i];
        if (!UuidValid(entry.authoringId) || !ids.insert(entry.authoringId).second)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_SIDECAR, i);
        switch (entry.kind)
        {
        case QraySidecarEntryKind::Material:
            if (!MaterialIntentValid(entry.material))
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_SIDECAR, i);
            break;
        case QraySidecarEntryKind::Light:
            if (!LightIntentValid(entry.light))
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_SIDECAR, i);
            break;
        case QraySidecarEntryKind::DtalGroup:
            if (!DtalIntentValid(entry.dtal))
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, QRAY_DIAG_BAD_SIDECAR, i);
            break;
        default:
            return Fail(pDiagnostic, QR_FEATURE_UNAVAILABLE, QRAY_DIAG_UNSUPPORTED, i);
        }
    }
    return Success(pDiagnostic);
}

const char *qrayGetSidecarEntryStatusName(QraySidecarEntryStatus status) noexcept
{
    switch (status)
    {
    case QraySidecarEntryStatus::Applied: return "applied";
    case QraySidecarEntryStatus::Rebound: return "rebound";
    case QraySidecarEntryStatus::Unchanged: return "unchanged";
    case QraySidecarEntryStatus::Orphaned: return "orphaned";
    case QraySidecarEntryStatus::Ambiguous: return "ambiguous";
    case QraySidecarEntryStatus::Unsupported: return "unsupported";
    case QraySidecarEntryStatus::Invalid: return "invalid";
    }
    return "unknown";
}

struct QraySceneBinding::Impl
{
    QrInstance instance = nullptr;
    QrayStableKey sceneKey;
    QraySceneFingerprint fingerprint;
    bool bound = false;
    bool poisoned = false;
    bool frameOpen = false;
    std::unordered_map<QrayStableKey, QrMaterial, KeyHash> materials;
    std::unordered_map<QrayStableKey, SurfaceBinding, KeyHash> surfaces;
    std::unordered_set<uint64_t> surfaceRuntimeIds;
    std::unordered_map<QrayStableKey, uint64_t, KeyHash> lights;
    std::vector<OwnedLight> sceneLights;
    std::vector<OwnedDtal> sceneDtal;

    ~Impl()
    {
        for (const auto &[key, material] : materials)
        {
            (void)key;
            qrDestroyMaterial(instance, material);
        }
    }
};

QraySceneBinding::QraySceneBinding() noexcept
    : m_impl(std::make_unique<Impl>())
{
}

QraySceneBinding::~QraySceneBinding() = default;

QraySceneBinding::QraySceneBinding(QraySceneBinding &&) noexcept = default;

QraySceneBinding &QraySceneBinding::operator=(QraySceneBinding &&) noexcept = default;

QrResult QraySceneBinding::Bind(QrInstance instance, const QraySceneAsset &asset) noexcept
{
    if (m_impl->frameOpen)
        return QR_WRONG_FUNCTION_CALL;

    QrResult result = qrayValidateSceneAsset(&asset);
    if (result != QR_SUCCESS)
        return result;

    std::unordered_map<QrayStableKey, QrMaterial, KeyHash> newMaterials;
    std::vector<QrMaterial> createdMaterials;
    auto poison = [&](QrResult failure) {
        for (QrMaterial created : createdMaterials)
            qrDestroyMaterial(instance, created);
        for (const auto &[key, material] : m_impl->materials)
        {
            (void)key;
            qrDestroyMaterial(m_impl->instance, material);
        }
        m_impl->materials.clear();
        m_impl->surfaces.clear();
        m_impl->surfaceRuntimeIds.clear();
        m_impl->lights.clear();
        m_impl->sceneLights.clear();
        m_impl->sceneDtal.clear();
        m_impl->instance = nullptr;
        m_impl->bound = false;
        m_impl->poisoned = true;
        return failure;
    };
    for (uint32_t i = 0; i < asset.materials.count; ++i)
    {
        const QraySceneMaterial &record = asset.materials.pData[i];
        QrMaterial material = QR_NO_MATERIAL;
        result = record.pAnimatedCreateInfo != nullptr
            ? qrCreateAnimatedMaterial(instance, record.pAnimatedCreateInfo, &material)
            : qrCreateMaterial(instance, record.pCreateInfo, &material);
        if (result != QR_SUCCESS || material == QR_NO_MATERIAL)
        {
            for (QrMaterial created : createdMaterials)
                qrDestroyMaterial(instance, created);
            return result != QR_SUCCESS ? result : QR_WRONG_ARGUMENT;
        }
        createdMaterials.push_back(material);
        newMaterials.emplace(record.key, material);
    }

    std::vector<QrWorldLightFace> faces;
    if (asset.worldLights.pUpload != nullptr && asset.worldLights.pUpload->numFaces != 0)
    {
        faces.assign(asset.worldLights.pUpload->pFaces,
                     asset.worldLights.pUpload->pFaces + asset.worldLights.pUpload->numFaces);
        for (uint32_t i = 0; i < asset.worldLights.pUpload->numFaces; ++i)
            faces[i].material = MaterialFor(newMaterials, asset.worldLights.faceMaterialKeys.pData[i]);
    }

    result = qrBeginStaticGeometries(instance);
    if (result != QR_SUCCESS)
    {
        for (QrMaterial created : createdMaterials)
            qrDestroyMaterial(instance, created);
        return result;
    }

    for (uint32_t i = 0; i < asset.geometry.count; ++i)
    {
        const QraySceneGeometry &record = asset.geometry.pData[i];
        QrGeometryUploadInfo upload = record.geometry;
        for (uint32_t layer = 0; layer < 3; ++layer)
            upload.geomMaterial.layerMaterials[layer] = MaterialFor(newMaterials, record.materialKeys[layer]);
        result = qrUploadGeometry(instance, &upload);
        if (result != QR_SUCCESS)
        {
            qrSubmitStaticGeometries(instance);
            return poison(result);
        }
    }

    result = qrSubmitStaticGeometries(instance);
    if (result != QR_SUCCESS)
        return poison(result);

    if (asset.worldLights.pUpload != nullptr)
    {
        QrWorldLightsUploadInfo upload = *asset.worldLights.pUpload;
        if (!faces.empty())
            upload.pFaces = faces.data();
        result = qrUploadWorldLights(instance, &upload);
        if (result != QR_SUCCESS)
            return poison(result);
    }
    if (asset.fogVolumes.count != 0)
    {
        result = qrSetFogVolumes(instance, asset.fogVolumes.count, asset.fogVolumes.pData);
        if (result != QR_SUCCESS)
            return poison(result);
    }

    std::unordered_map<QrayStableKey, SurfaceBinding, KeyHash> newSurfaces;
    std::unordered_set<uint64_t> newSurfaceRuntimeIds;
    std::unordered_map<QrayStableKey, uint64_t, KeyHash> newLights;
    std::vector<OwnedLight> ownedLights;
    std::vector<OwnedDtal> ownedDtal;

    for (uint32_t i = 0; i < asset.geometry.count; ++i)
    {
        newSurfaces.emplace(asset.geometry.pData[i].key,
                            SurfaceBinding{ asset.geometry.pData[i].geometry.uniqueID,
                                            asset.geometry.pData[i].geometry.vertexCount });
        newSurfaceRuntimeIds.insert(asset.geometry.pData[i].geometry.uniqueID);
    }

    for (uint32_t i = 0; i < asset.lights.count; ++i)
    {
        const QraySceneLight &light = asset.lights.pData[i];
        const QrMaterial area = MaterialFor(newMaterials, light.areaMaterial);
        ownedLights.push_back(OwnLight(light, area));
        const uint64_t id = light.kind == QraySceneLightKind::Directional ? light.pDirectional->uniqueID :
            light.kind == QraySceneLightKind::Spherical ? light.pSpherical->uniqueID :
            light.kind == QraySceneLightKind::Spot ? light.pSpot->uniqueID :
            light.kind == QraySceneLightKind::Polygonal ? light.pPolygonal->uniqueID :
            light.pTexturedArea->uniqueID;
        newLights.emplace(light.key, id);
    }
    for (uint32_t i = 0; i < asset.dtalBatches.count; ++i)
        ownedDtal.push_back(OwnDtal(asset.dtalBatches.pData[i], newMaterials));

    for (const auto &[key, material] : m_impl->materials)
    {
        (void)key;
        qrDestroyMaterial(m_impl->instance, material);
    }

    m_impl->instance = instance;
    m_impl->sceneKey = asset.sceneKey;
    m_impl->fingerprint = asset.fingerprint;
    m_impl->bound = true;
    m_impl->poisoned = false;
    m_impl->materials = std::move(newMaterials);
    m_impl->surfaces = std::move(newSurfaces);
    m_impl->surfaceRuntimeIds = std::move(newSurfaceRuntimeIds);
    m_impl->lights = std::move(newLights);
    m_impl->sceneLights = std::move(ownedLights);
    m_impl->sceneDtal = std::move(ownedDtal);
    return QR_SUCCESS;
}

QrResult QraySceneBinding::RenderFrame(const QrayFrameView &view,
                                       const QrayFrameUpdates &updates) noexcept
{
    if (!m_impl->bound || m_impl->poisoned || m_impl->frameOpen)
        return QR_WRONG_FUNCTION_CALL;

    QrResult result = qrayValidateFrameView(&view);
    if (result != QR_SUCCESS)
        return result;
    result = qrayValidateFrameUpdates(&updates);
    if (result != QR_SUCCESS)
        return result;

    for (uint32_t i = 0; i < updates.transforms.count; ++i)
        if (m_impl->surfaces.find(updates.transforms.pData[i].geometry) == m_impl->surfaces.end())
            return QR_WRONG_ARGUMENT;
    for (uint32_t i = 0; i < updates.texCoords.count; ++i)
    {
        const auto it = m_impl->surfaces.find(updates.texCoords.pData[i].geometry);
        const QrayTexCoordUpdate &update = updates.texCoords.pData[i];
        if (it == m_impl->surfaces.end() ||
            update.vertexOffset > it->second.vertexCount ||
            update.vertexCount > it->second.vertexCount - update.vertexOffset)
            return QR_WRONG_ARGUMENT;
    }
    for (uint32_t i = 0; i < updates.materialContents.count; ++i)
        if (m_impl->materials.find(updates.materialContents.pData[i].material) == m_impl->materials.end())
            return QR_WRONG_ARGUMENT;

    const auto materialBound = [&](const QrayStableKey &key) {
        return key.value == 0 || m_impl->materials.find(key) != m_impl->materials.end();
    };

    std::unordered_set<QrayStableKey, KeyHash> dynamicKeys;
    std::unordered_set<uint64_t> dynamicIds;
    for (uint32_t i = 0; i < updates.dynamicGeometry.count; ++i)
    {
        const QraySceneGeometry &geometry = updates.dynamicGeometry.pData[i];
        if (!dynamicKeys.insert(geometry.key).second ||
            !dynamicIds.insert(geometry.geometry.uniqueID).second ||
            m_impl->surfaces.find(geometry.key) != m_impl->surfaces.end() ||
            m_impl->surfaceRuntimeIds.find(geometry.geometry.uniqueID) !=
                m_impl->surfaceRuntimeIds.end())
            return QR_WRONG_ARGUMENT;
        for (const QrayStableKey &key : geometry.materialKeys)
            if (!materialBound(key))
                return QR_WRONG_ARGUMENT;
    }
    for (const QrayRasterizedUpdate &update : updates.rasterized)
        if (!materialBound(update.material))
            return QR_WRONG_ARGUMENT;
    for (const QrayParticleUpdate &update : updates.particles)
        if (!materialBound(update.material))
            return QR_WRONG_ARGUMENT;
    for (const QrayDecalUpdate &update : updates.decals)
        if (!materialBound(update.material))
            return QR_WRONG_ARGUMENT;
    for (const QraySceneLight &light : updates.lights)
        if (!materialBound(light.areaMaterial))
            return QR_WRONG_ARGUMENT;
    for (const QraySceneDtalBatch &dtal : updates.dtalBatches)
    {
        for (uint32_t i = 0; i < dtal.groupCount; ++i)
            if (!materialBound(dtal.groupMaterialKeys.pData[i]))
                return QR_WRONG_ARGUMENT;
    }

    result = qrStartFrame(m_impl->instance, view.pStartInfo);
    if (result != QR_SUCCESS)
        return result;
    m_impl->frameOpen = true;

    QrResult firstError = QR_SUCCESS;
    auto apply = [&](QrResult callResult) {
        if (firstError == QR_SUCCESS && callResult != QR_SUCCESS)
            firstError = callResult;
    };

    if (updates.lights.count == 0)
        for (const OwnedLight &light : m_impl->sceneLights)
            apply(ApplyOwnedLight(m_impl->instance, light));
    if (updates.dtalBatches.count == 0)
        for (const OwnedDtal &dtal : m_impl->sceneDtal)
            apply(ApplyOwnedDtal(m_impl->instance, dtal));

    for (uint32_t i = 0; i < updates.transforms.count; ++i)
    {
        const QrayTransformUpdate &update = updates.transforms.pData[i];
        const QrUpdateTransformInfo info = {
            m_impl->surfaces.at(update.geometry).runtimeId,
            update.transform,
        };
        apply(qrUpdateGeometryTransform(m_impl->instance, &info));
    }
    for (uint32_t i = 0; i < updates.texCoords.count; ++i)
    {
        const QrayTexCoordUpdate &update = updates.texCoords.pData[i];
        const QrUpdateTexCoordsInfo info = {
            m_impl->surfaces.at(update.geometry).runtimeId,
            update.vertexOffset,
            update.vertexCount,
            { update.pTexCoordLayerData[0], update.pTexCoordLayerData[1], update.pTexCoordLayerData[2] },
        };
        apply(qrUpdateGeometryTexCoords(m_impl->instance, &info));
    }
    for (uint32_t i = 0; i < updates.materialContents.count; ++i)
    {
        const QrayMaterialContentUpdate &update = updates.materialContents.pData[i];
        const QrMaterialUpdateInfo info = {
            m_impl->materials.at(update.material),
            update.textures,
        };
        apply(qrUpdateMaterialContents(m_impl->instance, &info));
    }
    for (uint32_t i = 0; i < updates.dynamicGeometry.count; ++i)
    {
        const QraySceneGeometry &record = updates.dynamicGeometry.pData[i];
        QrGeometryUploadInfo upload = record.geometry;
        for (uint32_t layer = 0; layer < 3; ++layer)
            upload.geomMaterial.layerMaterials[layer] = MaterialFor(m_impl->materials, record.materialKeys[layer]);
        apply(qrUploadGeometry(m_impl->instance, &upload));
    }
    for (uint32_t i = 0; i < updates.rasterized.count; ++i)
    {
        const QrayRasterizedUpdate &update = updates.rasterized.pData[i];
        QrRasterizedGeometryUploadInfo upload = update.upload;
        upload.material = MaterialFor(m_impl->materials, update.material);
        apply(qrUploadRasterizedGeometry(m_impl->instance, &upload, update.pViewProjection,
                                         &update.viewport));
    }
    for (uint32_t i = 0; i < updates.particles.count; ++i)
    {
        const QrayParticleUpdate &update = updates.particles.pData[i];
        QrParticleUploadInfo upload = update.upload;
        upload.material = MaterialFor(m_impl->materials, update.material);
        apply(qrUploadParticles(m_impl->instance, &upload));
    }
    for (uint32_t i = 0; i < updates.decals.count; ++i)
    {
        const QrayDecalUpdate &update = updates.decals.pData[i];
        QrDecalUploadInfo upload = update.upload;
        upload.material = MaterialFor(m_impl->materials, update.material);
        apply(qrUploadDecal(m_impl->instance, &upload));
    }
    for (const QrPortalUploadInfo &portal : updates.portals)
        apply(qrUploadPortal(m_impl->instance, &portal));
    for (const QraySceneLight &light : updates.lights)
        apply(ApplyLight(m_impl->instance, light, m_impl->materials));
    for (const QraySceneDtalBatch &dtal : updates.dtalBatches)
        apply(ApplyDtal(m_impl->instance, dtal, m_impl->materials));
    if (updates.pClusterSources != nullptr)
        apply(qrUploadClusterLightSources(m_impl->instance, updates.pClusterSources));
    if (updates.fogVolumes.count != 0)
        apply(qrSetFogVolumes(m_impl->instance, updates.fogVolumes.count, updates.fogVolumes.pData));

    const QrResult drawResult = qrDrawFrame(m_impl->instance, view.pDrawInfo);
    m_impl->frameOpen = false;
    apply(drawResult);
    return firstError;
}

QrResult QraySceneBinding::RenderHostFrame(const QrayFrameView &view, IQrayHost &host) noexcept
{
    const QrayHostCapabilities caps = host.Capabilities();
    if (!HeaderValid<QrayHostCapabilities>(caps.header) ||
        (caps.flags & QRAY_HOST_CAP_REFRESH) == 0)
        return QR_FEATURE_UNAVAILABLE;

    QrayFrameUpdates updates = {};
    updates.header = { sizeof(QrayFrameUpdates), kHostContractVersion };
    const QrResult refreshResult = host.Refresh(view, &updates);
    if (refreshResult != QR_SUCCESS)
        return refreshResult;
    return RenderFrame(view, updates);
}

QrResult QraySceneBinding::ApplySidecar(IQrayHost &host, const QraySidecar &sidecar,
                                        QrayMutableArrayView<QraySidecarEntryResult> results,
                                        uint32_t *pCount) noexcept
{
    if (!m_impl->bound || pCount == nullptr)
        return QR_WRONG_ARGUMENT;
    QrResult result = qrayValidateSidecar(&sidecar);
    if (result != QR_SUCCESS)
        return result;
    if (!(sidecar.sceneKey == m_impl->sceneKey) ||
        sidecar.fingerprint.geometry != m_impl->fingerprint.geometry ||
        sidecar.fingerprint.dependencies != m_impl->fingerprint.dependencies ||
        sidecar.fingerprint.algorithm != m_impl->fingerprint.algorithm ||
        results.count < sidecar.entries.count ||
        (results.count != 0 && results.pData == nullptr))
        return QR_WRONG_ARGUMENT;

    const QrayHostCapabilities caps = host.Capabilities();
    if (!HeaderValid<QrayHostCapabilities>(caps.header) ||
        (caps.flags & QRAY_HOST_CAP_SIDECAR) == 0)
        return QR_FEATURE_UNAVAILABLE;
    return host.ApplySidecar(sidecar, results, pCount);
}

QrResult QraySceneBinding::ResolveSurface(const QrayStableKey &key, uint64_t *pRuntimeId) const noexcept
{
    if (pRuntimeId == nullptr)
        return QR_WRONG_ARGUMENT;
    const auto it = m_impl->surfaces.find(key);
    if (it == m_impl->surfaces.end())
        return QR_WRONG_ARGUMENT;
    *pRuntimeId = it->second.runtimeId;
    return QR_SUCCESS;
}

QrResult QraySceneBinding::ResolveMaterial(const QrayStableKey &key, QrMaterial *pMaterial) const noexcept
{
    if (pMaterial == nullptr)
        return QR_WRONG_ARGUMENT;
    const auto it = m_impl->materials.find(key);
    if (it == m_impl->materials.end())
        return QR_WRONG_ARGUMENT;
    *pMaterial = it->second;
    return QR_SUCCESS;
}

QrResult QraySceneBinding::ResolveLight(const QrayStableKey &key, uint64_t *pRuntimeId) const noexcept
{
    if (pRuntimeId == nullptr)
        return QR_WRONG_ARGUMENT;
    const auto it = m_impl->lights.find(key);
    if (it == m_impl->lights.end())
        return QR_WRONG_ARGUMENT;
    *pRuntimeId = it->second;
    return QR_SUCCESS;
}

void QraySceneBinding::Reset() noexcept
{
    m_impl = std::make_unique<Impl>();
}

bool QraySceneBinding::IsBound() const noexcept
{
    return m_impl->bound;
}

bool QraySceneBinding::IsPoisoned() const noexcept
{
    return m_impl->poisoned;
}

} // namespace qray
