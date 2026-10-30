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

// Independent implementation of standard froxel volumetric techniques; no reference
// renderer source is copied.

#define DESC_SET_TLAS 0
#define DESC_SET_LIGHT_SHAFT_PARAMS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_LIGHT_SHAFT_VOLUME 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_LIGHT_SOURCES 5

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"
#include "Light.hlsli"

[[vk::binding(0, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] ConstantBuffer<LightShaftVolumeParams_BT> params;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_PARAMS)]] Texture3D<float4> historyVolume;
[[vk::binding(2, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<uint> lightList;
[[vk::binding(3, DESC_SET_LIGHT_SHAFT_PARAMS)]] RWStructuredBuffer<uint> rayCounter;
[[vk::binding(4, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<float> sunVisibility;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

static const float PI = 3.1415926535;

float shaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55 * asymmetry - 0.55 * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denominator * denominator);
}

float shaftWorldMask(float3 p)
{
    const float3 distance = abs((p - params.worldCenter.xyz) * params.worldHalfSizeInv.xyz);
    const float edge = max(distance.x, max(distance.y, distance.z));
    return 1.0 - smoothstep(0.75, 1.5, edge);
}

float shaftMediumSigma(float3 p)
{
    float sigma = max(params.mediumSigma, 0.0) * shaftWorldMask(p);

    for (int i = 0; i < MAX_FOG_VOLUMES; i++)
    {
        if (globalUniform.fogIsActive[i] == 0u)
        {
            continue;
        }

        const float3 mins = globalUniform.fogMins[i].xyz;
        const float3 maxs = globalUniform.fogMaxs[i].xyz;
        if (any(p < mins) || any(p > maxs))
        {
            continue;
        }

        const float density = dot(globalUniform.fogDensity[i].xyz, p) + globalUniform.fogDensity[i].w;
        sigma += max(density, 0.0) * max(params.mediumLocalScale, 0.0);
    }

    return max(sigma, 0.0);
}

float3 volumeRayDirection(float2 cellUv)
{
    const float2 ndc = cellUv * 2.0 - 1.0;
    float4 farPoint = mul(globalUniform.volumeViewProjInv, float4(ndc, 1.0, 1.0));
    farPoint.xyz /= farPoint.w;
    return normalize(farPoint.xyz - globalUniform.cameraPosition.xyz);
}

float3 volumeGetCenter(int3 cell)
{
    const float2 cellUv = ((float2)cell.xy + 0.5) /
                          float2((float)params.volumeSizeX, (float)params.volumeSizeY);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float t = saturate((cell.z + 0.5) / (float)params.volumeSizeZ);
    const float distance = nearPlane + (farPlane - nearPlane) * t * t;
    return globalUniform.cameraPosition.xyz + volumeRayDirection(cellUv) * distance;
}

uint volumeClusterIndex(int3 cell)
{
    const uint tilesX = max(params.tilesX, 1u);
    const uint tilesY = max(params.tilesY, 1u);
    const uint zBins = max(params.zBins, 1u);
    const uint tileSizeX = max(1u, (params.volumeSizeX + tilesX - 1) / tilesX);
    const uint tileSizeY = max(1u, (params.volumeSizeY + tilesY - 1) / tilesY);
    const uint tileX = min((uint)cell.x / tileSizeX, tilesX - 1u);
    const uint tileY = min((uint)cell.y / tileSizeY, tilesY - 1u);
    const uint zBin = min((uint)cell.z * zBins / params.volumeSizeZ, zBins - 1u);
    return (zBin * tilesY + tileY) * tilesX + tileX;
}

float hash1(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value & 0x00ffffffu) / 16777215.0;
}

bool lightShaftVisible(float3 origin, float3 target)
{
    const float3 delta = target - origin;
    const float distance = length(delta);

    if (distance <= 0.001)
    {
        return true;
    }

    uint slot;
    InterlockedAdd(rayCounter[0], 1u, slot);
    if (params.rayBudget != 0u && slot >= params.rayBudget)
    {
        return false;
    }

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = delta / distance;
    ray.TMin = 0.001;
    ray.TMax = distance - 0.001;

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, globalUniform.rayCullMaskWorld, ray);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

float4 sampleVolumeHistory(int3 cell, float3 center)
{
    if (params.temporalEnabled == 0u || params.historyValid == 0u)
    {
        return (float4)0;
    }

    const float4 clip = mul(globalUniform.volumeViewProj_Prev, float4(center, 1.0));
    if (!(clip.w > 0.000001))
    {
        return (float4)0;
    }

    const float2 uv = clip.xy * (0.5 / clip.w) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return (float4)0;
    }

    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float radialDistance = length(center - params.prevCameraPosition.xyz);
    const float normalizedDepth = (radialDistance - nearPlane) / (farPlane - nearPlane);
    if (normalizedDepth < 0.0 || normalizedDepth > 1.0)
    {
        return (float4)0;
    }

    const int2 previousXY = int2(clamp(uv, 0.0, 1.0) *
                                 float2((float)params.volumeSizeX, (float)params.volumeSizeY));
    const int previousZ = int(clamp(sqrt(normalizedDepth), 0.0, 1.0) * (float)(params.volumeSizeZ - 1u) + 0.5);
    return historyVolume.Load(int4(min(previousXY.x, (int)params.volumeSizeX - 1),
                                   min(previousXY.y, (int)params.volumeSizeY - 1),
                                   clamp(previousZ, 0, (int)params.volumeSizeZ - 1), 0));
}

bool shaftConeAllows(const LightShaft_BT shaft, float3 center, float boundsRadius)
{
    const float3 direction = shaft.direction.xyz;
    const float directionLength2 = dot(direction, direction);
    if (directionLength2 <= 0.000001)
    {
        return true;
    }

    const float3 toCell = center - shaft.positionRadius.xyz;
    const float cellDistance = length(toCell);
    if (cellDistance <= 0.001)
    {
        return true;
    }

    const float3 normalizedDirection = direction * rsqrt(directionLength2);
    const float cosAngle = clamp(dot(toCell / cellDistance, normalizedDirection), -1.0, 1.0);
    const float angle = acos(cosAngle);
    const float boundsAngle = asin(saturate(boundsRadius / max(cellDistance, 0.001)));
    const float outerAngle = acos(clamp(shaft.cone.y, -1.0, 1.0));
    return angle <= outerAngle + boundsAngle + 0.05;
}

bool sampleShaftEmitter(const LightShaft_BT shaft, float3 center, float2 rnd,
                        out float3 emitter, out float3 emitted)
{
    emitter = shaft.positionRadius.xyz;
    emitted = (float3)0;

    const uint memberCount = (uint)shaft.surface.y;

    if (memberCount > 0u)
    {
        const uint memberBase = (uint)shaft.surface.x;
        const uint textureIndex = (uint)shaft.surface.z;
        const float meanEmiss = shaft.surface.w;
        const bool projector = shaft.emission.x > 0.5;

        const int column = aliasColumn(rnd.x, (int)memberCount);
        const ShDtalMember columnMember = dtalMembers[memberBase + (uint)column];
        const uint chosen = aliasChoice(rnd.x, (int)memberCount, column,
                                        columnMember.prob, columnMember.aliasIndex);
        if (chosen >= memberCount)
        {
            return false;
        }

        const ShDtalMember m = dtalMembers[memberBase + chosen];
        const float memberPdf = max(m.marginalProb, 1e-6);

        TexturedAreaLight tal;
        tal.A = m.A;
        tal.B = m.B;
        tal.C = m.C;
        tal.normal = m.normal;
        tal.area = m.area;
        tal.textureIndex = asfloat(textureIndex);
        tal.meanEmiss = meanEmiss;
        tal.coneCosInner = shaft.cone.x;
        tal.coneCosOuter = shaft.cone.y;
        tal.projector = projector ? 1.0 : 0.0;
        tal.numVerts = clamp((int)m.numVerts, 0, MAX_TEXTURED_AREA_LIGHT_VERTS);
        tal.color = shaft.color.rgb;

        for (int v = 0; v < MAX_TEXTURED_AREA_LIGHT_VERTS; v++)
        {
            tal.uvVerts[v] = m.uv[v];
        }

        const LightSample ls = sampleTexturedAreaLight(tal, center, rnd);
        if (!(ls.dw > 0.0))
        {
            return false;
        }

        emitter = ls.position;
        emitted = ls.color * (ls.dw / memberPdf);
        return true;
    }

    if (shaft.source.x > 0.5)
    {
        const uint lightIndex = (uint)shaft.source.x - 1u;
        const ShLightEncoded encoded = lightSources[lightIndex];
        const LightSample ls = sampleLight(encoded, center, rnd);
        if (!(ls.dw > 0.0))
        {
            return false;
        }

        emitter = ls.position;
        emitted = ls.color * (ls.dw * max(shaft.source.y, 0.0));
        return true;
    }

    const float3 toCell = center - shaft.positionRadius.xyz;
    const float cellDistance = length(toCell);
    if (cellDistance <= 0.001)
    {
        return false;
    }

    const float3 direction = shaft.direction.xyz;
    const bool hasDirection = dot(direction, direction) > 0.000001;

    float cone = 1.0;
    if (hasDirection)
    {
        const float cosAngle = dot(toCell / cellDistance, normalize(direction));
        cone = smoothstep(shaft.cone.y, shaft.cone.x, cosAngle);
    }

    const float radius = max(shaft.positionRadius.w, 0.001);
    const float attenuation = radius * radius / (cellDistance * cellDistance + radius * radius);

    emitter = shaft.positionRadius.xyz;
    emitted = shaft.color.rgb * (cone * attenuation);
    return cone > 0.0;
}

float3 accumulateShaft(const LightShaft_BT shaft, float3 center, float3 toViewer, uint3 cell,
                       uint shaftIndex)
{
    const float reach = shaft.emission.y;
    const float boundsRadius = max(shaft.source.w, shaft.positionRadius.w);
    const float cellDistance = length(center - shaft.positionRadius.xyz);

    if (reach > 0.0 && cellDistance - boundsRadius > reach)
    {
        return (float3)0;
    }

    if (!shaftConeAllows(shaft, center, boundsRadius))
    {
        return (float3)0;
    }

    const uint samples = max(1u, min(params.lightShaftSamples, 3u));
    const uint frame = globalUniform.frameId;
    float3 sampleSum = (float3)0;
    float visibility = 1.0;
    bool visibilityKnown = false;

    for (uint sy = 0; sy < samples; sy++)
    {
        for (uint sx = 0; sx < samples; sx++)
        {
            const uint sampleIndex = sy * samples + sx;
            const uint seed = cell.x * 1973u + cell.y * 9277u + cell.z * 4391u +
                              shaftIndex * 65537u + sampleIndex * 131u + frame * 7919u;
            const float2 rnd = float2(hash1(seed), hash1(seed + 0x9e3779b9u));

            float3 emitter;
            float3 emitted;
            if (!sampleShaftEmitter(shaft, center, rnd, emitter, emitted))
            {
                continue;
            }

            if (reach > 0.0 && length(center - emitter) > reach)
            {
                continue;
            }

            if (!visibilityKnown)
            {
                visibility = lightShaftVisible(center, emitter) ? 1.0 : 0.0;
                visibilityKnown = true;
            }

            if (visibility <= 0.0)
            {
                continue;
            }

            const float3 fromLight = center - emitter;
            const float distToLight = length(fromLight);
            if (distToLight < 0.001)
            {
                continue;
            }

            const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), 0.5);
            sampleSum += emitted * phase;
        }
    }

    return sampleSum / (float)(samples * samples);
}

