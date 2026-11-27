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

#include "EditorSceneRuntime.h"

#include "EditorLightSelection.h"
#include "EditorSpatial.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace
{

constexpr float kRuntimePi = 3.14159265358979323846f;

std::string LowerAsciiRuntime(std::string_view text)
{
    std::string out(text);
    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

void EmitLog(const EditorRuntimeLogFn &log, const std::string &msg)
{
    if (log)
    {
        log(msg.c_str());
    }
}

void BuildFullbrightRmeAndEmission(const EditorTexture &texture, std::vector<uint8_t> &outRme,
                                   EditorMaterialEmission &outEmission)
{
    const size_t pixels = size_t(texture.width) * size_t(texture.height);
    const uint8_t defaultRough = uint8_t(kEditorDefaultRoughness * 255.0f);
    outRme.resize(pixels * 4u);

    double emissR = 0.0;
    double emissG = 0.0;
    double emissB = 0.0;
    double emissMean = 0.0;
    int minX = int(texture.width);
    int minY = int(texture.height);
    int maxX = -1;
    int maxY = -1;

    for (uint32_t y = 0; y < texture.height; y++)
    {
        for (uint32_t x = 0; x < texture.width; x++)
        {
            const size_t p = size_t(y) * size_t(texture.width) + size_t(x);
            const uint8_t fb = texture.fullbright[p];
            const size_t offset = p * 4u;
            outRme[offset + 0] = defaultRough;
            outRme[offset + 1] = 0u;
            outRme[offset + 2] = fb;
            outRme[offset + 3] = 0u;

            if (fb > 0u)
            {
                const float e = float(fb) / 255.0f;
                emissR += texture.rgba[offset + 0] * e;
                emissG += texture.rgba[offset + 1] * e;
                emissB += texture.rgba[offset + 2] * e;
                emissMean += e;
                if (e > kEditorGlowThreshold)
                {
                    minX = std::min(minX, int(x));
                    maxX = std::max(maxX, int(x));
                    minY = std::min(minY, int(y));
                    maxY = std::max(maxY, int(y));
                }
            }
        }
    }

    outEmission = {};
    outEmission.valid = true;
    outEmission.mean = float(emissMean / double(pixels));
    outEmission.meanBase = outEmission.mean;
    if (outEmission.meanBase > 1e-6f)
    {
        const double scale = 1.0 / (double(pixels) * 255.0 * double(outEmission.meanBase));
        outEmission.color[0] = float(emissR * scale);
        outEmission.color[1] = float(emissG * scale);
        outEmission.color[2] = float(emissB * scale);
    }
    FinalizeEditorGlowExtents(minX, minY, maxX, maxY, texture.width, texture.height, outEmission);
}

int32_t FindTextureIndexByKey(const std::unordered_map<std::string, int32_t> &indexByName,
                              const std::string &key)
{
    static constexpr const char kPrefix[] = "textures/";
    const std::string base = key.rfind(kPrefix, 0) == 0 ? key.substr(sizeof(kPrefix) - 1) : key;

    std::string candidates[4] = { key, base, std::string(), std::string() };
    if (!base.empty() && (base[0] == '*' || base[0] == '#'))
    {
        candidates[2] = base;
        candidates[2][0] = base[0] == '*' ? '#' : '*';
    }
    if (base.size() >= 2 && base[0] == '+' && (base[1] < '0' || base[1] > '9'))
    {
        candidates[3] = "+0" + base.substr(1);
    }

    for (const std::string &candidate : candidates)
    {
        if (candidate.empty())
        {
            continue;
        }
        const auto found = indexByName.find(candidate);
        if (found != indexByName.end())
        {
            return found->second;
        }
    }
    return -1;
}

bool LoadRuntimeImagePixels(const EditorAssetSource &assets, const std::string &key,
                            uint32_t &outWidth, uint32_t &outHeight, std::vector<uint8_t> &outRgba)
{
    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
        adapter != nullptr && adapter->loadExternalImageRgba != nullptr)
    {
        std::string resolved;
        if (adapter->loadExternalImageRgba(&assets, key, outRgba, outWidth, outHeight, resolved))
        {
            return true;
        }
    }
    return LoadEditorExternalImageRgba(assets, key, outWidth, outHeight, outRgba);
}

void BuildDocumentNameIndex(const EditorMaterialsDocument *doc,
                            std::unordered_map<std::string, std::string> &exact,
                            std::unordered_map<std::string, std::string> &stem)
{
    exact.clear();
    stem.clear();
    if (doc == nullptr)
    {
        return;
    }
    for (const std::string &name : doc->MaterialNames())
    {
        const std::string norm = NormalizeEditorAssetPath(name);
        const std::string stripped = StripEditorImageExtension(norm);
        exact.emplace(norm, name);
        stem.emplace(stripped, name);
    }
}

bool ReadDocumentMaterialNormalized(
    const EditorMaterialsDocument *doc,
    const std::unordered_map<std::string, std::string> &exactIndex,
    const std::unordered_map<std::string, std::string> &stemIndex, const std::string &queryName,
    EditorMaterialParams &ioParams)
{
    if (doc == nullptr)
    {
        return false;
    }
    if (doc->ReadMaterial(queryName, ioParams))
    {
        return true;
    }
    const std::string norm = NormalizeEditorAssetPath(queryName);
    if (const auto it = exactIndex.find(norm); it != exactIndex.end())
    {
        return doc->ReadMaterial(it->second, ioParams);
    }
    const std::string stripped = StripEditorImageExtension(norm);
    if (const auto it = stemIndex.find(stripped); it != stemIndex.end())
    {
        return doc->ReadMaterial(it->second, ioParams);
    }
    return false;
}

std::vector<EditorLight> CollectLeadingBspLights(const std::vector<EditorLight> &lights)
{
    std::vector<EditorLight> bsp;
    for (const EditorLight &l : lights)
    {
        if (l.emitterUniqueID != 0)
        {
            break;
        }
        bsp.push_back(l);
    }
    return bsp;
}

EditorLight EditorLightFromLevelEntry(const EditorScene &scene, const EditorLevelLight &entry,
                                      const std::string &defaultClassName)
{
    EditorLight light;
    for (int axis = 0; axis < 3; axis++)
    {
        light.position[axis] = entry.origin[axis];
        light.offset[axis] = entry.offset[axis];
        light.direction[axis] = entry.direction[axis];
    }
    light.hasOffset = entry.hasOffset;
    light.radius = entry.radius * kEditorUnitsPerMeter;
    light.spot = entry.spot;
    light.angleInner = entry.angleInner;
    light.angleOuter = entry.angleOuter;
    light.lightShafts = entry.lightShafts;
    light.lightShaftsPower = entry.lightShaftsPower;
    light.lightShaftsDistance = entry.lightShaftsDistance;
    light.lightShaftsFalloff = entry.lightShaftsFalloff;
    light.style = entry.style;
    light.forceRasterize = entry.forceRasterize;
    light.enabled = entry.enabled;
    light.className = entry.className.empty() ? defaultClassName : entry.className;
    light.hasColor = true;
    ResolveLightClusterFromEffectivePos(scene, light);

    const float tint[3] = { entry.color[0], entry.color[1], entry.color[2] };
    SetEditorLightTint(light, tint);
    SetEditorLightIntensity(light, entry.intensity * kEditorCustomLightIntensityUnits);
    ClampEditorLightCone(light);
    return light;
}

void UploadCutoutBackingGeometry(QrInstance instance, QrMaterial backingMaterial,
                                 size_t surfaceIndex, const EditorSurface &surface)
{
    if (backingMaterial == QR_NO_MATERIAL)
    {
        return;
    }

    std::vector<QrVertex> backingVertices = surface.vertices;
    OffsetEditorVerticesAlongSmoothNormals(backingVertices, surface.indices, -1.6f);
    std::vector<QrVertex> backingMeterVertices;

    QrGeometryUploadInfo backingUpload = {};
    backingUpload.uniqueID = 0x100000000ull + uint64_t(surfaceIndex) + 1u;
    backingUpload.flags =
        (surface.hasAuthoredNormals ? 0u : uint32_t(QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT)) |
        uint32_t(QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT);
    backingUpload.geomType = QR_GEOMETRY_TYPE_STATIC;
    backingUpload.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    backingUpload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    backingUpload.vertexCount = uint32_t(backingVertices.size());
    backingUpload.pVertices = backingVertices.data();
    backingUpload.indexCount = uint32_t(surface.indices.size());
    backingUpload.pIndices = surface.indices.data();
    backingUpload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
    backingUpload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    backingUpload.texCoordScale[0] = 1.0f;
    backingUpload.texCoordScale[1] = 1.0f;
    backingUpload.defaultRoughness = 0.9f;
    backingUpload.geomMaterial.layerMaterials[0] = backingMaterial;
    backingUpload.transform.matrix[0][0] = 1.0f;
    backingUpload.transform.matrix[1][1] = 1.0f;
    backingUpload.transform.matrix[2][2] = 1.0f;
    EditorMetersGeometry(backingUpload, backingMeterVertices);
    qrUploadGeometry(instance, &backingUpload);
}

bool IsSurfaceBaseStageDisabled(const EditorScene &scene, const EditorSurface &surface,
                                const EditorMaterialParams &params)
{
    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (tex == nullptr || tex->baseStageIndex >= tex->shaderStages.size() ||
        tex->baseStageIndex >= 32u)
    {
        return false;
    }
    return ((params.disabledStagesMask >> tex->baseStageIndex) & 1u) != 0u;
}

bool HasEnabledSurfaceUnderlayStage(const EditorScene &scene, const EditorSurface &surface,
                                    const EditorMaterialParams &params)
{
    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (tex == nullptr || tex->baseStageIndex == 0u ||
        tex->baseStageIndex > tex->shaderStages.size())
    {
        return false;
    }
    for (size_t i = 0; i < tex->baseStageIndex; i++)
    {
        if (!tex->shaderStages[i].isLightmap &&
            (i >= 32u || ((params.disabledStagesMask >> i) & 1u) == 0u))
        {
            return true;
        }
    }
    return false;
}

bool ShouldUploadStaticSurface(const EditorScene &scene, const EditorSurface &surface,
                               const EditorMaterialParams &params)
{
    if (surface.vertices.empty() || surface.indices.empty() || !params.enabled)
    {
        return false;
    }
    if (surface.isOverlayOnly && !EditorMaterialPromotesOverlayToTraced(params))
    {
        return false;
    }
    return !IsSurfaceBaseStageDisabled(scene, surface, params);
}

void AttachWorldClusterVis(const EditorWorldClusters &clusters, QrWorldLightsUploadInfo &upload)
{
    if (!clusters.visData.empty() && clusters.visOffsets.size() == upload.numClusters &&
        clusters.pvsRowBytes > 0)
    {
        upload.pVisData = clusters.visData.data();
        upload.visDataSize = uint32_t(clusters.visData.size());
        upload.pvsRowBytes = clusters.pvsRowBytes;
        upload.pVisOffsets = clusters.visOffsets.data();
    }
    if (!clusters.skyVis.empty())
    {
        upload.pClusterSkyVisibility = clusters.skyVis.data();
    }
}

void LogSurfaceClusterSummary(const EditorScene &scene, uint32_t numClusters,
                              const EditorRuntimeLogFn &log)
{
    if (!log)
    {
        return;
    }
    size_t unclustered = 0;
    size_t distinct = 0;
    uint32_t highest = 0;
    std::vector<uint8_t> seen(numClusters, 0);
    for (const QrVertex &vertex : scene.vertices)
    {
        if (vertex.cluster == 0u)
        {
            unclustered++;
            continue;
        }
        if (vertex.cluster < numClusters && seen[vertex.cluster] == 0u)
        {
            seen[vertex.cluster] = 1u;
            distinct++;
        }
        highest = std::max(highest, vertex.cluster);
    }
    EmitLog(log, "surface clusters: " + std::to_string(scene.vertices.size() - unclustered) + "/" +
                     std::to_string(scene.vertices.size()) + " vertices clustered, " +
                     std::to_string(distinct) + " distinct, max " + std::to_string(highest));
}

bool LoadSkyboxFaceImage(const EditorAssetSource &assets, const std::string &baseKey,
                         const char *suffix, uint32_t &outWidth, uint32_t &outHeight,
                         std::vector<uint8_t> &outRgba)
{
    if (LoadEditorExternalImageRgba(assets, baseKey + suffix, outWidth, outHeight, outRgba))
    {
        return true;
    }
    if (!baseKey.empty() && baseKey.back() != '_')
    {
        return LoadEditorExternalImageRgba(assets, baseKey + "_" + suffix, outWidth, outHeight,
                                           outRgba);
    }
    return false;
}

bool LoadSkyboxFaceWithFallback(const EditorAssetSource &assets, const std::string &baseKey,
                                const std::string &altKey, const char *suffix, uint32_t &outWidth,
                                uint32_t &outHeight, std::vector<uint8_t> &outRgba)
{
    if (LoadSkyboxFaceImage(assets, baseKey, suffix, outWidth, outHeight, outRgba))
    {
        return outWidth > 0 && outHeight > 0 &&
               outRgba.size() >= size_t(outWidth) * size_t(outHeight) * 4u;
    }
    if (baseKey != altKey &&
        LoadSkyboxFaceImage(assets, altKey, suffix, outWidth, outHeight, outRgba))
    {
        return outWidth > 0 && outHeight > 0 &&
               outRgba.size() >= size_t(outWidth) * size_t(outHeight) * 4u;
    }
    return false;
}

struct SkyboxFacePixels
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

bool ResolveSkyboxFaceRgba(const EditorAssetSource &assets, const EditorScene &scene,
                           const std::string &baseKey, const std::string &q1Key, int faceIdx,
                           SkyboxFacePixels &outFace)
{
    static constexpr const char *kSkyboxSuffixes[6] = { "rt", "bk", "lf", "ft", "up", "dn" };
    if (!scene.skyboxName.empty() &&
        LoadSkyboxFaceWithFallback(assets, baseKey, q1Key, kSkyboxSuffixes[faceIdx], outFace.width,
                                   outFace.height, outFace.rgba))
    {
        return true;
    }
    if (scene.generatedSkyboxSize == 0u)
    {
        return false;
    }
    const size_t expectedBytes =
        size_t(scene.generatedSkyboxSize) * size_t(scene.generatedSkyboxSize) * 4u;
    if (scene.generatedSkyboxFaces[size_t(faceIdx)].size() < expectedBytes)
    {
        return false;
    }
    outFace.width = scene.generatedSkyboxSize;
    outFace.height = scene.generatedSkyboxSize;
    outFace.rgba = scene.generatedSkyboxFaces[size_t(faceIdx)];
    return true;
}

void AccumulateSkyboxRgb(const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height,
                         double rgbSum[3], uint64_t &pixelCount)
{
    const size_t pixels = size_t(width) * size_t(height);
    for (size_t p = 0; p < pixels; p++)
    {
        rgbSum[0] += rgba[p * 4 + 0];
        rgbSum[1] += rgba[p * 4 + 1];
        rgbSum[2] += rgba[p * 4 + 2];
    }
    pixelCount += pixels;
}

struct ScrollingSkyContext
{
    const float *origin = nullptr;
    double timeSeconds = 0.0;
    float halfExtent = 0.0f;
};

void ComputeSkyBoxVertex(float s, float t, int axis, const ScrollingSkyContext &ctx,
                         float outPos[3])
{
    static constexpr int kStToVec[6][3] = {
        { 3, -1, 2 },  { -3, 1, 2 },  { 1, 3, 2 },
        { -1, -3, 2 }, { -2, -1, 3 }, { 2, -1, -3 },
    };
    const float b[3] = { s * ctx.halfExtent, t * ctx.halfExtent, ctx.halfExtent };
    for (int j = 0; j < 3; j++)
    {
        const int k = kStToVec[axis][j];
        const int axisIdx = std::clamp((k < 0 ? -k : k) - 1, 0, 2);
        const float component = k < 0 ? -b[axisIdx] : b[axisIdx];
        outPos[j] = component + ctx.origin[j];
    }
}

void MakeScrollingSkyVertex(const float posQuake[3], const ScrollingSkyContext &ctx, float speed,
                            QrVertex &outVertex)
{
    const float dir[3] = {
        posQuake[0] - ctx.origin[0],
        posQuake[1] - ctx.origin[1],
        (posQuake[2] - ctx.origin[2]) * 3.0f,
    };
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float scale = len > 1e-6f ? (6.0f * 63.0f) / len : 0.0f;
    const float scroll = float(std::fmod(ctx.timeSeconds * double(speed), 128.0));

    outVertex = {};
    outVertex.position[0] = EditorMetersLength(posQuake[0]);
    outVertex.position[1] = EditorMetersLength(posQuake[1]);
    outVertex.position[2] = EditorMetersLength(posQuake[2]);
    outVertex.texCoord[0] = (scroll + dir[0] * scale) * (1.0f / 128.0f);
    outVertex.texCoord[1] = (scroll + dir[1] * scale) * (1.0f / 128.0f);
    outVertex.packedColor = 0xFFFFFFFFu;
}

void AppendScrollingSkyFace(int axis, const ScrollingSkyContext &ctx,
                            std::vector<QrVertex> &solidVerts, std::vector<QrVertex> &alphaVerts)
{
    static constexpr uint32_t kQuadTri[6] = { 0, 1, 2, 0, 2, 3 };
    const int di = 4;
    const int dj = axis < 4 ? di * 2 : di;
    for (int i = 0; i < di; i++)
    {
        const float s0 = -1.0f + 2.0f * float(i) / float(di);
        const float s1 = -1.0f + 2.0f * float(i + 1) / float(di);
        for (int j = 0; j < dj; j++)
        {
            const float t0 = -1.0f + 2.0f * float(j) / float(dj);
            const float t1 = -1.0f + 2.0f * float(j + 1) / float(dj);
            float corners[4][3];
            ComputeSkyBoxVertex(s0, t0, axis, ctx, corners[0]);
            ComputeSkyBoxVertex(s0, t1, axis, ctx, corners[1]);
            ComputeSkyBoxVertex(s1, t1, axis, ctx, corners[2]);
            ComputeSkyBoxVertex(s1, t0, axis, ctx, corners[3]);
            for (uint32_t corner : kQuadTri)
            {
                QrVertex solid = {};
                MakeScrollingSkyVertex(corners[corner], ctx, 8.0f, solid);
                solidVerts.push_back(solid);
                QrVertex alpha = {};
                MakeScrollingSkyVertex(corners[corner], ctx, 16.0f, alpha);
                alphaVerts.push_back(alpha);
            }
        }
    }
}

void UploadSkyboxGeometry(QrInstance instance, const ScrollingSkyContext &ctx,
                          const QrMaterial materials[6], const uint32_t widths[6],
                          const uint32_t heights[6], const QrFloat4D &skyColor)
{
    static constexpr int kSkyTexOrder[6] = { 0, 2, 1, 3, 4, 5 };
    static constexpr float kCornerST[4][2] = {
        { -1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, -1.0f },
    };
    static constexpr uint32_t kQuadTri[6] = { 0, 1, 2, 0, 2, 3 };

    QrRasterizedGeometryUploadInfo upload = {};
    upload.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY;
    upload.transform.matrix[0][0] = 1.0f;
    upload.transform.matrix[1][1] = 1.0f;
    upload.transform.matrix[2][2] = 1.0f;
    upload.color = skyColor;

    for (int axis = 0; axis < 6; axis++)
    {
        const int face = kSkyTexOrder[axis];
        if (materials[face] == QR_NO_MATERIAL || widths[face] == 0u || heights[face] == 0u)
        {
            continue;
        }
        const float w = float(widths[face]);
        const float h = float(heights[face]);
        QrVertex corners[4] = {};
        for (int c = 0; c < 4; c++)
        {
            const float s = kCornerST[c][0];
            const float t = kCornerST[c][1];
            float posQuake[3] = {};
            ComputeSkyBoxVertex(s, t, axis, ctx, posQuake);
            float u = (s + 1.0f) * 0.5f;
            float v = (t + 1.0f) * 0.5f;
            u = u * (w - 1.0f) / w + 0.5f / w;
            v = 1.0f - (v * (h - 1.0f) / h + 0.5f / h);

            corners[c].position[0] = EditorMetersLength(posQuake[0]);
            corners[c].position[1] = EditorMetersLength(posQuake[1]);
            corners[c].position[2] = EditorMetersLength(posQuake[2]);
            corners[c].texCoord[0] = u;
            corners[c].texCoord[1] = v;
            corners[c].packedColor = 0xFFFFFFFFu;
        }
        QrVertex faceVerts[6] = {};
        for (int i = 0; i < 6; i++)
        {
            faceVerts[i] = corners[kQuadTri[i]];
        }
        upload.vertexCount = 6;
        upload.pVertices = faceVerts;
        upload.material = materials[face];
        qrUploadRasterizedGeometry(instance, &upload, nullptr, nullptr);
    }
}

void FillEditorLightClusterSource(const EditorLight &light, uint64_t uniqueID, float reach,
                                  QrClusterLightSource &out)
{
    const float px = light.position[0] + (light.hasOffset ? light.offset[0] : 0.0f);
    const float py = light.position[1] + (light.hasOffset ? light.offset[1] : 0.0f);
    const float pz = light.position[2] + (light.hasOffset ? light.offset[2] : 0.0f);
    const float lum =
        0.2125f * light.color[0] + 0.7154f * light.color[1] + 0.0721f * light.color[2];
    const float effRadius = std::max(light.radius, 8.0f);
    out = {};
    out.uniqueID = uniqueID;
    out.origin = { px, py, pz };
    out.cluster = light.cluster;
    out.reach = reach;
    out.radius = effRadius;
    // LightManager::EncodeAsSphereLight divides uploaded color by pi * r^2 after
    // EditorMetersSphericalLight divides by kEditorUnitsPerMeter^2, so lum(light.color) is already
    // in the same (1600 * meter-flux) units as EditorDtalPipeline's area_qu * meanEmiss * lum(color).
    out.power = std::max(0.0f, lum);
}

void AppendPointLightClusterSources(const std::vector<EditorLight> &lights,
                                    uint64_t uniqueIdBase, float reach,
                                    std::vector<QrClusterLightSource> &sources)
{
    for (size_t i = 0; i < lights.size(); i++)
    {
        if (!lights[i].enabled)
        {
            continue;
        }
        QrClusterLightSource source = {};
        FillEditorLightClusterSource(lights[i], uniqueIdBase + uint64_t(i), reach, source);
        if (std::isfinite(source.power) && source.power > 0.0f)
        {
            sources.push_back(source);
        }
    }
}

void AppendEmissiveClusterSources(const std::vector<EditorEmissiveLight> &emissiveLights,
                                  float staticReach, float dynamicReach,
                                  std::vector<QrClusterLightSource> &sources)
{
    for (const EditorEmissiveLight &light : emissiveLights)
    {
        if (!std::isfinite(light.power) || !(light.power > 0.0f))
        {
            continue;
        }
        const float normal[3] = {
            light.info.normal.data[0], light.info.normal.data[1], light.info.normal.data[2],
        };
        QrClusterLightSource source = {};
        source.uniqueID = light.info.uniqueID;
        source.origin = {
            light.center[0] + 2.0f * normal[0],
            light.center[1] + 2.0f * normal[1],
            light.center[2] + 2.0f * normal[2],
        };
        source.cluster = light.cluster;
        source.reach = light.isAliasModel ? dynamicReach : staticReach;
        source.radius = std::sqrt(std::max(light.info.area, 0.0f) / kRuntimePi);
        source.power = light.power;
        sources.push_back(source);
    }
}

float ComputeEditorRenderScale(float renderScalePercent)
{
    if (!(renderScalePercent > 0.0f))
    {
        return 1.0f;
    }
    return std::clamp(renderScalePercent / 100.0f, 0.2f, 1.0f);
}

QrRenderSharpenTechnique SharpenTechniqueForSetting(int32_t sharpenMode)
{
    switch (sharpenMode)
    {
    case 2:
        return QR_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
    case 1:
        return QR_RENDER_SHARPEN_TECHNIQUE_NAIVE;
    default:
        return QR_RENDER_SHARPEN_TECHNIQUE_NONE;
    }
}

float ResolveEditorSunIntensity(const EditorRenderSettings &settings, const EditorSunState &sun)
{
    if (sun.enabled)
    {
        return sun.radiance;
    }

    const bool timeSunActive = settings.skyType == QR_SKY_TYPE_PROCEDURAL && settings.timeOfDayEnable;
    return timeSunActive ? kEditorSkySunRadiance : 0.0f;
}

bool BuildCelestialDirectionalLight(const EditorRenderSettings &settings, const EditorSunState &sun,
                                    double celestialClockSeconds, QrDirectionalLightUploadInfo &outInfo)
{
    const bool timeSunActive = settings.skyType == QR_SKY_TYPE_PROCEDURAL && settings.timeOfDayEnable;
    if (!sun.enabled && !timeSunActive)
    {
        return false;
    }

    float toSun[3] = {};
    float toMoon[3] = {};
    float celestialRotRad = 0.0f;
    ComputeEditorCelestialFrame(settings, sun, celestialClockSeconds, toSun, toMoon, celestialRotRad);

    const float radiance =
        (sun.radiance > 0.0f ? sun.radiance : kEditorSkySunRadiance) * settings.brightness;
    outInfo = {};
    outInfo.uniqueID = 2;
    outInfo.angularDiameterDegrees = sun.angularDiameter;

    if (!settings.timeOfDayEnable)
    {
        outInfo.color = { radiance * settings.sunColor[0], radiance * settings.sunColor[1],
                          radiance * settings.sunColor[2] };
        outInfo.direction = { -toSun[0], -toSun[1], -toSun[2] };
        return true;
    }

    float trans[3] = {};
    EvalEditorSunGroundTransmittance(settings, toSun, trans);
    if (trans[0] + trans[1] + trans[2] > 1e-4f)
    {
        outInfo.color = { radiance * settings.sunColor[0] * trans[0],
                          radiance * settings.sunColor[1] * trans[1],
                          radiance * settings.sunColor[2] * trans[2] };
        outInfo.direction = { -toSun[0], -toSun[1], -toSun[2] };
        return true;
    }

    if (toMoon[2] > 0.0f && settings.moonIntensity > 0.0f)
    {
        const float phaseScale =
            settings.moonPhase >= 0.0f
                ? (0.18f + 0.82f * std::clamp(settings.moonPhase, 0.0f, 1.0f))
                : 0.7f;
        const float moonScale = radiance * 0.006f * std::max(0.0f, settings.moonIntensity) *
                                phaseScale * std::min(1.0f, toMoon[2] * 5.0f);
        outInfo.color = { moonScale * settings.moonColor[0],
                          moonScale * settings.moonColor[1],
                          moonScale * settings.moonColor[2] };
        outInfo.direction = { -toMoon[0], -toMoon[1], -toMoon[2] };
        return true;
    }
    return false;
}

} // namespace

