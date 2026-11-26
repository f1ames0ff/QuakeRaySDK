// Copyright (C) 2018 Christoph Schied
// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/asvgf_temporal.comp from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
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
// Ported from Q2RTX (GPL v2) shader/asvgf_temporal.comp. Adapted to the qray
// framework: gradients come from the Q2RTX-style gradient pipeline
// (CmQ2GradientReproject + CmQ2GradientImg + CmQ2GradientAtrous),
// depth derivative from DepthGrad, split surfaces via Throughput.a.
//


// HLSL counterpart of CmQ2Temporal.comp.
//
// Spellings that had to change:
//   * layout(local_size_x = GROUP_SIZE, local_size_y = GROUP_SIZE, local_size_z = 1) ->
//     [numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
//   * gl_GlobalInvocationID -> SV_DispatchThreadID, gl_LocalInvocationID -> SV_GroupThreadID,
//     gl_LocalInvocationIndex -> SV_GroupIndex, gl_WorkGroupID -> SV_GroupID. The four are entry
//     point semantics in HLSL, so preload and get_shared_data receive the ones they used to read
//     globally as parameters (groupID and linearIdx, groupThreadID); the arithmetic that reads
//     them is unchanged, ivec2(gl_WorkGroupID) is spelled int2(groupID.xy) and ivec2(gl_
//     LocalInvocationID) is spelled int2(groupThreadID.xy)
//   * shared -> groupshared, barrier() -> GroupMemoryBarrierWithGroupSync()
//   * the two texture2D parameters of q2LoadSH become Texture2D<float4>: the golden's q2LoadSH is
//     this file's own helper (the header recommends it), and neither half of the LF pair needs a
//     format tag to be loaded
//   * texelFetch(t, p, 0) -> t.Load(int3(p, 0)), imageStore(img, p, v) -> img[p] = v
//   * mix(a, b, s) -> lerp(a, b, s) at every one of the eleven sites, the same definition under
//     the HLSL name, as in BRDF.hlsli
//   * fract(x) -> frac(x): the x - floor(x) intrinsic under its HLSL name, as in
//     CmLuminanceHistogram.comp
//   * vecN(scalar) -> (floatN)scalar and uvec2(scalar) -> (uint2)scalar, because a scalar does
//     not broadcast through a constructor in HLSL: vec2(0.5), vec2(0.0) and vec3(0.0) and
//     vec4(0.0) of the golden become (float2)0.5, (float2)0.0, (float3)0.0 and (float4)0.0,
//     ivec2(0) becomes (int2)0, ivec2(FILTER_RADIUS) becomes (int2)FILTER_RADIUS, and
//     uvec2(1) becomes (uint2)1. The vecN(...) calls that collect several components keep their
//     argument lists and only change the type name (vec4(spatial_moments_hf, 1, 1) becomes
//     float4(spatial_moments_hf, 1, 1), and the two int literals still convert to floats)
//   * packHalf2x16(v) and unpackHalf2x16(u) have no single intrinsic in HLSL: f32tof16 converts
//     one float at a time and f16tof32 reads one half at a time, so the two helpers below
//     assemble the pair with the shift and the mask of the golden's layout (the low half first,
//     the high half second). dxc lowers the scalar f32tof16 to PackHalf2x16 of (x, 0) and the
//     scalar f16tof32 to UnpackHalf2x16(u).x, so the pair of them is exactly the instruction the
//     GLSL builtin emits. Neither compiler folds those two instructions, so the site is proven
//     at operand level and not on a folded number. The surrounding vec2/int2 constructors keep
//     their component lists: the s_normal_lum pair is
//     uint2(packHalf2x16(normal.xy), packHalf2x16(float2(normal.z, getLuminance(color_hf)))) and
//     the three unpack sites are float3(unpackHalf2x16(...), unpackHalf2x16(...).x) and
//     unpackHalf2x16(...).y
//   * uint(...) / int(...) / float(...) constructors -> C style casts with the same truncation:
//     int(floor(...)), int(globalUniform.renderWidth), float(linear_idx), float(SHARED_SIZE)
//     and uint(linear_idx) are the ones of this file
//   * uvec4(q2PackRGBE(...)) -> (uint4)q2PackRGBE(...): the one argument constructor of the
//     golden is the broadcast that a scalar cast performs in HLSL. uvec2(...) and uint2(...)
//     constructors of several components keep their argument lists
//   * Q2_STORE_SH and the Q2_* macros are used as they are; the macro body lives in Q2Asvgf.hlsli
//
// What did not change: the four shared buffers with their sizes and types, the GROUP_SIZE /
// FILTER_RADIUS / SHARED_SIZE values, every operand order and every clamp, max, min, pow, abs,
// dot, floor, square and length of the golden, the reprojection of both checkerboard modes with
// its q2CheckerToFlat / q2FlatToChecker conversions and the two integer divisions width / 2 and
// width / 2, the 2x2 bilinear tap loop with its four weights and the off[] table, the history
// validation (relative depth difference, both dot products, the 0.25 of the secondary surfaces),
// the two accumulation branches and their 1e-6 guards, the spatial 3x3 moment window, the
// antilag alphas and the history lengths with their pow(..., 10) and 256.0 clamp, the three
// alpha blends, the two fallback branches, the six stores, the 1/3 resolution downsample with
// the bilateral 3x3 window, and the final Q2_STORE_SH of the LF channel.
//
// Known caveat of the pair, the same one the fleet recorded for EfCommon.hlsli's divisions:
// t = ((float)linear_idx + 0.5) / (float)SHARED_SIZE keeps OpFDiv in the golden blob, which
// GenerateShaders.py builds with plain glslc, while dxc optimizes by default and rewrites the
// division into OpFMul by 0.0588235296 (measured: plain glslc on the golden keeps
// OpFDiv %float_17, the port has OpFMul %float_0_0588235296; with -O on both sides, as the
// property checker runs them, the golden rewrites it too and the two agree). The constant is the
// correctly rounded fl(1/17), but x * fl(1/17) and x / 17 differ by one ulp for many inputs
// (e.g. 44.538719177246094 / 17 = 2.619924545288086 against * = 2.619924783706665), so the two
// blobs can disagree by one ulp on t, which is enough to move a shared memory preload tap to a
// neighbouring texel. The port keeps the golden's spelling; the deviation is bounded by that
// one tap and cannot be removed without writing a hand-made reciprocal into the port.

