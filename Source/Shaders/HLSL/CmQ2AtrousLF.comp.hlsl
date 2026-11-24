// Copyright (C) 2018 Christoph Schied
// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/asvgf_lf.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
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
// Ported from Q2RTX (GPL v2) shader/asvgf_lf.comp.
//
// HLSL counterpart of CmQ2AtrousLF.comp. Pinned by the pair GLSL/CmQ2AtrousLF.comp <->
// this file, which pulls in Q2Asvgf.hlsli and pins it through the a-trous reads of the pass.
//
// Spellings that had to change:
//   * layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in
//     -> [numthreads(16, 16, 1)] on main
//   * gl_GlobalInvocationID -> SV_DispatchThreadID. HLSL has no global builtin and only the
//     entry point can receive a semantic, so filter_image and deflicker_image take the
//     dispatchThreadID they used to read as their first parameter, and each of the two keeps
//     its golden line int2 ipos_lowres = int2(dispatchThreadID.xy)
//   * layout(push_constant, std140) uniform IterationInfo { uint iteration; } push
//     -> struct IterationInfo_BT { uint iteration; }; [[vk::push_constant]]
//     ConstantBuffer<IterationInfo_BT> push, the same single uint at offset 0 on both halves
//   * ivec2/vec2/vec3/vec4 -> int2/float2/float3/float4
//   * texture2D img_shY / texture2D img_CoCg -> RWTexture2D<float4>, the storage image of the
//     rgba16f and the rg16f LF pair, and texelFetch(img, p, 0) -> img.Load(p). The ping/pong
//     pair is read through its storage images because this pass also writes them (A4.5); the
//     GLSL half selects the pair with one flag instead of taking the two parameters, because
//     glslang rejects an image format qualifier on a function parameter.
//     Q2_STORE_SH is the macro of Q2Asvgf.hlsli and keeps its two stores, shY first
//   * any(lessThan(a, b)) -> any(a < b) and any(greaterThanEqual(a, b)) -> any(a >= b): the
//     HLSL comparison operators are already componentwise and hand any() a bool vector, and
//     glslc lowers both spellings to the same OpSLessThan/OpSGreaterThanEqual plus OpAny
//   * int(x) / float(x) -> (int)x / (float)x, the C style casts that truncate toward zero
//     exactly as the GLSL constructors do. On either side of a shift the golden's
//     int(1u << (push.iteration - 1)) therefore stays an unsigned shift, converted afterwards
//   * vec3(1.0) / vec2(0.0) / ivec2(1) style splats -> (float3)1.0 / (float2)0.0 / (int2)1:
//     dxc refuses to widen a single scalar through a constructor, for an integer vector as
//     well ("too few elements in vector initialization"), and a C style cast is the same
//     broadcast the GLSL constructor performed
//
// What did not change: every operand order, every comparison, abs, exp, pow, dot, clamp, max
// and the ternary of the golden; the Q2_WAVELET_KERNEL read with two absolute loop indices;
// the two loops and their bounds; the checkerboard field boundaries; the switch over
// push.iteration with its four arms and the Q2_STORE_SH pair of each arm; the
// Q2_FLT_ATROUS_DEFLICKER_LF, Q2_FLT_DEFLICKER_DEPTH and Q2_FLT_DEFLICKER_NORMAL guards of
// deflicker_image, including the num_neighbors == 0 early out that keeps the mean from
// dividing by zero.

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "Q2Asvgf.hlsli"

// A-trous wavelet filter for the LF (indirect diffuse) channel, computed at
// 1/3 resolution. A simple depth/normal-guided bilateral blur over the SH data.

struct IterationInfo_BT
{
    uint iteration;
};

[[vk::push_constant]] ConstantBuffer<IterationInfo_BT> push;

Q2SH q2LoadSH(RWTexture2D<float4> img_shY, RWTexture2D<float4> img_CoCg, int2 p)
{
    Q2SH r;
    r.shY = img_shY.Load(p);
    r.CoCg = img_CoCg.Load(p).xy;
    return r;
}