void ApplyEditorResolutionParams(QrInstance instance, const EditorRenderSettings &settings,
                                 uint32_t clientWidth, uint32_t clientHeight,
                                 QrDrawFrameRenderResolutionParams &out)
{
    const float scale = ComputeEditorRenderScale(settings.renderScale);
    const uint32_t rw = std::max<uint32_t>(1u, uint32_t(float(clientWidth) * scale));
    const uint32_t rh = std::max<uint32_t>(1u, uint32_t(float(clientHeight) * scale));
    out.customRenderSize = { rw, rh };
    out.pPixelizedRenderSize = nullptr;
    out.sharpenTechnique = SharpenTechniqueForSetting(settings.sharpenMode);
    out.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_LINEAR;
    out.resolutionMode = QR_RENDER_RESOLUTION_MODE_CUSTOM;

    if (instance == nullptr)
    {
        return;
    }

    if (settings.upscaleDlss >= 1 && settings.upscaleDlss <= 5 &&
        qrIsRenderUpscaleTechniqueAvailable(instance, QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS))
    {
        static constexpr QrRenderResolutionMode kDlssModes[5] = {
            QR_RENDER_RESOLUTION_MODE_QUALITY,
            QR_RENDER_RESOLUTION_MODE_BALANCED,
            QR_RENDER_RESOLUTION_MODE_PERFORMANCE,
            QR_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE,
            QR_RENDER_RESOLUTION_MODE_CUSTOM,
        };
        out.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
        out.resolutionMode = kDlssModes[settings.upscaleDlss - 1];
    }

    if (settings.upscaleFsr31 >= 1 && settings.upscaleFsr31 <= 6 &&
        qrIsRenderUpscaleTechniqueAvailable(instance, QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3))
    {
        static constexpr QrRenderResolutionMode kFsrResModes[6] = {
            QR_RENDER_RESOLUTION_MODE_NATIVE_AA,
            QR_RENDER_RESOLUTION_MODE_QUALITY,
            QR_RENDER_RESOLUTION_MODE_BALANCED,
            QR_RENDER_RESOLUTION_MODE_PERFORMANCE,
            QR_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE,
            QR_RENDER_RESOLUTION_MODE_CUSTOM,
        };
        out.upscaleTechnique = QR_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3;
        out.resolutionMode = kFsrResModes[settings.upscaleFsr31 - 1];
    }
}

