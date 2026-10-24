// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#ifndef QRAY_HOST_CONTRACT_H_
#define QRAY_HOST_CONTRACT_H_

#include "qray/qray.h"

#include <cstdint>
#include <memory>

namespace qray
{

inline constexpr uint32_t kHostContractMajor = 1;
inline constexpr uint32_t kHostContractMinor = 0;
inline constexpr uint32_t kHostContractVersion =
    (kHostContractMajor << 16) | kHostContractMinor;

struct QrayContractHeader
{
    uint32_t size = 0;
    uint32_t version = 0;
};

template<typename T>
struct QrayArrayView
{
    const T *pData = nullptr;
    uint32_t count = 0;

    constexpr explicit operator bool() const noexcept { return count != 0; }
    constexpr const T *begin() const noexcept { return pData; }
    constexpr const T *end() const noexcept { return pData ? pData + count : pData; }
};

template<typename T>
struct QrayMutableArrayView
{
    T *pData = nullptr;
    uint32_t count = 0;

    constexpr explicit operator bool() const noexcept { return count != 0; }
    constexpr T *begin() const noexcept { return pData; }
    constexpr T *end() const noexcept { return pData ? pData + count : pData; }
};

enum QrayStableKeyDomain : uint32_t
{
    QRAY_KEY_SCENE = 1,
    QRAY_KEY_MATERIAL,
    QRAY_KEY_SURFACE,
    QRAY_KEY_LIGHT,
    QRAY_KEY_DTAL,
};

struct QrayStableKey
{
    uint64_t value = 0;
    uint32_t domain = 0;
    uint32_t reserved = 0;

    friend constexpr bool operator==(const QrayStableKey &a, const QrayStableKey &b) noexcept
    {
        return a.value == b.value && a.domain == b.domain && a.reserved == b.reserved;
    }
};

struct QrayAuthoringUUID
{
    uint64_t high = 0;
    uint64_t low = 0;

    friend constexpr bool operator==(const QrayAuthoringUUID &a, const QrayAuthoringUUID &b) noexcept
    {
        return a.high == b.high && a.low == b.low;
    }
};

struct QraySceneFingerprint
{
    uint64_t geometry = 0;
    uint64_t dependencies = 0;
    uint32_t algorithm = 0;
    uint32_t reserved = 0;
};

struct QraySceneMaterial
{
    QrayStableKey key;
    const QrMaterialCreateInfo *pCreateInfo = nullptr;
    const QrAnimatedMaterialCreateInfo *pAnimatedCreateInfo = nullptr;
};

struct QraySceneGeometry
{
    QrayStableKey key;
    QrayStableKey materialKeys[3];
    QrGeometryUploadInfo geometry = {};
};

enum class QraySceneLightKind : uint32_t
{
    Directional = 0,
    Spherical,
    Spot,
    Polygonal,
    TexturedArea,
};

struct QraySceneLight
{
    QrayStableKey key;
    QrayStableKey areaMaterial;

    QraySceneLightKind kind = QraySceneLightKind::Spherical;
    const QrDirectionalLightUploadInfo *pDirectional = nullptr;
    const QrSphericalLightUploadInfo *pSpherical = nullptr;
    const QrSpotLightUploadInfo *pSpot = nullptr;
    const QrPolygonalLightUploadInfo *pPolygonal = nullptr;
    const QrTexturedAreaLightUploadInfo *pTexturedArea = nullptr;
};

struct QraySceneDtalBatch
{
    const QrDtalGroupUploadInfo *pGroups = nullptr;
    uint32_t groupCount = 0;
    const QrDtalMemberUpload *pMembers = nullptr;
    uint32_t memberCount = 0;
    QrayArrayView<QrayStableKey> groupMaterialKeys;
};

struct QraySceneWorldLights
{
    const QrWorldLightsUploadInfo *pUpload = nullptr;
    QrayArrayView<QrayStableKey> faceMaterialKeys;
};

struct QraySceneAsset
{
    QrayContractHeader header;
    QrayStableKey sceneKey;
    QraySceneFingerprint fingerprint;

    QrayArrayView<QraySceneMaterial> materials;
    QrayArrayView<QraySceneGeometry> geometry;
    QrayArrayView<QraySceneLight> lights;
    QrayArrayView<QraySceneDtalBatch> dtalBatches;
    QraySceneWorldLights worldLights;
    QrayArrayView<QrFogVolume> fogVolumes;
};

enum QrayFrameViewFlagBits : uint32_t
{
    QRAY_FRAME_VIEW_RESET_HISTORY = 1u << 0,
};

struct QrayFrameView
{
    QrayContractHeader header;
    uint64_t viewId = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;

