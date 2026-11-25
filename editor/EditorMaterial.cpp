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

#include "EditorMaterial.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Scene.h"

namespace
{

// One of the scene's images, addressed the way a bake reads it: the source pixel that maps onto
// a pixel of the baked set. The source is not necessarily the size of the albedo, so a sampling
// step repeats it, matching the repeat address mode the material sampler uses.
struct SourceImage
{
    const uint8_t *rgba = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    bool Valid() const
    {
        return rgba != nullptr && width > 0 && height > 0;
    }

    const uint8_t *At(uint32_t x, uint32_t y, uint32_t widthTo, uint32_t heightTo) const
    {
        const uint32_t sourceX = uint32_t(uint64_t(x) * width / widthTo) % width;
        const uint32_t sourceY = uint32_t(uint64_t(y) * height / heightTo) % height;
        return rgba + (size_t(sourceY) * width + sourceX) * 4;
    }
};

uint32_t WrapSourceCoord(int32_t coord, uint32_t limit)
{
    const int32_t m = coord % int32_t(limit);
    return uint32_t(m < 0 ? (m + int32_t(limit)) : m);
}

void SampleSourceBilinear(const SourceImage &img, uint32_t x, uint32_t y, uint32_t widthTo,
                          uint32_t heightTo, uint8_t outRgba[4])
{
    if (img.width == widthTo && img.height == heightTo)
    {
        std::copy_n(img.rgba + (size_t(y) * img.width + x) * 4u, 4, outRgba);
        return;
    }

    const float u = ((float(x) + 0.5f) / float(widthTo)) * float(img.width) - 0.5f;
    const float v = ((float(y) + 0.5f) / float(heightTo)) * float(img.height) - 0.5f;
    const int32_t x0 = int32_t(std::floor(u));
    const int32_t y0 = int32_t(std::floor(v));
    const float fx = u - float(x0);
    const float fy = v - float(y0);
    const uint32_t wx0 = WrapSourceCoord(x0, img.width);
    const uint32_t wx1 = WrapSourceCoord(x0 + 1, img.width);
    const uint32_t wy0 = WrapSourceCoord(y0, img.height);
    const uint32_t wy1 = WrapSourceCoord(y0 + 1, img.height);
    const uint8_t *p00 = img.rgba + (size_t(wy0) * img.width + wx0) * 4u;
    const uint8_t *p10 = img.rgba + (size_t(wy0) * img.width + wx1) * 4u;
    const uint8_t *p01 = img.rgba + (size_t(wy1) * img.width + wx0) * 4u;
    const uint8_t *p11 = img.rgba + (size_t(wy1) * img.width + wx1) * 4u;
    for (int c = 0; c < 4; c++)
    {
        const float top = float(p00[c]) + (float(p10[c]) - float(p00[c])) * fx;
        const float bot = float(p01[c]) + (float(p11[c]) - float(p01[c])) * fx;
        outRgba[c] = uint8_t(std::clamp(std::lround(top + (bot - top) * fy), 0l, 255l));
    }
}

float Lanczos2Weight(float x)
{
    const float ax = std::fabs(x);
    if (ax < 1.0e-5f)
    {
        return 1.0f;
    }
    if (ax >= 2.0f)
    {
        return 0.0f;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float px = kPi * ax;
    return (std::sin(px) / px) * (std::sin(0.5f * px) / (0.5f * px));
}

void SampleSourceLanczos2(const SourceImage &img, uint32_t x, uint32_t y, uint32_t widthTo,
                          uint32_t heightTo, uint8_t outRgba[4])
{
    if (img.width == widthTo && img.height == heightTo)
    {
        std::copy_n(img.rgba + (size_t(y) * img.width + x) * 4u, 4, outRgba);
        return;
    }

    const float u = ((float(x) + 0.5f) / float(widthTo)) * float(img.width) - 0.5f;
    const float v = ((float(y) + 0.5f) / float(heightTo)) * float(img.height) - 0.5f;
    const int32_t cx = int32_t(std::floor(u));
    const int32_t cy = int32_t(std::floor(v));
    const float fx = u - float(cx);
    const float fy = v - float(cy);

    float wx[4];
    float wy[4];
    for (int i = 0; i < 4; i++)
    {
        const float d = float(i - 1);
        wx[i] = Lanczos2Weight(fx - d);
        wy[i] = Lanczos2Weight(fy - d);
    }

    float accum[4] = {};
    float wSum = 0.0f;
    for (int iy = 0; iy < 4; iy++)
    {
        const uint32_t sy = WrapSourceCoord(cy + iy - 1, img.height);
        const size_t row = size_t(sy) * img.width;
        for (int ix = 0; ix < 4; ix++)
        {
            const float w = std::max(0.0f, wx[ix] * wy[iy]);
            const uint32_t sx = WrapSourceCoord(cx + ix - 1, img.width);
            const uint8_t *px = img.rgba + (row + sx) * 4u;
            for (int c = 0; c < 4; c++)
            {
                accum[c] += float(px[c]) * w;
            }
            wSum += w;
        }
    }
    const float inv = (wSum > 1.0e-6f) ? (1.0f / wSum) : 1.0f;
    for (int c = 0; c < 4; c++)
    {
        outRgba[c] = uint8_t(std::clamp(std::lround(accum[c] * inv), 0l, 255l));
    }
}

SourceImage SourceOf(const EditorScene &scene, int32_t index)
{
    if (index < 0 || size_t(index) >= scene.textures.size())
    {
        return SourceImage{};
    }

    const EditorTexture &texture = scene.textures[size_t(index)];

    if (size_t(texture.width) * size_t(texture.height) * 4 != texture.rgba.size())
    {
        return SourceImage{};
    }

    return SourceImage{ texture.rgba.data(), texture.width, texture.height };
}

const uint8_t kWhite[4] = { 255, 255, 255, 255 };
const uint8_t kFlatNormal[4] = { 128, 128, 255, 255 };

const EditorTexture *TextureAt(const EditorScene &scene, int32_t index)
{
    if (index < 0 || size_t(index) >= scene.textures.size())
    {
        return nullptr;
    }

    return &scene.textures[size_t(index)];
}

bool HasFullbright(const EditorTexture *texture)
{
    return texture != nullptr && !texture->fullbright.empty();
}

bool HasActiveFullbright(const EditorTexture *texture)
{
    return texture != nullptr && !texture->fullbright.empty() &&
           texture->defaultEmissiveTexture.empty();
}

bool NeedsFullbrightBake(const EditorTexture *texture, const EditorMaterialParams &params)
{
    if (!HasFullbright(texture))
    {
        return false;
    }

    if (!texture->defaultEmissiveTexture.empty() && params.emissiveTexture.empty())
    {
        return true;
    }

    return !params.enabled || params.roughnessOverride >= 0.0f || params.metalnessFactor > 0.0f ||
           params.emissiveFactor != 1.0f || params.mirror || params.baseFactor != 1.0f;
}

// The falloff of a colour block's threshold mask, gl_texmgr.c RT_COLOR_EMISSIVE_FALLOFF.
constexpr float kColorEmissiveFalloff = 2.0f;

void FeatherMaskHorizontal(const std::vector<float> &src, uint32_t width, uint32_t height,
                           int radius, std::vector<float> &dst)
{
    const int w = int(width);
    const int h = int(height);
    const float window = float(radius * 2 + 1);

    for (int y = 0; y < h; y++)
    {
        const size_t row = size_t(y) * width;
        float sum = 0.0f;

        for (int i = -radius; i <= radius; i++)
        {
            sum += src[row + size_t(std::clamp(i, 0, w - 1))];
        }

        for (int x = 0; x < w; x++)
        {
            dst[row + size_t(x)] = sum / window;
            sum += src[row + size_t(std::min(x + radius + 1, w - 1))];
            sum -= src[row + size_t(std::max(x - radius, 0))];
        }
    }
}

void FeatherMaskVertical(const std::vector<float> &src, uint32_t width, uint32_t height, int radius,
                         std::vector<float> &dst)
{
    const int w = int(width);
    const int h = int(height);
    const float window = float(radius * 2 + 1);

    for (int x = 0; x < w; x++)
    {
        float sum = 0.0f;

        for (int i = -radius; i <= radius; i++)
        {
            sum += src[size_t(std::clamp(i, 0, h - 1)) * width + size_t(x)];
        }

        for (int y = 0; y < h; y++)
        {
            const size_t index = size_t(y) * width + size_t(x);
            dst[index] = sum / window;
            sum += src[size_t(std::min(y + radius + 1, h - 1)) * width + size_t(x)];
            sum -= src[size_t(std::max(y - radius, 0)) * width + size_t(x)];
        }
    }
}

void ApplyBoxBlurPass(std::vector<float> &mask, std::vector<float> &tmp, uint32_t width,
                      uint32_t height, int radius)
{
    FeatherMaskHorizontal(mask, width, height, radius, tmp);
    FeatherMaskVertical(tmp, width, height, radius, mask);
}

float PeakMaskValue(const std::vector<float> &mask)
{
    float peak = 0.0f;
    for (float value : mask)
    {
        peak = std::max(peak, value);
    }
    return peak;
}

// Softens a scalar mask: outward dilation pre-pass so thin highlights survive, followed by
// symmetric 3-pass Gaussian-like smoothing and peak preservation.
void FeatherMask(std::vector<float> &mask, uint32_t width, uint32_t height, float feather,
                 float maxScale)
{
    if (feather < 0.5f || width == 0 || height == 0 || mask.empty())
    {
        return;
    }

    const float peakBefore = PeakMaskValue(mask);
    if (peakBefore <= 1e-6f)
    {
        return;
    }

    const int radius = std::clamp(int(std::lround(feather)), 1, 16);
    const int dilateR = std::max(1, radius / 2);
    std::vector<float> tmp(mask.size(), 0.0f);
    std::vector<float> dilated(mask.size(), 0.0f);

    FeatherMaskHorizontal(mask, width, height, dilateR, tmp);
    FeatherMaskVertical(tmp, width, height, dilateR, dilated);
    for (size_t i = 0; i < mask.size(); i++)
    {
        mask[i] = std::max(mask[i], dilated[i]);
    }

    const int budget = std::max(2, radius * 2 - 1);
    const int passes[3] = { (budget + 2) / 3, (budget + 1) / 3, budget / 3 };
    for (int passRadius : passes)
    {
        if (passRadius > 0)
        {
            ApplyBoxBlurPass(mask, tmp, width, height, passRadius);
        }
    }

    const float peakAfter = PeakMaskValue(mask);
    if (peakAfter > 1e-5f && peakAfter < peakBefore)
    {
        const float gain = std::min(2.0f, peakBefore / peakAfter);
        const float ceiling = std::max(0.0f, maxScale);
        for (float &value : mask)
        {
            value = std::clamp(value * gain, 0.0f, ceiling);
        }
    }
}

// Whether a uv point falls inside a block's polygon, by the even-odd rule the synthesis uses.
bool InsidePolygon(const std::vector<float> &polyUv, float u, float v)
{
    const size_t count = polyUv.size() / 2;
    bool inside = false;

    for (size_t p = 0, q = count - 1; p < count; q = p++)
    {
        const float pu = polyUv[p * 2 + 0];
        const float pv = polyUv[p * 2 + 1];
        const float qu = polyUv[q * 2 + 0];
        const float qv = polyUv[q * 2 + 1];

        if (((pv > v) != (qv > v)) && (u < (qu - pu) * (v - pv) / (qv - pv) + pu))
        {
            inside = !inside;
        }
    }

    return inside;
}

float SampleBrushMaskBilinear(const EditorMaskEvalSpec &spec, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height)
{
    const uint8_t *brush = spec.brushMask->data();
    const float sx = std::clamp(
        ((float(x) + 0.5f) / float(width)) * float(spec.brushWidth) - 0.5f, 0.0f,
        float(spec.brushWidth - 1));
    const float sy = std::clamp(
        ((float(y) + 0.5f) / float(height)) * float(spec.brushHeight) - 0.5f, 0.0f,
        float(spec.brushHeight - 1));
    const int x0 = int(std::floor(sx));
    const int y0 = int(std::floor(sy));
    const int x1 = std::min(x0 + 1, int(spec.brushWidth) - 1);
    const int y1 = std::min(y0 + 1, int(spec.brushHeight) - 1);
    const float fx = sx - float(x0);
    const float fy = sy - float(y0);

    const auto at = [&](int bx, int by) {
        return float(brush[size_t(by) * spec.brushWidth + size_t(bx)]) / 255.0f;
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return top + (bot - top) * fy;
}

void EvaluateBrushMask(const EditorMaskEvalSpec &spec, uint32_t width, uint32_t height,
                       std::vector<float> &outMask)
{
    if (spec.brushWidth == width && spec.brushHeight == height)
    {
        const uint8_t *brush = spec.brushMask->data();
        const size_t pixels = size_t(width) * size_t(height);
        for (size_t i = 0; i < pixels; i++)
        {
            outMask[i] = (float(brush[i]) / 255.0f) * spec.scale;
        }
        return;
    }

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            outMask[size_t(y) * width + x] =
                SampleBrushMaskBilinear(spec, x, y, width, height) * spec.scale;
        }
    }
}

void EvaluatePolygonMask(const std::vector<float> &polyUv, float scale, uint32_t width,
                         uint32_t height, std::vector<float> &outMask)
{
    for (uint32_t y = 0; y < height; y++)
    {
        const float v = (float(y) + 0.5f) / float(height);
        for (uint32_t x = 0; x < width; x++)
        {
            const float u = (float(x) + 0.5f) / float(width);
            if (InsidePolygon(polyUv, u, v))
            {
                outMask[size_t(y) * width + x] = scale;
            }
        }
    }
}

float EvaluateColorMaskValue(const EditorMaskEvalSpec &spec, float d2)
{
    if (spec.isGloss)
    {
        const float feather = std::max(0.0f, spec.feather);
        const float dnorm = std::sqrt(d2 / 3.0f) / std::max(spec.threshold, 1e-4f);
        const float innerEnd = std::max(0.15f, 0.72f - 0.03f * feather);
        const float outerEnd = 1.0f + 0.06f * feather;
        if (dnorm <= innerEnd)
        {
            return spec.scale;
        }
        if (dnorm < outerEnd)
        {
            const float t = (dnorm - innerEnd) / std::max(1e-4f, outerEnd - innerEnd);
            const float w = 1.0f - t * t * (3.0f - 2.0f * t);
            return w * spec.scale;
        }
        return 0.0f;
    }

    const float threshold = std::max(0.0f, spec.threshold);
    if (d2 <= 3.0f * threshold * threshold)
    {
        const float dnorm = threshold > 0.0f ? std::sqrt(d2 / 3.0f) / threshold : 0.0f;
        const float tail = std::exp(-kColorEmissiveFalloff);
        const float value = (std::exp(-kColorEmissiveFalloff * dnorm) - tail) / (1.0f - tail);
        return value * spec.scale;
    }
    return 0.0f;
}

void EvaluateColorMask(const EditorMaskEvalSpec &spec, const uint8_t *rgba, uint32_t width,
                       uint32_t height, std::vector<float> &outMask, std::vector<float> *outDist)
{
    const size_t pixels = size_t(width) * size_t(height);
    for (size_t index = 0; index < pixels; index++)
    {
        const uint8_t *src = rgba + index * 4;
        const float dr = float(src[0]) / 255.0f - spec.color[0];
        const float dg = float(src[1]) / 255.0f - spec.color[1];
        const float db = float(src[2]) / 255.0f - spec.color[2];
        const float d2 = dr * dr + dg * dg + db * db;
        if (outDist != nullptr)
        {
            (*outDist)[index] = d2;
        }
        outMask[index] = EvaluateColorMaskValue(spec, d2);
    }
}

uint64_t HashBrushBytes(const std::vector<uint8_t> &bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t b : bytes)
    {
        hash ^= uint64_t(b);
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename Block>
void AppendSerializedBlockGeometry(const Block &block, std::string &text)
{
    char buffer[64];

    if (IsBrushBlock(block))
    {
        std::snprintf(buffer, sizeof(buffer), "brush %ux%u #%016llx", block.brushWidth,
                      block.brushHeight,
                      static_cast<unsigned long long>(HashBrushBytes(block.brushMask)));
        text += buffer;
        return;
    }

    if (IsPolygonBlock(block))
    {
        const size_t points = block.polyUv.size() / 2;
        text += "poly";
        for (size_t p = 0; p < points * 2; p++)
        {
            std::snprintf(buffer, sizeof(buffer), ",%.6g", double(block.polyUv[p]));
            text += buffer;
        }
        return;
    }

    std::snprintf(buffer, sizeof(buffer), "color %02x%02x%02x",
                  int(std::lround(std::clamp(block.color[0], 0.0f, 1.0f) * 255.0f)) & 0xff,
                  int(std::lround(std::clamp(block.color[1], 0.0f, 1.0f) * 255.0f)) & 0xff,
                  int(std::lround(std::clamp(block.color[2], 0.0f, 1.0f) * 255.0f)) & 0xff);
    text += buffer;
}

// Builds the emission and blend masks of a material's colour blocks: each block selects what it
// covers, feathers its own mask, and every pixel goes to the block whose colour is nearest among
// those that match. `emission` receives the scaled mask and `blend` the byte that travels in the
// alpha of the packed set.
void BuildColorEmissive(const EditorMaterialParams &params, const SourceImage &albedo,
                        std::vector<float> &emission, std::vector<uint8_t> &blend)
{
    const size_t pixels = size_t(albedo.width) * size_t(albedo.height);
    const uint8_t materialBlendCode =
        params.lumaBlend < 0 ? 0 : uint8_t(std::min<int32_t>(params.lumaBlend, 5) + 1);

    std::vector<float> distance(pixels, 0.0f);
    emission.assign(pixels, 0.0f);
    blend.assign(pixels, materialBlendCode);

    for (const EditorEmissiveBlock &block : params.emissiveBlocks)
    {
        const uint8_t blockCode =
            block.blend < 0 ? 0 : uint8_t(std::min<int32_t>(block.blend, 5) + 1);

        std::vector<float> mask;
        std::vector<float> dist;
        const EditorMaskEvalSpec spec{ block.color,      block.threshold,  block.feather,
                                       block.factor,     &block.polyUv,    &block.brushMask,
                                       block.brushWidth, block.brushHeight, false };
        ComputeEditorMask(spec, albedo.rgba, albedo.width, albedo.height, mask, &dist);

        for (size_t index = 0; index < pixels; index++)
        {
            if (!(mask[index] > 0.0f))
            {
                continue;
            }

            if (emission[index] > 0.0f &&
                (dist[index] > distance[index] ||
                 (dist[index] == distance[index] && !(mask[index] > emission[index]))))
            {
                continue;
            }

            emission[index] = mask[index];
            blend[index] = blockCode;
            distance[index] = dist[index];
        }
    }
}

struct ColorGlossBuffers
{
    std::vector<float> weight;
    std::vector<float> roughness;
    std::vector<float> metalness;
};

void BuildColorGloss(const EditorMaterialParams &params, const SourceImage &albedo,
                     ColorGlossBuffers &out)
{
    const size_t pixels = size_t(albedo.width) * size_t(albedo.height);
    std::vector<float> weightSum(pixels, 0.0f);
    std::vector<float> roughnessAcc(pixels, 0.0f);
    std::vector<float> metalnessAcc(pixels, 0.0f);
    std::vector<float> maxWeight(pixels, 0.0f);

    for (const EditorGlossBlock &block : params.glossBlocks)
    {
        std::vector<float> mask;
        const EditorMaskEvalSpec spec{ block.color,      block.threshold,  block.feather,
                                       1.0f,             &block.polyUv,    &block.brushMask,
                                       block.brushWidth, block.brushHeight, true };
        ComputeEditorMask(spec, albedo.rgba, albedo.width, albedo.height, mask, nullptr);

        const float blkRough = std::clamp(block.roughness, 0.0f, 1.0f);
        const float blkMetal = std::clamp(block.metalness, 0.0f, 1.0f);

        for (size_t index = 0; index < pixels; index++)
        {
            const float w = std::clamp(mask[index], 0.0f, 1.0f);
            if (!(w > 0.0f))
            {
                continue;
            }

            weightSum[index] += w;
            roughnessAcc[index] += w * blkRough;
            metalnessAcc[index] += w * blkMetal;
            maxWeight[index] = std::max(maxWeight[index], w);
        }
    }

    out.weight.assign(pixels, 0.0f);
    out.roughness.assign(pixels, 0.0f);
    out.metalness.assign(pixels, 0.0f);

    for (size_t index = 0; index < pixels; index++)
    {
        if (weightSum[index] > 1e-6f)
        {
            out.weight[index] = std::clamp(maxWeight[index], 0.0f, 1.0f);
            out.roughness[index] = roughnessAcc[index] / weightSum[index];
            out.metalness[index] = metalnessAcc[index] / weightSum[index];
        }
    }
}

// The blocks as one comparable string, so two materials that differ anywhere in their colour
// blocks are different bakes.
std::string EmissiveMaskOf(const std::vector<EditorEmissiveBlock> &blocks)
{
    std::string text;
    char buffer[160];

    for (const EditorEmissiveBlock &block : blocks)
    {
        AppendSerializedBlockGeometry(block, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g x%.6g b%d;", double(block.threshold),
                      double(block.feather), double(block.factor), block.blend);
        text += buffer;
    }

    return text;
}

std::string GlossMaskOf(const std::vector<EditorGlossBlock> &blocks)
{
    std::string text;
    char buffer[160];

    for (const EditorGlossBlock &block : blocks)
    {
        AppendSerializedBlockGeometry(block, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g r%.6g m%.6g;",
                      double(block.threshold), double(block.feather), double(block.roughness),
                      double(block.metalness));
        text += buffer;
    }

    return text;
}

template <typename Block>
void EvaluateFactorBlocksMask(const std::vector<Block> &blocks, const SourceImage &albedo,
                              std::vector<float> &combined)
{
    if (!albedo.Valid())
    {
        return;
    }

    const size_t pixels = size_t(albedo.width) * size_t(albedo.height);
    std::vector<float> mask;

    for (const Block &block : blocks)
    {
        const EditorMaskEvalSpec spec{ block.color,      block.threshold,  block.feather,
                                       block.factor,     &block.polyUv,    &block.brushMask,
                                       block.brushWidth, block.brushHeight, false };
        ComputeEditorMask(spec, albedo.rgba, albedo.width, albedo.height, mask, nullptr);

        for (size_t index = 0; index < pixels; index++)
        {
            combined[index] = std::max(combined[index], mask[index]);
        }
    }
}

template <typename Block>
std::string SerializeFactorBlocks(const std::vector<Block> &blocks)
{
    std::string text;
    char buffer[160];

    for (const Block &block : blocks)
    {
        AppendSerializedBlockGeometry(block, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g x%.6g;", double(block.threshold),
                      double(block.feather), double(block.factor));
        text += buffer;
    }

    return text;
}

struct FactorMaskInputs
{
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t *albedoRgba = nullptr;
    const uint8_t *texRgba = nullptr;
    uint32_t texWidth = 0;
    uint32_t texHeight = 0;
    float scale = 1.0f;
};

template <typename Block>
std::vector<uint8_t> BuildFactorMaskBytes(const std::vector<Block> &blocks,
                                          const FactorMaskInputs &inputs)
{
    const size_t pixels = size_t(inputs.width) * size_t(inputs.height);
    if (inputs.width == 0 || inputs.height == 0 || pixels == 0)
    {
        return {};
    }

    const float clampedScale =
        std::isfinite(inputs.scale) ? std::clamp(inputs.scale, 0.0f, 1.0f) : 0.0f;
    const SourceImage texImage{ inputs.texRgba, inputs.texWidth, inputs.texHeight };
    const SourceImage albedoImage{ inputs.albedoRgba, inputs.width, inputs.height };
    if (blocks.empty() && !texImage.Valid())
    {
        return std::vector<uint8_t>(
            pixels, uint8_t(std::clamp(clampedScale * 255.0f + 0.5f, 0.0f, 255.0f)));
    }

    std::vector<float> combined(pixels, 0.0f);
    if (texImage.Valid())
    {
        for (uint32_t y = 0; y < inputs.height; y++)
        {
            for (uint32_t x = 0; x < inputs.width; x++)
            {
                const uint8_t *tex = texImage.At(x, y, inputs.width, inputs.height);
                const float lum =
                    (0.2126f * tex[0] + 0.7152f * tex[1] + 0.0722f * tex[2]) / 255.0f;
                combined[size_t(y) * inputs.width + x] = lum;
            }
        }
    }

    if (!blocks.empty())
    {
        EvaluateFactorBlocksMask(blocks, albedoImage, combined);
    }

    std::vector<uint8_t> out(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        out[i] = uint8_t(std::clamp(combined[i] * clampedScale * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    return out;
}

struct BakeEmissionInputs
{
    const SourceImage &emissive;
    const EditorTexture *albedoTexture = nullptr;
    const std::vector<float> &colorEmission;
    const std::vector<uint8_t> &colorBlend;
    const std::vector<SourceImage> *upperCoverImages = nullptr;
    const std::vector<bool> *upperCoverInvertedAlpha = nullptr;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t maskWidth = 0;
    uint32_t maskHeight = 0;
    float emissiveFactor = 1.0f;
    uint8_t blendCode = 0;
};

float ApplyUpperCoverTransmittance(const BakeEmissionInputs &inputs, uint32_t x, uint32_t y,
                                   float emission)
{
    if (!(emission > 0.0f) || inputs.upperCoverImages == nullptr ||
        inputs.upperCoverInvertedAlpha == nullptr)
    {
        return emission;
    }

    const size_t count =
        std::min(inputs.upperCoverImages->size(), inputs.upperCoverInvertedAlpha->size());
    for (size_t i = 0; i < count && emission > 0.0f; i++)
    {
        uint8_t covPx[4] = {};
        SampleSourceBilinear((*inputs.upperCoverImages)[i], x, y, inputs.targetWidth,
                             inputs.targetHeight, covPx);
        const float a = float(covPx[3]) / 255.0f;
        const float t = (*inputs.upperCoverInvertedAlpha)[i] ? a : (1.0f - a);
        emission *= std::clamp(t, 0.0f, 1.0f);
    }
    return emission;
}

// Samples per-pixel emission and blend mode, merging any Quake fullbright mask over explicit
// luma textures or synthesized colour blocks as in gl_texmgr.c.
float SampleBakeEmission(const BakeEmissionInputs &inputs, uint32_t x, uint32_t y,
                         uint8_t &outBlend, uint8_t outEmissRgb[3], bool &outHasEmissRgb)
{
    float emission = 0.0f;
    outBlend = inputs.blendCode;
    outHasEmissRgb = false;

    if (inputs.emissive.Valid())
    {
        uint8_t mask[4] = {};
        SampleSourceLanczos2(inputs.emissive, x, y, inputs.targetWidth, inputs.targetHeight, mask);
        outEmissRgb[0] = mask[0];
        outEmissRgb[1] = mask[1];
        outEmissRgb[2] = mask[2];
        outHasEmissRgb = true;
        const float luminance =
            (0.2126f * mask[0] + 0.7152f * mask[1] + 0.0722f * mask[2]) / 255.0f;
        return ApplyUpperCoverTransmittance(inputs, x, y, luminance * inputs.emissiveFactor);
    }
    if (!inputs.colorEmission.empty() && inputs.maskWidth > 0 && inputs.maskHeight > 0)
    {
        const uint32_t mx = uint32_t(uint64_t(x) * inputs.maskWidth / inputs.targetWidth) %
                            inputs.maskWidth;
        const uint32_t my = uint32_t(uint64_t(y) * inputs.maskHeight / inputs.targetHeight) %
                            inputs.maskHeight;
        const size_t pixel = size_t(my) * inputs.maskWidth + mx;
        emission = inputs.colorEmission[pixel];
        outBlend = inputs.colorBlend[pixel];
    }
    if (inputs.albedoTexture != nullptr &&
        inputs.albedoTexture->defaultEmissiveTexture.empty() &&
        !inputs.albedoTexture->fullbright.empty() && inputs.albedoTexture->width > 0 &&
        inputs.albedoTexture->height > 0)
    {
        const uint32_t fx = uint32_t(uint64_t(x) * inputs.albedoTexture->width / inputs.targetWidth) %
                            inputs.albedoTexture->width;
        const uint32_t fy = uint32_t(uint64_t(y) * inputs.albedoTexture->height / inputs.targetHeight) %
                            inputs.albedoTexture->height;
        const float fullbright =
            (inputs.albedoTexture->fullbright[size_t(fy) * inputs.albedoTexture->width + fx] /
             255.0f) *
            inputs.emissiveFactor;

        if (fullbright > emission)
        {
            emission = fullbright;
            outBlend = inputs.blendCode;
        }
    }

    return ApplyUpperCoverTransmittance(inputs, x, y, emission);
}

// Computes bounding texel coordinates where emission exceeds kEditorGlowThreshold.
struct GlowBounds
{
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;
};

void AccumulateGlow(float emission, uint32_t x, uint32_t y, GlowBounds &bounds)
{
    if (!(emission > kEditorGlowThreshold))
    {
        return;
    }

    const int px = int(x);
    const int py = int(y);
    bounds.minX = std::min(bounds.minX, px);
    bounds.maxX = std::max(bounds.maxX, px);
    bounds.minY = std::min(bounds.minY, py);
    bounds.maxY = std::max(bounds.maxY, py);
}

bool IsChromeCapableMaterial(const EditorMaterialParams &material,
                             QrGeometryPassThroughType passThrough, bool mirror)
{
    if (mirror)
    {
        return true;
    }

    if (passThrough != QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE)
    {
        return false;
    }

    const bool hasExplicitGloss = !material.glossTexture.empty() ||
                                  (material.hasColorGloss && !material.glossBlocks.empty());
    const bool hasMirrorRoughness =
        material.roughnessOverride >= 0.0f && material.roughnessOverride < 0.02f;
    return hasExplicitGloss || hasMirrorRoughness;
}

bool IsWaterFamilyPassThrough(QrGeometryPassThroughType passThrough)
{
    return passThrough == QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT ||
           passThrough == QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT;
}

bool HasIdentityTexCoordTransform(const EditorMaterialParams &material)
{
    return material.textureScaleU == 1.0f && material.textureScaleV == 1.0f &&
           material.textureOffsetU == 0.0f && material.textureOffsetV == 0.0f;
}

void ApplyMaterialPassThroughAndRoughness(const EditorMaterialParams &material,
                                          QrGeometryPassThroughType derivedPassThrough,
                                          QrGeometryUploadInfo &upload)
{
    QrGeometryPassThroughType passThrough = derivedPassThrough;

    const bool glass = material.type == EditorMaterialType::Glass;
    const bool water = material.type == EditorMaterialType::Water;

    switch (material.type)
    {
    case EditorMaterialType::Glass:
        passThrough = QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT;
        break;
    case EditorMaterialType::Water:
        passThrough = QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT;
        break;
    case EditorMaterialType::None:
        break;
    }

    // The game's is_mirror: glass and water outrank a mirror, and only a surface neither of
    // them claimed keeps the mirror pass-through. A water or acid surface arrives here as a
    // derived pass-through, not as the material type, so both spellings have to be checked.
    const bool waterFamily = IsWaterFamilyPassThrough(passThrough);
    const bool mirror = material.mirror && !glass && !water && !waterFamily;

    const bool chromeCapable =
        !glass && !water && !waterFamily && IsChromeCapableMaterial(material, passThrough, mirror);

    if (chromeCapable)
    {
        passThrough = QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR;
        upload.flags |= QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT;
    }

    upload.passThroughType = passThrough;

    // Mirror forces roughness to 0; glass keeps its own override, as the game's tooltip says.
    const float roughness =
        material.roughnessOverride < 0.0f ? kEditorDefaultRoughness : material.roughnessOverride;

    upload.defaultRoughness = mirror ? 0.0f : roughness;
    upload.defaultMetallicity = material.metalnessFactor;
}

float DistanceToSegmentTexel(float px, float py, float x0, float y0, float x1, float y1)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lenSq = dx * dx + dy * dy;
    const float t =
        lenSq > 1e-8f ? std::clamp(((px - x0) * dx + (py - y0) * dy) / lenSq, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - (x0 + dx * t), py - (y0 + dy * t));
}

float ComputeBrushFalloff(float dist, float radiusTexels, float hardness)
{
    const float rNorm = dist / std::max(radiusTexels, 0.5f);
    if (rNorm <= hardness)
    {
        return 1.0f;
    }
    const float t = std::clamp((rNorm - hardness) / std::max(1e-4f, 1.0f - hardness), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

struct BrushSegmentBox
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int minX = 0;
    int maxX = -1;
    int minY = 0;
    int maxY = -1;
    float hardness = 0.5f;
    float opacity = 1.0f;
};

int ClampCoordToInt(double value, int lo, int hi)
{
    if (value <= double(lo))
    {
        return lo;
    }
    if (value >= double(hi))
    {
        return hi;
    }
    return int(value);
}

BrushSegmentBox BuildBrushSegmentBox(const EditorBrushStrokeSpec &stroke, uint32_t width,
                                     uint32_t height)
{
    BrushSegmentBox box;
    box.x0 = stroke.u0 * float(width);
    box.y0 = stroke.v0 * float(height);
    box.x1 = stroke.u1 * float(width);
    box.y1 = stroke.v1 * float(height);
    const double r = double(stroke.radiusTexels);
    box.minX = ClampCoordToInt(std::floor(double(std::min(box.x0, box.x1)) - r), 0, int(width));
    box.maxX = ClampCoordToInt(std::ceil(double(std::max(box.x0, box.x1)) + r), -1, int(width) - 1);
    box.minY = ClampCoordToInt(std::floor(double(std::min(box.y0, box.y1)) - r), 0, int(height));
    box.maxY = ClampCoordToInt(std::ceil(double(std::max(box.y0, box.y1)) + r), -1, int(height) - 1);
    box.hardness = std::clamp(stroke.hardness, 0.0f, 1.0f);
    box.opacity = std::clamp(stroke.opacity, 0.0f, 1.0f);
    return box;
}

bool IsValidBrushStroke(const EditorBrushStrokeSpec &stroke)
{
    return std::isfinite(stroke.u0) && std::isfinite(stroke.v0) && std::isfinite(stroke.u1) &&
           std::isfinite(stroke.v1) && std::isfinite(stroke.radiusTexels) &&
           std::isfinite(stroke.hardness) && std::isfinite(stroke.opacity) &&
           stroke.radiusTexels > 0.0f && stroke.opacity > 0.0f;
}

uint8_t BlendBrushTexel(uint8_t baseByte, float dab, bool erase)
{
    const float baseVal = float(baseByte) / 255.0f;
    const float target = erase ? baseVal * (1.0f - dab) : baseVal + (1.0f - baseVal) * dab;
    return uint8_t(std::clamp(std::lround(target * 255.0f), 0l, 255l));
}

bool HasValidBrushSpec(const EditorMaskEvalSpec &spec)
{
    return spec.brushMask != nullptr && spec.brushWidth > 0 && spec.brushHeight > 0 &&
           spec.brushMask->size() == size_t(spec.brushWidth) * size_t(spec.brushHeight);
}

bool HasValidPolySpec(const EditorMaskEvalSpec &spec)
{
    return spec.polyUv != nullptr && spec.polyUv->size() / 2 >= 3;
}

bool StampBrushSegmentRow(const BrushSegmentBox &box, const EditorBrushStrokeSpec &stroke,
                          uint32_t width, int y, const std::vector<uint8_t> &baseMask,
                          std::vector<float> &strokeAccum, std::vector<uint8_t> &outMask)
{
    bool changed = false;
    for (int x = box.minX; x <= box.maxX; x++)
    {
        const float d = DistanceToSegmentTexel(float(x) + 0.5f, float(y) + 0.5f, box.x0, box.y0,
                                               box.x1, box.y1);
        if (!(d < stroke.radiusTexels))
        {
            continue;
        }
        const float dab = std::clamp(
            box.opacity * ComputeBrushFalloff(d, stroke.radiusTexels, box.hardness), 0.0f, 1.0f);
        const size_t idx = size_t(y) * width + size_t(x);
        if (dab <= strokeAccum[idx])
        {
            continue;
        }
        strokeAccum[idx] = dab;
        const uint8_t nextVal = BlendBrushTexel(baseMask[idx], dab, stroke.erase);
        if (outMask[idx] != nextVal)
        {
            outMask[idx] = nextVal;
            changed = true;
        }
    }
    return changed;
}

} // namespace

void ComputeEditorMask(const EditorMaskEvalSpec &spec, const uint8_t *rgba, uint32_t width,
                       uint32_t height, std::vector<float> &outMask, std::vector<float> *outDist)
{
    if (width == 0 || height == 0)
    {
        outMask.clear();
        if (outDist != nullptr)
        {
            outDist->clear();
        }
        return;
    }

    const size_t pixels = size_t(width) * size_t(height);
    outMask.assign(pixels, 0.0f);
    if (outDist != nullptr)
    {
        outDist->assign(pixels, 0.0f);
    }

    if (HasValidBrushSpec(spec))
    {
        EvaluateBrushMask(spec, width, height, outMask);
    }
    else if (HasValidPolySpec(spec))
    {
        EvaluatePolygonMask(*spec.polyUv, spec.scale, width, height, outMask);
    }
    else if (rgba != nullptr && spec.color != nullptr)
    {
        EvaluateColorMask(spec, rgba, width, height, outMask, outDist);
    }

    FeatherMask(outMask, width, height, spec.feather, spec.scale);
}

bool ApplyEditorBrushSegment(const EditorBrushStrokeSpec &stroke, uint32_t width, uint32_t height,
                             const std::vector<uint8_t> &baseMask,
                             std::vector<float> &strokeAccum, std::vector<uint8_t> &outMask)
{
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || !IsValidBrushStroke(stroke) || baseMask.size() != pixels)
    {
        return false;
    }

    if (strokeAccum.size() != pixels)
    {
        strokeAccum.assign(pixels, 0.0f);
    }
    if (outMask.size() != pixels)
    {
        outMask = baseMask;
    }

    const BrushSegmentBox box = BuildBrushSegmentBox(stroke, width, height);
    bool changed = false;

    for (int y = box.minY; y <= box.maxY; y++)
    {
        changed |= StampBrushSegmentRow(box, stroke, width, y, baseMask, strokeAccum, outMask);
    }

    return changed;
}

void FinalizeEditorGlowExtents(int minX, int minY, int maxX, int maxY, uint32_t width,
                               uint32_t height, EditorMaterialEmission &out)
{
    out.hasGlow = false;
    out.glowUvMin[0] = 0.0f;
    out.glowUvMin[1] = 0.0f;
    out.glowUvMax[0] = 1.0f;
    out.glowUvMax[1] = 1.0f;
    out.glowMean = out.mean;

    if (maxX < minX || maxY < minY || width == 0 || height == 0)
    {
        return;
    }

    const float totalPixels = float(width) * float(height);
    const float glowArea = float(maxX - minX + 1) * float(maxY - minY + 1);
    const float glowFrac = glowArea / totalPixels;

    out.glowUvMin[0] = float(minX) / float(width);
    out.glowUvMin[1] = float(minY) / float(height);
    out.glowUvMax[0] = float(maxX + 1) / float(width);
    out.glowUvMax[1] = float(maxY + 1) / float(height);

    // Matches RT_EMIS_GLOW_FULL (0.999f) and RT_EmissiveLightParamsForTex in vkquake-rt.
    if (glowFrac < 0.999f && glowFrac > 1e-6f)
    {
        out.hasGlow = true;
        out.glowMean = out.mean / glowFrac;
    }
}

bool HasEditorSubsurface(const EditorMaterialParams &material)
{
    const bool blockedType = material.type == EditorMaterialType::Glass ||
                             material.type == EditorMaterialType::Water ||
                             material.mirror;
    return !blockedType &&
           std::isfinite(material.sssStrength) && material.sssStrength > 0.0f &&
           std::isfinite(material.sssRadius) && material.sssRadius > 0.0f;
}

bool EditorMaterialPromotesOverlayToTraced(const EditorMaterialParams &material)
{
    return material.mirror || material.type == EditorMaterialType::Glass ||
           material.type == EditorMaterialType::Water;
}

void ApplyEditorSubsurface(const EditorMaterialParams &material, QrGeometryUploadInfo &upload)
{
    const bool allowedPassThrough =
        upload.passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE ||
        upload.passThroughType == QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED;

    if (!HasEditorSubsurface(material) || !allowedPassThrough)
    {
        upload.flags &= ~QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT;
        upload.subsurfaceColor = { 0.0f, 0.0f, 0.0f };
        upload.subsurfaceRadius = 0.0f;
        upload.subsurfaceStrength = 0.0f;
        return;
    }

    auto sanitizeUnit = [](float v) {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    };

    upload.flags |= QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT;
    upload.subsurfaceColor = {
        sanitizeUnit(material.sssColor[0]),
        sanitizeUnit(material.sssColor[1]),
        sanitizeUnit(material.sssColor[2]),
    };
    upload.subsurfaceRadius = sanitizeUnit(material.sssRadius);
    upload.subsurfaceStrength = sanitizeUnit(material.sssStrength);
}

std::vector<uint8_t> BuildEditorSssMask(const EditorMaterialParams &params,
                                        uint32_t width,
                                        uint32_t height,
                                        const uint8_t *albedoRgba,
                                        const uint8_t *textureSssRgba,
                                        uint32_t textureSssWidth,
                                        uint32_t textureSssHeight)
{
    const FactorMaskInputs inputs{
        width, height, albedoRgba, textureSssRgba, textureSssWidth, textureSssHeight, 1.0f,
    };
    return BuildFactorMaskBytes(params.sssBlocks, inputs);
}

void ApplyEditorMaterial(const EditorMaterialParams &material,
                         QrGeometryPassThroughType derivedPassThrough,
                         QrGeometryUploadInfo &upload)
{
    ApplyMaterialPassThroughAndRoughness(material, derivedPassThrough, upload);

    // The game uploads no scalar emission for any geometry kind (r_world.c, r_alias.c and
    // r_sprite.c all pass 0): a surface's glow is the emission channel of its packed set, so the
    // scalar path is only the fallback for geometry that has no set at all.
    upload.defaultEmission = 0.0f;

    const float base = material.baseFactor;
    upload.layerColors[0] = { base, base, base, 1.0f };

    if (material.type == EditorMaterialType::Glass)
    {
        upload.layerColors[1] = { material.glassColor[0], material.glassColor[1],
                                  material.glassColor[2], 0.0f };
        upload.layerColors[2] = { material.glassIor, material.glassThickness, 0.0f, 0.0f };
    }

    upload.texCoordScale[0] = material.textureScaleU;
    upload.texCoordScale[1] = material.textureScaleV;
    upload.texCoordOffset[0] = material.textureOffsetU;
    upload.texCoordOffset[1] = material.textureOffsetV;

    if (!HasIdentityTexCoordTransform(material))
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT;
    }

    if (material.exactNormals)
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT;
    }

    ApplyEditorSubsurface(material, upload);
}

namespace
{

void CollectSurfaceMaterialNames(const EditorScene &scene, const EditorSurface &surface,
                                 std::vector<std::string> &names)
{
    bool addedFrame = false;

    for (int32_t frame : surface.animationFrames)
    {
        if (frame >= 0 && size_t(frame) < scene.textures.size())
        {
            const std::string &texName = scene.textures[size_t(frame)].name;

            if (!texName.empty() && texName[0] == '+')
            {
                names.push_back(texName);
                addedFrame = true;
            }
        }
    }

    if (!addedFrame && !surface.textureName.empty())
    {
        names.push_back(surface.textureName);
    }
}

int32_t PrimarySurfaceTextureIndex(const EditorSurface &surface)
{
    if (surface.texture >= 0)
    {
        return surface.texture;
    }

    return surface.animationFrames.empty() ? -1 : surface.animationFrames.front();
}

} // namespace

void EditorMaterialTable::Rebuild(const EditorScene &scene)
{
    m_entries.clear();
    m_indexByName.clear();

    std::vector<std::string> names;

    for (const EditorSurface &surface : scene.surfaces)
    {
        CollectSurfaceMaterialNames(scene, surface, names);
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    m_entries.reserve(names.size());

    for (const std::string &name : names)
    {
        m_indexByName[name] = m_entries.size();
        m_entries.push_back(Entry{ name, EditorMaterialParams{} });
    }

    for (const EditorSurface &surface : scene.surfaces)
    {
        if (surface.textureName.empty() ||
            m_indexByName.find(surface.textureName) != m_indexByName.end())
        {
            continue;
        }

        const int32_t primary = PrimarySurfaceTextureIndex(surface);

        if (primary >= 0 && size_t(primary) < scene.textures.size())
        {
            const auto found = m_indexByName.find(scene.textures[size_t(primary)].name);

            if (found != m_indexByName.end())
            {
                m_indexByName[surface.textureName] = found->second;
            }
        }
    }
}

int32_t EditorMaterialTable::IndexOf(const std::string &name) const
{
    const auto it = m_indexByName.find(name);
    return it == m_indexByName.end() ? -1 : int32_t(it->second);
}

int32_t EditorMaterialTable::Ensure(const std::string &name)
{
    const auto it = m_indexByName.find(name);

    if (it != m_indexByName.end())
    {
        return int32_t(it->second);
    }

    m_indexByName[name] = m_entries.size();
    m_entries.push_back(Entry{ name, EditorMaterialParams{} });
    return int32_t(m_entries.size() - 1);
}

const EditorMaterialParams &EditorMaterialTable::For(const EditorSurface &surface) const
{
    const auto it = m_indexByName.find(surface.textureName);

    if (it == m_indexByName.end())
    {
        return m_defaults;
    }

    return m_entries[it->second].params;
}

const std::string &EditorMaterialTable::EffectiveNameFor(const EditorSurface &surface) const
{
    const auto it = m_indexByName.find(surface.textureName);

    if (it == m_indexByName.end())
    {
        return surface.textureName;
    }

    return m_entries[it->second].name;
}

void EditorMaterialTable::BindSurfaceAlias(const std::string &surfaceName, int32_t entryIndex)
{
    if (!surfaceName.empty() && entryIndex >= 0 && size_t(entryIndex) < m_entries.size())
    {
        m_indexByName[surfaceName] = size_t(entryIndex);
    }
}

namespace
{

int32_t ActiveEmissiveIndex(const EditorMaterialParams &params,
                            const std::function<int32_t(const std::string &)> &resolveTexture)
{
    return params.enabled ? resolveTexture(params.emissiveTexture) : -1;
}

bool ShouldMaskEmission(const EditorMaterialParams &params, int32_t emissive)
{
    return params.enabled && params.hasColorEmissive && !params.emissiveBlocks.empty() &&
           emissive < 0;
}

float BakeEmissionFactor(const EditorMaterialParams &params)
{
    return params.enabled ? params.emissiveFactor : 0.0f;
}

bool ShouldBlendEmission(const EditorMaterialParams &params)
{
    return params.enabled && params.lumaBlend >= 0;
}

float SampleBakedEmissionIfEnabled(bool enabled, const BakeEmissionInputs &inputs, uint32_t x,
                                   uint32_t y, uint8_t &pixelBlend, uint8_t outEmissRgb[3],
                                   bool &outHasEmissRgb)
{
    if (!enabled)
    {
        pixelBlend = 0;
        outHasEmissRgb = false;
        return 0.0f;
    }

    return SampleBakeEmission(inputs, x, y, pixelBlend, outEmissRgb, outHasEmissRgb);
}

bool IsBakeEmissionValid(bool enabled, bool hasEmissiveImage, bool hasColorEmission,
                         bool hasActiveFullbright, double meanBase)
{
    return enabled && meanBase > 1.0e-6 &&
           (hasEmissiveImage || hasColorEmission || hasActiveFullbright);
}

bool ShouldMaskGloss(const EditorMaterialParams &params, int32_t gloss)
{
    return params.hasColorGloss && !params.glossBlocks.empty() && gloss < 0;
}

void BuildColorGlossIfNeeded(bool masksGloss, const EditorMaterialParams &params,
                             const SourceImage &albedo, ColorGlossBuffers &out)
{
    if (masksGloss)
    {
        BuildColorGloss(params, albedo, out);
    }
}

struct BakeNeedFlags
{
    int32_t gloss = -1;
    bool wantsNormal = false;
    int32_t emissive = -1;
    bool passesLight = false;
    bool blendsEmission = false;
    bool metalFromNormalAlpha = false;
    bool masksEmission = false;
    bool masksGloss = false;
    bool paintsOverlay = false;
    bool fullbright = false;
    bool uniformLightGlow = false;
    bool needsSss = false;
    bool neutralMirrorAlbedo = false;
};

bool ShouldBakeSss(const EditorMaterialParams &params, int32_t normal, int32_t sssTex, bool glass)
{
    if (glass || !HasEditorSubsurface(params))
    {
        return false;
    }

    return sssTex >= 0 || !params.sssBlocks.empty() || normal >= 0;
}

bool NeedsMaterialBake(const BakeNeedFlags &f)
{
    return f.gloss >= 0 || f.wantsNormal || f.emissive >= 0 || f.passesLight || f.blendsEmission ||
           f.metalFromNormalAlpha || f.masksEmission || f.masksGloss || f.paintsOverlay ||
           f.fullbright || f.uniformLightGlow || f.needsSss || f.neutralMirrorAlbedo;
}

SourceImage SelectBakeFallbackImage(const SourceImage &albedo, const SourceImage &gloss,
                                    const SourceImage &normal, const SourceImage &emissive,
                                    const SourceImage &overlay, const SourceImage &sssTex)
{
    if (albedo.Valid())
    {
        return albedo;
    }
    if (gloss.Valid())
    {
        return gloss;
    }
    if (normal.Valid())
    {
        return normal;
    }
    if (emissive.Valid())
    {
        return emissive;
    }
    if (overlay.Valid())
    {
        return overlay;
    }
    return sssTex;
}

std::vector<uint8_t> BuildEditorSssMaskIfNeeded(bool sssEnabled, const EditorMaterialParams &params,
                                                const SourceImage &albedoImage,
                                                const SourceImage &sssTexImage)
{
    if (!sssEnabled)
    {
        return {};
    }

    return BuildEditorSssMask(params, albedoImage.width, albedoImage.height, albedoImage.rgba,
                              sssTexImage.rgba, sssTexImage.width, sssTexImage.height);
}

uint8_t ResolveBakedNormalAlpha(const std::vector<uint8_t> &sssMaskBytes, size_t pixelIndex,
                                float glassAlpha)
{
    if (pixelIndex < sssMaskBytes.size())
    {
        return sssMaskBytes[pixelIndex];
    }

    return uint8_t(std::min(255.0f, std::max(0.0f, glassAlpha * 255.0f)));
}

struct BakeRoughnessInputs
{
    const SourceImage &gloss;
    const SourceImage &normal;
    const ColorGlossBuffers &colorGloss;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    uint32_t maskWidth = 0;
    uint32_t maskHeight = 0;
    bool metalFromNormalAlpha = false;
    bool mirror = false;
    bool hasRoughnessOverride = false;
    float defaultRoughness = 1.0f;
    float defaultMetallic = 0.0f;
};

void SampleBakeRoughnessMetallic(const BakeRoughnessInputs &inputs, uint32_t x, uint32_t y,
                                 float &roughness, float &metallic)
{
    roughness = inputs.defaultRoughness;
    metallic = inputs.defaultMetallic;

    if (inputs.metalFromNormalAlpha)
    {
        uint8_t normalPixel[4] = {};
        SampleSourceBilinear(inputs.normal, x, y, inputs.targetWidth, inputs.targetHeight,
                             normalPixel);
        metallic = (normalPixel[3] / 255.0f) * inputs.defaultMetallic;
    }

    if (inputs.gloss.Valid())
    {
        uint8_t glossPixel[4] = {};
        SampleSourceBilinear(inputs.gloss, x, y, inputs.targetWidth, inputs.targetHeight,
                             glossPixel);
        const float glossVal = glossPixel[0] / 255.0f;
        if (inputs.mirror)
        {
            roughness = 0.0f;
        }
        else if (inputs.hasRoughnessOverride)
        {
            roughness = 1.0f - glossVal * (1.0f - inputs.defaultRoughness);
        }
        else
        {
            roughness = 1.0f - glossVal;
        }
        return;
    }

    if (inputs.colorGloss.weight.empty() || inputs.maskWidth == 0 || inputs.maskHeight == 0)
    {
        return;
    }

    // Procedural gloss blocks own the roughness/metalness of selected texels, while unselected
    // texels fall back to the matte baseline (mirror still forces roughness to 0).
    roughness = inputs.mirror ? 0.0f : kEditorDefaultRoughness;
    if (!inputs.metalFromNormalAlpha)
    {
        metallic = 0.0f;
    }

    const uint32_t mx =
        uint32_t(uint64_t(x) * inputs.maskWidth / inputs.targetWidth) % inputs.maskWidth;
    const uint32_t my =
        uint32_t(uint64_t(y) * inputs.maskHeight / inputs.targetHeight) % inputs.maskHeight;
    const size_t pixel = size_t(my) * inputs.maskWidth + mx;
    const float w = inputs.colorGloss.weight[pixel];

    if (!(w > 0.0f))
    {
        return;
    }

    if (!inputs.mirror)
    {
        roughness += (inputs.colorGloss.roughness[pixel] - roughness) * w;
    }

    if (!inputs.metalFromNormalAlpha)
    {
        metallic += (inputs.colorGloss.metalness[pixel] - metallic) * w;
    }
}

void SampleBakeNormal(const SourceImage &normalImage, uint32_t targetWidth, uint32_t targetHeight,
                      float bumpScale, uint32_t x, uint32_t y, uint8_t *outNormal)
{
    if (!normalImage.Valid())
    {
        outNormal[0] = kFlatNormal[0];
        outNormal[1] = kFlatNormal[1];
        outNormal[2] = kFlatNormal[2];
        return;
    }

    uint8_t normalPixel[4] = {};
    SampleSourceBilinear(normalImage, x, y, targetWidth, targetHeight, normalPixel);
    const float nx0 = (float(normalPixel[0]) - 128.0f) / 127.0f;
    const float ny0 = (float(normalPixel[1]) - 128.0f) / 127.0f;
    const float nz0 = std::max(0.25f, (float(normalPixel[2]) - 128.0f) / 127.0f);
    constexpr float kBumpSlopeGain = 2.0f;
    const float sx = (nx0 / nz0) * bumpScale * kBumpSlopeGain;
    const float sy = (ny0 / nz0) * bumpScale * kBumpSlopeGain;
    outNormal[0] = uint8_t(std::clamp(std::lround(sx * 127.0f + 128.0f), 0l, 255l));
    outNormal[1] = uint8_t(std::clamp(std::lround(sy * 127.0f + 128.0f), 0l, 255l));
    outNormal[2] = normalPixel[2];
}

} // namespace

void EditorMaterialBakes::Destroy(QrInstance instance)
{
    if (instance != nullptr)
    {
        for (Entry &entry : m_entries)
        {
            if (entry.material != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(instance, entry.material);
            }
        }
    }

    m_entries.clear();
}

void EditorMaterialBakes::EndPass(QrInstance instance)
{
    for (size_t i = m_entries.size(); i-- > 0;)
    {
        if (m_entries[i].used)
        {
            continue;
        }

        if (m_entries[i].material != QR_NO_MATERIAL && instance != nullptr)
        {
            qrDestroyMaterial(instance, m_entries[i].material);
        }

        m_entries.erase(m_entries.begin() + ptrdiff_t(i));
    }
}

bool EditorMaterialBakes::InvalidateTexture(QrInstance instance, int32_t textureIndex)
{
    if (textureIndex < 0)
    {
        return false;
    }

    bool invalidated = false;
    for (size_t i = m_entries.size(); i-- > 0;)
    {
        const Key &k = m_entries[i].key;
        if (k.albedo != textureIndex && k.gloss != textureIndex && k.normal != textureIndex &&
            k.emissive != textureIndex && k.overlay != textureIndex && k.sssTex != textureIndex)
        {
            continue;
        }

        if (m_entries[i].material != QR_NO_MATERIAL && instance != nullptr)
        {
            qrDestroyMaterial(instance, m_entries[i].material);
        }

        m_entries.erase(m_entries.begin() + ptrdiff_t(i));
        invalidated = true;
    }
    return invalidated;
}

namespace
{

struct BakeEmissionAccum
{
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double mean = 0.0;
    double meanBase = 0.0;
    GlowBounds glow;
};

struct BakeAlbedoTarget
{
    const SourceImage &albedoImage;
    const SourceImage &overlayImage;
    float baseFactor = 1.0f;
    bool compositeOverlay = false;
    bool neutralMirrorAlbedo = false;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
};

void StampBakeAlbedoAndOverlay(const BakeAlbedoTarget &target, uint32_t x, uint32_t y,
                               uint8_t *outRgba)
{
    if (target.neutralMirrorAlbedo)
    {
        outRgba[0] = 255u;
        outRgba[1] = 255u;
        outRgba[2] = 255u;
        outRgba[3] = 255u;
    }
    else
    {
        uint8_t base[4] = {};
        SampleSourceBilinear(target.albedoImage, x, y, target.targetWidth, target.targetHeight,
                             base);

        // gl_texmgr.c multiplies the albedo by base_factor and keeps the alpha only where the
        // texture is meant to carry one; a scene image's alpha is already what the map's
        // cutouts use, so it is passed through.
        outRgba[0] = uint8_t(std::min(255.0f, std::max(0.0f, base[0] * target.baseFactor)));
        outRgba[1] = uint8_t(std::min(255.0f, std::max(0.0f, base[1] * target.baseFactor)));
        outRgba[2] = uint8_t(std::min(255.0f, std::max(0.0f, base[2] * target.baseFactor)));
        outRgba[3] = base[3];
    }

    // The game stamps the overlay on afterwards, weighted by its own alpha and rounded
    // to nearest: r = (r * (255 - a) + overlay * 255 + 127) / 255.
    if (target.compositeOverlay)
    {
        uint8_t stamp[4] = {};
        SampleSourceBilinear(target.overlayImage, x, y, target.targetWidth, target.targetHeight,
                             stamp);
        const int alpha = stamp[3];
        const int inverse = 255 - alpha;

        for (int channel = 0; channel < 3; channel++)
        {
            const int value = (int(outRgba[channel]) * inverse + int(stamp[channel]) * 255 + 127) / 255;
            outRgba[channel] = uint8_t(std::min(255, value));
        }
    }
}

EditorMaterialEmission BuildBakeEmissionSummary(bool valid, size_t pixels, uint32_t width,
                                                uint32_t height, const BakeEmissionAccum &accum)
{
    EditorMaterialEmission emissionInfo;
    emissionInfo.valid = valid;
    emissionInfo.mean = float(accum.mean / double(pixels));
    emissionInfo.meanBase = float(accum.meanBase / double(pixels));

    // A mask that emits nowhere keeps the white colour: dividing by its own zero mean would
    // otherwise turn a light the material still declares into one with no colour at all.
    if (emissionInfo.meanBase > 1e-6f)
    {
        const double scale = 1.0 / (double(pixels) * 255.0 * double(emissionInfo.meanBase));

        emissionInfo.color[0] = float(accum.r * scale);
        emissionInfo.color[1] = float(accum.g * scale);
        emissionInfo.color[2] = float(accum.b * scale);
    }

    FinalizeEditorGlowExtents(accum.glow.minX, accum.glow.minY, accum.glow.maxX, accum.glow.maxY,
                              width, height, emissionInfo);
    return emissionInfo;
}

void ApplyUniformLightGlowToPixel(bool uniformLightGlow, const float lightColor[3],
                                  float emissionFactor, uint8_t blendCode, uint8_t *albedoRgba,
                                  float &outEmission, uint8_t &outBlend)
{
    if (!uniformLightGlow)
    {
        return;
    }

    const float glow = std::clamp(emissionFactor, 0.0f, 1.0f);
    for (int c = 0; c < 3; c++)
    {
        const float target = std::clamp(lightColor[c], 0.0f, 1.0f) * 255.0f;
        const float blended = float(albedoRgba[c]) + (target - float(albedoRgba[c])) * glow;
        albedoRgba[c] = uint8_t(std::clamp(std::lround(blended), 0l, 255l));
    }
    outEmission = glow;
    outBlend = (blendCode != 0u) ? blendCode : 2u;
}

struct BakeResolvedContext
{
    int32_t albedo = -1;
    int32_t gloss = -1;
    int32_t normal = -1;
    int32_t emissive = -1;
    int32_t overlay = -1;
    int32_t sssTex = -1;
    int32_t emissiveStageIndex = -1;
    bool glass = false;
    bool passesLight = false;
    bool compositeOverlay = false;
    bool masksEmission = false;
    bool masksGloss = false;
    bool activeFullbright = false;
    bool uniformLightGlow = false;
    bool emissiveIsAnimatedUnderlay = false;
    bool sssEnabled = false;
    bool needsSss = false;
    bool mirror = false;
    bool neutralMirrorAlbedo = false;
    bool hasRoughnessOverride = false;
    float transparency = 1.0f;
    float roughness = 0.0f;
    float metallic = 0.0f;
    float emission = 0.0f;
    float underlayOpenFraction = 1.0f;
    const EditorTexture *albedoTex = nullptr;
    SourceImage overlayImage;
    SourceImage glossImage;
    SourceImage normalImage;
    SourceImage emissiveImage;
    SourceImage sssImage;
    SourceImage albedoImage;
    std::vector<SourceImage> upperCoverImages;
    std::vector<bool> upperCoverInvertedAlpha;
    std::string emissiveMask;
    std::string glossMask;
    std::string sssMask;
    float lightColor[3] = { 1.0f, 1.0f, 1.0f };
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
};

int32_t FindMatchingCompositeStageIndex(const EditorTexture *albedoTex,
                                        const std::string &emissiveTexture)
{
    if (albedoTex == nullptr || albedoTex->shaderStages.empty() || emissiveTexture.empty())
    {
        return -1;
    }
    const std::string targetStem =
        StripEditorImageExtension(NormalizeEditorAssetPath(emissiveTexture));
    if (targetStem.empty())
    {
        return -1;
    }
    for (size_t i = 0; i < albedoTex->shaderStages.size(); i++)
    {
        const EditorOverlayStage &st = albedoTex->shaderStages[i];
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

float MeanOpenTransmittance(const SourceImage &img, bool invertedAlpha)
{
    const size_t total = size_t(img.width) * size_t(img.height);
    if (total == 0 || img.rgba == nullptr)
    {
        return 1.0f;
    }
    double sum = 0.0;
    for (size_t i = 0; i < total; i++)
    {
        const float a = float(img.rgba[i * 4u + 3u]) / 255.0f;
        sum += double(invertedAlpha ? a : (1.0f - a));
    }
    return std::clamp(float(sum / double(total)), 0.05f, 1.0f);
}

bool IsUpperCoverStageCandidate(const EditorOverlayStage &st, uint32_t disabledStagesMask,
                                  size_t stageIdx)
{
    if (stageIdx < 32u && ((disabledStagesMask >> stageIdx) & 1u) != 0u)
    {
        return false;
    }
    if (st.isLightmap || st.isWhite || st.isEnvironment)
    {
        return false;
    }
    return st.blendDst == QR_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
           st.blendDst == QR_BLEND_FACTOR_SRC_ALPHA || st.alphaTested;
}

bool IsStageAnimatedCutoutUnderlay(const EditorTexture &tex, int32_t stageIdx)
{
    if (stageIdx < 0 || size_t(stageIdx) >= tex.baseStageIndex || !tex.isCutoutUnderlay)
    {
        return false;
    }
    const EditorOverlayStage &st = tex.shaderStages[size_t(stageIdx)];
    return !st.tcMods.empty() || st.animationMaps.size() > 1u || st.animated;
}

void ResolveUpperCompositeOcclusion(
    const EditorMaterialParams &params, const EditorScene &scene,
    const std::function<int32_t(const std::string &)> &resolveTexture, BakeResolvedContext &out)
{
    out.emissiveStageIndex = FindMatchingCompositeStageIndex(out.albedoTex, params.emissiveTexture);
    if (out.emissiveStageIndex < 0 || out.albedoTex == nullptr)
    {
        return;
    }
    out.emissiveIsAnimatedUnderlay =
        IsStageAnimatedCutoutUnderlay(*out.albedoTex, out.emissiveStageIndex);

    const size_t stageCount = out.albedoTex->shaderStages.size();
    const size_t baseStageIdx = out.albedoTex->baseStageIndex;
    for (size_t i = size_t(out.emissiveStageIndex) + 1u; i < stageCount; i++)
    {
        const EditorOverlayStage st = out.albedoTex->shaderStages[i];
        if (!IsUpperCoverStageCandidate(st, params.disabledStagesMask, i))
        {
            continue;
        }
        const int32_t coverTexIdx =
            (i == baseStageIdx) ? -1 : resolveTexture(StripEditorImageExtension(st.map));
        out.albedoTex = TextureAt(scene, out.albedo);
        const SourceImage coverImg =
            (i == baseStageIdx) ? out.albedoImage : SourceOf(scene, coverTexIdx);
        if (!coverImg.Valid())
        {
            continue;
        }
        const bool invertedAlpha = st.blendDst == QR_BLEND_FACTOR_SRC_ALPHA;
        if (out.emissiveIsAnimatedUnderlay)
        {
            out.underlayOpenFraction *= MeanOpenTransmittance(coverImg, invertedAlpha);
        }
        else
        {
            out.upperCoverImages.push_back(coverImg);
            out.upperCoverInvertedAlpha.push_back(invertedAlpha);
        }
    }
}

void ComputeBakeTargetDimensions(const BakeResolvedContext &ctx, uint32_t &outW, uint32_t &outH)
{
    uint32_t w = ctx.albedoImage.width;
    uint32_t h = ctx.albedoImage.height;
    for (const SourceImage *img : { &ctx.normalImage, &ctx.glossImage, &ctx.emissiveImage,
                                    &ctx.overlayImage, &ctx.sssImage })
    {
        if (img->Valid())
        {
            w = std::max(w, img->width);
            h = std::max(h, img->height);
        }
    }
    if ((ctx.emissiveImage.Valid() || ctx.activeFullbright) && w > 0 && h > 0)
    {
        const uint32_t minDim = std::min(w, h);
        if (minDim < 256u)
        {
            const double upScale = 256.0 / double(minDim);
            w = uint32_t(std::lround(double(w) * upScale));
            h = uint32_t(std::lround(double(h) * upScale));
        }
    }
    constexpr uint32_t kMaxBakeDim = 2048u;
    const uint32_t maxDim = std::max(w, h);
    if (maxDim > kMaxBakeDim)
    {
        const double scale = double(kMaxBakeDim) / double(maxDim);
        w = std::max<uint32_t>(1u, uint32_t(std::lround(double(w) * scale)));
        h = std::max<uint32_t>(1u, uint32_t(std::lround(double(h) * scale)));
    }
    outW = w;
    outH = h;
}

bool ComputeUniformLightGlow(const EditorMaterialParams &params, int32_t emissive,
                             bool masksEmission, bool activeFullbright)
{
    return params.enabled && params.luma && emissive < 0 && !masksEmission && !activeFullbright;
}

void PopulateBakeScalarsAndMasks(const EditorMaterialParams &params, bool paintsOverlay,
                                 BakeResolvedContext &out)
{
    out.compositeOverlay = paintsOverlay && out.overlayImage.rgba != out.albedoImage.rgba;
    if (!out.compositeOverlay)
    {
        out.overlay = -1;
    }
    out.sssEnabled = HasEditorSubsurface(params) && !out.glass;
    out.transparency = out.glass ? params.transparency : 1.0f;
    out.mirror = params.mirror && !out.glass && params.type != EditorMaterialType::Water;
    out.hasRoughnessOverride = params.roughnessOverride >= 0.0f;
    out.roughness = out.mirror ? 0.0f
                  : out.hasRoughnessOverride ? params.roughnessOverride
                                             : kEditorDefaultRoughness;
    out.metallic = params.metalnessFactor;
    out.emission = BakeEmissionFactor(params);
    if (out.masksEmission)
    {
        out.emissiveMask = EmissiveMaskOf(params.emissiveBlocks);
    }
    if (out.masksGloss)
    {
        out.glossMask = GlossMaskOf(params.glossBlocks);
    }
    if (out.sssEnabled)
    {
        out.sssMask = SerializeFactorBlocks(params.sssBlocks);
    }
    if (out.uniformLightGlow)
    {
        std::copy_n(params.lightColor, 3, out.lightColor);
    }
}

bool IsEnvironmentBaseTexture(const EditorTexture *tex)
{
    return tex != nullptr && tex->baseStageIndex < tex->shaderStages.size() &&
           tex->shaderStages[tex->baseStageIndex].isEnvironment;
}

int32_t ResolveVisibleEmissiveIndex(const EditorMaterialParams &params,
                                    const EditorTexture *albedoTex,
                                    int32_t emissive)
{
    if (emissive < 0 || albedoTex == nullptr)
    {
        return emissive;
    }

    const int32_t emStage = FindMatchingCompositeStageIndex(albedoTex, params.emissiveTexture);
    if (emStage >= 0 && emStage < 32 &&
        ((params.disabledStagesMask >> uint32_t(emStage)) & 1u) != 0u)
    {
        return -1;
    }
    return emissive;
}

bool FinalizeBakeContextGeometry(const EditorMaterialParams &params,
                                  const EditorScene &scene,
                                  const std::function<int32_t(const std::string &)> &resolveTexture,
                                  bool paintsOverlay, BakeResolvedContext &out)
{
    if (!out.albedoImage.Valid())
    {
        if (!out.neutralMirrorAlbedo)
        {
            return false;
        }
        PopulateBakeScalarsAndMasks(params, paintsOverlay, out);
        out.targetWidth = 1u;
        out.targetHeight = 1u;
        return true;
    }

    PopulateBakeScalarsAndMasks(params, paintsOverlay, out);
    ResolveUpperCompositeOcclusion(params, scene, resolveTexture, out);
    ComputeBakeTargetDimensions(out, out.targetWidth, out.targetHeight);
    return true;
}

bool ResolveBakeContext(const std::string &name, const EditorMaterialParams &params,
                        const EditorScene &scene,
                        const std::function<int32_t(const std::string &)> &resolveTexture,
                        bool forceEnvBase, BakeResolvedContext &out)
{
    out.glass = params.type == EditorMaterialType::Glass;
    const int32_t baseIdx = resolveTexture(params.baseTexture);
    out.albedo = baseIdx >= 0 ? baseIdx : resolveTexture(name);
    out.gloss = resolveTexture(params.glossTexture);
    out.normal = resolveTexture(params.normalTexture);
    out.overlay = resolveTexture(params.overlayTexture);
    out.sssTex = resolveTexture(params.textureSss);
    out.passesLight = out.glass && params.transparency != 1.0f;
    out.albedoTex = TextureAt(scene, out.albedo);
    out.emissive = ResolveVisibleEmissiveIndex(params, out.albedoTex,
                                               ActiveEmissiveIndex(params, resolveTexture));

    const bool mirrorActive = params.mirror && !out.glass && params.type != EditorMaterialType::Water;
    out.neutralMirrorAlbedo = mirrorActive && params.baseTexture.empty() &&
                              (forceEnvBase || IsEnvironmentBaseTexture(out.albedoTex));
    out.overlayImage = SourceOf(scene, out.overlay);
    const bool paintsOverlay = out.overlayImage.Valid() && !out.glass;
    out.masksEmission = ShouldMaskEmission(params, out.emissive);
    out.masksGloss = ShouldMaskGloss(params, out.gloss);
    out.activeFullbright = HasActiveFullbright(out.albedoTex);
    out.uniformLightGlow = ComputeUniformLightGlow(params, out.emissive, out.masksEmission,
                                                   out.activeFullbright);
    out.needsSss = ShouldBakeSss(params, out.normal, out.sssTex, out.glass);

    const BakeNeedFlags needFlags{ out.gloss,
                                   out.normal >= 0,
                                   out.emissive,
                                   out.passesLight,
                                   ShouldBlendEmission(params),
                                   params.metalnessFromNormalAlpha,
                                   out.masksEmission,
                                   out.masksGloss,
                                   paintsOverlay,
                                   NeedsFullbrightBake(out.albedoTex, params),
                                   out.uniformLightGlow,
                                   out.needsSss,
                                   out.neutralMirrorAlbedo };
    if (!NeedsMaterialBake(needFlags))
    {
        return false;
    }

    out.glossImage = SourceOf(scene, out.gloss);
    out.normalImage = SourceOf(scene, out.normal);
    out.emissiveImage = SourceOf(scene, out.emissive);
    out.sssImage = SourceOf(scene, out.sssTex);
    out.albedoImage = SelectBakeFallbackImage(SourceOf(scene, out.albedo), out.glossImage,
                                              out.normalImage, out.emissiveImage, out.overlayImage,
                                              out.sssImage);
    return FinalizeBakeContextGeometry(params, scene, resolveTexture, paintsOverlay, out);
}

struct BakeRasterOutputs
{
    std::vector<uint8_t> *albedoData = nullptr;
    std::vector<uint8_t> *rmeData = nullptr;
    std::vector<uint8_t> *normalData = nullptr;
    BakeEmissionAccum *emissAccum = nullptr;
    bool *hasColorEmission = nullptr;
    bool *bakeNormalSlot = nullptr;
};

void RasterizeMaterialBakePixels(const EditorMaterialParams &params, const BakeResolvedContext &ctx,
                                 const BakeRasterOutputs &out)
{
    std::vector<uint8_t> &albedoData = *out.albedoData;
    std::vector<uint8_t> &rmeData = *out.rmeData;
    std::vector<uint8_t> &normalData = *out.normalData;
    BakeEmissionAccum &emissAccum = *out.emissAccum;
    const size_t pixels = size_t(ctx.targetWidth) * size_t(ctx.targetHeight);
    albedoData.resize(pixels * 4);
    rmeData.resize(pixels * 4);
    normalData.resize(pixels * 4);

    const uint8_t blendCode = params.lumaBlend < 0 ? 0 : uint8_t(params.lumaBlend + 1);
    const bool metalFromNormalAlpha = params.metalnessFromNormalAlpha && ctx.normalImage.Valid();

    std::vector<float> colorEmission;
    std::vector<uint8_t> colorBlend;
    if (ctx.masksEmission)
    {
        BuildColorEmissive(params, ctx.albedoImage, colorEmission, colorBlend);
    }
    *out.hasColorEmission = !colorEmission.empty();

    ColorGlossBuffers colorGloss;
    BuildColorGlossIfNeeded(ctx.masksGloss, params, ctx.albedoImage, colorGloss);
    const std::vector<uint8_t> sssMaskBytes =
        BuildEditorSssMaskIfNeeded(ctx.sssEnabled, params, ctx.albedoImage, ctx.sssImage);
    *out.bakeNormalSlot = ctx.normalImage.Valid() || ctx.passesLight || !sssMaskBytes.empty();
    const BakeRoughnessInputs roughnessInputs{
        ctx.glossImage,           ctx.normalImage,        colorGloss,
        ctx.targetWidth,          ctx.targetHeight,       ctx.albedoImage.width,
        ctx.albedoImage.height,   metalFromNormalAlpha,   ctx.mirror,
        ctx.hasRoughnessOverride, ctx.roughness,          ctx.metallic,
    };
    const BakeEmissionInputs emissionInputs{
        ctx.emissiveImage,          ctx.albedoTex,
        colorEmission,              colorBlend,
        &ctx.upperCoverImages,      &ctx.upperCoverInvertedAlpha,
        ctx.targetWidth,            ctx.targetHeight,
        ctx.albedoImage.width,      ctx.albedoImage.height,
        ctx.emission,               blendCode,
    };

    const BakeAlbedoTarget albedoTarget{
        ctx.albedoImage,         ctx.overlayImage, params.baseFactor,
        ctx.compositeOverlay,    ctx.neutralMirrorAlbedo,
        ctx.targetWidth,         ctx.targetHeight,
    };

    for (uint32_t y = 0; y < ctx.targetHeight; y++)
    {
        for (uint32_t x = 0; x < ctx.targetWidth; x++)
        {
            const size_t offset = (size_t(y) * ctx.targetWidth + x) * 4;
            StampBakeAlbedoAndOverlay(albedoTarget, x, y, albedoData.data() + offset);

            size_t maskPixelIndex = size_t(-1);
            if (!sssMaskBytes.empty() && ctx.albedoImage.Valid())
            {
                const uint32_t mx =
                    uint32_t(uint64_t(x) * ctx.albedoImage.width / ctx.targetWidth) %
                    ctx.albedoImage.width;
                const uint32_t my =
                    uint32_t(uint64_t(y) * ctx.albedoImage.height / ctx.targetHeight) %
                    ctx.albedoImage.height;
                maskPixelIndex = size_t(my) * ctx.albedoImage.width + mx;
            }

            float roughness = ctx.roughness;
            float metallic = ctx.metallic;
            SampleBakeRoughnessMetallic(roughnessInputs, x, y, roughness, metallic);

            uint8_t pixelBlend = blendCode;
            uint8_t emissRgb[3] = {};
            bool hasEmissRgb = false;
            float emission = SampleBakedEmissionIfEnabled(
                params.enabled, emissionInputs, x, y, pixelBlend, emissRgb, hasEmissRgb);
            if (emission > 0.0f)
            {
                const uint8_t *colorSrc =
                    (hasEmissRgb && (int(emissRgb[0]) + int(emissRgb[1]) + int(emissRgb[2])) > 0)
                        ? emissRgb
                        : (albedoData.data() + offset);
                emissAccum.r += colorSrc[0] * emission;
                emissAccum.g += colorSrc[1] * emission;
                emissAccum.b += colorSrc[2] * emission;
            }
            emissAccum.mean += std::min(1.0f, emission);
            emissAccum.meanBase += emission;
            AccumulateGlow(emission, x, y, emissAccum.glow);

            ApplyUniformLightGlowToPixel(ctx.uniformLightGlow, params.lightColor, ctx.emission,
                                         blendCode, albedoData.data() + offset, emission,
                                         pixelBlend);

            rmeData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, roughness * 255.0f)));
            rmeData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, metallic * 255.0f)));
            rmeData[offset + 2] = uint8_t(std::min(255.0f, std::max(0.0f, emission * 255.0f)));
            rmeData[offset + 3] = pixelBlend;

            SampleBakeNormal(ctx.normalImage, ctx.targetWidth, ctx.targetHeight, params.bumpScale,
                             x, y, normalData.data() + offset);
            normalData[offset + 3] =
                ResolveBakedNormalAlpha(sssMaskBytes, maskPixelIndex, params.transparency);
        }
    }
    emissAccum.mean *= double(ctx.underlayOpenFraction);
}

} // namespace