void filter_image(
    const uint3 dispatchThreadID,
    RWTexture2D<float4> img_lf_shY,
    RWTexture2D<float4> img_lf_CoCg,
    out Q2SH filtered_lf)
{
    int2 ipos_lowres = int2(dispatchThreadID.xy);
    int2 ipos_hires = ipos_lowres * Q2_GRAD_DWN + (int2)1;

    // Load the color of the target low-res pixel
    Q2SH color_center_lf = q2LoadSH(img_lf_shY, img_lf_CoCg, ipos_lowres);

    if(Q2_ATROUS_ITERATIONS_LF <= push.iteration)
    {
        filtered_lf = color_center_lf;
        return;
    }

    // Load the parameters of the anchor pixel
    float3 geo_normal_center = texelFetchNormalGeometry(ipos_hires);
    float depth_center = framebufQ2ViewDepth_Sampled.Load(int3(ipos_hires, 0)).r;
    float fwidth_depth = framebufDepthGrad_Sampled.Load(int3(ipos_hires, 0)).r;

    const int step_size = (int)(1u << (push.iteration - 1));

    Q2SH sum_color_lf = color_center_lf;

    float sum_w_lf = 1.0;

    // Boundaries for the checkerboard field, either left or right half of the screen
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    int field_left = 0;
    int field_right = width / 2;
    if(ipos_hires.x >= field_right)
    {
        field_left = field_right;
        field_right = width;
    }

    // Zero-mean centrosymmetric 2x2 Bayer tap rotation on coarse LF steps prevents
    // blocky square grid ringing without temporal jitter.
    const int lf_rot_hi = (step_size >= 2) ? max(1, step_size / 2) : 0;
    const int lf_rot_lo = (step_size >= 4) ? max(1, step_size / 4) : 0;
    const uint lf_tile_hash = (((uint(ipos_lowres.x) & 1u) | ((uint(ipos_lowres.y) & 1u) << 1u)) + push.iteration) & 3u;
    int lf_rot = 0;
    if (lf_rot_hi > 0)
    {
        if (lf_tile_hash == 0u)      lf_rot =  lf_rot_hi;
        else if (lf_tile_hash == 1u) lf_rot = -lf_rot_hi;
        else if (lf_tile_hash == 2u) lf_rot =  lf_rot_lo;
        else                         lf_rot = -lf_rot_lo;
    }
    const float lf_eff_step = sqrt((float)(step_size * step_size + lf_rot * lf_rot));

    // Compute the weighted average of color from a sparse 3x3 pattern around the target pixel
    const int r = 1;
    for(int yy = -r; yy <= r; yy++)
    {
        for(int xx = -r; xx <= r; xx++)
        {
            int2 p_lowres = ipos_lowres + int2(xx * step_size + yy * lf_rot, yy * step_size - xx * lf_rot);
            int2 p_hires = p_lowres * Q2_GRAD_DWN + (int2)1;

            if(xx == 0 && yy == 0)
                continue;

            // Skip out-of-range neighbours instead of fetching them with a zero weight:
            // texelFetch with out-of-range integer coordinates is undefined, and a NaN
            // result would still poison the accumulated sum (NaN * 0 is NaN).
            if(any(p_lowres < (int2)0)
                    || any(p_hires < int2(field_left, 0))
                    || any(p_hires >= int2(field_right, height)))
                continue;

            float w = 1.0;

            // Use geometric normals so that we can blur over larger areas.
            // The lighting detail will be partially preserved by spherical harmonics.
            float3 geo_normal = texelFetchNormalGeometry(p_hires);

            float depth = framebufQ2ViewDepth_Sampled.Load(int3(p_hires, 0)).r;

            float dist_z = abs(depth_center - depth) * fwidth_depth * Q2_FLT_ATROUS_DEPTH;
            w *= exp(-dist_z / (lf_eff_step * (float)Q2_GRAD_DWN));
            w *= Q2_WAVELET_KERNEL[abs(xx)][abs(yy)];

            float w_lf = w;

            if(Q2_FLT_ATROUS_NORMAL_LF > 0)
            {
                float GNdotGN = max(0.0, dot(geo_normal_center, geo_normal));
                w_lf *= pow(GNdotGN, Q2_FLT_ATROUS_NORMAL_LF);
            }

            Q2SH c_lf = q2LoadSH(img_lf_shY, img_lf_CoCg, p_lowres);

            const float center_lum_safe = max(color_center_lf.shY.w, 1e-4);
            const float lum_ratio = max(c_lf.shY.w, 0.0) / center_lum_safe;
            // Suppress bright-outlier samples to prevent light leaking and firefly smearing,
            // while avoiding 0/0 NaNs when the center low-res pixel is dark.
            if(push.iteration == 3)
                w_lf *= clamp(1.5 - lum_ratio * 0.25, 0.0, 1.0);
            else if(color_center_lf.shY.w > 1e-4)
                w_lf *= clamp(2.0 - lum_ratio * 0.0625, 0.0, 1.0);

            q2AccumulateSH(sum_color_lf, c_lf, w_lf);
            sum_w_lf += w_lf;
        }
    }

    filtered_lf.shY = sum_color_lf.shY / sum_w_lf;
    filtered_lf.CoCg = sum_color_lf.CoCg / sum_w_lf;
}

