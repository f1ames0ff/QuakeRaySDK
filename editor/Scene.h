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

// One BSP polygon of a surface, kept because a textured area light is built from a face's own
// corners: the vertex list above is a triangle fan that no longer says where one polygon ends and
// the next begins, and a light fitted over a fan that spans two faces would stand for a shape the
// level does not have.
struct EditorSurfaceFace
{
    float normal[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    std::vector<QrFloat3D> positions;
    std::vector<QrFloat2D> texCoords;
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
    // The polygons the vertices above fan out of, in the order Scene.cpp met them.
    std::vector<EditorSurfaceFace> faces;
};

struct EditorLight
{
    float position[3] = {};
    // The hue an author edits. `color` is the radiance the upload carries, so it stays
    // intensity * kEditorLightIntensityScale * tint and moving the `light` key keeps the hue.
    float tint[3] = { 1.0f, 1.0f, 1.0f };
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 300.0f;
    float radius = 0.0f;
    // The world cluster the light stands in, resolved the way RT_ResolveLightLeaf resolves a
    // Quake entity light: the leaf at its origin, or the first non-solid leaf a short probe
    // outward finds. QR_CLUSTER_LIGHT_NO_CLUSTER when the map has no leaf table.
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    // A spot light is a spherical one plus a cone: the game's `spot` key with a direction and
    // the `angle_inner` / `angle_outer` half-angles, both in degrees and clamped to [0, 90]
    // with the inner one never wider than the outer.
    bool spot = false;
    float direction[3] = { 0.0f, 0.0f, 1.0f };
    float angleInner = 0.0f;
    float angleOuter = 30.0f;
    // The light style index the game animates the light with, the same index the map's `style`
    // key names and the same one rt_custom_style_names indexes: 0 is steady, 1..11 are the
    // animated styles. The preview stays at full brightness because a standalone editor has
    // none of the server's lightstyle strings, and an empty string leaves d_lightstylevalue[]
    // at 256 - which is exactly what a map light without a style renders as.
    int32_t style = 0;
    std::string className;
};

// Radiance per unit of a map light's `light` key, matching RT_FIXUP_LIGHT_INTENSITY: the game
// divides by rt_elight_normaliz (100), then multiplies by rt_globallight_mult (10), the
// Quake-to-meter area fix (1 / 0.025^2 = 1600) and rt_brightness (1.0).
inline constexpr float kEditorLightIntensityScale =
    10.0f * (1.0f / (0.025f * 0.025f)) / 100.0f;

// The scene units a metre is: the SDK's spatial convention maps a metre to forty units, so a
// length a level file states in metres is forty times as many units here.
inline constexpr float kEditorUnitsPerMeter = 40.0f;

// The `light` value one unit of a level's custom-light `intensity` key is worth: the game gives
// such a light RT_FIXUP_LIGHT_INTENSITY alone -- 1600 * 10, its area fix times
// rt_globallight_mult -- where a map light also divides by rt_elight_normaliz (100). One file unit
// is therefore a hundred `light` points, and RT_CUSTOM_INTENSITY_DEFAULT's 3 is exactly the
// editor's own 300 default.
inline constexpr float kEditorCustomLightIntensityUnits = 100.0f;

// Writes the radiance the uploaded light carries for this `light` key value. Authoring changes
// light.intensity, so colours must be refreshed through here rather than scaled twice.
void SetEditorLightIntensity(EditorLight &light, float intensity);

// Writes the hue multiplier and refreshes the radiance, so a colour edit does not have to know
// the intensity scale.
void SetEditorLightTint(EditorLight &light, const float tint[3]);

// Clamps the authored cone to the range RT_ClampLightCone accepts: half-angles in [0, 90]
// degrees with the inner one never wider than the outer.
void ClampEditorLightCone(EditorLight &light);

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
    // The skyflatcolor of gl_sky.c: the average of the sky texture's opaque front-layer
    // pixels, over 255. It is the ambient the game gives the volumetric fog, and it is not
    // rt_sky_color: a level whose sky is painted blue still has a warm-grey flat colour
    // because that is the picture, not the tint. False when the level carries no sky.
    float skyFlatColor[3] = {};
    bool hasSkyFlatColor = false;
    // The classic scrolling sky, split the way Sky_LoadTexture splits it: the right half of
    // every row is the opaque layer and the left half the alpha mask, whose zero index is the
    // transparent key. Both layers are half the sky texture's width, and the last sky texture
    // in the level owns them, as in the game.
    std::vector<uint8_t> skySolidRgba;
    std::vector<uint8_t> skyAlphaRgba;
    uint32_t skyLayerWidth = 0;
    uint32_t skyLayerHeight = 0;
    bool hasScrollingSky = false;
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

// Re-resolves the cluster a light stands in, the way the loader does when it reads the map, so
// moving a light in the panel keeps its world cluster light list correct.
uint32_t ResolveEditorLightCluster(const EditorScene &scene, const float origin[3]);