bool EditorSceneRuntime::LoadBspSceneMemory(const std::vector<uint8_t> &bytes,
                                            const std::string &sourceName,
                                            const uint8_t *palette768,
                                            const EditorSceneLoadOptions &options,
                                            std::string &error)
{
    m_dynamicMaterialCache.clear();
    return LoadEditorSceneBspMemory(bytes, sourceName, palette768, scene, error, options);
}

void EditorSceneRuntime::ApplySceneRenderDefaults()
{
    if (scene.usesExternalLightingModel && scene.hasSun)
    {
        sun.enabled = true;
        sun.pitch = std::clamp(scene.sunElevation, -89.0f, 89.0f);
        float yaw = std::fmod(scene.sunDegrees + 180.0f, 360.0f);
        if (yaw < 0.0f)
        {
            yaw += 360.0f;
        }
        sun.yaw = yaw;
        sun.radiance = kEditorSkySunRadiance * std::clamp(scene.sunIntensity / 80.0f, 0.25f, 8.0f);
        std::copy_n(scene.sunColor, 3, settings.sunColor);
    }

    if (!scene.usesExternalLightingModel)
    {
        return;
    }

    if (!scene.skyboxName.empty() || scene.generatedSkyboxSize > 0u)
    {
        settings.skyType = QR_SKY_TYPE_CUBEMAP;
    }
    settings.volumeType = 0;
    settings.lightShafts = false;
    settings.godRays = false;
}

