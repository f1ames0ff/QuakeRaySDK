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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "EditorSession.h"
#include "Scene.h"

// Camera frustum + visible viewport region for screen-space marquee light selection.
// Matches EditorApp::ProjectPoint projection math while excluding areas covered by UI chrome.
struct LightProjectionCamera
{
    float origin[3] = {};
    float forward[3] = { 1.0f, 0.0f, 0.0f };
    float right[3] = { 0.0f, -1.0f, 0.0f };
    float up[3] = { 0.0f, 0.0f, 1.0f };
    float tanHalfFovX = 1.0f;
    float tanHalfFovY = 1.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float visibleMinX = 0.0f;
    float visibleMinY = 0.0f;
    // Negative means "full viewport dimension".
    float visibleMaxX = -1.0f;
    float visibleMaxY = -1.0f;
};

struct MarqueeLightSelectionResult
{
    std::vector<int32_t> mapIndices;
    std::vector<int32_t> customIndices;
};

// Evaluates effective world origin (position + offset when enabled).
void ComputeEffectiveLightOrigin(const EditorLight &light, float outPos[3]);

// Projects a world-space point into screen pixels; returns false when behind camera.
bool ProjectLightWorldPoint(const LightProjectionCamera &cam, const float point[3], float &outX,
                            float &outY);

// Returns true when screen coordinates lie inside the unobstructed 3D viewport area.
bool IsProjectedPointVisible(const LightProjectionCamera &cam, float screenX, float screenY);

struct MarqueeRect
{
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

struct MarqueeSelectionQuery
{
    float startX = 0.0f;
    float startY = 0.0f;
    float endX = 0.0f;
    float endY = 0.0f;
    bool additive = false;
    const std::vector<int32_t> *existingMap = nullptr;
    const std::vector<int32_t> *existingCustom = nullptr;
};

// Normalizes drag endpoints to [minX, maxX] x [minY, maxY] and checks minimum travel threshold.
bool IsMarqueeDragPastThreshold(float startX, float startY, float endX, float endY,
                                float thresholdPx = 4.0f);
MarqueeRect NormalizeMarqueeRect(float startX, float startY, float endX, float endY);

// Selects all visible lights whose projected centers fall inside the normalized marquee rectangle.
// When `query.additive` is true, merges newly enclosed lights into existing selections (sorted, unique).
MarqueeLightSelectionResult SelectVisibleLightsInMarquee(
    const LightProjectionCamera &cam, const std::vector<EditorLight> &mapLights,
    const std::vector<EditorLight> &customLights, const MarqueeSelectionQuery &query);

// Summary + per-field mixed flags across a non-empty list of selected EditorLights.
// Mixed parameters should display a gray "?" in the inspector; setting any parameter overwrites
// that parameter across every selected light without clobbering unedited differing fields.
struct MultiLightAggregate
{
    size_t count = 0;
    size_t mapEntityCount = 0;
    size_t emitterCount = 0;
    size_t customCount = 0;
    bool allCustom = false;
    bool allEmitters = false;
    bool anyHasOffset = false;
    bool anySpot = false;
    bool anyLightShafts = false;

    bool enabled = false;
    bool mixedEnabled = false;

    float position[3] = {};
    uint8_t mixedPositionAxes = 0;

    bool hasOffset = false;
    bool mixedHasOffset = false;

    float offset[3] = {};
    uint8_t mixedOffsetAxes = 0;

    float radiusUnits = 16.0f;
    bool mixedRadius = false;

    float intensity = 300.0f;
    bool mixedIntensity = false;

    float tint[3] = { 1.0f, 1.0f, 1.0f };
    bool mixedColor = false;

    int32_t style = 0;
    int32_t styleIndex = 1; // 0 = NONE (unassigned emitter), 1..kEditorStyleCount = style + 1
    bool mixedStyle = false;

    bool spot = false;
    bool mixedSpot = false;

    float direction[3] = { 0.0f, 0.0f, 1.0f };
    uint8_t mixedDirectionAxes = 0;

    float angleInner = 0.0f;
    bool mixedAngleInner = false;

    float angleOuter = 30.0f;
    bool mixedAngleOuter = false;

    bool lightShafts = false;
    bool mixedLightShafts = false;

    float lightShaftsPower = 1.0f;
    bool mixedLightShaftsPower = false;

    float lightShaftsDistance = 0.0f;
    bool mixedLightShaftsDistance = false;

    float lightShaftsFalloff = 1.0f;
    bool mixedLightShaftsFalloff = false;