QrMaterial EditorMaterialBakes::Get(QrInstance instance, const std::string &name,
                                    const EditorMaterialParams &params, const EditorScene &scene,
                                    const std::vector<QrMaterial> &pool,
                                    const std::function<int32_t(const std::string &)> &resolveTexture,
                                    bool forceEnvBase)
{
    (void)pool;
    BakeResolvedContext ctx;
    if (!ResolveBakeContext(name, params, scene, resolveTexture, forceEnvBase, ctx))
    {
        return QR_NO_MATERIAL;
    }

    Key key;
    key.albedo = ctx.albedo;
    key.gloss = ctx.gloss;
    key.normal = ctx.normal;
    key.emissive = ctx.emissive;
    key.overlay = ctx.overlay;
    key.emissiveStageIndex = ctx.emissiveStageIndex;
    key.disabledStagesMask = params.disabledStagesMask;
    key.sssTex = ctx.sssEnabled ? ctx.sssTex : -1;
    key.width = ctx.targetWidth;
    key.height = ctx.targetHeight;
    key.bumpScale = params.bumpScale;
    key.transparency = ctx.transparency;
    key.lumaBlend = params.lumaBlend;
    key.metalnessFromNormalAlpha = params.metalnessFromNormalAlpha;
    key.enabled = params.enabled;
    key.uniformLightGlow = ctx.uniformLightGlow;
    std::copy_n(ctx.lightColor, 3, key.lightColor);
    key.sssEnabled = ctx.sssEnabled;
    key.emissiveMask = ctx.emissiveMask;
    key.glossMask = ctx.glossMask;
    key.sssMask = ctx.sssMask;
    key.hasRoughnessOverride = ctx.hasRoughnessOverride;
    key.mirror = ctx.mirror;
    key.neutralMirrorAlbedo = ctx.neutralMirrorAlbedo;
    key.roughness = ctx.roughness;
    key.metallic = ctx.metallic;
    key.emission = ctx.emission;

    for (Entry &entry : m_entries)
    {
        if (entry.key == key)
        {
            entry.used = true;
            return entry.material;
        }
    }

    std::vector<uint8_t> albedoData;
    std::vector<uint8_t> rmeData;
    std::vector<uint8_t> normalData;
    BakeEmissionAccum emissAccum{
        0.0, 0.0, 0.0, 0.0, 0.0, { int(ctx.targetWidth), int(ctx.targetHeight), -1, -1 }
    };
    bool hasColorEmission = false;
    bool bakeNormalSlot = false;
    const BakeRasterOutputs bakeOutputs{
        &albedoData, &rmeData, &normalData, &emissAccum, &hasColorEmission, &bakeNormalSlot,
    };
    RasterizeMaterialBakePixels(params, ctx, bakeOutputs);

    QrMaterialCreateInfo info = {};
    info.size = { ctx.targetWidth, ctx.targetHeight };
    info.textures.pDataAlbedoAlpha = albedoData.data();
    info.textures.pDataRoughnessMetallicEmission = rmeData.data();
    info.textures.pDataNormal = bakeNormalSlot ? normalData.data() : nullptr;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    QrMaterial material = QR_NO_MATERIAL;
    if (instance == nullptr || qrCreateMaterial(instance, &info, &material) != QR_SUCCESS)
    {
        return QR_NO_MATERIAL;
    }

    const size_t pixels = size_t(ctx.targetWidth) * size_t(ctx.targetHeight);
    const bool emissionValid = IsBakeEmissionValid(key.enabled, ctx.emissiveImage.Valid(),
                                                   hasColorEmission, ctx.activeFullbright,
                                                   emissAccum.meanBase);
    const EditorMaterialEmission emissionInfo = BuildBakeEmissionSummary(
        emissionValid, pixels, ctx.targetWidth, ctx.targetHeight, emissAccum);

    m_entries.push_back(Entry{ key, material, emissionInfo, true });
    return material;
}