EditorMaterialParams EditorSceneRuntime::DefaultMaterialParamsFor(const std::string &name) const
{
    EditorMaterialParams params;
    if (!scene.usesExternalLightingModel)
    {
        return params;
    }

    const EditorTexture *tex = FindEditorSceneTextureByName(scene, name);
    if (tex != nullptr)
    {
        if (tex->isMirrorShader)
        {
            params.mirror = true;
        }
        if (!tex->defaultEmissiveTexture.empty())
        {
            params.emissiveTexture = tex->defaultEmissiveTexture;
            params.emissiveEnabled = true;
        }
        if (tex->isEmissiveShader || !tex->fullbright.empty())
        {
            params.emissiveEnabled = true;
        }
    }
    return params;
}

void EditorSceneRuntime::ApplyMaterialsDocuments(const EditorMaterialsDocument &activeDoc,
                                                 const EditorMaterialsDocument *baseDoc,
                                                 size_t *outLoaded, size_t *outInherited)
{
    m_dynamicMaterialCache.clear();
    BuildDocumentNameIndex(&activeDoc, m_activeDocIndex.exact, m_activeDocIndex.stem);
    BuildDocumentNameIndex(baseDoc, m_baseDocIndex.exact, m_baseDocIndex.stem);

    size_t loaded = 0;
    size_t inherited = 0;
    for (size_t i = 0; i < materials.Count(); i++)
    {
        const std::string &name = materials.NameAt(i);
        EditorMaterialParams params = DefaultMaterialParamsFor(name);
        if (activeDoc.ReadMaterial(name, params))
        {
            loaded++;
        }
        else if (baseDoc != nullptr && baseDoc->ReadMaterial(name, params))
        {
            inherited++;
        }
        materials.At(i) = params;
    }

    if (outLoaded != nullptr)
    {
        *outLoaded = loaded;
    }
    if (outInherited != nullptr)
    {
        *outInherited = inherited;
    }
}

int EditorSceneRuntime::ApplyConfigText(const std::string &text)
{
    const int applied = ApplyEditorConfigText(text, settings, sun);
    if (applied > 0 && settings.skyBrightness <= 0.001f)
    {
        settings.skyBrightness = 1.0f;
    }
    return applied;
}

EditorLevelApplyResult EditorSceneRuntime::ApplyLevelDocument(const EditorLevelDocument &levelDoc,
                                                              const std::string &levelKey)
{
    EditorLevelApplyResult result;
    result.read = levelDoc.ReadLevel(levelKey);
    result.hasSky = result.read.hasSky;
    result.hasFog = result.read.hasFog;
    result.hasLights = result.read.hasLights;

    if (result.read.hasSky)
    {
        ApplyEditorSkyScope(result.read.sky, true, settings, sun);
    }

    emitterLightOverrides = levelDoc.ReadEmitterLights();

    if (result.read.hasFog)
    {
        std::copy_n(result.read.fog.color, 3, settings.fogColor);
        settings.fogDensity =
            result.read.fog.enabled ? std::max(0.0f, result.read.fog.density) : 0.0f;
    }

    if (result.read.hasLights)
    {
        const bool hasStandard = HasSavedStandardLightsBlock(result.read.lights);
        result.replacedStandardLights = hasStandard;
        if (hasStandard)
        {
            scene.lights.clear();
        }
        customLights.clear();

        for (const EditorLevelLight &entry : result.read.lights)
        {
            if (hasStandard && IsSavedStandardLightEntry(entry))
            {
                scene.lights.push_back(EditorLightFromLevelEntry(scene, entry, "light"));
            }
            else
            {
                customLights.push_back(EditorLightFromLevelEntry(scene, entry, "custom"));
            }
        }
    }

    return result;
}

void EditorSceneRuntime::ReapplyGeneratedLights()
{
    const std::vector<EditorLight> currentBspLights = CollectLeadingBspLights(scene.lights);
    scene.lights = currentBspLights;
    for (const EditorGeneratedLightBase &base : generatedLightBases)
    {
        const EditorEmitterLightOverride *ov =
            FindEditorEmitterOverride(emitterLightOverrides, base.name, base.uniqueID);
        scene.lights.push_back(ResolveEditorSingleGeneratedLight(base, ov, settings, scene));
    }
}

bool EditorSceneRuntime::CreatePoolMaterial(QrInstance instance, const EditorTexture &texture,
                                            QrMaterial &outMaterial,
                                            EditorMaterialEmission &outEmission)
{
    outMaterial = QR_NO_MATERIAL;
    outEmission = {};
    const size_t pixels = size_t(texture.width) * size_t(texture.height);
    if (instance == nullptr || texture.width == 0u || texture.height == 0u ||
        texture.rgba.size() != pixels * 4u)
    {
        return false;
    }

    QrMaterialCreateInfo info = {};
    info.size = { texture.width, texture.height };
    info.textures.pDataAlbedoAlpha = texture.rgba.data();
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    std::vector<uint8_t> rme;
    if (texture.fullbright.size() == pixels)
    {
        BuildFullbrightRmeAndEmission(texture, rme, outEmission);
        for (size_t p = 0; p < pixels; p++)
        {
            if (texture.roughnessMask.size() == pixels)
            {
                rme[p * 4u + 0u] = texture.roughnessMask[p];
            }
            if (texture.metallicMask.size() == pixels)
            {
                rme[p * 4u + 1u] = texture.metallicMask[p];
            }
        }
        info.textures.pDataRoughnessMetallicEmission = rme.data();
    }
    else if (texture.roughnessMask.size() == pixels || texture.metallicMask.size() == pixels)
    {
        const uint8_t defaultRough = uint8_t(kEditorDefaultRoughness * 255.0f);
        rme.resize(pixels * 4u);
        for (size_t p = 0; p < pixels; p++)
        {
            rme[p * 4u + 0u] =
                texture.roughnessMask.size() == pixels ? texture.roughnessMask[p] : defaultRough;
            rme[p * 4u + 1u] = texture.metallicMask.size() == pixels ? texture.metallicMask[p] : 0u;
            rme[p * 4u + 2u] = 0u;
            rme[p * 4u + 3u] = 0u;
        }
        info.textures.pDataRoughnessMetallicEmission = rme.data();
    }

    return qrCreateMaterial(instance, &info, &outMaterial) == QR_SUCCESS;
}

QrGeometryPassThroughType EditorSceneRuntime::PassThroughTypeForSurface(EditorSurfaceKind kind)
{
    switch (kind)
    {
    case EditorSurfaceKind::Water:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT;
    case EditorSurfaceKind::Slime:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT;
    case EditorSurfaceKind::Fence:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED;
    default:
        return QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    }
}

