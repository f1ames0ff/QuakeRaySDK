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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <qray/qray.h>

struct EditorScene;
struct EditorSurface;

// The `material` combo of the game editor (Quake/qr_editor.c qre_params): none keeps the
// pass-through the BSP loader derived from the texture name, glass and water force theirs.
enum class EditorMaterialType
{
    None,
    Glass,
    Water,
};

// One colour-emissive block, the game's rt_emissive_t: a colour and the tone controls that work
// for it alone. A block whose own keys are absent inherits the material-level ones, so what the
// editor holds is always resolved.
struct EditorEmissiveBlock
{
    float color[3] = { 1.0f, 1.0f, 1.0f };

    // How far a pixel's colour may differ and still glow, and the pixels of edge softening
    // applied to the mask afterwards, 0 leaving it hard.
    float threshold = 0.02f;
    float feather = 0.0f;

    // Scales this block's emission. The game multiplies the mask by it and by nothing else.
    float factor = 1.0f;

    // How the glow is composited: -1 follows the material's, 0..5 pin a mode.
    int32_t blend = -1;

    // Non-empty selects by uv instead of by colour -- pairs of u,v, at least three of them.
    // The colour and threshold are then unused, which is what the game's polygon form does.
    std::vector<float> polyUv;

    // Non-empty with non-zero dimensions selects by a painted grayscale texel mask [0..255].
    uint32_t brushWidth = 0;
    uint32_t brushHeight = 0;
    std::vector<uint8_t> brushMask;

    bool operator==(const EditorEmissiveBlock &) const = default;
};

// RT_MAT_MAX_EMISSIVE_COLORS: the game's loader stops reading blocks past this, so the editor
// stops adding them here rather than write entries the game would drop.
inline constexpr size_t kEditorEmissiveBlockMax = 10;

// One procedural gloss block: selects pixels by color cube distance or UV polygon mask and
// assigns smoothed roughness and metalness when no external texture_gloss is bound.
struct EditorGlossBlock
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float threshold = 0.02f;
    float feather = 0.0f;
    float roughness = 0.25f;
    float metalness = 0.0f;
    std::vector<float> polyUv;
    uint32_t brushWidth = 0;
    uint32_t brushHeight = 0;
    std::vector<uint8_t> brushMask;

    bool operator==(const EditorGlossBlock &) const = default;
};

inline constexpr size_t kEditorGlossBlockMax = 10;
inline constexpr int32_t kEditorMaskFeatherMax = 128;

template <typename Block>
bool IsBrushBlock(const Block &block)
{
    return block.brushWidth > 0 && block.brushHeight > 0 &&
           block.brushMask.size() == size_t(block.brushWidth) * size_t(block.brushHeight);
}

template <typename Block>
bool IsPolygonBlock(const Block &block)
{
    return !IsBrushBlock(block) && block.polyUv.size() >= 6;
}

struct EditorMaskEvalSpec
{
    const float *color = nullptr;
    float threshold = 0.02f;
    float feather = 0.0f;
    float scale = 1.0f;
    const std::vector<float> *polyUv = nullptr;
    const std::vector<uint8_t> *brushMask = nullptr;
    uint32_t brushWidth = 0;
    uint32_t brushHeight = 0;
    bool isGloss = false;
};

void ComputeEditorMask(const EditorMaskEvalSpec &spec, const uint8_t *rgba, uint32_t width,
                       uint32_t height, std::vector<float> &outMask,
                       std::vector<float> *outDist = nullptr);

struct EditorBrushStrokeSpec
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float radiusTexels = 8.0f;
    float hardness = 0.5f;
    float opacity = 1.0f;
    bool erase = false;
};

bool ApplyEditorBrushSegment(const EditorBrushStrokeSpec &stroke, uint32_t width,
                             uint32_t height, const std::vector<uint8_t> &baseMask,
                             std::vector<float> &strokeAccum,
                             std::vector<uint8_t> &outMask);

