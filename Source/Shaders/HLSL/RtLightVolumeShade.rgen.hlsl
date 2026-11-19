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
// Cache-shading raygen of the split volume update. One invocation owns one probe; it reads the
// packed visibility records the volume raygen wrote this frame, reconstructs the hit, runs the
// sun/local-light NEE and the world-radiance-cache lookup, and writes the probe SH4. The camera
// path and the glow/emissive work stay in the visibility raygen; this pass only shades.

#define MATERIAL_MAX_ALBEDO_LAYERS 0
#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_RAY_STATS 11
#define DESC_SET_CLOUD_SHADOW 12
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_INDIR)

#include "RaygenCommon.hlsli"
#include "Q2Asvgf.hlsli"
#include "Q2LightLists.hlsli"
#include "LightVolumeGpu.hlsli"
#include "RadianceCache.hlsli"
#include "LightVolumeShade.hlsli"

#define Q2_LIGHT_VOLUME_MIN_RAYS 16u
#define Q2_LIGHT_VOLUME_MAX_RAYS 64u
#define Q2_RNG_VOLUME_BASE 800u
#define Q2_RNG_VOLUME_STRIDE 8u

[shader("raygeneration")]
void main()
{
    if (globalUniform.radianceSplitParams.x == 0u)
    {
        return;
    }

    const uint invocation = DispatchRaysIndex().x;
    const uint localVolumeCount = q2LightVolumeLocalCount();

    uint probeIndex;
    uint probeLocalIndex;
    uint dim;
    float bounds;
    float3 origin;
    uint seedSalt;

    if (invocation < Q2_LIGHT_VOLUME_PROBES_PER_CASCADE)
    {
        const uint cascade = globalUniform.frameId % Q2_LIGHT_VOLUME_CASCADES;
        dim = Q2_LIGHT_VOLUME_DIM;
        bounds = q2LightVolumeBounds(cascade);
        origin = q2LightVolumeOrigin();
        probeLocalIndex = invocation;
        probeIndex = cascade * Q2_LIGHT_VOLUME_PROBES_PER_CASCADE + invocation;
        seedSalt = cascade;
    }
    else
    {
        if (localVolumeCount == 0u)
        {
            return;
        }

        const uint localProbe = invocation - Q2_LIGHT_VOLUME_PROBES_PER_CASCADE;

        if (localProbe >= Q2_LIGHT_VOLUME_PROBES_PER_LOCAL)
        {
            return;
        }

        const uint slot = globalUniform.frameId % localVolumeCount;
        const float4 volume = globalUniform.lightVolumeLocalParams[slot];

        if (!(volume.w > 0.0))
        {
            return;
        }

        dim = Q2_LIGHT_VOLUME_LOCAL_DIM;
        bounds = volume.w;
        origin = volume.xyz;
        probeLocalIndex = localProbe;
        probeIndex = Q2_LIGHT_VOLUME_CASCADE_PROBE_COUNT +
            slot * Q2_LIGHT_VOLUME_PROBES_PER_LOCAL + localProbe;
        seedSalt = Q2_LIGHT_VOLUME_CASCADES + slot;
    }

    const float cell = bounds / float(dim);
    const uint x = probeLocalIndex % dim;
    const uint y = (probeLocalIndex / dim) % dim;
    const uint z = probeLocalIndex / (dim * dim);
    const float3 probePosition = origin - bounds * 0.5 +
        (float3(float(x), float(y), float(z)) + 0.5) * cell;

    const uint seed = getRandomSeed(int2(int(probeLocalIndex), int(seedSalt)), globalUniform.frameId);

    float sh[4][3];

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] = 0.0;
        }
    }

    const uint rayCount = clamp(uint(globalUniform.lightVolumeParams.z + 0.5),
                                Q2_LIGHT_VOLUME_MIN_RAYS, Q2_LIGHT_VOLUME_MAX_RAYS);

    for (uint ray = 0u; ray < rayCount; ray++)
    {
        const uint salt = Q2_RNG_VOLUME_BASE + ray * Q2_RNG_VOLUME_STRIDE;
        const float2 u = rnd16_2(seed, salt);
        const float3 direction = sampleSphere(u.x, u.y);
        const float3 rayOrigin = probePosition + direction * (0.01 * globalUniform.sceneUnitsPerHostUnit);
        const ShLightVolumeHit record = lightVolumeHits[probeIndex * Q2_LIGHT_VOLUME_MAX_RAYS + ray];

        float3 radiance;

        if (record.valid == 0u)
        {
            radiance = volumeSkyRadiance();
        }
        else
        {
            const Surface hit = hitInfoToSurface_Indirect(
                getHitInfoBounce(record.payload, rayOrigin, 1.0, 0.0), direction);
            radiance = volumeHitCachedRadiance(hit, seed, salt + 1u);
        }

        q2LightVolumeAccumulate(direction, radiance, sh);
    }

    q2LightVolumeNormalize(sh, rayCount);

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            lightVolumeProbes[probeIndex].sh[band][channel] = sh[band][channel];
        }
    }
}
