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
static const float QUAKES_PER_METER = 40.0;

float shaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55 * asymmetry - 0.55 * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denominator * denominator);
}

float shaftRangeAttenuation(float distance, float reach)
{
    if (!(reach > 0.0))
    {
        return 1.0;
    }

    if (distance >= reach)
    {
        return 0.0;
    }

    const float ratio = distance / reach;
    const float window = saturate(1.0 - ratio * ratio);
    return window * window;
}

float shaftWorldMask(float3 p)
{
    const float3 distance = abs((p - params.worldCenter.xyz) * params.worldHalfSizeInv.xyz);
    const float edge = max(distance.x, max(distance.y, distance.z));
    return 1.0 - smoothstep(0.75, 1.5, edge);
}

void shaftMediumSigma(float3 p, out float scatteringSigma, out float extinctionSigma)
{
    const float ambientSigma = max(params.mediumSigma, 0.0) * shaftWorldMask(p);
    float localSigma = 0.0;

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
        localSigma += max(density, 0.0) * max(params.mediumLocalScale, 0.0);
    }

    scatteringSigma = max(ambientSigma * QUAKES_PER_METER + localSigma, 0.0);
    extinctionSigma = max(ambientSigma + localSigma, 0.0);
}

float3 volumeRayDirection(float2 cellUv)
{
    const float2 ndc = cellUv * 2.0 - 1.0;
    float4 farPoint = mul(globalUniform.volumeViewProjInv, float4(ndc, 1.0, 1.0));
    farPoint.xyz *= abs(farPoint.w) > 1e-6 ? (1.0 / farPoint.w) : 1.0;
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

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE,
                         (globalUniform.rayCullMaskWorld_Shadow & ~(uint)INSTANCE_MASK_GLASS), ray);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

bool sampleVolumeHistory(int3 cell, float3 center, out float4 history)
{
    history = (float4)0;
    if (params.temporalEnabled == 0u || params.historyValid == 0u)
    {
        return false;
    }

    const float4 clip = mul(globalUniform.volumeViewProj_Prev, float4(center, 1.0));
    if (!(clip.w > 0.000001))
    {
        return false;
    }

    const float2 uv = clip.xy * (0.5 / clip.w) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return false;
    }

    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float radialDistance = length(center - params.prevCameraPosition.xyz);
    const float normalizedDepth = (radialDistance - nearPlane) / (farPlane - nearPlane);
    if (normalizedDepth < 0.0 || normalizedDepth > 1.0)
    {
        return false;
    }

    const int2 previousXY = int2(clamp(uv, 0.0, 1.0) *
                                 float2((float)params.volumeSizeX, (float)params.volumeSizeY));
    const int previousZ = int(clamp(sqrt(normalizedDepth), 0.0, 1.0) * (float)(params.volumeSizeZ - 1u) + 0.5);
    history = historyVolume.Load(int4(min(previousXY.x, (int)params.volumeSizeX - 1),
                                      min(previousXY.y, (int)params.volumeSizeY - 1),
                                      clamp(previousZ, 0, (int)params.volumeSizeZ - 1), 0));
    return true;
}

float3 evaluateProjectedAreaLightShaft(const TexturedAreaLight tal, float3 center, float3 toViewer,
                                      float reach, float powerScale)
{
    const LightSample ls = sampleProjectedAreaLight(tal, center, float2(0.5, 0.5));
    if (!(ls.dw > 0.0))
    {
        return (float3)0;
    }

    const float3 emitted = ls.color * (ls.dw * powerScale);
    if (max(emitted.r, max(emitted.g, emitted.b)) <= 1e-6)
    {
        return (float3)0;
    }

    const float3 fromLight = center - ls.position;
    const float distToLight = length(fromLight);
    if (distToLight < 0.001 || (reach > 0.0 && distToLight >= reach))
    {
        return (float3)0;
    }

    if (!lightShaftVisible(center, ls.position))
    {
        return (float3)0;
    }

    const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), 0.5);
    return emitted * (shaftRangeAttenuation(distToLight, reach) * phase);
}

