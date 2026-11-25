#include "Scene.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace
{

struct Vec3
{
    float value[3] = {};
};

struct Vec2
{
    float value[2] = {};
};

struct ObjCorner
{
    int position = 0;
    int texCoord = -1;
    int normal = -1;
};

bool ParseFaceCorner(const std::string &token, ObjCorner &corner)
{
    corner = {};

    const size_t first = token.find('/');
    const std::string positionText = token.substr(0, first);

    if (positionText.empty())
    {
        return false;
    }

    corner.position = std::stoi(positionText);

    if (first == std::string::npos)
    {
        return true;
    }

    const size_t second = token.find('/', first + 1);

    if (second != std::string::npos)
    {
        const std::string texCoordText = token.substr(first + 1, second - first - 1);

        if (!texCoordText.empty())
        {
            corner.texCoord = std::stoi(texCoordText);
        }

        const std::string normalText = token.substr(second + 1);

        if (!normalText.empty())
        {
            corner.normal = std::stoi(normalText);
        }
    }
    else
    {
        const std::string texCoordText = token.substr(first + 1);

        if (!texCoordText.empty())
        {
            corner.texCoord = std::stoi(texCoordText);
        }
    }

    return true;
}

int ResolveIndex(int index, size_t count)
{
    if (index > 0)
    {
        return index - 1;
    }

    if (index < 0)
    {
        return int(count) + index;
    }

    return -1;
}

void UpdateSceneBounds(EditorScene &scene)
{
    if (scene.vertices.empty())
    {
        return;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        scene.boundsMin[axis] = scene.vertices[0].position[axis];
        scene.boundsMax[axis] = scene.vertices[0].position[axis];
    }

    for (const QrVertex &vertex : scene.vertices)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            scene.boundsMin[axis] = std::min(scene.boundsMin[axis], vertex.position[axis]);
            scene.boundsMax[axis] = std::max(scene.boundsMax[axis], vertex.position[axis]);
        }
    }

    scene.hasBounds = true;
}

struct ObjMeshTables
{
    std::vector<Vec3> positions;
    std::vector<Vec2> texCoords;
    std::vector<Vec3> normals;
};

void ComputeTriangleNormal(const float a[3], const float b[3], const float c[3], float outNormal[3])
{
    const float e0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e1[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    outNormal[0] = e0[1] * e1[2] - e0[2] * e1[1];
    outNormal[1] = e0[2] * e1[0] - e0[0] * e1[2];
    outNormal[2] = e0[0] * e1[1] - e0[1] * e1[0];

    const float len = std::sqrt(outNormal[0] * outNormal[0] + outNormal[1] * outNormal[1] +
                                outNormal[2] * outNormal[2]);

    if (len > 0.000001f)
    {
        outNormal[0] /= len;
        outNormal[1] /= len;
        outNormal[2] /= len;
    }
}

void AppendObjFacePolygon(const std::vector<ObjCorner> &corners, const ObjMeshTables &tables,
                          EditorScene &scene, std::vector<EditorSurfaceFace> &faces)
{
    if (corners.size() < 3)
    {
        return;
    }

    EditorSurfaceFace face;
    face.cluster = 0;
    face.positions.reserve(corners.size());
    face.texCoords.reserve(corners.size());

    ComputeTriangleNormal(tables.positions[size_t(corners[0].position)].value,
                          tables.positions[size_t(corners[1].position)].value,
                          tables.positions[size_t(corners[2].position)].value, face.normal);

    for (const ObjCorner &c : corners)
    {
        const Vec3 &p = tables.positions[size_t(c.position)];
        face.positions.push_back({ p.value[0], p.value[1], p.value[2] });
        const QrFloat2D uv = c.texCoord >= 0
                                 ? QrFloat2D{ tables.texCoords[size_t(c.texCoord)].value[0],
                                              tables.texCoords[size_t(c.texCoord)].value[1] }
                                 : QrFloat2D{ 0.0f, 0.0f };
        face.texCoords.push_back(uv);
    }

    faces.push_back(std::move(face));

    for (size_t i = 1; i + 1 < corners.size(); i++)
    {
        const ObjCorner triangle[3] = { corners[0], corners[i], corners[i + 1] };
        float triNormal[3] = {};
        ComputeTriangleNormal(tables.positions[size_t(triangle[0].position)].value,
                              tables.positions[size_t(triangle[1].position)].value,
                              tables.positions[size_t(triangle[2].position)].value, triNormal);

        for (const ObjCorner &corner : triangle)
        {
            QrVertex vertex = {};
            std::memcpy(vertex.position, tables.positions[size_t(corner.position)].value,
                        sizeof(vertex.position));
            const float *nSrc = corner.normal >= 0 ? tables.normals[size_t(corner.normal)].value
                                                   : triNormal;
            std::memcpy(vertex.normal, nSrc, sizeof(vertex.normal));

            if (corner.texCoord >= 0)
            {
                vertex.texCoord[0] = tables.texCoords[size_t(corner.texCoord)].value[0];
                vertex.texCoord[1] = tables.texCoords[size_t(corner.texCoord)].value[1];
            }

            vertex.packedColor = 0xFFFFFFFFu;
            scene.indices.push_back(uint32_t(scene.vertices.size()));
            scene.vertices.push_back(vertex);
        }
    }
}

void FinalizeObjSurface(EditorScene &scene, std::vector<EditorSurfaceFace> &&faces)
{
    if (!scene.surfaces.empty() || faces.empty())
    {
        return;
    }

    EditorSurface surface;
    surface.textureName = "default";
    surface.vertices = scene.vertices;
    surface.indices = scene.indices;
    surface.faces = std::move(faces);
    scene.surfaces.push_back(std::move(surface));
}

}

bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error)
{
    scene = EditorScene{};

    std::ifstream file(path);

    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }

    ObjMeshTables tables;
    std::vector<EditorSurfaceFace> faces;
    std::string line;

    while (std::getline(file, line))
    {
        std::istringstream stream(line);
        std::string type;
        stream >> type;

        if (type == "v")
        {
            Vec3 values;
            stream >> values.value[0] >> values.value[1] >> values.value[2];
            tables.positions.push_back(values);
        }
        else if (type == "vt")
        {
            Vec2 values;
            stream >> values.value[0] >> values.value[1];
            tables.texCoords.push_back(values);
        }
        else if (type == "vn")
        {
            Vec3 values;
            stream >> values.value[0] >> values.value[1] >> values.value[2];
            tables.normals.push_back(values);
        }
        else if (type == "f")
        {
            std::vector<ObjCorner> corners;
            std::string token;

            while (stream >> token)
            {
                ObjCorner corner = {};

                if (!ParseFaceCorner(token, corner))
                {
                    error = "invalid face in " + path;
                    return false;
                }

                corner.position = ResolveIndex(corner.position, tables.positions.size());
                corner.texCoord =
                    corner.texCoord != 0 ? ResolveIndex(corner.texCoord, tables.texCoords.size()) : -1;
                corner.normal =
                    corner.normal != 0 ? ResolveIndex(corner.normal, tables.normals.size()) : -1;

                if (corner.position < 0 || corner.position >= int(tables.positions.size()))
                {
                    error = "face position out of range in " + path;
                    return false;
                }

                if (corner.texCoord >= int(tables.texCoords.size()) ||
                    corner.normal >= int(tables.normals.size()))
                {
                    error = "face attribute out of range in " + path;
                    return false;
                }

                corners.push_back(corner);
            }

            AppendObjFacePolygon(corners, tables, scene, faces);
        }
    }

    if (scene.vertices.empty() || scene.indices.empty())
    {
        error = "no triangles in " + path;
        return false;
    }

    FinalizeObjSurface(scene, std::move(faces));
    UpdateSceneBounds(scene);
    return true;
}

