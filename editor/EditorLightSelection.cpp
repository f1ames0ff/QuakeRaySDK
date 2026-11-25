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

#include "EditorLightSelection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace
{

bool FloatNear(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

bool ColorNear(const float a[3], const float b[3])
{
    return FloatNear(a[0], b[0], 1e-3f) && FloatNear(a[1], b[1], 1e-3f) &&
           FloatNear(a[2], b[2], 1e-3f);
}

void NormalizeDirVec3(float vec[3])
{
    const float len = std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
    if (len > 1e-6f)
    {
        vec[0] /= len;
        vec[1] /= len;
        vec[2] /= len;
    }
    else
    {
        vec[0] = 0.0f;
        vec[1] = 0.0f;
        vec[2] = 1.0f;
    }
}

void MergeSortedUnique(std::vector<int32_t> &target, const std::vector<int32_t> *existing)
{
    if (existing != nullptr)
    {
        target.insert(target.end(), existing->begin(), existing->end());
    }
    std::sort(target.begin(), target.end());
    target.erase(std::unique(target.begin(), target.end()), target.end());
}

bool IsGroupEditEmitter(const EditorLight &l, bool isCustom)
{
    return !isCustom && l.emitterUniqueID != 0 && l.groupEdit;
}

int32_t EffectiveLightStyleIndex(const EditorLight &l, bool isCustom)
{
    if (!isCustom && l.emitterUniqueID != 0 && !l.hasStyle)
    {
        return 0;
    }
    return std::clamp(l.style, 0, kEditorStyleCount - 1) + 1;
}

void AccumulateLightCounts(const EditorLight &l, bool isCustom, MultiLightAggregate &agg)
{
    agg.count++;
    if (isCustom)
    {
        agg.customCount++;
    }
    else if (l.emitterUniqueID != 0)
    {
        agg.emitterCount++;
    }
    else
    {
        agg.mapEntityCount++;
    }

    agg.anyHasOffset = agg.anyHasOffset || l.hasOffset;
    agg.anySpot = agg.anySpot || l.spot;
    agg.anyLightShafts = agg.anyLightShafts || l.lightShafts;
}

void InitLightAggregateFromFirst(const EditorLight &l, bool isCustom, MultiLightAggregate &agg)
{
    agg.enabled = l.enabled;
    std::copy_n(l.position, 3, agg.position);
    agg.hasOffset = l.hasOffset;
    std::copy_n(l.offset, 3, agg.offset);
    agg.radiusUnits = l.radius;
    agg.intensity = l.intensity;
    std::copy_n(l.tint, 3, agg.tint);
    agg.styleIndex = EffectiveLightStyleIndex(l, isCustom);
    agg.style = agg.styleIndex > 0 ? (agg.styleIndex - 1) : -1;
    agg.spot = l.spot;
    std::copy_n(l.direction, 3, agg.direction);
    agg.angleInner = l.angleInner;
    agg.angleOuter = l.angleOuter;
    agg.lightShafts = l.lightShafts;
    agg.lightShaftsPower = l.lightShaftsPower;
    agg.lightShaftsDistance = l.lightShaftsDistance;
    agg.lightShaftsFalloff = l.lightShaftsFalloff;
    agg.forceRasterize = l.forceRasterize;
}

void AccumulateLightDirection(const EditorLight &l, bool isCustom,
                              const EditorLight *&firstDirLight, MultiLightAggregate &agg)
{
    if (IsGroupEditEmitter(l, isCustom))
    {
        return;
    }
    if (firstDirLight == nullptr)
    {
        firstDirLight = &l;
        std::copy_n(l.direction, 3, agg.direction);
        return;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!FloatNear(l.direction[axis], firstDirLight->direction[axis]))
        {
            agg.mixedDirectionAxes |= uint8_t(1u << axis);
        }
    }
}

void CompareLightRadiometryDiffs(const EditorLight &l, const EditorLight &first, bool isCustom,
                                 MultiLightAggregate &agg)
{
    agg.mixedEnabled = agg.mixedEnabled || (l.enabled != first.enabled);
    agg.mixedHasOffset = agg.mixedHasOffset || (l.hasOffset != first.hasOffset);
    agg.mixedRadius = agg.mixedRadius || !FloatNear(l.radius, first.radius);
    agg.mixedIntensity = agg.mixedIntensity || !FloatNear(l.intensity, first.intensity);
    agg.mixedColor = agg.mixedColor || !ColorNear(l.tint, first.tint);
    if (EffectiveLightStyleIndex(l, isCustom) != agg.styleIndex)
    {
        agg.mixedStyle = true;
    }
    agg.mixedForceRasterize = agg.mixedForceRasterize || (l.forceRasterize != first.forceRasterize);
}

void CompareLightSpatialAndConeDiffs(const EditorLight &l, const EditorLight &first,
                                      MultiLightAggregate &agg)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!FloatNear(l.position[axis], first.position[axis]))
        {
            agg.mixedPositionAxes |= uint8_t(1u << axis);
        }
        if (!FloatNear(l.offset[axis], first.offset[axis]))
        {
            agg.mixedOffsetAxes |= uint8_t(1u << axis);
        }
    }
    agg.mixedSpot = agg.mixedSpot || (l.spot != first.spot);
    agg.mixedAngleInner = agg.mixedAngleInner || !FloatNear(l.angleInner, first.angleInner);
    agg.mixedAngleOuter = agg.mixedAngleOuter || !FloatNear(l.angleOuter, first.angleOuter);
    agg.mixedLightShafts = agg.mixedLightShafts || (l.lightShafts != first.lightShafts);
    agg.mixedLightShaftsPower =
        agg.mixedLightShaftsPower || !FloatNear(l.lightShaftsPower, first.lightShaftsPower);
    agg.mixedLightShaftsDistance =
        agg.mixedLightShaftsDistance || !FloatNear(l.lightShaftsDistance, first.lightShaftsDistance);
    agg.mixedLightShaftsFalloff =
        agg.mixedLightShaftsFalloff || !FloatNear(l.lightShaftsFalloff, first.lightShaftsFalloff);
}

