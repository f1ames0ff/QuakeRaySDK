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

#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

static const float ATMO_PI = 3.14159265359;
static const float ATMO_PLANET_RADIUS = 6360000.0;
static const float ATMO_TOP_RADIUS = 6440000.0;
static const float ATMO_H_RAYLEIGH = 8000.0;
static const float ATMO_H_MIE = 1200.0;
static const float ATMO_OZONE_CENTER = 25000.0;
static const float ATMO_OZONE_HALF_WIDTH = 15000.0;

struct AtmosphereConfig
{
    float3 rayleighScatter;
    float  mieScatter;
    float3 rayleighExt;
    float  mieExt;
    float3 ozoneExt;
    float  scatterGain;
    float3 tint;
    float  density;
};

float2 atmoRaySphere(float3 ro, float3 rd, float radius)
{
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0)
    {
        return float2(1.0, -1.0);
    }
    float s = sqrt(max(disc, 0.0));
    return float2(-b - s, -b + s);
}

AtmosphereConfig makeAtmosphereConfig(float3 linearSkyColor, float3 atmosphereParams)
{
    AtmosphereConfig cfg;

    float3 skyCol = max(linearSkyColor, (float3)0.0);
    float skyPeak = max(max(skyCol.r, skyCol.g), skyCol.b);
    float3 skyNorm = skyPeak > 1.0e-5 ? (skyCol / skyPeak) : float3(0.0, 0.663, 1.0);

    float density = clamp(atmosphereParams.x, 0.0, 4.0);
    float mie     = clamp(atmosphereParams.y, 0.0, 4.0);
    float ozone   = clamp(atmosphereParams.z, 0.0, 4.0);

    float3 customScatter = max(lerp(float3(0.14, 0.14, 0.14), skyNorm, 0.88), float3(0.015, 0.015, 0.015)) * 28.0e-6;
    float3 earthExt = float3(5.8e-6, 12.2e-6, 30.0e-6);
    float3 extSpectrum = lerp(earthExt, max(skyNorm, float3(0.08, 0.08, 0.08)) * 26.0e-6, 0.22);

    cfg.rayleighScatter = customScatter * density;
    cfg.rayleighExt     = extSpectrum * density;
    cfg.mieScatter      = 4.2e-6 * mie * density;
    cfg.mieExt          = 4.7e-6 * mie * density;
    cfg.ozoneExt        = float3(0.65e-6, 1.88e-6, 0.085e-6) * ozone * density;
    cfg.scatterGain     = saturate(skyPeak) * (density > 0.0 ? 1.0 : 0.0);
    cfg.tint            = skyNorm;
    cfg.density         = density;
    return cfg;
}

float3 atmoOpticalDepth(float3 ro, float3 rd, float rayLen, int steps)
{
    if (rayLen <= 0.0 || steps <= 0)
    {
        return (float3)0.0;
    }

    float stepLen = rayLen / (float)steps;
    float3 od = (float3)0.0;

    for (int i = 0; i < steps; i++)
    {
        float3 pos = ro + rd * (((float)i + 0.5) * stepLen);
        float h = max(length(pos) - ATMO_PLANET_RADIUS, 0.0);
        float dR = exp(-h / ATMO_H_RAYLEIGH);
        float dM = exp(-h / ATMO_H_MIE);
        float dO = max(0.0, 1.0 - abs(h - ATMO_OZONE_CENTER) / ATMO_OZONE_HALF_WIDTH);
        od += float3(dR, dM, dO) * stepLen;
    }

    return od;
}

float atmoHorizonMaskCos(float altitudeMeters, float cosZenith)
{
    float h = max(altitudeMeters, 10.0);
    float r = ATMO_PLANET_RADIUS + h;
    float sinDip = -sqrt(max(h * (2.0 * ATMO_PLANET_RADIUS + h), 0.0)) / r;
    return smoothstep(sinDip - 0.024, sinDip + 0.015, cosZenith);
}

float atmoHorizonMask(float altitudeMeters, float3 lightDir)
{
    return atmoHorizonMaskCos(altitudeMeters, lightDir.z);
}

float3 evalLightTransmittance(float altitudeMeters, float cosZenith, AtmosphereConfig cfg)
{
    float horizon = atmoHorizonMaskCos(altitudeMeters, cosZenith);
    if (horizon <= 0.0)
    {
        return (float3)0.0;
    }

    if (cfg.density <= 1.0e-5)
    {
        return (float3)horizon;
    }

    float h = max(altitudeMeters, 10.0);
    float r = ATMO_PLANET_RADIUS + h;

    // Clamp grazing transmittance rays slightly above tangent so dawn/dusk light shifts
    // into golden-peach instead of over-attenuated blood red while horizon fades gently.
    float safeZ = max(cosZenith, 0.022);
    float safeH = sqrt(max(0.0, 1.0 - safeZ * safeZ));
    float3 safeDir = float3(safeH, 0.0, safeZ);

    float3 ro = float3(0.0, 0.0, r);
    float2 topHit = atmoRaySphere(ro, safeDir, ATMO_TOP_RADIUS);
    float rayLen = max(topHit.y, 0.0);
    float3 od = atmoOpticalDepth(ro, safeDir, rayLen, 4);

    float3 tau = cfg.rayleighExt * od.x + cfg.mieExt * od.y + cfg.ozoneExt * od.z;
    float3 directT = exp(-tau);
    float peakT = max(directT.r, max(directT.g, directT.b));
    float grazing = 1.0 - smoothstep(0.0, 0.18, max(cosZenith, 0.0));
    float3 msFloor = max(directT, (float3)(peakT * 0.36));
    return horizon * lerp(directT, msFloor, grazing * 0.42);
}

