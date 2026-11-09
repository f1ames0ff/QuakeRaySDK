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
// Shared probe-hit shading for the volume raygen and the split cache-shading raygen. The
// includer supplies RaygenCommon, Q2Asvgf, Q2LightLists, LightVolumeGpu and RadianceCache.

#ifndef LIGHT_VOLUME_SHADE_HLSLI_
#define LIGHT_VOLUME_SHADE_HLSLI_

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
            bool sunTraced = false;
            const float3 sunVis = traceSunVisibility(hit, sunLight, sunTraced);

            if (sunTraced)
            {
                rayStatsAdd(RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1);
            }

            float3 diffuse;
            shadeDiffuse(hit, sunLight, 1.0, diffuse);
            direct += diffuse * sunVis;
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

// World radiance cache: a hit reuses the shaded radiance of a previously seen cell. A miss shades
// and inserts; collisions overwrite. The cache is gated by radianceCacheParams.x (host cvar
// rt_radiance_cache), so the first slice stays opt-in.
float3 volumeHitCachedRadiance(const Surface hit, const uint seed, const uint salt)
{
    float3 cached;

    if (q2RadianceCacheGet(hit.position, cached))
    {
        return cached;
    }

    const float3 radiance = volumeHitRadiance(hit, seed, salt);
    q2RadianceCachePut(hit.position, radiance);
    return radiance;
}

#endif