int32_t EmitterStyleIndex(const EditorEmitterLightOverride &ov)
{
    return ov.hasStyle ? (std::clamp(ov.style, 0, kEditorStyleCount - 1) + 1) : 0;
}

void InitEmitterAggregateFromFirst(const EditorEmitterLightOverride &first,
                                   MultiEmitterAggregate &agg)
{
    agg.enabled = first.enabled;
    agg.radius = first.radius;
    agg.intensity = first.intensity;
    std::copy_n(first.offset, 3, agg.offset);
    std::copy_n(first.color, 3, agg.color);
    agg.styleIndex = EmitterStyleIndex(first);
    agg.spot = first.spot;
    std::copy_n(first.dir, 3, agg.dir);
    agg.angleInner = first.angleInner;
    agg.angleOuter = first.angleOuter;
    agg.forceRasterize = first.forceRasterize;
}

void AccumulateEmitterDirection(const EditorEmitterLightOverride &ov,
                                const EditorEmitterLightOverride *&firstInstance,
                                MultiEmitterAggregate &agg)
{
    if (ov.groupEdit)
    {
        return;
    }
    if (firstInstance == nullptr)
    {
        firstInstance = &ov;
        std::copy_n(ov.dir, 3, agg.dir);
        return;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!FloatNear(ov.dir[axis], firstInstance->dir[axis]))
        {
            agg.mixedDirAxes |= uint8_t(1u << axis);
        }
    }
}

