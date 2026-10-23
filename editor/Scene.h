#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <qray/qray.h>

// How a BSP surface behaves in the renderer, derived from its texture name the
// same way gl_model.c classifies TEX_SPECIAL surfaces.
enum class EditorSurfaceKind
{
    Opaque,
    Fence,
    Water,
    Slime,
    Lava,
    Teleport,
    Sky,
};

struct EditorTexture
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// One texture's worth of faces: the editor uploads each group as its own
// geometry with that texture's material, the way r_world.c uploads one surface
// per material at a time.
struct EditorSurface
{
    std::string textureName;
    int32_t texture = -1;
    std::vector<int32_t> animationFrames;
    EditorSurfaceKind kind = EditorSurfaceKind::Opaque;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
};

struct EditorLight
{
    float position[3] = {};
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 300.0f;
    float radius = 0.0f;
    std::string className;
};

struct EditorScene
{
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<EditorTexture> textures;
    std::vector<EditorSurface> surfaces;
    std::vector<EditorLight> lights;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    float cameraPosition[3] = {};
    float cameraYawDegrees = 0.0f;
    bool hasBounds = false;
    bool hasCamera = false;
};

// palette768 is the 768-byte Quake palette (gfx/palette.lmp); when it is null
// the palette stored inside each BSP texture is used instead.
bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error);
bool LoadEditorSceneBsp(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                        std::string &error);
bool LoadEditorScene(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                     std::string &error);
