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

#include "EditorOverlayPipeline.h"

#include "EditorSpatial.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <unordered_set>

namespace
{

constexpr float kOverlayPi = 3.14159265358979323846f;

std::string LowercaseAscii(std::string_view text)
{
    std::string out(text);
    for (char &ch : out)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool TryReadDecodedImage(const EditorAssetSource &assets, const std::string &candidate,
                         uint32_t &outWidth, uint32_t &outHeight, std::vector<uint8_t> &outRgba)
{
    std::vector<uint8_t> encoded;
    return assets.ReadFile(candidate, encoded) &&
           DecodeEditorImageRgba(encoded, outWidth, outHeight, outRgba);
}

bool HasImageFileExtension(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash);
}

void AppendExternalPathBases(const std::string &key, std::vector<std::string> &out)
{
    out.push_back(key);
    if (key.rfind("textures/", 0) != 0)
    {
        out.push_back("textures/" + key);
    }

    const size_t baseCount = out.size();
    for (size_t i = 0; i < baseCount; i++)
    {
        std::string alt = out[i];
        const size_t star = alt.find('*');
        if (star != std::string::npos)
        {
            alt[star] = '#';
            out.push_back(std::move(alt));
        }
    }
}

void CleanAdditiveBlackBackground(std::vector<uint8_t> &pixels)
{
    for (size_t i = 0; i + 3u < pixels.size(); i += 4u)
    {
        const uint8_t peak = std::max({ pixels[i + 0], pixels[i + 1], pixels[i + 2] });
        if (peak <= 6u)
        {
            pixels[i + 0] = 0;
            pixels[i + 1] = 0;
            pixels[i + 2] = 0;
            pixels[i + 3] = 0;
        }
        else if (peak < 18u)
        {
            const float scale = float(peak - 6u) / 12.0f;
            for (int c = 0; c < 3; c++)
            {
                pixels[i + c] =
                    uint8_t(std::clamp(float(pixels[i + c]) * scale, 0.0f, 255.0f));
            }
            pixels[i + 3] = uint8_t(std::clamp(float(pixels[i + 3]) * scale, 0.0f, 255.0f));
        }
    }
}

bool TryResolveTextureFromList(const std::vector<EditorTexture> &textures,
                               const std::string &requestedPath, std::vector<uint8_t> &outRgba,
                               uint32_t &outWidth, uint32_t &outHeight)
{
    const std::string needle = NormalizeEditorAssetPath(requestedPath);
    const std::string needleStem = StripEditorImageExtension(needle);
    if (needle.empty())
    {
        return false;
    }

    for (const EditorTexture &tex : textures)
    {
        if (tex.width == 0 || tex.height == 0 ||
            tex.rgba.size() != size_t(tex.width) * size_t(tex.height) * 4u)
        {
            continue;
        }
        const std::string cand = NormalizeEditorAssetPath(tex.name);
        if (cand == needle || StripEditorImageExtension(cand) == needleStem ||
            ("textures/" + cand) == needle ||
            ("textures/" + StripEditorImageExtension(cand)) == needleStem)
        {
            outWidth = tex.width;
            outHeight = tex.height;
            outRgba = tex.rgba;
            return true;
        }
    }
    return false;
}

bool LoadOverlayMaterialPixels(
    const EditorAssetSource &assets, const EditorScene &scene,
    const std::vector<const std::vector<EditorTexture> *> *extraTextureLists,
    const std::string &imageName, std::vector<uint8_t> &outRgba, uint32_t &outWidth,
    uint32_t &outHeight)
{
    bool loaded = false;
    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
        adapter != nullptr && adapter->loadExternalImageRgba != nullptr)
    {
        std::string resolved;
        loaded = adapter->loadExternalImageRgba(&assets, imageName, outRgba, outWidth, outHeight,
                                                resolved);
    }
    else
    {
        loaded = LoadEditorExternalImageRgba(assets, imageName, outWidth, outHeight, outRgba);
    }

    if (!loaded)
    {
        loaded = TryResolveTextureFromList(scene.textures, imageName, outRgba, outWidth, outHeight);
    }
    if (!loaded && extraTextureLists != nullptr)
    {
        for (const std::vector<EditorTexture> *list : *extraTextureLists)
        {
            if (list != nullptr &&
                TryResolveTextureFromList(*list, imageName, outRgba, outWidth, outHeight))
            {
                loaded = true;
                break;
            }
        }
    }
    return loaded && outWidth > 0 && outHeight > 0 &&
           outRgba.size() == size_t(outWidth) * size_t(outHeight) * 4u;
}

void BuildOverlayScreenEmissionRme(const std::vector<uint8_t> &rgba, size_t pxCount,
                                   std::vector<uint8_t> &rme)
{
    rme.resize(pxCount * 4u, 0u);
    for (size_t p = 0; p < pxCount; p++)
    {
        const uint8_t peak =
            std::max({ rgba[p * 4u + 0u], rgba[p * 4u + 1u], rgba[p * 4u + 2u] });
        rme[p * 4u + 0u] = 255u;
        rme[p * 4u + 1u] = 0u;
        rme[p * 4u + 2u] = peak;
        rme[p * 4u + 3u] = 0u;
    }
}

void ComputeEnvironmentTexCoord(const EditorCameraBasis &cam, const float pos[3],
                                const float normal[3], float out[2])
{
    float view[3] = {
        pos[0] - cam.origin[0],
        pos[1] - cam.origin[1],
        pos[2] - cam.origin[2],
    };
    const float len = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
    if (len <= 1.0e-4f)
    {
        out[0] = 0.5f;
        out[1] = 0.5f;
        return;
    }
    view[0] /= len;
    view[1] /= len;
    view[2] /= len;

    const float facing = view[0] * normal[0] + view[1] * normal[1] + view[2] * normal[2];
    const float refl[3] = {
        view[0] - 2.0f * facing * normal[0],
        view[1] - 2.0f * facing * normal[1],
        view[2] - 2.0f * facing * normal[2],
    };

    const float eyeX =
        -(refl[0] * cam.right[0] + refl[1] * cam.right[1] + refl[2] * cam.right[2]);
    const float eyeY = refl[0] * cam.up[0] + refl[1] * cam.up[1] + refl[2] * cam.up[2];
    const float eyeZ =
        -(refl[0] * cam.forward[0] + refl[1] * cam.forward[1] + refl[2] * cam.forward[2]);

    const float denom =
        2.0f * std::sqrt(eyeX * eyeX + eyeY * eyeY + (eyeZ + 1.0f) * (eyeZ + 1.0f));
    if (denom <= 1.0e-4f)
    {
        out[0] = 0.5f;
        out[1] = 0.5f;
        return;
    }
    out[0] = 0.5f + eyeX / denom;
    out[1] = 0.5f - eyeY / denom;
}