void AccumulateEmitterFieldDiffs(const EditorEmitterLightOverride &ov,
                                 const EditorEmitterLightOverride &first,
                                 MultiEmitterAggregate &agg)
{
    agg.mixedEnabled = agg.mixedEnabled || (ov.enabled != first.enabled);
    agg.mixedRadius = agg.mixedRadius || !FloatNear(ov.radius, first.radius);
    agg.mixedIntensity = agg.mixedIntensity || !FloatNear(ov.intensity, first.intensity);
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!FloatNear(ov.offset[axis], first.offset[axis]))
        {
            agg.mixedOffsetAxes |= uint8_t(1u << axis);
        }
    }
    agg.mixedColor = agg.mixedColor || !ColorNear(ov.color, first.color);
    agg.mixedStyle = agg.mixedStyle || (EmitterStyleIndex(ov) != agg.styleIndex);
    agg.mixedSpot = agg.mixedSpot || (ov.spot != first.spot);
    agg.mixedAngleInner = agg.mixedAngleInner || !FloatNear(ov.angleInner, first.angleInner);
    agg.mixedAngleOuter = agg.mixedAngleOuter || !FloatNear(ov.angleOuter, first.angleOuter);
    agg.mixedForceRasterize =
        agg.mixedForceRasterize || (ov.forceRasterize != first.forceRasterize);
}

void ScanVisibleLightsInto(const LightProjectionCamera &cam, const std::vector<EditorLight> &list,
                           const MarqueeRect &rect, std::vector<int32_t> &out)
{
    for (size_t i = 0; i < list.size(); ++i)
    {
        float pos[3] = {};
        ComputeEffectiveLightOrigin(list[i], pos);
        float sx = 0.0f;
        float sy = 0.0f;
        if (!ProjectLightWorldPoint(cam, pos, sx, sy) || !IsProjectedPointVisible(cam, sx, sy))
        {
            continue;
        }
        if (sx >= rect.minX && sx <= rect.maxX && sy >= rect.minY && sy <= rect.maxY)
        {
            out.push_back(int32_t(i));
        }
    }
}

} // namespace

void ComputeEffectiveLightOrigin(const EditorLight &light, float outPos[3])
{
    const float ox = light.hasOffset ? light.offset[0] : 0.0f;
    const float oy = light.hasOffset ? light.offset[1] : 0.0f;
    const float oz = light.hasOffset ? light.offset[2] : 0.0f;
    outPos[0] = light.position[0] + ox;
    outPos[1] = light.position[1] + oy;
    outPos[2] = light.position[2] + oz;
}

bool ProjectLightWorldPoint(const LightProjectionCamera &cam, const float point[3], float &outX,
                            float &outY)
{
    if (cam.viewportWidth <= 0.0f || cam.viewportHeight <= 0.0f || cam.tanHalfFovX <= 0.0f ||
        cam.tanHalfFovY <= 0.0f)
    {
        return false;
    }

    const float rel[3] = {
        point[0] - cam.origin[0],
        point[1] - cam.origin[1],
        point[2] - cam.origin[2],
    };

    const float depth =
        rel[0] * cam.forward[0] + rel[1] * cam.forward[1] + rel[2] * cam.forward[2];
    if (depth <= 1e-3f)
    {
        return false;
    }

    const float horiz = rel[0] * cam.right[0] + rel[1] * cam.right[1] + rel[2] * cam.right[2];
    const float vert = rel[0] * cam.up[0] + rel[1] * cam.up[1] + rel[2] * cam.up[2];
    const float ndcX = horiz / (depth * cam.tanHalfFovX);
    const float ndcY = vert / (depth * cam.tanHalfFovY);

    outX = (ndcX * 0.5f + 0.5f) * cam.viewportWidth;
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * cam.viewportHeight;
    return true;
}

bool IsProjectedPointVisible(const LightProjectionCamera &cam, float screenX, float screenY)
{
    const float maxX = cam.visibleMaxX >= 0.0f ? std::min(cam.visibleMaxX, cam.viewportWidth)
                                               : cam.viewportWidth;
    const float maxY = cam.visibleMaxY >= 0.0f ? std::min(cam.visibleMaxY, cam.viewportHeight)
                                               : cam.viewportHeight;
    return screenX >= cam.visibleMinX && screenX < maxX && screenY >= cam.visibleMinY &&
           screenY < maxY;
}