bool EditorSceneRuntime::CreateTextureMaterials(QrInstance instance,
                                                const EditorAssetSource &assets,
                                                const EditorRuntimeLogFn &log)
{
    if (instance == nullptr)
    {
        return false;
    }

    materialBakes.Destroy(instance);
    dynamicMaterialBakes.Destroy(instance);
    m_dynamicMaterialCache.clear();
    DestroyPoolMaterials(instance);

    textureMaterials.assign(scene.textures.size(), QR_NO_MATERIAL);
    textureEmissions.assign(scene.textures.size(), EditorMaterialEmission{});

    for (size_t i = 0; i < scene.textures.size(); i++)
    {
        const std::string key = NormalizeEditorAssetPath(scene.textures[i].name);
        if (!key.empty())
        {
            textureIndexByName.emplace(key, int32_t(i));
        }
    }
    for (const EditorSurface &surface : scene.surfaces)
    {
        if (surface.texture >= 0 && !surface.textureName.empty())
        {
            textureIndexByName.emplace(NormalizeEditorAssetPath(surface.textureName),
                                       surface.texture);
        }
    }

    uint32_t created = 0;
    for (size_t i = 0; i < scene.textures.size(); i++)
    {
        if (CreatePoolMaterial(instance, scene.textures[i], textureMaterials[i],
                               textureEmissions[i]))
        {
            created++;
        }
    }

    CreateScrollingSkyMaterials(instance, assets, log);
    EmitLog(log, "texture materials: " + std::to_string(created) + "/" +
                     std::to_string(scene.textures.size()));
    return true;
}

int32_t EditorSceneRuntime::TextureIndexByName(const std::string &name) const
{
    const std::string key = NormalizeEditorAssetPath(name);
    if (key.empty() || key == "none")
    {
        return -1;
    }
    return FindTextureIndexByKey(textureIndexByName, key);
}

int32_t EditorSceneRuntime::ResolveOrLoadTexture(QrInstance instance,
                                                 const EditorAssetSource &assets,
                                                 const std::string &name)
{
    const std::string key = NormalizeEditorAssetPath(name);
    if (key.empty() || key == "none")
    {
        return -1;
    }
    if (const int32_t existing = TextureIndexByName(key); existing >= 0)
    {
        return existing;
    }
    if (missingExternalTextures.count(key) != 0u)
    {
        return -1;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    if (!LoadRuntimeImagePixels(assets, key, width, height, rgba))
    {
        missingExternalTextures.insert(key);
        return -1;
    }

    const int32_t newIndex = int32_t(scene.textures.size());
    EditorTexture texture;
    texture.name = key;
    texture.width = width;
    texture.height = height;
    texture.rgba = std::move(rgba);
    texture.isExternal = true;
    scene.textures.push_back(std::move(texture));

    QrMaterial material = QR_NO_MATERIAL;
    EditorMaterialEmission emission;
    if (instance != nullptr)
    {
        CreatePoolMaterial(instance, scene.textures.back(), material, emission);
    }
    textureMaterials.push_back(material);
    textureEmissions.push_back(emission);
    textureIndexByName.emplace(key, newIndex);

    static constexpr const char kPrefix[] = "textures/";
    if (key.rfind(kPrefix, 0) == 0)
    {
        textureIndexByName.emplace(key.substr(sizeof(kPrefix) - 1), newIndex);
    }
    const std::string stem = StripEditorImageExtension(key);
    if (!stem.empty())
    {
        textureIndexByName.emplace(stem, newIndex);
    }
    return newIndex;
}

QrMaterial EditorSceneRuntime::SurfaceMaterial(const EditorSurface &surface,
                                               QrMaterial fallbackMaterial) const
{
    if (surface.texture >= 0 && size_t(surface.texture) < textureMaterials.size())
    {
        const QrMaterial material = textureMaterials[size_t(surface.texture)];
        if (material != QR_NO_MATERIAL)
        {
            return material;
        }
    }
    return fallbackMaterial;
}

EditorMaterialEmission EditorSceneRuntime::MaterialEmissionOf(QrMaterial material) const
{
    const EditorMaterialEmission baked = materialBakes.EmissionOf(material);
    if (baked.valid)
    {
        return baked;
    }
    const EditorMaterialEmission dynBaked = dynamicMaterialBakes.EmissionOf(material);
    if (dynBaked.valid)
    {
        return dynBaked;
    }
    const size_t count = std::min(textureMaterials.size(), textureEmissions.size());
    for (size_t i = 0; i < count; i++)
    {
        if (textureMaterials[i] == material)
        {
            return textureEmissions[i];
        }
    }
    return EditorMaterialEmission{};
}

QrMaterial EditorSceneRuntime::SurfaceMaterialWithOverride(QrInstance instance,
                                                           const EditorAssetSource &assets,
                                                           const EditorSurface &surface,
                                                           const EditorMaterialParams &params,
                                                           QrMaterial fallbackMaterial)
{
    const std::string &effectiveName = materials.EffectiveNameFor(surface);
    const auto resolve = [&](const std::string &name) {
        return ResolveOrLoadTexture(instance, assets, name);
    };

    const QrMaterial baked =
        materialBakes.Get(instance, effectiveName, params, scene, textureMaterials, resolve);
    if (baked != QR_NO_MATERIAL)
    {
        return baked;
    }

    int32_t index = ResolveOrLoadTexture(instance, assets, params.baseTexture);
    if (index < 0 && !effectiveName.empty() && effectiveName != surface.textureName)
    {
        index = TextureIndexByName(effectiveName);
    }
    if (index >= 0 && size_t(index) < textureMaterials.size() &&
        textureMaterials[size_t(index)] != QR_NO_MATERIAL)
    {
        return textureMaterials[size_t(index)];
    }
    return SurfaceMaterial(surface, fallbackMaterial);
}

EditorDynamicMaterialResult EditorSceneRuntime::ResolveDynamicMaterial(
    QrInstance instance, const EditorAssetSource &assets, const std::string &shaderName,
    const EditorMaterialsDocument *activeDoc, const EditorMaterialsDocument *baseDoc,
    bool forceEnvBase)
{
    const std::string norm = NormalizeEditorAssetPath(shaderName);
    const std::string cacheKey = forceEnvBase ? (norm + "|env") : norm;
    if (const auto cached = m_dynamicMaterialCache.find(cacheKey);
        cached != m_dynamicMaterialCache.end())
    {
        return cached->second;
    }

    EditorDynamicMaterialResult result;
    const int32_t tableIdx = materials.IndexOf(shaderName);
    if (tableIdx >= 0)
    {
        result.params = materials.At(size_t(tableIdx));
        result.hasAuthoredOverride = true;
    }
    else
    {
        result.params = DefaultMaterialParamsFor(shaderName);
        if (ReadDocumentMaterialNormalized(activeDoc, m_activeDocIndex.exact,
                                           m_activeDocIndex.stem, shaderName, result.params) ||
            ReadDocumentMaterialNormalized(baseDoc, m_baseDocIndex.exact, m_baseDocIndex.stem,
                                           shaderName, result.params))
        {
            result.hasAuthoredOverride = true;
        }
    }

    const bool explicitDtal = EditorSurfaceUsesDtalLighting(settings.truelight, result.params) &&
                              result.params.lightBrightness > 0.0f;
    result.emitRayOnly = !(settings.truelight > 0.0f && settings.modelLights && explicitDtal);

    const auto resolve = [&](const std::string &texName) {
        return ResolveOrLoadTexture(instance, assets, texName);
    };
    const QrMaterial baked = dynamicMaterialBakes.Get(instance, shaderName, result.params, scene,
                                                      textureMaterials, resolve, forceEnvBase);
    if (baked != QR_NO_MATERIAL)
    {
        result.surfaceMaterial = baked;
        result.lightMaterial = baked;
        result.emission = dynamicMaterialBakes.EmissionOf(baked);
    }
    else if (!result.params.baseTexture.empty())
    {
        const int32_t baseIdx = ResolveOrLoadTexture(instance, assets, result.params.baseTexture);
        if (baseIdx >= 0 && size_t(baseIdx) < textureMaterials.size())
        {
            result.surfaceMaterial = textureMaterials[size_t(baseIdx)];
            result.lightMaterial = result.surfaceMaterial;
            result.emission = textureEmissions[size_t(baseIdx)];
        }
    }

    m_dynamicMaterialCache.emplace(cacheKey, result);
    return result;
}

bool EditorSceneRuntime::UploadSurfaceGeometry(QrInstance instance, const EditorAssetSource &assets,
                                               size_t surfaceIndex, QrMaterial fallbackMaterial,
                                               std::vector<QrVertex> &meterVertices,
                                               const EditorRuntimeLogFn &log)
{
    const EditorSurface &surface = scene.surfaces[surfaceIndex];
    const EditorMaterialParams &params = materials.For(surface);
    if (!ShouldUploadStaticSurface(scene, surface, params))
    {
        return true;
    }

    const EditorSurfaceEmissionUploadMode emissionMode = ResolveEditorSurfaceEmissionUploadMode(
        scene, surface, params, settings.truelight > 0.0f, settings.emissiveNee);
    EditorMaterialParams effectiveParams = params;
    if (emissionMode.suppressBaseSurfaceEmission)
    {
        effectiveParams.emissiveFactor = 0.0f;
        effectiveParams.emissiveTexture.clear();
    }
    const QrMaterial material = SurfaceMaterialWithOverride(instance, assets, surface,
                                                            effectiveParams, fallbackMaterial);

    const bool isSkySurface = surface.kind == EditorSurfaceKind::Sky;
    QrGeometryUploadInfo upload = {};
    upload.uniqueID = uint32_t(surfaceIndex) + 1u;
    upload.flags = surface.hasAuthoredNormals
                       ? 0u
                       : uint32_t(QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT);
    upload.geomType = QR_GEOMETRY_TYPE_STATIC;
    upload.visibilityType =
        isSkySurface ? QR_GEOMETRY_VISIBILITY_TYPE_SKY : QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
    upload.vertexCount = uint32_t(surface.vertices.size());
    upload.pVertices = surface.vertices.data();
    upload.indexCount = uint32_t(surface.indices.size());
    upload.pIndices = surface.indices.data();
    upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
    upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    upload.geomMaterial.layerMaterials[0] = material;
    ApplyEditorMaterial(effectiveParams, PassThroughTypeForSurface(surface.kind), upload);
    if (emissionMode.emitRayOnly)
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT;
    }
    upload.transform.matrix[0][0] = 1.0f;
    upload.transform.matrix[1][1] = 1.0f;
    upload.transform.matrix[2][2] = 1.0f;
    EditorMetersGeometry(upload, meterVertices);

    const QrResult uploadResult = qrUploadGeometry(instance, &upload);
    if (uploadResult != QR_SUCCESS)
    {
        EmitLog(log, "geometry upload for '" + surface.textureName + "': " +
                         std::string(qrGetResultDescription(uploadResult)));
        return false;
    }

    if (surface.isCutoutUnderlay && !isSkySurface &&
        HasEnabledSurfaceUnderlayStage(scene, surface, params))
    {
        UploadCutoutBackingGeometry(instance, overlayCache.GetCutoutBackingMaterial(instance),
                                    surfaceIndex, surface);
    }
    return true;
}