namespace
{

template <typename T>
T ReadLittleEndian(const std::vector<uint8_t> &bytes, size_t offset)
{
    T value = {};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct BspLump
{
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct BspPlane
{
    float normal[3] = {};
    float distance = 0.0f;
};

struct BspEdge
{
    uint32_t vertex[2] = {};
};

struct BspTexInfo
{
    float u[4] = {};
    float v[4] = {};
    int32_t miptex = -1;
    int32_t flags = 0;
};

struct BspFace
{
    int32_t plane = 0;
    int32_t side = 0;
    int32_t firstEdge = 0;
    int32_t edgeCount = 0;
    int32_t texInfo = 0;
};

struct BspFormat
{
    int32_t bsp2 = 0; // 0 = BSP29/Q64, 1 = 2PSB, 2 = BSP2
    bool isQuake64 = false;
    size_t nodeStride = 24;
    size_t leafStride = 28;
    size_t faceStride = 20;
    size_t edgeStride = 4;
    size_t miptexHeaderSize = 40;
};

bool DetectBspFormat(int32_t version, BspFormat &out)
{
    constexpr int32_t kBspVersion29 = 29;
    constexpr int32_t kBsp2Version2psb = 0x42535032; // "2PSB"
    constexpr int32_t kBsp2VersionBsp2 = 0x32505342; // "BSP2"
    constexpr int32_t kBspVersionQuake64 = 0x51363420;
    constexpr int32_t kBspVersionQuake64Le = 0x20343651;

    out = BspFormat{};
    if (version == kBspVersion29)
    {
        return true;
    }
    if (version == kBspVersionQuake64 || version == kBspVersionQuake64Le)
    {
        out.isQuake64 = true;
        out.miptexHeaderSize = 44;
        return true;
    }
    if (version == kBsp2Version2psb)
    {
        out.bsp2 = 1;
        out.nodeStride = 32;
        out.leafStride = 32;
        out.faceStride = 28;
        out.edgeStride = 8;
        return true;
    }
    if (version == kBsp2VersionBsp2)
    {
        out.bsp2 = 2;
        out.nodeStride = 44;
        out.leafStride = 44;
        out.faceStride = 28;
        out.edgeStride = 8;
        return true;
    }
    return false;
}

bool LumpFits(const BspLump &lump, size_t fileSize, size_t elementSize)
{
    return lump.offset <= fileSize &&
           lump.length <= fileSize - lump.offset &&
           (elementSize == 0 || lump.length % elementSize == 0);
}

// One `{ "key" "value" ... }` block of the BSP entity lump (lump 0).
struct BspEntity
{
    std::vector<std::pair<std::string, std::string>> fields;

    const std::string &Value(const char *key) const
    {
        static const std::string empty;

        for (const auto &field : fields)
        {
            if (field.first == key)
            {
                return field.second;
            }
        }

        return empty;
    }
};

bool ParseBspEntities(const std::vector<uint8_t> &bytes, const BspLump &lump,
                      std::vector<BspEntity> &entities)
{
    if (!LumpFits(lump, bytes.size(), 0))
    {
        return false;
    }

    const char *cursor = reinterpret_cast<const char *>(bytes.data() + lump.offset);
    const char *end = cursor + lump.length;

    const auto skipSpace = [&cursor, end]() {
        while (cursor < end)
        {
            const unsigned char character = static_cast<unsigned char>(*cursor);

            if (std::isspace(character) || character == 0)
            {
                cursor++;
                continue;
            }

            break;
        }
    };

    const auto readToken = [&cursor, end](std::string &token) {
        if (cursor >= end || *cursor != '"')
        {
            return false;
        }

        cursor++;
        token.clear();

        while (cursor < end && *cursor != '"')
        {
            token.push_back(*cursor++);
        }

        if (cursor >= end)
        {
            return false;
        }

        cursor++;
        return true;
    };

    while (true)
    {
        skipSpace();

        if (cursor >= end)
        {
            break;
        }

        if (*cursor != '{')
        {
            cursor++;
            continue;
        }

        cursor++;
        BspEntity entity;

        while (true)
        {
            skipSpace();

            if (cursor >= end)
            {
                return false;
            }

            if (*cursor == '}')
            {
                cursor++;
                break;
            }

            std::string key;
            std::string value;

            if (!readToken(key))
            {
                return false;
            }

            skipSpace();

            if (!readToken(value))
            {
                return false;
            }

            entity.fields.emplace_back(std::move(key), std::move(value));
        }

        entities.push_back(std::move(entity));
    }

    return true;
}

bool StartsWithLight(const std::string &text)
{
    return text.size() >= 5 && text.compare(0, 5, "light") == 0;
}

// Mirrors RT_UploadAllElights + RT_LIGHT_Emit + RT_FIXUP_LIGHT_INTENSITY through
// kEditorLightIntensityScale. The radius keeps the rt_elight_radius default of one
// centimetre, which is 40 scene units at the SDK's 40 units per metre
// (kEditorUnitsPerMeter). Map light colours stay at the default light colour, exactly as
// the game ignores `_color`/`color` keys and the stored has_color flag.
constexpr float kDefaultLightIntensity = 200.0f;
constexpr float kDefaultLightRadiusMeters = 0.01f;

float Dot(const float a[3], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
}

struct MipTexture
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> indices;
    std::vector<uint8_t> palette;
};

std::string Lowercase(std::string text)
{
    for (char &character : text)
    {
        character = char(std::tolower(static_cast<unsigned char>(character)));
    }

    return text;
}

// The miptex name field is fixed at 16 bytes and is not always terminated.
std::string MipTextureName(const uint8_t *name)
{
    char buffer[17] = {};
    std::memcpy(buffer, name, 16);
    return Lowercase(std::string(buffer));
}

// Standard Quake 1 palette (gfx/palette.lmp) used as fallback when no external game palette is supplied.
constexpr uint8_t kDefaultQuakePalette[768] = {
    0x00, 0x00, 0x00, 0x0f, 0x0f, 0x0f, 0x1f, 0x1f, 0x1f, 0x2f, 0x2f, 0x2f,
    0x3f, 0x3f, 0x3f, 0x4b, 0x4b, 0x4b, 0x5b, 0x5b, 0x5b, 0x6b, 0x6b, 0x6b,
    0x7b, 0x7b, 0x7b, 0x8b, 0x8b, 0x8b, 0x9b, 0x9b, 0x9b, 0xab, 0xab, 0xab,
    0xbb, 0xbb, 0xbb, 0xcb, 0xcb, 0xcb, 0xdb, 0xdb, 0xdb, 0xeb, 0xeb, 0xeb,
    0x0f, 0x0b, 0x07, 0x17, 0x0f, 0x0b, 0x1f, 0x17, 0x0b, 0x27, 0x1b, 0x0f,
    0x2f, 0x23, 0x13, 0x37, 0x2b, 0x17, 0x3f, 0x2f, 0x17, 0x4b, 0x37, 0x1b,
    0x53, 0x3b, 0x1b, 0x5b, 0x43, 0x1f, 0x63, 0x4b, 0x1f, 0x6b, 0x53, 0x1f,
    0x73, 0x57, 0x1f, 0x7b, 0x5f, 0x23, 0x83, 0x67, 0x23, 0x8f, 0x6f, 0x23,
    0x0b, 0x0b, 0x0f, 0x13, 0x13, 0x1b, 0x1b, 0x1b, 0x27, 0x27, 0x27, 0x33,
    0x2f, 0x2f, 0x3f, 0x37, 0x37, 0x4b, 0x3f, 0x3f, 0x57, 0x47, 0x47, 0x67,
    0x4f, 0x4f, 0x73, 0x5b, 0x5b, 0x7f, 0x63, 0x63, 0x8b, 0x6b, 0x6b, 0x97,
    0x73, 0x73, 0xa3, 0x7b, 0x7b, 0xaf, 0x83, 0x83, 0xbb, 0x8b, 0x8b, 0xcb,
    0x00, 0x00, 0x00, 0x07, 0x07, 0x00, 0x0b, 0x0b, 0x00, 0x13, 0x13, 0x00,
    0x1b, 0x1b, 0x00, 0x23, 0x23, 0x00, 0x2b, 0x2b, 0x07, 0x2f, 0x2f, 0x07,
    0x37, 0x37, 0x07, 0x3f, 0x3f, 0x07, 0x47, 0x47, 0x07, 0x4b, 0x4b, 0x0b,
    0x53, 0x53, 0x0b, 0x5b, 0x5b, 0x0b, 0x63, 0x63, 0x0b, 0x6b, 0x6b, 0x0f,
    0x07, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x17, 0x00, 0x00, 0x1f, 0x00, 0x00,
    0x27, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x37, 0x00, 0x00, 0x3f, 0x00, 0x00,
    0x47, 0x00, 0x00, 0x4f, 0x00, 0x00, 0x57, 0x00, 0x00, 0x5f, 0x00, 0x00,
    0x67, 0x00, 0x00, 0x6f, 0x00, 0x00, 0x77, 0x00, 0x00, 0x7f, 0x00, 0x00,
    0x13, 0x13, 0x00, 0x1b, 0x1b, 0x00, 0x23, 0x23, 0x00, 0x2f, 0x2b, 0x00,
    0x37, 0x2f, 0x00, 0x43, 0x37, 0x00, 0x4b, 0x3b, 0x07, 0x57, 0x43, 0x07,
    0x5f, 0x47, 0x07, 0x6b, 0x4b, 0x0b, 0x77, 0x53, 0x0f, 0x83, 0x57, 0x13,
    0x8b, 0x5b, 0x13, 0x97, 0x5f, 0x1b, 0xa3, 0x63, 0x1f, 0xaf, 0x67, 0x23,
    0x23, 0x13, 0x07, 0x2f, 0x17, 0x0b, 0x3b, 0x1f, 0x0f, 0x4b, 0x23, 0x13,
    0x57, 0x2b, 0x17, 0x63, 0x2f, 0x1f, 0x73, 0x37, 0x23, 0x7f, 0x3b, 0x2b,
    0x8f, 0x43, 0x33, 0x9f, 0x4f, 0x33, 0xaf, 0x63, 0x2f, 0xbf, 0x77, 0x2f,
    0xcf, 0x8f, 0x2b, 0xdf, 0xab, 0x27, 0xef, 0xcb, 0x1f, 0xff, 0xf3, 0x1b,
    0x0b, 0x07, 0x00, 0x1b, 0x13, 0x00, 0x2b, 0x23, 0x0f, 0x37, 0x2b, 0x13,
    0x47, 0x33, 0x1b, 0x53, 0x37, 0x23, 0x63, 0x3f, 0x2b, 0x6f, 0x47, 0x33,
    0x7f, 0x53, 0x3f, 0x8b, 0x5f, 0x47, 0x9b, 0x6b, 0x53, 0xa7, 0x7b, 0x5f,
    0xb7, 0x87, 0x6b, 0xc3, 0x93, 0x7b, 0xd3, 0xa3, 0x8b, 0xe3, 0xb3, 0x97,
    0xab, 0x8b, 0xa3, 0x9f, 0x7f, 0x97, 0x93, 0x73, 0x87, 0x8b, 0x67, 0x7b,
    0x7f, 0x5b, 0x6f, 0x77, 0x53, 0x63, 0x6b, 0x4b, 0x57, 0x5f, 0x3f, 0x4b,
    0x57, 0x37, 0x43, 0x4b, 0x2f, 0x37, 0x43, 0x27, 0x2f, 0x37, 0x1f, 0x23,
    0x2b, 0x17, 0x1b, 0x23, 0x13, 0x13, 0x17, 0x0b, 0x0b, 0x0f, 0x07, 0x07,
    0xbb, 0x73, 0x9f, 0xaf, 0x6b, 0x8f, 0xa3, 0x5f, 0x83, 0x97, 0x57, 0x77,
    0x8b, 0x4f, 0x6b, 0x7f, 0x4b, 0x5f, 0x73, 0x43, 0x53, 0x6b, 0x3b, 0x4b,
    0x5f, 0x33, 0x3f, 0x53, 0x2b, 0x37, 0x47, 0x23, 0x2b, 0x3b, 0x1f, 0x23,
    0x2f, 0x17, 0x1b, 0x23, 0x13, 0x13, 0x17, 0x0b, 0x0b, 0x0f, 0x07, 0x07,
    0xdb, 0xc3, 0xbb, 0xcb, 0xb3, 0xa7, 0xbf, 0xa3, 0x9b, 0xaf, 0x97, 0x8b,
    0xa3, 0x87, 0x7b, 0x97, 0x7b, 0x6f, 0x87, 0x6f, 0x5f, 0x7b, 0x63, 0x53,
    0x6b, 0x57, 0x47, 0x5f, 0x4b, 0x3b, 0x53, 0x3f, 0x33, 0x43, 0x33, 0x27,
    0x37, 0x2b, 0x1f, 0x27, 0x1f, 0x17, 0x1b, 0x13, 0x0f, 0x0f, 0x0b, 0x07,
    0x6f, 0x83, 0x7b, 0x67, 0x7b, 0x6f, 0x5f, 0x73, 0x67, 0x57, 0x6b, 0x5f,
    0x4f, 0x63, 0x57, 0x47, 0x5b, 0x4f, 0x3f, 0x53, 0x47, 0x37, 0x4b, 0x3f,
    0x2f, 0x43, 0x37, 0x2b, 0x3b, 0x2f, 0x23, 0x33, 0x27, 0x1f, 0x2b, 0x1f,
    0x17, 0x23, 0x17, 0x0f, 0x1b, 0x13, 0x0b, 0x13, 0x0b, 0x07, 0x0b, 0x07,
    0xff, 0xf3, 0x1b, 0xef, 0xdf, 0x17, 0xdb, 0xcb, 0x13, 0xcb, 0xb7, 0x0f,
    0xbb, 0xa7, 0x0f, 0xab, 0x97, 0x0b, 0x9b, 0x83, 0x07, 0x8b, 0x73, 0x07,
    0x7b, 0x63, 0x07, 0x6b, 0x53, 0x00, 0x5b, 0x47, 0x00, 0x4b, 0x37, 0x00,
    0x3b, 0x2b, 0x00, 0x2b, 0x1f, 0x00, 0x1b, 0x0f, 0x00, 0x0b, 0x07, 0x00,
    0x00, 0x00, 0xff, 0x0b, 0x0b, 0xef, 0x13, 0x13, 0xdf, 0x1b, 0x1b, 0xcf,
    0x23, 0x23, 0xbf, 0x2b, 0x2b, 0xaf, 0x2f, 0x2f, 0x9f, 0x2f, 0x2f, 0x8f,
    0x2f, 0x2f, 0x7f, 0x2f, 0x2f, 0x6f, 0x2f, 0x2f, 0x5f, 0x2b, 0x2b, 0x4f,
    0x23, 0x23, 0x3f, 0x1b, 0x1b, 0x2f, 0x13, 0x13, 0x1f, 0x0b, 0x0b, 0x0f,
    0x2b, 0x00, 0x00, 0x3b, 0x00, 0x00, 0x4b, 0x07, 0x00, 0x5f, 0x07, 0x00,
    0x6f, 0x0f, 0x00, 0x7f, 0x17, 0x07, 0x93, 0x1f, 0x07, 0xa3, 0x27, 0x0b,
    0xb7, 0x33, 0x0f, 0xc3, 0x4b, 0x1b, 0xcf, 0x63, 0x2b, 0xdb, 0x7f, 0x3b,
    0xe3, 0x97, 0x4f, 0xe7, 0xab, 0x5f, 0xef, 0xbf, 0x77, 0xf7, 0xd3, 0x8b,
    0xa7, 0x7b, 0x3b, 0xb7, 0x9b, 0x37, 0xc7, 0xc3, 0x37, 0xe7, 0xe3, 0x57,
    0x7f, 0xbf, 0xff, 0xab, 0xe7, 0xff, 0xd7, 0xff, 0xff, 0x67, 0x00, 0x00,
    0x8b, 0x00, 0x00, 0xb3, 0x00, 0x00, 0xd7, 0x00, 0x00, 0xff, 0x00, 0x00,
    0xff, 0xf3, 0x93, 0xff, 0xf7, 0xc7, 0xff, 0xff, 0xff, 0x9f, 0x5b, 0x53,
};

const uint8_t *ResolveBspPalette(const uint8_t *palette768, const MipTexture &texture)
{
    if (palette768 != nullptr)
    {
        return palette768;
    }

    if (texture.palette.size() >= 768)
    {
        return texture.palette.data();
    }

    return kDefaultQuakePalette;
}

size_t NextMipTextureOffset(const std::vector<uint8_t> &bytes, const BspLump &lump, int32_t count,
                            int32_t currentOffset)
{
    size_t nextOffset = lump.length;

    for (int32_t j = 0; j < count; j++)
    {
        const int32_t otherOffset =
            ReadLittleEndian<int32_t>(bytes, lump.offset + 4 + size_t(j) * 4);

        if (otherOffset > currentOffset && size_t(otherOffset) < nextOffset)
        {
            nextOffset = size_t(otherOffset);
        }
    }

    return nextOffset;
}

void TryReadEmbeddedWad3Palette(const std::vector<uint8_t> &bytes, const BspLump &lump,
                                int32_t count, int32_t dataOffset, size_t headerSize,
                                MipTexture &texture)
{
    const size_t pixelCount = size_t(texture.width) * size_t(texture.height);
    const size_t nextOffset = NextMipTextureOffset(bytes, lump, count, dataOffset);
    const size_t entryStart = size_t(dataOffset) + headerSize;
    if (nextOffset <= entryStart)
    {
        return;
    }

    const size_t entryAvailable = nextOffset - entryStart;
    const size_t paletteOffset = pixelCount / 64 * 85;

    if (paletteOffset + 2 + 768 > entryAvailable)
    {
        return;
    }

    const size_t base = lump.offset + size_t(dataOffset);
    const uint16_t paletteSize = ReadLittleEndian<uint16_t>(bytes, base + headerSize + paletteOffset);

    if (paletteSize != 256)
    {
        return;
    }

    texture.palette.assign(bytes.data() + base + headerSize + paletteOffset + 2,
                           bytes.data() + base + headerSize + paletteOffset + 2 + 768);
}

// Quake stores its wall textures inside the BSP (lump 2): a miptex directory
// followed by an embedded 8-bit image with four mip levels and a palette.
bool DecodeBspTextures(const std::vector<uint8_t> &bytes, const BspLump &lump, size_t headerSize,
                       std::vector<MipTexture> &textures)
{
    textures.clear();

    if (lump.length < 4)
    {
        return true;
    }

    const int32_t count = ReadLittleEndian<int32_t>(bytes, lump.offset);

    if (count <= 0)
    {
        return true;
    }

    if (size_t(count) > (lump.length - 4) / 4)
    {
        return false;
    }

    textures.resize(size_t(count));

    for (int32_t i = 0; i < count; i++)
    {
        const int32_t dataOffset =
            ReadLittleEndian<int32_t>(bytes, lump.offset + 4 + size_t(i) * 4);

        if (dataOffset < 0 || size_t(dataOffset) + headerSize > lump.length)
        {
            continue;
        }

        const size_t base = lump.offset + size_t(dataOffset);
        MipTexture &texture = textures[size_t(i)];
        texture.name = MipTextureName(bytes.data() + base);
        texture.width = ReadLittleEndian<uint32_t>(bytes, base + 16);
        texture.height = ReadLittleEndian<uint32_t>(bytes, base + 20);

        if (texture.width == 0 || texture.height == 0 || texture.width > 4096 ||
            texture.height > 4096)
        {
            texture.width = 0;
            texture.height = 0;
            continue;
        }

        const size_t pixelCount = size_t(texture.width) * size_t(texture.height);
        const size_t available = lump.length - size_t(dataOffset) - headerSize;

        if (pixelCount > available)
        {
            texture.width = 0;
            texture.height = 0;
            continue;
        }

        texture.indices.assign(bytes.data() + base + headerSize,
                               bytes.data() + base + headerSize + pixelCount);

        TryReadEmbeddedWad3Palette(bytes, lump, count, dataOffset, headerSize, texture);
    }

    return true;
}

void ConvertIndexedToRgba(const MipTexture &texture, const uint8_t *palette768,
                          EditorTexture &out)
{
    out.name = texture.name;
    out.width = texture.width;
    out.height = texture.height;
    out.isExternal = false;
    out.rgba.assign(size_t(texture.width) * size_t(texture.height) * 4, 255);
    out.fullbright.clear();

    const uint8_t *palette = ResolveBspPalette(palette768, texture);
    const bool isFence = !texture.name.empty() && texture.name[0] == '{';

    for (size_t i = 0; i < texture.indices.size(); i++)
    {
        const uint8_t index = texture.indices[i];

        if (isFence && index == 255)
        {
            out.rgba[i * 4 + 0] = 0;
            out.rgba[i * 4 + 1] = 0;
            out.rgba[i * 4 + 2] = 0;
            out.rgba[i * 4 + 3] = 0;
            continue;
        }

        out.rgba[i * 4 + 0] = palette != nullptr ? palette[size_t(index) * 3 + 0] : index;
        out.rgba[i * 4 + 1] = palette != nullptr ? palette[size_t(index) * 3 + 1] : index;
        out.rgba[i * 4 + 2] = palette != nullptr ? palette[size_t(index) * 3 + 2] : index;
        out.rgba[i * 4 + 3] = 255;
    }
}

// Mirrors Sky_LoadTexture's skyflatcolor: a sky texture is a masked overlay whose front
// layer is the left half of every row, a zero index there is the transparent pixel, and
// the flat colour is the average of the opaque pixels over 255. The game derives the
// volumetric fog's ambient from this rather than from rt_sky_color, so a level whose sky
// is painted blue still gets the sky's own average -- warm grey for a Quake sky.
bool ComputeSkyFlatColor(const MipTexture &texture, const uint8_t *palette768, float color[3])
{
    const uint32_t halfWidth = texture.width / 2;

    if (halfWidth == 0 || texture.height == 0 || texture.indices.empty())
    {
        return false;
    }

    const uint8_t *palette = ResolveBspPalette(palette768, texture);

    uint64_t red = 0;
    uint64_t green = 0;
    uint64_t blue = 0;
    uint64_t count = 0;

    for (uint32_t y = 0; y < texture.height; y++)
    {
        const uint8_t *row = texture.indices.data() + size_t(y) * texture.width;

        for (uint32_t x = 0; x < halfWidth; x++)
        {
            const uint8_t index = row[x];

            if (index == 0)
            {
                continue;
            }

            red += palette[size_t(index) * 3 + 0];
            green += palette[size_t(index) * 3 + 1];
            blue += palette[size_t(index) * 3 + 2];
            count++;
        }
    }

    if (count == 0)
    {
        return false;
    }

    const double scale = 1.0 / (double(count) * 255.0);
    color[0] = float(double(red) * scale);
    color[1] = float(double(green) * scale);
    color[2] = float(double(blue) * scale);
    return true;
}

// The two layers of the classic scrolling sky, exactly as Sky_LoadTexture builds them: the
// back layer is the right half of every row and is opaque, the front layer is the left half
// and a zero index there is the transparent palette entry 255, which is carried here as
// alpha 0. The renderer samples both through the dome of Sky_DrawFace, so they keep the
// sky texture's own width/2 by height layout.
void BuildScrollingSkyLayers(const MipTexture &texture, const uint8_t *palette768,
                             EditorScene &scene)
{
    const uint32_t halfWidth = texture.width / 2;

    if (halfWidth == 0 || texture.height == 0 || texture.indices.empty() ||
        size_t(halfWidth) * size_t(texture.height) > texture.indices.size())
    {
        return;
    }

    const uint8_t *palette = ResolveBspPalette(palette768, texture);

    const size_t pixels = size_t(halfWidth) * size_t(texture.height);

    scene.skySolidRgba.assign(pixels * 4, 255);
    scene.skyAlphaRgba.assign(pixels * 4, 0);
    scene.skyLayerWidth = halfWidth;
    scene.skyLayerHeight = texture.height;
    scene.hasScrollingSky = true;

    for (uint32_t y = 0; y < texture.height; y++)
    {
        const uint8_t *row = texture.indices.data() + size_t(y) * texture.width;

        for (uint32_t x = 0; x < halfWidth; x++)
        {
            const size_t out = (size_t(y) * halfWidth + size_t(x)) * 4;
            const uint8_t back = row[halfWidth + x];
            const uint8_t front = row[x];

            scene.skySolidRgba[out + 0] = palette[size_t(back) * 3 + 0];
            scene.skySolidRgba[out + 1] = palette[size_t(back) * 3 + 1];
            scene.skySolidRgba[out + 2] = palette[size_t(back) * 3 + 2];
            scene.skySolidRgba[out + 3] = 255;

            if (front == 0)
            {
                scene.skyAlphaRgba[out + 0] = 0;
                scene.skyAlphaRgba[out + 1] = 0;
                scene.skyAlphaRgba[out + 2] = 0;
                scene.skyAlphaRgba[out + 3] = 0;
            }
            else
            {
                scene.skyAlphaRgba[out + 0] = palette[size_t(front) * 3 + 0];
                scene.skyAlphaRgba[out + 1] = palette[size_t(front) * 3 + 1];
                scene.skyAlphaRgba[out + 2] = palette[size_t(front) * 3 + 2];
                scene.skyAlphaRgba[out + 3] = 255;
            }
        }
    }
}

// Mirrors the TEX_SPECIAL handling of gl_model.c: the leading character of the
// texture name decides how the surface is lit and blended.
EditorSurfaceKind ClassifyTexture(const std::string &name)
{
    if (name.empty())
    {
        return EditorSurfaceKind::Opaque;
    }

    if (name[0] == '{')
    {
        return EditorSurfaceKind::Fence;
    }

    if (name[0] == '*')
    {
        if (name.find("lava") != std::string::npos)
        {
            return EditorSurfaceKind::Lava;
        }

        if (name.find("slime") != std::string::npos)
        {
            return EditorSurfaceKind::Slime;
        }

        if (name.find("tele") != std::string::npos)
        {
            return EditorSurfaceKind::Teleport;
        }

        return EditorSurfaceKind::Water;
    }

    if (name.compare(0, 3, "sky") == 0)
    {
        return EditorSurfaceKind::Sky;
    }

    return EditorSurfaceKind::Opaque;
}

// "+0floor" .. "+9floor" are the frames of one animated surface, so they are
// grouped under a single texture key.
std::string AnimationBaseName(const std::string &name)
{
    if (name.size() < 2 || name[0] != '+')
    {
        return name;
    }

    const char frame = name[1];

    if (std::isdigit(static_cast<unsigned char>(frame)) || (frame >= 'a' && frame <= 'j'))
    {
        return "+" + name.substr(2);
    }

    return name;
}

// Quake's leaf contents values, as stored in the BSP leaf lump.
constexpr int32_t kBspContentsSolid = -2;
constexpr int32_t kBspContentsWater = -3;
constexpr int32_t kBspContentsSlime = -4;
constexpr int32_t kBspContentsLava = -5;

// The distances RT_ResolveLightLeaf probes outward when a light sits in the solid leaf.
constexpr float kLightProbeDistances[3] = { 2.0f, 8.0f, 24.0f };

bool IsLiquidContents(int32_t contents)
{
    return contents == kBspContentsWater || contents == kBspContentsSlime ||
           contents == kBspContentsLava;
}

void ReadBspNodeChildren(const std::vector<uint8_t> &bytes, size_t offset, int32_t bsp2,
                         int32_t children[2])
{
    if (bsp2 != 0)
    {
        children[0] = ReadLittleEndian<int32_t>(bytes, offset + 4);
        children[1] = ReadLittleEndian<int32_t>(bytes, offset + 8);
    }
    else
    {
        children[0] = ReadLittleEndian<int16_t>(bytes, offset + 4);
        children[1] = ReadLittleEndian<int16_t>(bytes, offset + 6);
    }
}

void ReadBspLeafMinMax(const std::vector<uint8_t> &bytes, size_t offset, int32_t bsp2,
                       QrFloat3D &mins, QrFloat3D &maxs)
{
    for (int axis = 0; axis < 3; axis++)
    {
        if (bsp2 == 2)
        {
            mins.data[axis] = ReadLittleEndian<float>(bytes, offset + 8 + size_t(axis) * 4);
            maxs.data[axis] = ReadLittleEndian<float>(bytes, offset + 20 + size_t(axis) * 4);
        }
        else
        {
            mins.data[axis] = float(ReadLittleEndian<int16_t>(bytes, offset + 8 + size_t(axis) * 2));
            maxs.data[axis] = float(ReadLittleEndian<int16_t>(bytes, offset + 14 + size_t(axis) * 2));
        }
    }
}

bool ParseBspNodes(const std::vector<uint8_t> &bytes, const BspLump &nodeLump, size_t leafCount,
                   const std::vector<BspPlane> &planes, const BspFormat &format, EditorScene &scene,
                   std::string &error)
{
    const size_t nodeCount = nodeLump.length / format.nodeStride;
    scene.nodes.resize(nodeCount);

    for (size_t i = 0; i < nodeCount; i++)
    {
        const size_t offset = nodeLump.offset + i * format.nodeStride;
        const int32_t planeIndex = ReadLittleEndian<int32_t>(bytes, offset);

        if (planeIndex < 0 || size_t(planeIndex) >= planes.size())
        {
            error = "BSP node references an invalid plane";
            return false;
        }

        EditorBspNode &node = scene.nodes[i];
        node.normal[0] = planes[size_t(planeIndex)].normal[0];
        node.normal[1] = planes[size_t(planeIndex)].normal[1];
        node.normal[2] = planes[size_t(planeIndex)].normal[2];
        node.distance = planes[size_t(planeIndex)].distance;
        ReadBspNodeChildren(bytes, offset, format.bsp2, node.children);

        for (int child = 0; child < 2; child++)
        {
            if (node.children[child] >= 0)
            {
                if (size_t(node.children[child]) >= nodeCount)
                {
                    error = "BSP node child is out of range";
                    return false;
                }
            }
            else if (size_t(-int64_t(node.children[child]) - 1) >= leafCount)
            {
                error = "BSP node leaf is out of range";
                return false;
            }
        }
    }

    return true;
}

constexpr size_t kWorldClusterMax = 8192;
constexpr int32_t kWorldClusterCells = int32_t(kWorldClusterMax) - 1;

struct ParsedBspLeafTable
{
    std::vector<QrFloat3D> mins;
    std::vector<QrFloat3D> maxs;
    std::vector<int32_t> contents;
    std::vector<int32_t> visOffsets;
    QrFloat3D allMins = {};
    QrFloat3D allMaxs = {};
    QrFloat3D openMins = {};
    QrFloat3D openMaxs = {};
    bool hasOpenBounds = false;
    uint32_t visLeafs = 0;
    size_t worldLeafLimit = 0;
};

struct ClusterGridSpec
{
    float mins[3] = {};
    float cellSize[3] = {};
    float invCell[3] = {};
    int32_t dims[3] = { 1, 1, 1 };
    int32_t numCells = 1;
};

void ExpandBounds(const QrFloat3D &minIn, const QrFloat3D &maxIn, QrFloat3D &minOut,
                  QrFloat3D &maxOut, bool &haveBounds)
{
    for (int axis = 0; axis < 3; axis++)
    {
        minOut.data[axis] =
            haveBounds ? std::min(minOut.data[axis], minIn.data[axis]) : minIn.data[axis];
        maxOut.data[axis] =
            haveBounds ? std::max(maxOut.data[axis], maxIn.data[axis]) : maxIn.data[axis];
    }

    haveBounds = true;
}

uint8_t ClusterFlagsForContents(int32_t contents)
{
    return uint8_t((contents == kBspContentsSolid ? QR_WORLD_CLUSTER_SOLID_BIT : 0) |
                   (IsLiquidContents(contents) ? QR_WORLD_CLUSTER_LIQUID_BIT : 0));
}

void ReadBspLeafTable(const std::vector<uint8_t> &bytes, const BspLump lumps[15], size_t leafCount,
                      const BspFormat &format, ParsedBspLeafTable &out)
{
    const BspLump &visLump = lumps[4];
    const BspLump &leafLump = lumps[10];
    const BspLump &modelLump = lumps[14];

    out.mins.resize(leafCount);
    out.maxs.resize(leafCount);
    out.contents.assign(leafCount, kBspContentsSolid);
    out.visOffsets.assign(leafCount, -1);
    out.visLeafs = uint32_t(leafCount);

    if (LumpFits(modelLump, bytes.size(), 64) && modelLump.length >= 64)
    {
        const int32_t worldVisLeafs = ReadLittleEndian<int32_t>(bytes, modelLump.offset + 52);

        if (worldVisLeafs > 0 && uint32_t(worldVisLeafs) < out.visLeafs)
        {
            out.visLeafs = uint32_t(worldVisLeafs);
        }
    }

    out.worldLeafLimit = std::min<size_t>(leafCount, size_t(out.visLeafs) + 1u);

    bool haveAllBounds = false;

    for (size_t i = 1; i < leafCount; i++)
    {
        const size_t offset = leafLump.offset + i * format.leafStride;
        const int32_t contents = ReadLittleEndian<int32_t>(bytes, offset);

        ReadBspLeafMinMax(bytes, offset, format.bsp2, out.mins[i], out.maxs[i]);
        out.contents[i] = contents;
        ExpandBounds(out.mins[i], out.maxs[i], out.allMins, out.allMaxs, haveAllBounds);

        if (i < out.worldLeafLimit && contents != kBspContentsSolid)
        {
            ExpandBounds(out.mins[i], out.maxs[i], out.openMins, out.openMaxs, out.hasOpenBounds);
        }

        if (visLump.length > 0 && i < out.worldLeafLimit)
        {
            const int32_t visOfs = ReadLittleEndian<int32_t>(bytes, offset + 4);

            if (visOfs >= 0 && size_t(visOfs) < visLump.length)
            {
                out.visOffsets[i] = visOfs;
            }
        }
    }
}

void BuildWorldClustersIdentity(const std::vector<uint8_t> &bytes, const BspLump &visLump,
                                const ParsedBspLeafTable &table, EditorWorldClusters &clusters)
{
    const size_t leafCount = table.contents.size();

    clusters.numLeafs = uint32_t(leafCount);
    clusters.mins.resize(leafCount + 1);
    clusters.maxs.resize(leafCount + 1);
    clusters.flags.assign(leafCount + 1, QR_WORLD_CLUSTER_SOLID_BIT);
    clusters.contents = table.contents;
    clusters.leafCluster.assign(leafCount, 0);
    clusters.visData.clear();
    clusters.visOffsets.clear();
    clusters.pvsRowBytes = 0;

    for (size_t i = 1; i < leafCount; i++)
    {
        clusters.mins[i] = table.mins[i];
        clusters.maxs[i] = table.maxs[i];
        clusters.flags[i] = ClusterFlagsForContents(table.contents[i]);
        clusters.leafCluster[i] = uint32_t(i);
    }

    // Cluster 0 is the hole: the whole map, solid, and the answer for any leaf the tree cannot
    // reach. The edge cluster is the same. Neither one is a light's home.
    for (uint32_t cluster : { uint32_t(0), uint32_t(leafCount) })
    {
        clusters.mins[cluster] = table.allMins;
        clusters.maxs[cluster] = table.allMaxs;
        clusters.flags[cluster] = QR_WORLD_CLUSTER_SOLID_BIT;
    }

    if (visLump.length == 0 || leafCount <= 1)
    {
        return;
    }

    const uint32_t rowBytes = (table.visLeafs + 7u) / 8u;
    clusters.pvsRowBytes = rowBytes;
    clusters.visData.assign(visLump.length + size_t(rowBytes) + 2u, 0xFF);
    std::memcpy(clusters.visData.data(), bytes.data() + visLump.offset, visLump.length);
    clusters.visOffsets.assign(clusters.mins.size(), -1);

    for (size_t i = 1; i < table.worldLeafLimit; i++)
    {
        clusters.visOffsets[i] = table.visOffsets[i];
    }
}

int64_t GridCellsAt(const float ext[3], float scale, int32_t dims[3])
{
    int64_t cells = 1;

    for (int axis = 0; axis < 3; axis++)
    {
        int32_t n = int32_t(std::floor(ext[axis] * scale));
        n = std::clamp<int32_t>(n, 1, kWorldClusterCells);
        dims[axis] = n;
        cells *= n;
    }

    return cells;
}

int32_t GridCellOfPoint(const float point[3], const ClusterGridSpec &grid)
{
    int32_t cell = 0;

    for (int axis = 0; axis < 3; axis++)
    {
        float f = (point[axis] - grid.mins[axis]) * grid.invCell[axis];

        if (!(f > 0.0f))
        {
            f = 0.0f;
        }
        else if (f >= float(grid.dims[axis]))
        {
            f = float(grid.dims[axis]) - 1.0f;
        }

        cell = cell * grid.dims[axis] + int32_t(f);
    }

    return cell;
}

void GridCellBounds(int32_t cell, const ClusterGridSpec &grid, QrFloat3D &outMin,
                    QrFloat3D &outMax)
{
    const int32_t dimY = grid.dims[1];
    const int32_t dimZ = grid.dims[2];
    const int32_t idx[3] = { cell / (dimY * dimZ), (cell / dimZ) % dimY, cell % dimZ };

    for (int axis = 0; axis < 3; axis++)
    {
        outMin.data[axis] = grid.mins[axis] + float(idx[axis]) * grid.cellSize[axis];
        outMax.data[axis] = outMin.data[axis] + grid.cellSize[axis];
    }
}

void BuildClusterGridSpec(const ParsedBspLeafTable &table, ClusterGridSpec &grid)
{
    const QrFloat3D &boundsMin = table.hasOpenBounds ? table.openMins : table.allMins;
    const QrFloat3D &boundsMax = table.hasOpenBounds ? table.openMaxs : table.allMaxs;
    float ext[3] = {};

    for (int axis = 0; axis < 3; axis++)
    {
        ext[axis] = boundsMax.data[axis] - boundsMin.data[axis];
    }

    const double volume = double(ext[0]) * double(ext[1]) * double(ext[2]);
    float scale = 1.0f;

    if (volume > 1.0)
    {
        scale = float(std::pow(double(kWorldClusterCells) / volume, 1.0 / 3.0));
    }

    if (!(scale > 0.0f) || scale > 1.0e4f)
    {
        scale = 1.0f;
    }

    int64_t numCells = GridCellsAt(ext, scale, grid.dims);

    while (numCells > kWorldClusterCells && scale > 0.0f)
    {
        scale *= 0.9f;
        numCells = GridCellsAt(ext, scale, grid.dims);
    }

    grid.numCells = std::max<int32_t>(1, int32_t(numCells));

    for (int axis = 0; axis < 3; axis++)
    {
        grid.mins[axis] = boundsMin.data[axis];
        grid.cellSize[axis] = ext[axis] / float(grid.dims[axis]);
        grid.invCell[axis] = ext[axis] > 0.0f ? float(grid.dims[axis]) / ext[axis] : 0.0f;
    }
}

void ProjectVisBits(uint8_t bits, uint32_t byteIndex, uint32_t visLeafs,
                    const std::vector<uint32_t> &leafCluster, std::vector<uint8_t> &outRow)
{
    for (uint32_t k = 0; k < 8u; k++)
    {
        const uint32_t leaf = (byteIndex << 3u) + k + 1u;

        if ((bits & (1u << k)) == 0 || leaf > visLeafs || size_t(leaf) >= leafCluster.size())
        {
            continue;
        }

        const uint32_t targetCluster = leafCluster[leaf];

        if (targetCluster == 0)
        {
            continue;
        }

        const uint32_t bit = targetCluster - 1u;

        if ((bit >> 3u) < outRow.size())
        {
            outRow[bit >> 3u] |= uint8_t(1u << (bit & 7u));
        }
    }
}

void ProjectVisRow(const uint8_t *compressed, size_t avail, uint32_t leafRowBytes,
                   uint32_t visLeafs, const std::vector<uint32_t> &leafCluster,
                   std::vector<uint8_t> &outRow)
{
    const size_t inLimit = std::min(avail, size_t(leafRowBytes) * 2u);
    size_t in = 0;

    for (uint32_t b = 0; b < leafRowBytes && in < inLimit;)
    {
        const uint8_t bits = compressed[in++];

        if (bits != 0)
        {
            ProjectVisBits(bits, b, visLeafs, leafCluster, outRow);
            b++;
            continue;
        }

        if (in >= inLimit)
        {
            return;
        }

        uint32_t run = compressed[in++];

        if (run > leafRowBytes - b)
        {
            run = leafRowBytes - b;
        }

        b += run;
    }
}

void AppendCompressedVisRow(const std::vector<uint8_t> &row, std::vector<uint8_t> &visData)
{
    for (size_t i = 0; i < row.size();)
    {
        if (row[i] != 0)
        {
            visData.push_back(row[i++]);
            continue;
        }

        size_t run = 0;

        while (i + run < row.size() && run < 255u && row[i + run] == 0)
        {
            run++;
        }

        visData.push_back(0);
        visData.push_back(uint8_t(run));
        i += run;
    }
}

void PopulateGridCells(const ParsedBspLeafTable &table, const ClusterGridSpec &grid,
                       EditorWorldClusters &clusters, std::vector<int32_t> &cellLeafStart,
                       std::vector<uint32_t> &cellMembers)
{
    const size_t leafCount = table.contents.size();
    const size_t numClusters = size_t(grid.numCells) + 1u;
    const QrFloat3D &boundsMin = table.hasOpenBounds ? table.openMins : table.allMins;
    const QrFloat3D &boundsMax = table.hasOpenBounds ? table.openMaxs : table.allMaxs;

    clusters.numLeafs = uint32_t(leafCount);
    clusters.contents = table.contents;
    clusters.leafCluster.assign(leafCount, 0);
    clusters.mins.assign(numClusters, boundsMax);
    clusters.maxs.assign(numClusters, boundsMin);
    clusters.flags.assign(numClusters, QR_WORLD_CLUSTER_SOLID_BIT);

    std::vector<int32_t> cellCounts(size_t(grid.numCells), 0);

    for (size_t i = 1; i < leafCount; i++)
    {
        const float center[3] = {
            0.5f * (table.mins[i].data[0] + table.maxs[i].data[0]),
            0.5f * (table.mins[i].data[1] + table.maxs[i].data[1]),
            0.5f * (table.mins[i].data[2] + table.maxs[i].data[2]),
        };
        const int32_t cell = GridCellOfPoint(center, grid);
        const size_t cluster = size_t(cell) + 1u;
        clusters.leafCluster[i] = uint32_t(cluster);

        if (i >= table.worldLeafLimit || table.contents[i] == kBspContentsSolid)
        {
            continue;
        }

        clusters.flags[cluster] &= uint8_t(~QR_WORLD_CLUSTER_SOLID_BIT);

        if (IsLiquidContents(table.contents[i]))
        {
            clusters.flags[cluster] |= QR_WORLD_CLUSTER_LIQUID_BIT;
        }

        bool haveCellBounds = cellCounts[size_t(cell)] > 0;
        ExpandBounds(table.mins[i], table.maxs[i], clusters.mins[cluster], clusters.maxs[cluster],
                     haveCellBounds);
        cellCounts[size_t(cell)]++;
    }

    cellLeafStart.assign(size_t(grid.numCells) + 1u, 0);
    int32_t totalMembers = 0;

    for (int32_t cell = 0; cell < grid.numCells; cell++)
    {
        cellLeafStart[size_t(cell)] = totalMembers;
        totalMembers += cellCounts[size_t(cell)];
    }

    cellLeafStart[size_t(grid.numCells)] = totalMembers;
    cellMembers.assign(size_t(totalMembers), 0);
    std::vector<int32_t> filled(size_t(grid.numCells), 0);

    for (size_t i = 1; i < table.worldLeafLimit; i++)
    {
        if (table.contents[i] == kBspContentsSolid)
        {
            continue;
        }

        const size_t cell = size_t(clusters.leafCluster[i] - 1u);
        const size_t slot = size_t(cellLeafStart[cell]) + size_t(filled[cell]);
        cellMembers[slot] = uint32_t(i);
        filled[cell]++;
    }

    for (int32_t cell = 0; cell < grid.numCells; cell++)
    {
        if (cellCounts[size_t(cell)] == 0)
        {
            GridCellBounds(cell, grid, clusters.mins[size_t(cell) + 1u],
                           clusters.maxs[size_t(cell) + 1u]);
        }
    }

    clusters.mins[0] = boundsMin;
    clusters.maxs[0] = boundsMax;
    clusters.flags[0] = QR_WORLD_CLUSTER_SOLID_BIT;
}

void BuildGridVisibility(const std::vector<uint8_t> &bytes, const BspLump &visLump,
                         const ParsedBspLeafTable &table, int32_t numCells,
                         const std::vector<int32_t> &cellLeafStart,
                         const std::vector<uint32_t> &cellMembers, EditorWorldClusters &clusters)
{
    clusters.visData.clear();
    clusters.visOffsets.clear();
    clusters.pvsRowBytes = 0;

    if (visLump.length == 0 || numCells <= 0)
    {
        return;
    }

    const uint32_t numClusters = uint32_t(clusters.mins.size());
    const uint32_t clusterRowBytes = (numClusters + 7u) / 8u;
    const uint32_t leafRowBytes = (table.visLeafs + 7u) / 8u;

    clusters.pvsRowBytes = clusterRowBytes;
    clusters.visOffsets.assign(numClusters, -1);

    std::vector<uint8_t> row(clusterRowBytes, 0);

    for (int32_t cell = 0; cell < numCells; cell++)
    {
        const int32_t start = cellLeafStart[size_t(cell)];
        const int32_t end = cellLeafStart[size_t(cell) + 1u];

        if (start >= end)
        {
            continue;
        }

        std::fill(row.begin(), row.end(), 0);
        bool haveRow = false;

        for (int32_t idx = start; idx < end; idx++)
        {
            const int32_t visOfs = table.visOffsets[cellMembers[size_t(idx)]];

            if (visOfs < 0 || size_t(visOfs) >= visLump.length)
            {
                continue;
            }

            ProjectVisRow(bytes.data() + visLump.offset + size_t(visOfs),
                          visLump.length - size_t(visOfs), leafRowBytes, table.visLeafs,
                          clusters.leafCluster, row);
            haveRow = true;
        }

        if (haveRow)
        {
            clusters.visOffsets[size_t(cell) + 1u] = int32_t(clusters.visData.size());
            AppendCompressedVisRow(row, clusters.visData);
        }
    }

    clusters.visData.resize(clusters.visData.size() + size_t(clusterRowBytes) + 2u, 0xFF);
}

void BuildWorldClustersGrid(const std::vector<uint8_t> &bytes, const BspLump &visLump,
                            const ParsedBspLeafTable &table, EditorWorldClusters &clusters)
{
    ClusterGridSpec grid;
    BuildClusterGridSpec(table, grid);

    std::vector<int32_t> cellLeafStart;
    std::vector<uint32_t> cellMembers;
    PopulateGridCells(table, grid, clusters, cellLeafStart, cellMembers);
    BuildGridVisibility(bytes, visLump, table, grid.numCells, cellLeafStart, cellMembers, clusters);
}

bool ParseBspTreeAndVis(const std::vector<uint8_t> &bytes, const BspLump lumps[15],
                        const std::vector<BspPlane> &planes, const BspFormat &format,
                        EditorScene &scene, std::string &error)
{
    const size_t leafCount = lumps[10].length / format.leafStride;

    if (leafCount == 0)
    {
        error = "BSP has no leafs";
        return false;
    }

    if (!ParseBspNodes(bytes, lumps[5], leafCount, planes, format, scene, error))
    {
        return false;
    }

    ParsedBspLeafTable table;
    ReadBspLeafTable(bytes, lumps, leafCount, format, table);

    if (leafCount < kWorldClusterMax)
    {
        BuildWorldClustersIdentity(bytes, lumps[4], table, scene.worldClusters);
    }
    else
    {
        BuildWorldClustersGrid(bytes, lumps[4], table, scene.worldClusters);
    }

    return true;
}

uint32_t MapWorldCluster(const EditorWorldClusters &clusters, int32_t leaf)
{
    if (leaf <= 0 || size_t(leaf) >= clusters.leafCluster.size())
    {
        return 0;
    }

    return clusters.leafCluster[size_t(leaf)];
}

// Mod_PointInLeaf: walk from the tree's first node until a leaf.
int32_t LeafAtPoint(const EditorScene &scene, const float point[3])
{
    if (scene.nodes.empty())
    {
        return -1;
    }

    int32_t nodeIndex = 0;

    for (size_t step = 0; step <= scene.nodes.size(); step++)
    {
        if (nodeIndex < 0 || size_t(nodeIndex) >= scene.nodes.size())
        {
            return -1;
        }

        const EditorBspNode &node = scene.nodes[size_t(nodeIndex)];
        const float distance = point[0] * node.normal[0] + point[1] * node.normal[1] +
                               point[2] * node.normal[2] - node.distance;
        const int32_t child = distance > 0.0f ? node.children[0] : node.children[1];

        if (child >= 0)
        {
            nodeIndex = child;
            continue;
        }

        const int32_t leaf = -(child + 1);
        return uint32_t(leaf) < scene.worldClusters.numLeafs ? leaf : -1;
    }

    return -1;
}

// RT_ResolveLightLeaf: the light's own leaf, and when that one is solid or the solid leaf a
// probe outward at 2, 8 and 24 units, because a light modelled inside a wall still has to
// reach the room it lights.
uint32_t ResolveLightCluster(const EditorScene &scene, const float origin[3])
{
    const EditorWorldClusters &clusters = scene.worldClusters;

    if (scene.nodes.empty() || clusters.numLeafs == 0)
    {
        return QR_CLUSTER_LIGHT_NO_CLUSTER;
    }

    auto usableLeaf = [&clusters](int32_t leaf) {
        return leaf > 0 && uint32_t(leaf) < clusters.numLeafs &&
               (size_t(leaf) >= clusters.contents.size() ||
                clusters.contents[size_t(leaf)] != kBspContentsSolid);
    };

    const int32_t centerLeaf = LeafAtPoint(scene, origin);
    if (usableLeaf(centerLeaf))
    {
        return MapWorldCluster(clusters, centerLeaf);
    }

    static const float probeDirections[6][3] = {
        { 1.0f, 0.0f, 0.0f },  { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },  { 0.0f, 0.0f, -1.0f },
    };

    for (float probeDistance : kLightProbeDistances)
    {
        for (const auto &direction : probeDirections)
        {
            const float probe[3] = {
                origin[0] + probeDistance * direction[0],
                origin[1] + probeDistance * direction[1],
                origin[2] + probeDistance * direction[2],
            };
            const int32_t leaf = LeafAtPoint(scene, probe);

            if (usableLeaf(leaf))
            {
                return MapWorldCluster(clusters, leaf);
            }
        }
    }

    return QR_CLUSTER_LIGHT_NO_CLUSTER;
}

// RT_SurfaceCentroidLeaf: a surface belongs to the leaf just outside its own face, probed at
// 0.01 and then 1 unit along the normal for the faces whose centroid lands in the wall. The
// renderer indexes the light lists with the cluster of a shading point, so every vertex of the
// surface carries it. A face that finds no open leaf falls back to cluster 0, the hole the
// tables keep for solid geometry.
uint32_t ResolveSurfaceCluster(const EditorScene &scene, const float centroid[3],
                               const float normal[3])
{
    if (scene.nodes.empty() || scene.worldClusters.numLeafs == 0)
    {
        return 0;
    }

    static const float kProbeOffsets[2] = { 0.01f, 1.0f };

    for (float offset : kProbeOffsets)
    {
        const float point[3] = {
            centroid[0] + offset * normal[0],
            centroid[1] + offset * normal[1],
            centroid[2] + offset * normal[2],
        };
        const int32_t leaf = LeafAtPoint(scene, point);

        if (leaf > 0 && uint32_t(leaf) < scene.worldClusters.numLeafs &&
            (size_t(leaf) >= scene.worldClusters.contents.size() ||
             scene.worldClusters.contents[size_t(leaf)] != kBspContentsSolid))
        {
            return MapWorldCluster(scene.worldClusters, leaf);
        }
    }

    return 0;
}

constexpr int kSpawnflagNotEasy = 256;
constexpr int kSpawnflagNotMedium = 512;
constexpr int kSpawnflagNotHard = 1024;
constexpr int kSpawnflagNotDeathmatch = 2048;

bool IsBspEntityInhibited(const BspEntity &entity, bool multiplayer)
{
    const std::string &className = entity.Value("classname");

    if (className == "worldspawn")
    {
        return false;
    }

    const int spawnFlags = std::atoi(entity.Value("spawnflags").c_str());
    (void)kSpawnflagNotEasy;
    (void)kSpawnflagNotHard;

    if (multiplayer)
    {
        return (spawnFlags & kSpawnflagNotDeathmatch) != 0 ||
               className.compare(0, 8, "monster_") == 0;
    }

    return (spawnFlags & kSpawnflagNotMedium) != 0 || className == "func_episodegate";
}

void MarkBspModelFaces(const std::vector<uint8_t> &bytes, const BspLump &modelLump,
                       size_t modelIndex, uint8_t value, std::vector<uint8_t> &hidden)
{
    const size_t modelOffset = modelLump.offset + modelIndex * 64;
    const int32_t firstFace = ReadLittleEndian<int32_t>(bytes, modelOffset + 56);
    const int32_t numFaces = ReadLittleEndian<int32_t>(bytes, modelOffset + 60);

    if (numFaces <= 0)
    {
        return;
    }

    const int64_t startFace = std::max<int64_t>(firstFace, 0);
    const int64_t endFace = std::min<int64_t>(int64_t(firstFace) + numFaces, int64_t(hidden.size()));

    for (int64_t face = startFace; face < endFace; face++)
    {
        hidden[size_t(face)] = value;
    }
}

bool IsHiddenBspFaceTexture(const std::string &name)
{
    return name == "trigger" || name == "clip" || name == "skip" || name == "hint";
}

// Hides all inline submodels (`*1..*N`) from the world face pass so `AppendInlineBspSubmodels`
// can filter and transform non-hidden brush entities to their spawn positions.
std::vector<uint8_t> BuildHiddenBspFaceMask(const std::vector<uint8_t> &bytes,
                                            const BspLump &modelLump, size_t faceCount)
{
    std::vector<uint8_t> hidden(faceCount, 0);

    if (!LumpFits(modelLump, bytes.size(), 64))
    {
        return hidden;
    }

    const size_t modelCount = modelLump.length / 64;

    for (size_t modelIndex = 1; modelIndex < modelCount; modelIndex++)
    {
        MarkBspModelFaces(bytes, modelLump, modelIndex, 1, hidden);
    }

    return hidden;
}

bool ResolveBspFaceTexture(const BspTexInfo *texInfo, const std::vector<EditorTexture> &textures,
                           const std::vector<MipTexture> &mipTextures, int32_t &outTextureIndex,
                           std::string &outTextureName)
{
    outTextureIndex = -1;
    outTextureName = "notex";

    if (texInfo != nullptr && texInfo->miptex >= 0)
    {
        const size_t miptex = size_t(texInfo->miptex);

        if (miptex < textures.size() && textures[miptex].width > 0)
        {
            outTextureIndex = texInfo->miptex;
            outTextureName = textures[miptex].name;
        }
        else if (miptex < mipTextures.size() && !mipTextures[miptex].name.empty())
        {
            outTextureName = mipTextures[miptex].name;
        }
    }

    return !IsHiddenBspFaceTexture(outTextureName);
}

// Matches BuildSurfaceDisplayList in Quake/r_brush.c: normal BSP surfaces divide by the miptex's
// own pixel dimensions, while liquid/teleport warp surfaces omit texInfo offsets and divide by 128.
QrFloat2D ComputeBspTexCoord(const float position[3], const BspTexInfo *texInfo,
                             EditorSurfaceKind surfaceKind,
                             const std::vector<EditorTexture> &textures, int32_t textureIndex)
{
    if (texInfo == nullptr)
    {
        return { { 0.0f, 0.0f } };
    }

    const bool isWarp = surfaceKind == EditorSurfaceKind::Water ||
                        surfaceKind == EditorSurfaceKind::Slime ||
                        surfaceKind == EditorSurfaceKind::Lava ||
                        surfaceKind == EditorSurfaceKind::Teleport;

    if (isWarp)
    {
        const float u = position[0] * texInfo->u[0] + position[1] * texInfo->u[1] +
                        position[2] * texInfo->u[2];
        const float v = position[0] * texInfo->v[0] + position[1] * texInfo->v[1] +
                        position[2] * texInfo->v[2];
        return { { u / 128.0f, v / 128.0f } };
    }

    float width = 64.0f;
    float height = 64.0f;

    if (textureIndex >= 0 && size_t(textureIndex) < textures.size())
    {
        const EditorTexture &texture = textures[size_t(textureIndex)];

        if (texture.width > 0 && texture.height > 0)
        {
            width = float(texture.width);
            height = float(texture.height);
        }
    }

    return { { Dot(position, texInfo->u) / width, Dot(position, texInfo->v) / height } };
}

// Attaches all sibling animation frame textures (+0..+9, +a..+j) from the BSP texture
// lump onto each matching animated surface so Material Editor sees every frame.
void AttachAnimationFrameTextures(const EditorScene &scene,
                                  const std::unordered_map<std::string, size_t> &surfaceByTexture,
                                  std::vector<EditorSurface> &surfaces)
{
    for (size_t i = 0; i < scene.textures.size(); i++)
    {
        const EditorTexture &tex = scene.textures[i];

        if (tex.width == 0 || tex.height == 0 || tex.name.size() < 2 || tex.name[0] != '+')
        {
            continue;
        }

        const auto found = surfaceByTexture.find(AnimationBaseName(tex.name));

        if (found == surfaceByTexture.end())
        {
            continue;
        }

        std::vector<int32_t> &frames = surfaces[found->second].animationFrames;

        if (std::find(frames.begin(), frames.end(), int32_t(i)) == frames.end())
        {
            frames.push_back(int32_t(i));
        }
    }

    for (EditorSurface &surface : surfaces)
    {
        if (surface.animationFrames.size() <= 1)
        {
            continue;
        }

        std::sort(surface.animationFrames.begin(), surface.animationFrames.end(),
                  [&scene](int32_t a, int32_t b) {
                      return scene.textures[size_t(a)].name < scene.textures[size_t(b)].name;
                  });
        surface.texture = surface.animationFrames.front();
    }
}

struct BspGeometryTables
{
    BspFormat format;
    std::vector<BspPlane> planes;
    std::vector<float> vertices;
    std::vector<BspTexInfo> texInfos;
    std::vector<BspFace> faces;
    std::vector<BspEdge> edges;
    std::vector<int32_t> surfEdges;
    std::vector<uint8_t> hiddenFaces;
    std::vector<MipTexture> mipTextures;
};

bool ValidateBspHeaderAndLumps(const std::vector<uint8_t> &bytes, const std::string &sourceName,
                               BspLump lumps[15], BspFormat &format, std::string &error)
{
    if (bytes.size() < 124)
    {
        error = "invalid BSP header in " + sourceName;
        return false;
    }

    const int32_t version = ReadLittleEndian<int32_t>(bytes, 0);
    if (!DetectBspFormat(version, format))
    {
        error = "unsupported BSP version in " + sourceName + " (supported: BSP29, 2PSB, BSP2, Q64)";
        return false;
    }

    for (int i = 0; i < 15; i++)
    {
        lumps[i].offset = ReadLittleEndian<uint32_t>(bytes, 4 + size_t(i) * 8);
        lumps[i].length = ReadLittleEndian<uint32_t>(bytes, 8 + size_t(i) * 8);

        if (lumps[i].offset > bytes.size() || lumps[i].length > bytes.size() - lumps[i].offset)
        {
            error = "BSP lump outside the file in " + sourceName;
            return false;
        }
    }

    if (!LumpFits(lumps[1], bytes.size(), 20) || !LumpFits(lumps[3], bytes.size(), 12) ||
        !LumpFits(lumps[5], bytes.size(), format.nodeStride) ||
        !LumpFits(lumps[6], bytes.size(), 40) ||
        !LumpFits(lumps[7], bytes.size(), format.faceStride) ||
        !LumpFits(lumps[10], bytes.size(), format.leafStride) ||
        !LumpFits(lumps[12], bytes.size(), format.edgeStride) ||
        !LumpFits(lumps[13], bytes.size(), 4))
    {
        error = "malformed BSP lump sizes in " + sourceName;
        return false;
    }

    return true;
}

bool IsStaticFlameOrTorchClass(const std::string &className)
{
    return className == "light_torch_small_walltorch" ||
           className == "light_flame_large_yellow" ||
           className == "light_flame_small_yellow" ||
           className == "light_flame_small_white" ||
           className == "light_candle" ||
           className == "light_flame_gas" ||
           className == "light_lantern";
}

std::string ExtractWorldspawnSkyboxName(const std::vector<BspEntity> &entities)
{
    static constexpr const char *kSkyboxKeys[] = {
        "sky", "_sky", "skyname", "_skyname", "qlsky", "_qlsky",
    };

    for (const BspEntity &entity : entities)
    {
        if (entity.Value("classname") != "worldspawn")
        {
            continue;
        }

        for (const char *key : kSkyboxKeys)
        {
            const std::string &value = entity.Value(key);
            if (!value.empty())
            {
                return value;
            }
        }

        break;
    }

    return {};
}

void PopulateBspSpawnAndLights(const std::vector<BspEntity> &entities, bool multiplayer,
                               EditorScene &scene)
{
    scene.skyboxName = ExtractWorldspawnSkyboxName(entities);
    static constexpr const char *kSpOrder[3] = {
        "info_player_start", "info_player_coop", "info_player_deathmatch",
    };
    static constexpr const char *kMpOrder[3] = {
        "info_player_deathmatch", "info_player_start", "info_player_coop",
    };
    const char *const *spawnOrder = multiplayer ? kMpOrder : kSpOrder;
    const BspEntity *spawn = nullptr;

    for (size_t i = 0; i < 3 && spawn == nullptr; i++)
    {
        for (const BspEntity &entity : entities)
        {
            if (entity.Value("classname") == spawnOrder[i])
            {
                spawn = &entity;
                break;
            }
        }
    }

    if (spawn != nullptr)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (std::sscanf(spawn->Value("origin").c_str(), "%f %f %f", &x, &y, &z) == 3)
        {
            scene.cameraPosition[0] = x;
            scene.cameraPosition[1] = y;
            scene.cameraPosition[2] = z + 22.0f;
            scene.cameraYawDegrees = float(std::atof(spawn->Value("angle").c_str()));
            scene.hasCamera = true;
        }
    }

    for (const BspEntity &entity : entities)
    {
        const std::string &className = entity.Value("classname");
        if (!StartsWithLight(className) || IsBspEntityInhibited(entity, multiplayer))
        {
            continue;
        }

        EditorLight light;
        if (std::sscanf(entity.Value("origin").c_str(), "%f %f %f", &light.position[0],
                        &light.position[1], &light.position[2]) != 3)
        {
            continue;
        }

        if (IsStaticFlameOrTorchClass(className))
        {
            light.position[2] += 0.75f * kEditorUnitsPerMeter;
        }

        const std::string &intensityText = entity.Value("light");
        const float intensity = intensityText.empty() ? kDefaultLightIntensity
                                                      : float(std::atof(intensityText.c_str()));
        light.radius = kDefaultLightRadiusMeters * kEditorUnitsPerMeter;
        light.cluster = ResolveLightCluster(scene, light.position);
        light.className = className;

        const std::string &styleText = entity.Value("style");
        if (!styleText.empty())
        {
            light.style = std::atoi(styleText.c_str());
        }

        SetEditorLightIntensity(light, intensity);
        scene.lights.push_back(std::move(light));
    }

    scene.bspLights = scene.lights;
}

void ReadBspFaceEntry(const std::vector<uint8_t> &bytes, size_t offset, int32_t bsp2, BspFace &face)
{
    if (bsp2 != 0)
    {
        face.plane = ReadLittleEndian<int32_t>(bytes, offset);
        face.side = ReadLittleEndian<int32_t>(bytes, offset + 4);
        face.firstEdge = ReadLittleEndian<int32_t>(bytes, offset + 8);
        face.edgeCount = ReadLittleEndian<int32_t>(bytes, offset + 12);
        face.texInfo = ReadLittleEndian<int32_t>(bytes, offset + 16);
    }
    else
    {
        face.plane = ReadLittleEndian<int16_t>(bytes, offset);
        face.side = ReadLittleEndian<int16_t>(bytes, offset + 2);
        face.firstEdge = ReadLittleEndian<int32_t>(bytes, offset + 4);
        face.edgeCount = ReadLittleEndian<int16_t>(bytes, offset + 8);
        face.texInfo = ReadLittleEndian<int16_t>(bytes, offset + 10);
    }
}

void ReadBspEdgeEntry(const std::vector<uint8_t> &bytes, size_t offset, int32_t bsp2, BspEdge &edge)
{
    if (bsp2 != 0)
    {
        edge.vertex[0] = ReadLittleEndian<uint32_t>(bytes, offset);
        edge.vertex[1] = ReadLittleEndian<uint32_t>(bytes, offset + 4);
    }
    else
    {
        edge.vertex[0] = ReadLittleEndian<uint16_t>(bytes, offset);
        edge.vertex[1] = ReadLittleEndian<uint16_t>(bytes, offset + 2);
    }
}

bool ReadBspGeometryTables(const std::vector<uint8_t> &bytes, const BspLump lumps[15],
                           const std::string &sourceName, BspGeometryTables &tables,
                           std::string &error)
{
    const size_t vertexCount = lumps[3].length / 12;
    const size_t texInfoCount = lumps[6].length / 40;
    const size_t faceCount = lumps[7].length / tables.format.faceStride;
    const size_t edgeCount = lumps[12].length / tables.format.edgeStride;
    const size_t surfEdgeCount = lumps[13].length / 4;

    tables.vertices.resize(vertexCount * 3);
    tables.texInfos.resize(texInfoCount);
    tables.faces.resize(faceCount);
    tables.edges.resize(edgeCount);
    tables.surfEdges.resize(surfEdgeCount);

    for (size_t i = 0; i < vertexCount; i++)
    {
        const size_t offset = lumps[3].offset + i * 12;
        tables.vertices[i * 3 + 0] = ReadLittleEndian<float>(bytes, offset);
        tables.vertices[i * 3 + 1] = ReadLittleEndian<float>(bytes, offset + 4);
        tables.vertices[i * 3 + 2] = ReadLittleEndian<float>(bytes, offset + 8);
    }

    for (size_t i = 0; i < texInfoCount; i++)
    {
        const size_t offset = lumps[6].offset + i * 40;
        for (int axis = 0; axis < 4; axis++)
        {
            tables.texInfos[i].u[axis] = ReadLittleEndian<float>(bytes, offset + size_t(axis) * 4);
            tables.texInfos[i].v[axis] =
                ReadLittleEndian<float>(bytes, offset + 16 + size_t(axis) * 4);
        }
        tables.texInfos[i].miptex = ReadLittleEndian<int32_t>(bytes, offset + 32);
        tables.texInfos[i].flags = ReadLittleEndian<int32_t>(bytes, offset + 36);
    }

    for (size_t i = 0; i < faceCount; i++)
    {
        const size_t offset = lumps[7].offset + i * tables.format.faceStride;
        ReadBspFaceEntry(bytes, offset, tables.format.bsp2, tables.faces[i]);
    }

    tables.hiddenFaces = BuildHiddenBspFaceMask(bytes, lumps[14], faceCount);

    for (size_t i = 0; i < edgeCount; i++)
    {
        const size_t offset = lumps[12].offset + i * tables.format.edgeStride;
        ReadBspEdgeEntry(bytes, offset, tables.format.bsp2, tables.edges[i]);
    }

    for (size_t i = 0; i < surfEdgeCount; i++)
    {
        tables.surfEdges[i] = ReadLittleEndian<int32_t>(bytes, lumps[13].offset + i * 4);
    }

    if (!DecodeBspTextures(bytes, lumps[2], tables.format.miptexHeaderSize, tables.mipTextures))
    {
        error = "malformed BSP texture lump in " + sourceName;
        return false;
    }

    return true;
}

void BuildBspTexturesAndSky(const BspGeometryTables &tables, const uint8_t *palette768,
                            EditorScene &scene)
{
    scene.hasSkyFlatColor = false;
    std::fill(std::begin(scene.skyFlatColor), std::end(scene.skyFlatColor), 0.0f);
    scene.hasScrollingSky = false;
    scene.skyLayerWidth = 0;
    scene.skyLayerHeight = 0;
    scene.skySolidRgba.clear();
    scene.skyAlphaRgba.clear();
    scene.textures.resize(tables.mipTextures.size());

    for (size_t i = 0; i < tables.mipTextures.size(); i++)
    {
        const MipTexture &mip = tables.mipTextures[i];
        if (mip.width == 0 || mip.height == 0)
        {
            continue;
        }

        ConvertIndexedToRgba(mip, palette768, scene.textures[i]);
        if (mip.name.rfind("sky", 0) == 0)
        {
            if (ComputeSkyFlatColor(mip, palette768, scene.skyFlatColor))
            {
                scene.hasSkyFlatColor = true;
            }
            BuildScrollingSkyLayers(mip, palette768, scene);
        }
    }
}

bool ExtractBspFacePolygon(const BspFace &face, const BspGeometryTables &tables,
                           std::vector<float> &polygon, float faceNormal[3])
{
    const size_t vertexCount = tables.vertices.size() / 3;
    const size_t edgeCount = tables.edges.size();

    polygon.clear();
    polygon.reserve(size_t(face.edgeCount) * 3);

    for (int i = 0; i < face.edgeCount; i++)
    {
        const int32_t surfEdge = tables.surfEdges[size_t(face.firstEdge) + size_t(i)];
        const size_t edgeIndex = size_t(surfEdge < 0 ? -int64_t(surfEdge) : int64_t(surfEdge));
        if (edgeIndex == 0 || edgeIndex >= edgeCount)
        {
            return false;
        }

        const BspEdge &edge = tables.edges[edgeIndex];
        const uint32_t vertexIndex = surfEdge >= 0 ? edge.vertex[0] : edge.vertex[1];
        if (size_t(vertexIndex) >= vertexCount)
        {
            return false;
        }

        polygon.push_back(tables.vertices[size_t(vertexIndex) * 3 + 0]);
        polygon.push_back(tables.vertices[size_t(vertexIndex) * 3 + 1]);
        polygon.push_back(tables.vertices[size_t(vertexIndex) * 3 + 2]);
    }

    if (polygon.size() < 9)
    {
        return false;
    }

    if (face.plane >= 0 && size_t(face.plane) < tables.planes.size())
    {
        const float side = face.side != 0 ? -1.0f : 1.0f;
        for (int axis = 0; axis < 3; axis++)
        {
            faceNormal[axis] = tables.planes[size_t(face.plane)].normal[axis] * side;
        }
        return true;
    }

    float edge0[3] = {};
    float edge1[3] = {};
    for (int axis = 0; axis < 3; axis++)
    {
        edge0[axis] = polygon[6 + size_t(axis)] - polygon[size_t(axis)];
        edge1[axis] = polygon[3 + size_t(axis)] - polygon[size_t(axis)];
    }

    faceNormal[0] = edge0[1] * edge1[2] - edge0[2] * edge1[1];
    faceNormal[1] = edge0[2] * edge1[0] - edge0[0] * edge1[2];
    faceNormal[2] = edge0[0] * edge1[1] - edge0[1] * edge1[0];

    const float normalLength = std::sqrt(faceNormal[0] * faceNormal[0] +
                                         faceNormal[1] * faceNormal[1] +
                                         faceNormal[2] * faceNormal[2]);
    if (normalLength <= 0.000001f)
    {
        return false;
    }

    faceNormal[0] /= normalLength;
    faceNormal[1] /= normalLength;
    faceNormal[2] /= normalLength;
    return true;
}

EditorSurface &FindOrCreateBspSurface(const std::string &textureName, int32_t textureIndex,
                                      std::unordered_map<std::string, size_t> &surfaceByTexture,
                                      std::vector<EditorSurface> &surfaces)
{
    const std::string key = AnimationBaseName(textureName);
    const auto found = surfaceByTexture.find(key);

    if (found != surfaceByTexture.end())
    {
        EditorSurface &existing = surfaces[found->second];
        if (textureIndex >= 0 &&
            std::find(existing.animationFrames.begin(), existing.animationFrames.end(),
                      textureIndex) == existing.animationFrames.end())
        {
            existing.animationFrames.push_back(textureIndex);
        }
        return existing;
    }

    EditorSurface surface;
    surface.textureName = key;
    surface.texture = textureIndex;
    surface.kind = ClassifyTexture(key);
    if (textureIndex >= 0)
    {
        surface.animationFrames.push_back(textureIndex);
    }
    surfaceByTexture.emplace(key, surfaces.size());
    surfaces.push_back(std::move(surface));
    return surfaces.back();
}

struct AliasStVert
{
    int32_t onseam = 0;
    int32_t s = 0;
    int32_t t = 0;
};

struct AliasTriangle
{
    int32_t facesfront = 0;
    int32_t vertindex[3] = {};
};

struct AliasPoseVertex
{
    uint8_t v[3] = {};
    uint8_t lightnormalindex = 0;
};

struct ParsedAliasModel
{
    bool attempted = false;
    bool valid = false;
    float scale[3] = {};
    float scaleOrigin[3] = {};
    uint32_t skinWidth = 0;
    uint32_t skinHeight = 0;
    int32_t flags = 0;
    std::vector<int32_t> skinTextures;
    std::vector<EditorTexture> previewSkins;
    std::vector<AliasStVert> stverts;
    std::vector<AliasTriangle> triangles;
    std::vector<std::vector<AliasPoseVertex>> framePoses;
    std::vector<std::string> frameNames;
};

struct ParsedExternalBspModel
{
    bool attempted = false;
    bool valid = false;
    BspGeometryTables tables;
    std::vector<int32_t> sceneTexIndices;
    size_t faceStart = 0;
    size_t faceEnd = 0;
};

struct EntityModelSpec
{
    std::string modelPath;
    int32_t skin = 0;
    int32_t frame = 0;
    uint64_t uniqueID = 0;
    float origin[3] = {};
    float angles[3] = {};
};

struct EntityTransform
{
    float m[3][3] = {};
    float origin[3] = {};
};

constexpr float kAnorms[162][3] = {
    { -0.525731f, 0.000000f, 0.850651f },  { -0.442863f, 0.238856f, 0.864188f },
    { -0.295242f, 0.000000f, 0.955423f },  { -0.309017f, 0.500000f, 0.809017f },
    { -0.162460f, 0.262866f, 0.951056f },  { 0.000000f, 0.000000f, 1.000000f },
    { 0.000000f, 0.850651f, 0.525731f },   { -0.147621f, 0.716567f, 0.681718f },
    { 0.147621f, 0.716567f, 0.681718f },   { 0.000000f, 0.525731f, 0.850651f },
    { 0.309017f, 0.500000f, 0.809017f },   { 0.525731f, 0.000000f, 0.850651f },
    { 0.295242f, 0.000000f, 0.955423f },   { 0.442863f, 0.238856f, 0.864188f },
    { 0.162460f, 0.262866f, 0.951056f },   { -0.681718f, 0.147621f, 0.716567f },
    { -0.809017f, 0.309017f, 0.500000f },  { -0.587785f, 0.425325f, 0.688191f },
    { -0.850651f, 0.525731f, 0.000000f },  { -0.864188f, 0.442863f, 0.238856f },
    { -0.716567f, 0.681718f, 0.147621f },  { -0.688191f, 0.587785f, 0.425325f },
    { -0.500000f, 0.809017f, 0.309017f },  { -0.238856f, 0.864188f, 0.442863f },
    { -0.425325f, 0.688191f, 0.587785f },  { -0.716567f, 0.681718f, -0.147621f },
    { -0.500000f, 0.809017f, -0.309017f }, { -0.525731f, 0.850651f, 0.000000f },
    { 0.000000f, 0.850651f, -0.525731f },  { -0.238856f, 0.864188f, -0.442863f },
    { 0.000000f, 0.955423f, -0.295242f },  { -0.262866f, 0.951056f, -0.162460f },
    { 0.000000f, 1.000000f, 0.000000f },   { 0.000000f, 0.955423f, 0.295242f },
    { -0.262866f, 0.951056f, 0.162460f },  { 0.238856f, 0.864188f, 0.442863f },
    { 0.262866f, 0.951056f, 0.162460f },   { 0.500000f, 0.809017f, 0.309017f },
    { 0.238856f, 0.864188f, -0.442863f },  { 0.262866f, 0.951056f, -0.162460f },
    { 0.500000f, 0.809017f, -0.309017f },  { 0.850651f, 0.525731f, 0.000000f },
    { 0.716567f, 0.681718f, 0.147621f },   { 0.716567f, 0.681718f, -0.147621f },
    { 0.525731f, 0.850651f, 0.000000f },   { 0.425325f, 0.688191f, 0.587785f },
    { 0.864188f, 0.442863f, 0.238856f },   { 0.688191f, 0.587785f, 0.425325f },
    { 0.809017f, 0.309017f, 0.500000f },   { 0.681718f, 0.147621f, 0.716567f },
    { 0.587785f, 0.425325f, 0.688191f },   { 0.955423f, 0.295242f, 0.000000f },
    { 1.000000f, 0.000000f, 0.000000f },   { 0.951056f, 0.162460f, 0.262866f },
    { 0.850651f, -0.525731f, 0.000000f },  { 0.955423f, -0.295242f, 0.000000f },
    { 0.864188f, -0.442863f, 0.238856f },  { 0.951056f, -0.162460f, 0.262866f },
    { 0.809017f, -0.309017f, 0.500000f },  { 0.681718f, -0.147621f, 0.716567f },
    { 0.850651f, 0.000000f, 0.525731f },   { 0.864188f, 0.442863f, -0.238856f },
    { 0.809017f, 0.309017f, -0.500000f },  { 0.951056f, 0.162460f, -0.262866f },
    { 0.525731f, 0.000000f, -0.850651f },  { 0.681718f, 0.147621f, -0.716567f },
    { 0.681718f, -0.147621f, -0.716567f }, { 0.850651f, 0.000000f, -0.525731f },
    { 0.809017f, -0.309017f, -0.500000f }, { 0.864188f, -0.442863f, -0.238856f },
    { 0.951056f, -0.162460f, -0.262866f }, { 0.147621f, 0.716567f, -0.681718f },
    { 0.309017f, 0.500000f, -0.809017f },  { 0.425325f, 0.688191f, -0.587785f },
    { 0.442863f, 0.238856f, -0.864188f },  { 0.587785f, 0.425325f, -0.688191f },
    { 0.688191f, 0.587785f, -0.425325f },  { -0.147621f, 0.716567f, -0.681718f },
    { -0.309017f, 0.500000f, -0.809017f }, { 0.000000f, 0.525731f, -0.850651f },
    { -0.525731f, 0.000000f, -0.850651f }, { -0.442863f, 0.238856f, -0.864188f },
    { -0.295242f, 0.000000f, -0.955423f }, { -0.162460f, 0.262866f, -0.951056f },
    { 0.000000f, 0.000000f, -1.000000f },  { 0.295242f, 0.000000f, -0.955423f },
    { 0.162460f, 0.262866f, -0.951056f },  { -0.442863f, -0.238856f, -0.864188f },
    { -0.309017f, -0.500000f, -0.809017f }, { -0.162460f, -0.262866f, -0.951056f },
    { 0.000000f, -0.850651f, -0.525731f }, { -0.147621f, -0.716567f, -0.681718f },
    { 0.147621f, -0.716567f, -0.681718f }, { 0.000000f, -0.525731f, -0.850651f },
    { 0.309017f, -0.500000f, -0.809017f }, { 0.442863f, -0.238856f, -0.864188f },
    { 0.162460f, -0.262866f, -0.951056f }, { 0.238856f, -0.864188f, -0.442863f },
    { 0.500000f, -0.809017f, -0.309017f }, { 0.425325f, -0.688191f, -0.587785f },
    { 0.716567f, -0.681718f, -0.147621f }, { 0.688191f, -0.587785f, -0.425325f },
    { 0.587785f, -0.425325f, -0.688191f }, { 0.000000f, -0.955423f, -0.295242f },
    { 0.000000f, -1.000000f, 0.000000f },  { 0.262866f, -0.951056f, -0.162460f },
    { 0.000000f, -0.850651f, 0.525731f },  { 0.000000f, -0.955423f, 0.295242f },
    { 0.238856f, -0.864188f, 0.442863f },  { 0.262866f, -0.951056f, 0.162460f },
    { 0.500000f, -0.809017f, 0.309017f },  { 0.716567f, -0.681718f, 0.147621f },
    { 0.525731f, -0.850651f, 0.000000f },  { -0.238856f, -0.864188f, -0.442863f },
    { -0.500000f, -0.809017f, -0.309017f }, { -0.262866f, -0.951056f, -0.162460f },
    { -0.850651f, -0.525731f, 0.000000f }, { -0.716567f, -0.681718f, -0.147621f },
    { -0.716567f, -0.681718f, 0.147621f }, { -0.525731f, -0.850651f, 0.000000f },
    { -0.500000f, -0.809017f, 0.309017f }, { -0.238856f, -0.864188f, 0.442863f },
    { -0.262866f, -0.951056f, 0.162460f }, { -0.864188f, -0.442863f, 0.238856f },
    { -0.809017f, -0.309017f, 0.500000f }, { -0.688191f, -0.587785f, 0.425325f },
    { -0.681718f, -0.147621f, 0.716567f }, { -0.442863f, -0.238856f, 0.864188f },
    { -0.587785f, -0.425325f, 0.688191f }, { -0.309017f, -0.500000f, 0.809017f },
    { -0.147621f, -0.716567f, 0.681718f }, { -0.425325f, -0.688191f, 0.587785f },
    { -0.162460f, -0.262866f, 0.951056f }, { 0.442863f, -0.238856f, 0.864188f },
    { 0.162460f, -0.262866f, 0.951056f },  { 0.309017f, -0.500000f, 0.809017f },
    { 0.147621f, -0.716567f, 0.681718f },  { 0.000000f, -0.525731f, 0.850651f },
    { 0.425325f, -0.688191f, 0.587785f },  { 0.587785f, -0.425325f, 0.688191f },
    { 0.688191f, -0.587785f, 0.425325f },  { -0.955423f, 0.295242f, 0.000000f },
    { -0.951056f, 0.162460f, 0.262866f },  { -1.000000f, 0.000000f, 0.000000f },
    { -0.850651f, 0.000000f, 0.525731f },  { -0.955423f, -0.295242f, 0.000000f },
    { -0.951056f, -0.162460f, 0.262866f }, { -0.864188f, 0.442863f, -0.238856f },
    { -0.951056f, 0.162460f, -0.262866f }, { -0.809017f, 0.309017f, -0.500000f },
    { -0.864188f, -0.442863f, -0.238856f }, { -0.951056f, -0.162460f, -0.262866f },
    { -0.809017f, -0.309017f, -0.500000f }, { -0.681718f, 0.147621f, -0.716567f },
    { -0.681718f, -0.147621f, -0.716567f }, { -0.850651f, 0.000000f, -0.525731f },
    { -0.688191f, 0.587785f, -0.425325f }, { -0.587785f, 0.425325f, -0.688191f },
    { -0.425325f, 0.688191f, -0.587785f }, { -0.425325f, -0.688191f, -0.587785f },
    { -0.587785f, -0.425325f, -0.688191f }, { -0.688191f, -0.587785f, -0.425325f },
};

int FindBlackPaletteIndex(const uint8_t *palette)
{
    for (int i = 0; i < 256; i++)
    {
        if (palette[size_t(i) * 3 + 0] == 0 && palette[size_t(i) * 3 + 1] == 0 &&
            palette[size_t(i) * 3 + 2] == 0)
        {
            return i;
        }
    }

    return 0;
}

void FloodFillStep(uint8_t *cell, int nx, int ny, uint8_t fillColor, uint8_t &fdc,
                   std::pair<int16_t, int16_t> *fifo, int &inpt)
{
    constexpr int kFifoMask = 0x1000 - 1;

    if (*cell == fillColor)
    {
        *cell = 255;
        fifo[inpt] = { int16_t(nx), int16_t(ny) };
        inpt = (inpt + 1) & kFifoMask;
    }
    else if (*cell != 255)
    {
        fdc = *cell;
    }
}

void FloodFillAliasSkin(std::vector<uint8_t> &skin, int width, int height, const uint8_t *palette)
{
    if (width <= 0 || height <= 0 || skin.empty())
    {
        return;
    }

    const uint8_t filledColor = uint8_t(FindBlackPaletteIndex(palette));
    const uint8_t fillColor = skin[0];

    if (fillColor == filledColor || fillColor == 255)
    {
        return;
    }

    std::vector<std::pair<int16_t, int16_t>> fifo(0x1000);
    constexpr int kFifoMask = 0x1000 - 1;
    int inpt = 1;
    int outpt = 0;
    fifo[0] = { 0, 0 };

    while (outpt != inpt)
    {
        const int x = fifo[outpt].first;
        const int y = fifo[outpt].second;
        uint8_t fdc = filledColor;
        uint8_t *pos = skin.data() + x + width * y;
        outpt = (outpt + 1) & kFifoMask;

        if (x > 0)
        {
            FloodFillStep(pos - 1, x - 1, y, fillColor, fdc, fifo.data(), inpt);
        }
        if (x < width - 1)
        {
            FloodFillStep(pos + 1, x + 1, y, fillColor, fdc, fifo.data(), inpt);
        }
        if (y > 0)
        {
            FloodFillStep(pos - width, x, y - 1, fillColor, fdc, fifo.data(), inpt);
        }
        if (y < height - 1)
        {
            FloodFillStep(pos + width, x, y + 1, fillColor, fdc, fifo.data(), inpt);
        }
        *pos = fdc;
    }
}

void ConvertAliasSkinToRgba(const std::vector<uint8_t> &skin, uint32_t width, uint32_t height,
                            bool holey, const uint8_t *palette768, EditorTexture &out)
{
    const uint8_t *palette = palette768 != nullptr ? palette768 : kDefaultQuakePalette;
    out.width = width;
    out.height = height;
    out.isExternal = false;
    out.rgba.assign(size_t(width) * size_t(height) * 4, 255);
    out.fullbright.clear();

    for (size_t i = 0; i < skin.size(); i++)
    {
        const uint8_t index = skin[i];

        if (holey && index == 255)
        {
            out.rgba[i * 4 + 0] = 0;
            out.rgba[i * 4 + 1] = 0;
            out.rgba[i * 4 + 2] = 0;
            out.rgba[i * 4 + 3] = 0;
            continue;
        }

        const uint8_t r = palette[size_t(index) * 3 + 0];
        const uint8_t g = palette[size_t(index) * 3 + 1];
        const uint8_t b = palette[size_t(index) * 3 + 2];
        out.rgba[i * 4 + 0] = r;
        out.rgba[i * 4 + 1] = g;
        out.rgba[i * 4 + 2] = b;
        out.rgba[i * 4 + 3] = 255;
    }
}

int32_t FindOrAddSceneTexture(EditorScene &scene, EditorTexture &&tex)
{
    const std::string key = Lowercase(tex.name);

    for (size_t i = 0; i < scene.textures.size(); i++)
    {
        if (Lowercase(scene.textures[i].name) == key && scene.textures[i].width > 0)
        {
            return int32_t(i);
        }
    }

    const int32_t index = int32_t(scene.textures.size());
    scene.textures.push_back(std::move(tex));
    return index;
}

bool DecodeAliasSkins(const std::vector<uint8_t> &bytes, int32_t numSkins,
                      const std::string &modelPath, const uint8_t *palette768, size_t &cursor,
                      ParsedAliasModel &mdl)
{
    const size_t skinBytes = size_t(mdl.skinWidth) * size_t(mdl.skinHeight);
    const uint8_t *palette = palette768 != nullptr ? palette768 : kDefaultQuakePalette;
    const bool holey = (mdl.flags & 0x4000) != 0;
    mdl.previewSkins.clear();

    for (int32_t s = 0; s < numSkins; s++)
    {
        if (cursor + 4 > bytes.size())
        {
            return false;
        }

        const int32_t skinType = ReadLittleEndian<int32_t>(bytes, cursor);
        cursor += 4;
        size_t groupSkins = 1;

        if (skinType != 0)
        {
            if (cursor + 4 > bytes.size())
            {
                return false;
            }

            const int32_t rawCount = ReadLittleEndian<int32_t>(bytes, cursor);
            cursor += 4;

            if (rawCount <= 0 || rawCount > 1024 || size_t(rawCount) * 4 > bytes.size() - cursor)
            {
                return false;
            }

            groupSkins = size_t(rawCount);
            cursor += groupSkins * 4;
        }

        if (skinBytes == 0 || groupSkins > (bytes.size() - cursor) / skinBytes)
        {
            return false;
        }

        std::vector<uint8_t> indexed(bytes.data() + cursor, bytes.data() + cursor + skinBytes);
        FloodFillAliasSkin(indexed, int(mdl.skinWidth), int(mdl.skinHeight), palette);

        EditorTexture tex;
        tex.name = Lowercase(modelPath) + ":frame" + std::to_string(s);
        ConvertAliasSkinToRgba(indexed, mdl.skinWidth, mdl.skinHeight, holey, palette, tex);
        mdl.previewSkins.push_back(std::move(tex));
        cursor += groupSkins * skinBytes;
    }

    return true;
}

bool ParseAliasSkins(const std::vector<uint8_t> &bytes, int32_t numSkins,
                     const std::string &modelPath, const uint8_t *palette768, size_t &cursor,
                     EditorScene &scene, ParsedAliasModel &mdl)
{
    if (!DecodeAliasSkins(bytes, numSkins, modelPath, palette768, cursor, mdl))
    {
        return false;
    }

    for (EditorTexture &tex : mdl.previewSkins)
    {
        mdl.skinTextures.push_back(FindOrAddSceneTexture(scene, std::move(tex)));
    }

    mdl.previewSkins.clear();
    return true;
}

std::string ReadAliasFrameName(const std::vector<uint8_t> &bytes, size_t nameOffset)
{
    const char *raw = reinterpret_cast<const char *>(bytes.data() + nameOffset);
    size_t len = 0;

    while (len < 16 && raw[len] != '\0')
    {
        len++;
    }

    return std::string(raw, len);
}

bool ParseAliasFrames(const std::vector<uint8_t> &bytes, int32_t numVerts, int32_t numFrames,
                      size_t &cursor, ParsedAliasModel &mdl)
{
    const size_t poseDataBytes = 24 + size_t(numVerts) * 4;
    mdl.framePoses.resize(size_t(numFrames));
    mdl.frameNames.resize(size_t(numFrames));

    for (int32_t f = 0; f < numFrames; f++)
    {
        if (cursor + 4 > bytes.size())
        {
            return false;
        }

        const int32_t frameType = ReadLittleEndian<int32_t>(bytes, cursor);
        cursor += 4;
        size_t groupFrames = 1;

        if (frameType != 0)
        {
            if (cursor + 12 > bytes.size())
            {
                return false;
            }

            const int32_t rawGroup = ReadLittleEndian<int32_t>(bytes, cursor);
            cursor += 12;

            if (rawGroup <= 0 || rawGroup > 4096 ||
                size_t(rawGroup) * 4 > bytes.size() - cursor)
            {
                return false;
            }

            groupFrames = size_t(rawGroup);
            cursor += groupFrames * 4;
        }

        if (groupFrames > (bytes.size() - cursor) / poseDataBytes)
        {
            return false;
        }

        mdl.frameNames[size_t(f)] = ReadAliasFrameName(bytes, cursor + 8);
        std::vector<AliasPoseVertex> &pose = mdl.framePoses[size_t(f)];
        pose.resize(size_t(numVerts));
        const size_t vertsOffset = cursor + 24;

        for (int32_t v = 0; v < numVerts; v++)
        {
            const uint8_t *src = bytes.data() + vertsOffset + size_t(v) * 4;
            pose[size_t(v)].v[0] = src[0];
            pose[size_t(v)].v[1] = src[1];
            pose[size_t(v)].v[2] = src[2];
            pose[size_t(v)].lightnormalindex = src[3] < 162 ? src[3] : 0;
        }

        cursor += groupFrames * poseDataBytes;
    }

    return true;
}

struct AliasHeaderCounts
{
    int32_t numSkins = 0;
    int32_t numVerts = 0;
    int32_t numTris = 0;
    int32_t numFrames = 0;
};

bool InClosedRange(int32_t value, int32_t minValue, int32_t maxValue)
{
    return value >= minValue && value <= maxValue;
}

bool ValidateAliasHeader(const std::vector<uint8_t> &bytes, ParsedAliasModel &mdl,
                         AliasHeaderCounts &counts)
{
    constexpr uint32_t kMdlHeaderSize = 84;
    constexpr uint32_t kIdpoMagic = 0x4F504449u;

    if (bytes.size() < kMdlHeaderSize || ReadLittleEndian<uint32_t>(bytes, 0) != kIdpoMagic ||
        ReadLittleEndian<int32_t>(bytes, 4) != 6)
    {
        return false;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        mdl.scale[axis] = ReadLittleEndian<float>(bytes, 8 + size_t(axis) * 4);
        mdl.scaleOrigin[axis] = ReadLittleEndian<float>(bytes, 20 + size_t(axis) * 4);
    }

    counts.numSkins = ReadLittleEndian<int32_t>(bytes, 48);
    const int32_t skinWidth = ReadLittleEndian<int32_t>(bytes, 52);
    const int32_t skinHeight = ReadLittleEndian<int32_t>(bytes, 56);
    counts.numVerts = ReadLittleEndian<int32_t>(bytes, 60);
    counts.numTris = ReadLittleEndian<int32_t>(bytes, 64);
    counts.numFrames = ReadLittleEndian<int32_t>(bytes, 68);
    mdl.flags = ReadLittleEndian<int32_t>(bytes, 76);

    if (!InClosedRange(counts.numSkins, 1, 256) || !InClosedRange(skinWidth, 1, 4096) ||
        !InClosedRange(skinHeight, 1, 4096))
    {
        return false;
    }

    if (!InClosedRange(counts.numVerts, 1, 65535) || !InClosedRange(counts.numTris, 1, 65535) ||
        !InClosedRange(counts.numFrames, 1, 4096))
    {
        return false;
    }

    mdl.skinWidth = uint32_t(skinWidth);
    mdl.skinHeight = uint32_t(skinHeight);
    return true;
}

bool ParseAliasStVertsAndTris(const std::vector<uint8_t> &bytes, int32_t numVerts, int32_t numTris,
                              size_t &cursor, ParsedAliasModel &mdl)
{
    const size_t stBytes = size_t(numVerts) * 12;
    const size_t triBytes = size_t(numTris) * 16;

    if (stBytes + triBytes > bytes.size() - cursor)
    {
        return false;
    }

    mdl.stverts.resize(size_t(numVerts));

    for (int32_t i = 0; i < numVerts; i++)
    {
        mdl.stverts[size_t(i)].onseam = ReadLittleEndian<int32_t>(bytes, cursor + 0);
        mdl.stverts[size_t(i)].s = ReadLittleEndian<int32_t>(bytes, cursor + 4);
        mdl.stverts[size_t(i)].t = ReadLittleEndian<int32_t>(bytes, cursor + 8);
        cursor += 12;
    }

    mdl.triangles.resize(size_t(numTris));

    for (int32_t i = 0; i < numTris; i++)
    {
        mdl.triangles[size_t(i)].facesfront = ReadLittleEndian<int32_t>(bytes, cursor + 0);

        for (int k = 0; k < 3; k++)
        {
            const int32_t vi = ReadLittleEndian<int32_t>(bytes, cursor + 4 + size_t(k) * 4);
            if (vi < 0 || vi >= numVerts)
            {
                return false;
            }
            mdl.triangles[size_t(i)].vertindex[k] = vi;
        }

        cursor += 16;
    }

    return true;
}

bool ParseAliasModel(const std::vector<uint8_t> &bytes, const std::string &modelPath,
                     const uint8_t *palette768, EditorScene &scene, ParsedAliasModel &mdl)
{
    AliasHeaderCounts counts;

    if (!ValidateAliasHeader(bytes, mdl, counts))
    {
        return false;
    }

    size_t cursor = 84;

    if (!ParseAliasSkins(bytes, counts.numSkins, modelPath, palette768, cursor, scene, mdl))
    {
        return false;
    }

    if (!ParseAliasStVertsAndTris(bytes, counts.numVerts, counts.numTris, cursor, mdl))
    {
        return false;
    }

    if (!ParseAliasFrames(bytes, counts.numVerts, counts.numFrames, cursor, mdl))
    {
        return false;
    }

    mdl.valid = true;
    return true;
}

bool ParseVec3Text(const std::string &text, float out[3])
{
    return std::sscanf(text.c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3;
}

EntityTransform BuildEntityTransform(const float origin[3], const float angles[3])
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float yaw = angles[1] * kDegToRad;
    const float pitch = -angles[0] * kDegToRad;
    const float roll = angles[2] * kDegToRad;

    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);

