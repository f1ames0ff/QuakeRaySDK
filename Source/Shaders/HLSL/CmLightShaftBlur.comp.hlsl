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

#define DESC_SET_LIGHT_SHAFT_BLUR 0
#define DESC_SET_LIGHT_SHAFT_PARAMS 1
#define DESC_SET_FRAMEBUFFERS 2
#define DESC_SET_GLOBAL_UNIFORM 3

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"
#include "LightShaftBlur.hlsli"

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_BLUR)]] Texture2D<float4> lightShaftScatterIn;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_BLUR), vk::image_format("rgba16f")]] RWTexture2D<float4> lightShaftScatterOut;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<LightShaftParams_BT> lightShaftParams;

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pixelPos = int2(dispatchThreadID.xy);
    const int2 renderSize = int2(globalUniform.renderWidth, globalUniform.renderHeight);

    if (any(pixelPos >= renderSize))
    {
        return;
    }

    if (lightShaftParams[0].lightShaftCount == 0u)
    {
        lightShaftScatterOut[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    if (lightShaftParams[0].accumDebugMode != 0u)
    {
        lightShaftScatterOut[pixelPos] = lightShaftScatterIn.Load(int3(pixelPos, 0));
        return;
    }

    const float froxelPxX = clamp(
        (float)renderSize.x / max((float)lightShaftParams[0].accumVolumeSizeX, 1.0), 1.0, 12.0);
    lightShaftScatterOut[pixelPos] = filterLightShaft1D(
        lightShaftScatterIn, framebufQ2ViewDepth_Sampled, pixelPos, renderSize,
        int2(1, 0), froxelPxX);
}
