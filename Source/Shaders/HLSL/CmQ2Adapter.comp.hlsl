// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/compositing.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
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
//
// HLSL counterpart of CmQ2Adapter.comp. Pinned by the pair GLSL/CmQ2Adapter.comp <->
// this file, which pulls in BRDF.hlsli and Q2Asvgf.hlsli and pins them through the calls of
// this pass.
//
// Spellings that had to change:
//   * layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in
//     -> [numthreads(16, 16, 1)] on main, and gl_GlobalInvocationID -> SV_DispatchThreadID,
//     which only the entry point can receive
//   * const utexture2D buf -> const Texture2D<uint4> buf, the samplerless unsigned view of
//     the r32ui framebuffer, passed as a function parameter the way the other accessors of
//     the port pass a Texture2D. texelFetch(buf, p, 0) -> buf.Load(int3(p, 0))
//   * ivec2/vec2/vec3/vec4/uvec4 -> int2/float2/float3/float4/uint4; the reinterpret casts of
//     the golden become C style conversions: uvec4(q2PackRGBE(hf)) -> (uint4)q2PackRGBE(hf)
//   * vec3(0.0) -> (float3)0.0 and vec3(globalUniform.fixedAlbedo.x) -> (float3)x: dxc refuses
//     to widen a single scalar through a constructor, so the broadcast is spelled as a cast
//     (measured in Q2Asvgf.hlsli, where the same broadcast folds identically on both halves)
//   * texelFetch(framebufAlbedo_Sampled, ...) / texelFetch(framebufThroughput_Sampled, ...) /
//     texelFetch(framebufMetallicRoughness_Sampled, ...) -> .Load(int3(pix, 0)) on the same
//     sampled views, as in ShaderCommonHLSLFunc.hlsli; texelFetchUnfilteredDirect,
//     texelFetchUnfilteredSpecular and texelFetchUnfilteredIndirectSH are those accessors and
//     keep their names
//   * imageStore(img, pix, v) -> img[pix] = v, the storage image write of the port; the whole
//     vec4 of the Q2Color store is kept, including its zero alpha. The golden's identity
//     constructor vec4(lf.shY * Q2_STORAGE_SCALE_LF) is dropped, because a vec4 of a vec4 is
//     the same value; the four-component one keeps its float4 spelling
//   * int(globalUniform.renderWidth) -> (int)globalUniform.renderWidth
//
// What did not change: the firefly constants Q2_FIREFLY_FACTOR and Q2_FIREFLY_MIN_LUM and the
// firefly test with its 1.0 initial maximum and its nine neighbour bounds; every operand
// order, the diffuse + indirect and specular + ro_s sums of the unfiltered composite, the
// DEBUG_SHOW_FLAG_* branches with their else if chain, the throughput and
// getMaterialAmbient(albedo) products, the premultiplied blends of the Q2Transparent and
// Q2FogAccum framebuffers, the max((float3)0.0, illuminated) clamp of the Q2Color store and
// the Q2_STORAGE_SCALE_HF / _SPEC / _LF scalings of the filtered path; the Q2RTX channel
// order UnfilteredDirect -> HF, UnfilteredSpecular -> SPEC, UnfilteredIndirectSH -> LF.

//   UnfilteredDirect      -> HF   (direct diffuse irradiance, packed RGBE)
//   UnfilteredSpecular    -> SPEC (demodulated specular irradiance, packed RGBE)
//   UnfilteredIndirectSH  -> LF   (indirect diffuse, YCoCg luma SH + chroma)
//
// The ASVGF denoiser then operates on these channels (see CmQ2Temporal.comp,
// CmQ2AtrousLF.comp, CmQ2Atrous.comp).

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "BRDF.hlsli"
#include "Q2Asvgf.hlsli"
#include "Subsurface.hlsli"

#define Q2_FIREFLY_FACTOR 8.0
#define Q2_FIREFLY_MIN_LUM 1.0

#define Q2_LF_CHROMA_TO_RADIANCE (1.0 / 0.282095)

