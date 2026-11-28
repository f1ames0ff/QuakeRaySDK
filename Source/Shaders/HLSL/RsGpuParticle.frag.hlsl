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

#define DESC_SET_TEXTURES       0
#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_FRAMEBUFFERS   3
#define DESC_SET_TONEMAPPING    7

#include "ShaderCommonHLSLFunc.hlsli"
#include "Exposure.hlsli"
#include "Q2Asvgf.hlsli"
#include "Smoke.hlsli"
#include "GpuParticles.hlsli"

struct GpuParticleDrawPush_BT
{
    float4x4 viewProj;
    uint     mode;
    uint     maxParticles;
    uint2    renderSize;
};

[[vk::push_constant]] ConstantBuffer<GpuParticleDrawPush_BT> drawPush;

struct RsGpuParticleFragOutput
{
    [[vk::location(0)]] float4 outColor          : SV_Target0;
    [[vk::location(1)]] float4 outScreenEmission : SV_Target1;
};

void evalParticleHdrGains(const float emission, const float coreHotness, const float sceneExposure,
                          out float outAdditiveGain, out float outSurfaceEmisGain)
{
    const float overbright = max(emission - 6.0, 0.0);
    const float haloCurve  = 0.55 + emission * 0.38 + overbright * overbright * 0.045;
    const float coreBoost  = 1.0 + coreHotness * (0.55 + overbright * 0.24);
    outAdditiveGain        = haloCurve * coreBoost * sceneExposure;
    outSurfaceEmisGain     = (emission * 0.38 + overbright * overbright * 0.045) * sceneExposure;
}

RsGpuParticleFragOutput main(
    [[vk::location(0)]] float4 inColor  : COLOR0,
    [[vk::location(1)]] float2 inUv     : TEXCOORD0,
    [[vk::location(2)]] float3 inLit    : TEXCOORD1,
    [[vk::location(3)]] float3 inNormal : TEXCOORD2,
    [[vk::location(4)]] float4 inMeta   : TEXCOORD3,
    float4 fragCoord : SV_Position)
{
    const uint bucket       = drawPush.mode;
    const uint mode         = gpuParticleBucketToMode(bucket);
    const bool isAdditive   = gpuParticleBucketIsAdditive(bucket);
    const uint textureIndex = uint(inMeta.y + 0.5);
    const float emission    = max(inMeta.z, 0.0);
    const bool selfLit      = inLit.z < 0.0;

    const float adaptedLum = tonemapping[0].adaptedLuminance;
    const float sceneExposure = adaptedLum > 0.0
        ? adaptedLum * exp2(2.0 - tonemapping[0].tmExposureBias)
        : 1.0;

    float4 texSample = float4(1.0, 1.0, 1.0, 1.0);
    float coreHotness = 0.0;
    if (textureIndex != MATERIAL_NO_TEXTURE)
    {
        texSample = getTextureSample(textureIndex, inUv);
    }
    else if (mode == GPU_PARTICLE_MODE_SPARK)
    {
        const float2 p = inUv * 2.0 - 1.0;
        const float headBias = lerp(0.30, 1.0, smoothstep(-1.0, 0.45, p.x));
        const float axialFade = smoothstep(1.0, 0.04, abs(p.x));
        const float perpDist = abs(p.y);
        const float perpFade = smoothstep(1.0, 0.08, perpDist);
        const float core = exp(-9.5 * perpDist * perpDist) * axialFade * headBias;
        const float halo = exp(-3.0 * (perpDist * perpDist + 0.35 * p.x * p.x)) * axialFade * perpFade;
        const float falloff = saturate(core * 0.86 + halo * 0.34);
        const float3 incand = lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.97, 0.88), 0.25);
        texSample = float4(lerp((float3)1.0, incand, saturate(core * 1.2)), falloff);
        coreHotness = saturate(core * 1.15);
    }
    else if (mode == GPU_PARTICLE_MODE_BILLBOARD)
    {
        const float r = length(inUv * 2.0 - 1.0);
        if (isAdditive)
        {
            const float edgeFade = smoothstep(1.0, 0.02, r);
            const float core = exp(-7.5 * r * r) * edgeFade;
            const float halo = pow(edgeFade, 2.2);
            texSample = float4(1.0, 1.0, 1.0, saturate(core * 0.80 + halo * 0.35));
            coreHotness = saturate(core);
        }
        else
        {
            texSample = float4(1.0, 1.0, 1.0, pow(smoothstep(1.0, 0.05, r), 1.6));
        }
    }
    else if (mode == GPU_PARTICLE_MODE_DEBRIS_3D)
    {
        const float grain = smokeHash(float3(floor(inUv * 8.0), inMeta.w));
        const float shade = lerp(0.78, 1.12, grain);
        texSample = float4(shade, shade, shade, 1.0);
    }

    const float alpha = saturate(inColor.a * texSample.a);
    if (mode == GPU_PARTICLE_MODE_RETRO_SQUARE && alpha < 0.5)
    {
        discard;
    }
    if (alpha <= 1e-4)
    {
        discard;
    }

    float3 lightFactor = (float3)1.0;
    if (!selfLit)
    {
        const int2 cbPix = getCheckerboardPix(int2(fragCoord.xy));
        const int2 lfMax = max(int2(globalUniform.renderWidth, globalUniform.renderHeight) / Q2_GRAD_DWN - 1, (int2)0);
        const int2 lfPix = clamp(cbPix / Q2_GRAD_DWN, (int2)0, lfMax);

        Q2SH lf;
        lf.shY  = framebufQ2AtrousPingLF_SH_Sampled.Load(int3(lfPix, 0)) / Q2_STORAGE_SCALE_LF;
        lf.CoCg = framebufQ2AtrousPingLF_COCG_Sampled.Load(int3(lfPix, 0)).xy / Q2_STORAGE_SCALE_LF;

        const float3 shNormal = mode == GPU_PARTICLE_MODE_DEBRIS_3D ? safeNormalize(inNormal) : (float3)0.0;
        const float3 ambient = q2SHToIrradiance(lf, shNormal) * 0.25;
        lightFactor = max(inLit + ambient, (float3)0.015);
    }

    const float3 baseRgb = inColor.rgb * texSample.rgb;
    float additiveGain = 0.0;
    float surfaceEmisGain = 0.0;
    evalParticleHdrGains(emission, coreHotness, sceneExposure, additiveGain, surfaceEmisGain);

    RsGpuParticleFragOutput o;
    o.outScreenEmission = float4(0.0, 0.0, 0.0, 0.0);
    if (isAdditive)
    {
        const float3 hotRgb = lerp(baseRgb, lerp(baseRgb, float3(1.0, 0.97, 0.90), 0.50), coreHotness);
        o.outColor = float4(min(hotRgb * (additiveGain * alpha), (float3)32000.0), 0.0);
    }
    else
    {
        const float3 litRgb = selfLit
            ? baseRgb * (0.85 * sceneExposure + surfaceEmisGain)
            : (baseRgb * lightFactor + baseRgb * surfaceEmisGain);
        o.outColor = float4(min(litRgb, (float3)32000.0), alpha);
    }
    return o;
}