void DeformAutoSpriteQuad(QrVertex quad[4], const EditorCameraBasis &cam)
{
    float mid[3] = {};
    float uvMin[2] = { quad[0].texCoord[0], quad[0].texCoord[1] };
    float uvMax[2] = { quad[0].texCoord[0], quad[0].texCoord[1] };
    for (int i = 0; i < 4; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            mid[c] += quad[i].position[c];
        }
        uvMin[0] = std::min(uvMin[0], quad[i].texCoord[0]);
        uvMax[0] = std::max(uvMax[0], quad[i].texCoord[0]);
        uvMin[1] = std::min(uvMin[1], quad[i].texCoord[1]);
        uvMax[1] = std::max(uvMax[1], quad[i].texCoord[1]);
    }
    mid[0] *= 0.25f;
    mid[1] *= 0.25f;
    mid[2] *= 0.25f;

    float distSum = 0.0f;
    for (int i = 0; i < 4; i++)
    {
        const float dx = quad[i].position[0] - mid[0];
        const float dy = quad[i].position[1] - mid[1];
        const float dz = quad[i].position[2] - mid[2];
        distSum += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const float radius = (distSum * 0.25f) * 0.70710678f;
    const float left[3] = { -cam.right[0] * radius, -cam.right[1] * radius,
                            -cam.right[2] * radius };
    const float up[3] = { cam.up[0] * radius, cam.up[1] * radius, cam.up[2] * radius };
    const float uvs[4][2] = {
        { uvMin[0], uvMin[1] }, { uvMax[0], uvMin[1] },
        { uvMax[0], uvMax[1] }, { uvMin[0], uvMax[1] },
    };
    static constexpr float kSigns[4][2] = {
        { 1.0f, 1.0f }, { -1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f },
    };
    for (int i = 0; i < 4; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            quad[i].position[c] = mid[c] + kSigns[i][0] * left[c] + kSigns[i][1] * up[c];
            quad[i].normal[c] = -cam.forward[c];
        }
        quad[i].texCoord[0] = uvs[i][0];
        quad[i].texCoord[1] = uvs[i][1];
    }
}

void ComputeQuadMinorAxis(const float mid[2][3], const EditorCameraBasis &cam, float outMinor[3])
{
    const float major[3] = {
        mid[1][0] - mid[0][0],
        mid[1][1] - mid[0][1],
        mid[1][2] - mid[0][2],
    };
    outMinor[0] = major[1] * cam.forward[2] - major[2] * cam.forward[1];
    outMinor[1] = major[2] * cam.forward[0] - major[0] * cam.forward[2];
    outMinor[2] = major[0] * cam.forward[1] - major[1] * cam.forward[0];
    const float minorLen = std::sqrt(outMinor[0] * outMinor[0] + outMinor[1] * outMinor[1] +
                                     outMinor[2] * outMinor[2]);
    if (minorLen > 1.0e-5f)
    {
        outMinor[0] /= minorLen;
        outMinor[1] /= minorLen;
        outMinor[2] /= minorLen;
        return;
    }
    outMinor[0] = cam.right[0];
    outMinor[1] = cam.right[1];
    outMinor[2] = cam.right[2];
}

int FindBestShortEdgePair(const QrVertex quad[4], float outEdgeLenSq[6])
{
    static constexpr uint32_t kEdgeVerts[6][2] = {
        { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 },
    };
    for (int e = 0; e < 6; e++)
    {
        const QrVertex &a = quad[kEdgeVerts[e][0]];
        const QrVertex &b = quad[kEdgeVerts[e][1]];
        const float dx = a.position[0] - b.position[0];
        const float dy = a.position[1] - b.position[1];
        const float dz = a.position[2] - b.position[2];
        outEdgeLenSq[e] = dx * dx + dy * dy + dz * dz;
    }
    int bestPair = 0;
    float bestSum = outEdgeLenSq[0] + outEdgeLenSq[5];
    for (int p = 1; p < 3; p++)
    {
        const float sum = outEdgeLenSq[p] + outEdgeLenSq[5 - p];
        if (sum < bestSum)
        {
            bestSum = sum;
            bestPair = p;
        }
    }
    return bestPair;
}

void DeformAutoSprite2Quad(QrVertex quad[4], const uint32_t quadIdx[6], uint32_t baseVertex,
                           const EditorCameraBasis &cam)
{
    static constexpr uint32_t kEdgeVerts[6][2] = {
        { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 },
    };
    float edgeLenSq[6] = {};
    const int bestPair = FindBestShortEdgePair(quad, edgeLenSq);
    const int shortEdges[2] = { bestPair, 5 - bestPair };
    float mid[2][3] = {};
    for (int j = 0; j < 2; j++)
    {
        const QrVertex &a = quad[kEdgeVerts[shortEdges[j]][0]];
        const QrVertex &b = quad[kEdgeVerts[shortEdges[j]][1]];
        for (int c = 0; c < 3; c++)
        {
            mid[j][c] = 0.5f * (a.position[c] + b.position[c]);
        }
    }

    float minor[3] = {};
    ComputeQuadMinorAxis(mid, cam, minor);
    for (int j = 0; j < 2; j++)
    {
        const uint32_t aLocal = kEdgeVerts[shortEdges[j]][0];
        const uint32_t bLocal = kEdgeVerts[shortEdges[j]][1];
        const float halfLen = 0.5f * std::sqrt(edgeLenSq[shortEdges[j]]);
        bool foundOrder = false;
        for (int k = 0; k < 5 && !foundOrder; k++)
        {
            foundOrder = (quadIdx[k] == baseVertex + aLocal && quadIdx[k + 1] == baseVertex + bLocal);
        }
        const float dirSign = foundOrder ? -1.0f : 1.0f;
        for (int c = 0; c < 3; c++)
        {
            quad[aLocal].position[c] = mid[j][c] + dirSign * halfLen * minor[c];
            quad[bLocal].position[c] = mid[j][c] - dirSign * halfLen * minor[c];
        }
    }
    for (int i = 0; i < 4; i++)
    {
        quad[i].normal[0] = -cam.forward[0];
        quad[i].normal[1] = -cam.forward[1];
        quad[i].normal[2] = -cam.forward[2];
    }
}

void DeformQuadInPlace(QrVertex quad[4], const uint32_t quadIdx[6], uint32_t baseVertex,
                       bool autoSprite2, const EditorCameraBasis &cam)
{
    if (!autoSprite2)
    {
        DeformAutoSpriteQuad(quad, cam);
        return;
    }
    DeformAutoSprite2Quad(quad, quadIdx, baseVertex, cam);
}

void SetSequentialQuadIndices(uint32_t *dst, uint32_t base)
{
    dst[0] = base + 0u;
    dst[1] = base + 1u;
    dst[2] = base + 2u;
    dst[3] = base + 0u;
    dst[4] = base + 2u;
    dst[5] = base + 3u;
}

bool AreSequentialQuads(const std::vector<QrVertex> &verts, const std::vector<uint32_t> &indices)
{
    if (verts.size() % 4u != 0u || indices.size() != (verts.size() / 4u) * 6u)
    {
        return false;
    }
    for (size_t q = 0; q < verts.size() / 4u; q++)
    {
        const uint32_t base = uint32_t(q * 4u);
        for (size_t k = 0; k < 6u; k++)
        {
            const uint32_t idx = indices[q * 6u + k];
            if (idx < base || idx >= base + 4u)
            {
                return false;
            }
        }
    }
    return true;
}

struct ExpandedMeshBuffers
{
    std::vector<QrVertex> verts;
    std::vector<uint32_t> indices;
};

bool CollectUniqueQuadVertices(const std::vector<uint32_t> &indices, size_t offset,
                               size_t vertCount, uint32_t unique[6], size_t &uniqueCount)
{
    uniqueCount = 0;
    for (size_t j = 0; j < 6u; j++)
    {
        const uint32_t idx = indices[offset + j];
        if (idx >= vertCount)
        {
            return false;
        }
        bool seen = false;
        for (size_t u = 0; u < uniqueCount && !seen; u++)
        {
            seen = (unique[u] == idx);
        }
        if (!seen)
        {
            unique[uniqueCount++] = idx;
        }
    }
    return true;
}