float3 q2AntiFirefly(const int2 pix, const float3 center, const Texture2D<uint4> buf, const int width, const int height)
{
    const float centerLum = getLuminance(center);
    if (!isfinite(centerLum))
    {
        return (float3)0.0;
    }
    if (centerLum < Q2_FIREFLY_MIN_LUM)
    {
        return center;
    }

    const int halfWidth = width / 2;
    const int field_left = (pix.x >= halfWidth) ? halfWidth : 0;
    const int field_right = (pix.x >= halfWidth) ? width : halfWidth;

    float max1 = 1.0f;
    float max2 = 1.0f;
    for (int yy = -1; yy <= 1; yy++)
    {
        for (int xx = -1; xx <= 1; xx++)
        {
            if (xx == 0 && yy == 0)
            {
                continue;
            }

            const int2 p = pix + int2(xx, yy);
            if (p.x < field_left || p.x >= field_right || p.y < 0 || p.y >= height)
            {
                continue;
            }

            const float3 c = decodeE5B9G9R9(buf.Load(int3(p, 0)).r);
            const float lum = getLuminance(c);
            if (!isfinite(lum))
            {
                continue;
            }
            if (lum > max1)
            {
                max2 = max1;
                max1 = lum;
            }
            else if (lum > max2)
            {
                max2 = lum;
            }
        }
    }

    const float limit = max2 * Q2_FIREFLY_FACTOR;
    if (centerLum > limit)
    {
        return center * (limit / centerLum);
    }

    return center;
}

bool q2IsCurrGradientSample(const int2 p, const int width, const int height)
{
    if (globalUniform.fltEnable.x < 0.5 || !q2InGradBounds(p, width, height))
    {
        return false;
    }

    const int2 pos_grad = p / Q2_GRAD_DWN;
    const uint u = framebufQ2GradSmplPos_Sampled.Load(int3(pos_grad, 0)).r;
    if ((u & (1u << 31)) == 0u)
    {
        return false;
    }

    const int2 stratum = int2(
        (u >> (Q2_STRATUM_OFFSET_SHIFT * 0)) & Q2_STRATUM_OFFSET_MASK,
        (u >> (Q2_STRATUM_OFFSET_SHIFT * 1)) & Q2_STRATUM_OFFSET_MASK);
    return all(pos_grad * Q2_GRAD_DWN + stratum == p);
}

bool q2EvalHalfResNeighborWeight(const int2 p, const float depth_c, const float3 normal_c,
                                 const int width, const int height, out float wDepth, out float wBilateral)
{
    wDepth = 0.0;
    wBilateral = 0.0;
    if (isSkyPix(p) || q2IsCurrGradientSample(p, width, height))
    {
        return false;
    }

    const float depth_n = framebufQ2ViewDepth_Sampled.Load(int3(p, 0)).r;
    const float3 normal_n = texelFetchNormal(p);
    wDepth = exp(-abs(depth_n - depth_c) / max(0.05 * abs(depth_c), 1e-4));
    const float wNormal = pow(saturate(dot(normal_n, normal_c)), 8.0);
    wBilateral = wDepth * wNormal;
    return true;
}