float3 evaluateTexturedAreaLightShaft(const TexturedAreaLight tal, float3 center, float3 toViewer,
                                     float reach, float powerScale, uint samples, uint baseSeed)
{
    if (tal.projector > 0.5)
    {
        return evaluateProjectedAreaLightShaft(tal, center, toViewer, reach, powerScale);
    }

    const int vertCount = clamp(tal.numVerts, 1, MAX_TEXTURED_AREA_LIGHT_VERTS);
    float2 uvMin = tal.uvVerts[0];
    float2 uvMax = tal.uvVerts[0];
    for (int i = 1; i < MAX_TEXTURED_AREA_LIGHT_VERTS; i++)
    {
        if (i >= vertCount)
        {
            break;
        }
        uvMin = min(uvMin, tal.uvVerts[i]);
        uvMax = max(uvMax, tal.uvVerts[i]);
    }

    const float2 uvMid = 0.5 * (uvMin + uvMax);
    const float3 polyCenter = texturedAreaLightWorldPos(tal, uvMid);
    const float h = dot(tal.normal, center - polyCenter);
    if (h <= 1e-4 || (reach > 0.0 && h >= reach))
    {
        return (float3)0;
    }

    const float a11 = dot(tal.A, tal.A);
    const float a12 = dot(tal.A, tal.B);
    const float a22 = dot(tal.B, tal.B);
    const float det = a11 * a22 - a12 * a12;
    if (!(det > 1e-12))
    {
        return (float3)0;
    }

    const float3 pPerp = center - tal.normal * h;
    const float3 rel = pPerp - tal.C;
    const float b1 = dot(tal.A, rel);
    const float b2 = dot(tal.B, rel);
    const float2 uvPerp = float2(b1 * a22 - b2 * a12, b2 * a11 - b1 * a12) / det;

    float uLo = uvMin.x;
    float uHi = uvMax.x;
    float vLo = uvMin.y;
    float vHi = uvMax.y;

    bool hasRMax = false;
    float rMax = 0.0;
    if (reach > 0.0)
    {
        rMax = sqrt(max(reach * reach - h * h, 0.0));
        hasRMax = true;
    }
    if (tal.coneCosOuter > 0.0)
    {
        const float rCone = h * sqrt(max(1.0 - tal.coneCosOuter * tal.coneCosOuter, 0.0)) /
                            max(tal.coneCosOuter, 0.05);
        rMax = hasRMax ? min(rMax, rCone) : rCone;
        hasRMax = true;
    }

    if (hasRMax)
    {
        const float du = rMax * sqrt(max(a22 / det, 0.0));
        const float dv = rMax * sqrt(max(a11 / det, 0.0));
        uLo = max(uLo, uvPerp.x - du);
        uHi = min(uHi, uvPerp.x + du);
        vLo = max(vLo, uvPerp.y - dv);
        vHi = min(vHi, uvPerp.y + dv);
    }

    if (!(uLo < uHi && vLo < vHi))
    {
        return (float3)0;
    }

    float2 uvClosest = clamp(uvPerp, float2(uLo, vLo), float2(uHi, vHi));
    if (abs(a12) > 1e-6 * sqrt(max(a11 * a22, 0.0)))
    {
        const float vOpt = clamp(uvPerp.y - (a12 / max(a22, 1e-12)) * (uvClosest.x - uvPerp.x), vLo, vHi);
        const float uOpt = clamp(uvPerp.x - (a12 / max(a11, 1e-12)) * (uvClosest.y - uvPerp.y), uLo, uHi);
        const float3 c1 = center - texturedAreaLightWorldPos(tal, float2(uvClosest.x, vOpt));
        const float3 c2 = center - texturedAreaLightWorldPos(tal, float2(uOpt, uvClosest.y));
        uvClosest = dot(c1, c1) < dot(c2, c2) ? float2(uvClosest.x, vOpt) : float2(uOpt, uvClosest.y);
    }

    const float3 closestPos = texturedAreaLightWorldPos(tal, uvClosest);
    const DirectionAndLength closestToCell = calcDirectionAndLength(closestPos, center);
    if ((reach > 0.0 && closestToCell.len >= reach) ||
        (tal.coneCosOuter > 0.0 && dot(tal.normal, closestToCell.dir) <= tal.coneCosOuter))
    {
        return (float3)0;
    }

    float polyUvArea = 0.0;
    for (int t = 0; t < vertCount - 2; t++)
    {
        const float2 e1 = tal.uvVerts[t + 1] - tal.uvVerts[0];
        const float2 e2 = tal.uvVerts[t + 2] - tal.uvVerts[0];
        polyUvArea += 0.5 * abs(e1.x * e2.y - e1.y * e2.x);
    }
    const float bboxUvArea = max((uvMax.x - uvMin.x) * (uvMax.y - uvMin.y), 1e-8);
    const float refUvArea = (vertCount >= 3 && polyUvArea > 1e-8) ? polyUvArea : bboxUvArea;
    const float windowUvArea = (uHi - uLo) * (vHi - vLo);
    const float windowArea = tal.area * (windowUvArea / refUvArea);

    const uint textureIndex = asuint(tal.textureIndex);
    float3 sampleSum = (float3)0;
    float3 visTargetSum = (float3)0;
    float visWeightSum = 0.0;
    const float invSamples = 1.0 / (float)samples;

    for (uint sy = 0; sy < samples; sy++)
    {
        for (uint sx = 0; sx < samples; sx++)
        {
            const uint sampleIndex = sy * samples + sx;
            const uint seed = baseSeed + sampleIndex * 131u;
            const float2 rnd = float2(hash1(seed), hash1(seed + 0x9e3779b9u));
            const float2 uv = float2(
                lerp(uLo, uHi, ((float)sx + rnd.x) * invSamples),
                lerp(vLo, vHi, ((float)sy + rnd.y) * invSamples));

            if (vertCount >= 3 && !isUvInsideConvexPolygon(tal.uvVerts, vertCount, uv))
            {
                continue;
            }

            float mask = 1.0;
            float emiss = tal.meanEmiss;
            if (textureIndex != 0u)
            {
                mask = getTextureSampleLod(textureIndex, uv, 0.0).b;
                emiss = 1.0;
            }

            if (!(mask > 0.0) || !(emiss > 0.0))
            {
                continue;
            }

            const float3 pos = texturedAreaLightWorldPos(tal, uv);
            const DirectionAndLength lightToSurf = calcDirectionAndLength(pos, center);
            if (lightToSurf.len < 0.001 || (reach > 0.0 && lightToSurf.len >= reach))
            {
                continue;
            }

            const float cosNL = max(dot(tal.normal, lightToSurf.dir), 0.0);
            const float spotlight = (tal.coneCosOuter > 0.0)
                ? getSpotFactor(cosNL, tal.coneCosInner, tal.coneCosOuter)
                : sqrt(cosNL);
            if (!(spotlight > 0.0))
            {
                continue;
            }

            const float dw = safeSolidAngle(emiss * getTexturedAreaLightDw(windowArea, tal.normal, lightToSurf.dir, lightToSurf.len));
            if (!(dw > 0.0))
            {
                continue;
            }

            const float3 emitted = tal.color * (mask * spotlight * dw * powerScale);
            const float phase = shaftPhase(dot(-lightToSurf.dir, toViewer), 0.5);
            const float3 contrib = emitted * (shaftRangeAttenuation(lightToSurf.len, reach) * phase);
            const float lum = max(contrib.r, max(contrib.g, contrib.b));
            if (lum <= 1e-7)
            {
                continue;
            }

            sampleSum += contrib;
            visTargetSum += pos * lum;
            visWeightSum += lum;
        }
    }

    if (max(sampleSum.r, max(sampleSum.g, sampleSum.b)) <= 1e-6 || !(visWeightSum > 0.0))
    {
        return (float3)0;
    }

    const float3 visTarget = visWeightSum > 0.0 ? (visTargetSum / visWeightSum) : closestPos;
    if (!lightShaftVisible(center, visTarget))
    {
        return (float3)0;
    }

    return sampleSum / (float)(samples * samples);
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
    const uint baseSeed = cell.x * 1973u + cell.y * 9277u + cell.z * 4391u +
                          shaftIndex * 65537u + globalUniform.frameId * 7919u;
    const uint memberCount = (uint)shaft.surface.y;

    if (memberCount > 0u)
    {
        const uint memberBase = (uint)shaft.surface.x;
        const uint textureIndex = (uint)shaft.surface.z;
        const float meanEmiss = shaft.surface.w;
        const bool projector = shaft.emission.x > 0.5;
        float3 groupSum = (float3)0;

        for (uint mIdx = 0u; mIdx < memberCount; mIdx++)
        {
            const ShDtalMember m = dtalMembers[memberBase + mIdx];

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

            groupSum += evaluateTexturedAreaLightShaft(tal, center, toViewer, reach, 1.0, samples,
                                                      baseSeed + mIdx * 4099u);
        }

        return groupSum;
    }

    if (shaft.source.x > 0.5)
    {
        const uint lightIndex = (uint)shaft.source.x - 1u;
        const ShLightEncoded encoded = lightSources[lightIndex];
        const float powerScale = max(shaft.source.y, 0.0);

        if (encoded.lightType == LIGHT_TYPE_TEXTURED_AREA)
        {
            const TexturedAreaLight tal = decodeAsTexturedAreaLight(encoded);
            return evaluateTexturedAreaLightShaft(tal, center, toViewer, reach, powerScale, samples, baseSeed);
        }

        float3 sampleSum = (float3)0;
        float3 visTargetSum = (float3)0;
        float visWeightSum = 0.0;

        for (uint sy = 0; sy < samples; sy++)
        {
            for (uint sx = 0; sx < samples; sx++)
            {
                const uint sampleIndex = sy * samples + sx;
                const uint seed = baseSeed + sampleIndex * 131u;
                const float2 rnd = float2(hash1(seed), hash1(seed + 0x9e3779b9u));
                const LightSample ls = sampleLight(encoded, center, rnd);
                if (!(ls.dw > 0.0))
                {
                    continue;
                }

                const float3 emitted = ls.color * (ls.dw * powerScale);
                if (max(emitted.r, max(emitted.g, emitted.b)) <= 1e-6)
                {
                    continue;
                }

                const float3 fromLight = center - ls.position;
                const float distToLight = length(fromLight);
                if (distToLight < 0.001 || (reach > 0.0 && distToLight >= reach))
                {
                    continue;
                }

                const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), 0.5);
                const float3 contrib = emitted * (shaftRangeAttenuation(distToLight, reach) * phase);
                const float lum = max(contrib.r, max(contrib.g, contrib.b));
                if (lum <= 1e-7)
                {
                    continue;
                }

                sampleSum += contrib;
                visTargetSum += ls.position * lum;
                visWeightSum += lum;
            }
        }

        if (max(sampleSum.r, max(sampleSum.g, sampleSum.b)) <= 1e-6 || !(visWeightSum > 0.0))
        {
            return (float3)0;
        }

        const float3 visTarget = visTargetSum / visWeightSum;
        if (!lightShaftVisible(center, visTarget))
        {
            return (float3)0;
        }

        return sampleSum / (float)(samples * samples);
    }

    const float3 emitter = shaft.positionRadius.xyz;
    const float3 fromLight = center - emitter;
    const float distToLight = length(fromLight);
    if (distToLight <= 0.001 || (reach > 0.0 && distToLight >= reach))
    {
        return (float3)0;
    }

    const float3 direction = shaft.direction.xyz;
    float cone = 1.0;
    if (dot(direction, direction) > 0.000001)
    {
        const float cosAngle = dot(fromLight / distToLight, normalize(direction));
        cone = smoothstep(shaft.cone.y, max(shaft.cone.x, shaft.cone.y + 1e-4), cosAngle);
    }
    if (!(cone > 0.0))
    {
        return (float3)0;
    }

    const float radius = max(shaft.positionRadius.w, 0.001);
    const float attenuation = radius * radius / (distToLight * distToLight + radius * radius);
    const float3 emitted = shaft.color.rgb * (cone * attenuation * shaftRangeAttenuation(distToLight, reach));
    if (max(emitted.r, max(emitted.g, emitted.b)) <= 1e-6)
    {
        return (float3)0;
    }

    if (!lightShaftVisible(center, emitter))
    {
        return (float3)0;
    }

    const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), 0.5);
    return emitted * phase;
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
    float scatteringSigma = 0.0;
    float extinctionSigma = 0.0;
    shaftMediumSigma(center, scatteringSigma, extinctionSigma);
    const float3 albedo = (float3)max(params.mediumAlbedo, 0.0);
    const float3 sigmaS = albedo * scatteringSigma;

    if (params.debugMode == 1u)
    {
        lightShaftVolume[cell] = float4(scatteringSigma, scatteringSigma, scatteringSigma, 1.0);
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

        radiance += params.sunColor.rgb * visibility * shaftPhase(dot(sunDirection, toViewer), 0.5);
    }

    float4 current = float4(radiance * sigmaS, extinctionSigma);

    if (params.debugMode == 2u)
    {
        lightShaftVolume[cell] = current;
        return;
    }

    if (params.debugMode == 4u)
    {
        float4 history = (float4)0;
        sampleVolumeHistory(cell, center, history);
        lightShaftVolume[cell] = history;
        return;
    }

    if (params.temporalEnabled != 0u && params.historyValid != 0u && params.mediumHistoryWeight > 0.0)
    {
        float4 history = (float4)0;
        if (sampleVolumeHistory(cell, center, history))
        {
            const float weight = saturate(params.mediumHistoryWeight);
            const float3 clampedHistory = clamp(history.rgb, current.rgb * 0.25,
                                                max(current.rgb * 4.0, history.rgb * 0.75) + 1e-3);
            current.rgb = lerp(current.rgb, clampedHistory, weight);
            current.a = lerp(current.a, clamp(history.a, current.a * 0.5, current.a * 2.0 + 1e-6), weight);
        }
    }

    if (params.debugMode == 5u)
    {
        lightShaftVolume[cell] = float4(params.mediumHistoryWeight, params.mediumHistoryWeight,
                                        params.mediumHistoryWeight, 1.0);
        return;
    }

    lightShaftVolume[cell] = current;
}
