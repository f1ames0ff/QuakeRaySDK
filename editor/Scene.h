// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <qray/qray.h>

#include "Assets.h"

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

enum class EditorWaveType : uint8_t
{
    None = 0,
    Sin,
    Square,
    Triangle,
    Sawtooth,
    InverseSawtooth,
    Noise,
};

enum class EditorTcModType : uint8_t
{
    Scale = 0,
    Scroll,
    Rotate,
    Stretch,
    Turb,
};

struct EditorTcMod
{
    EditorTcModType type = EditorTcModType::Scale;
    EditorWaveType wave = EditorWaveType::None;
    float params[4] = {};
};

enum class EditorColorGen : uint8_t
{
    Default = 0,
    Identity,
    Vertex,
    Wave,
    Entity,
};

enum class EditorAlphaGen : uint8_t
{
    Default = 0,
    Identity,
    Vertex,
    Wave,
    Entity,
    LightingSpecular,
};

struct EditorOverlayStage
{
    std::string map;
    std::vector<std::string> animationMaps;
    float animationFps = 0.0f;
    bool clampMap = false;
    bool isLightmap = false;
    bool isWhite = false;
    bool isEnvironment = false;
    bool alphaTested = false;
    bool blended = false;
    bool additive = false;
    bool filter = false;
    bool alphaBlend = false;
    bool invertedAlphaBlend = false;
    bool darkenDecal = false;
    QrBlendFactor blendSrc = QR_BLEND_FACTOR_ONE;
    QrBlendFactor blendDst = QR_BLEND_FACTOR_ZERO;
    bool hasScale = false;
    float scale[2] = { 1.0f, 1.0f };
    std::vector<EditorTcMod> tcMods;
    EditorColorGen rgbGen = EditorColorGen::Default;
    EditorWaveType rgbWave = EditorWaveType::None;
    float rgbWaveBase = 1.0f;
    float rgbWaveAmplitude = 0.0f;
    float rgbWavePhase = 0.0f;
    float rgbWaveFrequency = 0.0f;
    EditorAlphaGen alphaGen = EditorAlphaGen::Default;
    EditorWaveType alphaWave = EditorWaveType::None;
    float alphaWaveBase = 1.0f;
    float alphaWaveAmplitude = 0.0f;
    float alphaWavePhase = 0.0f;
    float alphaWaveFrequency = 0.0f;
    bool animated = false;
    bool dynamic = false;
};

struct EditorTexture
{
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    // Per-texel fullbright mask extracted from Quake palette indices 224..255 (0 or 255).
    std::vector<uint8_t> fullbright;
    bool isExternal = false;
    bool isEmissiveShader = false;
    bool isMirrorShader = false;
    bool isCutoutUnderlay = false;
    bool isMetallicEnv = false;
    bool isWetGloss = false;
    std::vector<uint8_t> roughnessMask;
    std::vector<uint8_t> metallicMask;
    float surfaceLightValue = 0.0f;
    bool hasSurfaceLightColor = false;
    float surfaceLightColor[3] = { 1.0f, 1.0f, 1.0f };
    // Universal shader-stage metadata populated by engine adapters (Q3/RTCW .shader, etc.).
    std::string defaultBaseTexture;
    std::string defaultEmissiveTexture;
    std::string defaultOverlayTexture;
    size_t baseStageIndex = (std::numeric_limits<size_t>::max)();
    bool isNonTracedOverlay = false;
    bool twoSided = false;
    bool autoSprite = false;
    bool autoSprite2 = false;
    EditorWaveType deformWave = EditorWaveType::None;
    float deformFrequency = 0.0f;
    float deformAmplitude = 0.0f;
    std::vector<EditorOverlayStage> shaderStages;
};

struct EditorCameraBasis
{
    float origin[3] = {};
    float forward[3] = {};
    float right[3] = {};
    float up[3] = {};
    float tanHalfFovX = 1.0f;
    float tanHalfFovY = 1.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
};

