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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "BRDF.hlsli"
#include "Subsurface.hlsli"

struct SssBlurDirection
{
    uint axis;
};

[[vk::push_constant]] ConstantBuffer<SssBlurDirection> sssBlurDirection;

static const float kSssOffsets[13] = {
    -1.0000, -0.7470, -0.5223, -0.3299, -0.1722, -0.0568,
     0.0000,
     0.0568,  0.1722,  0.3299,  0.5223,  0.7470,  1.0000
};

static const float kSssStepWidths[13] = {
    0.1265, 0.2389, 0.2085, 0.1750, 0.1365, 0.0861,
    0.0568,
    0.0861, 0.1365, 0.1750, 0.2085, 0.2389, 0.1265
};

float3 blurSubsurfaceAxis(
    const int2 pix,
    const int2 dims,
    const float4 centerSss,
    uint axis)
{
    const int2 centerCb = getCheckerboardPix(pix);
    const float3 centerSignal = (axis == 0u)
        ? framebufQ2SssDiffuse_Sampled.Load(int3(centerCb, 0)).rgb
        : framebufQ2SssTemp.Load(pix).rgb;

    const float3 sssD = getSubsurfaceMeanFreePath(centerSss);
    const float maxD = max(sssD.r, max(sssD.g, sssD.b));
    const float centerDepth = abs(framebufQ2ViewDepth_Sampled.Load(int3(centerCb, 0)).x);
    if (maxD <= 0.0 || centerDepth <= 0.0)
    {
        return centerSignal;
    }

    const float projScale = (axis == 0u)
        ? abs(globalUniform.projection[0].x) * (float)dims.x
        : abs(globalUniform.projection[1].y) * (float)dims.y;
    const float pixelsPerMeter = (0.5 * projScale) / max(centerDepth, 0.05);
    const float worldHalfSpan = 3.0 * maxD;
    const float maxRadiusPx = min(worldHalfSpan * pixelsPerMeter, 28.0);
    if (maxRadiusPx < 0.75 || pixelsPerMeter <= 1e-4)
    {
        return centerSignal;
    }

    const int2 stepDir = (axis == 0u) ? int2(1, 0) : int2(0, 1);
    const float invDepthTolerance = 1.0 / max(maxD * 2.0, 0.015);
    const float metersPerNormUnit = maxRadiusPx / pixelsPerMeter;

    float3 colorSum = (float3)0.0;
    float3 weightSum = (float3)0.0;

    [unroll]
    for (int i = 0; i < 13; i++)
    {
        const float offsetPx = kSssOffsets[i] * maxRadiusPx;
        const int2 samplePix = pix + stepDir * (int)round(offsetPx);
        if (samplePix.x < 0 || samplePix.y < 0 || samplePix.x >= dims.x || samplePix.y >= dims.y)
        {
            continue;
        }

        const int2 sampleCb = getCheckerboardPix(samplePix);
        const float tapMask = getSubsurfaceMask(
            framebufQ2Subsurface_Sampled.Load(int3(sampleCb, 0)));
        if (i != 6 && tapMask <= 0.0)
        {
            continue;
        }

        const float tapDepth = abs(framebufQ2ViewDepth_Sampled.Load(int3(sampleCb, 0)).x);
        const float depthNorm = (tapDepth - centerDepth) * invDepthTolerance;
        const float depthWeight = exp(-depthNorm * depthNorm);

        const float absWorldDist = abs(offsetPx) / pixelsPerMeter;
        const float deltaMeters = kSssStepWidths[i] * metersPerNormUnit;
        const float maskWeight = (i == 6) ? max(tapMask, 1e-3) : tapMask;
        const float3 weight = evalBurleyDiffusion1D(absWorldDist, sssD) * deltaMeters * depthWeight * maskWeight;

        const float3 tapColor = (axis == 0u)
            ? framebufQ2SssDiffuse_Sampled.Load(int3(sampleCb, 0)).rgb
            : framebufQ2SssTemp.Load(samplePix).rgb;

        colorSum += tapColor * weight;
        weightSum += weight;
    }

    return colorSum / max(weightSum, (float3)1e-5);
}

[numthreads(COMPUTE_COMPOSE_GROUP_SIZE_X, COMPUTE_COMPOSE_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.xy);
    const int2 dims = int2((int)globalUniform.renderWidth, (int)globalUniform.renderHeight);
    if (pix.x >= dims.x || pix.y >= dims.y)
    {
        return;
    }

    const uint axis = sssBlurDirection.axis;
    const bool debugDisableSss = (globalUniform.debugShowFlags &
        (DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE |
         DEBUG_SHOW_FLAG_ONLY_SPECULAR |
         DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE)) != 0u;
    if (debugDisableSss)
    {
        if (axis == 0u)
        {
            framebufQ2SssTemp[pix] = (float4)0.0;
        }
        return;
    }

    const int2 cbPix = getCheckerboardPix(pix);
    const float4 centerSss = framebufQ2Subsurface_Sampled.Load(int3(cbPix, 0));
    const float centerMask = getSubsurfaceMask(centerSss);
    if (centerMask <= 0.0)
    {
        if (axis == 0u)
        {
            framebufQ2SssTemp[pix] = (float4)0.0;
        }
        return;
    }

    const float3 blurred = blurSubsurfaceAxis(pix, dims, centerSss, axis);
    if (axis == 0u)
    {
        framebufQ2SssTemp[pix] = float4(blurred, 0.0);
        return;
    }

    const float3 albedo = framebufAlbedo_Sampled.Load(int3(pix, 0)).rgb;
    const float3 throughput = framebufThroughput_Sampled.Load(int3(cbPix, 0)).rgb;
    const float metallic = framebufMetallicRoughness_Sampled.Load(int3(cbPix, 0)).x;
    const float4 q2Transparent = framebufQ2Transparent_Sampled.Load(int3(cbPix, 0));
    const float4 fogAccum = framebufQ2FogAccum_Sampled.Load(int3(cbPix, 0));

    float3 ro_d = albedo * (1.0 - metallic);
    if (globalUniform.fixedAlbedo.x != 0.0)
    {
        ro_d = (float3)globalUniform.fixedAlbedo.x;
    }

    const float3 sssContribution =
        blurred *
        ro_d *
        getMaterialAmbient(albedo) *
        throughput *
        (1.0 - q2Transparent.a) *
        (1.0 - fogAccum.a) *
        centerMask;

    const float3 baseColor = framebufPreFinal.Load(pix).rgb;
    framebufPreFinal[pix] = float4(max(baseColor + sssContribution, (float3)0.0), 0.0);
}