float3 q2FetchReconstructedIndirectSpec(const int2 pix, const int width, const int height, const bool filterFireflies)
{
    if (globalUniform.giBounceRays.x < 0.25 || isSkyPix(pix))
    {
        return (float3)0.0;
    }

    const int halfWidth = width / 2;
    const int field_left = (pix.x >= halfWidth) ? halfWidth : 0;
    const int field_right = (pix.x >= halfWidth) ? width : halfWidth;

    const float4 raw = framebufUnfilteredIndirectSpecular_Sampled.Load(int3(pix, 0));
    const bool halfRes = globalUniform.giBounceRays.x < 0.75;
    const float expectedTag = float(globalUniform.frameId & 1u) + 1.0;

    float3 center = raw.rgb;
    if (halfRes && abs(raw.a - expectedTag) > 0.25)
    {
        const float depth_c = framebufQ2ViewDepth_Sampled.Load(int3(pix, 0)).r;
        const float3 normal_c = texelFetchNormal(pix);
        const int2 flat_c = q2CheckerToFlat(pix, width);
        const bool canCrossFields = framebufThroughput_Sampled.Load(int3(pix, 0)).a <= 0.5;

        float3 sumBilateral = (float3)0.0;
        float sumW = 0.0;
        float3 sumSideBilateral = (float3)0.0;
        float sumSideW = 0.0;
        float3 sumFallback = (float3)0.0;
        float sumFallbackW = 0.0;

        if (canCrossFields)
        {
            for (int yy = -1; yy <= 1; yy += 2)
            {
                const int fy = flat_c.y + yy;
                if (fy < 0 || fy >= height)
                {
                    continue;
                }

                const int2 p = q2FlatToChecker(int2(flat_c.x, fy), width);
                const float4 s = framebufUnfilteredIndirectSpecular_Sampled.Load(int3(p, 0));
                if (abs(s.a - expectedTag) > 0.25)
                {
                    continue;
                }

                float wDepth, wBi;
                if (!q2EvalHalfResNeighborWeight(p, depth_c, normal_c, width, height, wDepth, wBi))
                {
                    continue;
                }

                sumBilateral += s.rgb * wBi;
                sumW += wBi;
                if (wDepth > 0.1)
                {
                    sumFallback += s.rgb * wDepth;
                    sumFallbackW += wDepth;
                }
            }
        }

        for (int yy = -1; yy <= 1; yy += 2)
        {
            for (int xx = -1; xx <= 1; xx++)
            {
                const int2 p = pix + int2(xx, yy);
                if (p.x < field_left || p.x >= field_right || p.y < 0 || p.y >= height)
                {
                    continue;
                }

                const int2 p_flat = q2CheckerToFlat(p, width);
                if (abs(p_flat.x - flat_c.x) > 1)
                {
                    continue;
                }

                const float4 s = framebufUnfilteredIndirectSpecular_Sampled.Load(int3(p, 0));
                if (abs(s.a - expectedTag) > 0.25)
                {
                    continue;
                }

                float wDepth, wBi;
                if (!q2EvalHalfResNeighborWeight(p, depth_c, normal_c, width, height, wDepth, wBi))
                {
                    continue;
                }

                const float wSpatial = 0.5;
                sumSideBilateral += s.rgb * (wSpatial * wBi);
                sumSideW += wSpatial * wBi;
                if (wDepth > 0.1)
                {
                    sumFallback += s.rgb * (wSpatial * wDepth);
                    sumFallbackW += wSpatial * wDepth;
                }
            }
        }

        if (sumW > 1e-4)
        {
            center = sumBilateral / sumW;
        }
        else if (sumSideW > 1e-6)
        {
            center = sumSideBilateral / sumSideW;
        }
        else if (sumFallbackW > 1e-6)
        {
            center = sumFallback / sumFallbackW;
        }
        else
        {
            center = (float3)0.0;
        }
    }

    const float centerLum = getLuminance(center);
    return isfinite(centerLum) ? max(center, (float3)0.0) : (float3)0.0;
}