void AppendExpandedQuadGroup(const std::vector<QrVertex> &verts,
                             const std::vector<uint32_t> &indices, size_t offset,
                             bool autoSprite2, const EditorCameraBasis &cam,
                             ExpandedMeshBuffers &dst)
{
    uint32_t unique[6] = {};
    size_t uniqueCount = 0;
    if (!CollectUniqueQuadVertices(indices, offset, verts.size(), unique, uniqueCount))
    {
        return;
    }

    const uint32_t base = uint32_t(dst.verts.size());
    if (uniqueCount != 4u)
    {
        for (size_t j = 0; j < 6u; j++)
        {
            dst.verts.push_back(verts[indices[offset + j]]);
            dst.indices.push_back(base + uint32_t(j));
        }
        return;
    }

    QrVertex quad[4];
    uint32_t quadIdx[6] = {};
    for (int u = 0; u < 4; u++)
    {
        quad[u] = verts[unique[u]];
    }
    for (int j = 0; j < 6; j++)
    {
        const uint32_t srcIdx = indices[offset + j];
        for (uint32_t u = 0; u < 4u; u++)
        {
            if (unique[u] == srcIdx)
            {
                quadIdx[j] = base + u;
                break;
            }
        }
    }
    DeformQuadInPlace(quad, quadIdx, base, autoSprite2, cam);
    dst.verts.insert(dst.verts.end(), std::begin(quad), std::end(quad));
    if (!autoSprite2)
    {
        const size_t idxStart = dst.indices.size();
        dst.indices.resize(idxStart + 6u);
        SetSequentialQuadIndices(&dst.indices[idxStart], base);
    }
    else
    {
        dst.indices.insert(dst.indices.end(), std::begin(quadIdx), std::end(quadIdx));
    }
}

void ApplyOverlaySingleTcMod(const EditorTcMod &mod, float timeSeconds, std::vector<QrVertex> &out)
{
    switch (mod.type)
    {
    case EditorTcModType::Scale:
        for (QrVertex &v : out)
        {
            v.texCoord[0] *= mod.params[0];
            v.texCoord[1] *= mod.params[1];
        }
        break;
    case EditorTcModType::Scroll:
    {
        const float du = mod.params[0] * timeSeconds;
        const float dv = mod.params[1] * timeSeconds;
        for (QrVertex &v : out)
        {
            v.texCoord[0] += du;
            v.texCoord[1] += dv;
        }
        break;
    }
    case EditorTcModType::Rotate:
    {
        const float rad = -mod.params[0] * timeSeconds * (kOverlayPi / 180.0f);
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        for (QrVertex &v : out)
        {
            const float du = v.texCoord[0] - 0.5f;
            const float dv = v.texCoord[1] - 0.5f;
            v.texCoord[0] = du * c - dv * s + 0.5f;
            v.texCoord[1] = du * s + dv * c + 0.5f;
        }
        break;
    }
    case EditorTcModType::Stretch:
    {
        const float w = EvaluateEditorWave(mod.wave, mod.params[0], mod.params[1], mod.params[2],
                                           mod.params[3], timeSeconds);
        const float inv = 1.0f / std::max(0.001f, w);
        for (QrVertex &v : out)
        {
            v.texCoord[0] = (v.texCoord[0] - 0.5f) * inv + 0.5f;
            v.texCoord[1] = (v.texCoord[1] - 0.5f) * inv + 0.5f;
        }
        break;
    }
    case EditorTcModType::Turb:
    {
        const float amp = mod.params[1];
        const float now = mod.params[2] + timeSeconds * mod.params[3];
        for (QrVertex &v : out)
        {
            v.texCoord[0] +=
                std::sin(((v.position[0] + v.position[2]) * 0.001f * mod.params[3] + now) * 2.0f *
                         kOverlayPi) *
                amp;
            v.texCoord[1] +=
                std::sin((v.position[1] * 0.001f * mod.params[3] + now) * 2.0f * kOverlayPi) * amp;
        }
        break;
    }
    }
}

void ApplyOverlayTcMods(const EditorOverlayStage &stage, float timeSeconds,
                        const std::vector<QrVertex> &source, std::vector<QrVertex> &out)
{
    out = source;
    for (const EditorTcMod &mod : stage.tcMods)
    {
        ApplyOverlaySingleTcMod(mod, timeSeconds, out);
    }
}

std::string ActiveOverlayStageTexture(const EditorOverlayStage &stage, float timeSeconds)
{
    if (stage.animationMaps.empty() || stage.animationFps <= 0.0f)
    {
        return stage.map;
    }
    const size_t frame =
        size_t(std::floor(std::max(timeSeconds, 0.0f) * stage.animationFps)) %
        stage.animationMaps.size();
    return stage.animationMaps[frame];
}

bool IsSimpleAlphaBlendOverlay(const EditorOverlayStage &stage, float surfaceLight)
{
    if (stage.animated || stage.isEnvironment || stage.rgbWave != EditorWaveType::None ||
        stage.alphaWave != EditorWaveType::None || surfaceLight > 0.0f)
    {
        return false;
    }
    const bool normalAlpha = stage.blendSrc == QR_BLEND_FACTOR_SRC_ALPHA &&
                             stage.blendDst == QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    const bool invAlpha = stage.blendSrc == QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
                          stage.blendDst == QR_BLEND_FACTOR_SRC_ALPHA;
    return normalAlpha || invAlpha;
}

bool ShouldSkipTracedOverlayStage(const EditorOverlayUploadInput &input,
                                   const std::string &baseMap, size_t stageIndex,
                                   const EditorOverlayStage &stage, bool isAdditive)
{
    if (input.explicitEmissiveStage >= 0 && int32_t(stageIndex) == input.explicitEmissiveStage)
    {
        return false;
    }
    if (input.metallicEnvBase)
    {
        return !isAdditive || stageIndex <= input.baseStageIndex;
    }
    if (!input.tracedBase)
    {
        return false;
    }
    if (stage.blendSrc == QR_BLEND_FACTOR_DST_COLOR && stage.blendDst == QR_BLEND_FACTOR_ZERO)
    {
        const std::string stageKey = NormalizeEditorAssetPath(stage.map);
        if (stageKey == NormalizeEditorAssetPath(baseMap) || stageKey == "textures/sfx/shadow")
        {
            return true;
        }
    }
    return IsSimpleAlphaBlendOverlay(stage, input.surfaceLight);
}

bool ComputeStageBlendAndState(const EditorOverlayUploadInput &input,
                               const EditorOverlayStage &stage, QrBlendFactor &source,
                               QrBlendFactor &destination, uint32_t &state)
{
    source = stage.blendSrc;
    destination = stage.blendDst;
    state = QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST;
    if (stage.blended)
    {
        state |= QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE;
    }
    else
    {
        if (!input.cutoutUnderlay)
        {
            state |= QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE;
        }
        source = QR_BLEND_FACTOR_ONE;
        destination = QR_BLEND_FACTOR_ZERO;
    }
    if (stage.alphaTested)
    {
        state |= QR_RASTERIZED_GEOMETRY_STATE_ALPHA_TEST;
    }
    return stage.additive ||
           (source == QR_BLEND_FACTOR_ONE && destination == QR_BLEND_FACTOR_ONE) ||
           (source == QR_BLEND_FACTOR_SRC_ALPHA && destination == QR_BLEND_FACTOR_ONE);
}

void PrepareOverlayStageVertices(const EditorOverlayUploadInput &input,
                               const EditorOverlayStage &stage, const EditorCameraBasis &cam,
                               float timeSeconds, std::vector<QrVertex> &stageVertices,
                               std::vector<QrVertex> &modVertices)
{
    stageVertices = *input.vertices;
    if (stage.isEnvironment)
    {
        for (QrVertex &v : stageVertices)
        {
            ComputeEnvironmentTexCoord(cam, v.position, v.normal, v.texCoord);
        }
    }
    if (!stage.tcMods.empty())
    {
        ApplyOverlayTcMods(stage, timeSeconds, stageVertices, modVertices);
        stageVertices.swap(modVertices);
    }

    const bool keepRgb =
        !input.cutoutUnderlay &&
        (stage.rgbGen == EditorColorGen::Vertex || stage.rgbGen == EditorColorGen::Entity);
    const bool keepAlpha =
        stage.alphaGen == EditorAlphaGen::Vertex || stage.alphaGen == EditorAlphaGen::Entity;
    for (QrVertex &v : stageVertices)
    {
        const uint32_t c = v.packedColor;
        const uint32_t rgb = keepRgb ? (c & 0x00FFFFFFu) : 0x00FFFFFFu;
        const uint32_t a = keepAlpha ? (c & 0xFF000000u) : 0xFF000000u;
        v.packedColor = rgb | a;
        v.position[0] = EditorMetersLength(v.position[0]);
        v.position[1] = EditorMetersLength(v.position[1]);
        v.position[2] = EditorMetersLength(v.position[2]);
    }
}