// One polygon of a surface, kept because a textured area light is built from a face's own
// corners: the vertex list above is a triangle fan that no longer says where one polygon ends and
// the next begins, and a light fitted over a fan that spans two faces would stand for a shape the
// level does not have.
struct EditorSurfaceFace
{
    float normal[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t cluster = QR_CLUSTER_LIGHT_NO_CLUSTER;
    uint32_t aliasInstanceId = 0;
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
    bool hasAuthoredNormals = false;
    bool isCutoutUnderlay = false;
    bool isOverlayOnly = false;
    bool noMarks = false;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    // The polygons the vertices above fan out of, in the order Scene.cpp met them.
    std::vector<EditorSurfaceFace> faces;
};

struct EditorOverlaySurface
{
    std::string shaderName;
    bool tracedBase = false;
    bool cutoutUnderlay = false;
    bool metallicEnvBase = false;
    bool twoSided = false;
    bool autoSprite = false;
    bool autoSprite2 = false;
    EditorWaveType deformWave = EditorWaveType::None;
    float deformSpread = 0.0f;
    float deformBase = 0.0f;
    float deformAmplitude = 0.0f;
    float deformPhase = 0.0f;
    float deformFrequency = 0.0f;
    float surfaceLight = 0.0f;
    size_t baseStageIndex = (std::numeric_limits<size_t>::max)();
    size_t firstOverlayStage = 0;
    size_t endOverlayStage = 0;
    std::vector<EditorOverlayStage> stages;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
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
    bool hasOffset = false;
    float offset[3] = {};
    bool lightShafts = false;
    float lightShaftsPower = 1.0f;
    float lightShaftsDistance = 0.0f;
    float lightShaftsFalloff = 1.0f;
    bool hasColor = false;
    bool forceRasterize = false;
    // The light style index the game animates the light with, the same index the map's `style`
    // key names and the same one rt_custom_style_names indexes: 0 is steady, 1..11 are the
    // animated styles. The preview stays at full brightness because a standalone editor has
    // none of the server's lightstyle strings, and an empty string leaves d_lightstylevalue[]
    // at 256 - which is exactly what a map light without a style renders as.
    int32_t style = 0;
    bool hasStyle = true;
    std::string className;
    uint64_t emitterUniqueID = 0;
    float modelSizeMeters = 0.0f;
    bool groupEdit = true;
    bool enabled = true;

    bool operator==(const EditorLight &) const = default;
};

// An alias (.mdl) entity instance placed in the scene, carrying its skin frame name,
// world origin, pose bounding radius (meters), and Quake entity unique ID so material
// dlights can be generated and edited just like vkquake-rt's RT_LIGHT_Emit.
struct EditorAliasInstance
{
    std::string modelName;
    std::string textureName;
    float origin[3] = {};
    float modelSizeMeters = 0.5f;
    uint64_t uniqueID = 0;
    uint32_t aliasInstanceId = 0;
    bool hasBounds = false;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
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

// The world cluster table of r_world.c: cluster 0 is the solid leaf/hole, and each BSP leaf
// maps through leafCluster[leaf] (identity when numLeafs < 8192, 3D grid cell when folded).
struct EditorWorldClusters
{
    std::vector<QrFloat3D> mins;
    std::vector<QrFloat3D> maxs;
    std::vector<uint8_t> flags;
    std::vector<int32_t> contents;
    std::vector<uint32_t> leafCluster;
    std::vector<uint8_t> visData;
    std::vector<int32_t> visOffsets;
    std::vector<uint8_t> skyVis;
    uint32_t pvsRowBytes = 0;
    uint32_t numLeafs = 0;
};

struct EditorScene
{
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<EditorTexture> textures;
    std::vector<EditorSurface> surfaces;
    std::vector<EditorOverlaySurface> overlaySurfaces;
    std::vector<EditorLight> lights;
    std::vector<EditorLight> bspLights;
    std::vector<EditorAliasInstance> aliasInstances;
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
    std::string skyboxName;
    std::array<std::vector<uint8_t>, 6> generatedSkyboxFaces;
    uint32_t generatedSkyboxSize = 0;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    float cameraPosition[3] = {};
    float cameraYawDegrees = 0.0f;
    bool hasBounds = false;
    bool hasCamera = false;
    bool usesExternalLightingModel = false;
    bool hasSun = false;
    float sunColor[3] = { 1.0f, 0.95f, 0.85f };
    float sunIntensity = 0.0f;
    float sunDegrees = 0.0f;
    float sunElevation = 45.0f;
};

struct EditorSceneLoadOptions
{
    const EditorAssetSource *assets = nullptr;
    bool multiplayer = false;
    bool includeInlineSubmodels = true;
};

// palette768 is the 768-byte Quake palette (gfx/palette.lmp); when it is null
// the palette stored inside each BSP texture is used instead.
bool LoadEditorSceneObj(const std::string &path, EditorScene &scene, std::string &error);
bool LoadEditorSceneBspMemory(const std::vector<uint8_t> &bytes, const std::string &sourceName,
                              const uint8_t *palette768, EditorScene &scene,
                              std::string &error, const EditorSceneLoadOptions &options = {});
bool LoadEditorSceneBsp(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                        std::string &error, const EditorSceneLoadOptions &options = {});
bool LoadEditorScene(const std::string &path, const uint8_t *palette768, EditorScene &scene,
                     std::string &error, const EditorSceneLoadOptions &options = {});

// Re-resolves the cluster a light stands in, the way the loader does when it reads the map, so
// moving a light in the panel keeps its world cluster light list correct.
uint32_t ResolveEditorLightCluster(const EditorScene &scene, const float origin[3]);

struct EditorPreviewModelSection
{
    std::string surfaceName;
    std::string materialName;
    std::string shaderName;
    bool alphaTested = false;
    bool twoSided = false;
    bool hasAuthoredNormals = true;
    bool isNonTracedOverlay = false;
    bool cutoutFrameUnderlay = false;
    bool usesOverlay = false;
    bool tracedBase = false;
    bool metallicEnvBase = false;
    bool autoSprite = false;
    bool autoSprite2 = false;
    EditorWaveType deformWave = EditorWaveType::None;
    float deformSpread = 0.0f;
    float deformBase = 0.0f;
    float deformAmplitude = 0.0f;
    float deformPhase = 0.0f;
    float deformFrequency = 0.0f;
    float surfaceLight = 0.0f;
    size_t baseStageIndex = (std::numeric_limits<size_t>::max)();
    std::vector<EditorOverlayStage> stages;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
};

struct EditorPreviewModelSkinPreset
{
    std::string name;
    std::unordered_map<std::string, std::string> surfaceMaterials;
};

struct EditorPreviewModelFrame
{
    std::string name;
    std::vector<QrVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<EditorPreviewModelSection> sections;
    float center[3] = {};
    float radius = 16.0f;
};

struct EditorPreviewModel
{
    std::string path;
    bool holey = false;
    bool hasMultiSections = false;
    std::vector<EditorTexture> skins;
    std::vector<EditorPreviewModelSkinPreset> skinPresets;
    std::vector<EditorTexture> surfaceTextures;
    std::unordered_set<std::string> hiddenMaterials;
    std::vector<EditorPreviewModelFrame> frames;
};

struct EditorPlayLevelRequest
{
    std::string executableDir;
    std::string gameExecutable;
    std::string gameDir;
    std::string scenePath;
    std::string levelName;
    std::string shaderFolder;
    std::string blueNoisePath;
    std::string waterNormalPath;
    std::string materialsPath;
    std::string lightsPath;
    std::string configPath;
    bool multiplayer = false;
};

struct EditorLaunchCommand
{
    std::string executable;
    std::string workingDirectory;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> envOverrides;
};

struct EditorFormatAdapter
{
    std::vector<std::string> extraArchiveExtensions;
    std::vector<std::string> extraModelExtensions;
    std::vector<std::string> extraBaseGameDirs;
    std::vector<std::string> extraModConfigFiles;
    std::vector<std::string> extraModSubdirs;
    std::string modDirectoryDescription;

