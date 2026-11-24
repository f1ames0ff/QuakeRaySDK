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

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TEST 2
#include "ShaderCommonHLSLFunc.hlsli"
#include "BRDF.hlsli"
#include "Q2Asvgf.hlsli"

struct SynthTestParams
{
    uint computeRef;
    uint lightOn;
    uint forceZeroHitDist;
    uint coplanarStep;
    float camPosX;
    float prevCamPosX;
    float pad1;
    float pad2;
};

[[vk::binding(0, DESC_SET_TEST)]]
ConstantBuffer<SynthTestParams> testParams;

[[vk::binding(1, DESC_SET_TEST), vk::image_format("rgba16f")]]
RWTexture2D<float4> testRefSpecular;

uint pcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float nextRand(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    const uint outVal = (word >> 22u) ^ word;
    return (float)(outVal & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

void bandMaterial(const int2 regPix, const int height, out float metallic, out float roughness, out float3 albedo)
{
    const int bandH = height / 6;
    const int band = clamp(regPix.y / bandH, 0, 5);
    albedo = float3(1.0, 1.0, 1.0);

    if (band == 0)
    {
        if (regPix.x >= 120 && regPix.x < 176)
        {
            metallic = 0.0;
            roughness = 0.02;
        }
        else
        {
            metallic = 1.0;
            roughness = 0.015;
        }
    }
    else if (band == 1)
    {
        metallic = 0.9;
        roughness = 0.15;
    }
    else if (band == 2)
    {
        metallic = 0.8;
        roughness = 0.25;
    }
    else if (band == 3)
    {
        metallic = 0.6;
        roughness = 0.50;
    }
    else if (band == 4)
    {
        metallic = 1.0;
        roughness = 0.06;
    }
    else
    {
        metallic = 0.8;
        roughness = (((regPix.x ^ regPix.y) & 1) != 0) ? 0.85 : 0.05;
    }
}

float3 synthWallColor(const float3 hitPos, const float tHit, const float tz, const float tx, const uint lightOn, const int band)
{
    if (abs(tHit - tz) < 1e-3 && hitPos.z > 2.5)
    {
        if (band == 2)
        {
            const bool horizBar = (lightOn != 0u) && (abs(hitPos.y + 0.5) < 0.75);
            return horizBar ? float3(6.0, 5.2, 3.8) : float3(0.28, 0.28, 0.30);
        }

        const bool strip = (lightOn != 0u) && (abs(hitPos.x) < 0.65);
        if (strip)
        {
            return float3(9.0, 7.5, 5.0);
        }

        if (band == 5)
        {
            return (hitPos.x < 0.0) ? float3(1.2, 0.35, 0.25) : float3(0.25, 0.45, 1.4);
        }

        return float3(0.22, 0.24, 0.28);
    }

    if (abs(tHit - tx) < 1e-3)
    {
        return hitPos.x < 0.0 ? float3(0.55, 0.12, 0.08) : float3(0.10, 0.55, 0.14);
    }

    return hitPos.y > 0.0 ? float3(0.35, 0.35, 0.35) : float3(0.16, 0.16, 0.18);
}

float3 synthBand0DielectricPeak(const int2 regPix, const int height)
{
    const int bandH = height / 6;
    if (regPix.y >= bandH || regPix.x < 120 || regPix.x >= 176)
    {
        return (float3)0.0;
    }

    const float dx = ((float)regPix.x - 148.0) / 10.0;
    const float dy = ((float)regPix.y - 0.5 * (float)bandH) / 7.0;
    const float profile = exp(-(dx * dx + dy * dy));
    return float3(10.0, 8.0, 6.0) * profile;
}

float3 evalSynthBounce(const float3 surfPos, const float3 normal, const float3 toViewerDir,
                       const float roughness, const float3 ro_s, const float2 u,
                       const int band, const bool addFirefly,
                       out float outHitDist)
{
    float oneOverPdf = 0.0;
    const float3 bounceDir = sampleSmithGGX(normal, toViewerDir, roughness, u.x, u.y, oneOverPdf);
    const float nl = dot(normal, bounceDir);
    if (nl <= 0.0)
    {
        outHitDist = Q2_MAX_VIEW_DIR_HIT_DIST;
        return (float3)0.0;
    }

    const float tz = (3.0 - surfPos.z) / max(bounceDir.z, 1e-4);
    const float tx = (abs(bounceDir.x) > 1e-4)
        ? (((bounceDir.x > 0.0 ? 6.0 : -6.0) - surfPos.x) / bounceDir.x)
        : 1e6;
    const float ty = (abs(bounceDir.y) > 1e-4)
        ? (((bounceDir.y > 0.0 ? 8.0 : -8.0) - surfPos.y) / bounceDir.y)
        : 1e6;

    const float tHit = min(tz, min(tx, ty));
    outHitDist = clamp(tHit, 0.0, Q2_MAX_VIEW_DIR_HIT_DIST);
    const float3 hitPos = surfPos + bounceDir * tHit;
    float3 rad = synthWallColor(hitPos, tHit, tz, tx, testParams.lightOn, band);
    if (addFirefly)
    {
        rad += float3(800.0, 800.0, 800.0);
    }

    const float specMaxRadiance = lerp(50.0, 10.0, smoothstep(0.08, 0.40, roughness));
    const float radLum = getLuminance(rad);
    if (radLum > specMaxRadiance)
    {
        rad *= (specMaxRadiance / radLum);
    }

    const float3 weight = clamp(nl * evalBRDFSmithGGX(normal, toViewerDir, bounceDir, roughness, ro_s) * oneOverPdf,
                                (float3)0.0, (float3)2.0);
    return rad * weight;
}

#ifdef PASS_GBUFFER
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = (int2)dispatchThreadID.xy;
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    if (pix.x >= width || pix.y >= height)
    {
        return;
    }

    const int2 regPix = getRegularPixFromCheckerboardPix(pix);
    const float2 uv = ((float2)regPix + 0.5) / float2(width, height);
    const float2 ndc = uv * 2.0 - 1.0;

    // Mirror wall at z = 0 viewed with fovY = 90 deg, aspect = 1.0 from (camPosX, 0, +3.0).
    const float3 camCur = float3(testParams.camPosX, 0.0, 3.0);
    const float3 worldPos = float3(testParams.camPosX + 3.0 * ndc.x, -3.0 * ndc.y, 0.0);
    const float baseCamDist = length(worldPos - camCur);
    const float3 rayDir = (worldPos - camCur) / max(baseCamDist, 1e-4);

    // Exact screen motion of static z = 0 wall from camPosX to prevCamPosX.
    const float prevNdcX = (worldPos.x - testParams.prevCamPosX) / 3.0;
    const float prevUvX = prevNdcX * 0.5 + 0.5;
    const float motionX = prevUvX - uv.x;
    const float camPrevDist = length(worldPos - float3(testParams.prevCamPosX, 0.0, 3.0));
    const float motionZ = camPrevDist - baseCamDist;

    const int band = clamp(regPix.y / (height / 6), 0, 5);
    float camDist = baseCamDist;
    if (testParams.coplanarStep != 0u && band == 4 && regPix.x >= width / 2)
    {
        camDist = baseCamDist * 2.4;
    }

    float metallic, roughness;
    float3 albedo;
    bandMaterial(regPix, height, metallic, roughness, albedo);

    const float3 normal = float3(0.0, 0.0, 1.0);
    framebufAlbedo[regPix] = float4(albedo, 0.0);
    framebufIsSky[pix] = (uint4)0u;
    imageStoreNormal(pix, normal);
    imageStoreNormalGeometry(pix, normal);
    framebufMetallicRoughness[pix] = float4(metallic, roughness, 0.0, 0.0);
    framebufQ2ViewDepth[pix] = float4(camDist, 0.0, 0.0, 0.0);
    framebufDepthWorld[pix] = float4(camDist, 0.0, 0.0, 0.0);
    framebufDepthGrad[pix] = float4(250.0, 0.0, 0.0, 0.0);
    framebufMotion[pix] = float4(motionX, 0.0, motionZ, 0.0);
    framebufSurfacePosition[pix] = float4(worldPos, 0.0);
    framebufViewDirection[pix] = float4(rayDir, -1.0);
    framebufThroughput[pix] = float4(1.0, 1.0, 1.0, 0.0);
    framebufQ2Cluster[pix] = (uint4)(uint)(band + 1);

    const uint seed = pcgHash((uint)regPix.x * 1973u + (uint)regPix.y * 9277u + globalUniform.frameId * 26699u + 1u);
    framebufQ2RngSeed[pix] = (uint4)seed;
    framebufQ2Transparent[pix] = (float4)0.0;
    framebufQ2FogAccum[pix] = (float4)0.0;
}
#endif

#ifdef PASS_SHADE
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = (int2)dispatchThreadID.xy;
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    if (pix.x >= width || pix.y >= height)
    {
        return;
    }

    const int2 regPix = getRegularPixFromCheckerboardPix(pix);
    const int bandH = height / 6;
    const int band = clamp(regPix.y / bandH, 0, 5);

    const uint seed = framebufQ2RngSeed_Sampled.Load(int3(pix, 0)).r;
    const float3 surfPos = framebufSurfacePosition_Sampled.Load(int3(pix, 0)).xyz;
    const float3 normal = texelFetchNormal(pix);
    const float2 mr = framebufMetallicRoughness_Sampled.Load(int3(pix, 0)).xy;
    const float metallic = mr.x;
    const float roughness = mr.y;

    const float3 rayDir = safeNormalize(framebufViewDirection.Load(pix).xyz);
    const float3 toViewerDir = -rayDir;
    const float3 albedo = float3(1.0, 1.0, 1.0);
    const float3 ro_s = getSpecularColor(albedo, metallic);
    const float3 directSpec = synthBand0DielectricPeak(regPix, height);

    float3 directDiffuse = float3(2.0, 2.0, 2.0);
    if (band == 2)
    {
        const bool litBar = (surfPos.x < 0.0) && (((int)floor((surfPos.x + 16.0) * 10.6667) & 1) == 0);
        directDiffuse = litBar ? float3(10.0, 10.0, 10.0) : float3(0.0, 0.0, 0.0);
    }

    const bool halfRes = globalUniform.giBounceRays.x >= 0.25 && globalUniform.giBounceRays.x < 0.75;
    const bool traced = !halfRes || ((pix.y & 1) == (int)(globalUniform.frameId & 1u));
    const float tracedTag = float(globalUniform.frameId & 1u) + 1.0;

    float3 demodIndir = (float3)0.0;
    float hitDist = -1.0;

    if (traced)
    {
        uint rng = seed;
        const float2 u = float2(nextRand(rng), nextRand(rng));
        const int wx = (int)floor((surfPos.x + 16.0) * 32.0);
        const int wy = (int)floor((surfPos.y + 16.0) * 32.0);
        const int ffRow = (surfPos.x < 0.0) ? 2 : 3;
        const bool injectFirefly = (band == 5) && ((wx & 7) == 2) && ((wy & 7) == ffRow) && (globalUniform.frameId >= 10u);
        const float3 specMod = evalSynthBounce(surfPos, normal, toViewerDir, roughness, ro_s, u, band, injectFirefly, hitDist);
        demodIndir = min(demodulateSpecular(specMod, ro_s), (float3)32768.0);
        framebufUnfilteredIndirectSpecular[pix] = float4(demodIndir, tracedTag);
        if (getLuminance(demodIndir) > 1e-5)
        {
            const float storedDist = (testParams.forceZeroHitDist != 0u) ? 0.0 : hitDist;
            framebufViewDirection[pix] = float4(rayDir, storedDist);
        }
        imageStoreUnfilteredIndirectSH(pix, irradianceToSH(float3(0.40, 0.35, 0.30), normal));
    }
    else
    {
        framebufUnfilteredIndirectSpecular[pix] = float4(0.0, 0.0, 0.0, 0.0);
        framebufViewDirection[pix] = float4(rayDir, -1.0);
        imageStoreUnfilteredIndirectSH(pix, irradianceToSH((float3)0.0, normal));
    }

    imageStoreUnfilteredDirect(pix, directDiffuse);
    imageStoreUnfilteredSpecular(pix, demodulateSpecular(directSpec, ro_s));

    if (testParams.computeRef != 0u)
    {
        const float3 camCur = float3(testParams.camPosX, 0.0, 3.0);
        const float2 uv = ((float2)regPix + 0.5) / float2(width, height);
        const float2 ndc = uv * 2.0 - 1.0;
        const float3 refSurfPos = float3(testParams.camPosX + 3.0 * ndc.x, -3.0 * ndc.y, 0.0);
        const float3 refToViewer = safeNormalize(camCur - refSurfPos);
        const float jitterX = (float)(pcgHash(seed) & 0xFFFFu) * (1.0 / 65536.0);
        const float jitterY = (float)(pcgHash(seed ^ 0x9E3779B9u) & 0xFFFFu) * (1.0 / 65536.0);

        float refMetallic, refRoughness;
        float3 refAlbedo;
        bandMaterial(regPix, height, refMetallic, refRoughness, refAlbedo);
        const float3 refNormal = float3(0.0, 0.0, 1.0);
        const float3 refRos = getSpecularColor(refAlbedo, refMetallic);

        float3 refAccum = (float3)0.0;
        const uint sampleCount = 512u;
        const float phi1 = 0.61803398875;
        const float phi2 = 0.75487766625;
        for (uint s = 0u; s < sampleCount; s++)
        {
            const float2 uRef = float2(
                frac(jitterX + (float)s * phi1),
                frac(jitterY + (float)s * phi2));
            float dummyDist;
            refAccum += evalSynthBounce(refSurfPos, refNormal, refToViewer, refRoughness, refRos, uRef, band, false, dummyDist);
        }
        testRefSpecular[pix] = float4(refAccum * (1.0 / (float)sampleCount) + directSpec, 0.0);
    }
}
#endif