struct OverlayStageUploadState
{
    QrInstance instance = nullptr;
    const EditorOverlayUploadInput *input = nullptr;
    const EditorCameraBasis *cam = nullptr;
    float timeSeconds = 0.0f;
    const std::string *baseMap = nullptr;
    const std::function<QrMaterial(const std::string &, bool, bool, bool)> *resolveMat = nullptr;
    std::vector<QrVertex> *stageVertices = nullptr;
    std::vector<QrVertex> *modVertices = nullptr;
};

bool ShouldAddScreenEmissionToOverlayStage(const EditorOverlayUploadInput &input, size_t stageIndex,
                                           const EditorOverlayStage &stage, bool isAdditive)
{
    const bool waveNotDim = stage.rgbWave == EditorWaveType::None ||
                            (stage.rgbWaveBase + std::fabs(stage.rgbWaveAmplitude)) > 0.35f;
    if (input.explicitEmissiveStage >= 0)
    {
        return int32_t(stageIndex) == input.explicitEmissiveStage && waveNotDim;
    }
    const bool isSelfLitOpaque =
        !stage.blended && (input.surfaceLight > 0.0f || input.cutoutUnderlay);
    return ((isAdditive && !stage.isEnvironment) || isSelfLitOpaque) && waveNotDim;
}

bool IsOverlayStageEmissive(const EditorOverlayUploadInput &input, size_t stageIndex,
                            const EditorOverlayStage &stage, bool isAdditive)
{
    if (input.explicitEmissiveStage >= 0)
    {
        return int32_t(stageIndex) == input.explicitEmissiveStage;
    }
    return (isAdditive && !stage.isEnvironment) || input.cutoutUnderlay ||
           (!stage.blended && input.surfaceLight > 0.0f);
}

float ComputeOverlayStageRgbScale(const EditorOverlayUploadInput &input, size_t stageIndex,
                                  const EditorOverlayStage &stage, bool isAdditive,
                                  float timeSeconds)
{
    const float rgbWave = EvaluateEditorWave(stage.rgbWave, stage.rgbWaveBase,
                                             stage.rgbWaveAmplitude, stage.rgbWavePhase,
                                             stage.rgbWaveFrequency, timeSeconds);
    const float gain = (isAdditive || (!stage.blended && input.surfaceLight > 0.0f)) ? 1.5f : 1.0f;
    if (IsOverlayStageEmissive(input, stageIndex, stage, isAdditive))
    {
        const float baseFactor =
            (input.tracedBase && !input.cutoutUnderlay) ? 1.0f : input.colorScale;
        return rgbWave * gain * baseFactor * input.emissiveScale;
    }
    return rgbWave * gain * input.colorScale;
}

const std::string &ResolveOverlayStageTextureName(const EditorOverlayUploadInput &input,
                                                  size_t stageIndex,
                                                  const EditorOverlayStage &stage,
                                                  bool isAdditive, const std::string &activeTexture)
{
    if (input.explicitEmissiveStage >= 0 || stage.animated ||
        input.emissiveTextureOverride.empty() ||
        !IsOverlayStageEmissive(input, stageIndex, stage, isAdditive))
    {
        return activeTexture;
    }
    const std::string activeStem =
        StripEditorImageExtension(NormalizeEditorAssetPath(activeTexture));
    const std::string defaultStem =
        StripEditorImageExtension(NormalizeEditorAssetPath(input.defaultEmissiveTexture));
    return (!defaultStem.empty() && activeStem == defaultStem) ? input.emissiveTextureOverride
                                                               : activeTexture;
}

bool ShouldUploadDynamicEmissiveStage(const EditorOverlayUploadInput &input, bool addScreenEmission,
                                      float rgbScale)
{
    if (input.overlaySurfaceIndex == (std::numeric_limits<size_t>::max)() || input.isDtalLight ||
        !addScreenEmission || input.emissiveScale <= 1.0e-4f || rgbScale <= 1.0e-4f)
    {
        return false;
    }
    return !input.tracedBase || input.cutoutUnderlay || input.hasAnimatedEmissiveStage;
}

void AppendBackfacingOverlayTriangles(const std::vector<QrVertex> &srcVertices,
                                      const std::vector<uint32_t> &srcIndices, float offsetMeters,
                                      std::vector<QrVertex> &rayVerts,
                                      std::vector<uint32_t> &rayIndices)
{
    const uint32_t baseIdx = uint32_t(rayVerts.size());
    for (const QrVertex &src : srcVertices)
    {
        QrVertex v = src;
        v.position[0] -= v.normal[0] * offsetMeters;
        v.position[1] -= v.normal[1] * offsetMeters;
        v.position[2] -= v.normal[2] * offsetMeters;
        v.normal[0] = -v.normal[0];
        v.normal[1] = -v.normal[1];
        v.normal[2] = -v.normal[2];
        rayVerts.push_back(v);
    }
    const size_t idxCount = srcIndices.size();
    rayIndices.reserve(idxCount * 2u);
    for (size_t i = 0; i + 2u < idxCount; i += 3u)
    {
        rayIndices.push_back(baseIdx + srcIndices[i + 0u]);
        rayIndices.push_back(baseIdx + srcIndices[i + 2u]);
        rayIndices.push_back(baseIdx + srcIndices[i + 1u]);
    }
}

void UploadDynamicEmissiveOverlayStage(const OverlayStageUploadState &ctx, size_t stageIndex,
                                       const EditorOverlayStage &stage, QrMaterial material,
                                       bool isAdditive, bool addScreenEmission, float rgbScale)
{
    const EditorOverlayUploadInput &input = *ctx.input;
    if (!ShouldUploadDynamicEmissiveStage(input, addScreenEmission, rgbScale))
    {
        return;
    }

    const float stageOffsetMeters = float(stageIndex) * 0.00125f;
    std::vector<QrVertex> rayVerts;
    const bool duplicateBackfaces = input.twoSided || !input.tracedBase;
    rayVerts.reserve(duplicateBackfaces ? ctx.stageVertices->size() * 2u
                                        : ctx.stageVertices->size());
    for (const QrVertex &src : *ctx.stageVertices)
    {
        QrVertex v = src;
        v.position[0] += v.normal[0] * stageOffsetMeters;
        v.position[1] += v.normal[1] * stageOffsetMeters;
        v.position[2] += v.normal[2] * stageOffsetMeters;
        rayVerts.push_back(v);
    }

    std::vector<uint32_t> rayIndices = *input.indices;
    if (duplicateBackfaces)
    {
        AppendBackfacingOverlayTriangles(*ctx.stageVertices, *input.indices, stageOffsetMeters,
                                         rayVerts, rayIndices);
    }

    const float emissionScale = std::max(0.0f, rgbScale);
    QrGeometryUploadInfo info = {};
    info.uniqueID =
        0x50000000ull + (uint64_t(input.overlaySurfaceIndex) << 8u) + uint64_t(stageIndex & 0xFFu);
    info.flags = QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT | QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT;
    info.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    info.passThroughType = (isAdditive || stage.alphaTested || stage.blended)
                               ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED
                               : QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    info.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER;
    info.vertexCount = uint32_t(rayVerts.size());
    info.pVertices = rayVerts.data();
    info.indexCount = uint32_t(rayIndices.size());
    info.pIndices = rayIndices.data();
    info.layerColors[0] = { emissionScale, emissionScale, emissionScale, 1.0f };
    info.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    info.texCoordScale[0] = 1.0f;
    info.texCoordScale[1] = 1.0f;
    info.defaultRoughness = kEditorDefaultRoughness;
    info.defaultEmission = std::clamp(emissionScale, 0.0f, 1.0f);
    info.geomMaterial.layerMaterials[0] = material;
    info.transform.matrix[0][0] = 1.0f;
    info.transform.matrix[1][1] = 1.0f;
    info.transform.matrix[2][2] = 1.0f;
    qrUploadGeometry(ctx.instance, &info);
}