// Colour-key, polygon, or painted brush region driving per-pixel subsurface scattering mask in Normal.a.
struct EditorSssBlock
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float threshold = 0.02f;
    float feather = 0.0f;
    float factor = 1.0f;
    std::vector<float> polyUv;
    uint32_t brushWidth = 0;
    uint32_t brushHeight = 0;
    std::vector<uint8_t> brushMask;

    bool operator==(const EditorSssBlock &) const = default;
};

inline constexpr size_t kEditorSssBlockMax = 10;

// The subset of the game's rt_material_t the editor expresses. The keys whose only consumer is
// a packed material texture -- the extra images, bump_scale, transparency, luma_blend -- are
// baked into the albedo / normal / roughness-metallic-emission set by EditorMaterialBakes, the
// way the game's texture manager synthesizes a material texture at load. force_rasterize has no
// slot in the SDK upload at all: its consumers are alias models and sprites, not the surfaces
// this editor uploads, so it is carried as data and changes nothing; the light_* group is
// carried the same way, because that is what "Regenerate from map" writes.
struct EditorMaterialParams
{
    // The game's texture_base: the image the surface shows instead of its own. An embedder that
    // can decode files resolves it as a path; the editor reads its images out of the scene, so
    // the value names one of those textures and a name the scene does not carry changes nothing.
    // Empty is the game's NONE: the map's own image.
    std::string baseTexture;

    // The game's texture_normals / texture_emissive / texture_gloss, resolved the same way as
    // baseTexture. Empty leaves the packed slot to what the surface already carries: no gloss
    // image means the roughness slider decides, no emissive image means emissiveFactor does.
    std::string normalTexture;
    std::string heightTexture;
    std::string emissiveTexture;
    std::string glossTexture;

    // The game's texture_overlay: an image stamped over the albedo by its own alpha, which is
    // how the map paints decals and grime onto a shared texture. gl_texmgr.c composites it into
    // the albedo of every non-glass surface and into a separate auxiliary texture for glass; the
    // traced path reads only the albedo, so a glass pane here keeps its base image and the
    // overlay is a border rather than a lie about what the picture shows.
    std::string overlayTexture;

    // Textures. Quake's texture_scale_* / texture_offset_* feed the layer-0 texcoord
    // transform; the SDK applies them under QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT.
    float textureScaleU = 1.0f;
    float textureScaleV = 1.0f;
    float textureOffsetU = 0.0f;
    float textureOffsetV = 0.0f;

    // Textures, bump_scale: how hard the normal image's xy is pushed away from flat, 1 leaving
    // it alone. Needs a normal image to act on, which is what the game's key does too.
    float bumpScale = 1.0f;

    // Textures, parallax_scale: POM relief depth fraction relative to UV tile size (0 disables).
    float parallaxScale = 0.0f;

    // Surface. roughnessOverride < 0 leaves the shading default (kEditorDefaultRoughness)
    // in charge, which is how the game's roughness_override slider at 0 behaves.
    float roughnessOverride = -1.0f;
    float metalnessFactor = 0.0f;

    // Surface, metalness_from_normal_alpha: the normal image's alpha carries the metalness
    // image, multiplied by metalnessFactor. Only a baked material can express it, since the
    // scalar path has nowhere to put a per-pixel value.
    bool metalnessFromNormalAlpha = false;
    float baseFactor = 1.0f;
    EditorMaterialType type = EditorMaterialType::None;

    // Surface, mirror: a mirror-smooth reflection. The game gives it roughness 0 and the
    // multiplicative albedo-over-refl/refr flag, and lets glass and water win over it.
    bool mirror = false;

    // Surface, glass: the pane's own tint, index of refraction and thickness. The renderer
    // reads them out of layer colors 1 and 2, which is why they need no extra upload field.
    // ior 0 keeps the engine's rt_refr_glass; thickness 2 and white are the game's defaults.
    float glassIor = 0.0f;
    float glassThickness = 2.0f;
    float glassColor[3] = { 1.0f, 1.0f, 1.0f };

    // Surface, glass: the game's transparency, how much light the pane passes. 1 is a pane that
    // passes everything. The traced path takes it out of the alpha of the normal slot, so it is
    // baked there and a non-glass material leaves that alpha at 1.
    float transparency = 1.0f;

    // Model
    bool exactNormals = false;

