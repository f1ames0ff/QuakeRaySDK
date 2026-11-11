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
// Transparency froxel light-scattering fill. One thread owns one SH4 probe: eight fixed sphere
// directions, one grid light per direction (the strongest in the cell), and the light color with a
// smooth radius falloff. The result is the SH4 integral read by q2SampleTransparencyFroxel.
// Visibility is not traced in this first slice; the froxels are a diffuse scattering
// approximation, not a shadowed light field.

#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_LIGHT_SOURCES 6

#include "ShaderCommonHLSL.hlsli"
#include "ShaderCommonHLSLFunc.hlsli"
#include "Q2LightGrid.hlsli"
#include "TransparencyFroxels.hlsli"

#define Q2_FROXEL_BOX_ORIGIN q2TransparencyFroxelBoxOrigin()
#define Q2_FROXEL_BOX_SIZE q2TransparencyFroxelBoxSize()
#define Q2_FROXEL_DIRECTIONS 8u

float3 q2FroxelDirection(const uint index)
{
    // Fibonacci sphere: eight directions spread over the sphere without a pole cluster.
    const float y = 1.0 - 2.0 * (float(index) + 0.5) / float(Q2_FROXEL_DIRECTIONS);
    const float radius = sqrt(max(1.0 - y * y, 0.0));
    const float phi = float(index) * 2.39996323;
    return float3(cos(phi) * radius, y, sin(phi) * radius);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint probeIndex = dispatchThreadId.x;

    if (probeIndex >= q2TransparencyFroxelCount())
    {
        return;
    }

    const uint3 dimU = q2TransparencyFroxelDim();
    const uint x = probeIndex % dimU.x;
    const uint y = (probeIndex / dimU.x) % dimU.y;
    const uint z = probeIndex / (dimU.x * dimU.y);
    const float3 dim = float3(dimU);
    const float3 cell = Q2_FROXEL_BOX_SIZE / dim;
    const float3 position = Q2_FROXEL_BOX_ORIGIN + (float3(x, y, z) + 0.5) * cell;

    float sh[4][3];

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] = 0.0;
        }
    }

    for (uint directionIndex = 0u; directionIndex < Q2_FROXEL_DIRECTIONS; directionIndex++)
    {
        const float3 direction = q2FroxelDirection(directionIndex);
        const float3 samplePosition = position + direction * 64.0;
        const int gridCell = q2GetLightGridCell(samplePosition);
        const uint count = min(q2GetLightGridCount(gridCell), 4u);

        uint bestLight = uint(LIGHT_INDEX_NONE);
        float bestLuminance = 0.0;

        for (uint i = 0u; i < count; i++)
        {
            const uint lightIndex = q2GetLightGridLight(gridCell, i);

            if (lightIndex < uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) ||
                lightIndex >= uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) + globalUniform.lightCount)
            {
                continue;
            }

            const float luminance = getLuminance(lightSources[lightIndex].color);

            if (luminance > bestLuminance)
            {
                bestLuminance = luminance;
                bestLight = lightIndex;
            }
        }

        if (bestLight == uint(LIGHT_INDEX_NONE))
        {
            continue;
        }

        const ShLightEncoded light = lightSources[bestLight];
        const float3 center = light.data_0.xyz;
        const float radius = max(light.data_0.w, 0.025);
        const float3 delta = center - position;
        const float falloff = (radius * radius) /
            (dot(delta, delta) + radius * radius + 6.25e-4);

        q2TransparencyFroxelAccumulate(direction, light.color * falloff, sh);
    }

    q2TransparencyFroxelNormalize(sh, Q2_FROXEL_DIRECTIONS);

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            transparencyFroxels[probeIndex].sh[band][channel] = sh[band][channel];
        }
    }
}
