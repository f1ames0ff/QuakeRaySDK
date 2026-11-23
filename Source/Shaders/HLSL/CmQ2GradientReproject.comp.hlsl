// Copyright (C) 2018 Christoph Schied
// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/asvgf_gradient_reproject.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
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
// Ported from Q2RTX (GPL v2) shader/asvgf_gradient_reproject.comp, adapted to
// the qray framework. In every 3x3 stratum a single surface is matched to the
// previous frame (via the motion vector and depth/normal similarity) and the
// previous frame's HF/SPEC luminances are stored as the "gradient sample".
//
// HLSL counterpart of CmQ2GradientReproject.comp.
//
// Spellings that had to change:
//   * gl_GlobalInvocationID -> SV_DispatchThreadID, and the golden's
//     ivec2(gl_GlobalInvocationID) becomes int2(dispatchThreadID.xy)
//   * texelFetch(t, p, 0) -> t.Load(int3(p, 0)), imageStore(img, p, v) -> img[p] = v
//   * vec2(0), vec2(0.5), vec4(0), uvec4(0) and uvec4(x) -> (float2)0, (float2)0.5, (float4)0,
//     (uint4)0 and (uint4)x: HLSL has no one argument vector constructor and a cast is the same
//     broadcast
//   * int(x) -> (int)x and uint(x) -> (uint)x, the same truncating conversion. ivec2 of a float
//     expression, ivec2(floor(...)) and ivec2(offx, offy), keeps its truncation as int2(...)
//   * all(equal(a, b)) -> all(a == b): HLSL compares the two int2 with the operator and hands the
//     bool vector to all()
//   * the golden's uint bit masks Q2_STRATUM_OFFSET_MASK keep their spelling; the conversion of
//     the shifted uint to the int2 components is the same wrap the golden does
//
// What did not change: the two nested loops over the 3x3 stratum, the reprojection through
// q2CheckerToFlat/q2FlatToChecker, the depth and normal comparisons of the surface matching, the
// brightest-sample rule of the stratum, the busy marker and the stratum position packed into
// gradient_idx, and every store of the previous frame's surface data.

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "Q2Asvgf.hlsli"

// The GLSL original declares the workgroup size here; in HLSL it is an attribute of the
// entry point instead, so the shader itself carries [numthreads(16, 16, 1)].