float3 evalSunTransmittanceAtAltitude(float altitudeMeters, float3 sunDir, AtmosphereConfig cfg)
{
    return evalLightTransmittance(altitudeMeters, sunDir.z, cfg);
}

float atmoRayleighPhase(float mu)
{
    return (3.0 / (16.0 * ATMO_PI)) * (1.0 + mu * mu);
}

float atmoMiePhase(float mu, float g)
{
    float g2 = g * g;
    float num = 3.0 * (1.0 - g2) * (1.0 + mu * mu);
    float denom = 8.0 * ATMO_PI * (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * mu, 1.0e-4), 1.5);
    return num / denom;
}

float3 atmoIlluminant(float3 tint)
{
    float peak = max(dot(tint, float3(0.25, 0.55, 0.20)), max(tint.r, max(tint.g, tint.b)) * 0.65);
    return lerp((float3)peak, tint, 0.42);
}

float3 evalAtmosphereRay(float3 viewDir, float3 sunDir, float sunEnabled, float3 sunTint,
                         float3 moonDir, float moonEnabled, float3 moonColor,
                         AtmosphereConfig cfg, out float3 viewTransmittance)
{
    viewTransmittance = (float3)1.0;

    float3 trueDir = viewDir;
    if (trueDir.z < 0.0)
    {
        trueDir = normalize(float3(trueDir.xy, 0.001));
    }

    // Smooth hyperbolic tangent floor avoids 876 km ground-skimming optical paths at z ~ 0
    // so dawn/sunset horizon scattering stays warm golden-peach rather than crushing to red.
    float effZ = 0.5 * (trueDir.z + sqrt(trueDir.z * trueDir.z + 0.0049));
    float3 rd = normalize(float3(trueDir.xy, effZ));

    float3 ro = float3(0.0, 0.0, ATMO_PLANET_RADIUS + 10.0);
    float2 topHit = atmoRaySphere(ro, rd, ATMO_TOP_RADIUS);
    float rayLen = max(topHit.y, 0.0);

    if (cfg.scatterGain <= 1.0e-5 || rayLen <= 0.0)
    {
        return (float3)0.0;
    }

    const int STEPS = 12;
    float stepLen = rayLen / (float)STEPS;

    float3 inscatterR = (float3)0.0;
    float3 inscatterM = (float3)0.0;
    float3 moonR      = (float3)0.0;
    float3 moonM      = (float3)0.0;
    float3 accumOd    = (float3)0.0;

    float3 sunIllum = sunEnabled > 0.5 ? atmoIlluminant(sunTint) : (float3)0.0;
    float3 sunDirectColor = sunEnabled > 0.5 ? sunTint : (float3)0.0;
    float3 moonIllum = moonEnabled > 0.5 ? moonColor : (float3)0.0;

    bool traceSun = sunEnabled > 0.5 && sunDir.z > -0.26;
    bool traceMoon = moonEnabled > 0.5 && moonDir.z > -0.22;

    for (int i = 0; i < STEPS; i++)
    {
        float3 pos = ro + rd * (((float)i + 0.5) * stepLen);
        float rPos = length(pos);
        float h = max(rPos - ATMO_PLANET_RADIUS, 0.0);

        float dR = exp(-h / ATMO_H_RAYLEIGH);
        float dM = exp(-h / ATMO_H_MIE);
        float dO = max(0.0, 1.0 - abs(h - ATMO_OZONE_CENTER) / ATMO_OZONE_HALF_WIDTH);

        accumOd += float3(dR, dM, dO) * stepLen;
        float3 tView = exp(-(cfg.rayleighExt * accumOd.x + cfg.mieExt * accumOd.y + cfg.ozoneExt * accumOd.z));

        if (traceSun)
        {
            float localSunZ = dot(pos, sunDir) / max(rPos, 1.0);
            float3 tSun = evalLightTransmittance(h, localSunZ, cfg);
            float3 weight = tView * tSun * stepLen;
            inscatterR += weight * dR;
            inscatterM += weight * dM;
        }

        if (traceMoon)
        {
            float localMoonZ = dot(pos, moonDir) / max(rPos, 1.0);
            float3 tMoon = evalLightTransmittance(h, localMoonZ, cfg);
            float3 mWeight = tView * tMoon * stepLen;
            moonR += mWeight * dR;
            moonM += mWeight * dM;
        }
    }

    viewTransmittance = exp(-(cfg.rayleighExt * accumOd.x + cfg.mieExt * accumOd.y + cfg.ozoneExt * accumOd.z));

    float muSun = dot(trueDir, sunDir);
    float phaseRSun = atmoRayleighPhase(muSun);
    float phaseMSun = atmoMiePhase(muSun, 0.78);

    float muMoon = dot(trueDir, moonDir);
    float phaseRMoon = atmoRayleighPhase(muMoon);
    float phaseMMoon = atmoMiePhase(muMoon, 0.76);

    // Multi-scattering approximation: prevents zenith crush at twilight while keeping anti-sun darker.
    float twilightFactor = sunEnabled > 0.5 ? smoothstep(-0.14, 0.15, sunDir.z) : 0.0;
    float lowSun = 1.0 - smoothstep(-0.04, 0.38, sunDir.z);
    float sunAzimuthBias = lerp(1.0, lerp(0.16, 1.35, smoothstep(-0.70, 0.85, muSun)), lowSun);
    float antiSunShadow = lerp(1.0, lerp(0.28, 1.0, smoothstep(-0.85, 0.35, muSun)),
                               lowSun * (1.0 - smoothstep(0.0, 0.60, trueDir.z)));
    float3 twilightAmbient = cfg.rayleighScatter * 8000.0 * (0.03 + 0.12 * saturate(trueDir.z + 0.25)) *
                             sunIllum * (0.06 + 0.94 * smoothstep(-0.08, 0.30, sunDir.z)) *
                             twilightFactor * sunAzimuthBias;

    float3 scatterSun = (inscatterR * cfg.rayleighScatter * phaseRSun * sunIllum * antiSunShadow +
                         inscatterM * cfg.mieScatter * phaseMSun * sunDirectColor) * 14.5 +
                        twilightAmbient;

    float moonAzimuthBias = lerp(0.18, 1.0, smoothstep(-0.65, 0.75, muMoon));
    float3 scatterMoon = (moonR * cfg.rayleighScatter * phaseRMoon * 0.07 * moonAzimuthBias +
                          moonM * cfg.mieScatter * phaseMMoon * 0.20) * moonIllum;

    float3 airglow = cfg.tint * (0.0012 * saturate(cfg.density));

    return (scatterSun + scatterMoon + airglow) * cfg.scatterGain;
}

