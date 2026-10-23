#pragma once

#include <cstddef>
#include <cstdint>
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

// The subset of the game's rt_material_t that the SDK carries per geometry. Anything the
// upload info cannot express yet (bump_scale, glass_ior, glass_thickness, transparency,
// metalness_from_normal_alpha, light_*) is deliberately absent instead of being a slider
// that would not change the picture.
struct EditorMaterialParams
{
    // The game's texture_base: the image the surface shows instead of its own. An embedder that
    // can decode files resolves it as a path; the editor reads its images out of the scene, so
    // the value names one of those textures and a name the scene does not carry changes nothing.
    // Empty is the game's NONE: the map's own image.
    std::string baseTexture;

    // Textures. Quake's texture_scale_* / texture_offset_* feed the layer-0 texcoord
    // transform; the SDK applies them under QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT.
    float textureScaleU = 1.0f;
    float textureScaleV = 1.0f;
    float textureOffsetU = 0.0f;
    float textureOffsetV = 0.0f;

    // Surface. roughnessOverride < 0 leaves the shading default (kEditorDefaultRoughness)
    // in charge, which is how the game's roughness_override slider at 0 behaves.
    float roughnessOverride = -1.0f;
    float metalnessFactor = 0.0f;
    float baseFactor = 1.0f;
    EditorMaterialType type = EditorMaterialType::None;

    // Model
    bool exactNormals = false;

    // Luma. defaultEmission is clamped to [0, 1] by the renderer.
    float emissiveFactor = 0.0f;
    bool luma = false;
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