SH q2FetchReconstructedIndirectSH(const int2 pix, const int width, const int height)
{
    if (globalUniform.giBounceRays.x < 0.25 || isSkyPix(pix))
    {
        return newSH();
    }

    const bool halfRes = globalUniform.giBounceRays.x < 0.75;
    const int currParity = (int)(globalUniform.frameId & 1u);
    if (!halfRes || ((pix.y & 1) == currParity))
    {
        return texelFetchUnfilteredIndirectSH(pix);
    }

    const int halfWidth = width / 2;
    const int field_left = (pix.x >= halfWidth) ? halfWidth : 0;
    const int field_right = (pix.x >= halfWidth) ? width : halfWidth;
    const float depth_c = framebufQ2ViewDepth_Sampled.Load(int3(pix, 0)).r;
    const float3 normal_c = texelFetchNormal(pix);
    const int2 flat_c = q2CheckerToFlat(pix, width);
    const bool canCrossFields = framebufThroughput_Sampled.Load(int3(pix, 0)).a <= 0.5;

    SH sumBilateral = newSH();
    float sumW = 0.0;
    SH sumSideBilateral = newSH();
    float sumSideW = 0.0;
    SH sumFallback = newSH();
    float sumFallbackW = 0.0;

    if (canCrossFields)
    {
        for (int yy = -1; yy <= 1; yy += 2)
        {
            const int fy = flat_c.y + yy;
            if (fy < 0 || fy >= height)
            {
                continue;
            }

            const int2 p = q2FlatToChecker(int2(flat_c.x, fy), width);
            float wDepth, wBi;
            if (!q2EvalHalfResNeighborWeight(p, depth_c, normal_c, width, height, wDepth, wBi))
            {
                continue;
            }

            const SH nSh = texelFetchUnfilteredIndirectSH(p);
            accumulateSH(sumBilateral, nSh, wBi);
            sumW += wBi;
            if (wDepth > 0.1)
            {
                accumulateSH(sumFallback, nSh, wDepth);
                sumFallbackW += wDepth;
            }
        }
    }

    for (int yy = -1; yy <= 1; yy += 2)
    {
        for (int xx = -1; xx <= 1; xx++)
        {
            const int2 p = pix + int2(xx, yy);
            if (p.x < field_left || p.x >= field_right || p.y < 0 || p.y >= height)
            {
                continue;
            }

            const int2 p_flat = q2CheckerToFlat(p, width);
            if (abs(p_flat.x - flat_c.x) > 1)
            {
                continue;
            }

            float wDepth, wBi;
            if (!q2EvalHalfResNeighborWeight(p, depth_c, normal_c, width, height, wDepth, wBi))
            {
                continue;
            }

            const float wSpatial = 0.5;
            const SH nSh = texelFetchUnfilteredIndirectSH(p);
            accumulateSH(sumSideBilateral, nSh, wSpatial * wBi);
            sumSideW += wSpatial * wBi;
            if (wDepth > 0.1)
            {
                accumulateSH(sumFallback, nSh, wSpatial * wDepth);
                sumFallbackW += wSpatial * wDepth;
            }
        }
    }

    if (sumW > 1e-4)
    {
        multiplySH(sumBilateral, 1.0 / sumW);
        return sumBilateral;
    }
    if (sumSideW > 1e-6)
    {
        multiplySH(sumSideBilateral, 1.0 / sumSideW);
        return sumSideBilateral;
    }
    if (sumFallbackW > 1e-6)
    {
        multiplySH(sumFallback, 1.0 / sumFallbackW);
        return sumFallback;
    }
    return newSH();
}