    bool (*canLoadBspMemory)(const std::vector<uint8_t> &bytes) = nullptr;
    bool (*loadBspSceneMemory)(const std::vector<uint8_t> &bytes, const std::string &sourceName,
                               EditorScene &scene, std::string &error,
                               const EditorSceneLoadOptions &options) = nullptr;

    bool (*canLoadPreviewModel)(const std::string &modelPath,
                                const EditorAssetSource &assets) = nullptr;
    bool (*loadPreviewModel)(const std::string &modelPath, const EditorAssetSource &assets,
                             EditorPreviewModel &outModel, std::string &error) = nullptr;

    bool (*shouldHideModelCatalogEntry)(const std::string &lowerPath) = nullptr;
    void (*postProcessModelCatalog)(std::vector<EditorAssetCatalogEntry> &entries) = nullptr;

    bool (*resolveModelMaterialPrefix)(const std::string &lowerModelPath,
                                       std::string &outMaterialPrefix) = nullptr;
    int32_t (*findDefaultPreviewModelIndex)(
        const std::vector<EditorAssetCatalogEntry> &catalog) = nullptr;

    bool (*loadExternalImageRgba)(const EditorAssetSource *assets, const std::string &requestedPath,
                                  std::vector<uint8_t> &outRgba, uint32_t &outWidth,
                                  uint32_t &outHeight, std::string &outResolvedPath) = nullptr;

    bool (*buildPlayLevelCommand)(const EditorPlayLevelRequest &request,
                                  EditorLaunchCommand &outCommand,
                                  std::string &outError) = nullptr;
};

void RegisterEditorFormatAdapter(const EditorFormatAdapter &adapter);
const EditorFormatAdapter *GetEditorFormatAdapter();

float EvaluateEditorWaveSigned(EditorWaveType type, float base, float amplitude, float phase,
                               float frequency, float timeSeconds);
float EvaluateEditorWave(EditorWaveType type, float base, float amplitude, float phase,
                         float frequency, float timeSeconds);
void OffsetEditorVerticesAlongSmoothNormals(std::vector<QrVertex> &vertices,
                                            const std::vector<uint32_t> &indices, float distance);
std::string NormalizeEditorAssetPath(std::string_view raw);
std::string StripEditorImageExtension(std::string_view path);

// Loads an IDPO v6 .mdl alias model from mounted archives into centered local-space preview frames.
bool LoadEditorAliasModel(const std::string &modelPath, const EditorAssetSource &assets,
                          const uint8_t *palette768, EditorPreviewModel &outModel,
                          std::string &error);

// Loads either an .mdl alias model or an adapter-supported preview model into preview frames.
bool LoadEditorPreviewModel(const std::string &modelPath, const EditorAssetSource &assets,
                            const uint8_t *palette768, EditorPreviewModel &outModel,
                            std::string &error);
