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

struct CmSkyCloudsParams_BT
{
    float4 faceBasis[18];
    float4 sunDirection;
    float4 skyColor;
    float4 skyParams;
    float4 cloudColor;
    float4 cloudParams;
    float4 sunDiscColor;
    float4 sunTint;
    float4 moonDirection;
    float4 moonParams;
    float4 atmosphereParams;
    float4 starParams;
    float4 cloudLayer;
    float4 cloudAnchor;
    float4 cloudMarch;
    float4 cloudShadowPlacement;
};

[[vk::binding(3, 0), vk::image_format("rgba16f")]] RWTexture2DArray<float4> cloudCubemapOut;
[[vk::binding(5, 0)]] Texture3D<float> cloudShadowMap;
[[vk::binding(6, 0)]] SamplerState cloudShadowMap_Sampler;
[[vk::binding(1, 0)]] ConstantBuffer<CmSkyCloudsParams_BT> params;

#include "CloudLayer.hlsli"
#include "CloudShadowMap.hlsli"
#include "AtmosphereCommon.hlsli"

static const float CLOUD_POWDER = 8.0;
static const float CLOUD_STEP_GROWTH = 1.045;
static const float CLOUD_MAX_MARCH_SPAN = 12.0;

float henyeyGreenstein(float mu, float g)
{
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * mu, 1.0e-4);
    return (1.0 - g2) / (d * sqrt(d));
}

float cloudPhase(float mu, float g)
{
    return lerp(henyeyGreenstein(mu, g), henyeyGreenstein(mu, -g * 0.35), 0.5);
}

float cloudSilverLiningPhase(float mu, float g)
{
    return 0.10 * henyeyGreenstein(mu, min(g + 0.05, 0.80)) +
           0.55 * henyeyGreenstein(mu, g * 0.55) +
           0.35 * henyeyGreenstein(mu, -g * 0.35);
}

float cloudMultiScatter(float sunDepth, float3 phase)
{
    float sum = 0.0;
    float energy = 0.5;

    for (int order = 1; order <= 3; order++)
    {
        float o = (float)order;
        sum += energy * phase[order - 1] * exp(-sunDepth / (1.0 + o));
        energy *= 0.5;
    }

    return sum;
}

float cloudSunTauFromMap(CloudLayer layer, float3 p, float3 sunDir, out float blend)
{
    const float4 placement = params.cloudShadowPlacement;
    const float3 worldPos = float3(p.xy, params.cloudAnchor.z + p.z);
    const float height = clamp((p.z - layer.altitude) / layer.thickness, 0.0, 1.0);

    return cloudShadowTau(cloudShadowMap, cloudShadowMap_Sampler, worldPos, sunDir, placement, height, blend);
}

