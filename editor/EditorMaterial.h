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

// The subset of the game's rt_material_t the editor expresses. The keys whose only consumer is
// a packed material texture -- the extra images, bump_scale, transparency, luma_blend -- are
// baked into the albedo / normal / roughness-metallic-emission set by EditorMaterialBakes, the
// way the game's texture manager synthesizes a material texture at load. Keys the SDK has no
// slot for at all (overlay, colored emissive, force_rasterize, light_*) stay absent instead of
// being sliders that would not change the picture.
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
    std::string emissiveTexture;
    std::string glossTexture;

    // Textures. Quake's texture_scale_* / texture_offset_* feed the layer-0 texcoord
    // transform; the SDK applies them under QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT.
    float textureScaleU = 1.0f;
    float textureScaleV = 1.0f;
    float textureOffsetU = 0.0f;
    float textureOffsetV = 0.0f;

    // Textures, bump_scale: how hard the normal image's xy is pushed away from flat, 1 leaving
    // it alone. Needs a normal image to act on, which is what the game's key does too.
    float bumpScale = 1.0f;

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

    // Luma. defaultEmission is clamped to [0, 1] by the renderer.
    float emissiveFactor = 0.0f;
    bool luma = false;

    // Luma, luma_blend: -1 follows the global rt_emis_blend the way the game's cvar value does,
    // 0..5 pin off / normal / add / overlay / hard light / color dodge. Rides the alpha of the
    // packed roughness-metallic-emission texture, so it only reaches the shader once a material
    // texture exists -- which any emission above zero already needs.
    int32_t lumaBlend = -1;
};

// The roughness the editor has always used for BSP surfaces, kept as the value a
// material falls back to when it has no override.
inline constexpr float kEditorDefaultRoughness = 0.7f;

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

    // The material of a surface, or the defaults when the table has no entry for its texture.
    const EditorMaterialParams &For(const EditorSurface &surface) const;

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
                   const std::function<int32_t(const std::string &)> &resolveTexture);

private:
    struct Key
    {
        int32_t albedo = -1;
        int32_t gloss = -1;
        int32_t normal = -1;
        int32_t emissive = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        float bumpScale = 1.0f;
        float transparency = 1.0f;
        float roughness = 0.0f;
        float metallic = 0.0f;
        float emission = 0.0f;
        int32_t lumaBlend = -1;
        bool metalnessFromNormalAlpha = false;
        bool operator==(const Key &other) const;
    };

    struct Entry
    {
        Key key;
        QrMaterial material = QR_NO_MATERIAL;
        bool used = false;
    };

    std::vector<Entry> m_entries;
};
