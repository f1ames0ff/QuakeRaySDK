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

bool NeedsFullbrightBake(const EditorTexture *texture, const EditorMaterialParams &params)
{
    if (!HasFullbright(texture))
    {
        return false;
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

void EvaluateSssBlocksMask(const std::vector<EditorSssBlock> &blocks, const SourceImage &albedo,
                           std::vector<float> &combined)
{
    if (!albedo.Valid())
    {
        return;
    }

    const size_t pixels = size_t(albedo.width) * size_t(albedo.height);
    std::vector<float> mask;

    for (const EditorSssBlock &block : blocks)
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

std::string SssMaskOf(const std::vector<EditorSssBlock> &blocks)
{
    std::string text;
    char buffer[160];

    for (const EditorSssBlock &block : blocks)
    {
        AppendSerializedBlockGeometry(block, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g x%.6g;", double(block.threshold),
                      double(block.feather), double(block.factor));
        text += buffer;
    }

    return text;
}

struct BakeEmissionInputs
{
    const SourceImage &emissive;
    const EditorTexture *albedoTexture = nullptr;
    const std::vector<float> &colorEmission;
    const std::vector<uint8_t> &colorBlend;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    float emissiveFactor = 1.0f;
    uint8_t blendCode = 0;
};

// Samples per-pixel emission and blend mode, merging any Quake fullbright mask over explicit
// luma textures or synthesized colour blocks as in gl_texmgr.c.
float SampleBakeEmission(const BakeEmissionInputs &inputs, uint32_t x, uint32_t y,
                         uint8_t &outBlend)
{
    float emission = 0.0f;
    outBlend = inputs.blendCode;

    if (inputs.emissive.Valid())
    {
        const uint8_t *mask = inputs.emissive.At(x, y, inputs.targetWidth, inputs.targetHeight);
        const float luminance =
            (0.2126f * mask[0] + 0.7152f * mask[1] + 0.0722f * mask[2]) / 255.0f;
        emission = luminance * inputs.emissiveFactor;
    }
    else if (!inputs.colorEmission.empty())
    {
        const size_t pixel = size_t(y) * inputs.targetWidth + x;
        emission = inputs.colorEmission[pixel];
        outBlend = inputs.colorBlend[pixel];
    }

    if (inputs.albedoTexture != nullptr && !inputs.albedoTexture->fullbright.empty() &&
        inputs.albedoTexture->width > 0 && inputs.albedoTexture->height > 0)
    {
        const uint32_t fx = uint32_t(uint64_t(x) * inputs.albedoTexture->width / inputs.targetWidth) %
                            inputs.albedoTexture->width;
        const uint32_t fy = uint32_t(uint64_t(y) * inputs.albedoTexture->height / inputs.targetHeight) %
                            inputs.albedoTexture->height;
        const float fullbright =
            inputs.albedoTexture->fullbright[size_t(fy) * inputs.albedoTexture->width + fx] / 255.0f;

        if (fullbright > emission)
        {
            emission = fullbright;
            outBlend = inputs.blendCode;
        }
    }

    return emission;
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
    const size_t pixels = size_t(width) * size_t(height);
    if (width == 0 || height == 0 || pixels == 0)
    {
        return {};
    }

    const SourceImage texImage{ textureSssRgba, textureSssWidth, textureSssHeight };
    const SourceImage albedoImage{ albedoRgba, width, height };
    if (params.sssBlocks.empty() && !texImage.Valid())
    {
        return std::vector<uint8_t>(pixels, 255u);
    }

    std::vector<float> combined(pixels, 0.0f);
    if (texImage.Valid())
    {
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                const uint8_t *tex = texImage.At(x, y, width, height);
                const float lum =
                    (0.2126f * tex[0] + 0.7152f * tex[1] + 0.0722f * tex[2]) / 255.0f;
                combined[size_t(y) * width + x] = lum;
            }
        }
    }

    if (!params.sssBlocks.empty())
    {
        EvaluateSssBlocksMask(params.sssBlocks, albedoImage, combined);
    }

    std::vector<uint8_t> out(pixels);
    for (size_t i = 0; i < pixels; i++)
    {
        out[i] = uint8_t(std::clamp(combined[i] * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    return out;
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
                                   uint32_t y, uint8_t &pixelBlend)
{
    if (!enabled)
    {
        pixelBlend = 0;
        return 0.0f;
    }

    return SampleBakeEmission(inputs, x, y, pixelBlend);
}

bool IsBakeEmissionValid(bool enabled, bool hasEmissiveImage, bool hasColorEmission,
                         bool hasFullbright)
{
    return enabled && (hasEmissiveImage || hasColorEmission || hasFullbright);
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
    bool needsSss = false;
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
           f.fullbright || f.needsSss;
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
    return overlay.Valid() ? overlay : sssTex;
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
        const uint8_t *normalPixel =
            inputs.normal.At(x, y, inputs.targetWidth, inputs.targetHeight);
        metallic = (normalPixel[3] / 255.0f) * inputs.defaultMetallic;
    }

    if (inputs.gloss.Valid())
    {
        const uint8_t *glossPixel = inputs.gloss.At(x, y, inputs.targetWidth, inputs.targetHeight);
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

    if (inputs.colorGloss.weight.empty())
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

    const size_t pixel = size_t(y) * inputs.targetWidth + x;
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

void SampleBakeNormal(const SourceImage &normalImage, const SourceImage &albedoImage,
                      float bumpScale, uint32_t x, uint32_t y, uint8_t *outNormal)
{
    if (!normalImage.Valid())
    {
        outNormal[0] = kFlatNormal[0];
        outNormal[1] = kFlatNormal[1];
        outNormal[2] = kFlatNormal[2];
        return;
    }

    const uint8_t *normalPixel = normalImage.At(x, y, albedoImage.width, albedoImage.height);
    const float nx = (normalPixel[0] - 128.0f) * bumpScale + 128.0f;
    const float ny = (normalPixel[1] - 128.0f) * bumpScale + 128.0f;
    outNormal[0] = uint8_t(std::min(255.0f, std::max(0.0f, nx)));
    outNormal[1] = uint8_t(std::min(255.0f, std::max(0.0f, ny)));
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

void StampBakeAlbedoAndOverlay(const SourceImage &albedoImage, const SourceImage &overlayImage,
                               float baseFactor, bool compositeOverlay, uint32_t x, uint32_t y,
                               uint8_t *outRgba)
{
    const uint8_t *base = albedoImage.At(x, y, albedoImage.width, albedoImage.height);

    // gl_texmgr.c multiplies the albedo by base_factor and keeps the alpha only where the
    // texture is meant to carry one; a scene image's alpha is already what the map's
    // cutouts use, so it is passed through.
    outRgba[0] = uint8_t(std::min(255.0f, std::max(0.0f, base[0] * baseFactor)));
    outRgba[1] = uint8_t(std::min(255.0f, std::max(0.0f, base[1] * baseFactor)));
    outRgba[2] = uint8_t(std::min(255.0f, std::max(0.0f, base[2] * baseFactor)));
    outRgba[3] = base[3];

    // The game stamps the overlay on afterwards, weighted by its own alpha and rounded
    // to nearest: r = (r * (255 - a) + overlay * 255 + 127) / 255.
    if (compositeOverlay)
    {
        const uint8_t *stamp = overlayImage.At(x, y, albedoImage.width, albedoImage.height);
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

struct BakeResolvedInputs
{
    int32_t albedo = -1;
    int32_t gloss = -1;
    int32_t normal = -1;
    int32_t emissive = -1;
    int32_t overlay = -1;
    int32_t sssTex = -1;
    bool glass = false;
    bool passesLight = false;
    bool paintsOverlay = false;
    bool masksEmission = false;
    bool masksGloss = false;
    bool bakeSss = false;
    const EditorTexture *albedoTex = nullptr;
};

BakeResolvedInputs ResolveBakeInputs(const std::string &name, const EditorMaterialParams &params,
                                     const EditorScene &scene,
                                     const std::function<int32_t(const std::string &)> &resolveTexture)
{
    BakeResolvedInputs out;
    out.glass = params.type == EditorMaterialType::Glass;
    out.albedo = resolveTexture(params.baseTexture);
    if (out.albedo < 0)
    {
        out.albedo = resolveTexture(name);
    }

    out.gloss = resolveTexture(params.glossTexture);
    out.normal = resolveTexture(params.normalTexture);
    out.emissive = ActiveEmissiveIndex(params, resolveTexture);
    out.overlay = resolveTexture(params.overlayTexture);
    out.sssTex = resolveTexture(params.textureSss);

    out.passesLight = out.glass && params.transparency != 1.0f;
    out.albedoTex = TextureAt(scene, out.albedo);
    const SourceImage overlayImage = SourceOf(scene, out.overlay);
    out.paintsOverlay = overlayImage.Valid() && !out.glass;
    out.masksEmission = ShouldMaskEmission(params, out.emissive);
    out.masksGloss = ShouldMaskGloss(params, out.gloss);
    out.bakeSss = ShouldBakeSss(params, out.normal, out.sssTex, out.glass);
    return out;
}

bool NeedsBakeForInputs(const BakeResolvedInputs &in, const EditorMaterialParams &params)
{
    const BakeNeedFlags flags{ in.gloss,
                               in.normal >= 0,
                               in.emissive,
                               in.passesLight,
                               ShouldBlendEmission(params),
                               params.metalnessFromNormalAlpha,
                               in.masksEmission,
                               in.masksGloss,
                               in.paintsOverlay,
                               NeedsFullbrightBake(in.albedoTex, params),
                               in.bakeSss };
    return NeedsMaterialBake(flags);
}

struct BakePixelOutputs
{
    std::vector<uint8_t> albedoData;
    std::vector<uint8_t> rmeData;
    std::vector<uint8_t> normalData;
    BakeEmissionAccum emissAccum;
    bool emissionValid = false;
    bool bakeNormalSlot = false;
};

struct BakeScalarConfig
{
    bool enabled = true;
    bool sssEnabled = false;
    bool compositeOverlay = false;
    bool metalFromNormalAlpha = false;
    bool mirror = false;
    bool hasRoughnessOverride = false;
    bool masksEmission = false;
    bool masksGloss = false;
    bool passesLight = false;
    float roughness = kEditorDefaultRoughness;
    float metallic = 0.0f;
    float emission = 0.0f;
    float glassAlpha = 1.0f;
    uint8_t blendCode = 0;
};

BakePixelOutputs BuildBakePixelOutputs(const EditorMaterialParams &params, const EditorScene &scene,
                                       const BakeResolvedInputs &in, const SourceImage &albedoImage,
                                       const BakeScalarConfig &cfg)
{
    const SourceImage glossImage = SourceOf(scene, in.gloss);
    const SourceImage normalImage = SourceOf(scene, in.normal);
    const SourceImage emissiveImage = SourceOf(scene, in.emissive);
    const SourceImage overlayImage = SourceOf(scene, in.overlay);
    const SourceImage sssTexImage = SourceOf(scene, in.sssTex);

    const size_t pixels = size_t(albedoImage.width) * size_t(albedoImage.height);
    BakePixelOutputs out;
    out.albedoData.resize(pixels * 4);
    out.rmeData.resize(pixels * 4);
    out.normalData.resize(pixels * 4);
    out.emissAccum = BakeEmissionAccum{
        0.0, 0.0, 0.0, 0.0, 0.0, { int(albedoImage.width), int(albedoImage.height), -1, -1 }
    };

    std::vector<float> colorEmission;
    std::vector<uint8_t> colorBlend;
    if (cfg.masksEmission)
    {
        BuildColorEmissive(params, albedoImage, colorEmission, colorBlend);
    }
    ColorGlossBuffers colorGloss;
    BuildColorGlossIfNeeded(cfg.masksGloss, params, albedoImage, colorGloss);
    const std::vector<uint8_t> sssMaskBytes =
        BuildEditorSssMaskIfNeeded(cfg.sssEnabled, params, albedoImage, sssTexImage);
    const BakeRoughnessInputs roughnessInputs{ glossImage,
                                               normalImage,
                                               colorGloss,
                                               albedoImage.width,
                                               albedoImage.height,
                                               cfg.metalFromNormalAlpha && normalImage.Valid(),
                                               cfg.mirror,
                                               cfg.hasRoughnessOverride,
                                               cfg.roughness,
                                               cfg.metallic };
    const BakeEmissionInputs emissionInputs{
        emissiveImage, in.albedoTex, colorEmission, colorBlend, albedoImage.width,
        albedoImage.height, cfg.emission, cfg.blendCode,
    };

    for (uint32_t y = 0; y < albedoImage.height; y++)
    {
        for (uint32_t x = 0; x < albedoImage.width; x++)
        {
            const size_t pixelIndex = size_t(y) * albedoImage.width + x;
            const size_t offset = pixelIndex * 4;

            StampBakeAlbedoAndOverlay(albedoImage, overlayImage, params.baseFactor,
                                      cfg.compositeOverlay, x, y, out.albedoData.data() + offset);

            float roughness = cfg.roughness;
            float metallic = cfg.metallic;
            SampleBakeRoughnessMetallic(roughnessInputs, x, y, roughness, metallic);

            uint8_t pixelBlend = cfg.blendCode;
            const float emission = SampleBakedEmissionIfEnabled(cfg.enabled, emissionInputs, x, y,
                                                                pixelBlend);
            if (emission > 0.0f)
            {
                out.emissAccum.r += out.albedoData[offset + 0] * emission;
                out.emissAccum.g += out.albedoData[offset + 1] * emission;
                out.emissAccum.b += out.albedoData[offset + 2] * emission;
            }
            out.emissAccum.mean += std::min(1.0f, emission);
            out.emissAccum.meanBase += emission;
            AccumulateGlow(emission, x, y, out.emissAccum.glow);

            out.rmeData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, roughness * 255.0f)));
            out.rmeData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, metallic * 255.0f)));
            out.rmeData[offset + 2] = uint8_t(std::min(255.0f, std::max(0.0f, emission * 255.0f)));
            out.rmeData[offset + 3] = pixelBlend;

            SampleBakeNormal(normalImage, albedoImage, params.bumpScale, x, y,
                             out.normalData.data() + offset);
            out.normalData[offset + 3] =
                ResolveBakedNormalAlpha(sssMaskBytes, pixelIndex, cfg.glassAlpha);
        }
    }

    out.emissionValid = IsBakeEmissionValid(cfg.enabled, emissiveImage.Valid(),
                                            !colorEmission.empty(), HasFullbright(in.albedoTex));
    out.bakeNormalSlot = normalImage.Valid() || cfg.passesLight || !sssMaskBytes.empty();
    return out;
}

} // namespace

QrMaterial EditorMaterialBakes::Get(QrInstance instance, const std::string &name,
                                    const EditorMaterialParams &params, const EditorScene &scene,
                                    const std::vector<QrMaterial> &pool,
                                    const std::function<int32_t(const std::string &)> &resolveTexture)
{
    const BakeResolvedInputs in = ResolveBakeInputs(name, params, scene, resolveTexture);
    if (!NeedsBakeForInputs(in, params))
    {
        return QR_NO_MATERIAL;
    }

    const SourceImage albedoImage = SelectBakeFallbackImage(
        SourceOf(scene, in.albedo), SourceOf(scene, in.gloss), SourceOf(scene, in.normal),
        SourceOf(scene, in.emissive), SourceOf(scene, in.overlay), SourceOf(scene, in.sssTex));
    if (!albedoImage.Valid())
    {
        return QR_NO_MATERIAL;
    }

    const SourceImage overlayImage = SourceOf(scene, in.overlay);
    const bool compositeOverlay = in.paintsOverlay && overlayImage.rgba != albedoImage.rgba;
    const bool sssEnabled = HasEditorSubsurface(params) && !in.glass;
    const bool mirror = params.mirror && !in.glass && params.type != EditorMaterialType::Water;
    const bool hasRoughnessOverride = params.roughnessOverride >= 0.0f;
    const float roughness =
        mirror ? 0.0f : (hasRoughnessOverride ? params.roughnessOverride : kEditorDefaultRoughness);

    Key key;
    key.albedo = in.albedo;
    key.gloss = in.gloss;
    key.normal = in.normal;
    key.emissive = in.emissive;
    key.overlay = compositeOverlay ? in.overlay : -1;
    key.sssTex = sssEnabled ? in.sssTex : -1;
    key.width = albedoImage.width;
    key.height = albedoImage.height;
    key.bumpScale = params.bumpScale;
    key.transparency = in.glass ? params.transparency : 1.0f;
    key.lumaBlend = params.lumaBlend;
    key.metalnessFromNormalAlpha = params.metalnessFromNormalAlpha;
    key.enabled = params.enabled;
    key.sssEnabled = sssEnabled;
    key.emissiveMask = in.masksEmission ? EmissiveMaskOf(params.emissiveBlocks) : std::string();
    key.glossMask = in.masksGloss ? GlossMaskOf(params.glossBlocks) : std::string();
    key.sssMask = sssEnabled ? SssMaskOf(params.sssBlocks) : std::string();
    key.hasRoughnessOverride = hasRoughnessOverride;
    key.mirror = mirror;
    key.roughness = roughness;
    key.metallic = params.metalnessFactor;
    key.emission = BakeEmissionFactor(params);

    for (Entry &entry : m_entries)
    {
        if (entry.key == key)
        {
            entry.used = true;
            return entry.material;
        }
    }

    const BakeScalarConfig cfg{
        params.enabled,
        sssEnabled,
        compositeOverlay,
        params.metalnessFromNormalAlpha,
        mirror,
        hasRoughnessOverride,
        in.masksEmission,
        in.masksGloss,
        in.passesLight,
        roughness,
        params.metalnessFactor,
        key.emission,
        params.transparency,
        params.lumaBlend < 0 ? uint8_t(0) : uint8_t(params.lumaBlend + 1),
    };
    const BakePixelOutputs baked = BuildBakePixelOutputs(params, scene, in, albedoImage, cfg);

    QrMaterialCreateInfo info = {};
    info.size = { albedoImage.width, albedoImage.height };
    info.textures.pDataAlbedoAlpha = baked.albedoData.data();
    info.textures.pDataRoughnessMetallicEmission = baked.rmeData.data();
    info.textures.pDataNormal = baked.bakeNormalSlot ? baked.normalData.data() : nullptr;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    QrMaterial material = QR_NO_MATERIAL;
    if (instance == nullptr || qrCreateMaterial(instance, &info, &material) != QR_SUCCESS)
    {
        return QR_NO_MATERIAL;
    }

    const size_t pixels = size_t(albedoImage.width) * size_t(albedoImage.height);
    const EditorMaterialEmission emissionInfo = BuildBakeEmissionSummary(
        baked.emissionValid, pixels, albedoImage.width, albedoImage.height, baked.emissAccum);
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