float cloudCoarseTau(CloudLayer layer, float3 p, float3 lightDir)
{
    if (lightDir.z <= -0.04)
    {
        return 0.0;
    }

    const int STEPS = 4;
    float maxDist = lightDir.z > 0.04
        ? min((layer.altitude + layer.thickness - p.z) / lightDir.z, layer.thickness * 3.0)
        : layer.thickness * 3.0;
    float dt = max(maxDist, 0.0) / (float)STEPS;
    float col = 0.0;

    for (int i = 0; i < STEPS; i++)
    {
        float3 q = p + lightDir * (((float)i + 0.5) * dt);
        if (q.z <= layer.altitude || q.z >= layer.altitude + layer.thickness)
        {
            continue;
        }
        col += cloudDensity(layer, q, false) * dt;
    }

    return cloudOpticalDepth(layer, col);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int3 ipos = (int3)dispatchThreadID;

    uint sizeX, sizeY, elements;
    cloudCubemapOut.GetDimensions(sizeX, sizeY, elements);
    const int2 size = int2(sizeX, sizeY);

    const int face = ipos.z + (int)(params.cloudMarch.y + 0.5);
    if (ipos.x >= size.x || ipos.y >= size.y || face >= 6)
    {
        return;
    }

    float3 right   = params.faceBasis[face * 3 + 0].xyz;
    float3 up      = params.faceBasis[face * 3 + 1].xyz;
    float3 forward = params.faceBasis[face * 3 + 2].xyz;

    float2 ndc = ((float2)ipos.xy + 0.5) / (float2)size * 2.0 - 1.0;
    float3 dir = normalize(ndc.x * right + (-ndc.y) * up + forward);

    float opacity  = clamp(params.skyParams.y, 0.0, 1.0);
    bool  aaaMode  = params.skyColor.w > 0.5;
    float sunLight = params.sunDirection.w;

    CloudLayer layer;
    layer.altitude  = max(params.cloudLayer.x, 0.025);
    layer.thickness = max(params.cloudLayer.y, 0.025);
    layer.coverage  = clamp(params.cloudParams.x, 0.0, 0.99);
    layer.density   = max(params.cloudParams.y, 0.0);
    layer.detail    = clamp(params.cloudMarch.z, 0.0, 0.9);
    layer.speed     = params.cloudParams.z;
    layer.time      = params.cloudColor.w;
    cloudPrepareLayer(layer);

    int viewSteps = clamp((int)(params.cloudMarch.x + 0.5), 8, 256);
    float phaseG  = clamp(params.cloudMarch.w, 0.0, 0.95);

    uint volumeX, volumeY, volumeZ;
    cloudShadowMap.GetDimensions(volumeX, volumeY, volumeZ);
    int sunSlices = (int)volumeZ;

    float3 albedo = max(params.cloudColor.xyz, float3(0.0, 0.0, 0.0));
    float3 sunDir = normalize(params.sunDirection.xyz);
    float3 moonDir = length(params.moonDirection.xyz) > 1.0e-5
        ? normalize(params.moonDirection.xyz)
        : -sunDir;

    AtmosphereConfig atmo = makeAtmosphereConfig(params.skyColor.rgb, params.atmosphereParams.xyz);
    float moonPhaseScale = params.moonParams.w >= 0.0
        ? lerp(0.18, 1.0, clamp(params.moonParams.w, 0.0, 1.0))
        : lerp(0.18, 1.0, clamp(0.5 + 0.5 * dot(-sunDir, moonDir), 0.0, 1.0));
    float3 moonBase = max(params.moonParams.xyz, (float3)0.0) * moonPhaseScale;
    bool moonActive = aaaMode && params.moonDirection.w > 0.0 &&
                      max(moonBase.r, max(moonBase.g, moonBase.b)) > 1.0e-5;
    bool sunActive = sunLight > 0.5 &&
                     (!aaaMode || atmoHorizonMask(layer.altitude + layer.thickness, sunDir) > 0.0);

    float3 zenithAmbient = (float3)0.0;
    float3 horizonAmbient = (float3)0.0;
    if (aaaMode)
    {
        float3 sunTintRaw = max(params.sunTint.xyz, (float3)0.0) * max(params.sunTint.w, 0.0);
        evalCloudSkyAmbient(sunDir, sunActive ? 1.0 : 0.0, sunTintRaw,
                            moonDir, moonActive ? 1.0 : 0.0, moonBase,
                            atmo, zenithAmbient, horizonAmbient);
        zenithAmbient *= max(params.cloudLayer.w, 0.0);
        horizonAmbient *= max(params.cloudLayer.w, 0.0);
    }

    float3 legacySunColor = params.sunDiscColor.xyz * max(params.cloudLayer.z, 0.0) * (sunLight > 0.5 ? sunLight : 0.0);
    float3 legacySkyLight = max(params.skyColor.xyz, float3(0.0, 0.0, 0.0)) * max(params.cloudLayer.w, 0.0);
    float3 aaaSunBase = max(params.sunTint.xyz, (float3)0.0) * max(params.sunTint.w, 0.0) * max(params.cloudLayer.z, 0.0) * 2.8;

    float3 inscatter = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    bool lit = aaaMode
        ? (sunActive || moonActive || atmo.scatterGain > 0.0)
        : (legacySunColor.r + legacySunColor.g + legacySunColor.b > 0.0 ||
           legacySkyLight.r + legacySkyLight.g + legacySkyLight.b > 0.0);

    if (dir.z > 0.0 && lit && opacity > 0.0 && params.cloudParams.w > 0.5 &&
        layer.coverage < 1.0 && layer.density > 0.0)
    {
        float tIn = layer.altitude / dir.z;
        float tOut = min((layer.altitude + layer.thickness) / dir.z,
                         tIn + layer.thickness * CLOUD_MAX_MARCH_SPAN);

        float span = (pow(CLOUD_STEP_GROWTH, (float)viewSteps) - 1.0) / (CLOUD_STEP_GROWTH - 1.0);
        float dt = (tOut - tIn) / span;

        float jitter = cloudMarchSeed(ipos.xy, face);
        float t = tIn;
        float step = dt;

        float muSun = dot(dir, sunDir);
        float sunPhase = aaaMode ? cloudSilverLiningPhase(muSun, phaseG) : cloudPhase(muSun, phaseG);
        float muMoon = dot(dir, moonDir);
        float moonPhaseFn = cloudPhase(muMoon, phaseG * 0.8);

        float3 multiScatterPhase;
        for (int order = 1; order <= 3; order++)
        {
            float gOrder = phaseG * (1.0 - 0.3 * (float)order);
            multiScatterPhase[order - 1] =
                lerp(henyeyGreenstein(muSun, gOrder), henyeyGreenstein(muSun, -gOrder), 0.5);
        }

        float3 moonMultiPhase;
        for (int order = 1; order <= 3; order++)
        {
            float gOrder = phaseG * (1.0 - 0.3 * (float)order);
            moonMultiPhase[order - 1] =
                lerp(henyeyGreenstein(muMoon, gOrder), henyeyGreenstein(muMoon, -gOrder), 0.5);
        }

        for (int i = 0; i < viewSteps; i++)
        {
            float offset = frac(jitter + (float)i * CLOUD_SEQUENCE_STEP);
            float ts = t + step * offset;

            float3 p = float3(dir.x * ts + params.cloudAnchor.x,
                              dir.y * ts + params.cloudAnchor.y,
                              dir.z * ts);
            float density = cloudDensity(layer, p, true);

            if (density > 1.0e-3)
            {
                float sigma = cloudOpticalDepth(layer, density);
                float h = (p.z - layer.altitude) / layer.thickness;
                float solid = density / max(cloudHeightProfile(h), 1.0e-3);
                float skyShield = exp(-solid * layer.density * CLOUD_EXTINCTION * (1.0 - h));
                float powder = 1.0 - exp(-density * CLOUD_POWDER);

                float3 light = (float3)0.0;

                if (sunActive)
                {
                    float blend = 0.0;
                    float mapTau = (params.cloudShadowPlacement.x > 0.5 && sunDir.z > 0.01)
                        ? cloudSunTauFromMap(layer, p, sunDir, blend)
                        : -1.0;
                    float marchTau = -1.0;
                    if (blend < 1.0)
                    {
                        marchTau = cloudSunDepth(layer, p, sunDir, CLOUD_SHADOW_STEPS, sunSlices);
                    }
                    float sunTau = (marchTau >= 0.0 && blend > 0.0 && mapTau >= 0.0)
                        ? lerp(marchTau, mapTau, blend)
                        : (blend >= 1.0 && mapTau >= 0.0 ? mapTau : marchTau);
                    float sunVisibility = exp(-sunTau);

                    float rim = lerp(1.0, powder, clamp(-muSun, 0.0, 1.0));
                    float3 sunColorHere = aaaMode
                        ? aaaSunBase * evalSunTransmittanceAtAltitude(p.z, sunDir, atmo)
                        : legacySunColor;

                    light += sunColorHere * (sunVisibility * sunPhase * rim +
                                             cloudMultiScatter(sunTau, multiScatterPhase));
                }

                if (aaaMode && moonActive)
                {
                    float moonTau = cloudCoarseTau(layer, p, moonDir);
                    float moonVis = exp(-moonTau);
                    float moonRim = lerp(1.0, powder, clamp(-muMoon, 0.0, 1.0));
                    float3 moonColorHere = moonBase * evalSunTransmittanceAtAltitude(p.z, moonDir, atmo) * 0.55;
                    light += moonColorHere * (moonVis * moonPhaseFn * moonRim +
                                              cloudMultiScatter(moonTau, moonMultiPhase));
                }

                if (aaaMode)
                {
                    float3 skyFill = lerp(horizonAmbient * 0.45 + zenithAmbient * 0.55,
                                          zenithAmbient, smoothstep(0.15, 0.85, h)) * 1.15;
                    light += skyFill * skyShield;
                }
                else
                {
                    light += legacySkyLight * skyShield;
                }

                float stepTransmittance = exp(-sigma * step);
                inscatter += transmittance * light * (1.0 - stepTransmittance);
                transmittance *= stepTransmittance;

                if (transmittance < 0.01)
                {
                    break;
                }
            }

            t += step;
            step *= CLOUD_STEP_GROWTH;
        }
    }

    cloudCubemapOut[int3(ipos.xy, face)] = float4(inscatter * albedo * opacity, lerp(1.0, transmittance, opacity));
}