#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "Q2Asvgf.hlsli"

#define GROUP_SIZE_X 24
#define GROUP_SIZE_Y 12
// spatially compute variance in a 3x3 (radius = 1) or a 5x5 (radius = 2) window
#define FILTER_RADIUS 1
// size of the shared memory copies of color, depth, and normals
#define SHARED_SIZE_X (GROUP_SIZE_X + FILTER_RADIUS * 2)
#define SHARED_SIZE_Y (GROUP_SIZE_Y + FILTER_RADIUS * 2)
#define LOWRES_SIZE_X (GROUP_SIZE_X / Q2_GRAD_DWN)
#define LOWRES_SIZE_Y (GROUP_SIZE_Y / Q2_GRAD_DWN)

// Store some color data in shared memory for efficient access and for downsampling
groupshared uint2 s_normal_lum[SHARED_SIZE_Y][SHARED_SIZE_X];
groupshared float s_depth[SHARED_SIZE_Y][SHARED_SIZE_X];
groupshared float2 s_spec_lum_rough[SHARED_SIZE_Y][SHARED_SIZE_X];
groupshared float4 s_lf_shy[GROUP_SIZE_Y][GROUP_SIZE_X];
groupshared float2 s_lf_cocg[GROUP_SIZE_Y][GROUP_SIZE_X];
groupshared float s_depth_width[LOWRES_SIZE_Y][LOWRES_SIZE_X];