    // Model, force_rasterize: the game lets a model or sprite material skip its rasterized
    // emitter. Those are paths this editor does not upload, and QrGeometryUploadInfo has no such
    // flag, so the value is carried through the session and reaches nothing.
    bool forceRasterize = false;

    // Luma. defaultEmission is clamped to [0, 1] by the renderer. The game's material default is
    // 1 (rt_material.c's rt_mat_reset), which is what a light material's colour is scaled by.
    float emissiveFactor = 1.0f;
    bool luma = false;
    bool emissiveEnabled = false;
    bool hasColorEmissive = false;

    // Luma, luma_blend: -1 follows the global rt_emis_blend the way the game's cvar value does,
    // 0..5 pin off / normal / add / overlay / hard light / color dodge. Rides the alpha of the
    // packed roughness-metallic-emission texture, so it only reaches the shader once a material
    // texture exists -- which any emission above zero already needs.
    int32_t lumaBlend = -1;

    // Luma, the game's color_emissive blocks: an atlas with several differently coloured glow
    // regions, each with its own tone controls. A block builds its own mask out of the albedo
    // and the bake composites them by nearest colour, exactly as gl_texmgr.c does. A material
    // that names an emissive image gets that image instead, which is the game's own precedence.
    std::vector<EditorEmissiveBlock> emissiveBlocks;

    // Luma, luma_threshold / luma_feather: the material-level fallbacks a block inherits when its
    // entry omits them, and the values a block the editor appends starts from.
    float lumaThreshold = 0.02f;
    float lumaFeather = 0.0f;

    // Gloss: procedural roughness/metalness mask blocks evaluated when texture_gloss is empty.
    bool glossEnabled = false;
    bool hasColorGloss = false;
    std::vector<EditorGlossBlock> glossBlocks;
    float glossThreshold = 0.02f;
    float glossFeather = 0.0f;

    // Light: the rest of the group the game's is_light (`luma` above) belongs to. They say what
    // light the material's own surface casts and how bright and which colour it is. The SDK's
    // upload path derives a surface's cast light from geometry it does not carry for an alias
    // model, so the editor authors, reads and re-writes these keys instead of showing sliders
    // that would not move the picture; the merge then keeps a light material's cast light on the
    // way back to the game.
    bool lightStyles = false;
    bool hasLightColor = false;
    float lightColor[3] = { 1.0f, 1.0f, 1.0f };
    float lightBrightness = 1.0f;
    float lightMinArea = 0.0f;
    // The game's RT_MAT default: -1 is "unset", and only a value above 0 is written.
    float lightFocus = -1.0f;
    float lightFocusSoft = -1.0f;
    bool lightProjector = false;
    bool lightShafts = false;
    float lightShaftsPower = 1.0f;
    float lightShaftsDistance = 0.0f;
    float lightShaftsFalloff = 1.0f;
    bool enabled = true;
    // Bitmask of disabled overlay shader stages (bit i = 1 disables stage i).
    uint32_t disabledStagesMask = 0;

    // Subsurface scattering: optional explicit mask texture, global strength in [0, 1], linear
    // scatter tint, scatter radius in meters (clamped to [0, 1]), and colour-key / polygon
    // blocks that bake per-pixel SSS strength into Normal.a.
    bool sssEnabled = false;
    std::string textureSss;
    float sssStrength = 0.0f;
    float sssColor[3] = { 1.0f, 0.35f, 0.2f };
    float sssRadius = 0.02f;
    std::vector<EditorSssBlock> sssBlocks;
    float sssThreshold = 0.02f;
    float sssFeather = 0.0f;

    bool operator==(const EditorMaterialParams &) const = default;
};

// Default roughness matching the game's rt_brush_rough ("1.0") when a material has no override.
inline constexpr float kEditorDefaultRoughness = 0.8f;

bool HasEditorSubsurface(const EditorMaterialParams &material);
bool EditorMaterialPromotesOverlayToTraced(const EditorMaterialParams &material);

void ApplyEditorSubsurface(const EditorMaterialParams &material,
                           QrGeometryUploadInfo &upload);