    // Matches R_RotateForEntity: Rz(yaw) * Ry(-pitch) * Rx(roll).
    EntityTransform xf;
    std::copy_n(origin, 3, xf.origin);
    xf.m[0][0] = cy * cp;
    xf.m[0][1] = cy * sp * sr - sy * cr;
    xf.m[0][2] = cy * sp * cr + sy * sr;
    xf.m[1][0] = sy * cp;
    xf.m[1][1] = sy * sp * sr + cy * cr;
    xf.m[1][2] = sy * sp * cr - cy * sr;
    xf.m[2][0] = -sp;
    xf.m[2][1] = cp * sr;
    xf.m[2][2] = cp * cr;
    return xf;
}

void TransformEntityPoint(const EntityTransform &xf, const float in[3], float out[3])
{
    for (int row = 0; row < 3; row++)
    {
        out[row] = xf.m[row][0] * in[0] + xf.m[row][1] * in[1] + xf.m[row][2] * in[2] +
                   xf.origin[row];
    }
}

void TransformEntityVector(const EntityTransform &xf, const float in[3], float out[3])
{
    for (int row = 0; row < 3; row++)
    {
        out[row] = xf.m[row][0] * in[0] + xf.m[row][1] * in[1] + xf.m[row][2] * in[2];
    }
}