void EditorSceneRuntime::BuildEmissiveLights(QrInstance instance, const EditorAssetSource &assets,
                                             QrMaterial fallbackMaterial,
                                             const EditorRuntimeLogFn &log)
{
    const EditorDtalMaterialLookup lookup{
        [&](const EditorSurface &surf, const EditorMaterialParams &params) {
            return SurfaceMaterialWithOverride(instance, assets, surf, params, fallbackMaterial);
        },
        [&](QrMaterial mat) { return MaterialEmissionOf(mat); },
    };

    EditorDtalBuildResult built;
    BuildEditorSceneEmissiveLights(scene, materials, settings, lookup, built);
    emissiveLights = std::move(built.emissiveLights);
    generatedLightBases = std::move(built.generatedLightBases);

    if (settings.truelight > 0.0f)
    {
        EmitLog(log, "area lights: " + std::to_string(emissiveLights.size()) + " from " +
                         std::to_string(scene.surfaces.size()) + " surfaces, " +
                         std::to_string(built.counts.fitted) + " fitted, " +
                         std::to_string(built.counts.glowClipped) + " glow-clipped and " +
                         std::to_string(built.counts.squared) + " squared");
    }

    ReapplyGeneratedLights();
    EmitLog(log, "generated lights: " + std::to_string(scene.lights.size()) + " (" +
                     std::to_string(generatedLightBases.size()) + " emitters)");
}

bool EditorSceneRuntime::UploadStaticGeometry(QrInstance instance, const EditorAssetSource &assets,
                                              QrMaterial fallbackMaterial,
                                              const EditorRuntimeLogFn &log)
{
    if (instance == nullptr)
    {
        return false;
    }

    const QrResult beginResult = qrBeginStaticGeometries(instance);
    if (beginResult != QR_SUCCESS)
    {
        EmitLog(log, "begin static scene: " + std::string(qrGetResultDescription(beginResult)));
        return false;
    }

    materialBakes.BeginPass();
    uint32_t uploadedSurfaces = 0;
    std::vector<QrVertex> meterVertices;
    for (size_t i = 0; i < scene.surfaces.size(); i++)
    {
        if (!ShouldUploadStaticSurface(scene, scene.surfaces[i], materials.For(scene.surfaces[i])))
        {
            continue;
        }
        if (!UploadSurfaceGeometry(instance, assets, i, fallbackMaterial, meterVertices, log))
        {
            return false;
        }
        uploadedSurfaces++;
    }

    if (scene.surfaces.empty() && !scene.vertices.empty() && !scene.indices.empty())
    {
        QrGeometryUploadInfo upload = {};
        upload.uniqueID = 1;
        upload.flags = QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
        upload.geomType = QR_GEOMETRY_TYPE_STATIC;
        upload.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
        upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_WORLD_0;
        upload.vertexCount = uint32_t(scene.vertices.size());
        upload.pVertices = scene.vertices.data();
        upload.indexCount = uint32_t(scene.indices.size());
        upload.pIndices = scene.indices.data();
        upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
        upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
        upload.geomMaterial.layerMaterials[0] = fallbackMaterial;
        ApplyEditorMaterial(EditorMaterialParams{}, QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, upload);
        upload.transform.matrix[0][0] = 1.0f;
        upload.transform.matrix[1][1] = 1.0f;
        upload.transform.matrix[2][2] = 1.0f;
        EditorMetersGeometry(upload, meterVertices);
        if (const QrResult res = qrUploadGeometry(instance, &upload); res != QR_SUCCESS)
        {
            EmitLog(log, "geometry upload: " + std::string(qrGetResultDescription(res)));
            return false;
        }
    }

    EmitLog(log, "geometry upload: " + std::to_string(uploadedSurfaces) + " surfaces");
    BuildEmissiveLights(instance, assets, fallbackMaterial, log);

    const QrResult submitResult = qrSubmitStaticGeometries(instance);
    if (submitResult == QR_SUCCESS)
    {
        materialBakes.EndPass(instance);
    }

    EmitLog(log, "material bakes: " + std::to_string(materialBakes.Count()));
    EmitLog(log, "submit static scene: " + std::string(qrGetResultDescription(submitResult)));
    return submitResult == QR_SUCCESS;
}

bool EditorSceneRuntime::UploadWorldClusters(QrInstance instance, const EditorRuntimeLogFn &log)
{
    if (instance == nullptr)
    {
        return false;
    }

    const EditorWorldClusters &clusters = scene.worldClusters;
    if (clusters.numLeafs == 0u || clusters.mins.empty())
    {
        QrWorldLightsUploadInfo emptyUpload = {};
        emptyUpload.clusterGeneration = ++clusterGeneration;
        return qrUploadWorldLights(instance, &emptyUpload) == QR_SUCCESS;
    }

    QrWorldLightsUploadInfo upload = {};
    upload.numClusters = uint32_t(clusters.mins.size());
    upload.pClusterMins = clusters.mins.data();
    upload.pClusterMaxs = clusters.maxs.data();
    upload.pClusterFlags = clusters.flags.data();
    upload.clusterGeneration = ++clusterGeneration;
    AttachWorldClusterVis(clusters, upload);

    std::vector<QrFloat3D> meterMins;
    std::vector<QrFloat3D> meterMaxs;
    EditorMetersWorldLights(upload, meterMins, meterMaxs);

    const QrResult result = qrUploadWorldLights(instance, &upload);
    if (result != QR_SUCCESS)
    {
        EmitLog(log, "world clusters: " + std::string(qrGetResultDescription(result)));
        return false;
    }

    size_t resolved = 0;
    for (const EditorLight &light : scene.lights)
    {
        if (light.cluster != QR_CLUSTER_LIGHT_NO_CLUSTER)
        {
            resolved++;
        }
    }
    EmitLog(log, "world clusters: " + std::to_string(upload.numClusters) +
                     " clusters, lights resolved " + std::to_string(resolved) + "/" +
                     std::to_string(scene.lights.size()));
    LogSurfaceClusterSummary(scene, upload.numClusters, log);
    return true;
}

void EditorSceneRuntime::CreateSkyboxMaterials(QrInstance instance,
                                               const EditorAssetSource &assets,
                                               const EditorRuntimeLogFn &log)
{
    if (instance == nullptr)
    {
        return;
    }

    const std::string rawKey = NormalizeEditorAssetPath(scene.skyboxName);
    const std::string q1Key = NormalizeEditorAssetPath("gfx/env/" + scene.skyboxName);
    const std::string &baseKey =
        (scene.usesExternalLightingModel || rawKey.rfind("env/", 0) == 0) ? rawKey : q1Key;

    double rgbSum[3] = { 0.0, 0.0, 0.0 };
    uint64_t pixelCount = 0;
    int loadedFaces = 0;
    for (int i = 0; i < 6; i++)
    {
        SkyboxFacePixels face;
        if (!ResolveSkyboxFaceRgba(assets, scene, baseKey, q1Key, i, face))
        {
            continue;
        }
        QrMaterialCreateInfo info = {};
        info.size = { face.width, face.height };
        info.filter = QR_SAMPLER_FILTER_LINEAR;
        info.addressModeU = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.textures.pDataAlbedoAlpha = face.rgba.data();
        if (qrCreateMaterial(instance, &info, &skyboxMaterials[i]) == QR_SUCCESS)
        {
            skyboxWidths[i] = face.width;
            skyboxHeights[i] = face.height;
            loadedFaces++;
            AccumulateSkyboxRgb(face.rgba, face.width, face.height, rgbSum, pixelCount);
        }
    }

    hasSkybox = loadedFaces > 0;
    if (!scene.skyboxName.empty() || hasSkybox)
    {
        const std::string label =
            scene.skyboxName.empty() ? std::string("generated cloud sky") : scene.skyboxName;
        EmitLog(log, "skybox '" + label + "': " + std::to_string(loadedFaces) + "/6 faces loaded");
    }

    if (hasSkybox && !scene.hasSkyFlatColor && pixelCount > 0u)
    {
        const double scale = 1.0 / (double(pixelCount) * 255.0);
        scene.skyFlatColor[0] = float(rgbSum[0] * scale);
        scene.skyFlatColor[1] = float(rgbSum[1] * scale);
        scene.skyFlatColor[2] = float(rgbSum[2] * scale);
        scene.hasSkyFlatColor = true;
    }
}

