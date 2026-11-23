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

// Independent implementation of the standard front-to-back prefix integral;
// no reference renderer source is copied.

#define DESC_SET_LIGHT_SHAFT_VOLUME 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_LIGHT_SHAFT_PARAMS 2

#include "ShaderCommonHLSLFunc.hlsli"
#include "LightShafts.hlsli"
#include "Medium.hlsli"

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] ConstantBuffer<LightShaftVolumeParams_BT> params;

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME)]] Texture3D<float4> lightShaftInjection;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_VOLUME), vk::image_format("rgba16f")]] RWTexture3D<float4> lightShaftVolume;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 column = int2(dispatchThreadID.xy);
    if (column.x >= (int)params.volumeSizeX || column.y >= (int)params.volumeSizeY)
    {
        return;
    }

    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    float3 accumulated = (float3)0;
    float transmittance = 1.0;

    for (int z = 0; z < (int)params.volumeSizeZ; z++)
    {
        const int3 cell = int3(column, z);
        const float4 current = lightShaftInjection.Load(int4(cell, 0));

        if (params.debugMode != 0u && params.debugMode != 3u)
        {
            lightShaftVolume[cell] = current;
            continue;
        }

        const float t0 = (float)z / (float)params.volumeSizeZ;
        const float t1 = (float)(z + 1) / (float)params.volumeSizeZ;
        const float stepLength = (farPlane - nearPlane) * (t1 * t1 - t0 * t0);
        const float sigma = max(current.a, 0.0);
        const float stepTransmittance = mediumTransmittance(sigma, stepLength);
        const float integration = mediumIntegration(sigma, stepLength);

        accumulated += transmittance * current.rgb * integration;
        transmittance *= stepTransmittance;
        lightShaftVolume[cell] = float4(accumulated, transmittance);
    }
}