struct SimpleClassModel
{
    const char *className;
    const char *modelPath;
    int32_t skin;
    int32_t frame;
};

constexpr SimpleClassModel kSimpleClassModels[] = {
    { "light_flame_large_yellow", "progs/flame2.mdl", 0, 1 },
    { "light_flame_small_yellow", "progs/flame2.mdl", 0, 0 },
    { "light_flame_small_white", "progs/flame2.mdl", 0, 0 },
    { "light_torch_small_walltorch", "progs/flame.mdl", 0, 0 },
    { "light_candle", "progs/candle.mdl", 0, 0 },
    { "light_flame_gas", "progs/flame3.mdl", 0, 0 },
    { "light_lantern", "progs/lantern.mdl", 0, 0 },
    { "misc_fireball", "progs/lavaball.mdl", 0, 0 },
    { "monster_army", "progs/soldier.mdl", 0, 0 },
    { "monster_boss", "progs/boss.mdl", 0, 0 },
    { "monster_demon1", "progs/demon.mdl", 0, 0 },
    { "monster_dog", "progs/dog.mdl", 0, 0 },
    { "monster_enforcer", "progs/enforcer.mdl", 0, 0 },
    { "monster_fish", "progs/fish.mdl", 0, 0 },
    { "monster_hell_knight", "progs/hknight.mdl", 0, 0 },
    { "monster_knight", "progs/knight.mdl", 0, 0 },
    { "monster_ogre", "progs/ogre.mdl", 0, 0 },
    { "monster_ogre_marksman", "progs/ogre.mdl", 0, 0 },
    { "monster_oldone", "progs/oldone.mdl", 0, 0 },
    { "monster_shalrath", "progs/shalrath.mdl", 0, 0 },
    { "monster_shambler", "progs/shambler.mdl", 0, 0 },
    { "monster_tarbaby", "progs/tarbaby.mdl", 0, 0 },
    { "monster_wizard", "progs/wizard.mdl", 0, 0 },
    { "weapon_supershotgun", "progs/g_shot.mdl", 0, 0 },
    { "weapon_nailgun", "progs/g_nail.mdl", 0, 0 },
    { "weapon_supernailgun", "progs/g_nail2.mdl", 0, 0 },
    { "weapon_grenadelauncher", "progs/g_rock.mdl", 0, 0 },
    { "weapon_rocketlauncher", "progs/g_rock2.mdl", 0, 0 },
    { "weapon_lightning", "progs/g_light.mdl", 0, 0 },
    { "item_armor1", "progs/armor.mdl", 0, 0 },
    { "item_armor2", "progs/armor.mdl", 1, 0 },
    { "item_armorInv", "progs/armor.mdl", 2, 0 },
    { "item_artifact_envirosuit", "progs/suit.mdl", 0, 0 },
    { "item_artifact_invisibility", "progs/invisibl.mdl", 0, 0 },
    { "item_artifact_invulnerability", "progs/invulner.mdl", 0, 0 },
    { "item_artifact_super_damage", "progs/quaddama.mdl", 0, 0 },
    { "misc_explobox", "maps/b_explob.bsp", 0, 0 },
    { "misc_explobox2", "maps/b_exbox2.bsp", 0, 0 },
};

