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
// GPU sampling helper for the transparency froxel irradiance volume. The layout and the SH math
// match Source/TransparencyFroxelVolume.h/.cpp and qray_transparency_froxel_tests. The buffer is
// bound at set 6 binding 20; the light-scattering fill pass is the next step.

#ifndef TRANSPARENCY_FROXELS_HLSLI_
#define TRANSPARENCY_FROXELS_HLSLI_

// The active tier's dimensions come from transparencyFroxelParams.yzw (32/16/32, 64/32/64 or
// 128/64/128); the buffer is allocated for the high tier.
uint3 q2TransparencyFroxelDim()
{
    return uint3(globalUniform.transparencyFroxelParams.y,
                 globalUniform.transparencyFroxelParams.z,
                 globalUniform.transparencyFroxelParams.w);
}

uint q2TransparencyFroxelCount()
{
    const uint3 dim = q2TransparencyFroxelDim();
    return dim.x * dim.y * dim.z;
}

struct ShTransparencyFroxel
{
    float sh[4][3];
};

#if defined(DESC_SET_LIGHT_SOURCES)
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_TRANSPARENCY_FROXELS, DESC_SET_LIGHT_SOURCES)]]
RWStructuredBuffer<ShTransparencyFroxel> transparencyFroxels;
#endif

void q2TransparencyFroxelAccumulate(const float3 direction, const float3 radiance,
                                    inout float sh[4][3])
{
    const float basis[4] =
    {
        0.282095,
        0.488603 * direction.x,
        0.488603 * direction.y,
        0.488603 * direction.z,
    };

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] += radiance[channel] * basis[band];
        }
    }
}

void q2TransparencyFroxelNormalize(inout float sh[4][3], const uint sampleCount)
{
    if (sampleCount == 0u)
    {
        for (uint band = 0u; band < 4u; band++)
        {
            for (uint channel = 0u; channel < 3u; channel++)
            {
                sh[band][channel] = 0.0;
            }
        }
        return;
    }

    const float scale = 4.0 * 3.14159265358979323846 / float(sampleCount);

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] *= scale;
        }
    }
}

// Trilinear froxel lookup followed by the Lambertian SH convolution, the shader twin of
// TransparencyFroxelVolume::SampleIrradiance.
bool q2SampleTransparencyFroxel(const float3 position, const float3 normal,
                                const float3 volumeOrigin, const float3 volumeSize,
                                out float3 outRgb)
{
    outRgb = (float3)0.0;

    const uint3 dim = q2TransparencyFroxelDim();
    const float3 cell = volumeSize / float3(dim);
    const float3 index = (position - volumeOrigin) / cell - float3(0.5, 0.5, 0.5);

    if (any(index < float3(-0.5, -0.5, -0.5)) ||
        any(index > float3(dim) - float3(0.5, 0.5, 0.5)))
    {
        return false;
    }

    int3 base;
    float3 frac;

    for (uint axis = 0u; axis < 3u; axis++)
    {
        const float value = floor(index[axis]);
        base[axis] = clamp(int(value), 0, int(dim[axis]) - 2);
        frac[axis] = clamp(index[axis] - float(base[axis]), 0.0, 1.0);
    }

    float sh[4][3];

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] = 0.0;
        }
    }

    for (uint dz = 0u; dz < 2u; dz++)
    {
        for (uint dy = 0u; dy < 2u; dy++)
        {
            for (uint dx = 0u; dx < 2u; dx++)
            {
                const float wx = dx ? frac.x : (1.0 - frac.x);
                const float wy = dy ? frac.y : (1.0 - frac.y);
                const float wz = dz ? frac.z : (1.0 - frac.z);
                const float w = wx * wy * wz;
                const uint probeIndex =
                    uint((base.z + int(dz)) * int(dim.y) + (base.y + int(dy))) * dim.x +
                    uint(base.x + int(dx));
                const ShTransparencyFroxel probe = transparencyFroxels[probeIndex];

                for (uint band = 0u; band < 4u; band++)
                {
                    for (uint channel = 0u; channel < 3u; channel++)
                    {
                        sh[band][channel] += probe.sh[band][channel] * w;
                    }
                }
            }
        }
    }

    const float n = normal.x * 0.488603;
    const float m = normal.y * 0.488603;
    const float l = normal.z * 0.488603;

    for (uint channel = 0u; channel < 3u; channel++)
    {
        outRgb[channel] = 0.886227 * sh[0][channel] +
                          1.023328 * (sh[1][channel] * n + sh[2][channel] * m + sh[3][channel] * l);
    }

    return true;
}

#endif
