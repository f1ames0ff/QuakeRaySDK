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

#define DESC_SET_TEXTURES       0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_PARTICLE_DRAW  2
#define DESC_SET_FRAMEBUFFERS   3
#define SMOKE_LIGHT_EXTERNAL_TLAS 1
#define DESC_SET_LIGHT_SOURCES  5
#define SMOKE_RAY_STATS_SET     6

[[vk::binding(0, 4)]] RaytracingAccelerationStructure topLevelAS;

#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "Smoke.hlsli"
#include "Light.hlsli"
#include "Q2ClusterLights.hlsli"
#include "SmokeLight.hlsli"
#include "GpuParticles.hlsli"

struct GpuParticleDrawPush_BT
{
    float4x4 viewProj;
    uint     mode;
    uint     maxParticles;
    uint2    renderSize;
};

[[vk::push_constant]] ConstantBuffer<GpuParticleDrawPush_BT> drawPush;
[[vk::binding(0, DESC_SET_PARTICLE_DRAW)]] StructuredBuffer<ShGpuParticle> particlePool;
[[vk::binding(1, DESC_SET_PARTICLE_DRAW)]] StructuredBuffer<uint> renderIndices;

static const float2 kQuadCorners[6] =
{
    float2(-1.0, -1.0),
    float2( 1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0, -1.0),
    float2( 1.0,  1.0),
    float2(-1.0,  1.0),
};

static const float3 kOctaVerts[6] =
{
    float3( 1.0,  0.0,  0.0),
    float3(-1.0,  0.0,  0.0),
    float3( 0.0,  1.0,  0.0),
    float3( 0.0, -1.0,  0.0),
    float3( 0.0,  0.0,  1.0),
    float3( 0.0,  0.0, -1.0),
};

static const uint3 kOctaFaces[8] =
{
    uint3(0u, 2u, 4u),
    uint3(2u, 1u, 4u),
    uint3(1u, 3u, 4u),
    uint3(3u, 0u, 4u),
    uint3(2u, 0u, 5u),
    uint3(1u, 2u, 5u),
    uint3(3u, 1u, 5u),
    uint3(0u, 3u, 5u),
};