void UploadSingleOverlayStage(const OverlayStageUploadState &ctx, size_t stageIndex)
{
    const EditorOverlayUploadInput &input = *ctx.input;
    if (stageIndex < 32u && ((input.disabledStagesMask >> stageIndex) & 1u) != 0u)
    {
        return;
    }

    const EditorOverlayStage &stage = (*input.stages)[stageIndex];
    if (input.suppressEnvironmentStages && stage.isEnvironment)
    {
        return;
    }
    if (input.promotedOverlayToTraced && stageIndex == input.baseStageIndex)
    {
        return;
    }
    const std::string activeTexture = ActiveOverlayStageTexture(stage, ctx.timeSeconds);
    if (activeTexture.empty() || stage.isLightmap)
    {
        return;
    }

    QrBlendFactor source = QR_BLEND_FACTOR_ONE;
    QrBlendFactor destination = QR_BLEND_FACTOR_ZERO;
    uint32_t state = 0;
    const bool isAdditive = ComputeStageBlendAndState(input, stage, source, destination, state);
    if (!input.cutoutUnderlay &&
        ShouldSkipTracedOverlayStage(input, *ctx.baseMap, stageIndex, stage, isAdditive))
    {
        return;
    }

    const float rgbScale =
        ComputeOverlayStageRgbScale(input, stageIndex, stage, isAdditive, ctx.timeSeconds);
    if (rgbScale <= 1.0e-4f)
    {
        return;
    }

    const std::string &textureName =
        ResolveOverlayStageTextureName(input, stageIndex, stage, isAdditive, activeTexture);
    const bool addScreenEmission =
        ShouldAddScreenEmissionToOverlayStage(input, stageIndex, stage, isAdditive);
    const QrMaterial material = (*ctx.resolveMat)(textureName, stage.clampMap,
                                                  isAdditive && !stage.isEnvironment,
                                                  addScreenEmission);
    if (material == QR_NO_MATERIAL)
    {
        return;
    }

    PrepareOverlayStageVertices(input, stage, *ctx.cam, ctx.timeSeconds, *ctx.stageVertices,
                                *ctx.modVertices);

    const float alphaWave = EvaluateEditorWave(stage.alphaWave, stage.alphaWaveBase,
                                               stage.alphaWaveAmplitude, stage.alphaWavePhase,
                                               stage.alphaWaveFrequency, ctx.timeSeconds);

    QrRasterizedGeometryUploadInfo info = {};
    info.renderType = QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT;
    info.vertexCount = uint32_t(ctx.stageVertices->size());
    info.pVertices = ctx.stageVertices->data();
    info.indexCount = uint32_t(input.indices->size());
    info.pIndices = input.indices->data();
    info.transform.matrix[0][0] = 1.0f;
    info.transform.matrix[1][1] = 1.0f;
    info.transform.matrix[2][2] = 1.0f;
    info.color = { rgbScale, rgbScale, rgbScale, alphaWave * input.alphaScale };
    info.texCoordScale[0] = 1.0f;
    info.texCoordScale[1] = 1.0f;
    info.material = material;
    info.pipelineState = state;
    info.blendFuncSrc = source;
    info.blendFuncDst = destination;
    qrUploadRasterizedGeometry(ctx.instance, &info, nullptr, nullptr);
    UploadDynamicEmissiveOverlayStage(ctx, stageIndex, stage, material, isAdditive,
                                      addScreenEmission, rgbScale);
}

struct OverlayOrderItem
{
    int group = 0;
    float distSq = 0.0f;
    size_t index = 0;
};

float ComputeOverlayCentroidDistSq(const std::vector<QrVertex> &vertices,
                                     const EditorCameraBasis &cam)
{
    double cx = 0.0;
    double cy = 0.0;
    double cz = 0.0;
    for (const QrVertex &v : vertices)
    {
        cx += v.position[0];
        cy += v.position[1];
        cz += v.position[2];
    }
    const float inv = 1.0f / float(vertices.size());
    const float dx = float(cx) * inv - cam.origin[0];
    const float dy = float(cy) * inv - cam.origin[1];
    const float dz = float(cz) * inv - cam.origin[2];
    return dx * dx + dy * dy + dz * dz;
}

std::vector<OverlayOrderItem> BuildOverlayOrder(const std::vector<EditorOverlaySurface> &surfaces,
                                                const EditorCameraBasis &cam)
{
    std::vector<OverlayOrderItem> order;
    order.reserve(surfaces.size());
    for (size_t i = 0; i < surfaces.size(); i++)
    {
        const EditorOverlaySurface &surf = surfaces[i];
        if (surf.vertices.empty() || surf.indices.empty() || surf.stages.empty())
        {
            continue;
        }
        const int group = surf.cutoutUnderlay ? 0 : (surf.tracedBase ? 1 : 2);
        const float distSq =
            (group == 2) ? ComputeOverlayCentroidDistSq(surf.vertices, cam) : 0.0f;
        order.push_back({ group, distSq, i });
    }

    std::stable_sort(order.begin(), order.end(),
                     [](const OverlayOrderItem &a, const OverlayOrderItem &b) {
                         if (a.group != b.group)
                         {
                             return a.group < b.group;
                         }
                         if (a.group == 2)
                         {
                             return a.distSq > b.distSq;
                         }
                         return a.index < b.index;
                     });
    return order;
}

bool DefaultOverlayUsesAutoEmissive(const EditorTexture *tex, bool hasAnimatedEmissiveStage)
{
    return tex != nullptr && !tex->isNonTracedOverlay &&
           (tex->isEmissiveShader || !tex->defaultEmissiveTexture.empty() ||
            !tex->fullbright.empty() || hasAnimatedEmissiveStage);
}

struct OverlayMaterialScale
{
    float colorScale = 1.0f;
    float alphaScale = 1.0f;
    float emissiveScale = 1.0f;
    int32_t explicitEmissiveStage = -1;
    uint32_t disabledStagesMask = 0;
    bool isDtalLight = false;
    bool hasAnimatedEmissiveStage = false;
    bool twoSided = false;
    bool suppressEnvironmentStages = false;
    bool promotedOverlayToTraced = false;
    bool baseStageDisabled = false;
    std::string defaultEmissiveTexture;
    std::string emissiveOverride;
};

int32_t ResolveExplicitOverlayEmissiveStage(const EditorTexture *tex,
                                          const std::string &emissiveTexture)
{
    if (tex == nullptr || emissiveTexture.empty())
    {
        return -1;
    }
    const int32_t stageIdx = FindEditorMatchingStageIndexInTexture(tex, emissiveTexture);
    return (stageIdx >= 0 && size_t(stageIdx) != tex->baseStageIndex) ? stageIdx : -1;
}

