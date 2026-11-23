// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/god_rays_filter.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
// which is distributed under the terms of the GNU General Public License
// version 2.  It has been adapted to the renderer interface of this project.
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
// Bilateral spatial upscale of the half-resolution god rays to full screen
// resolution. Ported 1:1 from Quake 2 RTX (god_rays_filter.comp), GPL v2.
// Q2RTX additionally blends the result onto the color buffer in this shader;
// in qray the compositing happens in CmPrepareHdr. The filtered volumetric
// sunlight is written to RGB and the light-shaft extinction (1 - transmittance)
// to alpha so a zero-cleared buffer decodes as transmittance 1.0.


// HLSL counterpart of CmGodRaysFilter.comp.
//
// Spellings that had to change:
//   * layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) -> [numthreads(16, 16, 1)]
//   * gl_GlobalInvocationID.xy -> SV_DispatchThreadID.xy, guarded by the same comparison
//   * ivec2(gl_GlobalInvocationID.xy) -> int2(dispatchThreadID.xy)
//   * any(greaterThanEqual(a, b)) / any(lessThan(a, b)) -> any(a >= b) / any(a < b): HLSL
//     compares vectors with the operators and hands back a bool vector for any()
//   * ivec2(0) -> (int2)0: the scalar does not broadcast through a constructor in HLSL, the cast
//     replicates it into both components instead
//   * ivec2(x, y) -> int2(x, y) with the same component lists, and the ivec2(globalUniform.
//     renderWidth, globalUniform.renderHeight) of the golden becomes the same int2(...) of the
//     same two floats, as renderWidth and renderHeight are floats on both sides
//   * vec4(0.0) -> (float4)0.0, vec4(result.rgb, 0.0) -> float4(result.rgb, 0.0)
//   * vec2(highResPos - iPosition) -> (float2)(highResPos - iPosition): the conversion of the
//     int2 difference, spelled as a cast. The parentheses are needed because the cast binds
//     tighter than the subtraction
//   * texelFetch(t, p, 0) -> t.Load(int3(p, 0)), imageStore(img, p, v) -> img[p] = v
//   * iPosition >> 1 keeps its spelling: HLSL shifts an int2 by a scalar component-wise
//
// What did not change: the early out against the render size, the two nested 5x5 loops of the
// bilateral filter with their bounds, the checkerboard remap of the high resolution position
// (lowResPos * 2 + int2(lowResPos.y & 1, 1 - (lowResPos.x & 1))), the abs() of both view depths
// (the reflection pass stores a negative view depth), the depth-aware weight
// (clamp(1.0 - 10.0 * abs(viewDepth - referenceViewDepth) / max(abs(referenceViewDepth), 1e-6),
// 0.0, 1.0)) with its max() guard, the spatial weight through the length of the high-res offset,
// the accumulation into result and fallbackResult, the weightSum branch (division by weightSum
// against the fixed / 16.0 of the fallback), and the final store of result.rgb with a zero alpha.

#define DESC_SET_LIGHT_SHAFT_PARAMS 1
#define DESC_SET_FRAMEBUFFERS 2
#define DESC_SET_GLOBAL_UNIFORM 3
#define DESC_SET_LIGHT_SHAFT_SCATTER 4
#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"
#include "LightShaftBlur.hlsli"

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<LightShaftParams_BT> lightShaftParams;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_SCATTER)]] Texture2D<float4> lightShaftScatter;

[numthreads(16, 16, 1)]
void main( uint3 dispatchThreadID : SV_DispatchThreadID )
{
    const int2 iPosition = int2(dispatchThreadID.xy);
    if (any(iPosition >= int2(globalUniform.renderWidth, globalUniform.renderHeight)))
        return;

    float4 result = (float4)0.0;
    float4 fallbackResult = (float4)0.0;
    float weightSum = 0.0;

    // Q2RTX uses TEX_PT_VIEW_DEPTH_A (positive primary view depth). The
    // reflection pass stores a negative view depth there, so take abs().
    const float referenceViewDepth = abs(framebufQ2ViewDepth_Sampled.Load(int3(getCheckerboardPix(iPosition), 0)).r);
    const int2 lowResOrigin = iPosition >> 1;

    const int2 lowResSize = int2((globalUniform.renderWidth + 1) / 2, (globalUniform.renderHeight + 1) / 2);

    for (int dy = -2; dy <= 2; dy++)
    {
        for (int dx = -2; dx <= 2; dx++)
        {
            const int2 lowResPos = lowResOrigin + int2(dx, dy);
            if (any(lowResPos < (int2)0) || any(lowResPos >= lowResSize))
                continue;

            const int2 highResPos = lowResPos * 2 + int2(lowResPos.y & 1, 1 - (lowResPos.x & 1));
            if (any(highResPos >= int2(globalUniform.renderWidth, globalUniform.renderHeight)))
                continue;

            const float4 color = framebufGodRays_Sampled.Load(int3(lowResPos, 0));
            const float viewDepth = abs(framebufQ2ViewDepth_Sampled.Load(int3(getCheckerboardPix(highResPos), 0)).r);

            // depth-aware (bilateral) and spatial weights, exactly as Q2RTX
            float weight = clamp(1.0 - 10.0 * abs(viewDepth - referenceViewDepth) / max(abs(referenceViewDepth), 1e-6), 0.0, 1.0);
            weight *= clamp(5.0 - length((float2)(highResPos - iPosition)), 0.0, 1.0);

            result.rgba += color.rgba * weight;
            fallbackResult.rgba += color.rgba;
            weightSum += weight;
        }
    }

    if (weightSum > 0.0)
    {
        // some relevant low-res pixels found - bilateral average
        result.rgba = result.rgba / weightSum;
    }
    else
    {
        // none found - plain (non-bilateral) spatial blur, as Q2RTX
        result.rgba = fallbackResult.rgba / 16.0;
    }

    float3 lightShafts = (float3)0.0;
    float shaftTransmittance = 1.0;

    if (lightShaftParams[0].lightShaftCount > 0u)
    {
        float4 filteredShaft;
        if (lightShaftParams[0].accumDebugMode != 0u)
        {
            filteredShaft = lightShaftScatter.Load(int3(iPosition, 0));
        }
        else
        {
            const int2 renderSize = int2(globalUniform.renderWidth, globalUniform.renderHeight);
            const float froxelPxY = clamp(
                (float)renderSize.y / max((float)lightShaftParams[0].accumVolumeSizeY, 1.0), 1.0, 12.0);
            filteredShaft = filterLightShaft1D(
                lightShaftScatter, framebufQ2ViewDepth_Sampled, iPosition, renderSize,
                int2(0, 1), froxelPxY);
        }

        lightShafts = filteredShaft.rgb;
        shaftTransmittance = saturate(filteredShaft.a);
    }

    const float shaftExtinction = saturate(1.0 - shaftTransmittance);
    framebufGodRaysFiltered[iPosition] = float4(result.rgb + lightShafts, shaftExtinction);
}