std::vector<uint8_t> BuildEditorSssMask(const EditorMaterialParams &params,
                                        uint32_t width,
                                        uint32_t height,
                                        const uint8_t *albedoRgba,
                                        const uint8_t *textureSssRgba = nullptr,
                                        uint32_t textureSssWidth = 0,
                                        uint32_t textureSssHeight = 0);

// Writes one material into an upload info: the albedo tint from baseFactor, the shading
// scalars, the texcoord transform and the exact-normals flag. `passThroughType` is passed in
// because only the caller knows which one the surface kind derived.
void ApplyEditorMaterial(const EditorMaterialParams &material,
                         QrGeometryPassThroughType derivedPassThrough,
                         QrGeometryUploadInfo &upload);

// One material per texture name, the way the game keys rt_material_t: two surfaces that share
// a texture share a material, so editing it changes every face that uses it.
class EditorMaterialTable
{
public:
    void Rebuild(const EditorScene &scene);

    size_t Count() const { return m_entries.size(); }
    bool Empty() const { return m_entries.empty(); }

    const std::string &NameAt(size_t index) const { return m_entries[index].name; }
    EditorMaterialParams &At(size_t index) { return m_entries[index].params; }
    const EditorMaterialParams &At(size_t index) const { return m_entries[index].params; }

    // Index of the material a texture name maps to, or -1.
    int32_t IndexOf(const std::string &name) const;

    // The index of `name`, appending an entry with default parameters when the scene has no
    // surface of that name. This is how a material the map does not texture yet -- an alias
    // model's progs/foo.mdl:frame0, which the editor's scene carries no geometry for -- is
    // authored: the game's regenerate creates the same entries in its own table.
    int32_t Ensure(const std::string &name);

    // The material of a surface, or the defaults when the table has no entry for its texture.
    const EditorMaterialParams &For(const EditorSurface &surface) const;

    // Effective material entry name bound to `surface` (e.g. active "+0..." animation frame).
    const std::string &EffectiveNameFor(const EditorSurface &surface) const;

    // Binds a surface family name (e.g. "+basebtn") to a specific frame entry index.
    void BindSurfaceAlias(const std::string &surfaceName, int32_t entryIndex);

private:
    struct Entry
    {
        std::string name;
        EditorMaterialParams params;
    };

    std::vector<Entry> m_entries;
    std::map<std::string, size_t> m_indexByName;
    EditorMaterialParams m_defaults;
};

// Matches RT_EMIS_GLOW_THRESHOLD in vkquake-rt's Quake/gl_texmgr.c.
inline constexpr float kEditorGlowThreshold = 0.02f;

// What the emission channel of a bake says about the light its surface casts. gl_texmgr.c
// derives the same three things while it synthesizes the packed set: rtemissivecolor, the
// albedo weighted sum of the mask normalised by mean_base, and rtemissivemean, the
// brightness clamped mean a masked light carries as its meanEmiss. A material with no mask has
// no record at all, which is the game's QR_NO_MATERIAL light: uniform over its whole polygon.
struct EditorMaterialEmission
{
    bool valid = false;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float mean = 0.0f;
    float meanBase = 0.0f;
    bool hasGlow = false;
    float glowUvMin[2] = { 0.0f, 0.0f };
    float glowUvMax[2] = { 1.0f, 1.0f };
    float glowMean = 0.0f;
};

// Computes tight UV glow bounds and mean emission over the active box from texel extents.
void FinalizeEditorGlowExtents(int minX, int minY, int maxX, int maxY, uint32_t width,
                               uint32_t height, EditorMaterialEmission &out);

// Bakes the material parameters whose only consumer is a packed material texture. The SDK takes
// one albedo, one normal and one roughness-metallic-emission image, so a material that names a
// gloss, normal or emissive image, pushes its bumps, passes light through a pane or picks an
// emission blend has to have those folded into that set -- exactly what Quake/gl_texmgr.c does
// when it synthesizes a material texture. Everything else keeps the pool material the scene
// uploaded, so a material with no such key costs no bake.
class EditorMaterialBakes
{
public:
    // Materials created by Get are released here; the caller must have uploaded no geometry
    // that still references them.
    void Destroy(QrInstance instance);