// Enabled on iteration 0, before the first a-trous step, which is where Q2RTX runs it: an
// isolated spike in the temporal result gets cut there instead of being smeared over its
// neighbours by the four iterations that follow. It was disabled in d8460b9f, where it
// darkened the LF signal around dark silhouettes (screen frame, weapon); the three problems
// it used to have are addressed below:
//   1. neighbours are now validated by depth and geometry normal, like filter_image, so
//      unrelated geometry (a dark weapon in front of a lit wall) cannot change the result;
//   2. Q2_FLT_ATROUS_DEFLICKER_LF is a ratio against the mean of the valid neighbours, so
//      it has to stay >= 1 to leave flat regions untouched; it is 2.0, as in Q2RTX
//      (flt_atrous_deflicker_lf), where 0.75 darkened flat regions;
//   3. the fetches below are bounded to this pixel's checkerboard field and the mean is
//      taken over the neighbours that passed validation, so there is no division by zero.
// The fireflies this pass was re-enabled for turned out to come from the sun's contribution
// to an indirect bounce (rt_sky_sun_bounce_scale / rt_sky_sun_bounce_range), not from the LF channel.
void deflicker_image(
    const uint3 dispatchThreadID,
    RWTexture2D<float4> img_lf_shY,
    RWTexture2D<float4> img_lf_CoCg,
    out Q2SH filtered_lf)
{
    int2 ipos_lowres = int2(dispatchThreadID.xy);
    int2 ipos_hires = ipos_lowres * Q2_GRAD_DWN + (int2)1;

    Q2SH color_center_lf = q2LoadSH(img_lf_shY, img_lf_CoCg, ipos_lowres);

    const float lum_center = color_center_lf.shY.w;
    if(lum_center <= 0.0)
    {
        filtered_lf = color_center_lf;
        return;
    }

    // The anchor is the only pixel of the low-res entry that has depth and normal.
    float3 geo_normal_center = texelFetchNormalGeometry(ipos_hires);
    float depth_center = framebufQ2ViewDepth_Sampled.Load(int3(ipos_hires, 0)).r;

    // Boundaries for the checkerboard field, either left or right half of the screen
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    int field_left = 0;
    int field_right = width / 2;
    if(ipos_hires.x >= field_right)
    {
        field_left = field_right;
        field_right = width;
    }

    float sum_lum = 0.0;
    float max_lum1 = -1e30;
    int num_neighbors = 0;

    const int r = 1;
    for(int yy = -r; yy <= r; yy++)
    {
        for(int xx = -r; xx <= r; xx++)
        {
            if(xx == 0 && yy == 0)
                continue;

            int2 p_lowres = ipos_lowres + int2(xx, yy);
            int2 p_hires = p_lowres * Q2_GRAD_DWN + (int2)1;

            // Skip out-of-range neighbours instead of fetching them: texelFetch with
            // out-of-range integer coordinates is undefined, and the other half of the
            // rows belongs to the checkerboard field that this invocation does not own.
            if(any(p_hires < int2(field_left, 0))
                    || any(p_hires >= int2(field_right, height)))
                continue;

            float depth = framebufQ2ViewDepth_Sampled.Load(int3(p_hires, 0)).r;
            float3 geo_normal = texelFetchNormalGeometry(p_hires);

            float dist_depth = abs(depth_center - depth) / max(abs(depth_center), 1e-6);
            float dot_geo_normals = dot(geo_normal_center, geo_normal);

            if(dist_depth >= Q2_FLT_DEFLICKER_DEPTH || dot_geo_normals <= Q2_FLT_DEFLICKER_NORMAL)
                continue;

            const float n_lum = q2LoadSH(img_lf_shY, img_lf_CoCg, p_lowres).shY.w;
            sum_lum += n_lum;
            max_lum1 = max(max_lum1, n_lum);
            num_neighbors++;
        }
    }

    // Nothing to compare against: leave the sample untouched instead of dividing by zero.
    if(num_neighbors == 0)
    {
        filtered_lf = color_center_lf;
        return;
    }

    // Trimmed mean dropping the single brightest neighbour so adjacent emissive firefly pairs
    // cannot shield each other without darkening sparse valid indirect illumination.
    float trimmed_sum = sum_lum;
    int trimmed_count = num_neighbors;
    if(num_neighbors >= 3)
    {
        trimmed_sum -= max_lum1;
        trimmed_count -= 1;
    }

    float limit = (max(0.0, trimmed_sum) / (float)max(trimmed_count, 1)) * Q2_FLT_ATROUS_DEFLICKER_LF;
    if(lum_center > limit)
    {
        float ratio = limit / lum_center;
        color_center_lf.shY *= ratio;
        color_center_lf.CoCg *= ratio;
    }

    filtered_lf = color_center_lf;
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    int2 ipos = int2(dispatchThreadID.xy);

    // Reject invocations without a valid high-res anchor. For a render size of 3k + 1 the
    // old test admitted one extra low-res column/row whose anchor texel lies outside the
    // image, and every fetch with that anchor is undefined.
    if(any(ipos * Q2_GRAD_DWN + (int2)1 >=
                int2(globalUniform.renderWidth, globalUniform.renderHeight)))
        return;

    Q2SH filtered_lf;

    // Read the ping/pong LF pair through the storage images, not the sampled views: this
    // pass also writes both images of the pair (each iteration reads the field it just
    // wrote), and binding both views of one image in one set makes the SRV and the UAV
    // disagree about the image layout (A4.5).
    switch(push.iteration)
    {
    case 0:
        deflicker_image(dispatchThreadID, framebufQ2AtrousPingLF_SH, framebufQ2AtrousPingLF_COCG, filtered_lf);
        Q2_STORE_SH(framebufQ2AtrousPongLF_SH, framebufQ2AtrousPongLF_COCG, ipos, filtered_lf);
        break;
    case 1:
        filter_image(dispatchThreadID, framebufQ2AtrousPongLF_SH, framebufQ2AtrousPongLF_COCG, filtered_lf);
        Q2_STORE_SH(framebufQ2AtrousPingLF_SH, framebufQ2AtrousPingLF_COCG, ipos, filtered_lf);
        break;
    case 2:
        filter_image(dispatchThreadID, framebufQ2AtrousPingLF_SH, framebufQ2AtrousPingLF_COCG, filtered_lf);
        Q2_STORE_SH(framebufQ2AtrousPongLF_SH, framebufQ2AtrousPongLF_COCG, ipos, filtered_lf);
        break;
    case 3:
        filter_image(dispatchThreadID, framebufQ2AtrousPongLF_SH, framebufQ2AtrousPongLF_COCG, filtered_lf);
        Q2_STORE_SH(framebufQ2AtrousPingLF_SH, framebufQ2AtrousPingLF_COCG, ipos, filtered_lf);
        break;
    }
}