bool ComputeOverlayDtalEnabled(bool truelight, bool emissiveNee, const EditorTexture *tex,
                               const EditorMaterialParams &matParams)
{
    if (!truelight)
    {
        return false;
    }
    if (EditorSurfaceUsesDtalLighting(1.0f, matParams))
    {
        return true;
    }
    return emissiveNee && EditorTextureHasAutoEmissiveSource(tex, matParams);
}

void ResolveOverlayEmissive(const EditorMaterialParams &matParams, const EditorTexture *tex,
                            OverlayMaterialScale &out)
{
    if (!matParams.emissiveTexture.empty())
    {
        out.explicitEmissiveStage =
            ResolveExplicitOverlayEmissiveStage(tex, matParams.emissiveTexture);
        out.emissiveScale = matParams.emissiveFactor;
        out.emissiveOverride = matParams.emissiveTexture;
        return;
    }

    const bool hasDefaultEmissive = !out.defaultEmissiveTexture.empty();
    out.emissiveScale =
        (hasDefaultEmissive && !matParams.hasColorEmissive) ? 0.0f : matParams.emissiveFactor;
}

void ApplyOverlayTextureMetadata(const EditorTexture *tex, uint32_t disabledStagesMask,
                                 OverlayMaterialScale &out)
{
    if (tex == nullptr)
    {
        return;
    }
    out.baseStageDisabled = tex->baseStageIndex < tex->shaderStages.size() &&
                            tex->baseStageIndex < 32u &&
                            ((disabledStagesMask >> tex->baseStageIndex) & 1u) != 0u;
    out.hasAnimatedEmissiveStage =
        EditorTextureHasAnimatedEmissiveStage(*tex, disabledStagesMask);
}

bool ResolveOverlayMaterialScale(const EditorMaterialTable &materials, const EditorScene &scene,
                                 bool truelight, bool emissiveNee, const std::string &shaderName,
                                 OverlayMaterialScale &out)
{
    out = {};
    const EditorTexture *tex = FindEditorSceneTextureByName(scene, shaderName);
    if (tex != nullptr)
    {
        out.defaultEmissiveTexture = tex->defaultEmissiveTexture;
        out.twoSided = tex->twoSided;
        out.hasAnimatedEmissiveStage = EditorTextureHasAnimatedEmissiveStage(*tex, 0u);
    }

    const int32_t matIdx = materials.IndexOf(shaderName);
    if (matIdx < 0)
    {
        out.isDtalLight = truelight && emissiveNee &&
                          DefaultOverlayUsesAutoEmissive(tex, out.hasAnimatedEmissiveStage);
        return true;
    }

    const EditorMaterialParams &matParams = materials.At(size_t(matIdx));
    if (!matParams.enabled)
    {
        return false;
    }

    out.colorScale = matParams.baseFactor;
    out.alphaScale = (matParams.type == EditorMaterialType::Glass) ? matParams.transparency : 1.0f;
    out.disabledStagesMask = matParams.disabledStagesMask;
    out.suppressEnvironmentStages =
        matParams.mirror && matParams.type == EditorMaterialType::None;
    out.promotedOverlayToTraced = (tex != nullptr && tex->isNonTracedOverlay) &&
                                  EditorMaterialPromotesOverlayToTraced(matParams);
    out.isDtalLight = ComputeOverlayDtalEnabled(truelight, emissiveNee, tex, matParams);
    ApplyOverlayTextureMetadata(tex, matParams.disabledStagesMask, out);
    ResolveOverlayEmissive(matParams, tex, out);
    return true;
}

EditorOverlayUploadInput BuildOverlayUploadInput(const EditorOverlaySurface &surf,
                                                 const EditorCameraBasis &cam, float timeSeconds,
                                                 std::vector<QrVertex> &deformedVerts,
                                                 std::vector<uint32_t> &deformedIndices)
{
    const bool needsDeform =
        surf.deformWave != EditorWaveType::None || surf.autoSprite || surf.autoSprite2;
    const std::vector<QrVertex> *vertsPtr = &surf.vertices;
    const std::vector<uint32_t> *indicesPtr = &surf.indices;
    if (needsDeform)
    {
        deformedVerts = surf.vertices;
        deformedIndices = surf.indices;
        const EditorWaveDeformParams deform{
            surf.deformWave,      surf.deformSpread, surf.deformBase,
            surf.deformAmplitude, surf.deformPhase,  surf.deformFrequency,
        };
        DeformEditorWaveVertices(deformedVerts, deform, timeSeconds);
        DeformEditorAutoSpriteVertices(deformedVerts, deformedIndices, surf.autoSprite,
                                       surf.autoSprite2, cam);
        vertsPtr = &deformedVerts;
        indicesPtr = &deformedIndices;
    }

    EditorOverlayUploadInput input;
    input.cutoutUnderlay = surf.cutoutUnderlay;
    input.tracedBase = surf.tracedBase;
    input.metallicEnvBase = surf.metallicEnvBase;
    input.surfaceLight = surf.surfaceLight;
    input.baseStageIndex = surf.baseStageIndex;
    input.firstStage = surf.firstOverlayStage;
    input.endStage = surf.endOverlayStage;
    input.stages = &surf.stages;
    input.vertices = vertsPtr;
    input.indices = indicesPtr;
    return input;
}

} // namespace

void EditorOverlayCache::Destroy(QrInstance instance)
{
    if (instance != nullptr)
    {
        if (m_cutoutBackingMaterial != QR_NO_MATERIAL)
        {
            qrDestroyMaterial(instance, m_cutoutBackingMaterial);
        }
        for (auto &kv : m_overlayMaterials)
        {
            if (kv.second != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(instance, kv.second);
            }
        }
    }
    ResetHandlesOnly();
}

void EditorOverlayCache::ResetHandlesOnly()
{
    m_cutoutBackingMaterial = QR_NO_MATERIAL;
    m_overlayMaterials.clear();
}

QrMaterial EditorOverlayCache::GetCutoutBackingMaterial(QrInstance instance)
{
    if (instance == nullptr)
    {
        return QR_NO_MATERIAL;
    }
    if (m_cutoutBackingMaterial != QR_NO_MATERIAL)
    {
        return m_cutoutBackingMaterial;
    }

    static constexpr uint32_t kSize = 4u;
    std::vector<uint8_t> albedo(size_t(kSize) * size_t(kSize) * 4u);
    std::vector<uint8_t> rme(size_t(kSize) * size_t(kSize) * 4u);
    for (size_t i = 0; i < size_t(kSize) * size_t(kSize); i++)
    {
        albedo[i * 4u + 0u] = 8u;
        albedo[i * 4u + 1u] = 8u;
        albedo[i * 4u + 2u] = 8u;
        albedo[i * 4u + 3u] = 255u;
        rme[i * 4u + 0u] = 230u;
        rme[i * 4u + 1u] = 0u;
        rme[i * 4u + 2u] = 0u;
        rme[i * 4u + 3u] = 0u;
    }

    QrMaterialCreateInfo info = {};
    info.size = { kSize, kSize };
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.textures.pDataAlbedoAlpha = albedo.data();
    info.textures.pDataRoughnessMetallicEmission = rme.data();
    qrCreateMaterial(instance, &info, &m_cutoutBackingMaterial);
    return m_cutoutBackingMaterial;
}