bool IsMarqueeDragPastThreshold(float startX, float startY, float endX, float endY,
                                float thresholdPx)
{
    return std::hypot(endX - startX, endY - startY) >= thresholdPx;
}

MarqueeRect NormalizeMarqueeRect(float startX, float startY, float endX, float endY)
{
    MarqueeRect rect;
    rect.minX = std::min(startX, endX);
    rect.maxX = std::max(startX, endX);
    rect.minY = std::min(startY, endY);
    rect.maxY = std::max(startY, endY);
    return rect;
}

MarqueeLightSelectionResult SelectVisibleLightsInMarquee(
    const LightProjectionCamera &cam, const std::vector<EditorLight> &mapLights,
    const std::vector<EditorLight> &customLights, const MarqueeSelectionQuery &query)
{
    const MarqueeRect rect =
        NormalizeMarqueeRect(query.startX, query.startY, query.endX, query.endY);

    MarqueeLightSelectionResult result;
    ScanVisibleLightsInto(cam, mapLights, rect, result.mapIndices);
    ScanVisibleLightsInto(cam, customLights, rect, result.customIndices);

    if (query.additive)
    {
        MergeSortedUnique(result.mapIndices, query.existingMap);
        MergeSortedUnique(result.customIndices, query.existingCustom);
    }

    return result;
}

MultiLightAggregate AggregateMultiLightState(const std::vector<const EditorLight *> &lights,
                                             const std::vector<bool> *isCustomFlags)
{
    MultiLightAggregate agg;
    if (lights.empty())
    {
        return agg;
    }

    const EditorLight *first = nullptr;
    const EditorLight *firstDirLight = nullptr;
    for (size_t i = 0; i < lights.size(); ++i)
    {
        const EditorLight *l = lights[i];
        if (l == nullptr)
        {
            continue;
        }
        const bool isCustom =
            (isCustomFlags != nullptr && i < isCustomFlags->size()) ? (*isCustomFlags)[i] : false;
        AccumulateLightCounts(*l, isCustom, agg);
        AccumulateLightDirection(*l, isCustom, firstDirLight, agg);

        if (first == nullptr)
        {
            first = l;
            InitLightAggregateFromFirst(*l, isCustom, agg);
            continue;
        }

        CompareLightRadiometryDiffs(*l, *first, isCustom, agg);
        CompareLightSpatialAndConeDiffs(*l, *first, agg);
    }

    if (firstDirLight != nullptr)
    {
        std::copy_n(firstDirLight->direction, 3, agg.direction);
    }
    agg.allCustom = agg.count > 0 && agg.customCount == agg.count;
    agg.allEmitters = agg.count > 0 && agg.emitterCount == agg.count;
    return agg;
}

void ResolveLightClusterFromEffectivePos(const EditorScene &scene, EditorLight &light)
{
    float effective[3] = {};
    ComputeEffectiveLightOrigin(light, effective);
    light.cluster = ResolveEditorLightCluster(scene, effective);
    if (light.cluster == 0 || light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
    {
        light.cluster = ResolveEditorLightCluster(scene, light.position);
    }
}

void ApplyBatchLightEnabled(const std::vector<EditorLight *> &lights, bool enabled)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->enabled = enabled;
        }
    }
}

void ApplyBatchLightOrigin(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                           uint8_t axisMask, const float position[3])
{
    for (EditorLight *l : lights)
    {
        if (l == nullptr)
        {
            continue;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if ((axisMask & (1u << axis)) != 0)
            {
                l->position[axis] = position[axis];
            }
        }
        ResolveLightClusterFromEffectivePos(scene, *l);
    }
}

void ApplyBatchLightHasOffset(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                              bool hasOffset)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->hasOffset = hasOffset;
            ResolveLightClusterFromEffectivePos(scene, *l);
        }
    }
}