    const QrStartFrameInfo *pStartInfo = nullptr;
    const QrDrawFrameInfo *pDrawInfo = nullptr;
};

struct QrayTransformUpdate
{
    QrayStableKey geometry;
    QrTransform transform = {};
};

struct QrayTexCoordUpdate
{
    QrayStableKey geometry;
    uint32_t vertexOffset = 0;
    uint32_t vertexCount = 0;
    const void *pTexCoordLayerData[3] = {};
};

struct QrayMaterialContentUpdate
{
    QrayStableKey material;
    QrTextureSet textures = {};
};

struct QrayRasterizedUpdate
{
    QrayStableKey material;
    QrRasterizedGeometryUploadInfo upload = {};
    const float *pViewProjection = nullptr;
    QrViewport viewport = {};
};

struct QrayParticleUpdate
{
    QrayStableKey material;
    QrParticleUploadInfo upload = {};
};

struct QrayDecalUpdate
{
    QrayStableKey material;
    QrDecalUploadInfo upload = {};
};

struct QrayFrameUpdates
{
    QrayContractHeader header;

    QrayArrayView<QrayTransformUpdate> transforms;
    QrayArrayView<QrayTexCoordUpdate> texCoords;
    QrayArrayView<QrayMaterialContentUpdate> materialContents;
    QrayArrayView<QraySceneGeometry> dynamicGeometry;
    QrayArrayView<QrayRasterizedUpdate> rasterized;
    QrayArrayView<QrayParticleUpdate> particles;
    QrayArrayView<QrayDecalUpdate> decals;
    QrayArrayView<QrPortalUploadInfo> portals;
    QrayArrayView<QraySceneLight> lights;
    QrayArrayView<QraySceneDtalBatch> dtalBatches;
    const QrClusterLightSourcesUploadInfo *pClusterSources = nullptr;
    QrayArrayView<QrFogVolume> fogVolumes;
};

enum QrayMaterialIntentFlagBits : uint32_t
{
    QRAY_MATERIAL_INTENT_ROUGHNESS = 1u << 0,
    QRAY_MATERIAL_INTENT_METALLIC = 1u << 1,
    QRAY_MATERIAL_INTENT_EMISSION = 1u << 2,
    QRAY_MATERIAL_INTENT_LAYER_COLORS = 1u << 3,
    QRAY_MATERIAL_INTENT_ALBEDO_TEXTURE = 1u << 4,
    QRAY_MATERIAL_INTENT_RME_TEXTURE = 1u << 5,
    QRAY_MATERIAL_INTENT_NORMAL_TEXTURE = 1u << 6,
};

struct QrayMaterialIntent
{
    QrayStableKey surface;
    uint32_t flags = 0;
    float roughness = 0.0f;
    float metallic = 0.0f;
    float emission = 0.0f;
    QrFloat4D layerColors[3] = {};
    const char *pAlbedoTexture = nullptr;
    const char *pRoughnessMetallicEmissionTexture = nullptr;
    const char *pNormalTexture = nullptr;
};

enum QrayLightIntentFlagBits : uint32_t
{
    QRAY_LIGHT_INTENT_COLOR = 1u << 0,
    QRAY_LIGHT_INTENT_POSITION = 1u << 1,
    QRAY_LIGHT_INTENT_DIRECTION = 1u << 2,
    QRAY_LIGHT_INTENT_RADIUS = 1u << 3,
    QRAY_LIGHT_INTENT_CONE = 1u << 4,
    QRAY_LIGHT_INTENT_POWER = 1u << 5,
};

struct QrayLightIntent
{
    QrayStableKey light;
    QrayStableKey material;
    QraySceneLightKind kind = QraySceneLightKind::Spherical;
    uint32_t flags = 0;

    QrFloat3D color = {};
    QrFloat3D position = {};
    QrFloat3D direction = {};
    float radius = 0.0f;
    float angleInner = 0.0f;
    float angleOuter = 0.0f;
    float power = 0.0f;
};

enum QrayDtalIntentFlagBits : uint32_t
{
    QRAY_DTAL_INTENT_MATERIAL = 1u << 0,
    QRAY_DTAL_INTENT_COLOR = 1u << 1,
    QRAY_DTAL_INTENT_REACH = 1u << 2,
    QRAY_DTAL_INTENT_POWER = 1u << 3,
};

struct QrayDtalIntent
{
    QrayStableKey group;
    QrayStableKey material;
    uint32_t flags = 0;
    QrFloat3D color = {};
    float reach = 0.0f;
    float estimatedPower = 0.0f;
};

enum class QraySidecarEntryKind : uint32_t
{
    Material = 0,
    Light,
    DtalGroup,
};

struct QraySidecarEntry
{
    QrayAuthoringUUID authoringId;
    QraySidecarEntryKind kind = QraySidecarEntryKind::Material;
    uint32_t flags = 0;
    QrayMaterialIntent material;
    QrayLightIntent light;
    QrayDtalIntent dtal;
};

struct QraySidecar
{
    QrayContractHeader header;
    uint32_t schemaVersion = 1;
    uint32_t reserved = 0;
    QrayStableKey sceneKey;
    QraySceneFingerprint fingerprint;
    QrayArrayView<QraySidecarEntry> entries;
};

enum class QraySidecarEntryStatus : uint32_t
{
    Applied = 0,
    Rebound,
    Unchanged,
    Orphaned,
    Ambiguous,
    Unsupported,
    Invalid,
};

struct QraySidecarEntryResult
{
    QrayAuthoringUUID authoringId;
    QraySidecarEntryStatus status = QraySidecarEntryStatus::Invalid;
    QrResult result = QR_SUCCESS;
};

enum QrayHostCapabilityBits : uint64_t
{
    QRAY_HOST_CAP_SURFACE_KEYS = 1ull << 0,
    QRAY_HOST_CAP_MATERIAL_KEYS = 1ull << 1,
    QRAY_HOST_CAP_LIGHT_KEYS = 1ull << 2,
    QRAY_HOST_CAP_REFRESH = 1ull << 3,
    QRAY_HOST_CAP_STYLE_EVALUATION = 1ull << 4,
    QRAY_HOST_CAP_SIDECAR = 1ull << 5,
    QRAY_HOST_CAP_AUTHORED_LIGHTS = 1ull << 6,
    QRAY_HOST_CAP_PICK = 1ull << 7,
};

struct QrayHostCapabilities
{
    QrayContractHeader header;
    uint64_t flags = 0;
};

struct QrayPickRequest
{
    QrFloat3D origin = {};
    QrFloat3D direction = {};
    float maxDistance = 0.0f;
    uint32_t reserved = 0;
};

struct QrayPickResult
{
    QrBool32 hit = QR_FALSE;
    uint32_t reserved = 0;
    QrayStableKey target;
    QrFloat3D position = {};
    QrFloat3D normal = {};
    float distance = 0.0f;
};

class IQrayHost
{
public:
    virtual ~IQrayHost() = default;