struct BinaryVariantModel
{
    const char *className;
    const char *normalModel;
    const char *bigModel;
};

constexpr BinaryVariantModel kAmmoVariantModels[] = {
    { "item_shells", "maps/b_shell0.bsp", "maps/b_shell1.bsp" },
    { "item_spikes", "maps/b_nail0.bsp", "maps/b_nail1.bsp" },
    { "item_rockets", "maps/b_rock0.bsp", "maps/b_rock1.bsp" },
    { "item_cells", "maps/b_batt0.bsp", "maps/b_batt1.bsp" },
};

const char *ResolveSigilOrWeaponModel(const std::string &className, int32_t spawnFlags)
{
    if (className == "item_sigil")
    {
        if ((spawnFlags & 8) != 0) return "progs/end4.mdl";
        if ((spawnFlags & 4) != 0) return "progs/end3.mdl";
        if ((spawnFlags & 2) != 0) return "progs/end2.mdl";
        return "progs/end1.mdl";
    }
    if (className == "item_weapon")
    {
        const bool big = (spawnFlags & 8) != 0;
        if ((spawnFlags & 2) != 0) return big ? "maps/b_rock1.bsp" : "maps/b_rock0.bsp";
        if ((spawnFlags & 4) != 0) return big ? "maps/b_nail1.bsp" : "maps/b_nail0.bsp";
        return big ? "maps/b_shell1.bsp" : "maps/b_shell0.bsp";
    }
    return nullptr;
}

