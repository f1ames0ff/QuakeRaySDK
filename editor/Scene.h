#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <qray/qray.h>

struct EditorScene
{
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
};

bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error);
