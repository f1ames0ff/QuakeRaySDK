#include "Scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

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
};

struct BspEdge
{
    uint16_t vertex[2] = {};
};

struct BspTexInfo
{
    float u[4] = {};
    float v[4] = {};
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

float Dot(const float a[3], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
}

}

bool LoadEditorSceneBsp(const std::string &path, EditorScene &scene, std::string &error)
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
        !LumpFits(lumps[6], bytes.size(), 40) ||
        !LumpFits(lumps[7], bytes.size(), 20) ||
        !LumpFits(lumps[12], bytes.size(), 4) ||
        !LumpFits(lumps[13], bytes.size(), 4))
    {
        error = "malformed BSP lump sizes in " + path;
        return false;
    }

    if (lumps[0].length > 0)
    {
        const std::string entities(
            reinterpret_cast<const char *>(bytes.data() + lumps[0].offset),
            lumps[0].length);
        const size_t spawn = entities.find("\"classname\" \"info_player_start\"");

        if (spawn != std::string::npos)
        {
            const size_t blockStart = entities.rfind('{', spawn);
            const size_t blockEnd = entities.find('}', spawn);

            if (blockStart != std::string::npos && blockEnd != std::string::npos)
            {
                const std::string block = entities.substr(blockStart, blockEnd - blockStart);

                auto readValue = [&block](const char *key) {
                    const std::string quotedKey = "\"" + std::string(key) + "\"";
                    const size_t keyPosition = block.find(quotedKey);

                    if (keyPosition == std::string::npos)
                    {
                        return std::string();
                    }

                    const size_t valueStart = block.find('"', keyPosition + quotedKey.size());

                    if (valueStart == std::string::npos)
                    {
                        return std::string();
                    }

                    const size_t valueEnd = block.find('"', valueStart + 1);

                    if (valueEnd == std::string::npos)
                    {
                        return std::string();
                    }

                    return block.substr(valueStart + 1, valueEnd - valueStart - 1);
                };

                const std::string origin = readValue("origin");
                const std::string angle = readValue("angle");
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;

                if (std::sscanf(origin.c_str(), "%f %f %f", &x, &y, &z) == 3)
                {
                    scene.cameraPosition[0] = x;
                    scene.cameraPosition[1] = y;
                    scene.cameraPosition[2] = z + 22.0f;
                    scene.cameraYawDegrees = float(std::atof(angle.c_str()));
                    scene.hasCamera = true;
                }
            }
        }
    }

    const size_t vertexCount = lumps[3].length / 12;
    const size_t planeCount = lumps[1].length / 20;
    const size_t texInfoCount = lumps[6].length / 40;
    const size_t faceCount = lumps[7].length / 20;
    const size_t edgeCount = lumps[12].length / 4;
    const size_t surfEdgeCount = lumps[13].length / 4;

    std::vector<float> vertices(vertexCount * 3);
    std::vector<BspPlane> planes(planeCount);
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

    for (size_t i = 0; i < planeCount; i++)
    {
        const size_t offset = lumps[1].offset + i * 20;
        planes[i].normal[0] = ReadLittleEndian<float>(bytes, offset);
        planes[i].normal[1] = ReadLittleEndian<float>(bytes, offset + 4);
        planes[i].normal[2] = ReadLittleEndian<float>(bytes, offset + 8);
    }

    for (size_t i = 0; i < texInfoCount; i++)
    {
        const size_t offset = lumps[6].offset + i * 40;

        for (int axis = 0; axis < 4; axis++)
        {
            texInfos[i].u[axis] = ReadLittleEndian<float>(bytes, offset + size_t(axis) * 4);
            texInfos[i].v[axis] = ReadLittleEndian<float>(bytes, offset + 16 + size_t(axis) * 4);
        }

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
                scene.indices.push_back(uint32_t(scene.vertices.size()));
                scene.vertices.push_back(vertex);
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

bool LoadEditorScene(const std::string &path, EditorScene &scene, std::string &error)
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
        return LoadEditorSceneBsp(path, scene, error);
    }

    return LoadEditorSceneObj(path, scene, error);
}
