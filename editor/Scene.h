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
    // The world cluster the light stands in, resolved the way RT_ResolveLightLeaf resolves a
    // Quake entity light: the leaf at its origin, or the first non-solid leaf a short probe
    // outward finds. QR_CLUSTER_LIGHT_NO_CLUSTER when the map has no leaf table.
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    std::string className;
};

// One node of the BSP tree, holding only what a point-in-leaf walk needs. children >= 0 are
// node indices, children < 0 are leaves addressed as -(child + 1).
struct EditorBspNode
{
    float normal[3] = {};
    float distance = 0.0f;
    int32_t children[2] = {};
};

// The world cluster table of r_world.c RT_BuildWorldClustersIdentity: cluster i is leaf i,
// cluster 0 is the solid leaf, and cluster numLeafs is the solid edge cluster. A light's
// cluster index is therefore its BSP leaf index, exactly as in the game.
struct EditorWorldClusters
{
    std::vector<QrFloat3D> mins;
    std::vector<QrFloat3D> maxs;
    std::vector<uint8_t> flags;
    std::vector<int32_t> contents;
    uint32_t numLeafs = 0;
};

struct EditorScene
{
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<EditorTexture> textures;
    std::vector<EditorSurface> surfaces;
    std::vector<EditorLight> lights;
    std::vector<EditorBspNode> nodes;
    EditorWorldClusters worldClusters;
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
