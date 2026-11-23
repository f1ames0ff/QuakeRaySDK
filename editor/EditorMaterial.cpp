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

void FeatherMaskHorizontal(const std::vector<float> &mask, uint32_t width, uint32_t height,
                           int radius, std::vector<float> &tmp)
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
            sum += mask[row + size_t(std::clamp(i, 0, w - 1))];
        }

        for (int x = 0; x < w; x++)
        {
            tmp[row + size_t(x)] = sum / window;
            sum += mask[row + size_t(std::min(x + radius + 1, w - 1))];
            sum -= mask[row + size_t(std::max(x - radius, 0))];
        }
    }
}

void FeatherMaskVertical(const std::vector<float> &tmp, uint32_t width, uint32_t height, int radius,
                         std::vector<float> &mask)
{
    const int w = int(width);
    const int h = int(height);
    const float window = float(radius * 2 + 1);

    for (int x = 0; x < w; x++)
    {
        float sum = 0.0f;

        for (int i = -radius; i <= radius; i++)
        {
            sum += tmp[size_t(std::clamp(i, 0, h - 1)) * width + size_t(x)];
        }

        for (int y = 0; y < h; y++)
        {
            const float blurred = sum / window;
            const size_t index = size_t(y) * width + size_t(x);

            if (blurred > mask[index])
            {
                mask[index] = blurred;
            }

            sum += tmp[size_t(std::min(y + radius + 1, h - 1)) * width + size_t(x)];
            sum -= tmp[size_t(std::max(y - radius, 0)) * width + size_t(x)];
        }
    }
}