void ApplyBatchLightOffset(const EditorScene &scene, const std::vector<EditorLight *> &lights,
                           uint8_t axisMask, const float offset[3])
{
    for (EditorLight *l : lights)
    {
        if (l == nullptr)
        {
            continue;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if ((axisMask & (1u << axis)) != 0)
            {
                l->offset[axis] = offset[axis];
            }
        }
        ResolveLightClusterFromEffectivePos(scene, *l);
    }
}

void ApplyBatchLightRadiusUnits(const std::vector<EditorLight *> &lights, float radiusUnits)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->radius = std::max(radiusUnits, 0.5f);
        }
    }
}

void ApplyBatchLightIntensity(const std::vector<EditorLight *> &lights, float intensity)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            SetEditorLightIntensity(*l, intensity);
        }
    }
}

void ApplyBatchLightTint(const std::vector<EditorLight *> &lights, const float tint[3])
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            SetEditorLightTint(*l, tint);
            l->hasColor = true;
        }
    }
}

void ApplyBatchLightStyle(const std::vector<EditorLight *> &lights, int32_t style)
{
    const int32_t clamped = std::clamp(style, 0, kEditorStyleCount - 1);
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->style = clamped;
            l->hasStyle = true;
        }
    }
}

void ApplyBatchLightSpot(const std::vector<EditorLight *> &lights, bool spot)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->spot = spot;
            if (spot)
            {
                ClampEditorLightCone(*l);
            }
        }
    }
}

void ApplyBatchLightDirection(const std::vector<EditorLight *> &lights, uint8_t axisMask,
                              const float direction[3])
{
    for (EditorLight *l : lights)
    {
        if (l == nullptr)
        {
            continue;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if ((axisMask & (1u << axis)) != 0)
            {
                l->direction[axis] = direction[axis];
            }
        }
        NormalizeDirVec3(l->direction);
    }
}

void ApplyBatchLightAngleInner(const std::vector<EditorLight *> &lights, float angleInner)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->angleInner = angleInner;
            if (l->angleOuter < l->angleInner)
            {
                l->angleOuter = l->angleInner;
            }
            ClampEditorLightCone(*l);
        }
    }
}

void ApplyBatchLightAngleOuter(const std::vector<EditorLight *> &lights, float angleOuter)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->angleOuter = angleOuter;
            if (l->angleInner > l->angleOuter)
            {
                l->angleInner = l->angleOuter;
            }
            ClampEditorLightCone(*l);
        }
    }
}

void ApplyBatchLightShafts(const std::vector<EditorLight *> &lights, bool enabled)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->lightShafts = enabled;
        }
    }
}

void ApplyBatchLightShaftsPower(const std::vector<EditorLight *> &lights, float power)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->lightShaftsPower = std::max(0.0f, power);
        }
    }
}

void ApplyBatchLightShaftsDistance(const std::vector<EditorLight *> &lights, float distance)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->lightShaftsDistance = std::max(0.0f, distance);
        }
    }
}

void ApplyBatchLightShaftsFalloff(const std::vector<EditorLight *> &lights, float falloff)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->lightShaftsFalloff = std::max(0.0f, falloff);
        }
    }
}

void ApplyBatchLightForceRasterize(const std::vector<EditorLight *> &lights, bool forceRasterize)
{
    for (EditorLight *l : lights)
    {
        if (l != nullptr)
        {
            l->forceRasterize = forceRasterize;
        }
    }
}

void ApplyEmitterDirectionAxes(EditorEmitterLightOverride &ov, uint8_t axisMask, const float dir[3])
{
    if (ov.groupEdit || axisMask == 0)
    {
        return;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if ((axisMask & uint8_t(1u << axis)) != 0)
        {
            ov.dir[axis] = dir[axis];
        }
    }
    NormalizeDirVec3(ov.dir);
    ov.hasDir = true;
}

void ApplyEmitterAimDirection(EditorEmitterLightOverride &ov, const float dir[3])
{
    if (ov.groupEdit)
    {
        return;
    }
    std::copy_n(dir, 3, ov.dir);
    NormalizeDirVec3(ov.dir);
    ov.hasDir = true;
}