const char *ResolveClassVariantModel(const std::string &className, int32_t worldType,
                                     int32_t spawnFlags)
{
    static constexpr const char *kSilverKeys[3] = {
        "progs/w_s_key.mdl", "progs/m_s_key.mdl", "progs/b_s_key.mdl",
    };
    static constexpr const char *kGoldKeys[3] = {
        "progs/w_g_key.mdl", "progs/m_g_key.mdl", "progs/b_g_key.mdl",
    };
    const int32_t wt = std::clamp(worldType, 0, 2);

    if (className == "item_key1")
    {
        return kSilverKeys[wt];
    }
    if (className == "item_key2")
    {
        return kGoldKeys[wt];
    }
    if (className == "item_health")
    {
        return (spawnFlags & 1) != 0 ? "maps/b_bh10.bsp"
                                     : ((spawnFlags & 2) != 0 ? "maps/b_bh100.bsp"
                                                              : "maps/b_bh25.bsp");
    }

    for (const BinaryVariantModel &entry : kAmmoVariantModels)
    {
        if (className == entry.className)
        {
            return (spawnFlags & 1) != 0 ? entry.bigModel : entry.normalModel;
        }
    }

    return ResolveSigilOrWeaponModel(className, spawnFlags);
}

struct CorpsePoseSpec
{
    const char *modelPath;
    int32_t frame;
};

bool ResolveMiscCorpseModel(int32_t style, EntityModelSpec &out)
{
    static constexpr CorpsePoseSpec kCorpsePoses[] = {
        { "progs/demon.mdl", 53 },    { "progs/dog.mdl", 16 },      { "progs/dog.mdl", 25 },
        { "progs/enforcer.mdl", 54 }, { "progs/enforcer.mdl", 65 }, { "progs/fish.mdl", 38 },
        { "progs/hknight.mdl", 53 },  { "progs/hknight.mdl", 62 },  { "progs/knight.mdl", 85 },
        { "progs/knight.mdl", 96 },   { "progs/ogre.mdl", 125 },    { "progs/ogre.mdl", 135 },
        { "progs/shalrath.mdl", 22 }, { "progs/shambler.mdl", 93 }, { "progs/soldier.mdl", 17 },
        { "progs/soldier.mdl", 28 },  { "progs/wizard.mdl", 53 },   { "progs/player.mdl", 49 },
        { "progs/player.mdl", 60 },   { "progs/player.mdl", 69 },   { "progs/player.mdl", 84 },
        { "progs/player.mdl", 93 },   { "progs/player.mdl", 102 },  { "progs/h_demon.mdl", 0 },
        { "progs/h_dog.mdl", 0 },     { "progs/h_guard.mdl", 0 },   { "progs/h_hellkn.mdl", 0 },
        { "progs/h_knight.mdl", 0 },  { "progs/h_mega.mdl", 0 },    { "progs/h_ogre.mdl", 0 },
        { "progs/h_player.mdl", 0 },  { "progs/h_shal.mdl", 0 },    { "progs/h_shams.mdl", 0 },
        { "progs/h_wizard.mdl", 0 },  { "progs/h_zombie.mdl", 0 },  { "progs/gib1.mdl", 0 },
        { "progs/gib2.mdl", 0 },      { "progs/gib3.mdl", 0 },
    };

    if (style < 1 || size_t(style) > std::size(kCorpsePoses))
    {
        return false;
    }

    const CorpsePoseSpec &pose = kCorpsePoses[size_t(style - 1)];
    out.modelPath = pose.modelPath;
    out.frame = pose.frame;
    return true;
}

bool ResolveEntityModelSpec(const BspEntity &entity, int32_t worldType, EntityModelSpec &out)
{
    if (!ParseVec3Text(entity.Value("origin"), out.origin))
    {
        return false;
    }

    const std::string &className = entity.Value("classname");
    const int32_t spawnFlags = std::atoi(entity.Value("spawnflags").c_str());

    if (!ParseVec3Text(entity.Value("angles"), out.angles))
    {
        out.angles[0] = 0.0f;
        out.angles[1] = float(std::atof(entity.Value("angle").c_str()));
        out.angles[2] = 0.0f;
    }

    if (className.compare(0, 6, "light_") == 0 && (spawnFlags & 4) != 0)
    {
        out.angles[0] = 180.0f;
    }

    const std::string &explicitModel =
        !entity.Value("model").empty() ? entity.Value("model") : entity.Value("mdl");

    if (!explicitModel.empty() && explicitModel[0] != '*')
    {
        out.modelPath = explicitModel;
        out.skin = std::atoi(entity.Value("skin").c_str());
        out.frame = std::atoi(entity.Value("frame").c_str());
        return true;
    }

    if (className == "monster_zombie")
    {
        out.modelPath = "progs/zombie.mdl";
        out.frame = (spawnFlags & 1) != 0 ? 192 : 0;
        return true;
    }

    if (className == "misc_corpse")
    {
        return ResolveMiscCorpseModel(std::atoi(entity.Value("style").c_str()), out);
    }

    for (const SimpleClassModel &entry : kSimpleClassModels)
    {
        if (className == entry.className)
        {
            out.modelPath = entry.modelPath;
            out.skin = entry.skin;
            out.frame = entry.frame;
            return true;
        }
    }

    if (const char *variant = ResolveClassVariantModel(className, worldType, spawnFlags))
    {
        out.modelPath = variant;
        return true;
    }

    return false;
}

size_t ClampAliasIndex(int32_t index, size_t count)
{
    return (index >= 0 && size_t(index) < count) ? size_t(index) : 0;
}

QrVertex BuildAliasLocalVertex(const ParsedAliasModel &mdl, const std::vector<AliasPoseVertex> &pose,
                               const AliasTriangle &tri, int corner)
{
    const int32_t order[3] = { tri.vertindex[2], tri.vertindex[1], tri.vertindex[0] };
    const int32_t vi = order[corner];
    const AliasPoseVertex &pv = pose[size_t(vi)];
    const AliasStVert &st = mdl.stverts[size_t(vi)];

    QrVertex vertex = {};
    vertex.position[0] = float(pv.v[0]) * mdl.scale[0] + mdl.scaleOrigin[0];
    vertex.position[1] = float(pv.v[1]) * mdl.scale[1] + mdl.scaleOrigin[1];
    vertex.position[2] = float(pv.v[2]) * mdl.scale[2] + mdl.scaleOrigin[2];
    std::copy_n(kAnorms[pv.lightnormalindex], 3, vertex.normal);

    int32_t s = st.s;
    if (tri.facesfront == 0 && st.onseam != 0)
    {
        s += int32_t(mdl.skinWidth / 2);
    }
    vertex.texCoord[0] = (float(s) + 0.5f) / float(mdl.skinWidth);
    vertex.texCoord[1] = (float(st.t) + 0.5f) / float(mdl.skinHeight);
    vertex.packedColor = 0xFFFFFFFFu;
    return vertex;
}

void AppendAliasTriangle(const ParsedAliasModel &mdl, const std::vector<AliasPoseVertex> &pose,
                         const AliasTriangle &tri, const EntityTransform &xf,
                         uint32_t resolvedCluster, uint32_t aliasInstanceId,
                         EditorSurface &surface)
{
    // Reverse triangle winding (2, 1, 0) matching GLMesh_LoadVertexBuffer.
    float p[3][3] = {};
    float n[3][3] = {};
    QrFloat2D uv[3] = {};

    for (int corner = 0; corner < 3; corner++)
    {
        const QrVertex local = BuildAliasLocalVertex(mdl, pose, tri, corner);
        TransformEntityPoint(xf, local.position, p[corner]);
        TransformEntityVector(xf, local.normal, n[corner]);
        uv[corner] = { { local.texCoord[0], local.texCoord[1] } };
    }

    float geomNormal[3] = {};
    ComputeTriangleNormal(p[0], p[1], p[2], geomNormal);
    const float sumNormal[3] = {
        n[0][0] + n[1][0] + n[2][0],
        n[0][1] + n[1][1] + n[2][1],
        n[0][2] + n[1][2] + n[2][2],
    };
    if (geomNormal[0] * sumNormal[0] + geomNormal[1] * sumNormal[1] +
            geomNormal[2] * sumNormal[2] < 0.0f)
    {
        geomNormal[0] = -geomNormal[0];
        geomNormal[1] = -geomNormal[1];
        geomNormal[2] = -geomNormal[2];
    }

    EditorSurfaceFace face;
    std::copy_n(geomNormal, 3, face.normal);
    face.cluster = resolvedCluster;
    face.aliasInstanceId = aliasInstanceId;
    for (int corner = 0; corner < 3; corner++)
    {
        face.positions.push_back({ p[corner][0], p[corner][1], p[corner][2] });
        face.texCoords.push_back(uv[corner]);
    }
    surface.faces.push_back(std::move(face));

    for (int corner = 0; corner < 3; corner++)
    {
        QrVertex vertex = {};
        std::copy_n(p[corner], 3, vertex.position);
        std::copy_n(n[corner], 3, vertex.normal);
        vertex.texCoord[0] = uv[corner].data[0];
        vertex.texCoord[1] = uv[corner].data[1];
        vertex.packedColor = 0xFFFFFFFFu;
        vertex.cluster = resolvedCluster;
        surface.indices.push_back(uint32_t(surface.vertices.size()));
        surface.vertices.push_back(vertex);
    }
}

