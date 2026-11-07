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
// GPU-side probe record, SH helpers and cascade lookup. The constants and the lookup math match
// Source/LightVolume.h/.cpp; the CPU oracle stays the reference for the tests. The GPU update pass
// currently fills the six cascades only (one per frame); the local-volume storage is present in
// the ABI but not written or sampled yet.

#ifndef LIGHT_VOLUME_GPU_HLSLI_
#define LIGHT_VOLUME_GPU_HLSLI_

#include "SphericalHarmonics.hlsli"

#define Q2_LIGHT_VOLUME_CASCADES 6u
#define Q2_LIGHT_VOLUME_DIM 16u
#define Q2_LIGHT_VOLUME_PROBES_PER_CASCADE (Q2_LIGHT_VOLUME_DIM * Q2_LIGHT_VOLUME_DIM * Q2_LIGHT_VOLUME_DIM)
#define Q2_LIGHT_VOLUME_BASE_BOUNDS 1280.0
#define Q2_LIGHT_VOLUME_SNAP 640.0
#define Q2_LIGHT_VOLUME_PI 3.14159265358979323846

struct ShLightVolumeProbe
{
    float sh[4][3];
};

#if defined(DESC_SET_LIGHT_SOURCES)
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_LIGHT_VOLUME_PROBES, DESC_SET_LIGHT_SOURCES)]]
RWStructuredBuffer<ShLightVolumeProbe> lightVolumeProbes;
#endif

float3 q2LightVolumeOrigin()
{
    return floor(globalUniform.cameraPosition.xyz / Q2_LIGHT_VOLUME_SNAP + 0.5) * Q2_LIGHT_VOLUME_SNAP;
}

float q2LightVolumeBounds(const uint cascade)
{
    return Q2_LIGHT_VOLUME_BASE_BOUNDS * exp2(float(cascade));
}

// Finest cascade containing the point, or -1 outside all of them.
int q2LightVolumeFindCascade(const float3 position)
{
    const float3 origin = q2LightVolumeOrigin();

    for (uint cascade = 0u; cascade < Q2_LIGHT_VOLUME_CASCADES; cascade++)
    {
        const float half = q2LightVolumeBounds(cascade) * 0.5;
        const float3 delta = abs(position - origin);

        if (delta.x <= half && delta.y <= half && delta.z <= half)
        {
            return int(cascade);
        }
    }

    return -1;
}

void q2LightVolumeAccumulate(const float3 direction, const float3 radiance, inout float sh[4][3])
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

void q2LightVolumeNormalize(inout float sh[4][3], const uint sampleCount)
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

    const float scale = 4.0 * Q2_LIGHT_VOLUME_PI / float(sampleCount);

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] *= scale;
        }
    }
}

// Samples the integrated radiance SH and converts it to the screen-space SH layout used by
// SphericalHarmonics.hlsli: (L0, Ly, Lz, Lx), radiance coefficients (the volume's integral is
// divided by 4*pi). The weight comes from lightVolumeParams.y.
bool q2SampleLightVolumeSH(const float3 position, const float weight, out SH outSh)
{
    outSh = newSH();

    if (!(globalUniform.lightVolumeParams.x > 0.0) || !(weight > 0.0))
    {
        return false;
    }

    const int cascade = q2LightVolumeFindCascade(position);

    if (cascade < 0)
    {
        return false;
    }

    const float bounds = q2LightVolumeBounds(uint(cascade));
    const float cell = bounds / float(Q2_LIGHT_VOLUME_DIM);
    const float3 origin = q2LightVolumeOrigin();
    const float3 index = (position - origin) / cell + float(Q2_LIGHT_VOLUME_DIM) * 0.5;

    int3 base;
    float3 frac;

    for (uint axis = 0u; axis < 3u; axis++)
    {
        const float baseValue = floor(index[axis] - 0.5);
        base[axis] = clamp(int(baseValue), 0, int(Q2_LIGHT_VOLUME_DIM) - 2);
        frac[axis] = clamp(index[axis] - 0.5 - float(base[axis]), 0.0, 1.0);
    }

    const uint baseProbe = uint(cascade) * Q2_LIGHT_VOLUME_PROBES_PER_CASCADE +
        uint((base.z * int(Q2_LIGHT_VOLUME_DIM) + base.y) * int(Q2_LIGHT_VOLUME_DIM) + base.x);

    float integrated[4][3];
    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            integrated[band][channel] = 0.0;
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
                const uint probeIndex = baseProbe + dx + Q2_LIGHT_VOLUME_DIM * dy +
                    Q2_LIGHT_VOLUME_DIM * Q2_LIGHT_VOLUME_DIM * dz;
                const ShLightVolumeProbe probe = lightVolumeProbes[probeIndex];

                for (uint band = 0u; band < 4u; band++)
                {
                    for (uint channel = 0u; channel < 3u; channel++)
                    {
                        integrated[band][channel] += probe.sh[band][channel] * w;
                    }
                }
            }
        }
    }

    const float scale = weight / (4.0 * Q2_LIGHT_VOLUME_PI);

    // The stored order is (L0, Lx, Ly, Lz); the screen SH order is (L0, Ly, Lz, Lx).
    outSh.r = float4(integrated[0][0], integrated[2][0], integrated[3][0], integrated[1][0]) * scale;
    outSh.g = float4(integrated[0][1], integrated[2][1], integrated[3][1], integrated[1][1]) * scale;
    outSh.b = float4(integrated[0][2], integrated[2][2], integrated[3][2], integrated[1][2]) * scale;
    return true;
}

#endif
