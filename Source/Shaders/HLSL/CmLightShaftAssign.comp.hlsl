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

#define DESC_SET_LIGHT_SHAFT_PARAMS 0
#define DESC_SET_LIGHT_SHAFT_LISTS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_TLAS 3

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] ConstantBuffer<LightShaftVolumeParams_BT> params;
[[vk::binding(3, DESC_SET_LIGHT_SHAFT_PARAMS)]] RWStructuredBuffer<uint> rayCounter;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_LISTS)]] RWStructuredBuffer<uint> lightLists;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_LISTS)]] RWStructuredBuffer<float> sunVisibility;
[[vk::binding(0, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;

float3 lightShaftVolumePoint(float2 cellUv, float zNorm)
{
    const float2 ndc = cellUv * 2.0 - 1.0;
    float4 farPoint = mul(globalUniform.volumeViewProjInv, float4(ndc, 1.0, 1.0));
    farPoint.xyz /= farPoint.w;
    const float3 direction = normalize(farPoint.xyz - globalUniform.cameraPosition.xyz);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float distance = nearPlane + (farPlane - nearPlane) * zNorm * zNorm;
    return globalUniform.cameraPosition.xyz + direction * distance;
}

bool lightShaftTraceVisible(float3 origin, float3 target)
{
    const float3 delta = target - origin;
    const float distance = length(delta);

    if (distance <= 0.001)
    {
        return true;
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

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint clusterIndex = dispatchThreadID.x;
    if (clusterIndex >= params.lightListClusterCount)
    {
        return;
    }

    const uint tilesX = max(params.tilesX, 1u);
    const uint tilesY = max(params.tilesY, 1u);
    const uint zBins = max(params.zBins, 1u);
    const uint tileX = clusterIndex % tilesX;
    const uint tileY = (clusterIndex / tilesX) % tilesY;
    const uint zBin = clusterIndex / (tilesX * tilesY);

    const uint tileSizeX = max(1u, (params.volumeSizeX + tilesX - 1) / tilesX);
    const uint tileSizeY = max(1u, (params.volumeSizeY + tilesY - 1) / tilesY);
    const uint sliceSize = max(1u, (params.volumeSizeZ + zBins - 1) / zBins);

    const uint x0 = min(tileX * tileSizeX, params.volumeSizeX - 1u);
    const uint y0 = min(tileY * tileSizeY, params.volumeSizeY - 1u);
    const uint z0 = min(zBin * sliceSize, params.volumeSizeZ - 1u);
    const uint x1 = min(x0 + tileSizeX, params.volumeSizeX);
    const uint y1 = min(y0 + tileSizeY, params.volumeSizeY);
    const uint z1 = min(z0 + sliceSize, params.volumeSizeZ);

    const float2 uv0 = float2(x0, y0) / float2(params.volumeSizeX, params.volumeSizeY);
    const float2 uv1 = float2(x1, y1) / float2(params.volumeSizeX, params.volumeSizeY);
    const float t0 = (float)z0 / (float)params.volumeSizeZ;
    const float t1 = (float)z1 / (float)params.volumeSizeZ;

    const float3 p000 = lightShaftVolumePoint(uv0, t0);
    const float3 p100 = lightShaftVolumePoint(float2(uv1.x, uv0.y), t0);
    const float3 p010 = lightShaftVolumePoint(float2(uv0.x, uv1.y), t0);
    const float3 p110 = lightShaftVolumePoint(uv1, t0);
    const float3 p001 = lightShaftVolumePoint(uv0, t1);
    const float3 p101 = lightShaftVolumePoint(float2(uv1.x, uv0.y), t1);
    const float3 p011 = lightShaftVolumePoint(float2(uv0.x, uv1.y), t1);
    const float3 p111 = lightShaftVolumePoint(uv1, t1);

    const float3 clusterMin = min(min(min(p000, p100), min(p010, p110)),
                                  min(min(p001, p101), min(p011, p111)));
    const float3 clusterMax = max(max(max(p000, p100), max(p010, p110)),
                                  max(max(p001, p101), max(p011, p111)));
    const float3 clusterCenter = (clusterMin + clusterMax) * 0.5;
    const float clusterRadius = length((clusterMax - clusterMin) * 0.5);

    const uint listBase = clusterIndex * (1u + params.lightListCapacity);
    uint count = 0;
    const uint shaftCount = min(params.lightShaftCount, (uint)MAX_LIGHT_SHAFTS);

    for (uint i = 0; i < shaftCount; i++)
    {
        const LightShaft_BT shaft = params.lightShafts[i];
        const float influenceRadius = shaft.source.z;
        if (influenceRadius > 0.0 &&
            distance(clusterCenter, shaft.positionRadius.xyz) > clusterRadius + influenceRadius)
        {
            continue;
        }

        if (count < params.lightListCapacity)
        {
            lightLists[listBase + 1u + count] = i;
            count++;
        }
    }

    lightLists[listBase] = count;

    float sun = 0.0;
    if (params.sunEnabled != 0 && dot(params.sunDirection.xyz, params.sunDirection.xyz) > 0.000001)
    {
        uint slot;
        InterlockedAdd(rayCounter[0], 1u, slot);
        bool visible = false;
        if (params.rayBudget == 0u || slot < params.rayBudget)
        {
            const float3 direction = normalize(params.sunDirection.xyz);
            visible = lightShaftTraceVisible(clusterCenter, clusterCenter + direction * MAX_RAY_LENGTH);
        }
        sun = visible ? 1.0 : 0.0;
    }

    sunVisibility[clusterIndex] = sun;
}