QrMaterial EditorOverlayCache::GetOverlayStageMaterial(
    QrInstance instance, const EditorAssetSource &assets, const EditorScene &scene,
    const EditorOverlayMaterialRequest &request)
{
    if (instance == nullptr || request.imageName.empty())
    {
        return QR_NO_MATERIAL;
    }

    const std::string key = NormalizeEditorAssetPath(request.imageName) +
                            (request.clampMap ? "|c" : "|r") +
                            (request.cleanAdditiveBlack ? "|a" : "|n") +
                            (request.addScreenEmission ? "|e" : "|0");
    const auto cached = m_overlayMaterials.find(key);
    if (cached != m_overlayMaterials.end())
    {
        return cached->second;
    }

    std::vector<uint8_t> rgba;
    uint32_t width = 0;
    uint32_t height = 0;
    QrMaterial mat = QR_NO_MATERIAL;
    std::vector<uint8_t> rme;
    if (LoadOverlayMaterialPixels(assets, scene, request.extraTextureLists, request.imageName, rgba,
                                  width, height))
    {
        if (request.cleanAdditiveBlack)
        {
            CleanAdditiveBlackBackground(rgba);
        }
        QrMaterialCreateInfo info = {};
        info.size = { width, height };
        info.filter = QR_SAMPLER_FILTER_LINEAR;
        info.addressModeU = request.clampMap ? QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : QR_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = request.clampMap ? QR_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                             : QR_SAMPLER_ADDRESS_MODE_REPEAT;
        info.textures.pDataAlbedoAlpha = rgba.data();
        if (request.addScreenEmission)
        {
            BuildOverlayScreenEmissionRme(rgba, size_t(width) * size_t(height), rme);
            info.textures.pDataRoughnessMetallicEmission = rme.data();
        }
        qrCreateMaterial(instance, &info, &mat);
    }

    m_overlayMaterials.emplace(key, mat);
    return mat;
}

bool LoadEditorExternalImageRgba(const EditorAssetSource &assets, const std::string &key,
                                 uint32_t &outWidth, uint32_t &outHeight,
                                 std::vector<uint8_t> &outRgba)
{
    static constexpr const char *kExtensions[] = { ".png", ".tga", ".jpg", ".jpeg" };

    std::vector<std::string> bases;
    AppendExternalPathBases(key, bases);

    for (const std::string &base : bases)
    {
        if (TryReadDecodedImage(assets, base, outWidth, outHeight, outRgba))
        {
            return true;
        }

        std::string stem = base;
        if (HasImageFileExtension(base))
        {
            const size_t dot = base.rfind('.');
            stem = dot != std::string::npos ? base.substr(0, dot) : base;
        }

        for (const char *ext : kExtensions)
        {
            if (TryReadDecodedImage(assets, stem + ext, outWidth, outHeight, outRgba))
            {
                return true;
            }
        }
    }

    return false;
}

bool IsEditorShaderStageAdditive(const EditorOverlayStage &stage)
{
    return stage.additive ||
           (stage.blendSrc == QR_BLEND_FACTOR_ONE && stage.blendDst == QR_BLEND_FACTOR_ONE) ||
           (stage.blendSrc == QR_BLEND_FACTOR_SRC_ALPHA && stage.blendDst == QR_BLEND_FACTOR_ONE);
}

bool IsEditorTextureStageAnimated(const EditorOverlayStage &stage)
{
    return stage.animated || stage.animationMaps.size() > 1u ||
           stage.rgbWave != EditorWaveType::None || stage.alphaWave != EditorWaveType::None ||
           !stage.tcMods.empty();
}

bool EditorTextureHasAnimatedEmissiveStage(const EditorTexture &tex, uint32_t disabledStagesMask)
{
    if (tex.isNonTracedOverlay || tex.isCutoutUnderlay)
    {
        return !tex.shaderStages.empty();
    }
    const bool deformed =
        tex.deformWave != EditorWaveType::None || tex.autoSprite || tex.autoSprite2;
    for (size_t i = 0; i < tex.shaderStages.size() && i < 32u; ++i)
    {
        if (i == tex.baseStageIndex || ((disabledStagesMask >> i) & 1u) != 0u)
        {
            continue;
        }
        const EditorOverlayStage &stage = tex.shaderStages[i];
        if (stage.isLightmap || stage.isWhite || stage.isEnvironment)
        {
            continue;
        }
        if (IsEditorShaderStageAdditive(stage) &&
            (deformed || IsEditorTextureStageAnimated(stage)))
        {
            return true;
        }
    }
    return false;
}

const EditorTexture *FindEditorSceneTextureByName(const EditorScene &scene,
                                                 const std::string &shaderName)
{
    const std::string needle = LowercaseAscii(shaderName);
    for (const EditorTexture &tex : scene.textures)
    {
        if (LowercaseAscii(tex.name) == needle)
        {
            return &tex;
        }
    }
    return nullptr;
}

const EditorTexture *FindEditorSurfaceTexture(const EditorScene &scene,
                                              const EditorSurface &surface)
{
    if (surface.texture >= 0 && size_t(surface.texture) < scene.textures.size())
    {
        return &scene.textures[size_t(surface.texture)];
    }
    return FindEditorSceneTextureByName(scene, surface.textureName);
}

bool EditorTextureHasAutoEmissiveSource(const EditorTexture *tex, const EditorMaterialParams &params)
{
    if (!params.enabled || !params.emissiveEnabled || params.luma ||
        params.emissiveFactor <= 1.0e-4f)
    {
        return false;
    }
    if (tex != nullptr && tex->isNonTracedOverlay)
    {
        return false;
    }
    if (!params.emissiveTexture.empty() ||
        (params.hasColorEmissive && !params.emissiveBlocks.empty()))
    {
        return true;
    }
    if (tex == nullptr || (!tex->defaultEmissiveTexture.empty() && params.emissiveTexture.empty()))
    {
        return false;
    }
    return tex->isEmissiveShader || !tex->fullbright.empty() ||
           EditorTextureHasAnimatedEmissiveStage(*tex, params.disabledStagesMask);
}

bool EditorSurfaceHasAutoEmissiveSource(const EditorScene &scene, const EditorSurface &surface,
                                        const EditorMaterialParams &params)
{
    if (surface.isOverlayOnly || surface.faces.empty() || surface.kind == EditorSurfaceKind::Sky)
    {
        return false;
    }
    return EditorTextureHasAutoEmissiveSource(FindEditorSurfaceTexture(scene, surface), params);
}

int32_t FindEditorMatchingStageIndexInTexture(const EditorTexture *tex, const std::string &texRef)
{
    if (tex == nullptr || tex->shaderStages.empty() || texRef.empty())
    {
        return -1;
    }
    const std::string targetStem = StripEditorImageExtension(NormalizeEditorAssetPath(texRef));
    if (targetStem.empty())
    {
        return -1;
    }
    for (size_t i = 0; i < tex->shaderStages.size(); i++)
    {
        const EditorOverlayStage &st = tex->shaderStages[i];
        if (!st.map.empty() &&
            StripEditorImageExtension(NormalizeEditorAssetPath(st.map)) == targetStem)
        {
            return int32_t(i);
        }
        for (const std::string &animMap : st.animationMaps)
        {
            if (!animMap.empty() &&
                StripEditorImageExtension(NormalizeEditorAssetPath(animMap)) == targetStem)
            {
                return int32_t(i);
            }
        }
    }
    return -1;
}

bool EditorSurfaceEmissiveBelongsToOverlayStage(const EditorScene &scene,
                                                const EditorSurface &surface,
                                                const EditorMaterialParams &params)
{
    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (!params.emissiveEnabled || tex == nullptr || tex->shaderStages.empty())
    {
        return false;
    }
    if (!params.emissiveTexture.empty())
    {
        const int32_t stageIdx = FindEditorMatchingStageIndexInTexture(tex, params.emissiveTexture);
        if (stageIdx < 0)
        {
            return false;
        }
        return size_t(stageIdx) != tex->baseStageIndex &&
               (surface.isCutoutUnderlay || size_t(stageIdx) > tex->baseStageIndex);
    }
    return EditorTextureHasAnimatedEmissiveStage(*tex, params.disabledStagesMask);
}