// Composites the raw, unfiltered ReSTIR signal. Used when the denoiser is
// disabled (Q2RTX flt_enable = 0): the lighting channels come straight from the
// path tracer instead of the ASVGF reconstructions, so the image is noisy but
// reacts to any lighting change immediately.
void q2CompositeUnfiltered(const int2 pix, const int width, const int height)
{
    const float3 diffuse = texelFetchUnfilteredDirect(pix);
    const float3 specular = texelFetchUnfilteredSpecular(pix) + q2FetchReconstructedIndirectSpec(pix, width, height, false);
    const SH indir = q2FetchReconstructedIndirectSH(pix, width, height);

    const float3 albedo = framebufAlbedo_Sampled.Load(int3(getRegularPixFromCheckerboardPix(pix), 0)).rgb;
    const float3 throughput = framebufThroughput_Sampled.Load(int3(pix, 0)).rgb;

    float3 illuminated;
    float3 sssDiffuse = (float3)0.0;

    if (!isSkyPix(pix))
    {
        const float2 mrFb = framebufMetallicRoughness_Sampled.Load(int3(pix, 0)).xy;
        const float3 normal = texelFetchNormal(pix);

        const float metallic = mrFb.x;

        const float3 indirect = SHToIrradiance(indir, normal);

        const float3 ro_s = getSpecularColor(albedo, metallic);
        float3 ro_d = albedo * (1.0 - metallic);

        // Q2RTX flt_fixed_albedo: replace the diffuse albedo after filtering,
        // keeping the specular reflectivity textured, for a "no textures" mode.
        if (globalUniform.fixedAlbedo.x != 0.0)
        {
            ro_d = (float3)globalUniform.fixedAlbedo.x;
        }

        const bool debugDisableSss = (globalUniform.debugShowFlags &
            (DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE |
             DEBUG_SHOW_FLAG_ONLY_SPECULAR |
             DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE)) != 0u;
        const float sssMask = debugDisableSss
            ? 0.0
            : getSubsurfaceMask(framebufQ2Subsurface_Sampled.Load(int3(pix, 0)));

        illuminated =
            (diffuse + indirect) * ((1.0 - sssMask) * ro_d) +
            specular * ro_s;
        if (sssMask > 0.0)
        {
            sssDiffuse = max(diffuse + indirect, (float3)0.0);
        }

        if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE) != 0)
        {
            illuminated = diffuse * albedo;
        }
        else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_ONLY_SPECULAR) != 0)
        {
            illuminated = specular * ro_s;
        }
        else if ((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE) != 0)
        {
            illuminated = indirect;
        }

        illuminated *= throughput;
        illuminated *= getMaterialAmbient(albedo);
    }
    else
    {
        illuminated = albedo * throughput;
    }

    const float4 q2Transparent = framebufQ2Transparent_Sampled.Load(int3(pix, 0));
    illuminated = q2Transparent.rgb + illuminated * (1.0 - q2Transparent.a);

    const float4 fogAccum = framebufQ2FogAccum_Sampled.Load(int3(pix, 0));
    illuminated = fogAccum.rgb + illuminated * (1.0 - fogAccum.a);

    framebufQ2Color[pix] = float4(max((float3)0.0, illuminated), 0);
    framebufQ2SssDiffuse[pix] = float4(sssDiffuse, 0);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.xy);
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;

    if (pix.x >= width || pix.y >= height)
    {
        return;
    }

    if (globalUniform.fltEnable.x < 0.5)
    {
        q2CompositeUnfiltered(pix, width, height);
        return;
    }

    const float3 diffuse = q2AntiFirefly(pix, texelFetchUnfilteredDirect(pix), framebufUnfilteredDirect_Sampled, width, height);
    const float3 rawDirectSpec = texelFetchUnfilteredSpecular(pix);
    const float3 directSpec = isfinite(getLuminance(rawDirectSpec)) ? max(rawDirectSpec, (float3)0.0) : (float3)0.0;
    const float3 indirectSpec = q2FetchReconstructedIndirectSpec(pix, width, height, false);
    const float3 specular = directSpec + indirectSpec;
    const SH indir = q2FetchReconstructedIndirectSH(pix, width, height);

    const float3 hf = diffuse * Q2_STORAGE_SCALE_HF;
    framebufQ2ColorHF[pix] = (uint4)q2PackRGBE(hf);

    // SPEC: demodulated specular irradiance, scaled for storage
    const float3 spec = specular * Q2_STORAGE_SCALE_SPEC;
    framebufQ2ColorSpec[pix] = (uint4)q2PackRGBE(spec);

    // LF: indirect diffuse SH converted to the YCoCg luma-SH + chroma format
    const Q2SH lf = q2IrradianceToSH(indir.r, indir.g, indir.b);
    framebufQ2ColorLF_SH[pix] = lf.shY * Q2_STORAGE_SCALE_LF;
    framebufQ2ColorLF_COCG[pix] = float4(lf.CoCg * Q2_LF_CHROMA_TO_RADIANCE * Q2_STORAGE_SCALE_LF, 0.0, 0.0);
}