void EditorSceneRuntime::CreateScrollingSkyMaterials(QrInstance instance,
                                                     const EditorAssetSource &assets,
                                                     const EditorRuntimeLogFn &log)
{
    DestroySkyMaterials(instance);
    CreateSkyboxMaterials(instance, assets, log);

    if (instance == nullptr || !scene.hasScrollingSky || scene.skyLayerWidth == 0u ||
        scene.skyLayerHeight == 0u)
    {
        return;
    }
    const size_t expectedBytes = size_t(scene.skyLayerWidth) * size_t(scene.skyLayerHeight) * 4u;
    if (scene.skySolidRgba.size() != expectedBytes || scene.skyAlphaRgba.size() != expectedBytes)
    {
        return;
    }

    QrMaterialCreateInfo info = {};
    info.size = { scene.skyLayerWidth, scene.skyLayerHeight };
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.textures.pDataAlbedoAlpha = scene.skySolidRgba.data();
    qrCreateMaterial(instance, &info, &skySolidMaterial);
    info.textures.pDataAlbedoAlpha = scene.skyAlphaRgba.data();
    qrCreateMaterial(instance, &info, &skyAlphaMaterial);
}

void EditorSceneRuntime::UploadSky(QrInstance instance, const float cameraOrigin[3],
                                   double timeSeconds, float skyDistance) const
{
    if (instance == nullptr || cameraOrigin == nullptr ||
        (settings.skyType != QR_SKY_TYPE_RASTERIZED_GEOMETRY &&
         settings.skyType != QR_SKY_TYPE_CUBEMAP))
    {
        return;
    }

    const ScrollingSkyContext ctx{
        cameraOrigin,
        timeSeconds,
        std::max(skyDistance * 0.5f, 512.0f) / std::sqrt(3.0f),
    };
    const float visibleSkyScale =
        std::max(0.0f, settings.skyScale * settings.skyBrightness * settings.brightness);
    const QrFloat4D skyColor = { visibleSkyScale, visibleSkyScale, visibleSkyScale, 1.0f };
    const bool hasScrolling = scene.hasScrollingSky && skySolidMaterial != QR_NO_MATERIAL &&
                              skyAlphaMaterial != QR_NO_MATERIAL;

    if (settings.skyType == QR_SKY_TYPE_CUBEMAP)
    {
        if (hasSkybox)
        {
            UploadSkyboxGeometry(instance, ctx, skyboxMaterials, skyboxWidths, skyboxHeights,
                                 skyColor);
            return;
        }
        if (!hasScrolling)
        {
            return;
        }
    }
    else if (!hasScrolling)
    {
        if (hasSkybox)
        {
            UploadSkyboxGeometry(instance, ctx, skyboxMaterials, skyboxWidths, skyboxHeights,
                                 skyColor);
        }
        return;
    }

    std::vector<QrVertex> solidVerts;
    std::vector<QrVertex> alphaVerts;
    solidVerts.reserve(1152);
    alphaVerts.reserve(1152);
    for (int axis = 0; axis < 6; axis++)
    {
        AppendScrollingSkyFace(axis, ctx, solidVerts, alphaVerts);
    }

    QrRasterizedGeometryUploadInfo upload = {};
    upload.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY;
    upload.transform.matrix[0][0] = 1.0f;
    upload.transform.matrix[1][1] = 1.0f;
    upload.transform.matrix[2][2] = 1.0f;
    upload.color = skyColor;

    upload.vertexCount = uint32_t(solidVerts.size());
    upload.pVertices = solidVerts.data();
    upload.material = skySolidMaterial;
    qrUploadRasterizedGeometry(instance, &upload, nullptr, nullptr);

    upload.vertexCount = uint32_t(alphaVerts.size());
    upload.pVertices = alphaVerts.data();
    upload.material = skyAlphaMaterial;
    upload.pipelineState = QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
    upload.blendFuncSrc = QR_BLEND_FACTOR_SRC_ALPHA;
    upload.blendFuncDst = QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    qrUploadRasterizedGeometry(instance, &upload, nullptr, nullptr);
}

void EditorSceneRuntime::UploadWorldOverlays(
    QrInstance instance, const EditorAssetSource &assets, const EditorCameraBasis &cam,
    float timeSeconds, const std::vector<const std::vector<EditorTexture> *> *extraTextureLists)
{
    const EditorOverlaySceneUploadOptions options{
        &scene, &materials, &assets, &cam, settings.truelight > 0.0f, settings.emissiveNee,
        timeSeconds, extraTextureLists,
    };
    UploadEditorOverlaySurfaces(instance, options, overlayCache);
}

void EditorSceneRuntime::AdvanceCelestialClock(float deltaSeconds)
{
    if (settings.skyType == QR_SKY_TYPE_PROCEDURAL && settings.timeOfDayEnable &&
        settings.timeOfDaySpeed > 0.0f)
    {
        celestialClockSeconds += double(deltaSeconds);
    }
}

void EditorSceneRuntime::UploadSun(QrInstance instance, const EditorRuntimeLogFn &log) const
{
    if (instance == nullptr)
    {
        return;
    }

    QrDirectionalLightUploadInfo info = {};
    if (!BuildCelestialDirectionalLight(settings, sun, celestialClockSeconds, info))
    {
        return;
    }

    const QrResult result = qrUploadDirectionalLight(instance, &info);
    if (result != QR_SUCCESS)
    {
        EmitLog(log, "sun: " + std::string(qrGetResultDescription(result)));
    }
}

QrResult EditorSceneRuntime::UploadSingleLight(QrInstance instance, const EditorLight &light,
                                               uint64_t uniqueID, float styleScale)
{
    const float px = light.position[0] + (light.hasOffset ? light.offset[0] : 0.0f);
    const float py = light.position[1] + (light.hasOffset ? light.offset[1] : 0.0f);
    const float pz = light.position[2] + (light.hasOffset ? light.offset[2] : 0.0f);
    const bool isSpot = light.spot && (light.direction[0] * light.direction[0] +
                                       light.direction[1] * light.direction[1] +
                                       light.direction[2] * light.direction[2]) > 0.0f;

    if (isSpot)
    {
        QrSpotLightUploadInfo info = {};
        info.uniqueID = uniqueID;
        info.color = { light.color[0] * styleScale, light.color[1] * styleScale,
                       light.color[2] * styleScale };
        info.position = { px, py, pz };
        info.direction = { light.direction[0], light.direction[1], light.direction[2] };
        info.radius = light.radius;
        info.angleInner = light.angleInner * (kRuntimePi / 180.0f);
        info.angleOuter = light.angleOuter * (kRuntimePi / 180.0f);
        info.lightShafts = light.lightShafts ? 1 : 0;
        info.lightShaftsPower = std::max(0.0f, light.lightShaftsPower);
        info.lightShaftsDistance = std::max(0.0f, light.lightShaftsDistance);
        info.lightShaftsFalloff = std::max(0.01f, light.lightShaftsFalloff);
        EditorMetersSpotLight(info);
        return qrUploadSpotLight(instance, &info);
    }

    QrSphericalLightUploadInfo info = {};
    info.uniqueID = uniqueID;
    info.color = { light.color[0] * styleScale, light.color[1] * styleScale,
                   light.color[2] * styleScale };
    info.position = { px, py, pz };
    info.radius = light.radius;
    info.lightShafts = light.lightShafts ? 1 : 0;
    info.lightShaftsPower = std::max(0.0f, light.lightShaftsPower);
    info.lightShaftsDistance = std::max(0.0f, light.lightShaftsDistance);
    info.lightShaftsFalloff = std::max(0.01f, light.lightShaftsFalloff);
    EditorMetersSphericalLight(info);
    return qrUploadSphericalLight(instance, &info);
}

QrResult EditorSceneRuntime::UploadLights(QrInstance instance, double timeSeconds,
                                          bool animateLightStyles,
                                          const EditorRuntimeLogFn &log) const
{
    if (instance == nullptr)
    {
        return QR_SUCCESS;
    }

    QrResult lastResult = QR_SUCCESS;
    for (size_t index = 0; index < scene.lights.size(); index++)
    {
        const EditorLight &light = scene.lights[index];
        if (!light.enabled)
        {
            continue;
        }
        const float style = (animateLightStyles && light.hasStyle && light.style > 0)
                                ? EvaluateEditorLightStyle(light.style, timeSeconds)
                                : 1.0f;
        lastResult = UploadSingleLight(instance, light, EditorLightUniqueID(index), style);
        if (lastResult != QR_SUCCESS)
        {
            EmitLog(log, "lights: " + std::string(qrGetResultDescription(lastResult)));
            return lastResult;
        }
    }

    for (size_t index = 0; index < customLights.size(); index++)
    {
        const EditorLight &light = customLights[index];
        if (!light.enabled)
        {
            continue;
        }
        const float style = (animateLightStyles && light.hasStyle && light.style > 0)
                                ? EvaluateEditorLightStyle(light.style, timeSeconds)
                                : 1.0f;
        lastResult = UploadSingleLight(instance, light, EditorCustomLightUniqueID(index), style);
        if (lastResult != QR_SUCCESS)
        {
            EmitLog(log, "lights: " + std::string(qrGetResultDescription(lastResult)));
            return lastResult;
        }
    }
    return lastResult;
}

