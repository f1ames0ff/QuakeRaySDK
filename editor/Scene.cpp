#include "Scene.h"

#include <algorithm>
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

}

bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error)
{
    std::ifstream file(path);

    if (!file)
    {
        error = "cannot open " + path;
        return false;
    }

    std::vector<Vec3> positions;
    std::vector<Vec2> texCoords;
    std::vector<Vec3> normals;
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
            positions.push_back(values);
        }
        else if (type == "vt")
        {
            Vec2 values;
            stream >> values.value[0] >> values.value[1];
            texCoords.push_back(values);
        }
        else if (type == "vn")
        {
            Vec3 values;
            stream >> values.value[0] >> values.value[1] >> values.value[2];
            normals.push_back(values);
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

                corner.position = ResolveIndex(corner.position, positions.size());
                corner.texCoord = corner.texCoord != 0 ? ResolveIndex(corner.texCoord, texCoords.size()) : -1;
                corner.normal = corner.normal != 0 ? ResolveIndex(corner.normal, normals.size()) : -1;

                if (corner.position < 0 || corner.position >= int(positions.size()))
                {
                    error = "face position out of range in " + path;
                    return false;
                }

                if (corner.texCoord >= int(texCoords.size()) || corner.normal >= int(normals.size()))
                {
                    error = "face attribute out of range in " + path;
                    return false;
                }

                corners.push_back(corner);
            }

            if (corners.size() < 3)
            {
                continue;
            }

            for (size_t i = 1; i + 1 < corners.size(); i++)
            {
                const ObjCorner triangle[3] = { corners[0], corners[i], corners[i + 1] };
                float edge0[3] = {};
                float edge1[3] = {};
                float faceNormal[3] = {};

                for (int axis = 0; axis < 3; axis++)
                {
                    edge0[axis] = positions[triangle[1].position].value[axis] -
                                  positions[triangle[0].position].value[axis];
                    edge1[axis] = positions[triangle[2].position].value[axis] -
                                  positions[triangle[0].position].value[axis];
                }

                faceNormal[0] = edge0[1] * edge1[2] - edge0[2] * edge1[1];
                faceNormal[1] = edge0[2] * edge1[0] - edge0[0] * edge1[2];
                faceNormal[2] = edge0[0] * edge1[1] - edge0[1] * edge1[0];

                const float length = std::sqrt(faceNormal[0] * faceNormal[0] +
                                               faceNormal[1] * faceNormal[1] +
                                               faceNormal[2] * faceNormal[2]);

                if (length > 0.000001f)
                {
                    faceNormal[0] /= length;
                    faceNormal[1] /= length;
                    faceNormal[2] /= length;
                }

                for (int cornerIndex = 0; cornerIndex < 3; cornerIndex++)
                {
                    const ObjCorner &corner = triangle[cornerIndex];
                    QrVertex vertex = {};
                    vertex.position[0] = positions[corner.position].value[0];
                    vertex.position[1] = positions[corner.position].value[1];
                    vertex.position[2] = positions[corner.position].value[2];

                    if (corner.normal >= 0)
                    {
                        vertex.normal[0] = normals[corner.normal].value[0];
                        vertex.normal[1] = normals[corner.normal].value[1];
                        vertex.normal[2] = normals[corner.normal].value[2];
                    }
                    else
                    {
                        vertex.normal[0] = faceNormal[0];
                        vertex.normal[1] = faceNormal[1];
                        vertex.normal[2] = faceNormal[2];
                    }

                    if (corner.texCoord >= 0)
                    {
                        vertex.texCoord[0] = texCoords[corner.texCoord].value[0];
                        vertex.texCoord[1] = texCoords[corner.texCoord].value[1];
                    }

                    vertex.packedColor = 0xFFFFFFFFu;
                    scene.indices.push_back(uint32_t(scene.vertices.size()));
                    scene.vertices.push_back(vertex);
                }
            }
        }
    }

    if (scene.vertices.empty() || scene.indices.empty())
    {
        error = "no triangles in " + path;
        return false;
    }

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
    uint16_t vertex[2] = {};
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
    int16_t plane = 0;
    int16_t side = 0;
    int32_t firstEdge = 0;
    int16_t edgeCount = 0;
    int16_t texInfo = 0;
};

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
// centimetre, which is 40 scene units at the SDK's 40 units per metre. Map light
// colours stay at the default light colour, exactly as the game ignores
// `_color`/`color` keys and the stored has_color flag.
constexpr float kQuakeUnitsPerMeter = 40.0f;
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