    bool forceRasterize = false;
    bool mixedForceRasterize = false;
};

MultiLightAggregate AggregateMultiLightState(const std::vector<const EditorLight *> &lights,
                                             const std::vector<bool> *isCustomFlags = nullptr);

void ResolveLightClusterFromEffectivePos(const EditorScene &scene, EditorLight &light);

void ApplyBatchLightEnabled(const std::vector<EditorLight *> &lights, bool enabled);
void ApplyBatchLightOrigin(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                           uint8_t axisMask, const float position[3]);
void ApplyBatchLightHasOffset(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                              bool hasOffset);
void ApplyBatchLightOffset(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                           uint8_t axisMask, const float offset[3]);
void ApplyBatchLightRadiusUnits(const std::vector<EditorLight *> &lights, float radiusUnits);
void ApplyBatchLightIntensity(const std::vector<EditorLight *> &lights, float intensity);
void ApplyBatchLightTint(const std::vector<EditorLight *> &lights, const float tint[3]);
void ApplyBatchLightStyle(const std::vector<EditorLight *> &lights, int32_t style);
void ApplyBatchLightSpot(const std::vector<EditorLight *> &lights, bool spot);
void ApplyBatchLightDirection(const std::vector<EditorLight *> &lights, uint8_t axisMask,
                              const float direction[3]);
void ApplyBatchLightAngleInner(const std::vector<EditorLight *> &lights, float angleInner);
void ApplyBatchLightAngleOuter(const std::vector<EditorLight *> &lights, float angleOuter);
void ApplyBatchLightShafts(const std::vector<EditorLight *> &lights, bool enabled);
void ApplyBatchLightShaftsPower(const std::vector<EditorLight *> &lights, float power);
void ApplyBatchLightShaftsDistance(const std::vector<EditorLight *> &lights, float distance);
void ApplyBatchLightShaftsFalloff(const std::vector<EditorLight *> &lights, float falloff);
void ApplyBatchLightForceRasterize(const std::vector<EditorLight *> &lights, bool forceRasterize);

// Applies axis-masked direction edits or camera aim to an emitter override, preserving `groupEdit`
// emitters whose direction comes from geometry rather than override `dir`.
void ApplyEmitterDirectionAxes(EditorEmitterLightOverride &ov, uint8_t axisMask,
                               const float dir[3]);
void ApplyEmitterAimDirection(EditorEmitterLightOverride &ov, const float dir[3]);
// Assigns `styleIndex` (0 = NONE, 1..kEditorStyleCount = kEditorStyleNames[styleIndex - 1]).
void ApplyEmitterStyleIndex(EditorEmitterLightOverride &ov, int32_t styleIndex);

// Summary + mixed flags across effective emitter overrides for all-emitter multi-selections.
struct MultiEmitterAggregate
{
    size_t count = 0;
    bool anySpot = false;
    bool anyGroupEdit = false;
    bool anyInstanceEdit = false;

    bool enabled = false;
    bool mixedEnabled = false;

    float radius = 0.5f;
    bool mixedRadius = false;

    float intensity = 1.0f;
    bool mixedIntensity = false;

    float offset[3] = {};
    uint8_t mixedOffsetAxes = 0;

    float color[3] = { 1.0f, 1.0f, 1.0f };
    bool mixedColor = false;

    int32_t styleIndex = 0; // 0 = NONE, 1..kEditorStyleCount = style + 1
    bool mixedStyle = false;

    bool spot = false;
    bool mixedSpot = false;

    float dir[3] = { 0.0f, 0.0f, 1.0f };
    uint8_t mixedDirAxes = 0;

    float angleInner = 0.0f;
    bool mixedAngleInner = false;

    float angleOuter = 30.0f;
    bool mixedAngleOuter = false;

    bool forceRasterize = false;
    bool mixedForceRasterize = false;
};

MultiEmitterAggregate AggregateMultiEmitterOverrides(
    const std::vector<EditorEmitterLightOverride> &overrides);

std::string NormalizeEmitterNameBase(const std::string &name);
void BuildEmitterOverrideCandidates(const std::string &name, std::vector<std::string> &outNames);
const EditorEmitterLightOverride *FindEmitterOverrideExact(
    const std::vector<EditorEmitterLightOverride> &list, const std::string &key);
bool ResetSingleEmitterOverride(const std::string &className, uint64_t emitterUniqueID,
                                std::vector<EditorEmitterLightOverride> &current,
                                const std::vector<EditorEmitterLightOverride> &baseline);
size_t CountLeadingBspLights(const std::vector<EditorLight> &lights);
const EditorLight *BaselineMapLightAt(int32_t lightIndex,
                                      const std::vector<EditorLight> &sceneLights,
                                      const std::vector<EditorLight> &baselineMapLights);
const EditorLight *BaselineCustomLightAt(int32_t customIndex,
                                         const std::vector<EditorLight> &customLights,
                                         const std::vector<EditorLight> &baselineCustomLights);

struct EditorLightClipboard
{
    std::vector<EditorLight> lights;
    float copyCameraPos[3] = {};
    float copyCameraForward[3] = {};
    uint32_t pasteCount = 0;
};

// Snapshots selected scene and/or custom lights into sanitized templates (offset folded into
// position, emitter/classname metadata cleared) ordered ascending by index.
std::vector<EditorLight> BuildCopiedLights(const std::vector<EditorLight> &sceneLights,
                                           const std::vector<int32_t> &selectedScene,
                                           int32_t singleScene,
                                           const std::vector<EditorLight> &customLights,
                                           const std::vector<int32_t> &selectedCustom,
                                           int32_t singleCustom);

// Produces pasted custom lights from `clipboard` at the current camera: steps upward in place when
// the camera has barely moved/turned, or places the group's centroid ahead of the moved camera.
std::vector<EditorLight> BuildPastedLights(EditorLightClipboard &clipboard,
                                           const float cameraPos[3],
                                           const float cameraForward[3],
                                           float stepUnits = 16.0f,
                                           float spawnDistanceUnits = 96.0f);