    virtual QrayHostCapabilities Capabilities() const noexcept = 0;

    virtual QrResult Attach(QrInstance instance) noexcept;
    virtual void Detach() noexcept;

    virtual QrResult SurfaceKey(uint64_t runtimeId, QrayStableKey *pResult) const noexcept;
    virtual QrResult ResolveSurface(const QrayStableKey &key, uint64_t *pRuntimeId) const noexcept;
    virtual QrResult ResolveMaterial(const QrayStableKey &key, QrMaterial *pMaterial) const noexcept;
    virtual QrResult LightKey(uint64_t runtimeId, QrayStableKey *pResult) const noexcept;
    virtual QrResult Refresh(const QrayFrameView &view, QrayFrameUpdates *pUpdates) noexcept;
    virtual QrResult EvaluateStyles(double time, QrayArrayView<const uint8_t> styles,
                                    QrayMutableArrayView<float> scales) noexcept;
    virtual QrResult ApplySidecar(const QraySidecar &sidecar,
                                  QrayMutableArrayView<QraySidecarEntryResult> results,
                                  uint32_t *pCount) noexcept;
    virtual QrResult LoadAuthoredLights(QrayMutableArrayView<QraySceneLight> lights,
                                        uint32_t *pCount) noexcept;
    virtual QrResult Pick(const QrayPickRequest &request, QrayPickResult *pResult) noexcept;
};

class QraySceneBinding
{
public:
    QraySceneBinding() noexcept;
    ~QraySceneBinding();

    QraySceneBinding(QraySceneBinding &&) noexcept;
    QraySceneBinding &operator=(QraySceneBinding &&) noexcept;

    QraySceneBinding(const QraySceneBinding &) = delete;
    QraySceneBinding &operator=(const QraySceneBinding &) = delete;

    QrResult Bind(QrInstance instance, const QraySceneAsset &asset) noexcept;
    QrResult RenderFrame(const QrayFrameView &view, const QrayFrameUpdates &updates) noexcept;
    QrResult RenderHostFrame(const QrayFrameView &view, IQrayHost &host) noexcept;
    QrResult ApplySidecar(IQrayHost &host, const QraySidecar &sidecar,
                          QrayMutableArrayView<QraySidecarEntryResult> results,
                          uint32_t *pCount) noexcept;
    QrResult ResolveSurface(const QrayStableKey &key, uint64_t *pRuntimeId) const noexcept;
    QrResult ResolveMaterial(const QrayStableKey &key, QrMaterial *pMaterial) const noexcept;
    QrResult ResolveLight(const QrayStableKey &key, uint64_t *pRuntimeId) const noexcept;

    void Reset() noexcept;
    bool IsBound() const noexcept;
    bool IsPoisoned() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

struct QrayContractDiagnostic
{
    QrResult result = QR_SUCCESS;
    uint32_t index = UINT32_MAX;
    uint32_t code = 0;
};

uint32_t qrayGetHostContractVersion() noexcept;
QrResult qrayValidateSceneAsset(const QraySceneAsset *pAsset,
                                QrayContractDiagnostic *pDiagnostic = nullptr) noexcept;
QrResult qrayValidateFrameView(const QrayFrameView *pView,
                               QrayContractDiagnostic *pDiagnostic = nullptr) noexcept;
QrResult qrayValidateFrameUpdates(const QrayFrameUpdates *pUpdates,
                                  QrayContractDiagnostic *pDiagnostic = nullptr) noexcept;
QrResult qrayValidateSidecar(const QraySidecar *pSidecar,
                             QrayContractDiagnostic *pDiagnostic = nullptr) noexcept;
const char *qrayGetSidecarEntryStatusName(QraySidecarEntryStatus status) noexcept;

} // namespace qray

#endif