QrResult EditorSceneRuntime::UploadEmissiveLights(
    QrInstance instance, float timeSeconds,
    const std::vector<EditorEmissiveLight> *extraDynamicLights,
    const EditorRuntimeLogFn &log) const
{
    const QrResult result = UploadEditorEmissiveLights(instance, scene, materials, emissiveLights,
                                                       timeSeconds, extraDynamicLights);
    if (result != QR_SUCCESS)
    {
        EmitLog(log, "area lights: " + std::string(qrGetResultDescription(result)));
    }
    return result;
}

void AppendExtraClusterLightSources(const EditorClusterLightUploadExtras *extras,
                                      std::vector<QrClusterLightSource> &sources)
{
    if (extras == nullptr)
    {
        return;
    }

    if (extras->extraQuakeUnitSources != nullptr)
    {
        for (const QrClusterLightSource &source : *extras->extraQuakeUnitSources)
        {
            if (std::isfinite(source.power) && source.power > 0.0f)
            {
                sources.push_back(source);
            }
        }
    }

    if (extras->extraDynamicEmissiveLights != nullptr)
    {
        AppendEmissiveClusterSources(*extras->extraDynamicEmissiveLights, kEditorLightReachUnits,
                                     kEditorDynamicLightReachUnits, sources);
    }
}

QrResult EditorSceneRuntime::UploadClusterLightSources(
    QrInstance instance, const EditorClusterLightUploadExtras *extras,
    const EditorRuntimeLogFn &log) const
{
    if (instance == nullptr || scene.worldClusters.numLeafs == 0u)
    {
        return QR_SUCCESS;
    }

    std::vector<QrClusterLightSource> sources;
    const size_t baseCount = scene.lights.size() + customLights.size() + emissiveLights.size();
    sources.reserve(baseCount);

    const float mapReach =
        scene.usesExternalLightingModel ? kEditorLightReachUnits : kEditorDynamicLightReachUnits;
    AppendPointLightClusterSources(scene.lights, kEditorFirstLightUniqueID, mapReach, sources);
    AppendPointLightClusterSources(customLights, kEditorFirstCustomLightUniqueID,
                                   kEditorDynamicLightReachUnits, sources);
    AppendEmissiveClusterSources(emissiveLights, kEditorLightReachUnits,
                                 kEditorDynamicLightReachUnits, sources);
    AppendExtraClusterLightSources(extras, sources);

    for (QrClusterLightSource &source : sources)
    {
        EditorMetersClusterLightSource(source);
    }

    QrClusterLightSourcesUploadInfo info = {};
    info.numLights = uint32_t(sources.size());
    info.pLights = sources.empty() ? nullptr : sources.data();
    info.topUpReach = EditorMetersLength(kEditorLightTopUpReachUnits);
    info.allowIncremental = 1;
    info.allowOverflow = 1;
    info.validate = 0;

    const QrResult result = qrUploadClusterLightSources(instance, &info);
    if (result != QR_SUCCESS)
    {
        EmitLog(log, "cluster light sources: " + std::string(qrGetResultDescription(result)));
    }
    return result;
}

void EditorSceneRuntime::PopulateDrawFrame(QrInstance instance, const float viewerPos[3],
                                           uint32_t clientWidth, uint32_t clientHeight,
                                           EditorFrameParams &outFrameParams,
                                           QrDrawFrameInfo &ioDraw) const
{
    float toSun[3] = {};
    float toMoon[3] = {};
    float celestialRotRad = 0.0f;
    ComputeEditorCelestialFrame(settings, sun, celestialClockSeconds, toSun, toMoon,
                                celestialRotRad);
    const float activeSunIntensity = ResolveEditorSunIntensity(settings, sun);
    BuildEditorFrameParams(settings, toSun, toMoon, celestialRotRad, activeSunIntensity, viewerPos,
                           scene.hasSkyFlatColor ? scene.skyFlatColor : nullptr, outFrameParams);
    ApplyEditorResolutionParams(instance, settings, clientWidth, clientHeight,
                                outFrameParams.resolution);

    outFrameParams.postEffects.pBloom = &outFrameParams.bloomEffect;
    outFrameParams.postEffects.pVignette = &outFrameParams.vignette;
    outFrameParams.postEffects.pFilmGrain = &outFrameParams.filmGrain;
    outFrameParams.postEffects.pSharpen = &outFrameParams.sharpen;
    outFrameParams.postEffects.pCRT = &outFrameParams.crt;

    ioDraw.pSkyParams = &outFrameParams.sky;
    ioDraw.pTonemappingParams = &outFrameParams.tonemap;
    ioDraw.pVolumetricParams = &outFrameParams.volume;
    ioDraw.pLevelFogParams = &outFrameParams.fog;
    ioDraw.pReflectRefractParams = &outFrameParams.reflectRefract;
    ioDraw.pTexturesParams = &outFrameParams.textures;
    ioDraw.pBloomParams = &outFrameParams.bloom;
    ioDraw.pIlluminationParams = &outFrameParams.illumination;
    ioDraw.forceAntiFirefly = outFrameParams.antifirefly;
    ioDraw.pRenderResolutionParams = &outFrameParams.resolution;
    ioDraw.postEffectParams = outFrameParams.postEffects;
}

void EditorSceneRuntime::DestroyPoolMaterials(QrInstance instance)
{
    if (instance != nullptr)
    {
        for (QrMaterial &material : textureMaterials)
        {
            if (material != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(instance, material);
                material = QR_NO_MATERIAL;
            }
        }
    }
    textureMaterials.clear();
    textureEmissions.clear();
    textureIndexByName.clear();
    missingExternalTextures.clear();
    m_dynamicMaterialCache.clear();
    while (!scene.textures.empty() && scene.textures.back().isExternal)
    {
        scene.textures.pop_back();
    }
}

void EditorSceneRuntime::DestroySkyMaterials(QrInstance instance)
{
    hasSkybox = false;
    for (int i = 0; i < 6; i++)
    {
        if (instance != nullptr && skyboxMaterials[i] != QR_NO_MATERIAL)
        {
            qrDestroyMaterial(instance, skyboxMaterials[i]);
        }
        skyboxMaterials[i] = QR_NO_MATERIAL;
        skyboxWidths[i] = 0u;
        skyboxHeights[i] = 0u;
    }
    if (instance != nullptr)
    {
        if (skySolidMaterial != QR_NO_MATERIAL)
        {
            qrDestroyMaterial(instance, skySolidMaterial);
        }
        if (skyAlphaMaterial != QR_NO_MATERIAL)
        {
            qrDestroyMaterial(instance, skyAlphaMaterial);
        }
    }
    skySolidMaterial = QR_NO_MATERIAL;
    skyAlphaMaterial = QR_NO_MATERIAL;
}

void EditorSceneRuntime::DestroyOverlayMaterials(QrInstance instance)
{
    overlayCache.Destroy(instance);
}

void EditorSceneRuntime::DestroyGpuResources(QrInstance instance)
{
    if (instance != nullptr)
    {
        if (qrBeginStaticGeometries(instance) == QR_SUCCESS)
        {
            qrSubmitStaticGeometries(instance);
        }
        QrWorldLightsUploadInfo emptyClusters = {};
        emptyClusters.clusterGeneration = ++clusterGeneration;
        qrUploadWorldLights(instance, &emptyClusters);
        materialBakes.Destroy(instance);
        dynamicMaterialBakes.Destroy(instance);
        DestroyPoolMaterials(instance);
        DestroySkyMaterials(instance);
        DestroyOverlayMaterials(instance);
    }
    else
    {
        ResetGpuHandlesOnly();
    }

    emissiveLights.clear();
    generatedLightBases.clear();
    emitterLightOverrides.clear();
    customLights.clear();
    m_dynamicMaterialCache.clear();
    m_activeDocIndex = {};
    m_baseDocIndex = {};
    scene = EditorScene{};
    materials = EditorMaterialTable{};
}

void EditorSceneRuntime::ResetGpuHandlesOnly()
{
    materialBakes = EditorMaterialBakes{};
    dynamicMaterialBakes = EditorMaterialBakes{};
    textureMaterials.clear();
    textureEmissions.clear();
    textureIndexByName.clear();
    missingExternalTextures.clear();
    m_dynamicMaterialCache.clear();
    overlayCache.ResetHandlesOnly();
    skySolidMaterial = QR_NO_MATERIAL;
    skyAlphaMaterial = QR_NO_MATERIAL;
    hasSkybox = false;
    for (int i = 0; i < 6; i++)
    {
        skyboxMaterials[i] = QR_NO_MATERIAL;
        skyboxWidths[i] = 0u;
        skyboxHeights[i] = 0u;
    }
}