    // Releases only the cached bakes that reference `textureIndex` in any texture slot
    // (albedo, gloss, normal, emissive, overlay). Returns true if at least one entry was
    // invalidated so the caller knows whether static scene geometry needs a re-upload.
    bool InvalidateTexture(QrInstance instance, int32_t textureIndex);

    // Call before re-uploading the scene: Get marks what the pass keeps, EndPass releases the
    // bakes no geometry uses any more, which is how a slider drag does not pile up materials.
    void BeginPass() { for (Entry &entry : m_entries) { entry.used = false; } }
    void EndPass(QrInstance instance);

    // The bakes that are alive, which is what a scripted session reads to tell an upload that
    // used them from one that fell back to the pool.
    size_t Count() const { return m_entries.size(); }

    // The material to upload for one surface: the bake that matches its parameters, or
    // QR_NO_MATERIAL when they need none and the pool material is the answer. `resolveTexture`
    // maps a texture name to an index in `scene.textures`, or -1.
    QrMaterial Get(QrInstance instance, const std::string &name, const EditorMaterialParams &params,
                   const EditorScene &scene, const std::vector<QrMaterial> &pool,
                   const std::function<int32_t(const std::string &)> &resolveTexture,
                   bool forceEnvBase = false);

    // The emission the bake behind this handle carries, or an invalid record when the handle is
    // no bake of this pass. A textured area light reads the mask off the material it is handed,
    // so a surface that casts light takes its colour and its mean from the same bake.
    EditorMaterialEmission EmissionOf(QrMaterial material) const;

private:
    struct Key
    {
        int32_t albedo = -1;
        int32_t gloss = -1;
        int32_t normal = -1;
        int32_t heightTex = -1;
        int32_t emissive = -1;
        int32_t overlay = -1;
        int32_t emissiveStageIndex = -1;
        uint32_t disabledStagesMask = 0;
        int32_t sssTex = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        float bumpScale = 1.0f;
        float transparency = 1.0f;
        float roughness = 0.0f;
        float metallic = 0.0f;
        float emission = 0.0f;
        int32_t lumaBlend = -1;
        bool metalnessFromNormalAlpha = false;
        bool hasRoughnessOverride = false;
        bool useEmbeddedPbrMasks = false;
        bool hasParallaxHeight = false;
        bool mirror = false;
        bool neutralMirrorAlbedo = false;
        bool enabled = true;
        bool uniformLightGlow = false;
        float lightColor[3] = { 1.0f, 1.0f, 1.0f };
        bool sssEnabled = false;
        // The colour blocks, serialized: two materials that differ in any block's colour,
        // threshold, feather, factor, blend or polygon are different bakes.
        std::string emissiveMask;
        std::string glossMask;
        std::string sssMask;
        bool operator==(const Key &other) const = default;
    };

    struct Entry
    {
        Key key;
        QrMaterial material = QR_NO_MATERIAL;
        EditorMaterialEmission emission;
        bool used = false;
    };

    std::vector<Entry> m_entries;
};

// Target map type for on-demand AI / intrinsic texture generation from a diffuse texture.
enum class EditorAiMapKind
{
    Normals,
    Emissive,
    Gloss,
    Delight,
    Height,
    Upscale,
};

// Generates a seamless RGBA texture map (`Normals`, `Emissive`, `Gloss`, `Delight`, `Height`, or
// `Upscale`) from a diffuse RGBA image (`width` x `height` x 4). Optionally incorporates
// `fullbrightMask` (`width` x `height` bytes) for `Emissive` when available.
bool GenerateEditorAiTextureFromDiffuse(const uint8_t *diffuseRgba, uint32_t width, uint32_t height,
                                        const uint8_t *fullbrightMask, EditorAiMapKind kind,
                                        std::vector<uint8_t> &outRgba);

// Encodes tightly-packed 8-bit RGBA pixels into an uncompressed 32-bit top-left TGA byte buffer.
std::vector<uint8_t> EncodeEditorTgaRgba(const uint8_t *rgba, uint32_t width, uint32_t height);