[numthreads(4, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int3 cell = int3(dispatchThreadID);
    if (cell.x >= (int)params.volumeSizeX || cell.y >= (int)params.volumeSizeY ||
        cell.z >= (int)params.volumeSizeZ)
    {
        return;
    }

    const float3 center = volumeGetCenter(cell);
    const float3 toViewer = normalize(globalUniform.cameraPosition.xyz - center);
    const float sigma = shaftMediumSigma(center);
    const float3 albedo = (float3)max(params.mediumAlbedo, 0.0);
    const float3 sigmaS = albedo * sigma;

    if (params.debugMode == 1u)
    {
        lightShaftVolume[cell] = float4(sigma, sigma, sigma, 1.0);
        return;
    }

    float3 radiance = params.mediumEmissive.rgb;

    const uint shaftCount = min(params.lightShaftCount, (uint)MAX_LIGHT_SHAFTS);
    const uint clusterCount = params.lightListClusterCount;
    const uint clusterIndex = volumeClusterIndex(cell);

    if (clusterCount > 0u)
    {
        const uint listBase = clusterIndex * (1u + params.lightListCapacity);
        const uint listCount = min(lightList[listBase], params.lightListCapacity);
        for (uint i = 0; i < listCount; i++)
        {
            const uint shaftIndex = lightList[listBase + 1u + i];
            if (shaftIndex < shaftCount)
            {
                radiance += accumulateShaft(params.lightShafts[shaftIndex], center, toViewer, cell,
                                            shaftIndex);
            }
        }
    }
    else
    {
        for (uint i = 0; i < shaftCount; i++)
        {
            radiance += accumulateShaft(params.lightShafts[i], center, toViewer, cell, i);
        }
    }

    if (params.sunEnabled != 0u && dot(params.sunDirection.xyz, params.sunDirection.xyz) > 0.000001)
    {
        const float3 sunDirection = normalize(params.sunDirection.xyz);
        float visibility = 0.0;
        if (clusterCount > 0u && clusterIndex < clusterCount)
        {
            visibility = sunVisibility[clusterIndex];
        }
        else
        {
            visibility = lightShaftVisible(center, center + sunDirection * MAX_RAY_LENGTH) ? 1.0 : 0.0;
        }

        radiance += params.sunColor.rgb * visibility * shaftPhase(dot(-sunDirection, toViewer), 0.5);
    }

    float4 current = float4(radiance * sigmaS, sigma);

    if (params.debugMode == 2u)
    {
        lightShaftVolume[cell] = current;
        return;
    }

    if (params.debugMode == 4u)
    {
        lightShaftVolume[cell] = sampleVolumeHistory(cell, center);
        return;
    }

    if (params.temporalEnabled != 0u && params.historyValid != 0u && params.mediumHistoryWeight > 0.0)
    {
        const float4 history = sampleVolumeHistory(cell, center);
        const float weight = saturate(params.mediumHistoryWeight);
        current.rgb = lerp(current.rgb, clamp(history.rgb, current.rgb * 0.5, current.rgb * 2.0 + 1e-6), weight);
        current.a = lerp(current.a, clamp(history.a, current.a * 0.5, current.a * 2.0 + 1e-6), weight);
    }

    if (params.debugMode == 5u)
    {
        lightShaftVolume[cell] = float4(params.mediumHistoryWeight, params.mediumHistoryWeight,
                                        params.mediumHistoryWeight, 1.0);
        return;
    }

    lightShaftVolume[cell] = current;
}