float3 evalAtmosphereSky(float3 viewDir, float3 sunDir, float sunEnabled, float3 sunTint,
                         float3 moonDir, float moonEnabled, float3 moonColor,
                         AtmosphereConfig cfg, out float3 viewTransmittance)
{
    if (viewDir.z >= 0.0)
    {
        return evalAtmosphereRay(viewDir, sunDir, sunEnabled, sunTint, moonDir, moonEnabled, moonColor,
                                 cfg, viewTransmittance);
    }

    float3 horizTrans;
    float3 horizonColor = evalAtmosphereRay(normalize(float3(viewDir.xy, 0.001)),
                                            sunDir, sunEnabled, sunTint, moonDir, moonEnabled, moonColor,
                                            cfg, horizTrans);
    viewTransmittance = horizTrans;

    float3 sunGround = sunEnabled > 0.5 ? sunTint * evalSunTransmittanceAtAltitude(10.0, sunDir, cfg) : (float3)0.0;
    float3 groundBounce = (horizonColor * 0.28 + sunGround * cfg.tint * 0.05) * saturate(cfg.density > 0.0 ? 1.0 : 0.0);
    return lerp(horizonColor * 0.85, groundBounce, smoothstep(0.0, -0.25, viewDir.z));
}

void evalCloudSkyAmbient(float3 sunDir, float sunEnabled, float3 sunTint,
                         float3 moonDir, float moonEnabled, float3 moonColor,
                         AtmosphereConfig cfg,
                         out float3 zenithAmbient, out float3 horizSunward, out float3 horizAway)
{
    float3 dummyTrans;
    zenithAmbient = evalAtmosphereRay(float3(0.0, 0.0, 1.0), sunDir, sunEnabled, sunTint,
                                      moonDir, moonEnabled, moonColor, cfg, dummyTrans);

    float2 primaryXY = (sunEnabled > 0.5 && sunDir.z > -0.15) ? sunDir.xy : moonDir.xy;
    float3 horizDir = length(primaryXY) > 1.0e-4
        ? normalize(float3(primaryXY, 0.14))
        : normalize(float3(1.0, 0.0, 0.14));
    horizSunward = evalAtmosphereRay(horizDir, sunDir, sunEnabled, sunTint,
                                     moonDir, moonEnabled, moonColor, cfg, dummyTrans);
    horizAway = evalAtmosphereRay(float3(-horizDir.xy, 0.14), sunDir, sunEnabled, sunTint,
                                  moonDir, moonEnabled, moonColor, cfg, dummyTrans);
}

#endif
