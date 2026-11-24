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
#define DESC_SET_CLOUD_SHADOW 6

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"
#include "Light.hlsli"
#include "CloudShadowWorld.hlsli"

[[vk::binding(0, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] ConstantBuffer<LightShaftVolumeParams_BT> params;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_PARAMS)]] Texture3D<float4> historyVolume;
[[vk::binding(2, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<uint> lightList;
[[vk::binding(3, DESC_SET_LIGHT_SHAFT_PARAMS)]] RWStructuredBuffer<uint> rayCounter;
[[vk::binding(4, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<float> sunVisibility;
[[vk::binding(5, DESC_SET_LIGHT_SHAFT_PARAMS)]] SamplerState historySampler;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

static const float PI = 3.1415926535;
static const float QUAKES_PER_METER = 40.0;
static const float LOCAL_SHAFT_PHASE_G = 0.2;

float shaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55 * asymmetry - 0.55 * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0 + k * cosTheta;
    return (1.0 - k * k) / (4.0 * PI * denominator * denominator);
}

float shaftDistanceAttenuation(float distance, float reach, float falloff)
{
    if (reach > 0.0 && distance >= reach)
    {
        return 0.0;
    }

    const float rCore = 0.75;
    const float r2 = rCore * rCore;
    const float f = clamp(falloff > 0.0 ? falloff : 1.0, 0.01, 4.0);
    const float radial = pow((1.0 + r2) / (distance * distance + r2), 0.5 * f);

    float window = 1.0;
    if (reach > 0.0)
    {
        const float ratio = distance / reach;
        const float w = saturate(1.0 - ratio * ratio);
        window = w * w;
    }

    return radial * window;
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

float3 volumeFroxelJitter(int3 cell)
{
    if (params.temporalEnabled == 0u || params.historyValid == 0u)
    {
        return float3(0.5, 0.5, 0.5);
    }

    const uint frameSlot = globalUniform.frameId & 63u;
    const float3 base = frac(float3(0.5, 0.5, 0.5) +
                             (float)frameSlot * float3(0.8191725134, 0.6710436067, 0.5497004779));
    const float3 spatial = float3(
        frac((float)cell.x * 0.3819660113 + (float)cell.y * 0.6180339887 + (float)cell.z * 0.2360679775),
        frac((float)cell.x * 0.7182818285 + (float)cell.y * 0.3141592654 + (float)cell.z * 0.5772156649),
        frac((float)cell.x * 0.5284271247 + (float)cell.y * 0.8660254038 + (float)cell.z * 0.4142135624));
    return lerp(0.08, 0.92, frac(base + spatial));
}

void volumeCellSegment(int3 cell, float3 jitter,
                       out float3 segStart, out float3 segEnd, out float3 center)
{
    const float2 volSize = float2((float)params.volumeSizeX, (float)params.volumeSizeY);
    const float2 centerUv = ((float2)cell.xy + 0.5) / volSize;
    const float2 jitterUv = ((float2)cell.xy + jitter.xy) / volSize;
    const float3 centerDir = volumeRayDirection(centerUv);
    const float3 rayDir = volumeRayDirection(jitterUv);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float invZ = 1.0 / (float)params.volumeSizeZ;
    const float t0 = saturate((float)cell.z * invZ);
    const float t1 = saturate((float)(cell.z + 1) * invZ);
    const float tm = saturate(((float)cell.z + 0.5) * invZ);
    const float3 camPos = globalUniform.cameraPosition.xyz;
    segStart = camPos + rayDir * (nearPlane + (farPlane - nearPlane) * t0 * t0);
    segEnd = camPos + rayDir * (nearPlane + (farPlane - nearPlane) * t1 * t1);
    center = camPos + centerDir * (nearPlane + (farPlane - nearPlane) * tm * tm);
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

    query.TraceRayInline(topLevelAS, RAY_FLAG_NONE,
                         (globalUniform.rayCullMaskWorld_Shadow & ~(uint)INSTANCE_MASK_GLASS), ray);

    while (query.Proceed())
    {
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

bool sampleVolumeHistory(int3 cell, float3 center, out float4 history, out float edgeFade)
{
    history = (float4)0;
    edgeFade = 0.0;
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

    const float3 prevUvw = float3(uv, sqrt(normalizedDepth));
    history = historyVolume.SampleLevel(historySampler, prevUvw, 0.0);
    if (any(!isfinite(history)))
    {
        history = (float4)0;
        return false;
    }

    edgeFade = saturate(min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)) * 32.0);
    return true;
}

bool evaluateRectAreaShaft(const LightShaft_BT shaft, float3 evalPos, float3 toViewer,
                           float reach, uint samples,
                           out float3 outRadiance, out float3 outVisTarget)
{
    outRadiance = (float3)0;
    outVisTarget = shaft.positionRadius.xyz;

    const float3 origin = shaft.positionRadius.xyz;
    const float3 N = normalize(shaft.direction.xyz);
    const float h = dot(N, evalPos - origin);
    if (h <= 1e-4 || (reach > 0.0 && h >= reach))
    {
        return false;
    }

    const float3 U = normalize(shaft.tangentU.xyz);
    const float3 V = normalize(shaft.tangentV.xyz);
    const float halfU = shaft.tangentU.w;
    const float halfV = shaft.tangentV.w;

    const float3 rel = evalPos - origin;
    const float uPerp = dot(rel, U);
    const float vPerp = dot(rel, V);

    float uLo = -halfU;
    float uHi = halfU;
    float vLo = -halfV;
    float vHi = halfV;

    bool hasRMax = false;
    float rMax = 0.0;
    if (reach > 0.0)
    {
        rMax = sqrt(max(reach * reach - h * h, 0.0));
        hasRMax = true;
    }
    if (shaft.cone.y > 0.0)
    {
        const float rCone = h * sqrt(max(1.0 - shaft.cone.y * shaft.cone.y, 0.0)) /
                            max(shaft.cone.y, 0.05);
        rMax = hasRMax ? min(rMax, rCone) : rCone;
        hasRMax = true;
    }

    if (hasRMax)
    {
        uLo = max(uLo, uPerp - rMax);
        uHi = min(uHi, uPerp + rMax);
        vLo = max(vLo, vPerp - rMax);
        vHi = min(vHi, vPerp + rMax);
    }

    if (!(uLo < uHi && vLo < vHi))
    {
        return false;
    }

    const float2 uvClosest = clamp(float2(uPerp, vPerp), float2(uLo, vLo), float2(uHi, vHi));
    const float3 closestPos = origin + U * uvClosest.x + V * uvClosest.y;
    const DirectionAndLength closestToCell = calcDirectionAndLength(closestPos, evalPos);
    if ((reach > 0.0 && closestToCell.len >= reach) ||
        (shaft.cone.y > 0.0 && dot(N, closestToCell.dir) <= shaft.cone.y))
    {
        return false;
    }

    const float rectArea = max(4.0 * halfU * halfV, 1e-8);
    const float polyArea = shaft.positionRadius.w > 0.0
        ? (PI * shaft.positionRadius.w * shaft.positionRadius.w)
        : rectArea;
    const float windowArea = polyArea * (((uHi - uLo) * (vHi - vLo)) / rectArea);
    const float meanEmiss = max(shaft.surface.w, 0.0);
    if (!(windowArea > 0.0) || !(meanEmiss > 0.0))
    {
        return false;
    }

    const float falloff = shaft.emission.z > 0.0 ? shaft.emission.z : 1.0;
    float3 sampleSum = (float3)0;
    float3 visTargetSum = (float3)0;
    float visWeightSum = 0.0;
    const float invSamples = 1.0 / (float)samples;

    for (uint sy = 0; sy < samples; sy++)
    {
        const float vSample = lerp(vLo, vHi, ((float)sy + 0.5) * invSamples);
        for (uint sx = 0; sx < samples; sx++)
        {
            const float uSample = lerp(uLo, uHi, ((float)sx + 0.5) * invSamples);
            const float3 pos = origin + U * uSample + V * vSample;
            const DirectionAndLength lightToCell = calcDirectionAndLength(pos, evalPos);
            if (lightToCell.len < 0.001 || (reach > 0.0 && lightToCell.len >= reach))
            {
                continue;
            }

            const float cosNL = max(dot(N, lightToCell.dir), 0.0);
            const float spotlight = (shaft.cone.y > 0.0)
                ? getSpotFactor(cosNL, shaft.cone.x, shaft.cone.y)
                : sqrt(cosNL);
            if (!(spotlight > 0.0))
            {
                continue;
            }

            const float atten = shaftDistanceAttenuation(lightToCell.len, reach, falloff);
            const float dw = safeSolidAngle(meanEmiss * windowArea * cosNL * atten);
            if (!(dw > 0.0))
            {
                continue;
            }

            const float phase = shaftPhase(dot(-lightToCell.dir, toViewer), LOCAL_SHAFT_PHASE_G);
            const float3 contrib = shaft.color.rgb * (spotlight * dw * phase);
            const float lum = max(contrib.r, max(contrib.g, contrib.b));
            if (lum <= 1e-8)
            {
                continue;
            }

            sampleSum += contrib;
            visTargetSum += pos * lum;
            visWeightSum += lum;
        }
    }

    if (!(visWeightSum > 0.0))
    {
        return false;
    }

    outRadiance = sampleSum / (float)(samples * samples);
    outVisTarget = (visTargetSum / visWeightSum) + N * 0.002;
    return max(outRadiance.r, max(outRadiance.g, outRadiance.b)) > 1e-7;
}

bool evaluateProjectedAreaCandidate(const LightShaft_BT shaft, float3 evalPos, float3 toViewer,
                                    float reach, out float3 outRadiance, out float3 outVisTarget)
{
    outRadiance = (float3)0;
    outVisTarget = shaft.positionRadius.xyz;

    TexturedAreaLight tal;
    float powerScale = 1.0;

    if ((uint)shaft.surface.y > 0u)
    {
        const ShDtalMember m = dtalMembers[(uint)shaft.surface.x];
        tal.A = m.A;
        tal.B = m.B;
        tal.C = m.C;
        tal.normal = m.normal;
        tal.area = m.area;
        tal.textureIndex = asfloat((uint)shaft.surface.z);
        tal.meanEmiss = shaft.surface.w;
        tal.coneCosInner = shaft.cone.x;
        tal.coneCosOuter = shaft.cone.y;
        tal.projector = 1.0;
        tal.numVerts = clamp((int)m.numVerts, 0, MAX_TEXTURED_AREA_LIGHT_VERTS);
        tal.color = shaft.color.rgb;
        for (int v = 0; v < MAX_TEXTURED_AREA_LIGHT_VERTS; v++)
        {
            tal.uvVerts[v] = m.uv[v];
        }
    }
    else if (shaft.source.x > 0.5)
    {
        const uint lightIndex = (uint)shaft.source.x - 1u;
        const ShLightEncoded encoded = lightSources[lightIndex];
        if (encoded.lightType != LIGHT_TYPE_TEXTURED_AREA)
        {
            return false;
        }
        tal = decodeAsTexturedAreaLight(encoded);
        powerScale = max(shaft.source.y, 0.0);
    }
    else
    {
        return false;
    }

    const LightSample ls = sampleProjectedAreaLight(tal, evalPos, float2(0.5, 0.5));
    if (!(ls.dw > 0.0))
    {
        return false;
    }

    const float3 fromLight = evalPos - ls.position;
    const float distToLight = length(fromLight);
    if (distToLight < 0.001 || (reach > 0.0 && distToLight >= reach))
    {
        return false;
    }

    const float falloff = shaft.emission.z > 0.0 ? shaft.emission.z : 1.0;
    const float shapedDw = safeSolidAngle(
        ls.dw * (distToLight * distToLight) * shaftDistanceAttenuation(distToLight, reach, falloff));
    const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), LOCAL_SHAFT_PHASE_G);
    outRadiance = ls.color * (shapedDw * powerScale * phase);
    outVisTarget = ls.position + normalize(tal.normal) * 0.002;
    return max(outRadiance.r, max(outRadiance.g, outRadiance.b)) > 1e-7;
}

bool evaluateSourceLightCandidate(const LightShaft_BT shaft, float3 evalPos, float3 toViewer,
                                  float reach, uint samples,
                                  out float3 outRadiance, out float3 outVisTarget)
{
    outRadiance = (float3)0;
    outVisTarget = shaft.positionRadius.xyz;

    const uint lightIndex = (uint)shaft.source.x - 1u;
    const ShLightEncoded encoded = lightSources[lightIndex];
    const float powerScale = max(shaft.source.y, 0.0);
    const float falloff = shaft.emission.z > 0.0 ? shaft.emission.z : 1.0;

    float3 sampleSum = (float3)0;
    float3 visTargetSum = (float3)0;
    float visWeightSum = 0.0;
    const float invSamples = 1.0 / (float)samples;

    for (uint sy = 0; sy < samples; sy++)
    {
        for (uint sx = 0; sx < samples; sx++)
        {
            const float2 rnd = float2(((float)sx + 0.5) * invSamples, ((float)sy + 0.5) * invSamples);
            const LightSample ls = sampleLight(encoded, evalPos, rnd);
            if (!(ls.dw > 0.0))
            {
                continue;
            }

            const float3 fromLight = evalPos - ls.position;
            const float distToLight = length(fromLight);
            if (distToLight < 0.001 || (reach > 0.0 && distToLight >= reach))
            {
                continue;
            }

            const float shapedDw = safeSolidAngle(
                ls.dw * (distToLight * distToLight) * shaftDistanceAttenuation(distToLight, reach, falloff));
            if (!(shapedDw > 0.0))
            {
                continue;
            }

            const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), LOCAL_SHAFT_PHASE_G);
            const float3 contrib = ls.color * (shapedDw * powerScale * phase);
            const float lum = max(contrib.r, max(contrib.g, contrib.b));
            if (lum <= 1e-8)
            {
                continue;
            }

            sampleSum += contrib;
            visTargetSum += ls.position * lum;
            visWeightSum += lum;
        }
    }

    if (!(visWeightSum > 0.0))
    {
        return false;
    }

    outRadiance = sampleSum / (float)(samples * samples);
    const float3 dir = shaft.direction.xyz;
    const float3 normalOffset = dot(dir, dir) > 1e-6 ? normalize(dir) * 0.002 : (float3)0;
    outVisTarget = (visTargetSum / visWeightSum) + normalOffset;
    return max(outRadiance.r, max(outRadiance.g, outRadiance.b)) > 1e-7;
}

bool evaluateAnalyticShaftCandidate(const LightShaft_BT shaft, float3 evalPos, float3 toViewer,
                                   float reach, out float3 outRadiance, out float3 outVisTarget)
{
    outRadiance = (float3)0;
    const float3 emitter = shaft.positionRadius.xyz;
    const float3 direction = shaft.direction.xyz;
    const bool hasDir = dot(direction, direction) > 1e-6;
    outVisTarget = emitter + (hasDir ? normalize(direction) * 0.002 : (float3)0);

    const float3 fromLight = evalPos - emitter;
    const float distToLight = length(fromLight);
    if (distToLight <= 0.001 || (reach > 0.0 && distToLight >= reach))
    {
        return false;
    }

    float cone = 1.0;
    if (hasDir)
    {
        const float cosAngle = dot(fromLight / distToLight, normalize(direction));
        cone = smoothstep(shaft.cone.y, max(shaft.cone.x, shaft.cone.y + 1e-4), cosAngle);
    }
    if (!(cone > 0.0))
    {
        return false;
    }

    const float radius = max(shaft.positionRadius.w, 0.001);
    const float falloff = shaft.emission.z > 0.0 ? shaft.emission.z : 1.0;
    const float attenuation = (radius * radius) * shaftDistanceAttenuation(distToLight, reach, falloff);
    const float3 emitted = shaft.color.rgb * (cone * attenuation);
    if (max(emitted.r, max(emitted.g, emitted.b)) <= 1e-7)
    {
        return false;
    }

    const float phase = shaftPhase(dot(-fromLight / distToLight, toViewer), LOCAL_SHAFT_PHASE_G);
    outRadiance = emitted * phase;
    return true;
}

float3 accumulateShaft(const LightShaft_BT shaft, float3 segStart, float3 segEnd,
                       float evalT, float3 toViewer)
{
    float3 pa = segStart;
    float3 pb = segEnd;
    float segWeight = 1.0;

    const float3 dir = shaft.direction.xyz;
    const float dirLen2 = dot(dir, dir);
    if (dirLen2 > 1e-6 && shaft.cone.y >= -1e-4)
    {
        const float3 N = dir * rsqrt(dirLen2);
        const float minH = 0.01;
        const float h0 = dot(N, segStart - shaft.positionRadius.xyz);
        const float h1 = dot(N, segEnd - shaft.positionRadius.xyz);
        if (h0 < minH && h1 < minH)
        {
            return (float3)0;
        }

        float s0 = 0.0;
        float s1 = 1.0;
        if (h0 < minH || h1 < minH)
        {
            const float split = clamp((minH - h0) / (h1 - h0), 0.0, 1.0);
            if (h0 < minH)
            {
                s0 = split;
            }
            else
            {
                s1 = split;
            }
        }

        segWeight = saturate(s1 - s0);
        if (segWeight <= 1e-4)
        {
            return (float3)0;
        }

        pa = lerp(segStart, segEnd, s0);
        pb = lerp(segStart, segEnd, s1);
    }

    const float3 evalPos = lerp(pa, pb, evalT);

    const float reach = shaft.emission.y;
    const float boundsRadius = max(shaft.source.w, shaft.positionRadius.w);
    if (reach > 0.0 && length(evalPos - shaft.positionRadius.xyz) - boundsRadius > reach)
    {
        return (float3)0;
    }

    if (!shaftConeAllows(shaft, evalPos, boundsRadius))
    {
        return (float3)0;
    }

    const uint samples = max(1u, min(params.lightShaftSamples, 2u));
    float3 radiance = (float3)0;
    float3 visTarget = shaft.positionRadius.xyz;
    bool hasCandidate = false;

    if (shaft.emission.x <= 0.5 &&
        shaft.tangentU.w > 0.0 && shaft.tangentV.w > 0.0 &&
        dot(shaft.tangentU.xyz, shaft.tangentU.xyz) > 1e-6 &&
        dot(shaft.tangentV.xyz, shaft.tangentV.xyz) > 1e-6)
    {
        hasCandidate = evaluateRectAreaShaft(shaft, evalPos, toViewer, reach, samples,
                                             radiance, visTarget);
    }
    else if (shaft.emission.x > 0.5 && (shaft.surface.y > 0.5 || shaft.source.x > 0.5))
    {
        hasCandidate = evaluateProjectedAreaCandidate(shaft, evalPos, toViewer, reach,
                                                      radiance, visTarget);
    }
    else if (shaft.source.x > 0.5)
    {
        hasCandidate = evaluateSourceLightCandidate(shaft, evalPos, toViewer, reach, samples,
                                                    radiance, visTarget);
    }
    else
    {
        hasCandidate = evaluateAnalyticShaftCandidate(shaft, evalPos, toViewer, reach,
                                                      radiance, visTarget);
    }

    if (!hasCandidate || max(radiance.r, max(radiance.g, radiance.b)) <= 1e-7)
    {
        return (float3)0;
    }

    if (!lightShaftVisible(evalPos, visTarget))
    {
        return (float3)0;
    }

    return radiance * segWeight;
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

    if (params.debugMode == 99u)
    {
        rayCounter[0] = 0u;
    }

    const float3 jitter = volumeFroxelJitter(cell);
    float3 segStart;
    float3 segEnd;
    float3 center;
    volumeCellSegment(cell, jitter, segStart, segEnd, center);
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
    const uint listBase = clusterIndex * (1u + params.lightListCapacity);
    const uint loopCount = clusterCount > 0u
        ? min(lightList[listBase], params.lightListCapacity)
        : shaftCount;

    for (uint i = 0; i < loopCount; i++)
    {
        const uint shaftIndex = clusterCount > 0u ? lightList[listBase + 1u + i] : i;
        if (shaftIndex < shaftCount)
        {
            radiance += accumulateShaft(params.lightShafts[shaftIndex], segStart, segEnd,
                                        jitter.z, toViewer);
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

        visibility *= getCloudSunTransmittance(center, sunDirection, true);
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
        float edgeFade = 0.0;
        sampleVolumeHistory(cell, center, history, edgeFade);
        lightShaftVolume[cell] = history;
        return;
    }

    if (params.temporalEnabled != 0u && params.historyValid != 0u && params.mediumHistoryWeight > 0.0)
    {
        float4 history = (float4)0;
        float edgeFade = 0.0;
        if (sampleVolumeHistory(cell, center, history, edgeFade))
        {
            const float weight = saturate(params.mediumHistoryWeight) * edgeFade;
            current.rgb = lerp(current.rgb, max(history.rgb, (float3)0.0), weight);
            current.a = lerp(current.a, max(history.a, 0.0), weight);
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
