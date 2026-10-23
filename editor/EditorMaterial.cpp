#include "EditorMaterial.h"

#include <algorithm>

#include "Scene.h"

void ApplyEditorMaterial(const EditorMaterialParams &material,
                         QrGeometryPassThroughType derivedPassThrough,
                         QrGeometryUploadInfo &upload)
{
    QrGeometryPassThroughType passThrough = derivedPassThrough;

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

    upload.passThroughType = passThrough;

    upload.defaultRoughness = material.roughnessOverride < 0.0f ? kEditorDefaultRoughness
                                                                : material.roughnessOverride;
    upload.defaultMetallicity = material.metalnessFactor;
    upload.defaultEmission = material.emissiveFactor;

    const float base = material.baseFactor;
    upload.layerColors[0] = { base, base, base, 1.0f };

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

const EditorMaterialParams &EditorMaterialTable::For(const EditorSurface &surface) const
{
    const auto it = m_indexByName.find(surface.textureName);

    if (it == m_indexByName.end())
    {
        return m_defaults;
    }

    return m_entries[it->second].params;
}