// Softens a scalar mask: a box blur of the given radius, so the mask's edge fades outward from
// what the threshold selected while those pixels keep their value. Two separable passes over a
// running sum, matching TexMgr_FeatherEmissive, including its clamped edges and its rule that the
// blur may only raise a value.
void FeatherMask(std::vector<float> &mask, uint32_t width, uint32_t height, int radius)
{
    if (radius <= 0 || width == 0 || height == 0)
    {
        return;
    }

    std::vector<float> tmp(mask.size(), 0.0f);
    FeatherMaskHorizontal(mask, width, height, radius, tmp);
    FeatherMaskVertical(tmp, width, height, radius, mask);
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

struct ColorMaskSpec
{
    const float *color = nullptr;
    float threshold = 0.02f;
    float feather = 0.0f;
    float scale = 1.0f;
    const std::vector<float> *polyUv = nullptr;
    bool flatWithinThreshold = false;
};

void ComputeColorMask(const ColorMaskSpec &spec, const SourceImage &albedo,
                      std::vector<float> &mask, std::vector<float> &dist)
{
    const size_t pixels = size_t(albedo.width) * size_t(albedo.height);
    const bool polygon = spec.polyUv != nullptr && spec.polyUv->size() / 2 >= 3;
    const float threshold = spec.threshold;

    mask.assign(pixels, 0.0f);
    dist.assign(pixels, 0.0f);

    for (uint32_t y = 0; y < albedo.height; y++)
    {
        for (uint32_t x = 0; x < albedo.width; x++)
        {
            const size_t index = size_t(y) * albedo.width + x;
            const uint8_t *src = albedo.rgba + index * 4;
            const float dr = src[0] / 255.0f - spec.color[0];
            const float dg = src[1] / 255.0f - spec.color[1];
            const float db = src[2] / 255.0f - spec.color[2];
            const float d2 = dr * dr + dg * dg + db * db;
            dist[index] = d2;

            if (polygon)
            {
                const float u = (float(x) + 0.5f) / float(albedo.width);
                const float v = (float(y) + 0.5f) / float(albedo.height);

                if (InsidePolygon(*spec.polyUv, u, v))
                {
                    mask[index] = spec.scale;
                    dist[index] = 0.0f;
                }
            }
            else if (d2 <= 3.0f * threshold * threshold)
            {
                if (spec.flatWithinThreshold)
                {
                    mask[index] = spec.scale;
                }
                else
                {
                    const float dnorm =
                        threshold > 0.0f ? std::sqrt(d2 / 3.0f) / threshold : 0.0f;
                    const float tail = std::exp(-kColorEmissiveFalloff);
                    const float value =
                        (std::exp(-kColorEmissiveFalloff * dnorm) - tail) / (1.0f - tail);
                    mask[index] = value * spec.scale;
                }
            }
        }
    }

    const int feather = spec.feather >= 1.0f ? int(std::lround(spec.feather)) : 0;
    FeatherMask(mask, albedo.width, albedo.height, feather);
}

void AppendSerializedMaskGeometry(const std::vector<float> &polyUv, const float color[3],
                                  std::string &text)
{
    char buffer[64];
    const size_t points = polyUv.size() / 2;

    if (points >= 3)
    {
        text += "poly";
        for (size_t p = 0; p < points * 2; p++)
        {
            std::snprintf(buffer, sizeof(buffer), ",%.6g", polyUv[p]);
            text += buffer;
        }
        return;
    }

    std::snprintf(buffer, sizeof(buffer), "color %02x%02x%02x",
                  int(std::lround(std::clamp(color[0], 0.0f, 1.0f) * 255.0f)) & 0xff,
                  int(std::lround(std::clamp(color[1], 0.0f, 1.0f) * 255.0f)) & 0xff,
                  int(std::lround(std::clamp(color[2], 0.0f, 1.0f) * 255.0f)) & 0xff);
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
        const ColorMaskSpec spec{ block.color, block.threshold, block.feather, block.factor,
                                  &block.polyUv };
        ComputeColorMask(spec, albedo, mask, dist);

        for (size_t index = 0; index < pixels; index++)
        {
            if (!(mask[index] > 0.0f))
            {
                continue;
            }

            // A pixel with a nearer block already keeps that one, and the nearest match wins
            // even when it selected the pixel with a smaller mask.
            if (emission[index] > 0.0f && !(dist[index] < distance[index]))
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
    std::vector<float> distance(pixels, 0.0f);
    out.weight.assign(pixels, 0.0f);
    out.roughness.assign(pixels, 0.0f);
    out.metalness.assign(pixels, 0.0f);

    for (const EditorGlossBlock &block : params.glossBlocks)
    {
        std::vector<float> mask;
        std::vector<float> dist;
        const ColorMaskSpec spec{ block.color, block.threshold, block.feather, 1.0f, &block.polyUv,
                                  true };
        ComputeColorMask(spec, albedo, mask, dist);

        const float blkRough = std::clamp(block.roughness, 0.0f, 1.0f);
        const float blkMetal = std::clamp(block.metalness, 0.0f, 1.0f);

        for (size_t index = 0; index < pixels; index++)
        {
            if (!(mask[index] > 0.0f))
            {
                continue;
            }

            if (out.weight[index] > 0.0f && !(dist[index] < distance[index]))
            {
                continue;
            }

            out.weight[index] = std::min(1.0f, mask[index]);
            out.roughness[index] = blkRough;
            out.metalness[index] = blkMetal;
            distance[index] = dist[index];
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
        AppendSerializedMaskGeometry(block.polyUv, block.color, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g x%.6g b%d;", block.threshold,
                      block.feather, block.factor, block.blend);
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
        AppendSerializedMaskGeometry(block.polyUv, block.color, text);
        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g r%.6g m%.6g;", block.threshold,
                      block.feather, block.roughness, block.metalness);
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

} // namespace

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
};

bool NeedsMaterialBake(const BakeNeedFlags &f)
{
    return f.gloss >= 0 || f.wantsNormal || f.emissive >= 0 || f.passesLight || f.blendsEmission ||
           f.metalFromNormalAlpha || f.masksEmission || f.masksGloss || f.paintsOverlay ||
           f.fullbright;
}

SourceImage SelectBakeFallbackImage(const SourceImage &albedo, const SourceImage &gloss,
                                    const SourceImage &normal, const SourceImage &emissive,
                                    const SourceImage &overlay)
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
    return emissive.Valid() ? emissive : (overlay.Valid() ? overlay : SourceImage{});
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

} // namespace

QrMaterial EditorMaterialBakes::Get(QrInstance instance, const std::string &name,
                                    const EditorMaterialParams &params, const EditorScene &scene,
                                    const std::vector<QrMaterial> &pool,
                                    const std::function<int32_t(const std::string &)> &resolveTexture)
{
    const bool glass = params.type == EditorMaterialType::Glass;

    // The albedo is what the surface shows: the named base image when it resolves, otherwise the
    // map's own. Both are scene textures, so a bake can read either one.
    int32_t albedo = resolveTexture(params.baseTexture);

    if (albedo < 0)
    {
        albedo = resolveTexture(name);
    }

    const int32_t gloss = resolveTexture(params.glossTexture);
    const int32_t normal = resolveTexture(params.normalTexture);
    const int32_t emissive = ActiveEmissiveIndex(params, resolveTexture);
    const int32_t overlay = resolveTexture(params.overlayTexture);

    const bool wantsNormal = normal >= 0;
    const bool passesLight = glass && params.transparency != 1.0f;
    const bool blendsEmission = ShouldBlendEmission(params);
    const EditorTexture *albedoTex = TextureAt(scene, albedo);

    // gl_texmgr.c composites the overlay into the albedo of a surface that is not glass and
    // hands a glass pane a separate auxiliary image the traced path never reads. A name that
    // does not resolve is the same as no overlay at all.
    const SourceImage overlayImage = SourceOf(scene, overlay);
    const bool paintsOverlay = overlayImage.Valid() && !glass;

    // The game gives a named emissive image precedence over the colour blocks, so a material
    // that resolves one is baked from that image and its blocks stay dormant.
    const bool masksEmission = ShouldMaskEmission(params, emissive);
    const bool masksGloss = ShouldMaskGloss(params, gloss);

    // A material with none of these keys keeps the pool material the scene uploaded, which is
    // every surface until someone authors one.
    const BakeNeedFlags needFlags{ gloss,
                                   wantsNormal,
                                   emissive,
                                   passesLight,
                                   blendsEmission,
                                   params.metalnessFromNormalAlpha,
                                   masksEmission,
                                   masksGloss,
                                   paintsOverlay,
                                   NeedsFullbrightBake(albedoTex, params) };
    if (!NeedsMaterialBake(needFlags))
    {
        return QR_NO_MATERIAL;
    }

    const SourceImage rawAlbedo = SourceOf(scene, albedo);
    const SourceImage glossImage = SourceOf(scene, gloss);
    const SourceImage normalImage = SourceOf(scene, normal);
    const SourceImage emissiveImage = SourceOf(scene, emissive);

    // The set is the size of the image the surface shows; a bake authored on a surface without
    // one still needs a size, which the extra images can supply.
    const SourceImage albedoImage =
        SelectBakeFallbackImage(rawAlbedo, glossImage, normalImage, emissiveImage, overlayImage);

    if (!albedoImage.Valid())
    {
        return QR_NO_MATERIAL;
    }

    // An overlay that supplied the set's size would only be stamped over itself.
    const bool compositeOverlay = paintsOverlay && overlayImage.rgba != albedoImage.rgba;

    Key key;
    key.albedo = albedo;
    key.gloss = gloss;
    key.normal = normal;
    key.emissive = emissive;
    key.overlay = compositeOverlay ? overlay : -1;
    key.width = albedoImage.width;
    key.height = albedoImage.height;
    key.bumpScale = params.bumpScale;
    key.transparency = glass ? params.transparency : 1.0f;
    key.lumaBlend = params.lumaBlend;
    key.metalnessFromNormalAlpha = params.metalnessFromNormalAlpha;
    key.enabled = params.enabled;
    key.emissiveMask = masksEmission ? EmissiveMaskOf(params.emissiveBlocks) : std::string();
    key.glossMask = masksGloss ? GlossMaskOf(params.glossBlocks) : std::string();

    // The scalars are only overridden by the baked set where it exists, so the bake has to
    // repeat what the upload info would otherwise have carried.
    const bool mirror = params.mirror && !glass && params.type != EditorMaterialType::Water;
    const bool hasRoughnessOverride = params.roughnessOverride >= 0.0f;
    key.hasRoughnessOverride = hasRoughnessOverride;
    key.mirror = mirror;
    key.roughness = mirror ? 0.0f
                  : hasRoughnessOverride ? params.roughnessOverride
                                         : kEditorDefaultRoughness;
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

    const size_t pixels = size_t(albedoImage.width) * size_t(albedoImage.height);

    std::vector<uint8_t> albedoData(pixels * 4);
    std::vector<uint8_t> rmeData(pixels * 4);
    std::vector<uint8_t> normalData(pixels * 4);

    const float baseFactor = params.baseFactor;
    const float bumpScale = params.bumpScale;
    const float glassAlpha = params.transparency;
    const uint8_t blendCode = params.lumaBlend < 0 ? 0 : uint8_t(params.lumaBlend + 1);
    const bool metalFromNormalAlpha = params.metalnessFromNormalAlpha && normalImage.Valid();

    // The emission the mask carries, summed the way gl_texmgr.c sums it: the albedo weighted
    // colour is what a masked light's own colour is when the material names no light_color, and
    // the two means are its meanEmiss and the base the colour is normalised by.
    BakeEmissionAccum emissAccum{ 0.0, 0.0, 0.0, 0.0, 0.0,
                                  { int(albedoImage.width), int(albedoImage.height), -1, -1 } };

    // The blocks select out of the albedo, so their masks are the size of the baked set.
    std::vector<float> colorEmission;
    std::vector<uint8_t> colorBlend;

    if (masksEmission)
    {
        BuildColorEmissive(params, albedoImage, colorEmission, colorBlend);
    }
    ColorGlossBuffers colorGloss;
    BuildColorGlossIfNeeded(masksGloss, params, albedoImage, colorGloss);
    const BakeRoughnessInputs roughnessInputs{ glossImage,           normalImage,
                                               colorGloss,           albedoImage.width,
                                               albedoImage.height,   metalFromNormalAlpha,
                                               mirror,               hasRoughnessOverride,
                                               key.roughness,        key.metallic };

    for (uint32_t y = 0; y < albedoImage.height; y++)
    {
        for (uint32_t x = 0; x < albedoImage.width; x++)
        {
            const size_t offset = (size_t(y) * albedoImage.width + x) * 4;

            StampBakeAlbedoAndOverlay(albedoImage, overlayImage, baseFactor, compositeOverlay, x, y,
                                      albedoData.data() + offset);

            float roughness = key.roughness;
            float metallic = key.metallic;
            SampleBakeRoughnessMetallic(roughnessInputs, x, y, roughness, metallic);

            uint8_t pixelBlend = blendCode;
            const BakeEmissionInputs emissionInputs{
                emissiveImage, albedoTex, colorEmission, colorBlend, albedoImage.width,
                albedoImage.height, key.emission, blendCode,
            };
            const float emission = SampleBakedEmissionIfEnabled(key.enabled, emissionInputs, x, y,
                                                                pixelBlend);

            if (emission > 0.0f)
            {
                emissAccum.r += albedoData[offset + 0] * emission;
                emissAccum.g += albedoData[offset + 1] * emission;
                emissAccum.b += albedoData[offset + 2] * emission;
            }

            emissAccum.mean += std::min(1.0f, emission);
            emissAccum.meanBase += emission;
            AccumulateGlow(emission, x, y, emissAccum.glow);

            rmeData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, roughness * 255.0f)));
            rmeData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, metallic * 255.0f)));
            rmeData[offset + 2] = uint8_t(std::min(255.0f, std::max(0.0f, emission * 255.0f)));
            rmeData[offset + 3] = pixelBlend;

            SampleBakeNormal(normalImage, albedoImage, bumpScale, x, y, normalData.data() + offset);
            // The traced path takes a pane's transparency out of the alpha of the normal slot.
            normalData[offset + 3] = uint8_t(std::min(255.0f, std::max(0.0f, glassAlpha * 255.0f)));
        }
    }

    const bool bakeNormalSlot = normalImage.Valid() || passesLight;

    QrMaterialCreateInfo info = {};
    info.size = { albedoImage.width, albedoImage.height };
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

    const bool emissionValid = IsBakeEmissionValid(key.enabled, emissiveImage.Valid(),
                                                   !colorEmission.empty(), HasFullbright(albedoTex));
    const EditorMaterialEmission emissionInfo = BuildBakeEmissionSummary(
        emissionValid, pixels, albedoImage.width, albedoImage.height, emissAccum);

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