// Matches RT_AliasModelLightSize in Quake/r_alias.c: half the diagonal of the active pose's
// model-space bounding box, in meters (clamped to a tiny positive floor).
float ComputeAliasModelLightSizeMeters(const ParsedAliasModel &mdl,
                                       const std::vector<AliasPoseVertex> &pose)
{
    if (pose.empty())
    {
        return 0.5f;
    }

    uint8_t minV[3] = { 255, 255, 255 };
    uint8_t maxV[3] = { 0, 0, 0 };

    for (const AliasPoseVertex &vert : pose)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            minV[axis] = std::min(minV[axis], vert.v[axis]);
            maxV[axis] = std::max(maxV[axis], vert.v[axis]);
        }
    }

    float size[3] = {};
    for (int axis = 0; axis < 3; axis++)
    {
        size[axis] = float(int(maxV[axis]) - int(minV[axis])) * std::fabs(mdl.scale[axis]);
    }

    const float diag = std::sqrt(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]);
    return std::max(0.5f * diag / kEditorUnitsPerMeter, 0.001f);
}

void AppendAliasModelInstance(const ParsedAliasModel &mdl, const EntityModelSpec &spec,
                              uint32_t aliasInstanceId, EditorScene &scene,
                              std::unordered_map<std::string, size_t> &surfaceByTexture,
                              std::vector<EditorSurface> &surfaces)
{
    if (!mdl.valid || mdl.skinTextures.empty() || mdl.framePoses.empty())
    {
        return;
    }

    const size_t skinIndex = ClampAliasIndex(spec.skin, mdl.skinTextures.size());
    const size_t frameIndex = ClampAliasIndex(spec.frame, mdl.framePoses.size());
    const int32_t textureIndex = mdl.skinTextures[skinIndex];

    if (textureIndex < 0 || size_t(textureIndex) >= scene.textures.size())
    {
        return;
    }

    const std::string &texName = scene.textures[size_t(textureIndex)].name;
    EditorSurface &surface = FindOrCreateBspSurface(texName, textureIndex, surfaceByTexture, surfaces);
    if ((mdl.flags & 0x4000) != 0)
    {
        surface.kind = EditorSurfaceKind::Fence;
    }

    const EntityTransform xf = BuildEntityTransform(spec.origin, spec.angles);
    const std::vector<AliasPoseVertex> &pose = mdl.framePoses[frameIndex];
    const uint32_t entityCluster = ResolveSurfaceCluster(scene, spec.origin, kAnorms[5]);
    const uint32_t cluster = entityCluster != 0 ? entityCluster : ResolveLightCluster(scene, spec.origin);
    const uint32_t resolvedCluster = cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? 0 : cluster;

    EditorAliasInstance instance;
    instance.modelName = Lowercase(spec.modelPath);
    instance.textureName = texName;
    std::copy_n(spec.origin, 3, instance.origin);
    instance.modelSizeMeters = ComputeAliasModelLightSizeMeters(mdl, pose);
    instance.uniqueID = spec.uniqueID;
    instance.aliasInstanceId = aliasInstanceId;
    scene.aliasInstances.push_back(std::move(instance));

    for (const AliasTriangle &tri : mdl.triangles)
    {
        AppendAliasTriangle(mdl, pose, tri, xf, resolvedCluster, aliasInstanceId, surface);
    }
}

void ReadBspPlanes(const std::vector<uint8_t> &bytes, const BspLump &planeLump,
                   std::vector<BspPlane> &planes)
{
    const size_t planeCount = planeLump.length / 20;
    planes.resize(planeCount);

    for (size_t i = 0; i < planeCount; i++)
    {
        const size_t offset = planeLump.offset + i * 20;
        planes[i].normal[0] = ReadLittleEndian<float>(bytes, offset);
        planes[i].normal[1] = ReadLittleEndian<float>(bytes, offset + 4);
        planes[i].normal[2] = ReadLittleEndian<float>(bytes, offset + 8);
        planes[i].distance = ReadLittleEndian<float>(bytes, offset + 12);
    }
}

bool ParseExternalBspModel(const std::vector<uint8_t> &bytes, const std::string &modelPath,
                           const uint8_t *palette768, EditorScene &scene,
                           ParsedExternalBspModel &out)
{
    BspLump lumps[15] = {};
    std::string ignoredError;

    if (!ValidateBspHeaderAndLumps(bytes, modelPath, lumps, out.tables.format, ignoredError))
    {
        return false;
    }

    ReadBspPlanes(bytes, lumps[1], out.tables.planes);

    if (!ReadBspGeometryTables(bytes, lumps, modelPath, out.tables, ignoredError))
    {
        return false;
    }

    out.faceStart = 0;
    out.faceEnd = out.tables.faces.size();

    if (LumpFits(lumps[14], bytes.size(), 64) && lumps[14].length >= 64)
    {
        const int32_t firstFace = ReadLittleEndian<int32_t>(bytes, lumps[14].offset + 56);
        const int32_t numFaces = ReadLittleEndian<int32_t>(bytes, lumps[14].offset + 60);
        out.faceStart = size_t(std::max(0, firstFace));
        out.faceEnd = std::min(out.tables.faces.size(), out.faceStart + size_t(std::max(0, numFaces)));
    }

    out.sceneTexIndices.assign(out.tables.mipTextures.size(), -1);

    for (size_t i = 0; i < out.tables.mipTextures.size(); i++)
    {
        const MipTexture &mip = out.tables.mipTextures[i];

        if (mip.width == 0 || mip.height == 0)
        {
            continue;
        }

        EditorTexture tex;
        ConvertIndexedToRgba(mip, palette768, tex);
        out.sceneTexIndices[i] = FindOrAddSceneTexture(scene, std::move(tex));
    }

    out.valid = true;
    return true;
}

bool ResolveExternalBspTexture(const ParsedExternalBspModel &bsp, const BspFace &face,
                               const EditorScene &scene, const BspTexInfo *&outTexInfo,
                               int32_t &outTextureIndex, std::string &outTextureName)
{
    outTexInfo = (face.texInfo >= 0 && size_t(face.texInfo) < bsp.tables.texInfos.size())
                     ? &bsp.tables.texInfos[size_t(face.texInfo)]
                     : nullptr;
    outTextureIndex = -1;
    outTextureName = "notex";

    if (outTexInfo != nullptr && outTexInfo->miptex >= 0 &&
        size_t(outTexInfo->miptex) < bsp.sceneTexIndices.size())
    {
        outTextureIndex = bsp.sceneTexIndices[size_t(outTexInfo->miptex)];
        if (outTextureIndex >= 0 && size_t(outTextureIndex) < scene.textures.size())
        {
            outTextureName = scene.textures[size_t(outTextureIndex)].name;
        }
    }

    return !IsHiddenBspFaceTexture(outTextureName);
}

struct ExtractedBspPolygon
{
    std::vector<float> localPoly;
    float localNormal[3] = {};
};

bool HasValidBspFaceEdges(const BspGeometryTables &tables, const BspFace &face)
{
    return face.edgeCount >= 3 && face.firstEdge >= 0 &&
           size_t(face.firstEdge) < tables.surfEdges.size() &&
           size_t(face.edgeCount) <= tables.surfEdges.size() - size_t(face.firstEdge);
}

void AppendTransformedBspFacePolygon(const ExtractedBspPolygon &extracted, const BspTexInfo *texInfo,
                                     int32_t textureIndex, const EntityTransform &xf,
                                     EditorScene &scene, EditorSurface &surface)
{
    float worldNormal[3] = {};
    TransformEntityVector(xf, extracted.localNormal, worldNormal);

    const size_t cornerCount = extracted.localPoly.size() / 3;
    std::vector<float> worldPoly(extracted.localPoly.size());
    float centroid[3] = {};

    for (size_t c = 0; c < cornerCount; c++)
    {
        TransformEntityPoint(xf, &extracted.localPoly[c * 3], &worldPoly[c * 3]);
        centroid[0] += worldPoly[c * 3 + 0];
        centroid[1] += worldPoly[c * 3 + 1];
        centroid[2] += worldPoly[c * 3 + 2];
    }

    centroid[0] /= float(cornerCount);
    centroid[1] /= float(cornerCount);
    centroid[2] /= float(cornerCount);

    const uint32_t cluster = ResolveSurfaceCluster(scene, centroid, worldNormal);
    EditorSurfaceFace lightFace;
    std::copy_n(worldNormal, 3, lightFace.normal);
    lightFace.cluster = cluster;

    const uint32_t baseVertex = uint32_t(surface.vertices.size());

    for (size_t c = 0; c < cornerCount; c++)
    {
        const float *worldPos = &worldPoly[c * 3];
        const QrFloat2D uv = ComputeBspTexCoord(&extracted.localPoly[c * 3], texInfo,
                                                surface.kind, scene.textures, textureIndex);
        lightFace.positions.push_back({ worldPos[0], worldPos[1], worldPos[2] });
        lightFace.texCoords.push_back(uv);

        QrVertex vertex = {};
        std::copy_n(worldPos, 3, vertex.position);
        std::copy_n(worldNormal, 3, vertex.normal);
        vertex.texCoord[0] = uv.data[0];
        vertex.texCoord[1] = uv.data[1];
        vertex.packedColor = 0xFFFFFFFFu;
        vertex.cluster = cluster;
        surface.vertices.push_back(vertex);
    }

    surface.faces.push_back(std::move(lightFace));

    for (size_t corner = 1; corner + 1 < cornerCount; corner++)
    {
        surface.indices.push_back(baseVertex + uint32_t(corner + 1));
        surface.indices.push_back(baseVertex + uint32_t(corner));
        surface.indices.push_back(baseVertex);
    }
}

void AppendExternalBspFace(const ParsedExternalBspModel &bsp, size_t faceIndex,
                           const EntityTransform &xf, EditorScene &scene,
                           std::unordered_map<std::string, size_t> &surfaceByTexture,
                           std::vector<EditorSurface> &surfaces)
{
    const BspFace &face = bsp.tables.faces[faceIndex];

    if (!HasValidBspFaceEdges(bsp.tables, face))
    {
        return;
    }

    ExtractedBspPolygon extracted;

    if (!ExtractBspFacePolygon(face, bsp.tables, extracted.localPoly, extracted.localNormal))
    {
        return;
    }

    const BspTexInfo *texInfo = nullptr;
    int32_t textureIndex = -1;
    std::string textureName;

    if (!ResolveExternalBspTexture(bsp, face, scene, texInfo, textureIndex, textureName))
    {
        return;
    }

    EditorSurface &surface =
        FindOrCreateBspSurface(textureName, textureIndex, surfaceByTexture, surfaces);
    AppendTransformedBspFacePolygon(extracted, texInfo, textureIndex, xf, scene, surface);
}

void AppendExternalBspInstance(const ParsedExternalBspModel &bsp, const EntityModelSpec &spec,
                               EditorScene &scene,
                               std::unordered_map<std::string, size_t> &surfaceByTexture,
                               std::vector<EditorSurface> &surfaces)
{
    if (!bsp.valid)
    {
        return;
    }

    const EntityTransform xf = BuildEntityTransform(spec.origin, spec.angles);

    for (size_t faceIndex = bsp.faceStart; faceIndex < bsp.faceEnd; faceIndex++)
    {
        AppendExternalBspFace(bsp, faceIndex, xf, scene, surfaceByTexture, surfaces);
    }
}

uint64_t CountStaticFlameOrTorchEntities(const std::vector<BspEntity> &entities, bool multiplayer)
{
    uint64_t count = 0;

    for (const BspEntity &entity : entities)
    {
        if (!IsBspEntityInhibited(entity, multiplayer) &&
            IsStaticFlameOrTorchClass(entity.Value("classname")))
        {
            count++;
        }
    }

    return count;
}

void AppendBspEntityModels(const std::vector<BspEntity> &entities,
                           const EditorSceneLoadOptions &options, const uint8_t *palette768,
                           EditorScene &scene,
                           std::unordered_map<std::string, size_t> &surfaceByTexture,
                           std::vector<EditorSurface> &surfaces)
{
    if (options.assets == nullptr || entities.empty())
    {
        return;
    }

    const int32_t worldType = std::atoi(entities.front().Value("worldtype").c_str());
    const uint64_t numStatics = CountStaticFlameOrTorchEntities(entities, options.multiplayer);
    uint64_t nextStaticIndex = 0;
    uint32_t nextAliasInstanceId = 1;
    std::unordered_map<std::string, ParsedAliasModel> aliasCache;
    std::unordered_map<std::string, ParsedExternalBspModel> bspCache;

    for (size_t entityIdx = 0; entityIdx < entities.size(); entityIdx++)
    {
        const BspEntity &entity = entities[entityIdx];

        if (IsBspEntityInhibited(entity, options.multiplayer))
        {
            continue;
        }

        const bool isStatic = IsStaticFlameOrTorchClass(entity.Value("classname"));
        const uint64_t entUniqueId = isStatic ? nextStaticIndex++ : (numStatics + uint64_t(entityIdx));

        EntityModelSpec spec;

        if (!ResolveEntityModelSpec(entity, worldType, spec))
        {
            continue;
        }

        spec.uniqueID = (uint64_t(2) << 60) | entUniqueId;

        const std::string key = Lowercase(spec.modelPath);

        if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".mdl") == 0)
        {
            ParsedAliasModel &mdl = aliasCache[key];

            if (!mdl.attempted)
            {
                mdl.attempted = true;
                std::vector<uint8_t> bytes;
                if (options.assets->ReadFile(spec.modelPath, bytes))
                {
                    ParseAliasModel(bytes, key, palette768, scene, mdl);
                }
            }

            AppendAliasModelInstance(mdl, spec, nextAliasInstanceId++, scene, surfaceByTexture,
                                     surfaces);
        }
        else if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".bsp") == 0)
        {
            ParsedExternalBspModel &extBsp = bspCache[key];

            if (!extBsp.attempted)
            {
                extBsp.attempted = true;
                std::vector<uint8_t> bytes;
                if (options.assets->ReadFile(spec.modelPath, bytes))
                {
                    ParseExternalBspModel(bytes, key, palette768, scene, extBsp);
                }
            }

            AppendExternalBspInstance(extBsp, spec, scene, surfaceByTexture, surfaces);
        }
    }
}

void AppendBspFacePolygon(EditorSurface &surface, const BspTexInfo *texInfo,
                          const float faceNormal[3], const std::vector<float> &polygon,
                          int32_t textureIndex, EditorScene &scene)
{
    float centroid[3] = { 0.0f, 0.0f, 0.0f };
    for (size_t corner = 0; corner + 2 < polygon.size(); corner += 3)
    {
        centroid[0] += polygon[corner + 0];
        centroid[1] += polygon[corner + 1];
        centroid[2] += polygon[corner + 2];
    }
    const size_t cornerCount = polygon.size() / 3;
    centroid[0] /= float(cornerCount);
    centroid[1] /= float(cornerCount);
    centroid[2] /= float(cornerCount);

    const uint32_t surfaceCluster = ResolveSurfaceCluster(scene, centroid, faceNormal);

    EditorSurfaceFace lightFace;
    std::copy_n(faceNormal, 3, lightFace.normal);
    lightFace.cluster = surfaceCluster;

    const uint32_t baseVertex = uint32_t(surface.vertices.size());

    for (size_t c = 0; c < cornerCount; c++)
    {
        const float *position = &polygon[c * 3];
        const QrFloat2D texCoord = ComputeBspTexCoord(position, texInfo, surface.kind,
                                                      scene.textures, textureIndex);
        lightFace.positions.push_back({ position[0], position[1], position[2] });
        lightFace.texCoords.push_back(texCoord);

        QrVertex vertex = {};
        std::copy_n(position, 3, vertex.position);
        std::copy_n(faceNormal, 3, vertex.normal);
        vertex.texCoord[0] = texCoord.data[0];
        vertex.texCoord[1] = texCoord.data[1];
        vertex.packedColor = 0xFFFFFFFFu;
        vertex.cluster = surfaceCluster;
        surface.vertices.push_back(vertex);
    }

    surface.faces.push_back(std::move(lightFace));

    for (size_t corner = 1; corner + 1 < cornerCount; corner++)
    {
        surface.indices.push_back(baseVertex + uint32_t(corner + 1));
        surface.indices.push_back(baseVertex + uint32_t(corner));
        surface.indices.push_back(baseVertex);
    }
}

struct BspSurfaceBuildInputs
{
    const std::vector<uint8_t> &bytes;
    const BspLump &modelLump;
    const BspGeometryTables &tables;
    const std::vector<BspEntity> &entities;
    const EditorSceneLoadOptions &options;
    const uint8_t *palette768 = nullptr;
    const std::string &sourceName;
};

bool NonZeroVec3(const float v[3])
{
    return v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f;
}

const BspEntity *FindEntityByTargetName(const std::vector<BspEntity> &entities,
                                        const std::string &targetName)
{
    if (targetName.empty())
    {
        return nullptr;
    }

    for (const BspEntity &entity : entities)
    {
        if (entity.Value("targetname") == targetName)
        {
            return &entity;
        }
    }

    return nullptr;
}

bool BoundsStraddleOrigin(const float mins[3], const float maxs[3])
{
    return mins[0] <= 0.0f && maxs[0] >= 0.0f && mins[1] <= 0.0f && maxs[1] >= 0.0f &&
           mins[2] <= 0.0f && maxs[2] >= 0.0f;
}

bool IsHiddenInlineBrushEntity(const BspEntity &entity, bool multiplayer)
{
    if (IsBspEntityInhibited(entity, multiplayer))
    {
        return true;
    }

    static constexpr const char *kAlwaysHiddenClasses[] = {
        "hub_trigger_changelevel",
        "func_episodegate",
        "func_multi_exploder",
        "func_particlefield",
        "func_togglewall",
    };

    const std::string &className = entity.Value("classname");
    if (className.compare(0, 8, "trigger_") == 0 || className.compare(0, 8, "monster_") == 0)
    {
        return true;
    }

    for (const char *hiddenClass : kAlwaysHiddenClasses)
    {
        if (className == hiddenClass)
        {
            return true;
        }
    }

    if (className == "func_dm_only" && !multiplayer)
    {
        return true;
    }

    return className == "func_movewall" && (std::atoi(entity.Value("spawnflags").c_str()) & 1) == 0;
}

void ResolveRotateEntityOrigin(const BspEntity &entity, const std::vector<BspEntity> &entities,
                               float outOrigin[3])
{
    const std::string &className = entity.Value("classname");
    float pos2[3] = {};

    if (className == "rotate_object_continuously" && ParseVec3Text(entity.Value("pos2"), pos2) &&
        NonZeroVec3(pos2))
    {
        std::copy_n(pos2, 3, outOrigin);
        return;
    }

    if (ParseVec3Text(entity.Value("origin"), outOrigin))
    {
        return;
    }

    if (const BspEntity *target = FindEntityByTargetName(entities, entity.Value("target")))
    {
        ParseVec3Text(target->Value("origin"), outOrigin);
    }
}