// The pair packing of the golden, assembled from the two scalar conversions because HLSL has no
// intrinsic that packs two halves into one uint. The layout is the GLSL one: the first component
// in the low 16 bits, the second in the high 16 bits.
uint packHalf2x16(const float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

float2 unpackHalf2x16(const uint u)
{
    return float2(f16tof32(u & 0xFFFF), f16tof32(u >> 16));
}

Q2SH q2LoadSH(Texture2D<float4> img_shY, Texture2D<float4> img_CoCg, int2 p)
{
    Q2SH r;
    r.shY = img_shY.Load(int3(p, 0));
    r.CoCg = img_CoCg.Load(int3(p, 0)).xy;
    return r;
}

bool q2IsCurrGradientSample(const int2 p, const int width, const int height)
{
    if (!q2InGradBounds(p, width, height))
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

// Preload the color data into shared memory
void
preload(const uint3 groupID, const uint linearIdx)
{
    const int2 groupBase = int2(groupID.xy) * int2(GROUP_SIZE_X, GROUP_SIZE_Y) - FILTER_RADIUS;
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;

    // The size of these shared memory buffers is larger than the group size because we
    // use them for some spatial filtering. So a single load per thread is not enough.
    for(uint linear_idx = linearIdx; linear_idx < SHARED_SIZE_X * SHARED_SIZE_Y; linear_idx += GROUP_SIZE_X * GROUP_SIZE_Y)
    {
        int xx = (int)(linear_idx % SHARED_SIZE_X);
        int yy = (int)(linear_idx / SHARED_SIZE_X);

        int2 ipos = clamp(groupBase + int2(xx, yy), int2(0, 0),
                          int2(width - 1, height - 1));
        float depth = framebufQ2ViewDepth_Sampled.Load(int3(ipos, 0)).r;
        float3 normal = texelFetchNormal(ipos);
        float3 color_hf = q2UnpackRGBE(framebufQ2ColorHF_Sampled.Load(int3(ipos, 0)).r);
        float3 color_spec = q2UnpackRGBE(framebufQ2ColorSpec_Sampled.Load(int3(ipos, 0)).r);
        float rough = framebufMetallicRoughness_Sampled.Load(int3(ipos, 0)).y;
        const bool isGrad = (ipos.x >= 0 && ipos.y >= 0 && ipos.x < width && ipos.y < height)
                          && q2IsCurrGradientSample(ipos, width, height);

        s_normal_lum[yy][xx] = uint2(packHalf2x16(normal.xy), packHalf2x16(float2(normal.z, getLuminance(color_hf))));
        s_depth[yy][xx] = depth;
        s_spec_lum_rough[yy][xx] = float2(getLuminance(color_spec), rough + (isGrad ? 2.0 : 0.0));
    }
}

// Load the color and normal data from shared memory
void
get_shared_data(const uint3 groupThreadID, int2 offset, out float depth, out float3 normal, out float lum_hf)
{
    int2 addr = int2(groupThreadID.xy) + (int2)FILTER_RADIUS + offset;

    uint2 normal_lum = s_normal_lum[addr.y][addr.x];
    depth = s_depth[addr.y][addr.x];

    normal = float3(unpackHalf2x16(normal_lum.x), unpackHalf2x16(normal_lum.y).x);
    lum_hf = unpackHalf2x16(normal_lum.y).y;
}

void
get_shared_spec_data(const uint3 groupThreadID, int2 offset, out float lum_spec, out float rough, out bool is_grad)
{
    int2 addr = int2(groupThreadID.xy) + (int2)FILTER_RADIUS + offset;
    float2 spec_rough = s_spec_lum_rough[addr.y][addr.x];
    lum_spec = spec_rough.x;
    is_grad = (spec_rough.y >= 1.5);
    rough = is_grad ? (spec_rough.y - 2.0) : spec_rough.y;
}

bool
q2ProjectStaticMotion(const float3 worldPos, out float2 outMotion)
{
    const float4 viewCur  = mul(globalUniform.view, float4(worldPos, 1.0));
    const float4 viewPrev = mul(globalUniform.viewPrev, float4(worldPos, 1.0));
    const float4 clipCur  = mul(globalUniform.projection, viewCur);
    const float4 clipPrev = mul(globalUniform.projectionPrev, viewPrev);

    if (clipCur.w <= 1e-4 || clipPrev.w <= 1e-4)
    {
        outMotion = (float2)0.0;
        return false;
    }

    const float2 screenCur  = (clipCur.xy / clipCur.w) * 0.5 + 0.5;
    const float2 screenPrev = (clipPrev.xy / clipPrev.w) * 0.5 + 0.5;
    outMotion = screenPrev - screenCur;
    return true;
}

[numthreads(GROUP_SIZE_X, GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID,
          uint groupIndex : SV_GroupIndex, uint3 groupID : SV_GroupID)
{
    preload(groupID, groupIndex);
    GroupMemoryBarrierWithGroupSync();

    int2 ipos = int2(dispatchThreadID.xy);
    const int width = (int)globalUniform.renderWidth;
    const int height = (int)globalUniform.renderHeight;
    const float inv_width = 1.0 / globalUniform.renderWidth;
    const float inv_height = 1.0 / globalUniform.renderHeight;
    const bool inBounds = ipos.x < width && ipos.y < height;

    float gradDepth = 0.0;
    Q2SH out_color_lf = q2InitSH();
    float3 out_color_hf = (float3)0.0;
    float4 out_color_histlen_spec = (float4)0.0;
    float4 out_moments_histlen_hf = (float4)0.0;

    if (inBounds)
    {
        float4 motion = framebufMotion_Sampled.Load(int3(ipos, 0));
        gradDepth = framebufDepthGrad_Sampled.Load(int3(ipos, 0)).r;

        // Find out if this pixel belongs to a checkerboard-split-path surface (water/glass)
        const float wasSplit = framebufThroughput_Sampled.Load(int3(ipos, 0)).a;
        const bool is_checkerboarded_surface = wasSplit > 0.5;

        // On a single GPU we can access both checkerboard fields for higher sampling quality
        const bool sample_across_fields = !is_checkerboarded_surface;

        const int cb_half = width / 2;
        const int cb_left = (ipos.x < cb_half) ? 0 : cb_half;
        const int cb_right = (ipos.x < cb_half) ? cb_half : width;

        const float2 jitter_delta = float2(
            globalUniform.jitterX - globalUniform.jitterPrevX,
            globalUniform.jitterY - globalUniform.jitterPrevY);

        float2 pos_prev;
        if (sample_across_fields)
        {
            // Reprojection in flat-screen coordinates
            pos_prev = (float2(q2CheckerToFlat(ipos, width)) + (float2)0.5 + jitter_delta) + motion.xy * float2(width, height);
        }
        else
        {
            // Reprojection in checkerboarded coordinates
            pos_prev = (float2(ipos) + (float2)0.5 + float2(0.5 * jitter_delta.x, jitter_delta.y)) + motion.xy * float2(width / 2, height);
        }

        float motion_length = length(motion.xy * float2(width, height));

        // Load the parameters of the target pixel
        float depth_curr;
        float3 normal_curr;
        float lum_curr_hf;
        get_shared_data(groupThreadID, (int2)0, depth_curr, normal_curr, lum_curr_hf);

        float2 metal_rough = framebufMetallicRoughness_Sampled.Load(int3(ipos, 0)).xy;
        float shininess = clamp(2.0 / max(square(square(metal_rough.y)), 1e-6) - 2.0, 0.0, 32.0);

        float3 geo_normal_curr = texelFetchNormalGeometry(ipos);

        // Try to get the history sample for all channels, including HF and SPEC moments
        bool temporal_sample_valid_diff = false;
        bool temporal_sample_valid_spec = false;
        Q2SH temporal_color_lf = q2InitSH();
        float3 temporal_color_hf = (float3)0.0;
        float4 temporal_color_histlen_spec = (float4)0.0;
        float4 temporal_moments_histlen_hf = (float4)0.0;
        float2 temporal_moments_spec = (float2)0.0;

        // Virtual-point parallax reprojection for SPEC: reflect hit distance into virtual
        // reflected world point, blending back to surface motion for rough, curved, or
        // dynamically moving reflectors (disabled on sky and secondary refl/refr surfaces).
        const bool validPrimarySurface = (depth_curr > 1e-4) && (dot(geo_normal_curr, geo_normal_curr) > 0.5);
        const float3 surfPos = framebufSurfacePosition_Sampled.Load(int3(ipos, 0)).xyz;
        const float4 viewDirAndDist = framebufViewDirection_Sampled.Load(int3(ipos, 0));
        float hitDist = (viewDirAndDist.w >= 0.0) ? min(viewDirAndDist.w, Q2_MAX_VIEW_DIR_HIT_DIST) : -1.0;
        float3 viewDirCurr = safeNormalize(viewDirAndDist.xyz);
        if (validPrimarySurface && hitDist < 0.0)
        {
            float minValidDist = Q2_MAX_VIEW_DIR_HIT_DIST + 1.0;
            [unroll]
            for (int dy = -1; dy <= 1; dy += 2)
            {
                const int ny = ipos.y + dy;
                if (ny >= 0 && ny < height)
                {
                    const int2 np = int2(ipos.x, ny);
                    const float nDepth = framebufQ2ViewDepth_Sampled.Load(int3(np, 0)).r;
                    if (abs(depth_curr - nDepth) / max(abs(depth_curr), 1e-3) < 0.05
                        && dot(normal_curr, texelFetchNormal(np)) > 0.95)
                    {
                        const float4 nView = framebufViewDirection_Sampled.Load(int3(np, 0));
                        if (nView.w >= 0.0)
                        {
                            minValidDist = min(minValidDist, min(nView.w, Q2_MAX_VIEW_DIR_HIT_DIST));
                            if (dot(viewDirAndDist.xyz, viewDirAndDist.xyz) < 1e-6)
                            {
                                viewDirCurr = safeNormalize(nView.xyz);
                            }
                        }
                    }
                }
            }
            hitDist = (minValidDist <= Q2_MAX_VIEW_DIR_HIT_DIST) ? minValidDist : -1.0;
        }

        float normalVar = 0.0;
        const int2 cardOffsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        [unroll]
        for (int ci = 0; ci < 4; ci++)
        {
            const int2 cp = ipos + cardOffsets[ci];
            if (cp.x < cb_left || cp.x >= cb_right || cp.y < 0 || cp.y >= height)
            {
                continue;
            }
            float dCard;
            float3 nCard;
            float lumCard;
            get_shared_data(groupThreadID, cardOffsets[ci], dCard, nCard, lumCard);
            if (validPrimarySurface && abs(depth_curr - dCard) / max(abs(depth_curr), 1e-3) < 0.05)
            {
                normalVar = max(normalVar, 1.0 - dot(normal_curr, nCard));
            }
        }
        const float curvMetric = normalVar * square(max(globalUniform.renderHeight / 1080.0, 0.25));
        const float curvatureBlend = smoothstep(0.0008, 0.015, curvMetric);

        float dynamicObjBlend = 1.0;
        float2 staticSurfMotion;
        if (validPrimarySurface && q2ProjectStaticMotion(surfPos, staticSurfMotion))
        {
            const float objMotionPx = length((motion.xy - staticSurfMotion) * float2(width, height));
            dynamicObjBlend = saturate((objMotionPx - 0.5) / 2.0);
        }

        const float roughBlend = smoothstep(0.06, 0.38, metal_rough.y);
        float surface_blend = 1.0;
        float spec_virtual_weight = 0.0;
        float2 motion_spec_xy = motion.xy;
        float2 virtualMotion;
        if (validPrimarySurface && hitDist > 0.0
            && q2ProjectStaticMotion(surfPos + viewDirCurr * hitDist, virtualMotion))
        {
            surface_blend = max(roughBlend, max(curvatureBlend, dynamicObjBlend));
            spec_virtual_weight = 1.0 - surface_blend;
            motion_spec_xy = lerp(virtualMotion, motion.xy, surface_blend);
        }

        float2 pos_prev_spec;
        if (sample_across_fields)
        {
            pos_prev_spec = (float2(q2CheckerToFlat(ipos, width)) + (float2)0.5 + jitter_delta) + motion_spec_xy * float2(width, height);
        }
        else
        {
            pos_prev_spec = (float2(ipos) + (float2)0.5 + float2(0.5 * jitter_delta.x, jitter_delta.y)) + motion_spec_xy * float2(width / 2, height);
        }

        const float motion_z = (depth_curr < 0.0) ? -motion.z : motion.z;
        const float3 surf_view_dir = safeNormalize(viewDirAndDist.xyz);
        const float ndotv_geo = validPrimarySurface ? abs(dot(geo_normal_curr, surf_view_dir)) : 1.0;
        const float grazing_factor = validPrimarySurface ? smoothstep(0.45, 0.10, ndotv_geo) : 0.0;
        // Cap grazing depth tolerance strictly below 0.50 so genuine coplanar depth discontinuities (e.g. 2.4x step) stay rejected.
        const float grazing_depth_tol = lerp(0.10, 0.35, grazing_factor);

        if (globalUniform.restirParams.z == 0u)
        {
            float temporal_sum_w_diff = 0.0;
            float temporal_sum_w_spec = 0.0;

            float2 pos_ld = floor(pos_prev - (float2)0.5);
            float2 subpix = frac(pos_prev - (float2)0.5 - pos_ld);

            int field_left = 0;
            int field_right = sample_across_fields ? width : (width / 2);
            if (!sample_across_fields && ipos.x >= width / 2)
            {
                field_left = field_right;
                field_right = width;
            }

            // Bilinear/bilateral filter (diffuse channels at surface motion)
            const int2 off[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
            float w[4] = {
                (1.0 - subpix.x) * (1.0 - subpix.y),
                (subpix.x      ) * (1.0 - subpix.y),
                (1.0 - subpix.x) * (subpix.y      ),
                (subpix.x      ) * (subpix.y      )
            };
            for(int i = 0; i < 4; i++)
            {
                int2 p = int2(pos_ld) + off[i];

                if(p.x < field_left || p.x >= field_right || p.y >= height || p.y < 0)
                    continue;

                if (sample_across_fields)
                {
                    // p is in flat coordinates - translate back into checkerboarded coordinates
                    p = q2FlatToChecker(p, width);
                }

                float depth_prev = framebufQ2ViewDepth_Prev_Sampled.Load(int3(p, 0)).r;
                float3  normal_prev = texelFetchNormal_Prev(p);
                float3  geo_normal_prev = texelFetchNormalGeometry_Prev(p);
                float3  surf_pos_prev = framebufSurfacePosition_Prev_Sampled.Load(int3(p, 0)).xyz;

                // Q2ViewDepth is negative for reflection/refraction surfaces, and the
                // stored motion depth delta (motion.z) is in the positive primary
                // space - flip its sign so the motion-compensated match stays exact.
                float dist_depth = abs(depth_curr - depth_prev + motion_z) / max(abs(depth_curr), 1e-4);
                float dot_normals = dot(normal_curr, normal_prev);
                float dot_geo_normals = dot(geo_normal_curr, geo_normal_prev);

                if(depth_curr < 0)
                {
                    // Reduce the filter sensitivity to depth for secondary surfaces,
                    // because reflection/refraction motion vectors are often inaccurate.
                    dist_depth *= 0.25;
                }

                const float diff_plane_dist = validPrimarySurface
                    ? (abs(dot(surfPos - surf_pos_prev, geo_normal_curr)) / max(abs(depth_curr), 0.25))
                    : 1.0;
                const bool depth_ok_diff = (dist_depth < 0.1) ||
                    (validPrimarySurface && grazing_factor > 0.0 && diff_plane_dist < 0.025 && dist_depth < grazing_depth_tol);

                if(depth_ok_diff && dot_geo_normals > 0.5)
                {
                    float w_diff = w[i] * max(dot_normals, 0);

                    Q2SH hist_color_lf = q2LoadSH(framebufQ2HistColorLF_SH_Prev_Sampled, framebufQ2HistColorLF_COCG_Prev_Sampled, p);
                    q2AccumulateSH(temporal_color_lf, hist_color_lf, w_diff);

                    temporal_color_hf           += q2UnpackRGBE(framebufQ2HistColorHF_Prev_Sampled.Load(int3(p, 0)).r) * w_diff;
                    temporal_moments_histlen_hf += framebufQ2HistMomentsHF_Prev_Sampled.Load(int3(p, 0)).rgba * w_diff;
                    temporal_sum_w_diff         += w_diff;
                }
            }

            // Dedicated specular 2x2 bilinear history fetch at virtual-parallax reprojection coords
            const float2 pos_ld_spec = floor(pos_prev_spec - (float2)0.5);
            const float2 subpix_spec = frac(pos_prev_spec - (float2)0.5 - pos_ld_spec);
            const float w_s[4] = {
                (1.0 - subpix_spec.x) * (1.0 - subpix_spec.y),
                (subpix_spec.x      ) * (1.0 - subpix_spec.y),
                (1.0 - subpix_spec.x) * (subpix_spec.y      ),
                (subpix_spec.x      ) * (subpix_spec.y      )
            };
            for(int j = 0; j < 4; j++)
            {
                int2 p_s = int2(pos_ld_spec) + off[j];
                if(p_s.x < field_left || p_s.x >= field_right || p_s.y >= height || p_s.y < 0)
                    continue;

                if (sample_across_fields)
                {
                    p_s = q2FlatToChecker(p_s, width);
                }

                const float3 surfPosPrev = framebufSurfacePosition_Prev_Sampled.Load(int3(p_s, 0)).xyz;
                const float depth_prev_s = framebufQ2ViewDepth_Prev_Sampled.Load(int3(p_s, 0)).r;
                const float3 geo_normal_prev_s = texelFetchNormalGeometry_Prev(p_s);
                const float rough_prev = framebufMetallicRoughness_Prev_Sampled.Load(int3(p_s, 0)).y;

                const float planeDist = abs(dot(surfPosPrev - surfPos, geo_normal_curr)) / max(abs(depth_curr), 1e-3);
                float surfDepthDist = abs(depth_curr - depth_prev_s + motion_z) / max(abs(depth_curr), 1e-3);
                if(depth_curr < 0.0)
                {
                    surfDepthDist *= 0.25;
                }
                const float geomDist = lerp(planeDist, surfDepthDist, surface_blend);
                const float parallaxUv = length(motion_spec_xy - motion.xy);
                const float planeTol = lerp(0.015, 0.08, surface_blend);
                const float spec_grazing_dist = abs(dot(surfPosPrev - surfPos, geo_normal_curr)) / max(abs(depth_curr), 0.25);
                const bool spec_grazing_ok = (validPrimarySurface && grazing_factor > 0.0 && spec_grazing_dist < 0.025);
                const float spec_depth_limit = max(0.08 + 4.0 * parallaxUv, spec_grazing_ok ? grazing_depth_tol : 0.0);

                if(dot(geo_normal_curr, geo_normal_prev_s) > 0.75
                    && (geomDist < planeTol || (spec_grazing_ok && surfDepthDist < grazing_depth_tol))
                    && surfDepthDist < spec_depth_limit
                    && abs(rough_prev - metal_rough.y) <= 0.25)
                {
                    const float3 normal_prev_s = texelFetchNormal_Prev(p_s);
                    const float w_rough = exp(-abs(rough_prev - metal_rough.y) * 10.0);
                    const float w_spec = w_s[j] * q2SafePow(max(dot(normal_curr, normal_prev_s), 0.0), shininess) * w_rough;

                    temporal_color_histlen_spec += framebufQ2FilteredSpec_Prev_Sampled.Load(int3(p_s, 0)) * w_spec;
                    temporal_moments_spec       += framebufQ2HistMomentsSpec_Prev_Sampled.Load(int3(p_s, 0)).xy * w_spec;
                    temporal_sum_w_spec         += w_spec;
                }
            }

            if(temporal_sum_w_diff > 1e-6)
            {
                float inv_w_diff = 1.0 / temporal_sum_w_diff;
                temporal_color_lf.shY  *= inv_w_diff;
                temporal_color_lf.CoCg *= inv_w_diff;
                temporal_color_hf      *= inv_w_diff;
                temporal_moments_histlen_hf *= inv_w_diff;
                temporal_sample_valid_diff = true;
            }

            if(temporal_sum_w_spec > 1e-6)
            {
                float inv_w_spec = 1.0 / temporal_sum_w_spec;
                temporal_color_histlen_spec *= inv_w_spec;
                temporal_moments_spec       *= inv_w_spec;
                temporal_sample_valid_spec = true;
            }
        }

        // Compute spatial moments of HF and SPEC channels in a 3x3 window
        float lum_curr_spec;
        float rough_curr_shared;
        bool is_grad_curr = false;
        get_shared_spec_data(groupThreadID, (int2)0, lum_curr_spec, rough_curr_shared, is_grad_curr);
        float2 spatial_moments_hf = float2(lum_curr_hf, lum_curr_hf * lum_curr_hf);
        float2 spatial_moments_spec = is_grad_curr ? (float2)0.0 : float2(lum_curr_spec, lum_curr_spec * lum_curr_spec);

        {
            float spatial_sum_w_hf = 1.0;
            float spatial_sum_w_spec = is_grad_curr ? 0.0 : 1.0;
            for(int yy = -FILTER_RADIUS; yy <= FILTER_RADIUS; yy++)
            {
                for(int xx = -FILTER_RADIUS; xx <= FILTER_RADIUS; xx++)
                {
                    if(xx == 0 && yy == 0)
                        continue;

                    const int2 mp = ipos + int2(xx, yy);
                    if (mp.x < cb_left || mp.x >= cb_right || mp.y < 0 || mp.y >= height)
                        continue;

                    float depth;
                    float3 normal;
                    float lum_p_hf;
                    get_shared_data(groupThreadID, int2(xx, yy), depth, normal, lum_p_hf);

                    float dist_z = abs(depth_curr - depth) * gradDepth;
                    const float spatial_depth_tol = lerp(2.0, 3.5, grazing_factor);
                    if(dist_z < spatial_depth_tol)
                    {
                        float w_hf = pow(max(0.0, dot(normal, normal_curr)), 128.0);

                        spatial_moments_hf += float2(lum_p_hf * w_hf, lum_p_hf * lum_p_hf * w_hf);
                        spatial_sum_w_hf  += w_hf;

                        float lum_p_spec;
                        float rough_p;
                        bool is_grad_p = false;
                        get_shared_spec_data(groupThreadID, int2(xx, yy), lum_p_spec, rough_p, is_grad_p);
                        if (!is_grad_p)
                        {
                            const float w_s_mom = pow(max(0.0, dot(normal, normal_curr)), 32.0)
                                                * exp(-abs(rough_p - metal_rough.y) * 10.0);
                            spatial_moments_spec += float2(lum_p_spec * w_s_mom, lum_p_spec * lum_p_spec * w_s_mom);
                            spatial_sum_w_spec   += w_s_mom;
                        }
                    }
                }
            }

            spatial_moments_hf /= spatial_sum_w_hf;
            spatial_moments_spec = (spatial_sum_w_spec > 1e-6)
                ? (spatial_moments_spec / spatial_sum_w_spec)
                : float2(lum_curr_spec, lum_curr_spec * lum_curr_spec);
        }

        // Load the target pixel colors for all channels
        Q2SH color_curr_lf = q2LoadSH(framebufQ2ColorLF_SH_Sampled, framebufQ2ColorLF_COCG_Sampled, ipos);
        float3 color_curr_hf = q2UnpackRGBE(framebufQ2ColorHF_Sampled.Load(int3(ipos, 0)).r);
        float3 color_curr_spec = q2UnpackRGBE(framebufQ2ColorSpec_Sampled.Load(int3(ipos, 0)).r);
        if (is_grad_curr)
        {
            float3 sum_curr_spec = (float3)0.0;
            float sum_w_curr = 0.0;

            for (int yy = -1; yy <= 1; yy++)
            {
                for (int xx = -1; xx <= 1; xx++)
                {
                    if (xx == 0 && yy == 0)
                    {
                        continue;
                    }

                    const int2 np = ipos + int2(xx, yy);
                    if (np.x < cb_left || np.x >= cb_right || np.y < 0 || np.y >= height)
                    {
                        continue;
                    }

                    float d_n;
                    float3 n_n;
                    float lum_hf_n;
                    get_shared_data(groupThreadID, int2(xx, yy), d_n, n_n, lum_hf_n);

                    float lum_spec_n;
                    float rough_n;
                    bool is_grad_n = false;
                    get_shared_spec_data(groupThreadID, int2(xx, yy), lum_spec_n, rough_n, is_grad_n);

                    if (!is_grad_n && abs(depth_curr - d_n) * gradDepth < 2.0)
                    {
                        const float w_n = ((xx == 0 || yy == 0) ? 1.0 : 0.5)
                                          * q2SafePow(max(0.0, dot(n_n, normal_curr)), shininess)
                                          * exp(-abs(rough_n - metal_rough.y) * 10.0);
                        sum_curr_spec += q2UnpackRGBE(framebufQ2ColorSpec_Sampled.Load(int3(np, 0)).r) * w_n;
                        sum_w_curr += w_n;
                    }
                }
            }

            if (sum_w_curr > 1e-5)
            {
                color_curr_spec = sum_curr_spec / sum_w_curr;
                lum_curr_spec = getLuminance(color_curr_spec);
            }
        }

        // NRD-ReLAX / SVGF current-sample firefly clamp on non-mirror lobes: suppress isolated
        // 1-spp outlier sparks against same-surface 3x3 spatial moments before temporal blend.
        const float rough_clamp_t = smoothstep(0.04, 0.25, metal_rough.y);
        if (rough_clamp_t > 0.0)
        {
            const float sp_mean_s = max(0.0, spatial_moments_spec.x);
            const float sp_std_s = sqrt(max(0.0, spatial_moments_spec.y - sp_mean_s * sp_mean_s));
            const float spec_sigma_k = lerp(6.0, 2.25, rough_clamp_t);
            const float spec_cap = sp_mean_s + spec_sigma_k * max(sp_std_s, 0.12 * sp_mean_s + 0.04);
            if (lum_curr_spec > spec_cap && lum_curr_spec > 0.08)
            {
                color_curr_spec *= (spec_cap / lum_curr_spec);
                lum_curr_spec = spec_cap;
            }
        }


        // Load the gradients (produced by the Q2RTX-style gradient pipeline at
        // 1/3 res: CmQ2GradientReproject + Img + Atrous). After the 7 atrous
        // iterations the LF result is in Q2GradLFPong and the HF/SPEC result
        // (after 3 iterations) in Q2GradHFSpecPong. Clamped like Q2RTX.
        const int2 grad_ipos = q2ClampGradPos(ipos, width, height);
        float grad_lf = framebufQ2GradLFPong_Sampled.Load(int3(grad_ipos, 0)).x;
        float2 grad_hf_spec = framebufQ2GradHFSpecPong_Sampled.Load(int3(grad_ipos, 0)).xy;
        grad_lf = clamp(grad_lf, 0.0, 1.0);
        grad_hf_spec = clamp(grad_hf_spec, (float2)0.0, (float2)1.0);

        if(temporal_sample_valid_diff)
        {
            float antilag_alpha_lf = clamp(lerp(1.0, Q2_FLT_ANTILAG_LF * grad_lf, Q2_FLT_TEMPORAL_LF), 0, 1);
            float antilag_alpha_hf = clamp(lerp(1.0, Q2_FLT_ANTILAG_HF * grad_hf_spec.x, Q2_FLT_TEMPORAL_HF), 0, 1);

            // Adjust the history length, taking the antilag factors into account
            float hist_len_hf = min(temporal_moments_histlen_hf.b * pow(1.0 - antilag_alpha_hf, 10) + 1.0, 256.0);
            float hist_len_lf = min(temporal_moments_histlen_hf.a * pow(1.0 - antilag_alpha_lf, 10) + 1.0, 256.0);

            // Compute the blending weights based on history length, so that the filter
            // converges faster (first frame weight 1.0, second 1/2, third 1/3, ...).
            float alpha_color_lf = max(Q2_FLT_MIN_ALPHA_COLOR_LF, 1.0 / hist_len_lf);
            float alpha_color_hf = max(Q2_FLT_MIN_ALPHA_COLOR_HF, 1.0 / hist_len_hf);
            float alpha_moments_hf = max(Q2_FLT_MIN_ALPHA_MOMENTS_HF, 1.0 / hist_len_hf);

            // Adjust the blending factors, taking the antilag factors into account again
            alpha_color_lf = lerp(alpha_color_lf, 1.0, antilag_alpha_lf);
            alpha_color_hf = lerp(alpha_color_hf, 1.0, antilag_alpha_hf);
            alpha_moments_hf = lerp(alpha_moments_hf, 1.0, antilag_alpha_hf);

            out_color_lf = q2MixSH(temporal_color_lf, color_curr_lf, alpha_color_lf);
            out_color_hf.rgb = lerp(temporal_color_hf.rgb, color_curr_hf.rgb, alpha_color_hf);

            out_moments_histlen_hf.rg = lerp(temporal_moments_histlen_hf.rg, spatial_moments_hf.rg, alpha_moments_hf);
            out_moments_histlen_hf.b = hist_len_hf;
            out_moments_histlen_hf.a = hist_len_lf;
        }
        else
        {
            // No valid history - just use the current color and spatial moments
            out_color_lf = color_curr_lf;
            out_color_hf.rgb = color_curr_hf;
            out_moments_histlen_hf = float4(spatial_moments_hf, 1, 1);
        }

        const float2 curr_moments_spec = float2(lum_curr_spec, lum_curr_spec * lum_curr_spec);
        float2 out_moments_spec;

        if(temporal_sample_valid_spec)
        {
            // Resolution-normalized motion antilag attenuated when virtual-point parallax is trusted
            const float resScale1080 = 1080.0 / max(globalUniform.renderHeight, 1.0);
            const float motion_length_spec = motion_length * resScale1080 * (1.0 - 0.95 * spec_virtual_weight);
            float antilag = grad_hf_spec.y * Q2_FLT_ANTILAG_SPEC + motion_length_spec * Q2_FLT_ANTILAG_SPEC_MOTION;
            float antilag_alpha_spec = clamp(lerp(1.0, antilag, Q2_FLT_TEMPORAL_SPEC), 0, 1);
            float hist_len_spec = min(temporal_color_histlen_spec.a * pow(1.0 - antilag_alpha_spec, 10) + 1.0, 256.0);
            float alpha_color_spec = max(Q2_FLT_MIN_ALPHA_COLOR_SPEC, 1.0 / hist_len_spec);
            alpha_color_spec = lerp(alpha_color_spec, 1.0, antilag_alpha_spec);
            out_color_histlen_spec.rgb = lerp(temporal_color_histlen_spec.rgb, color_curr_spec.rgb, alpha_color_spec);
            out_color_histlen_spec.a = hist_len_spec;

            float alpha_moments_spec = max(Q2_FLT_MIN_ALPHA_MOMENTS_SPEC, 1.0 / hist_len_spec);
            alpha_moments_spec = lerp(alpha_moments_spec, 1.0, antilag_alpha_spec);
            out_moments_spec = lerp(temporal_moments_spec, curr_moments_spec, alpha_moments_spec);
        }
        else
        {
            out_color_histlen_spec = float4(color_curr_spec, 1);
            out_moments_spec = curr_moments_spec;
        }

        const float2 atrous_moments_spec = (out_color_histlen_spec.a < 4.0)
            ? lerp(spatial_moments_spec, out_moments_spec, saturate((out_color_histlen_spec.a - 1.0) / 3.0))
            : out_moments_spec;

        float2 atrous_moments_hf = out_moments_histlen_hf.xy;
        if (out_moments_histlen_hf.b < 4.0)
        {
            float2 sum_mom5 = float2(lum_curr_hf, lum_curr_hf * lum_curr_hf);
            float sum_w5 = 1.0;
            const float sp5_depth_tol = lerp(2.0, 3.5, grazing_factor);
            for (int yy5 = -2; yy5 <= 2; yy5++)
            {
                for (int xx5 = -2; xx5 <= 2; xx5++)
                {
                    if (xx5 == 0 && yy5 == 0)
                        continue;
                    const int2 mp5 = ipos + int2(xx5, yy5);
                    if (mp5.x < cb_left || mp5.x >= cb_right || mp5.y < 0 || mp5.y >= height)
                        continue;
                    const float d5 = framebufQ2ViewDepth_Sampled.Load(int3(mp5, 0)).r;
                    const float3 n5 = texelFetchNormal(mp5);
                    const float dz5 = abs(depth_curr - d5) * gradDepth;
                    const float nd5 = max(0.0, dot(normal_curr, n5));
                    if (dz5 < sp5_depth_tol && nd5 > 0.7)
                    {
                        const float w5 = pow(nd5, 32.0);
                        const float l5 = getLuminance(q2UnpackRGBE(framebufQ2ColorHF_Sampled.Load(int3(mp5, 0)).r));
                        sum_mom5 += float2(l5, l5 * l5) * w5;
                        sum_w5 += w5;
                    }
                }
            }
            const float2 spatial5x5_hf = sum_mom5 / max(sum_w5, 1e-6);
            const float fast_t_hf = saturate((out_moments_histlen_hf.b - 1.0) / 3.0);
            atrous_moments_hf = lerp(spatial5x5_hf, out_moments_histlen_hf.xy, fast_t_hf);
        }

        // Store the outputs for further processing by the a-trous filter
        framebufQ2HistMomentsHF[ipos] = out_moments_histlen_hf;
        framebufQ2HistMomentsSpec[ipos] = float4(out_moments_spec, 0.0, 0.0);
        Q2_STORE_SH(framebufQ2HistColorLF_SH, framebufQ2HistColorLF_COCG, ipos, out_color_lf);
        framebufQ2AtrousPingHF[ipos] = (uint4)q2PackRGBE(out_color_hf);
        framebufQ2AtrousPingSpec[ipos] = (uint4)q2PackRGBE(out_color_histlen_spec.rgb);
        framebufQ2AtrousPingMoments[ipos] = float4(atrous_moments_hf, atrous_moments_spec);
        framebufQ2FilteredSpec[ipos] = out_color_histlen_spec;
    }

    GroupMemoryBarrierWithGroupSync();

    // Store the LF channel into shared memory for averaging
    s_lf_shy[groupThreadID.y][groupThreadID.x] = out_color_lf.shY;
    s_lf_cocg[groupThreadID.y][groupThreadID.x] = out_color_lf.CoCg;

    if(groupThreadID.x % Q2_GRAD_DWN == 1 && groupThreadID.y % Q2_GRAD_DWN == 1)
    {
        s_depth_width[groupThreadID.y / Q2_GRAD_DWN][groupThreadID.x / Q2_GRAD_DWN] = gradDepth;
    }

    GroupMemoryBarrierWithGroupSync();

    // Compute a 1/3-resolution version of the LF channel for this group.
    // Use a bilateral filter that takes the center pixel of each 3x3 square as the anchor.

    uint2 lowres_local_id;
    lowres_local_id.x = groupIndex % LOWRES_SIZE_X;
    lowres_local_id.y = groupIndex / LOWRES_SIZE_X;

    if(lowres_local_id.y >= LOWRES_SIZE_Y)
        return;

    uint2 center_shared_pos = lowres_local_id * Q2_GRAD_DWN + (uint2)1;
    const int2 center_ipos = int2(groupID.xy) * int2(GROUP_SIZE_X, GROUP_SIZE_Y) + int2(center_shared_pos);
    if (center_ipos.x >= width || center_ipos.y >= height)
        return;

    const int lf_half_width = width / 2;
    const int lf_field_left = (center_ipos.x < lf_half_width) ? 0 : lf_half_width;
    const int lf_field_right = (center_ipos.x < lf_half_width) ? lf_half_width : width;

    float3 center_normal = float3(unpackHalf2x16(s_normal_lum[center_shared_pos.y + FILTER_RADIUS][center_shared_pos.x + FILTER_RADIUS].x),
                                  unpackHalf2x16(s_normal_lum[center_shared_pos.y + FILTER_RADIUS][center_shared_pos.x + FILTER_RADIUS].y).x);
    float center_depth = s_depth[center_shared_pos.y + FILTER_RADIUS][center_shared_pos.x + FILTER_RADIUS];
    float depth_width = s_depth_width[lowres_local_id.y][lowres_local_id.x];

    Q2SH center_lf;
    center_lf.shY = s_lf_shy[center_shared_pos.y][center_shared_pos.x];
    center_lf.CoCg = s_lf_cocg[center_shared_pos.y][center_shared_pos.x];

    float sum_w = 1;
    Q2SH sum_lf = center_lf;

    const float3 lf_view_dir = safeNormalize(framebufViewDirection_Sampled.Load(int3(center_ipos, 0)).xyz);
    const float lf_grazing = (center_depth > 1e-4 && dot(center_normal, center_normal) > 0.5)
        ? smoothstep(0.45, 0.10, abs(dot(center_normal, lf_view_dir)))
        : 0.0;
    const float lf_depth_tol = lerp(2.0, 3.5, lf_grazing);

    // Average the anchor pixel color with the relevant neighborhood
    for(int yy = -1; yy <= 1; yy++)
    {
        for(int xx = -1; xx <= 1; xx++)
        {
            if(yy == 0 && xx == 0)
                continue;

            const int2 p_ipos = center_ipos + int2(xx, yy);
            if (p_ipos.x < lf_field_left || p_ipos.x >= lf_field_right || p_ipos.y < 0 || p_ipos.y >= height)
                continue;

            float3 p_normal = float3(unpackHalf2x16(s_normal_lum[center_shared_pos.y + FILTER_RADIUS + yy][center_shared_pos.x + FILTER_RADIUS + xx].x),
                                     unpackHalf2x16(s_normal_lum[center_shared_pos.y + FILTER_RADIUS + yy][center_shared_pos.x + FILTER_RADIUS + xx].y).x);
            float p_depth = s_depth[center_shared_pos.y + FILTER_RADIUS + yy][center_shared_pos.x + FILTER_RADIUS + xx];

            float dist_depth = abs(p_depth - center_depth) * depth_width;
            if(dist_depth < lf_depth_tol)
            {
                float w = pow(max(dot(p_normal, center_normal), 0), 8);

                Q2SH p_lf;
                p_lf.shY = s_lf_shy[center_shared_pos.y + yy][center_shared_pos.x + xx];
                p_lf.CoCg = s_lf_cocg[center_shared_pos.y + yy][center_shared_pos.x + xx];

                q2AccumulateSH(sum_lf, p_lf, w);
                sum_w += w;
            }
        }
    }

    float inv_w = 1.0 / sum_w;
    sum_lf.shY  *= inv_w;
    sum_lf.CoCg *= inv_w;

    // Store the LF result for further processing by the a-trous LF filter
    int2 ipos_lowres = int2(groupID.xy) * int2(LOWRES_SIZE_X, LOWRES_SIZE_Y) + int2(lowres_local_id);

    Q2_STORE_SH(framebufQ2AtrousPingLF_SH, framebufQ2AtrousPingLF_COCG, ipos_lowres, sum_lf);
}
