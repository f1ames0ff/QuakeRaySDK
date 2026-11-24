// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/physical_sky.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
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

struct Params_BT
{
    float4 faceBasis[18];       // 6 faces * (right, up, forward)
    float4 sunDirection;        // xyz = normalized direction TOWARD the sun, w = sun visible/active
    float4 skyColor;            // rgb = atmosphere tint (rt_sky_color), w > 0.5 enables AAA atmosphere/night
    float4 skyParams;           // x = multiplier, y = cloud opacity, z = sun disc intensity, w = sun disc radius (rad)
    float4 cloudColor;          // rgb = cloud albedo, w = cloud time (s)
    float4 cloudParams;         // x = coverage, y = density, z = wind, w = clouds enabled
    float4 sunDiscColor;        // rgb = sun disc color, w unused
    float4 sunTint;             // rgb = sun spectrum tint, w = sun light strength
    float4 moonDirection;       // xyz = direction TOWARD the moon, w = moon angular radius (rad)
    float4 moonParams;          // rgb = moon tint * intensity, w = moon phase (0..1, <0 auto)
    float4 atmosphereParams;    // x = density, y = mie, z = ozone, w = celestial rotation (rad)
    float4 starParams;          // x = stars enabled, y = stars density, z = stars intensity, w = clock (s)
    float4 cloudLayer;          // x = altitude (m), y = thickness (m), z = sun scale, w = sky scale
    float4 cloudAnchor;         // xyz = viewer position (m), w = cloud detail factor
};

[[vk::binding(1, 0)]] ConstantBuffer<Params_BT> params;