void ApplyEmitterStyleIndex(EditorEmitterLightOverride &ov, int32_t styleIndex)
{
    if (styleIndex <= 0)
    {
        ov.hasStyle = false;
        ov.style = 0;
        return;
    }
    ov.hasStyle = true;
    ov.style = std::clamp(styleIndex - 1, 0, kEditorStyleCount - 1);
}

MultiEmitterAggregate AggregateMultiEmitterOverrides(
    const std::vector<EditorEmitterLightOverride> &overrides)
{
    MultiEmitterAggregate agg;
    if (overrides.empty())
    {
        return agg;
    }

    agg.count = overrides.size();
    const EditorEmitterLightOverride &first = overrides[0];
    InitEmitterAggregateFromFirst(first, agg);

    const EditorEmitterLightOverride *firstInstance = nullptr;
    for (const EditorEmitterLightOverride &ov : overrides)
    {
        agg.anySpot = agg.anySpot || ov.spot;
        agg.anyGroupEdit = agg.anyGroupEdit || ov.groupEdit;
        agg.anyInstanceEdit = agg.anyInstanceEdit || !ov.groupEdit;
        AccumulateEmitterDirection(ov, firstInstance, agg);
        AccumulateEmitterFieldDiffs(ov, first, agg);
    }

    return agg;
}

namespace
{
std::string LowerAscii(const std::string &text)
{
    std::string out = text;
    for (char &c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool MatchesAnyOverrideKey(const std::string &name, const std::vector<std::string> &keys)
{
    const std::string lower = LowerAscii(name);
    return std::find(keys.begin(), keys.end(), lower) != keys.end();
}

void EraseOverridesMatchingKeys(std::vector<EditorEmitterLightOverride> &list,
                                const std::vector<std::string> &keys)
{
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&keys](const EditorEmitterLightOverride &ov) {
                                  return MatchesAnyOverrideKey(ov.name, keys);
                              }),
               list.end());
}

const EditorEmitterLightOverride *FindFirstOverrideMatchingKeys(
    const std::vector<EditorEmitterLightOverride> &list, const std::vector<std::string> &keys)
{
    for (const std::string &key : keys)
    {
        if (const EditorEmitterLightOverride *found = FindEmitterOverrideExact(list, key))
        {
            return found;
        }
    }
    return nullptr;
}

EditorEmitterLightOverride *FindMutableOverrideMatchingKeys(
    std::vector<EditorEmitterLightOverride> &list, const std::vector<std::string> &keys)
{
    for (const std::string &key : keys)
    {
        for (EditorEmitterLightOverride &ov : list)
        {
            if (LowerAscii(ov.name) == key)
            {
                return &ov;
            }
        }
    }
    return nullptr;
}

bool RestoreOverrideByKeys(std::vector<EditorEmitterLightOverride> &current,
                           const std::vector<EditorEmitterLightOverride> &baseline,
                           const std::vector<std::string> &keys)
{
    const EditorEmitterLightOverride *basePtr = FindFirstOverrideMatchingKeys(baseline, keys);
    EditorEmitterLightOverride *currPtr = FindMutableOverrideMatchingKeys(current, keys);
    if (basePtr == nullptr && currPtr == nullptr)
    {
        return false;
    }
    if (basePtr == nullptr)
    {
        EraseOverridesMatchingKeys(current, keys);
        return true;
    }
    const EditorEmitterLightOverride baseCopy = *basePtr;
    if (currPtr != nullptr)
    {
        if (*currPtr == baseCopy)
        {
            return false;
        }
        *currPtr = baseCopy;
        return true;
    }
    current.push_back(baseCopy);
    return true;
}
} // namespace

