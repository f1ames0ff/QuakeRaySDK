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

// The falloff of a colour block's threshold mask, gl_texmgr.c RT_COLOR_EMISSIVE_FALLOFF.
constexpr float kColorEmissiveFalloff = 2.0f;

// Softens an emission map: a box blur of the given radius, so the mask's edge fades outward from
// what the threshold selected while those pixels keep their value. Two separable passes over a
// running sum, matching TexMgr_FeatherEmissive, including its clamped edges and its rule that the
// blur may only raise a value.
void FeatherEmissive(std::vector<float> &mask, uint32_t width, uint32_t height, int radius)
{
    if (radius <= 0 || width == 0 || height == 0)
    {
        return;
    }

    const int w = int(width);
    const int h = int(height);
    const float window = float(radius * 2 + 1);
    std::vector<float> tmp(mask.size(), 0.0f);

    for (int y = 0; y < h; y++)
    {
        const size_t row = size_t(y) * width;
        float sum = 0.0f;

        for (int i = -radius; i <= radius; i++)
        {
            const int sampleX = i < 0 ? 0 : (i >= w ? w - 1 : i);
            sum += mask[row + size_t(sampleX)];
        }

        for (int x = 0; x < w; x++)
        {
            tmp[row + size_t(x)] = sum / window;

            const int add = x + radius + 1;
            const int sub = x - radius;
            sum += mask[row + size_t(add >= w ? w - 1 : add)];
            sum -= mask[row + size_t(sub < 0 ? 0 : sub)];
        }
    }

    for (int x = 0; x < w; x++)
    {
        float sum = 0.0f;

        for (int i = -radius; i <= radius; i++)
        {
            const int sampleY = i < 0 ? 0 : (i >= h ? h - 1 : i);
            sum += tmp[size_t(sampleY) * width + size_t(x)];
        }

        for (int y = 0; y < h; y++)
        {
            const float blurred = sum / window;
            const size_t index = size_t(y) * width + size_t(x);

            if (blurred > mask[index])
            {
                mask[index] = blurred;
            }

            const int add = y + radius + 1;
            const int sub = y - radius;
            sum += tmp[size_t(add >= h ? h - 1 : add) * width + size_t(x)];
            sum -= tmp[size_t(sub < 0 ? 0 : sub) * width + size_t(x)];
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
        const float threshold = block.threshold;
        const int feather =
            block.feather >= 1.0f ? int(block.feather + 0.5f) : 0;
        const uint8_t blockCode =
            block.blend < 0 ? 0 : uint8_t(std::min<int32_t>(block.blend, 5) + 1);

        const bool polygon = block.polyUv.size() / 2 >= 3;

        std::vector<float> mask(pixels, 0.0f);
        std::vector<float> dist(pixels, 0.0f);

        for (uint32_t y = 0; y < albedo.height; y++)
        {
            for (uint32_t x = 0; x < albedo.width; x++)
            {
                const size_t index = size_t(y) * albedo.width + x;
                const uint8_t *src = albedo.rgba + index * 4;
                const float dr = src[0] / 255.0f - block.color[0];
                const float dg = src[1] / 255.0f - block.color[1];
                const float db = src[2] / 255.0f - block.color[2];
                const float d2 = dr * dr + dg * dg + db * db;

                dist[index] = d2;

                if (polygon)
                {
                    const float u = (float(x) + 0.5f) / float(albedo.width);
                    const float v = (float(y) + 0.5f) / float(albedo.height);

                    if (InsidePolygon(block.polyUv, u, v))
                    {
                        mask[index] = block.factor;
                        dist[index] = 0.0f;
                    }
                }
                else if (d2 <= 3.0f * threshold * threshold)
                {
                    const float dnorm =
                        threshold > 0.0f ? std::sqrt(d2 / 3.0f) / threshold : 0.0f;
                    const float tail = std::exp(-kColorEmissiveFalloff);
                    const float value =
                        (std::exp(-kColorEmissiveFalloff * dnorm) - tail) / (1.0f - tail);

                    mask[index] = value * block.factor;
                }
            }
        }

        FeatherEmissive(mask, albedo.width, albedo.height, feather);

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

// The blocks as one comparable string, so two materials that differ anywhere in their colour
// blocks are different bakes.
std::string EmissiveMaskOf(const std::vector<EditorEmissiveBlock> &blocks)
{
    std::string text;
    char buffer[160];

    for (const EditorEmissiveBlock &block : blocks)
    {
        const size_t points = block.polyUv.size() / 2;

        if (points >= 3)
        {
            text += "poly";
            for (size_t p = 0; p < points * 2; p++)
            {
                std::snprintf(buffer, sizeof(buffer), ",%.6g", block.polyUv[p]);
                text += buffer;
            }
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "color %02x%02x%02x",
                          int(std::min(1.0f, std::max(0.0f, block.color[0])) * 255.0f + 0.5f) & 0xff,
                          int(std::min(1.0f, std::max(0.0f, block.color[1])) * 255.0f + 0.5f) & 0xff,
                          int(std::min(1.0f, std::max(0.0f, block.color[2])) * 255.0f + 0.5f) & 0xff);
            text += buffer;
        }

        std::snprintf(buffer, sizeof(buffer), " t%.6g f%.6g x%.6g b%d;", block.threshold,
                      block.feather, block.factor, block.blend);
        text += buffer;
    }

    return text;
}

} // namespace

void ApplyEditorMaterial(const EditorMaterialParams &material,
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
    const bool waterFamily = passThrough == QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT ||
                             passThrough == QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT;
    const bool mirror = material.mirror && !glass && !water && !waterFamily;

    if (mirror)
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
    upload.defaultEmission = material.emissiveFactor;

    const float base = material.baseFactor;
    upload.layerColors[0] = { base, base, base, 1.0f };

    if (glass)
    {
        upload.layerColors[1] = { material.glassColor[0], material.glassColor[1],
                                  material.glassColor[2], 0.0f };
        upload.layerColors[2] = { material.glassIor, material.glassThickness, 0.0f, 0.0f };
    }

    upload.texCoordScale[0] = material.textureScaleU;
    upload.texCoordScale[1] = material.textureScaleV;
    upload.texCoordOffset[0] = material.textureOffsetU;
    upload.texCoordOffset[1] = material.textureOffsetV;

    const bool identityTransform = material.textureScaleU == 1.0f &&
                                   material.textureScaleV == 1.0f &&
                                   material.textureOffsetU == 0.0f &&
                                   material.textureOffsetV == 0.0f;

    if (!identityTransform)
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT;
    }

    if (material.exactNormals)
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT;
    }

    if (material.luma)
    {
        upload.flags |= QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT;
    }
}