float3 rotateAroundAxis(const float3 v, const float3 axis, float angle)
{
    const float c = cos(angle);
    const float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

float3 perturbOctaVertex(uint idx, uint seed)
{
    uint rng = seed ^ (idx * 747796405u + 13u);
    const float radial = lerp(0.55, 1.35, gpuRand01(rng));
    const float3 jitter = float3(
        gpuRand01(rng) - 0.5,
        gpuRand01(rng) - 0.5,
        gpuRand01(rng) - 0.5) * 0.35;
    return kOctaVerts[idx] * radial + jitter;
}

void main(
    uint vertexId   : SV_VertexID,
    uint instanceId : SV_InstanceID,

    [[vk::location(0)]] out float4 outColor       : COLOR0,
    [[vk::location(1)]] out float2 outUv          : TEXCOORD0,
    [[vk::location(2)]] out float3 outLit         : TEXCOORD1,
    [[vk::location(3)]] out float3 outNormal      : TEXCOORD2,
    [[vk::location(4)]] out float4 outMeta        : TEXCOORD3,
    out float4 outPosition : SV_Position)
{
    const uint bucket     = drawPush.mode;
    const uint renderMode = gpuParticleBucketToMode(bucket);
    const uint slot       = renderIndices[bucket * drawPush.maxParticles + instanceId];
    const ShGpuParticle p = particlePool[slot];

    const uint flags = p.meta.x >> 8u;
    const float lifeFrac = saturate(p.posAndAge.w / max(p.velAndLife.w, 1e-4));
    const float curSize = max(lerp(p.sizeAndStretch.x, p.sizeAndStretch.y, lifeFrac), 1e-4);
    float4 col = lerp(p.colorStart, p.colorEnd, lifeFrac);

    const float3 camRight   = getColumn(globalUniform.invView, 0).xyz;
    const float3 camUp      = getColumn(globalUniform.invView, 1).xyz;
    const float3 camForward = -getColumn(globalUniform.invView, 2).xyz;

    float3 worldPos = p.posAndAge.xyz;
    float3 worldNormal = -camForward;
    float2 uv = float2(0.5, 0.5);

    if (renderMode == GPU_PARTICLE_MODE_DEBRIS_3D)
    {
        const uint faceIdx = (vertexId / 3u) & 7u;
        const uint cornerIdx = vertexId % 3u;
        const uint3 faceVerts = kOctaFaces[faceIdx];

        uint rng = p.meta.w ^ 0x9E3779B9u;
        const float3 spinAxis = gpuSampleConeDirection(float3(0.0, 0.0, 1.0), M_PI, rng);
        const float halfScale = 0.5 * curSize;

        const float3 lv0 = rotateAroundAxis(perturbOctaVertex(faceVerts.x, p.meta.w), spinAxis, p.spinData.x) * halfScale;
        const float3 lv1 = rotateAroundAxis(perturbOctaVertex(faceVerts.y, p.meta.w), spinAxis, p.spinData.x) * halfScale;
        const float3 lv2 = rotateAroundAxis(perturbOctaVertex(faceVerts.z, p.meta.w), spinAxis, p.spinData.x) * halfScale;

        const float3 localPos = cornerIdx == 0u ? lv0 : (cornerIdx == 1u ? lv1 : lv2);
        worldPos += localPos;
        worldNormal = safeNormalize(cross(lv1 - lv0, lv2 - lv0));
        uv = cornerIdx == 0u ? float2(0.0, 0.0) : (cornerIdx == 1u ? float2(1.0, 0.0) : float2(0.5, 1.0));
    }
    else
    {
        const float2 corner = kQuadCorners[vertexId % 6u];
        uv = corner * 0.5 + 0.5;

        if (renderMode == GPU_PARTICLE_MODE_SPARK)
        {
            const float3 vel = p.velAndLife.xyz;
            const float3 viewVel = vel - camForward * dot(vel, camForward);
            const float viewVelLen = length(viewVel);
            const float3 tangent = viewVelLen > 1e-3 ? (viewVel / viewVelLen) : camRight;
            const float3 perp = safeNormalize(cross(camForward, tangent));

            const float distToCam = max(dot(worldPos - globalUniform.cameraPosition.xyz, camForward), 0.1);
            const float minPixelRadius = distToCam / max(float(drawPush.renderSize.y), 1.0);
            const float baseRadius = 0.5 * curSize;
            const float halfThick = max(baseRadius, minPixelRadius);
            const float coverageFix = saturate(baseRadius / halfThick);
            col.a *= coverageFix;

            const float halfLen = max(halfThick, baseRadius + 0.5 * length(vel) * max(p.sizeAndStretch.z, 0.0));
            worldPos += tangent * (corner.x * halfLen) + perp * (corner.y * halfThick);
        }
        else if (renderMode == GPU_PARTICLE_MODE_BILLBOARD)
        {
            const float c = cos(p.spinData.x);
            const float s = sin(p.spinData.x);
            const float2 rCorner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
            worldPos += (camRight * rCorner.x + camUp * rCorner.y) * (0.5 * curSize);
        }
        else
        {
            worldPos += (camRight * corner.x + camUp * corner.y) * (0.5 * curSize);
        }
    }

    const bool selfLit = (renderMode == GPU_PARTICLE_MODE_SPARK) ||
                         ((flags & GPU_PARTICLE_FLAG_SELF_LIT) != 0u);

    float3 lit = float3(0.0, 0.0, -1.0);
    if (!selfLit)
    {
        const uint cluster = p.meta.z;
        const float3 direct = (cluster != 0u && cluster != 0xFFFFFFFFu)
            ? smokeLightAt(worldPos, cluster, 0.0, SMOKE_LIGHT_STRENGTH, 0.0)
            : (float3)0.04;
        if (renderMode == GPU_PARTICLE_MODE_DEBRIS_3D)
        {
            const float3 keyDir = safeNormalize(float3(0.35, 0.25, 0.90));
            const float faceShade = lerp(0.28, 1.15, 0.5 + 0.5 * dot(worldNormal, keyDir));
            lit = direct * (0.45 * faceShade);
        }
        else
        {
            lit = direct * 0.45;
        }
    }

    outColor    = col;
    outUv       = uv;
    outLit      = lit;
    outNormal   = worldNormal;
    outMeta     = float4(float(bucket), float(p.meta.y), p.sizeAndStretch.w, float(p.meta.w & 0xFFFFu));
    outPosition = mul(drawPush.viewProj, float4(worldPos, 1.0));
}