[[vk::binding(0, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> cubemapOut;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> envCubemapOut;
[[vk::binding(3, 0)]] TextureCube<float4> cloudCubemap;
[[vk::binding(4, 0)]] SamplerState cloudCubemap_Sampler;

#include "CloudLayer.hlsli"
#include "AtmosphereCommon.hlsli"

static const float CLOUD_READ_SPREAD = 1.0;
static const float SUN_DISC_CLOUD_HIDE = 4.0;

float3 hash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float3 rotateAroundAxis(float3 v, float3 axis, float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

float3 starTempColor(float t)
{
    float3 hot  = float3(0.66, 0.79, 1.00); // O/B blue-white
    float3 mid  = float3(0.95, 0.96, 1.00); // A/F white
    float3 warm = float3(1.00, 0.89, 0.71); // G/K pale gold
    float3 cool = float3(1.00, 0.67, 0.45); // M orange giant

    if (t < 0.35)
    {
        return lerp(hot, mid, t / 0.35);
    }
    if (t < 0.72)
    {
        return lerp(mid, warm, (t - 0.35) / 0.37);
    }
    return lerp(warm, cool, (t - 0.72) / 0.28);
}

float3 evalStarTier(float3 rotDir, float gridScale, float baseOccupancy, float minSizeRad,
                    float haloRad, float baseGain, float magPower, float densityScale,
                    float twinkleClock, float horizonScintStrength, float texelRad)
{
    float3 scaled = rotDir * gridScale;
    int3 baseCell = (int3)floor(scaled);
    float occThreshold = clamp(baseOccupancy * densityScale, 0.0, 0.92);
    if (occThreshold <= 0.0001)
    {
        return (float3)0.0;
    }

    float invGrid = 1.0 / gridScale;
    float shellMargin = 1.85 * invGrid;
    float3 accum = (float3)0.0;

    for (int dz = -1; dz <= 1; dz++)
    {
        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int3 cell = baseCell + int3(dx, dy, dz);
                float3 cellMid = ((float3)cell + 0.5) * invGrid;
                if (abs(length(cellMid) - 1.0) > shellMargin)
                {
                    continue;
                }

                float3 seedInput = (float3)cell + float3(gridScale, gridScale * 0.37, gridScale * 1.13);
                float3 h0 = hash33(seedInput);
                if (h0.x > occThreshold)
                {
                    continue;
                }

                float3 starOffset = hash33(seedInput + 19.19);
                float3 starPos = ((float3)cell + 0.12 + 0.76 * starOffset) * invGrid;
                float3 starDir = normalize(starPos);

                float dist = length(rotDir - starDir);
                float magRand = h0.y;
                float sizeRad = max(texelRad * 0.9, minSizeRad * lerp(0.75, 1.55, magRand));
                float aa = max(texelRad * 0.55, sizeRad * 0.45);

                if (dist > max(haloRad, sizeRad + aa))
                {
                    continue;
                }

                float core = 1.0 - smoothstep(max(sizeRad - aa, 0.0), sizeRad + aa, dist);
                float halo = haloRad > 0.0
                    ? exp(-dist * dist / max(haloRad * haloRad * 0.14, 1.0e-8)) * 0.22 * magRand
                    : 0.0;

                float mag = pow(magRand, magPower);
                float scint = 1.0 + horizonScintStrength * 0.18 *
                    sin(twinkleClock * (2.1 + 4.3 * h0.z) + h0.x * 91.7);

                accum += starTempColor(h0.z) * (baseGain * (0.12 + mag) * (core + halo) * max(scint, 0.4));
            }
        }
    }

    return accum;
}

float3 evalStarfield(float3 dir, float skyLuma, float3 starParams, float celestialRot, float twinkleClock)
{
    if (starParams.x <= 0.5 || starParams.y <= 0.0 || starParams.z <= 0.0 || dir.z <= -0.02 || skyLuma > 0.28)
    {
        return (float3)0.0;
    }

    float daylightFade = exp(-skyLuma * 24.0);
    float horizonFade = smoothstep(-0.015, 0.16, dir.z);
    float vis = daylightFade * horizonFade * starParams.z;
    if (vis <= 0.002)
    {
        return (float3)0.0;
    }

    float3 axis = normalize(float3(0.32, -0.58, 0.75));
    float3 rotDir = rotateAroundAxis(dir, axis, -celestialRot);
    float horizonScint = saturate(1.15 - dir.z * 1.15);

    uint sizeX, sizeY, sizeZ;
    cubemapOut.GetDimensions(sizeX, sizeY, sizeZ);
    float texelRad = 1.5707963 / (float)max(sizeX, sizeY);

    float densityScale = clamp(starParams.y, 0.0, 4.0);

    // Tier 1: Rare bright 1st/2nd magnitude stars with optical halo
    float3 stars = evalStarTier(rotDir, 28.0, 0.06, 0.0021, 0.0085, 2.2, 1.8,
                                densityScale, twinkleClock, horizonScint, texelRad);
    // Tier 2: Medium constellation stars
    stars += evalStarTier(rotDir, 64.0, 0.09, 0.0014, 0.0032, 0.85, 2.8,
                          densityScale, twinkleClock, horizonScint, texelRad);
    // Tier 3: Fine background star dust
    stars += evalStarTier(rotDir, 130.0, 0.13, 0.0011, 0.0, 0.35, 3.8,
                          densityScale, twinkleClock, horizonScint, texelRad);

    return stars * vis;
}

void evalMoon(float3 dir, float3 moonDir, float moonRadius, float3 moonColor,
              float moonPhase, float3 sunDir, AtmosphereConfig atmo,
              out float3 moonRadiance, out float moonDiscMask)
{
    moonRadiance = (float3)0.0;
    moonDiscMask = 0.0;

    if (moonRadius <= 0.0 || moonDir.z <= -0.10 || max(moonColor.r, max(moonColor.g, moonColor.b)) <= 1.0e-5)
    {
        return;
    }

    float cosBound = cos(min(moonRadius + 0.05, 1.2));
    float mu = dot(dir, moonDir);
    if (mu < cosBound)
    {
        return;
    }

    float3 horizonTrans = evalSunTransmittanceAtAltitude(10.0, moonDir, atmo);
    float moonAbove = smoothstep(-0.025, 0.015, dir.z);

    uint sizeX, sizeY, sizeZ;
    cubemapOut.GetDimensions(sizeX, sizeY, sizeZ);
    float edgeW = max(1.25 / (float)max(sizeX, sizeY), moonRadius * 0.03);

    float sinAng = length(cross(dir, moonDir));
    float angDist = atan2(sinAng, max(mu, 1.0e-5));

    float aura = exp(-angDist * angDist / max(moonRadius * moonRadius * 1.8, 1.0e-6)) * 0.09;
    moonRadiance += moonColor * horizonTrans * aura * moonAbove;

    if (angDist > moonRadius + edgeW)
    {
        return;
    }

    moonDiscMask = (1.0 - smoothstep(max(moonRadius - edgeW, 0.0), moonRadius + edgeW, angDist)) * moonAbove;
    if (moonDiscMask <= 0.0)
    {
        return;
    }

    float3 helper = abs(moonDir.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 rightM = normalize(cross(helper, moonDir));
    float3 upM    = cross(moonDir, rightM);

    float dx = dot(dir, rightM) / max(moonRadius, 1.0e-5);
    float dy = dot(dir, upM) / max(moonRadius, 1.0e-5);
    float r2 = dx * dx + dy * dy;
    float dz = sqrt(max(0.0, 1.0 - min(r2, 1.0)));

    float3 localN = normalize(float3(dx, dy, dz));
    float3 worldN = normalize(localN.x * rightM + localN.y * upM - localN.z * moonDir);

    float3 lightLocal;
    if (moonPhase < 0.0)
    {
        lightLocal = normalize(float3(dot(sunDir, rightM), dot(sunDir, upM), dot(sunDir, -moonDir)));
    }
    else
    {
        float phaseAngle = (1.0 - clamp(moonPhase, 0.0, 1.0)) * ATMO_PI;
        lightLocal = normalize(float3(sin(phaseAngle), 0.12 * sin(phaseAngle), cos(phaseAngle)));
    }

    float nMacro = cloudNoise(worldN * 3.4 + float3(2.1, -1.3, 0.7), 4);
    float nMicro = cloudNoise(worldN * 14.0 + float3(-4.0, 3.2, 5.1), 2);
    float maria = smoothstep(0.42, 0.62, nMacro);
    float surfaceAlbedo = lerp(0.50, 1.02, maria) * lerp(0.86, 1.06, nMicro);

    float ndl = max(dot(localN, lightLocal), 0.0);
    float ndv = max(localN.z, 0.06);
    float lommel = (ndl / max(ndl + ndv * 0.55, 1.0e-4)) * smoothstep(0.0, 0.06, ndl);
    float litSurface = (0.022 + 2.35 * lommel) * surfaceAlbedo;

    moonRadiance += moonColor * horizonTrans * litSurface * moonDiscMask;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int3 ipos = int3(dispatchThreadID);

    uint sizeX, sizeY, sizeZ;
    cubemapOut.GetDimensions(sizeX, sizeY, sizeZ);
    int2 size = int2(sizeX, sizeY);

    if (ipos.x >= size.x || ipos.y >= size.y || ipos.z >= 6)
    {
        return;
    }

    int face = ipos.z;
    float3 right   = params.faceBasis[face * 3 + 0].xyz;
    float3 up      = params.faceBasis[face * 3 + 1].xyz;
    float3 forward = params.faceBasis[face * 3 + 2].xyz;

    float2 ndc = (float2(ipos.xy) + (float2)0.5) / (float2)size * 2.0 - 1.0;
    float3 dir = normalize(ndc.x * right + (-ndc.y) * up + forward);

    float3 sunDir       = normalize(params.sunDirection.xyz);
    float3 skyColor     = max(params.skyColor.xyz, (float3)0.0);
    bool  aaaMode       = params.skyColor.w > 0.5;
    float sunAmount     = params.sunDirection.w;
    float sunAngRad     = max(params.skyParams.w, 0.0);
    float multiplier    = params.skyParams.x;
    float cloudOpacity  = clamp(params.skyParams.y, 0.0, 1.0);
    float sunIntensity  = params.skyParams.z;

    bool cloudsOn = params.cloudParams.w > 0.5 && cloudOpacity > 0.0;

    float4 cloud = float4(0.0, 0.0, 0.0, 1.0);
    if (cloudsOn)
    {
        uint layerWidth, layerHeight, layerMipLevels;
        cloudCubemap.GetDimensions(0, layerWidth, layerHeight, layerMipLevels);
        float2 texel = 1.0 / float2(layerWidth, layerHeight);
        float2 spread = texel * CLOUD_READ_SPREAD;
        float4 centre = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir, 0.0);
        float4 edges = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - up * spread.y, 0.0) +
                       cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + up * spread.y, 0.0);
        float4 corners = cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x - up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir - right * spread.x + up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x - up * spread.y, 0.0) +
                         cloudCubemap.SampleLevel(cloudCubemap_Sampler, dir + right * spread.x + up * spread.y, 0.0);
        cloud = (centre * 4.0 + edges * 2.0 + corners) / 16.0;
    }

    float edgeWidth = min(sunAngRad * 0.1, 1.0 / float(max(size.x, size.y)));
    float innerRadius = 2.0 * sin(max(sunAngRad - edgeWidth, 0.0) * 0.5);
    float outerRadius = 2.0 * sin((sunAngRad + edgeWidth) * 0.5);
    float3 discOffset = dir - sunDir;
    float disc = sunAngRad > 0.0
        ? (1.0 - smoothstep(innerRadius * innerRadius, outerRadius * outerRadius,
                           dot(discOffset, discOffset))) * sunAmount
        : 0.0;
    float cosAng = cos(sunAngRad + 4.0 / float(max(size.x, size.y)));

    float3 moonDir = length(params.moonDirection.xyz) > 1.0e-5
        ? normalize(params.moonDirection.xyz)
        : -sunDir;
    float moonAngRad = max(params.moonDirection.w, 0.0);

    float cloudTransmittance = cloud.a;
    float discTransmittance = 1.0;

    if (cloudsOn)
    {
        if (aaaMode && params.cloudLayer.x > 0.0 && params.cloudLayer.y > 0.0)
        {
            float nearSunCos = cos(min(max(sunAngRad * 3.0, 0.06), 0.35));
            float nearMoonCos = cos(min(max(moonAngRad * 2.5, 0.05), 0.35));
            bool nearSun = sunAmount > 0.0 && sunAngRad > 0.0 && dot(dir, sunDir) > nearSunCos;
            bool nearMoon = moonAngRad > 0.0 && dot(dir, moonDir) > nearMoonCos;

            if (dir.z > 0.0 && (nearSun || nearMoon))
            {
                CloudLayer layer;
                layer.altitude  = max(params.cloudLayer.x, 0.025);
                layer.thickness = max(params.cloudLayer.y, 0.025);
                layer.coverage  = clamp(params.cloudParams.x, 0.0, 0.99);
                layer.density   = max(params.cloudParams.y, 0.0);
                layer.detail    = clamp(params.cloudAnchor.w, 0.0, 0.9);
                layer.speed     = params.cloudParams.z;
                layer.time      = params.cloudColor.w;
                cloudPrepareLayer(layer);

                float exactT = cloudLineOfSightTransmittance(layer, params.cloudAnchor.xyz, dir);
                discTransmittance = lerp(1.0, exactT, cloudOpacity);
            }
            else
            {
                discTransmittance = cloud.a;
            }
        }
        else
        {
            float layerTransmittance = cloudOpacity > 0.0
                ? clamp(1.0 - (1.0 - cloud.a) / cloudOpacity, 0.0, 1.0)
                : 1.0;
            discTransmittance = lerp(1.0, pow(layerTransmittance, SUN_DISC_CLOUD_HIDE), cloudOpacity);
        }
    }

    float cloudShare = 1.0;
    if (sunAmount > 0.0 && sunIntensity > 0.0 && sunAngRad > 0.0)
    {
        float motionCosAng = cos(sunAngRad + 8.0 / float(max(size.x, size.y)));
        cloudShare = 1.0 - smoothstep(motionCosAng, cosAng, dot(dir, sunDir));
    }

    float3 sunTint = params.sunDiscColor.xyz;
    const float sunPeak = max(max(sunTint.r, sunTint.g), sunTint.b);
    const float sunWhite = smoothstep(1.0, 4.0, sunPeak);
    sunTint = lerp(sunTint, (float3)sunPeak, sunWhite * 0.9);

    float3 cloudRadiance = max(cloud.rgb, (float3)0.0);

    if (!aaaMode)
    {
        cloudRadiance = cloudRadiance / (1.0 + cloudRadiance);
        float3 visible = skyColor * cloud.a + sunTint * sunIntensity * disc * discTransmittance + cloudRadiance;
        cubemapOut[ipos] = float4(visible * multiplier, cloudShare);

        float3 env = skyColor * cloud.a + cloud.rgb;
        envCubemapOut[ipos] = float4(env * multiplier, 1.0);
        return;
    }

    AtmosphereConfig atmo = makeAtmosphereConfig(skyColor, params.atmosphereParams.xyz);
    float3 directSunTint = max(params.sunTint.xyz, (float3)0.0) * max(params.sunTint.w, 0.0);

    float moonPhaseScale = params.moonParams.w >= 0.0
        ? lerp(0.18, 1.0, clamp(params.moonParams.w, 0.0, 1.0))
        : lerp(0.18, 1.0, clamp(0.5 + 0.5 * dot(-sunDir, moonDir), 0.0, 1.0));
    float3 moonAtmoColor = max(params.moonParams.xyz, (float3)0.0) * moonPhaseScale;
    float moonEnabled = moonAngRad > 0.0 && max(moonAtmoColor.r, max(moonAtmoColor.g, moonAtmoColor.b)) > 1.0e-5
        ? 1.0
        : 0.0;

    float3 viewTrans;
    float3 skyAtmo = evalAtmosphereSky(dir, sunDir, sunAmount, directSunTint,
                                       moonDir, moonEnabled, moonAtmoColor,
                                       atmo, viewTrans);

    float sunHorizon = smoothstep(-0.022, 0.006, dir.z);
    float3 sunTrans = evalSunTransmittanceAtAltitude(10.0, sunDir, atmo);
    float3 sunDisc = sunTint * sunIntensity * disc * sunHorizon * sunTrans * discTransmittance;

    float3 moonRadiance;
    float moonMask;
    evalMoon(dir, moonDir, moonAngRad, max(params.moonParams.xyz, (float3)0.0),
             params.moonParams.w, sunDir, atmo, moonRadiance, moonMask);
    moonRadiance *= lerp(1.0, discTransmittance, moonMask * 0.75 + 0.25);

    if (moonEnabled > 0.5 && moonAngRad > 0.0)
    {
        float cosMoonAng = cos(moonAngRad + 4.0 / float(max(size.x, size.y)));
        float motionCosMoon = cos(moonAngRad + 8.0 / float(max(size.x, size.y)));
        float moonShare = 1.0 - smoothstep(motionCosMoon, cosMoonAng, dot(dir, moonDir));
        cloudShare = min(cloudShare, moonShare);
    }

    float skyLuma = dot(skyAtmo, float3(0.2126, 0.7152, 0.0722));
    float3 stars = evalStarfield(dir, skyLuma, params.starParams.xyz, params.atmosphereParams.w, params.starParams.w);
    stars *= viewTrans * (1.0 - moonMask) * cloudTransmittance;

    float3 visible = skyAtmo * cloudTransmittance + stars + sunDisc + moonRadiance + cloudRadiance;
    cubemapOut[ipos] = float4(max(visible, (float3)0.0) * multiplier, cloudShare);

    // Keep sharp stars, sun disc and moon disc out of the blurred IBL cubemap to avoid GI fireflies.
    float3 env = skyAtmo * cloudTransmittance + cloudRadiance;
    envCubemapOut[ipos] = float4(max(env, (float3)0.0) * multiplier, 1.0);
}