void ResolveInlineBrushEntityOrigin(const BspEntity &entity, const std::vector<BspEntity> &entities,
                                    const float modelMins[3], const float modelMaxs[3],
                                    float outOrigin[3])
{
    std::fill_n(outOrigin, 3, 0.0f);
    const std::string &className = entity.Value("classname");

    if (className == "rotate_object_continuously" || className == "rotate_object")
    {
        ResolveRotateEntityOrigin(entity, entities, outOrigin);
        return;
    }

    if ((className == "func_train" || className == "func_train2") &&
        !entity.Value("target").empty())
    {
        float targetOrigin[3] = {};
        const BspEntity *target = FindEntityByTargetName(entities, entity.Value("target"));
        if (target != nullptr && ParseVec3Text(target->Value("origin"), targetOrigin))
        {
            for (int axis = 0; axis < 3; axis++)
            {
                outOrigin[axis] = targetOrigin[axis] - (modelMins[axis] - 1.0f);
            }
        }
        return;
    }

    float parsedOrigin[3] = {};
    if (ParseVec3Text(entity.Value("origin"), parsedOrigin) && NonZeroVec3(parsedOrigin) &&
        BoundsStraddleOrigin(modelMins, modelMaxs))
    {
        std::copy_n(parsedOrigin, 3, outOrigin);
    }
}

void AppendInlineBspSubmodel(const BspSurfaceBuildInputs &inputs, size_t modelIndex,
                             const EntityTransform &xf, EditorScene &scene,
                             std::unordered_map<std::string, size_t> &surfaceByTexture,
                             std::vector<EditorSurface> &surfaces)
{
    const size_t modelOffset = inputs.modelLump.offset + modelIndex * 64;
    const int32_t firstFace = ReadLittleEndian<int32_t>(inputs.bytes, modelOffset + 56);
    const int32_t numFaces = ReadLittleEndian<int32_t>(inputs.bytes, modelOffset + 60);
    const int64_t rawEnd = int64_t(firstFace) + int64_t(numFaces);

    if (numFaces <= 0 || rawEnd <= 0)
    {
        return;
    }

    const size_t startFace = size_t(std::max<int64_t>(firstFace, 0));
    const size_t endFace = std::min<size_t>(size_t(rawEnd), inputs.tables.faces.size());
    ExtractedBspPolygon extracted;

    for (size_t faceIndex = startFace; faceIndex < endFace; faceIndex++)
    {
        const BspFace &face = inputs.tables.faces[faceIndex];

        if (!HasValidBspFaceEdges(inputs.tables, face) ||
            !ExtractBspFacePolygon(face, inputs.tables, extracted.localPoly,
                                   extracted.localNormal))
        {
            continue;
        }

        const BspTexInfo *texInfo =
            (face.texInfo >= 0 && size_t(face.texInfo) < inputs.tables.texInfos.size())
                ? &inputs.tables.texInfos[size_t(face.texInfo)]
                : nullptr;

        int32_t textureIndex = -1;
        std::string textureName;

        if (!ResolveBspFaceTexture(texInfo, scene.textures, inputs.tables.mipTextures, textureIndex,
                                   textureName))
        {
            continue;
        }

        EditorSurface &surface =
            FindOrCreateBspSurface(textureName, textureIndex, surfaceByTexture, surfaces);
        AppendTransformedBspFacePolygon(extracted, texInfo, textureIndex, xf, scene, surface);
    }
}

void AppendInlineBspSubmodels(const BspSurfaceBuildInputs &inputs, EditorScene &scene,
                              std::unordered_map<std::string, size_t> &surfaceByTexture,
                              std::vector<EditorSurface> &surfaces)
{
    if (!LumpFits(inputs.modelLump, inputs.bytes.size(), 64))
    {
        return;
    }

    const size_t modelCount = inputs.modelLump.length / 64;
    constexpr float kZeroAngles[3] = { 0.0f, 0.0f, 0.0f };

    for (const BspEntity &entity : inputs.entities)
    {
        if (IsHiddenInlineBrushEntity(entity, inputs.options.multiplayer))
        {
            continue;
        }

        const std::string &modelName = entity.Value("model");

        if (modelName.size() < 2 || modelName[0] != '*')
        {
            continue;
        }

        const int modelIndex = std::atoi(modelName.c_str() + 1);

        if (modelIndex <= 0 || size_t(modelIndex) >= modelCount)
        {
            continue;
        }

        const size_t modelOffset = inputs.modelLump.offset + size_t(modelIndex) * 64;
        float modelMins[3] = {};
        float modelMaxs[3] = {};
        for (int axis = 0; axis < 3; axis++)
        {
            modelMins[axis] = ReadLittleEndian<float>(inputs.bytes, modelOffset + size_t(axis) * 4);
            modelMaxs[axis] =
                ReadLittleEndian<float>(inputs.bytes, modelOffset + 12 + size_t(axis) * 4);
        }

        float origin[3] = {};
        ResolveInlineBrushEntityOrigin(entity, inputs.entities, modelMins, modelMaxs, origin);
        const EntityTransform xf = BuildEntityTransform(origin, kZeroAngles);
        AppendInlineBspSubmodel(inputs, size_t(modelIndex), xf, scene, surfaceByTexture, surfaces);
    }
}

bool BuildBspSurfaces(const BspSurfaceBuildInputs &inputs, EditorScene &scene, std::string &error)
{
    std::vector<EditorSurface> surfaces;
    std::unordered_map<std::string, size_t> surfaceByTexture;
    std::vector<float> polygon;

    for (size_t faceIndex = 0; faceIndex < inputs.tables.faces.size(); faceIndex++)
    {
        const BspFace &face = inputs.tables.faces[faceIndex];
        if (face.edgeCount < 3 || face.firstEdge < 0 || inputs.tables.hiddenFaces[faceIndex] != 0)
        {
            continue;
        }

        if (size_t(face.firstEdge) >= inputs.tables.surfEdges.size() ||
            size_t(face.edgeCount) > inputs.tables.surfEdges.size() - size_t(face.firstEdge))
        {
            error = "BSP face references invalid surfedges in " + inputs.sourceName;
            return false;
        }

        float faceNormal[3] = {};
        if (!ExtractBspFacePolygon(face, inputs.tables, polygon, faceNormal))
        {
            continue;
        }

        const BspTexInfo *texInfo =
            (face.texInfo >= 0 && size_t(face.texInfo) < inputs.tables.texInfos.size())
                ? &inputs.tables.texInfos[size_t(face.texInfo)]
                : nullptr;

        int32_t textureIndex = -1;
        std::string textureName;
        if (!ResolveBspFaceTexture(texInfo, scene.textures, inputs.tables.mipTextures, textureIndex,
                                   textureName))
        {
            continue;
        }

        EditorSurface &surface =
            FindOrCreateBspSurface(textureName, textureIndex, surfaceByTexture, surfaces);
        AppendBspFacePolygon(surface, texInfo, faceNormal, polygon, textureIndex, scene);
    }

    AppendInlineBspSubmodels(inputs, scene, surfaceByTexture, surfaces);
    AppendBspEntityModels(inputs.entities, inputs.options, inputs.palette768, scene,
                          surfaceByTexture, surfaces);
    AttachAnimationFrameTextures(scene, surfaceByTexture, surfaces);

    if (surfaces.empty())
    {
        error = "no triangles in " + inputs.sourceName;
        return false;
    }

    for (const EditorSurface &surface : surfaces)
    {
        const uint32_t offset = uint32_t(scene.vertices.size());
        scene.vertices.insert(scene.vertices.end(), surface.vertices.begin(),
                              surface.vertices.end());
        for (uint32_t index : surface.indices)
        {
            scene.indices.push_back(offset + index);
        }
    }

    scene.surfaces = std::move(surfaces);
    return true;
}

void BuildPreviewModelFrame(const ParsedAliasModel &mdl, size_t frameIndex,
                            EditorPreviewModelFrame &outFrame)
{
    const std::vector<AliasPoseVertex> &pose = mdl.framePoses[frameIndex];
    if (frameIndex < mdl.frameNames.size())
    {
        outFrame.name = mdl.frameNames[frameIndex];
    }

    outFrame.vertices.reserve(mdl.triangles.size() * 3);
    outFrame.indices.reserve(mdl.triangles.size() * 3);

    float minP[3] = { 1e30f, 1e30f, 1e30f };
    float maxP[3] = { -1e30f, -1e30f, -1e30f };

    for (const AliasTriangle &tri : mdl.triangles)
    {
        for (int corner = 0; corner < 3; corner++)
        {
            const QrVertex v = BuildAliasLocalVertex(mdl, pose, tri, corner);
            for (int axis = 0; axis < 3; axis++)
            {
                minP[axis] = std::min(minP[axis], v.position[axis]);
                maxP[axis] = std::max(maxP[axis], v.position[axis]);
            }
            outFrame.indices.push_back(uint32_t(outFrame.vertices.size()));
            outFrame.vertices.push_back(v);
        }
    }

    if (outFrame.vertices.empty())
    {
        return;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        outFrame.center[axis] = 0.5f * (minP[axis] + maxP[axis]);
    }

    for (QrVertex &v : outFrame.vertices)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            v.position[axis] -= outFrame.center[axis];
        }
    }

    const float dx = maxP[0] - minP[0];
    const float dy = maxP[1] - minP[1];
    const float dz = maxP[2] - minP[2];
    const float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    outFrame.radius = std::max(0.5f * diag, 1.0f);
}

}

void SetEditorLightIntensity(EditorLight &light, float intensity)
{
    light.intensity = intensity;

    for (int channel = 0; channel < 3; channel++)
    {
        light.color[channel] = intensity * kEditorLightIntensityScale * light.tint[channel];
    }
}

void SetEditorLightTint(EditorLight &light, const float tint[3])
{
    for (int channel = 0; channel < 3; channel++)
    {
        light.tint[channel] = tint[channel];
    }

    SetEditorLightIntensity(light, light.intensity);
}

void ClampEditorLightCone(EditorLight &light)
{
    light.angleOuter = std::min(90.0f, std::max(0.0f, light.angleOuter));
    light.angleInner = std::min(light.angleOuter, std::max(0.0f, light.angleInner));
}

uint32_t ResolveEditorLightCluster(const EditorScene &scene, const float origin[3])
{
    return ResolveLightCluster(scene, origin);
}

namespace
{

struct EditorFormatAdapterRegistry
{
    bool registered = false;
    EditorFormatAdapter adapter;
};

EditorFormatAdapterRegistry &FormatAdapterRegistry()
{
    static EditorFormatAdapterRegistry registry;
    return registry;
}

} // namespace

void RegisterEditorFormatAdapter(const EditorFormatAdapter &adapter)
{
    EditorFormatAdapterRegistry &registry = FormatAdapterRegistry();
    registry.adapter = adapter;
    registry.registered = true;
}

const EditorFormatAdapter *GetEditorFormatAdapter()
{
    const EditorFormatAdapterRegistry &registry = FormatAdapterRegistry();
    return registry.registered ? &registry.adapter : nullptr;
}

std::string NormalizeEditorAssetPath(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());

    for (char ch : raw)
    {
        const char normalized = (ch == '\\') ? '/' : ch;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(normalized))));
    }

    while (!out.empty() && out.front() == '/')
    {
        out.erase(out.begin());
    }

    while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
    {
        out.erase(0, 2);
    }

    return out;
}

std::string StripEditorImageExtension(std::string_view path)
{
    static constexpr std::array<std::string_view, 6> kExts = {
        ".tga", ".jpg", ".jpeg", ".png", ".pcx", ".bmp"
    };

    std::string normalized = NormalizeEditorAssetPath(path);
    for (const std::string_view ext : kExts)
    {
        if (normalized.size() > ext.size() &&
            normalized.compare(normalized.size() - ext.size(), ext.size(), ext) == 0)
        {
            return normalized.substr(0, normalized.size() - ext.size());
        }
    }

    return normalized;
}

float EvaluateEditorWaveSigned(EditorWaveType type, float base, float amplitude, float phase,
                               float frequency, float timeSeconds)
{
    if (type == EditorWaveType::None)
    {
        return 0.0f;
    }

    constexpr float kTwoPi = 6.28318530717958647692f;
    const float argument = timeSeconds * frequency + phase;
    float value = 0.0f;
    switch (type)
    {
    case EditorWaveType::Sin:
        value = std::sin(argument * kTwoPi);
        break;
    case EditorWaveType::Square:
        value = std::sin(argument * kTwoPi) >= 0.0f ? 1.0f : -1.0f;
        break;
    case EditorWaveType::Triangle:
    {
        const float cycle = argument - std::floor(argument);
        value = cycle < 0.5f ? (cycle * 4.0f - 1.0f) : (3.0f - cycle * 4.0f);
        break;
    }
    case EditorWaveType::Sawtooth:
        value = argument - std::floor(argument);
        break;
    case EditorWaveType::InverseSawtooth:
        value = 1.0f - (argument - std::floor(argument));
        break;
    case EditorWaveType::Noise:
    {
        const uint32_t bits = static_cast<uint32_t>(argument * 4096.0f);
        uint32_t hash = bits * 747796405u + 2891336453u;
        hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
        hash = (hash >> 22u) ^ hash;
        value = float(hash & 0x00FFFFFFu) / 16777215.0f;
        break;
    }
    case EditorWaveType::None:
    default:
        return 0.0f;
    }
    return base + amplitude * value;
}

float EvaluateEditorWave(EditorWaveType type, float base, float amplitude, float phase,
                         float frequency, float timeSeconds)
{
    if (type == EditorWaveType::None)
    {
        return 1.0f;
    }
    return std::clamp(EvaluateEditorWaveSigned(type, base, amplitude, phase, frequency, timeSeconds),
                      0.0f, 8.0f);
}

void OffsetEditorVerticesAlongSmoothNormals(std::vector<QrVertex> &vertices,
                                            const std::vector<uint32_t> &indices, float distance)
{
    if (distance == 0.0f || vertices.empty())
    {
        return;
    }

    struct Int3Key
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        bool operator==(const Int3Key &o) const = default;
    };
    struct Int3Hash
    {
        size_t operator()(const Int3Key &k) const noexcept
        {
            size_t h = size_t(uint32_t(k.x)) * 73856093u;
            h ^= size_t(uint32_t(k.y)) * 19349663u;
            h ^= size_t(uint32_t(k.z)) * 83492791u;
            return h;
        }
    };
    const auto makeKey = [](const float p[3]) {
        return Int3Key{
            int32_t(std::llround(p[0] * 8.0f)),
            int32_t(std::llround(p[1] * 8.0f)),
            int32_t(std::llround(p[2] * 8.0f)),
        };
    };

    std::unordered_map<Int3Key, std::array<float, 3>, Int3Hash> smooth;
    smooth.reserve(vertices.size());
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            continue;
        }
        const float *a = vertices[i0].position;
        const float *b = vertices[i1].position;
        const float *c = vertices[i2].position;
        const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float fn[3] = {
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0],
        };
        const float lenSq = fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2];
        if (!(lenSq > 1.0e-10f))
        {
            continue;
        }
        const float inv = 1.0f / std::sqrt(lenSq);
        fn[0] *= inv;
        fn[1] *= inv;
        fn[2] *= inv;
        for (uint32_t idx : { i0, i1, i2 })
        {
            auto &acc = smooth[makeKey(vertices[idx].position)];
            acc[0] += fn[0];
            acc[1] += fn[1];
            acc[2] += fn[2];
        }
    }

    for (QrVertex &v : vertices)
    {
        float n[3] = { v.normal[0], v.normal[1], v.normal[2] };
        const auto it = smooth.find(makeKey(v.position));
        if (it != smooth.end())
        {
            n[0] = it->second[0];
            n[1] = it->second[1];
            n[2] = it->second[2];
        }
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1.0e-5f)
        {
            v.position[0] += (n[0] / len) * distance;
            v.position[1] += (n[1] / len) * distance;
            v.position[2] += (n[2] / len) * distance;
        }
    }
}

bool LoadEditorSceneBspMemory(const std::vector<uint8_t> &bytes, const std::string &sourceName,
                              const uint8_t *palette768, EditorScene &scene, std::string &error,
                              const EditorSceneLoadOptions &options)
{
    scene = EditorScene{};

    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
        adapter != nullptr && adapter->canLoadBspMemory != nullptr &&
        adapter->loadBspSceneMemory != nullptr && adapter->canLoadBspMemory(bytes))
    {
        return adapter->loadBspSceneMemory(bytes, sourceName, scene, error, options);
    }

    BspLump lumps[15] = {};
    BspGeometryTables tables;
    if (!ValidateBspHeaderAndLumps(bytes, sourceName, lumps, tables.format, error))
    {
        return false;
    }

    std::vector<BspEntity> entities;
    if (!ParseBspEntities(bytes, lumps[0], entities))
    {
        error = "malformed BSP entity lump in " + sourceName;
        return false;
    }

    ReadBspPlanes(bytes, lumps[1], tables.planes);

    std::string treeError;
    if (!ParseBspTreeAndVis(bytes, lumps, tables.planes, tables.format, scene, treeError))
    {
        error = treeError + " in " + sourceName;
        return false;
    }

    PopulateBspSpawnAndLights(entities, options.multiplayer, scene);

    if (!ReadBspGeometryTables(bytes, lumps, sourceName, tables, error))
    {
        return false;
    }

    BuildBspTexturesAndSky(tables, palette768, scene);

    const BspSurfaceBuildInputs buildInputs{
        bytes, lumps[14], tables, entities, options, palette768, sourceName,
    };
    if (!BuildBspSurfaces(buildInputs, scene, error))
    {
        return false;
    }

    UpdateSceneBounds(scene);
    return true;
}

bool LoadEditorSceneBsp(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                        std::string &error, const EditorSceneLoadOptions &options)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }

    const std::streamsize length = file.tellg();

    if (length < 124)
    {
        error = "invalid BSP header in " + path;
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(length), 0);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), length);

    if (!file)
    {
        error = "cannot read " + path;
        return false;
    }

    return LoadEditorSceneBspMemory(bytes, path, palette768, scene, error, options);
}

bool LoadEditorScene(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                     std::string &error, const EditorSceneLoadOptions &options)
{
    std::string extension;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("\\/");

    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        extension = path.substr(dot);

        for (char &character : extension)
        {
            character = char(std::tolower(static_cast<unsigned char>(character)));
        }
    }

    if (extension == ".bsp")
    {
        return LoadEditorSceneBsp(path, palette768, scene, error, options);
    }

    return LoadEditorSceneObj(path, scene, error);
}

bool LoadEditorAliasModel(const std::string &modelPath, const EditorAssetSource &assets,
                          const uint8_t *palette768, EditorPreviewModel &outModel,
                          std::string &error)
{
    outModel = EditorPreviewModel{};
    std::vector<uint8_t> bytes;

    if (!assets.ReadFile(modelPath, bytes))
    {
        error = "cannot read model " + modelPath;
        return false;
    }

    ParsedAliasModel mdl;
    AliasHeaderCounts counts;
    size_t cursor = 84;

    if (!ValidateAliasHeader(bytes, mdl, counts) ||
        !DecodeAliasSkins(bytes, counts.numSkins, modelPath, palette768, cursor, mdl) ||
        !ParseAliasStVertsAndTris(bytes, counts.numVerts, counts.numTris, cursor, mdl) ||
        !ParseAliasFrames(bytes, counts.numVerts, counts.numFrames, cursor, mdl) ||
        mdl.previewSkins.empty() || mdl.framePoses.empty())
    {
        error = "invalid or unsupported IDPO v6 MDL " + modelPath;
        return false;
    }

    outModel.path = Lowercase(modelPath);
    outModel.holey = (mdl.flags & 0x4000) != 0;
    outModel.skins = std::move(mdl.previewSkins);
    outModel.frames.resize(mdl.framePoses.size());

    for (size_t f = 0; f < mdl.framePoses.size(); f++)
    {
        BuildPreviewModelFrame(mdl, f, outModel.frames[f]);
    }

    return true;
}

bool LoadEditorPreviewModel(const std::string &modelPath, const EditorAssetSource &assets,
                            const uint8_t *palette768, EditorPreviewModel &outModel,
                            std::string &error)
{
    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
        adapter != nullptr && adapter->canLoadPreviewModel != nullptr &&
        adapter->loadPreviewModel != nullptr && adapter->canLoadPreviewModel(modelPath, assets))
    {
        return adapter->loadPreviewModel(modelPath, assets, outModel, error);
    }

    return LoadEditorAliasModel(modelPath, assets, palette768, outModel, error);
}