float ComputeEditorSurfaceEmissiveNormalOffset(const EditorScene &scene,
                                               const EditorSurface &surface,
                                               const EditorMaterialParams &params)
{
    if (!surface.isCutoutUnderlay)
    {
        return 0.0f;
    }
    const EditorTexture *tex = FindEditorSurfaceTexture(scene, surface);
    if (tex == nullptr)
    {
        return 0.0f;
    }
    if (params.emissiveTexture.empty())
    {
        return -0.8f;
    }
    const int32_t stageIdx = FindEditorMatchingStageIndexInTexture(tex, params.emissiveTexture);
    return (stageIdx >= 0 && size_t(stageIdx) < tex->baseStageIndex) ? -0.8f : 0.0f;
}

bool EditorSurfaceUsesDtalLighting(float truelight, const EditorMaterialParams &params)
{
    return params.enabled && truelight > 0.0f && params.luma;
}

EditorSurfaceEmissionUploadMode ResolveEditorSurfaceEmissionUploadMode(
    const EditorScene &scene, const EditorSurface &surface, const EditorMaterialParams &params,
    bool truelightEnabled, bool emissiveNee)
{
    const bool usesDtalLight =
        EditorSurfaceUsesDtalLighting(truelightEnabled ? 1.0f : 0.0f, params);
    const bool usesAutoEmissiveNee =
        truelightEnabled && emissiveNee && EditorSurfaceHasAutoEmissiveSource(scene, surface, params);
    const bool belongsToOverlay =
        !params.hasColorEmissive && EditorSurfaceEmissiveBelongsToOverlayStage(scene, surface, params);
    EditorSurfaceEmissionUploadMode mode;
    mode.suppressBaseSurfaceEmission = belongsToOverlay;
    mode.emitRayOnly = !usesDtalLight && !usesAutoEmissiveNee && !belongsToOverlay;
    return mode;
}

void DeformEditorWaveVertices(std::vector<QrVertex> &verts, const EditorWaveDeformParams &deform,
                              float timeSeconds)
{
    if (deform.wave == EditorWaveType::None)
    {
        return;
    }
    for (QrVertex &v : verts)
    {
        const float phaseOffset =
            deform.frequency != 0.0f
                ? (v.position[0] + v.position[1] + v.position[2]) * deform.spread
                : 0.0f;
        const float disp = EvaluateEditorWaveSigned(deform.wave, deform.base, deform.amplitude,
                                                    deform.phase + phaseOffset, deform.frequency,
                                                    timeSeconds);
        v.position[0] += v.normal[0] * disp;
        v.position[1] += v.normal[1] * disp;
        v.position[2] += v.normal[2] * disp;
    }
}

void DeformEditorAutoSpriteVertices(std::vector<QrVertex> &verts, std::vector<uint32_t> &indices,
                                    bool autoSprite, bool autoSprite2, const EditorCameraBasis &cam)
{
    if ((!autoSprite && !autoSprite2) || verts.size() < 4u || indices.size() < 6u)
    {
        return;
    }

    if (AreSequentialQuads(verts, indices))
    {
        for (size_t q = 0; q < verts.size() / 4u; q++)
        {
            const uint32_t base = uint32_t(q * 4u);
            DeformQuadInPlace(&verts[base], &indices[q * 6u], base, autoSprite2, cam);
            if (!autoSprite2)
            {
                SetSequentialQuadIndices(&indices[q * 6u], base);
            }
        }
        return;
    }

    if (indices.size() % 6u != 0u)
    {
        return;
    }

    ExpandedMeshBuffers expanded;
    expanded.verts.reserve((indices.size() / 6u) * 4u);
    expanded.indices.reserve(indices.size());
    for (size_t k = 0; k + 5u < indices.size(); k += 6u)
    {
        AppendExpandedQuadGroup(verts, indices, k, autoSprite2, cam, expanded);
    }
    if (!expanded.verts.empty() && !expanded.indices.empty())
    {
        verts.swap(expanded.verts);
        indices.swap(expanded.indices);
    }
}

void UploadEditorOverlayStageSet(
    QrInstance instance, const EditorOverlayUploadInput &input, const EditorCameraBasis &cam,
    float timeSeconds,
    const std::function<QrMaterial(const std::string &, bool, bool, bool)> &resolveMat)
{
    if (instance == nullptr || input.stages == nullptr || input.vertices == nullptr ||
        input.indices == nullptr || input.vertices->empty() || input.indices->empty())
    {
        return;
    }

    const size_t firstStage = input.firstStage;
    const size_t endStage = std::min(input.endStage, input.stages->size());
    if (firstStage >= endStage)
    {
        return;
    }

    const std::string baseMap =
        input.baseStageIndex < input.stages->size() ? (*input.stages)[input.baseStageIndex].map : "";
    std::vector<QrVertex> stageVertices;
    std::vector<QrVertex> modVertices;
    const OverlayStageUploadState uploadCtx{
        instance, &input, &cam, timeSeconds, &baseMap, &resolveMat, &stageVertices, &modVertices,
    };

    for (size_t stageIndex = firstStage; stageIndex < endStage; stageIndex++)
    {
        UploadSingleOverlayStage(uploadCtx, stageIndex);
    }
}

void UploadEditorOverlaySurfaces(QrInstance instance,
                                 const EditorOverlaySceneUploadOptions &options,
                                 EditorOverlayCache &cache)
{
    if (instance == nullptr || options.scene == nullptr || options.materials == nullptr ||
        options.assets == nullptr || options.camera == nullptr ||
        options.scene->overlaySurfaces.empty())
    {
        return;
    }

    const EditorScene &scene = *options.scene;
    const EditorCameraBasis &cam = *options.camera;
    const std::vector<OverlayOrderItem> order = BuildOverlayOrder(scene.overlaySurfaces, cam);
    const auto resolveOverlayMat = [&](const std::string &imageName, bool clampMap,
                                       bool cleanAdditiveBlack, bool addScreenEmission) {
        const EditorOverlayMaterialRequest req{
            imageName, clampMap, cleanAdditiveBlack, addScreenEmission, options.extraTextureLists,
        };
        return cache.GetOverlayStageMaterial(instance, *options.assets, scene, req);
    };

    std::vector<QrVertex> deformedVerts;
    std::vector<uint32_t> deformedIndices;
    for (const OverlayOrderItem &item : order)
    {
        const EditorOverlaySurface &surf = scene.overlaySurfaces[item.index];
        OverlayMaterialScale scale;
        if (!ResolveOverlayMaterialScale(*options.materials, scene, options.truelightEnabled,
                                         options.emissiveNee, surf.shaderName, scale))
        {
            continue;
        }

        EditorOverlayUploadInput input = BuildOverlayUploadInput(surf, cam, options.timeSeconds,
                                                                 deformedVerts, deformedIndices);
        input.colorScale = scale.colorScale;
        input.alphaScale = scale.alphaScale;
        input.emissiveScale = scale.emissiveScale;
        input.explicitEmissiveStage = scale.explicitEmissiveStage;
        input.disabledStagesMask = scale.disabledStagesMask;
        input.suppressEnvironmentStages = scale.suppressEnvironmentStages;
        input.promotedOverlayToTraced = scale.promotedOverlayToTraced;
        if (scale.baseStageDisabled && input.cutoutUnderlay)
        {
            input.cutoutUnderlay = false;
        }
        input.isDtalLight = scale.isDtalLight;
        input.hasAnimatedEmissiveStage = scale.hasAnimatedEmissiveStage;
        input.twoSided = scale.twoSided;
        input.overlaySurfaceIndex = item.index;
        input.defaultEmissiveTexture = std::move(scale.defaultEmissiveTexture);
        input.emissiveTextureOverride = std::move(scale.emissiveOverride);
        UploadEditorOverlayStageSet(instance, input, cam, options.timeSeconds, resolveOverlayMat);
    }
}
