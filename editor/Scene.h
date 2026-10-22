#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <qray/qray.h>

struct EditorScene
{
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    float cameraPosition[3] = {};
    float cameraYawDegrees = 0.0f;
    bool hasBounds = false;
    bool hasCamera = false;
};

bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error);
bool LoadEditorSceneBsp(const std::string &path, EditorScene &scene, std::string &error);
bool LoadEditorScene(const std::string &path, EditorScene &scene, std::string &error);