std::string NormalizeEmitterNameBase(const std::string &name)
{
    const size_t hashPos = name.find('#');
    std::string out = LowerAscii(hashPos == std::string::npos ? name : name.substr(0, hashPos));
    for (char &c : out)
    {
        if (c == '*')
        {
            c = '#';
        }
    }

    const size_t dot = out.find_last_of('.');
    if (dot != std::string::npos && out.find(':', dot) == std::string::npos)
    {
        out.erase(dot);
    }

    return out;
}

void BuildEmitterOverrideCandidates(const std::string &name, std::vector<std::string> &outNames)
{
    const std::string base = NormalizeEmitterNameBase(name);
    auto pushUnique = [&outNames](const std::string &val) {
        if (!val.empty() && std::find(outNames.begin(), outNames.end(), val) == outNames.end())
        {
            outNames.push_back(val);
        }
    };

    pushUnique(base);
    if (base.compare(0, 9, "textures/") == 0)
    {
        pushUnique(base.substr(9));
    }
    else if (base.find('/') == std::string::npos && base.find(':') == std::string::npos)
    {
        pushUnique("textures/" + base);
    }

    const size_t colon = base.find(':');
    if (colon != std::string::npos)
    {
        pushUnique(NormalizeEmitterNameBase(base.substr(0, colon)));
        pushUnique(base.substr(0, colon));
    }
}

const EditorEmitterLightOverride *FindEmitterOverrideExact(
    const std::vector<EditorEmitterLightOverride> &list, const std::string &key)
{
    for (const EditorEmitterLightOverride &ov : list)
    {
        if (LowerAscii(ov.name) == key)
        {
            return &ov;
        }
    }
    return nullptr;
}

bool ResetSingleEmitterOverride(const std::string &className, uint64_t emitterUniqueID,
                                std::vector<EditorEmitterLightOverride> &current,
                                const std::vector<EditorEmitterLightOverride> &baseline)
{
    if (emitterUniqueID == 0 || className.empty())
    {
        return false;
    }

    std::vector<std::string> candidates;
    BuildEmitterOverrideCandidates(className, candidates);
    if (candidates.empty())
    {
        return false;
    }

    const std::string suffix = "#" + std::to_string(emitterUniqueID);
    std::vector<std::string> instanceKeys;
    instanceKeys.reserve(candidates.size());
    for (const std::string &cand : candidates)
    {
        instanceKeys.push_back(cand + suffix);
    }

    if (FindFirstOverrideMatchingKeys(baseline, instanceKeys) != nullptr ||
        FindFirstOverrideMatchingKeys(current, instanceKeys) != nullptr)
    {
        return RestoreOverrideByKeys(current, baseline, instanceKeys);
    }
    return RestoreOverrideByKeys(current, baseline, candidates);
}

size_t CountLeadingBspLights(const std::vector<EditorLight> &lights)
{
    size_t count = 0;
    for (const EditorLight &l : lights)
    {
        if (l.emitterUniqueID != 0)
        {
            break;
        }
        count++;
    }
    return count;
}

const EditorLight *BaselineMapLightAt(int32_t lightIndex,
                                      const std::vector<EditorLight> &sceneLights,
                                      const std::vector<EditorLight> &baselineMapLights)
{
    if (lightIndex < 0)
    {
        return nullptr;
    }

    const size_t idx = size_t(lightIndex);
    const size_t currentBspCount = CountLeadingBspLights(sceneLights);
    const size_t baselineBspCount = CountLeadingBspLights(baselineMapLights);
    if (currentBspCount != baselineBspCount || idx >= currentBspCount ||
        idx >= baselineMapLights.size())
    {
        return nullptr;
    }

    return &baselineMapLights[idx];
}

const EditorLight *BaselineCustomLightAt(int32_t customIndex,
                                         const std::vector<EditorLight> &customLights,
                                         const std::vector<EditorLight> &baselineCustomLights)
{
    if (customIndex < 0 || customLights.size() != baselineCustomLights.size())
    {
        return nullptr;
    }
    const size_t idx = size_t(customIndex);
    return idx < baselineCustomLights.size() ? &baselineCustomLights[idx] : nullptr;
}
