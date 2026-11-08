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
// One-cascade-per-frame irradiance-volume update. Each invocation owns one probe of cascade
// `frameId % 6`; it traces Q2_LIGHT_VOLUME_RAYS directions, shades the hit with the sun NEE and
// emission, projects the result into SH4 and stores the integrated coefficients in the probe
// buffer. Sky misses contribute the uniform sky fallback colour (no cubemap dependency, so this
// pass needs no sets 7/8).
//
// Local lights on the hit are not sampled yet: the first GPU slice is the sun + sky + emission
// field, which is enough to exercise the atlas, the schedule and the final-gather path.

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

#define Q2_LIGHT_VOLUME_RAYS 16u
#define Q2_RNG_VOLUME_BASE 800u
#define Q2_RNG_VOLUME_STRIDE 8u

Surface traceVolumeBounce(const float3 origin, const float3 direction)
{
    const ShPayload payload = traceIndirectRay(0u, origin, direction);
    rayStatsAdd(RAY_STATS_CATEGORY_INDIRECT, 1);

    if (!doesPayloadContainHitInfo(payload))
    {
        Surface surface;
        surface.isSky = true;
        surface.geometryInstanceFlags = 0u;
        return surface;
    }

    return hitInfoToSurface_Indirect(
        getHitInfoBounce(payload, origin, 1.0, 0.0), direction);
}

float3 volumeSkyRadiance()
{
    return globalUniform.skyColorDefault.xyz * max(globalUniform.skyColorMultiplier, 0.0);
}

float3 volumeHitLocalLights(const Surface hit, const uint seed, const uint salt)
{
    const int gridCell = q2GetLightGridCell(hit.position);
    const float3 rng = float3(
        rnd24(seed, salt),
        rnd24(seed, salt + 1u),
        rnd24(seed, salt + 2u));

    const float alpha = square(hit.roughness);
    const float phongExp = q2RoughnessSquareToSpecPower(alpha);
    const float phongScale = min(100.0, 1.0 / (M_PI * max(alpha * alpha, 1e-4)));
    const float phongWeight = clamp(
        getLuminance(hit.specularColor) /
            (getLuminance(hit.specularColor) + getLuminance(hit.albedo)),
        0.0, 0.9);

    uint lightIndex = LIGHT_INDEX_NONE;
    uint lightSlot = 0u;
    float lightPdf = 0.0;
    bool sampledFromGrid = false;

    if (gridCell >= 0)
    {
        q2SampleGridLights(gridCell, hit.position, hit.normal, hit.toViewerDir,
                           phongExp, phongScale, phongWeight, rng,
                           lightIndex, lightSlot, lightPdf);
        sampledFromGrid = lightIndex != LIGHT_INDEX_NONE;
    }

    if (!sampledFromGrid)
    {
        q2SampleClusterLights(hit.cluster, hit.position, hit.normal, hit.toViewerDir,
                              phongExp, phongScale, phongWeight, false, rng,
                              lightIndex, lightSlot, lightPdf);
    }

    if (lightIndex == LIGHT_INDEX_NONE || lightPdf <= 0.0)
    {
        return (float3)0.0;
    }

    const float2 pointRnd = rnd16_2(seed, salt + 3u);
    const float2 memberRnd = float2(rnd24(seed, salt + 4u), rnd24(seed, salt + 5u));
    float memberPdf = 1.0;
    LightSample light = sampleLightNee(lightSources[lightIndex], hit.position, pointRnd, memberRnd, memberPdf);

    if (getLuminance(light.color) <= 0.0)
    {
        return (float3)0.0;
    }

    bool lightTraced = false;
    const float3 visibility = traceLightVisibility(hit, light, lightIndex, lightTraced);

    if (lightTraced)
    {
        rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1);
    }

    float3 diffuse;
    shadeDiffuse(hit, light, 1.0 / max(lightPdf * memberPdf, 1e-9), diffuse);
    return diffuse * visibility;
}

float3 volumeHitRadiance(const Surface hit, const uint seed, const uint salt)
{
    float3 direct = (float3)0.0;

    if (globalUniform.directionalLightExists != 0)
    {
        const DirectionalLight sun =
            decodeAsDirectionalLight(lightSources[LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET]);
        const float2 sunRnd = rnd16_2(seed, salt) * 0.99;
        const LightSample sunLight = sampleDirectionalLight(sun, hit.position, sunRnd);

        if (getLuminance(sunLight.color) > 0.0)
        {
            bool traced = false;
            const float3 visibility = traceSunVisibility(hit, sunLight, traced);

            if (traced)
            {
                rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1);
            }

            float3 diffuse;
            shadeDiffuse(hit, sunLight, 1.0, diffuse);
            direct += diffuse * visibility;
        }
    }

    direct += volumeHitLocalLights(hit, seed, salt + 2u);

    float3 radiance = direct * hit.albedo;

    if ((hit.geometryInstanceFlags & GEOM_INST_FLAG_EMISSION_RAY_ONLY) != 0)
    {
        radiance += hit.emission * hit.albedo;
    }

    return radiance;
}

[shader("raygeneration")]
void main()
{
    const uint localIndex = DispatchRaysIndex().x;

    if (localIndex >= Q2_LIGHT_VOLUME_PROBES_PER_CASCADE)
    {
        return;
    }

    const uint cascade = globalUniform.frameId % Q2_LIGHT_VOLUME_CASCADES;
    const float bounds = q2LightVolumeBounds(cascade);
    const float cell = bounds / float(Q2_LIGHT_VOLUME_DIM);
    const float3 origin = q2LightVolumeOrigin();

    const uint x = localIndex % Q2_LIGHT_VOLUME_DIM;
    const uint y = (localIndex / Q2_LIGHT_VOLUME_DIM) % Q2_LIGHT_VOLUME_DIM;
    const uint z = localIndex / (Q2_LIGHT_VOLUME_DIM * Q2_LIGHT_VOLUME_DIM);
    const float3 probePosition = origin - bounds * 0.5 +
        (float3(float(x), float(y), float(z)) + 0.5) * cell;

    const uint seed = getRandomSeed(int2(int(localIndex), int(cascade)), globalUniform.frameId);

    float sh[4][3];

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            sh[band][channel] = 0.0;
        }
    }

    for (uint ray = 0u; ray < Q2_LIGHT_VOLUME_RAYS; ray++)
    {
        const uint salt = Q2_RNG_VOLUME_BASE + ray * Q2_RNG_VOLUME_STRIDE;
        const float2 u = rnd16_2(seed, salt);
        const float3 direction = sampleSphere(u.x, u.y);
        const float3 rayOrigin = probePosition + direction * (0.01 * globalUniform.sceneUnitsPerHostUnit);

        const Surface hit = traceVolumeBounce(rayOrigin, direction);
        const float3 radiance = hit.isSky
            ? volumeSkyRadiance()
            : volumeHitRadiance(hit, seed, salt + 1u);

        q2LightVolumeAccumulate(direction, radiance, sh);
    }

    q2LightVolumeNormalize(sh, Q2_LIGHT_VOLUME_RAYS);

    const uint probeIndex = cascade * Q2_LIGHT_VOLUME_PROBES_PER_CASCADE + localIndex;

    for (uint band = 0u; band < 4u; band++)
    {
        for (uint channel = 0u; channel < 3u; channel++)
        {
            lightVolumeProbes[probeIndex].sh[band][channel] = sh[band][channel];
        }
    }
}
