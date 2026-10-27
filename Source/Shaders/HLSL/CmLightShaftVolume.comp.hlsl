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
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

static const float PI = 3.1415926535;

float shaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55 * asymmetry - 0.55 * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denominator * denominator);
}

float shaftDensity(float3 p)
{
    const float3 distance = abs((p - params.worldCenter.xyz) * params.worldHalfSizeInv.xyz);
    const float edge = max(distance.x, max(distance.y, distance.z));
    return 1.0 - smoothstep(0.75, 1.5, edge);
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

[numthreads(4, 4, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int3 cell = int3(dispatchThreadID);
    if (cell.x >= (int)params.volumeSizeX || cell.y >= (int)params.volumeSizeY ||
        cell.z >= (int)params.volumeSizeZ)
    {
        return;
    }

    const uint shaftCount = min(params.lightShaftCount, (uint)MAX_LIGHT_SHAFTS);
    if (shaftCount == 0)
    {
        lightShaftVolume[cell] = (float4)0;
        return;
    }

    const float3 center0 = volumeGetCenter(cell);
    const float3 center = center0;
    const float3 toViewer = normalize(globalUniform.cameraPosition.xyz - center);
    const float density = 0.02 * shaftDensity(center);
    if (density <= 0.0)
    {
        lightShaftVolume[cell] = (float4)0;
        return;
    }
    float3 radiance = (float3)0;

    for (uint i = 0; i < shaftCount; ++i)
    {
        const LightShaft_BT shaft = params.lightShafts[i];
        const uint memberBase = (uint)shaft.surface.x;
        const uint memberCount = (uint)shaft.surface.y;
        const uint textureIndex = (uint)shaft.surface.z;
        const float meanEmiss = shaft.surface.w;
        const bool projector = shaft.emission.x > 0.5;

        const float3 source = shaft.positionRadius.xyz;
        const float radius = max(shaft.positionRadius.w, 0.001);
        float3 lightDir = shaft.direction.xyz;

        if (dot(lightDir, lightDir) <= 0.000001)
        {
            continue;
        }

        lightDir = normalize(lightDir);

        const float reach = shaft.emission.y;
        const float3 toCell = center - source;
        const float cellDistance = length(toCell);

        if (reach > 0.0 && cellDistance > reach)
        {
            continue;
        }

        if (cellDistance > 0.001 && dot(toCell / cellDistance, lightDir) < shaft.cone.y - 0.001)
        {
            continue;
        }

        const uint samples = max(1u, min(params.lightShaftSamples, 3u));
        float3 sampleSum = (float3)0;

        for (uint sy = 0; sy < samples; sy++)
        {
            for (uint sx = 0; sx < samples; sx++)
            {
                const uint sampleIndex = sy * samples + sx;
                const uint seed = (uint)(cell.x * 1973 + cell.y * 9277 + cell.z * 4391 +
                                         (int)i * 65537 + (int)sampleIndex * 131);

                float3 emitter = source;
                float3 emitted = (float3)0;

                if (memberCount > 0u)
                {
                    const float memberRnd = hash1(seed);
                    const int column = aliasColumn(memberRnd, (int)memberCount);
                    const ShDtalMember columnMember = dtalMembers[memberBase + (uint)column];
                    const uint chosen = aliasChoice(memberRnd, (int)memberCount, column,
                                                    columnMember.prob, columnMember.aliasIndex);

                    if (chosen >= memberCount)
                    {
                        continue;
                    }

                    const ShDtalMember m = dtalMembers[memberBase + chosen];
                    const float memberPdf = max(m.marginalProb, 1e-6);

                    TexturedAreaLight tal;
                    tal.A = m.A;
                    tal.B = m.B;
                    tal.C = m.C;
                    tal.normal = m.normal;
                    tal.area = m.area;
                    tal.textureIndex = textureIndex;
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

                    const LightSample ls = sampleTexturedAreaLight(
                        tal, center, float2(hash1(seed + 7u), hash1(seed + 13u)));

                    if (!(ls.dw > 0.0))
                    {
                        continue;
                    }

                    emitter = ls.position;
                    emitted = ls.color * (ls.dw / memberPdf);
                }
                else
                {
                    const float3 fromSource = center - source;
                    const float distToSource = length(fromSource);

                    if (distToSource < 0.001)
                    {
                        continue;
                    }

                    const float cosAngle = dot(fromSource / distToSource, lightDir);
                    const float cone = smoothstep(shaft.cone.y, shaft.cone.x, cosAngle);

                    if (cone <= 0.0)
                    {
                        continue;
                    }

                    const float sourceRadius = max(radius, 0.15);
                    const float attenuation =
                        sourceRadius * sourceRadius / (distToSource * distToSource + sourceRadius * sourceRadius);

                    emitter = source + lightDir * 0.001;
                    emitted = shaft.color.rgb * (cone * attenuation);
                }

                if (!lightShaftVisible(center, emitter))
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

        radiance += sampleSum / (float)(samples * samples);
    }

    lightShaftVolume[cell] = float4(radiance * density, 0.0);
}