// Quake stores its wall textures inside the BSP (lump 2): a miptex directory
// followed by an embedded 8-bit image with four mip levels and a palette.
bool DecodeBspTextures(const std::vector<uint8_t> &bytes, const BspLump &lump,
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

        if (dataOffset < 0 || size_t(dataOffset) + 40 > lump.length)
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
        const size_t available = lump.length - size_t(dataOffset) - 40;

        if (pixelCount > available)
        {
            texture.width = 0;
            texture.height = 0;
            continue;
        }

        texture.indices.assign(bytes.data() + base + 40,
                               bytes.data() + base + 40 + pixelCount);

        const size_t paletteOffset = pixelCount / 64 * 85;

        if (paletteOffset + 768 <= available)
        {
            texture.palette.assign(bytes.data() + base + 40 + paletteOffset,
                                   bytes.data() + base + 40 + paletteOffset + 768);
        }
    }

    return true;
}

void ConvertIndexedToRgba(const MipTexture &texture, const uint8_t *palette768,
                          EditorTexture &out)
{
    out.name = texture.name;
    out.width = texture.width;
    out.height = texture.height;
    out.rgba.assign(size_t(texture.width) * size_t(texture.height) * 4, 255);

    const uint8_t *palette = palette768;

    if (palette == nullptr && texture.palette.size() >= 768)
    {
        palette = texture.palette.data();
    }

    for (size_t i = 0; i < texture.indices.size(); i++)
    {
        const uint8_t index = texture.indices[i];
        uint8_t red = index;
        uint8_t green = index;
        uint8_t blue = index;

        if (palette != nullptr)
        {
            red = palette[size_t(index) * 3 + 0];
            green = palette[size_t(index) * 3 + 1];
            blue = palette[size_t(index) * 3 + 2];
        }

        out.rgba[i * 4 + 0] = red;
        out.rgba[i * 4 + 1] = green;
        out.rgba[i * 4 + 2] = blue;
        out.rgba[i * 4 + 3] = 255;
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

// Reads the BSP tree and builds the identity world cluster table (r_world.c
// RT_BuildWorldClustersIdentity): cluster i is leaf i, cluster 0 is the solid
// leaf and cluster numLeafs is the solid edge cluster.
bool ParseBspTree(const std::vector<uint8_t> &bytes, const BspLump &nodeLump,
                  const BspLump &leafLump, const std::vector<BspPlane> &planes,
                  EditorScene &scene, std::string &error)
{
    const size_t nodeCount = nodeLump.length / 24;
    const size_t leafCount = leafLump.length / 28;

    if (leafCount == 0)
    {
        error = "BSP has no leafs";
        return false;
    }

    scene.nodes.resize(nodeCount);

    for (size_t i = 0; i < nodeCount; i++)
    {
        const size_t offset = nodeLump.offset + i * 24;
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
        node.children[0] = ReadLittleEndian<int16_t>(bytes, offset + 4);
        node.children[1] = ReadLittleEndian<int16_t>(bytes, offset + 6);

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

    EditorWorldClusters &clusters = scene.worldClusters;
    clusters.numLeafs = uint32_t(leafCount);
    clusters.mins.resize(leafCount + 1);
    clusters.maxs.resize(leafCount + 1);
    clusters.flags.assign(leafCount + 1, 0);
    clusters.contents.assign(leafCount + 1, kBspContentsSolid);

    QrFloat3D boundsMin = { { 0.0f, 0.0f, 0.0f } };
    QrFloat3D boundsMax = { { 0.0f, 0.0f, 0.0f } };
    bool haveBounds = false;

    for (size_t i = 1; i < leafCount; i++)
    {
        const size_t offset = leafLump.offset + i * 28;
        const int32_t contents = ReadLittleEndian<int32_t>(bytes, offset);

        for (int axis = 0; axis < 3; axis++)
        {
            clusters.mins[i].data[axis] =
                float(ReadLittleEndian<int16_t>(bytes, offset + 8 + size_t(axis) * 2));
            clusters.maxs[i].data[axis] =
                float(ReadLittleEndian<int16_t>(bytes, offset + 14 + size_t(axis) * 2));

            if (!haveBounds)
            {
                boundsMin.data[axis] = clusters.mins[i].data[axis];
                boundsMax.data[axis] = clusters.maxs[i].data[axis];
            }
            else
            {
                boundsMin.data[axis] = std::min(boundsMin.data[axis], clusters.mins[i].data[axis]);
                boundsMax.data[axis] = std::max(boundsMax.data[axis], clusters.maxs[i].data[axis]);
            }
        }

        haveBounds = true;
        clusters.contents[i] = contents;
        clusters.flags[i] = uint8_t((contents == kBspContentsSolid ? QR_WORLD_CLUSTER_SOLID_BIT : 0) |
                                    (IsLiquidContents(contents) ? QR_WORLD_CLUSTER_LIQUID_BIT : 0));
    }

    // Cluster 0 is the hole: the whole map, solid, and the answer for any leaf the tree cannot
    // reach. The edge cluster is the same. Neither one is a light's home.
    for (uint32_t cluster : { uint32_t(0), uint32_t(leafCount) })
    {
        clusters.mins[cluster] = boundsMin;
        clusters.maxs[cluster] = boundsMax;
        clusters.flags[cluster] = QR_WORLD_CLUSTER_SOLID_BIT;
    }

    return true;
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
               clusters.contents[size_t(leaf)] != kBspContentsSolid;
    };

    if (usableLeaf(LeafAtPoint(scene, origin)))
    {
        return uint32_t(LeafAtPoint(scene, origin));
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
                return uint32_t(leaf);
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
            scene.worldClusters.contents[size_t(leaf)] != kBspContentsSolid)
        {
            return uint32_t(leaf);
        }
    }

    return 0;
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

bool LoadEditorSceneBsp(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                        std::string &error)
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

    if (ReadLittleEndian<int32_t>(bytes, 0) != 29)
    {
        error = "unsupported BSP version in " + path + " (only Quake BSP29 is supported)";
        return false;
    }

    BspLump lumps[15] = {};

    for (int i = 0; i < 15; i++)
    {
        lumps[i].offset = ReadLittleEndian<uint32_t>(bytes, 4 + size_t(i) * 8);
        lumps[i].length = ReadLittleEndian<uint32_t>(bytes, 8 + size_t(i) * 8);
    }

    for (const BspLump &lump : lumps)
    {
        if (lump.offset > bytes.size() || lump.length > bytes.size() - lump.offset)
        {
            error = "BSP lump outside the file in " + path;
            return false;
        }
    }

    if (!LumpFits(lumps[1], bytes.size(), 20) ||
        !LumpFits(lumps[3], bytes.size(), 12) ||
        !LumpFits(lumps[5], bytes.size(), 24) ||
        !LumpFits(lumps[6], bytes.size(), 40) ||
        !LumpFits(lumps[7], bytes.size(), 20) ||
        !LumpFits(lumps[10], bytes.size(), 28) ||
        !LumpFits(lumps[12], bytes.size(), 4) ||
        !LumpFits(lumps[13], bytes.size(), 4))
    {
        error = "malformed BSP lump sizes in " + path;
        return false;
    }

    std::vector<BspEntity> entities;

    if (!ParseBspEntities(bytes, lumps[0], entities))
    {
        error = "malformed BSP entity lump in " + path;
        return false;
    }

    const size_t planeCount = lumps[1].length / 20;
    std::vector<BspPlane> planes(planeCount);

    for (size_t i = 0; i < planeCount; i++)
    {
        const size_t offset = lumps[1].offset + i * 20;
        planes[i].normal[0] = ReadLittleEndian<float>(bytes, offset);
        planes[i].normal[1] = ReadLittleEndian<float>(bytes, offset + 4);
        planes[i].normal[2] = ReadLittleEndian<float>(bytes, offset + 8);
        planes[i].distance = ReadLittleEndian<float>(bytes, offset + 12);
    }

    std::string treeError;

    if (!ParseBspTree(bytes, lumps[5], lumps[10], planes, scene, treeError))
    {
        error = treeError + " in " + path;
        return false;
    }

    const BspEntity *spawn = nullptr;

    for (const char *spawnClass : { "info_player_start", "info_player_deathmatch",
                                    "info_player_coop" })
    {
        for (const BspEntity &entity : entities)
        {
            if (entity.Value("classname") == spawnClass)
            {
                spawn = &entity;
                break;
            }
        }

        if (spawn)
        {
            break;
        }
    }

    if (spawn)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        if (std::sscanf(spawn->Value("origin").c_str(), "%f %f %f", &x, &y, &z) == 3)
        {
            scene.cameraPosition[0] = x;
            scene.cameraPosition[1] = y;
            // The Quake player's eye sits 22 units above the entity origin.
            scene.cameraPosition[2] = z + 22.0f;
            scene.cameraYawDegrees = float(std::atof(spawn->Value("angle").c_str()));
            scene.hasCamera = true;
        }
    }

    for (const BspEntity &entity : entities)
    {
        const std::string &className = entity.Value("classname");

        if (!StartsWithLight(className))
        {
            continue;
        }

        float position[3] = {};

        if (std::sscanf(entity.Value("origin").c_str(), "%f %f %f", &position[0],
                        &position[1], &position[2]) != 3)
        {
            continue;
        }

        const std::string &intensityText = entity.Value("light");
        float intensity = intensityText.empty()
                              ? kDefaultLightIntensity
                              : float(std::atof(intensityText.c_str()));

        EditorLight light;
        light.position[0] = position[0];
        light.position[1] = position[1];
        light.position[2] = position[2];
        light.radius = kDefaultLightRadiusMeters * kQuakeUnitsPerMeter;
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

    const size_t vertexCount = lumps[3].length / 12;
    const size_t texInfoCount = lumps[6].length / 40;
    const size_t faceCount = lumps[7].length / 20;
    const size_t edgeCount = lumps[12].length / 4;
    const size_t surfEdgeCount = lumps[13].length / 4;

    std::vector<float> vertices(vertexCount * 3);
    std::vector<BspTexInfo> texInfos(texInfoCount);
    std::vector<BspFace> faces(faceCount);
    std::vector<BspEdge> edges(edgeCount);
    std::vector<int32_t> surfEdges(surfEdgeCount);

    for (size_t i = 0; i < vertexCount; i++)
    {
        const size_t offset = lumps[3].offset + i * 12;
        vertices[i * 3 + 0] = ReadLittleEndian<float>(bytes, offset);
        vertices[i * 3 + 1] = ReadLittleEndian<float>(bytes, offset + 4);
        vertices[i * 3 + 2] = ReadLittleEndian<float>(bytes, offset + 8);
    }

    for (size_t i = 0; i < texInfoCount; i++)
    {
        const size_t offset = lumps[6].offset + i * 40;

        for (int axis = 0; axis < 4; axis++)
        {
            texInfos[i].u[axis] = ReadLittleEndian<float>(bytes, offset + size_t(axis) * 4);
            texInfos[i].v[axis] = ReadLittleEndian<float>(bytes, offset + 16 + size_t(axis) * 4);
        }

        texInfos[i].miptex = ReadLittleEndian<int32_t>(bytes, offset + 32);
        texInfos[i].flags = ReadLittleEndian<int32_t>(bytes, offset + 36);
    }

    for (size_t i = 0; i < faceCount; i++)
    {
        const size_t offset = lumps[7].offset + i * 20;
        faces[i].plane = ReadLittleEndian<int16_t>(bytes, offset);
        faces[i].side = ReadLittleEndian<int16_t>(bytes, offset + 2);
        faces[i].firstEdge = ReadLittleEndian<int32_t>(bytes, offset + 4);
        faces[i].edgeCount = ReadLittleEndian<int16_t>(bytes, offset + 8);
        faces[i].texInfo = ReadLittleEndian<int16_t>(bytes, offset + 10);
    }

    for (size_t i = 0; i < edgeCount; i++)
    {
        const size_t offset = lumps[12].offset + i * 4;
        edges[i].vertex[0] = ReadLittleEndian<uint16_t>(bytes, offset);
        edges[i].vertex[1] = ReadLittleEndian<uint16_t>(bytes, offset + 2);
    }

    for (size_t i = 0; i < surfEdgeCount; i++)
    {
        surfEdges[i] = ReadLittleEndian<int32_t>(bytes, lumps[13].offset + i * 4);
    }

    std::vector<MipTexture> mipTextures;

    if (!DecodeBspTextures(bytes, lumps[2], mipTextures))
    {
        error = "malformed BSP texture lump in " + path;
        return false;
    }

    scene.textures.resize(mipTextures.size());

    for (size_t i = 0; i < mipTextures.size(); i++)
    {
        if (mipTextures[i].width == 0 || mipTextures[i].height == 0)
        {
            continue;
        }

        ConvertIndexedToRgba(mipTextures[i], palette768, scene.textures[i]);
    }

    std::vector<EditorSurface> surfaces;
    std::unordered_map<std::string, size_t> surfaceByTexture;

    auto surfaceForTexture = [&surfaces, &surfaceByTexture](const std::string &textureName,
                                                            int32_t textureIndex) -> EditorSurface & {
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
    };

    for (size_t faceIndex = 0; faceIndex < faceCount; faceIndex++)
    {
        const BspFace &face = faces[faceIndex];

        if (face.edgeCount < 3 || face.firstEdge < 0)
        {
            continue;
        }

        if (size_t(face.firstEdge) >= surfEdgeCount ||
            size_t(face.edgeCount) > surfEdgeCount - size_t(face.firstEdge))
        {
            error = "BSP face references invalid surfedges in " + path;
            return false;
        }

        float faceNormal[3] = {};
        bool haveFaceNormal = false;

        if (face.plane >= 0 && size_t(face.plane) < planeCount)
        {
            const float side = face.side != 0 ? -1.0f : 1.0f;
            faceNormal[0] = planes[face.plane].normal[0] * side;
            faceNormal[1] = planes[face.plane].normal[1] * side;
            faceNormal[2] = planes[face.plane].normal[2] * side;
            haveFaceNormal = true;
        }

        const BspTexInfo *texInfo = nullptr;

        if (face.texInfo >= 0 && size_t(face.texInfo) < texInfoCount)
        {
            texInfo = &texInfos[face.texInfo];
        }

        std::vector<float> polygon;
        polygon.reserve(size_t(face.edgeCount) * 3);

        bool valid = true;

        for (int i = 0; i < face.edgeCount; i++)
        {
            const int32_t surfEdge = surfEdges[size_t(face.firstEdge) + size_t(i)];
            const size_t edgeIndex = size_t(surfEdge < 0 ? -int64_t(surfEdge) : int64_t(surfEdge));

            if (edgeIndex == 0 || edgeIndex >= edgeCount)
            {
                valid = false;
                break;
            }

            const BspEdge &edge = edges[edgeIndex];
            const uint16_t vertexIndex = surfEdge >= 0 ? edge.vertex[0] : edge.vertex[1];

            if (size_t(vertexIndex) >= vertexCount)
            {
                valid = false;
                break;
            }

            polygon.push_back(vertices[size_t(vertexIndex) * 3 + 0]);
            polygon.push_back(vertices[size_t(vertexIndex) * 3 + 1]);
            polygon.push_back(vertices[size_t(vertexIndex) * 3 + 2]);
        }

        if (!valid || polygon.size() < 9)
        {
            continue;
        }

        if (!haveFaceNormal)
        {
            float edge0[3] = {};
            float edge1[3] = {};

            for (int axis = 0; axis < 3; axis++)
            {
                edge0[axis] = polygon[3 + axis] - polygon[axis];
                edge1[axis] = polygon[6 + axis] - polygon[axis];
            }

            faceNormal[0] = edge0[1] * edge1[2] - edge0[2] * edge1[1];
            faceNormal[1] = edge0[2] * edge1[0] - edge0[0] * edge1[2];
            faceNormal[2] = edge0[0] * edge1[1] - edge0[1] * edge1[0];

            const float normalLength = std::sqrt(faceNormal[0] * faceNormal[0] +
                                                 faceNormal[1] * faceNormal[1] +
                                                 faceNormal[2] * faceNormal[2]);

            if (normalLength <= 0.000001f)
            {
                continue;
            }

            faceNormal[0] /= normalLength;
            faceNormal[1] /= normalLength;
            faceNormal[2] /= normalLength;
        }

        int32_t textureIndex = -1;
        std::string textureName = "notex";

        if (texInfo != nullptr && texInfo->miptex >= 0 &&
            size_t(texInfo->miptex) < scene.textures.size() &&
            scene.textures[size_t(texInfo->miptex)].width > 0)
        {
            textureIndex = texInfo->miptex;
            textureName = scene.textures[size_t(textureIndex)].name;
        }
        else if (texInfo != nullptr && texInfo->miptex >= 0 &&
                 size_t(texInfo->miptex) < mipTextures.size() &&
                 !mipTextures[size_t(texInfo->miptex)].name.empty())
        {
            textureName = mipTextures[size_t(texInfo->miptex)].name;
        }

        EditorSurface &surface = surfaceForTexture(textureName, textureIndex);

        float centroid[3] = { 0.0f, 0.0f, 0.0f };

        for (size_t corner = 0; corner + 2 < polygon.size(); corner += 3)
        {
            centroid[0] += polygon[corner + 0];
            centroid[1] += polygon[corner + 1];
            centroid[2] += polygon[corner + 2];
        }

        if (polygon.size() >= 3)
        {
            const float corners = float(polygon.size() / 3);
            centroid[0] /= corners;
            centroid[1] /= corners;
            centroid[2] /= corners;
        }

        const uint32_t surfaceCluster = ResolveSurfaceCluster(scene, centroid, faceNormal);

        for (size_t corner = 1; corner + 1 < polygon.size() / 3; corner++)
        {
            const size_t sourceCorners[3] = { 0, corner, corner + 1 };

            for (size_t i = 0; i < 3; i++)
            {
                const float *position = &polygon[sourceCorners[i] * 3];
                QrVertex vertex = {};
                vertex.position[0] = position[0];
                vertex.position[1] = position[1];
                vertex.position[2] = position[2];
                vertex.normal[0] = faceNormal[0];
                vertex.normal[1] = faceNormal[1];
                vertex.normal[2] = faceNormal[2];

                if (texInfo != nullptr)
                {
                    vertex.texCoord[0] = Dot(position, texInfo->u) / 32.0f;
                    vertex.texCoord[1] = Dot(position, texInfo->v) / 32.0f;
                }

                vertex.packedColor = 0xFFFFFFFFu;
                vertex.cluster = surfaceCluster;
                surface.indices.push_back(uint32_t(surface.vertices.size()));
                surface.vertices.push_back(vertex);
            }
        }
    }

    if (surfaces.empty())
    {
        error = "no triangles in " + path;
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

    UpdateSceneBounds(scene);
    return true;
}

bool LoadEditorScene(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                     std::string &error)
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
        return LoadEditorSceneBsp(path, palette768, scene, error);
    }

    return LoadEditorSceneObj(path, scene, error);
}
