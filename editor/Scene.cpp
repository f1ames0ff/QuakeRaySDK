#include "Scene.h"

#include <cmath>
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

    return true;
}