EditorMaterialEmission EditorMaterialBakes::EmissionOf(QrMaterial material) const
{
    if (material != QR_NO_MATERIAL)
    {
        for (const Entry &entry : m_entries)
        {
            if (entry.material == material)
            {
                return entry.emission;
            }
        }
    }

    return EditorMaterialEmission{};
}

namespace
{

size_t WrapIndex(int32_t coord, uint32_t limit)
{
    const int32_t s = int32_t(limit);
    const int32_t m = coord % s;
    return size_t(m < 0 ? (m + s) : m);
}

void WrappedBlurHorizontal(const std::vector<float> &src, uint32_t width, uint32_t height,
                           int32_t radius, std::vector<float> &dst)
{
    const float invWindow = 1.0f / float(radius * 2 + 1);
    for (uint32_t y = 0; y < height; y++)
    {
        const size_t row = size_t(y) * width;
        float sum = 0.0f;
        for (int32_t dx = -radius; dx <= radius; dx++)
        {
            sum += src[row + WrapIndex(dx, width)];
        }
        for (uint32_t x = 0; x < width; x++)
        {
            dst[row + x] = sum * invWindow;
            sum += src[row + WrapIndex(int32_t(x) + radius + 1, width)];
            sum -= src[row + WrapIndex(int32_t(x) - radius, width)];
        }
    }
}

void WrappedBlurVertical(const std::vector<float> &src, uint32_t width, uint32_t height,
                         int32_t radius, std::vector<float> &dst)
{
    const float invWindow = 1.0f / float(radius * 2 + 1);
    for (uint32_t x = 0; x < width; x++)
    {
        float sum = 0.0f;
        for (int32_t dy = -radius; dy <= radius; dy++)
        {
            sum += src[WrapIndex(dy, height) * width + x];
        }
        for (uint32_t y = 0; y < height; y++)
        {
            dst[size_t(y) * width + x] = sum * invWindow;
            sum += src[WrapIndex(int32_t(y) + radius + 1, height) * width + x];
            sum -= src[WrapIndex(int32_t(y) - radius, height) * width + x];
        }
    }
}

void WrappedBoxBlur(const std::vector<float> &src, uint32_t width, uint32_t height, int32_t radius,
                    std::vector<float> &out)
{
    if (radius <= 0 || width == 0 || height == 0)
    {
        out = src;
        return;
    }
    std::vector<float> tmp(src.size(), 0.0f);
    out.resize(src.size(), 0.0f);
    WrappedBlurHorizontal(src, width, height, radius, tmp);
    WrappedBlurVertical(tmp, width, height, radius, out);
}

struct AiDiffuseFields
{
    uint32_t width = 0;
    uint32_t height = 0;
    float medianLuma = 0.5f;
    std::vector<float> luma;
    std::vector<float> sat;
};

void ExtractDiffuseFields(const uint8_t *rgba, uint32_t width, uint32_t height,
                          AiDiffuseFields &out)
{
    const size_t pixels = size_t(width) * size_t(height);
    out.width = width;
    out.height = height;
    out.luma.resize(pixels);
    out.sat.resize(pixels);
    uint32_t hist[256] = {};

    for (size_t i = 0; i < pixels; i++)
    {
        const float r = float(rgba[i * 4u + 0u]) / 255.0f;
        const float g = float(rgba[i * 4u + 1u]) / 255.0f;
        const float b = float(rgba[i * 4u + 2u]) / 255.0f;
        const float y = 0.299f * r + 0.587f * g + 0.114f * b;
        const float cMax = std::max({ r, g, b });
        const float cMin = std::min({ r, g, b });
        out.luma[i] = y;
        out.sat[i] = cMax > 1e-4f ? ((cMax - cMin) / cMax) : 0.0f;
        const size_t bin =
            std::min<size_t>(255u, size_t(std::clamp(y * 255.0f + 0.5f, 0.0f, 255.0f)));
        hist[bin]++;
    }

    const size_t target = pixels / 2u;
    size_t accum = 0;
    out.medianLuma = 0.5f;
    for (size_t b = 0; b < 256u; b++)
    {
        accum += hist[b];
        if (accum >= target)
        {
            out.medianLuma = float(b) / 255.0f;
            break;
        }
    }
}

void BuildMultiScaleHeightField(const AiDiffuseFields &fields, std::vector<float> &outHeight)
{
    std::vector<float> blur1;
    std::vector<float> blur3;
    WrappedBoxBlur(fields.luma, fields.width, fields.height, 1, blur1);
    WrappedBoxBlur(fields.luma, fields.width, fields.height, 3, blur3);

    outHeight.resize(fields.luma.size());
    for (size_t i = 0; i < fields.luma.size(); i++)
    {
        outHeight[i] =
            0.45f * fields.luma[i] + 0.30f * blur1[i] + 0.25f * blur3[i] + 0.08f * fields.sat[i];
    }
}

uint8_t FloatToByte(float value)
{
    return uint8_t(std::clamp(std::lround(value * 255.0f), 0L, 255L));
}

void ComputeAiNormals(const std::vector<float> &heightField, uint32_t width, uint32_t height,
                      std::vector<uint8_t> &outRgba)
{
    const auto sampleH = [&](int32_t x, int32_t y) -> float {
        return heightField[WrapIndex(y, height) * width + WrapIndex(x, width)];
    };

    outRgba.resize(size_t(width) * size_t(height) * 4u);
    constexpr float kNormalStrength = 6.0f;

    for (uint32_t y = 0; y < height; y++)
    {
        const int32_t iy = int32_t(y);
        for (uint32_t x = 0; x < width; x++)
        {
            const int32_t ix = int32_t(x);
            const float hTL = sampleH(ix - 1, iy - 1);
            const float hT = sampleH(ix, iy - 1);
            const float hTR = sampleH(ix + 1, iy - 1);
            const float hL = sampleH(ix - 1, iy);
            const float hR = sampleH(ix + 1, iy);
            const float hBL = sampleH(ix - 1, iy + 1);
            const float hB = sampleH(ix, iy + 1);
            const float hBR = sampleH(ix + 1, iy + 1);

            const float dHdx =
                (3.0f * (hTR - hTL) + 10.0f * (hR - hL) + 3.0f * (hBR - hBL)) / 32.0f;
            const float dHdy =
                (3.0f * (hBL - hTL) + 10.0f * (hB - hT) + 3.0f * (hBR - hTR)) / 32.0f;

            // OpenGL tangent-space convention: -X left, +Y up in image orientation.
            float nx = -dHdx * kNormalStrength;
            float ny = dHdy * kNormalStrength;
            float nz = 1.0f;
            const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= invLen;
            ny *= invLen;
            nz *= invLen;

            const size_t dst = (size_t(y) * width + x) * 4u;
            outRgba[dst + 0u] = FloatToByte(nx * 0.5f + 0.5f);
            outRgba[dst + 1u] = FloatToByte(ny * 0.5f + 0.5f);
            outRgba[dst + 2u] = FloatToByte(nz * 0.5f + 0.5f);
            outRgba[dst + 3u] = 255u;
        }
    }
}

void ComputeAiGloss(const AiDiffuseFields &fields, const std::vector<float> &heightField,
                    std::vector<uint8_t> &outRgba)
{
    std::vector<float> smoothH;
    WrappedBoxBlur(heightField, fields.width, fields.height, 2, smoothH);

    outRgba.resize(fields.luma.size() * 4u);
    for (size_t i = 0; i < fields.luma.size(); i++)
    {
        const float curvature = heightField[i] - smoothH[i];
        const float metallicCue = fields.luma[i] * (1.0f - 0.55f * fields.sat[i]);
        const float gloss =
            std::clamp(0.18f + 0.52f * metallicCue + 1.65f * curvature, 0.05f, 0.92f);
        const uint8_t gByte = FloatToByte(gloss);
        outRgba[i * 4u + 0u] = gByte;
        outRgba[i * 4u + 1u] = gByte;
        outRgba[i * 4u + 2u] = gByte;
        outRgba[i * 4u + 3u] = 255u;
    }
}

float SmoothStepRange(float edge0, float edge1, float value)
{
    const float t = std::clamp((value - edge0) / std::max(1e-4f, edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void ComputeAiEmissive(const uint8_t *diffuseRgba, const AiDiffuseFields &fields,
                       const uint8_t *fullbrightMask, std::vector<uint8_t> &outRgba)
{
    const size_t pixels = fields.luma.size();
    std::vector<float> core(pixels, 0.0f);

    if (fullbrightMask != nullptr)
    {
        for (size_t i = 0; i < pixels; i++)
        {
            core[i] = float(fullbrightMask[i]) / 255.0f;
        }
    }
    else
    {
        const float lumaGate = std::min(0.82f, std::max(0.45f, fields.medianLuma + 0.20f));
        for (size_t i = 0; i < pixels; i++)
        {
            const float lumaExcess = std::max(0.0f, fields.luma[i] - lumaGate);
            const float chromaGlow = fields.sat[i] * std::max(0.0f, fields.luma[i] - 0.28f);
            const float energy = lumaExcess * 2.5f + chromaGlow * 1.35f;
            core[i] = SmoothStepRange(0.12f, 0.55f, energy);
        }
        std::vector<float> halo;
        WrappedBoxBlur(core, fields.width, fields.height, 1, halo);
        for (size_t i = 0; i < pixels; i++)
        {
            core[i] = std::max(core[i], halo[i] * 0.65f);
        }
    }

    outRgba.resize(pixels * 4u);
    for (size_t i = 0; i < pixels; i++)
    {
        const float m = std::clamp(core[i], 0.0f, 1.0f);
        outRgba[i * 4u + 0u] = uint8_t(std::clamp(std::lround(float(diffuseRgba[i * 4u + 0u]) * m), 0L, 255L));
        outRgba[i * 4u + 1u] = uint8_t(std::clamp(std::lround(float(diffuseRgba[i * 4u + 1u]) * m), 0L, 255L));
        outRgba[i * 4u + 2u] = uint8_t(std::clamp(std::lround(float(diffuseRgba[i * 4u + 2u]) * m), 0L, 255L));
        outRgba[i * 4u + 3u] = 255u;
    }
}

} // namespace

bool GenerateEditorAiTextureFromDiffuse(const uint8_t *diffuseRgba, uint32_t width, uint32_t height,
                                        const uint8_t *fullbrightMask, EditorAiMapKind kind,
                                        std::vector<uint8_t> &outRgba)
{
    if (diffuseRgba == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    AiDiffuseFields fields;
    ExtractDiffuseFields(diffuseRgba, width, height, fields);

    if (kind == EditorAiMapKind::Emissive)
    {
        ComputeAiEmissive(diffuseRgba, fields, fullbrightMask, outRgba);
        return true;
    }

    std::vector<float> heightField;
    BuildMultiScaleHeightField(fields, heightField);

    if (kind == EditorAiMapKind::Normals)
    {
        ComputeAiNormals(heightField, width, height, outRgba);
        return true;
    }

    ComputeAiGloss(fields, heightField, outRgba);
    return true;
}

std::vector<uint8_t> EncodeEditorTgaRgba(const uint8_t *rgba, uint32_t width, uint32_t height)
{
    if (rgba == nullptr || width == 0 || height == 0 || width > 65535u || height > 65535u)
    {
        return {};
    }

    const size_t pixels = size_t(width) * size_t(height);
    std::vector<uint8_t> out(18u + pixels * 4u, 0u);
    out[2] = 2u; // Uncompressed true-color image
    out[12] = uint8_t(width & 0xFFu);
    out[13] = uint8_t((width >> 8u) & 0xFFu);
    out[14] = uint8_t(height & 0xFFu);
    out[15] = uint8_t((height >> 8u) & 0xFFu);
    out[16] = 32u;
    out[17] = 0x28u; // Top-left origin + 8-bit alpha channel

    for (size_t i = 0; i < pixels; i++)
    {
        out[18u + i * 4u + 0u] = rgba[i * 4u + 2u];
        out[18u + i * 4u + 1u] = rgba[i * 4u + 1u];
        out[18u + i * 4u + 2u] = rgba[i * 4u + 0u];
        out[18u + i * 4u + 3u] = rgba[i * 4u + 3u];
    }

    return out;
}