bool q2IsPrevGradientSample(const int2 pp, const int width, const int height)
{
    if (!q2InGradBounds(pp, width, height))
    {
        return false;
    }

    const int2 pos_grad_prev = pp / Q2_GRAD_DWN;
    const uint prev_grad_sample_pos = framebufQ2GradSmplPos_Prev_Sampled.Load(int3(pos_grad_prev, 0)).x;
    if ((prev_grad_sample_pos & (1u << 31)) == 0u)
    {
        return false;
    }

    const int2 stratum_prev = int2(
        (prev_grad_sample_pos >> (Q2_STRATUM_OFFSET_SHIFT * 0)) & Q2_STRATUM_OFFSET_MASK,
        (prev_grad_sample_pos >> (Q2_STRATUM_OFFSET_SHIFT * 1)) & Q2_STRATUM_OFFSET_MASK);
    return all(pos_grad_prev * Q2_GRAD_DWN + stratum_prev == pp);
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pos_grad = int2(dispatchThreadID.xy);

    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    const int grad_width = width / Q2_GRAD_DWN;
    const int grad_height = height / Q2_GRAD_DWN;

    if (pos_grad.x >= grad_width || pos_grad.y >= grad_height)
    {
        return;
    }

    const float inv_width = 1.0 / globalUniform.renderWidth;
    const float inv_height = 1.0 / globalUniform.renderHeight;
    const bool halfResIndir = (globalUniform.giBounceRays.x >= 0.25 && globalUniform.giBounceRays.x < 0.75);
    const int currTracedParity = (int)(globalUniform.frameId & 1u);
    const int prevTracedParity = currTracedParity ^ 1;

    bool found = false;
    int2 found_offset = (int2)0;
    int2 found_pos_prev = (int2)0;
    float2 found_prev_lum = (float2)0;

    for (int offy = 0; offy < Q2_GRAD_DWN && globalUniform.restirParams.z == 0u; offy++)
    {
        for (int offx = 0; offx < Q2_GRAD_DWN; offx++)
        {
            int2 p = pos_grad * Q2_GRAD_DWN + int2(offx, offy);
            if (halfResIndir && ((p.y & 1) != currTracedParity))
            {
                continue;
            }

            // Reproject the current pixel into the previous frame
            float4 motion = framebufMotion_Sampled.Load(int3(p, 0));
            const bool is_checkerboarded_surface = framebufThroughput_Sampled.Load(int3(p, 0)).a > 0.5;
            int2 pp;
            if (!is_checkerboarded_surface)
            {
                int2 p_flat = q2CheckerToFlat(p, width);
                float2 pos_prev_flat = ((float2)p_flat + (float2)0.5) * float2(inv_width, inv_height) + motion.xy;
                int2 pp_flat = int2(floor(pos_prev_flat * float2(width, height)));

                if (pp_flat.x < 0 || pp_flat.x >= width || pp_flat.y < 0 || pp_flat.y >= height)
                {
                    continue;
                }

                pp = q2FlatToChecker(pp_flat, width);
            }
            else
            {
                int field_left = 0;
                int field_right = width / 2;
                if (p.x >= field_right)
                {
                    field_left = field_right;
                    field_right = width;
                }

                float2 pos_prev = ((float2(p) + (float2)0.5) * float2(inv_width * 2.0, inv_height) + motion.xy) * float2(width / 2, height);
                pp = int2(floor(pos_prev));

                if (pp.x < field_left || pp.x >= field_right || pp.y < 0 || pp.y >= height)
                {
                    continue;
                }
            }

            if (halfResIndir && ((pp.y & 1) != prevTracedParity))
            {
                pp.y ^= 1;
                if (pp.y >= height)
                {
                    continue;
                }
            }

            // Don't reuse the same pixel if it was a gradient sample on the
            // previous frame (carrying the same RNG forward would add bias).
            if (q2IsPrevGradientSample(pp, width, height))
            {
                continue;
            }

            // Surface matching: cluster, depth and geometric normal similarity.
            uint cluster_curr = framebufQ2Cluster.Load(p).x;
            uint cluster_prev = framebufQ2Cluster_Prev_Sampled.Load(int3(pp, 0)).x;
            float depth_curr = framebufQ2ViewDepth_Sampled.Load(int3(p, 0)).r;
            float depth_prev = framebufQ2ViewDepth_Prev_Sampled.Load(int3(pp, 0)).r;
            float3 geo_normal_curr = texelFetchNormalGeometry(p);
            float3 geo_normal_prev = texelFetchNormalGeometry_Prev(pp);

            const float motion_z = (depth_curr < 0.0) ? -motion.z : motion.z;
            float dist_depth = abs(depth_curr - depth_prev + motion_z) / abs(depth_curr);
            float dot_geo_normals = dot(geo_normal_curr, geo_normal_prev);

            if (cluster_curr == cluster_prev && dist_depth < 0.1 && dot_geo_normals > 0.9)
            {
                const float3 albedo_curr = framebufAlbedo_Sampled.Load(int3(getRegularPixFromCheckerboardPix(p), 0)).rgb;
                const float3 albedo_prev = framebufAlbedo_Prev_Sampled.Load(int3(getRegularPixFromCheckerboardPix(pp), 0)).rgb;
                if (any(abs(albedo_curr - albedo_prev) > 0.01))
                {
                    continue;
                }

                float3 prev_hf = q2UnpackRGBE(framebufQ2ColorHF_Prev_Sampled.Load(int3(pp, 0)).r);
                float3 prev_spec = q2UnpackRGBE(framebufQ2ColorSpec_Prev_Sampled.Load(int3(pp, 0)).r);
                float2 prev_lum = float2(getLuminance(prev_hf), getLuminance(prev_spec));

                // Pick the brightest pixel in the stratum: avoids bright trails
                // when a light moves (a random penumbra pixel would miss it).
                if (prev_lum.x + prev_lum.y > found_prev_lum.x + found_prev_lum.y)
                {
                    found_prev_lum = prev_lum;
                    found_offset = int2(offx, offy);
                    found_pos_prev = pp;
                    found = true;
                }
            }
        }
    }

    if (!found)
    {
        framebufQ2GradSmplPos[pos_grad] = (uint4)0;
        framebufQ2GradHFSpecPing[pos_grad] = (float4)0;
        return;
    }

    // busy marker + position of the sample inside the stratum (current frame)
    uint gradient_idx =
          (1u << 31)
        | ((uint)found_offset.x << (Q2_STRATUM_OFFSET_SHIFT * 0))
        | ((uint)found_offset.y << (Q2_STRATUM_OFFSET_SHIFT * 1));

    framebufQ2GradSmplPos[pos_grad] = (uint4)gradient_idx;
    framebufQ2GradHFSpecPing[pos_grad] = float4(found_prev_lum, 0.0, 0.0);

    const int2 ipos = pos_grad * Q2_GRAD_DWN + found_offset;

    framebufQ2Cluster[ipos] =
               framebufQ2Cluster_Prev_Sampled.Load(int3(found_pos_prev, 0));

    framebufQ2RngSeed[ipos] =
               framebufQ2RngSeed_Prev_Sampled.Load(int3(found_pos_prev, 0));

    // Preserve current-frame shading normal & MR across direct/indirect gradient tracing,
    // then restore them in CmQ2GradientImg before temporal filtering & compositing.
    const uint curr_normal_enc = framebufNormal[ipos].r;
    const float2 curr_mr = framebufMetallicRoughness[ipos].xy;
    framebufQ2BaseColor[ipos] = float4(
        (float)(curr_normal_enc & 0xFFu),
        (float)((curr_normal_enc >> 8) & 0xFFu),
        (float)((curr_normal_enc >> 16) & 0xFFu),
        (float)((curr_normal_enc >> 24) & 0xFFu));
    framebufQ2Metallic[ipos] = float4(curr_mr, 0.0, 0.0);
    framebufQ2GradSavedSurfPos[pos_grad] = framebufSurfacePosition[ipos];
    framebufQ2GradSavedViewDir[pos_grad] = framebufViewDirection[ipos];

    framebufNormal[ipos] =
               framebufNormal_Prev_Sampled.Load(int3(found_pos_prev, 0));
    framebufMetallicRoughness[ipos] =
               framebufMetallicRoughness_Prev_Sampled.Load(int3(found_pos_prev, 0));
    framebufSurfacePosition[ipos] =
               framebufSurfacePosition_Prev_Sampled.Load(int3(found_pos_prev, 0));

    float4 view_direction = framebufViewDirection_Prev_Sampled.Load(int3(found_pos_prev, 0));
    framebufViewDirection[ipos] = view_direction;
}