void EditorMaterialTable::Rebuild(const EditorScene &scene)
{
    m_entries.clear();
    m_indexByName.clear();

    std::vector<std::string> names;

    for (const EditorSurface &surface : scene.surfaces)
    {
        if (!surface.textureName.empty())
        {
            names.push_back(surface.textureName);
        }
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    m_entries.reserve(names.size());

    for (const std::string &name : names)
    {
        m_indexByName[name] = m_entries.size();
        m_entries.push_back(Entry{ name, EditorMaterialParams{} });
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

bool EditorMaterialBakes::Key::operator==(const Key &other) const
{
    return albedo == other.albedo && gloss == other.gloss && normal == other.normal &&
           emissive == other.emissive && width == other.width && height == other.height &&
           bumpScale == other.bumpScale && transparency == other.transparency &&
           roughness == other.roughness && metallic == other.metallic &&
           emission == other.emission && lumaBlend == other.lumaBlend &&
           metalnessFromNormalAlpha == other.metalnessFromNormalAlpha &&
           emissiveMask == other.emissiveMask;
}

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
    const int32_t emissive = resolveTexture(params.emissiveTexture);

    const bool wantsNormal = normal >= 0 && params.bumpScale != 1.0f;
    const bool passesLight = glass && params.transparency != 1.0f;
    const bool blendsEmission = params.lumaBlend >= 0;

    // The game gives a named emissive image precedence over the colour blocks, so a material
    // that resolves one is baked from that image and its blocks stay dormant.
    const bool masksEmission = !params.emissiveBlocks.empty() && emissive < 0;

    // A material with none of these keys keeps the pool material the scene uploaded, which is
    // every surface until someone authors one.
    if (gloss < 0 && !wantsNormal && emissive < 0 && !passesLight && !blendsEmission &&
        !params.metalnessFromNormalAlpha && !masksEmission)
    {
        return QR_NO_MATERIAL;
    }

    SourceImage albedoImage = SourceOf(scene, albedo);
    const SourceImage glossImage = SourceOf(scene, gloss);
    const SourceImage normalImage = SourceOf(scene, normal);
    const SourceImage emissiveImage = SourceOf(scene, emissive);

    // The set is the size of the image the surface shows; a bake authored on a surface without
    // one still needs a size, which the extra images can supply.
    if (!albedoImage.Valid())
    {
        albedoImage = glossImage.Valid() ? glossImage
                      : normalImage.Valid() ? normalImage
                      : emissiveImage.Valid() ? emissiveImage
                                              : SourceImage{};
    }

    if (!albedoImage.Valid())
    {
        return QR_NO_MATERIAL;
    }

    Key key;
    key.albedo = albedo;
    key.gloss = gloss;
    key.normal = normal;
    key.emissive = emissive;
    key.width = albedoImage.width;
    key.height = albedoImage.height;
    key.bumpScale = params.bumpScale;
    key.transparency = glass ? params.transparency : 1.0f;
    key.lumaBlend = params.lumaBlend;
    key.metalnessFromNormalAlpha = params.metalnessFromNormalAlpha;
    key.emissiveMask = masksEmission ? EmissiveMaskOf(params.emissiveBlocks) : std::string();

    // The scalars are only overridden by the baked set where it exists, so the bake has to
    // repeat what the upload info would otherwise have carried.
    const bool mirror = params.mirror && !glass && params.type != EditorMaterialType::Water;
    key.roughness = mirror ? 0.0f
                  : params.roughnessOverride >= 0.0f ? params.roughnessOverride
                                                     : kEditorDefaultRoughness;
    key.metallic = params.metalnessFactor;
    key.emission = params.emissiveFactor;

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

    // The blocks select out of the albedo, so their masks are the size of the baked set.
    std::vector<float> colorEmission;
    std::vector<uint8_t> colorBlend;

    if (masksEmission)
    {
        BuildColorEmissive(params, albedoImage, colorEmission, colorBlend);
    }

    for (uint32_t y = 0; y < albedoImage.height; y++)
    {
        for (uint32_t x = 0; x < albedoImage.width; x++)
        {
            const size_t offset = (size_t(y) * albedoImage.width + x) * 4;

            const uint8_t *base = albedoImage.At(x, y, albedoImage.width, albedoImage.height);

            // gl_texmgr.c multiplies the albedo by base_factor and keeps the alpha only where the
            // texture is meant to carry one; a scene image's alpha is already what the map's
            // cutouts use, so it is passed through.
            albedoData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, base[0] * baseFactor)));
            albedoData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, base[1] * baseFactor)));
            albedoData[offset + 2] = uint8_t(std::min(255.0f, std::max(0.0f, base[2] * baseFactor)));
            albedoData[offset + 3] = base[3];

            float roughness = key.roughness;

            if (glossImage.Valid())
            {
                const uint8_t *gloss = glossImage.At(x, y, albedoImage.width, albedoImage.height);
                roughness = 1.0f - gloss[0] / 255.0f;
            }

            float metallic = key.metallic;

            if (metalFromNormalAlpha)
            {
                const uint8_t *normalPixel =
                    normalImage.At(x, y, albedoImage.width, albedoImage.height);
                metallic = (normalPixel[3] / 255.0f) * key.metallic;
            }

            // The mask's brightness is its luminance scaled by emissive_factor, the same way the
            // game turns texture_emissive into the blue channel of the packed set. A colour block
            // carries its own factor and its own blend, and the game lets it replace the image
            // entirely rather than combine with one.
            float emission = 0.0f;
            uint8_t pixelBlend = blendCode;

            if (emissiveImage.Valid())
            {
                const uint8_t *mask = emissiveImage.At(x, y, albedoImage.width, albedoImage.height);
                const float luminance =
                    (0.2126f * mask[0] + 0.7152f * mask[1] + 0.0722f * mask[2]) / 255.0f;
                emission = luminance * key.emission;
            }
            else if (!colorEmission.empty())
            {
                const size_t pixel = offset / 4;
                emission = colorEmission[pixel];
                pixelBlend = colorBlend[pixel];
            }
            else
            {
                emission = key.emission;
            }

            rmeData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, roughness * 255.0f)));
            rmeData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, metallic * 255.0f)));
            rmeData[offset + 2] = uint8_t(std::min(255.0f, std::max(0.0f, emission * 255.0f)));
            rmeData[offset + 3] = pixelBlend;

            if (normalImage.Valid())
            {
                const uint8_t *normalPixel =
                    normalImage.At(x, y, albedoImage.width, albedoImage.height);
                const float nx = (normalPixel[0] - 128.0f) * bumpScale + 128.0f;
                const float ny = (normalPixel[1] - 128.0f) * bumpScale + 128.0f;

                normalData[offset + 0] = uint8_t(std::min(255.0f, std::max(0.0f, nx)));
                normalData[offset + 1] = uint8_t(std::min(255.0f, std::max(0.0f, ny)));
                normalData[offset + 2] = normalPixel[2];
            }
            else
            {
                normalData[offset + 0] = kFlatNormal[0];
                normalData[offset + 1] = kFlatNormal[1];
                normalData[offset + 2] = kFlatNormal[2];
            }

            // The traced path takes a pane's transparency out of the alpha of the normal slot.
            normalData[offset + 3] = uint8_t(std::min(255.0f, std::max(0.0f, glassAlpha * 255.0f)));
        }
    }

    QrMaterialCreateInfo info = {};
    info.size = { albedoImage.width, albedoImage.height };
    info.textures.pDataAlbedoAlpha = albedoData.data();
    info.textures.pDataRoughnessMetallicEmission = rmeData.data();
    info.textures.pDataNormal = normalData.data();
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    QrMaterial material = QR_NO_MATERIAL;

    if (instance == nullptr || qrCreateMaterial(instance, &info, &material) != QR_SUCCESS)
    {
        return QR_NO_MATERIAL;
    }

    m_entries.push_back(Entry{ key, material, true });
    return material;
}
