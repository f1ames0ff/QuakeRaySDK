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

#include "VulkanTestContext.h"
#include "Matrix.h"
#include "RHI/RhiPipeline.h"
#include "Generated/ShaderCommonC.h"
#include "Generated/ShaderCommonCFramebuf.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace qray;

namespace
{

constexpr uint32_t kWidth = 194;
constexpr uint32_t kHeight = 194;
constexpr uint32_t kThirdWidth = (kWidth + 2u) / 3u;
constexpr uint32_t kThirdHeight = (kHeight + 2u) / 3u;
constexpr uint32_t kGradWidth = kWidth / 3u;
constexpr uint32_t kGradHeight = kHeight / 3u;
constexpr float kPi = 3.14159265358979323846f;

struct FbEntry
{
    FramebufferImageIndex image;
    bool isUav;
};

struct alignas(16) SynthTestParams
{
    uint32_t computeRef = 0;
    uint32_t lightOn = 1;
    uint32_t forceZeroHitDist = 0;
    uint32_t coplanarStep = 0;
    float camPosX = 0.0f;
    float prevCamPosX = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};

struct FrameConfig
{
    uint32_t frameId = 0;
    float bounceRays = 0.5f;
    float fltEnable = 1.0f;
    uint32_t debugFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
    uint32_t lightOn = 1;
    uint32_t forceZeroHitDist = 0;
    uint32_t coplanarStep = 0;
    uint32_t computeRef = 0;
    bool resetHistory = false;
    bool runPrepareFinal = false;
    float camPosX = 0.0f;
    float prevCamPosX = 0.0f;
};

constexpr std::array<FbEntry, 16> kGbufferBindings = {{
    { FB_IMAGE_INDEX_ALBEDO, true },
    { FB_IMAGE_INDEX_IS_SKY, true },
    { FB_IMAGE_INDEX_NORMAL, true },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, true },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, true },
    { FB_IMAGE_INDEX_DEPTH_WORLD, true },
    { FB_IMAGE_INDEX_DEPTH_GRAD, true },
    { FB_IMAGE_INDEX_MOTION, true },
    { FB_IMAGE_INDEX_SURFACE_POSITION, true },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, true },
    { FB_IMAGE_INDEX_THROUGHPUT, true },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, true },
    { FB_IMAGE_INDEX_Q2_TRANSPARENT, true },
    { FB_IMAGE_INDEX_Q2_FOG_ACCUM, true },
    { FB_IMAGE_INDEX_Q2_RNG_SEED, true },
    { FB_IMAGE_INDEX_Q2_CLUSTER, true },
}};

constexpr std::array<FbEntry, 11> kShadeBindings = {{
    { FB_IMAGE_INDEX_UNFILTERED_DIRECT, true },
    { FB_IMAGE_INDEX_UNFILTERED_SPECULAR, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_R, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_G, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_B, true },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR, true },
    { FB_IMAGE_INDEX_NORMAL, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, false },
    { FB_IMAGE_INDEX_SURFACE_POSITION, false },
    { FB_IMAGE_INDEX_Q2_RNG_SEED, false },
}};

constexpr std::array<FbEntry, 29> kReprojectBindings = {{
    { FB_IMAGE_INDEX_NORMAL, true },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, true },
    { FB_IMAGE_INDEX_SURFACE_POSITION, true },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, true },
    { FB_IMAGE_INDEX_Q2_GRAD_SAVED_SURF_POS, true },
    { FB_IMAGE_INDEX_Q2_GRAD_SAVED_VIEW_DIR, true },
    { FB_IMAGE_INDEX_Q2_BASE_COLOR, true },
    { FB_IMAGE_INDEX_Q2_METALLIC, true },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING, true },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, true },
    { FB_IMAGE_INDEX_Q2_RNG_SEED, true },
    { FB_IMAGE_INDEX_Q2_CLUSTER, true },
    { FB_IMAGE_INDEX_ALBEDO, false },
    { FB_IMAGE_INDEX_ALBEDO_PREV, false },
    { FB_IMAGE_INDEX_NORMAL_PREV, false },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, false },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY_PREV, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS_PREV, false },
    { FB_IMAGE_INDEX_MOTION, false },
    { FB_IMAGE_INDEX_SURFACE_POSITION_PREV, false },
    { FB_IMAGE_INDEX_VIEW_DIRECTION_PREV, false },
    { FB_IMAGE_INDEX_THROUGHPUT, false },
    { FB_IMAGE_INDEX_Q2_COLOR_H_F_PREV, false },
    { FB_IMAGE_INDEX_Q2_COLOR_SPEC_PREV, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH_PREV, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV, false },
    { FB_IMAGE_INDEX_Q2_RNG_SEED_PREV, false },
    { FB_IMAGE_INDEX_Q2_CLUSTER_PREV, false },
}};

constexpr std::array<FbEntry, 22> kAdapterBindings = {{
    { FB_IMAGE_INDEX_Q2_COLOR_L_F_S_H, true },
    { FB_IMAGE_INDEX_Q2_COLOR_L_F_C_O_C_G, true },
    { FB_IMAGE_INDEX_Q2_COLOR_H_F, true },
    { FB_IMAGE_INDEX_Q2_COLOR_SPEC, true },
    { FB_IMAGE_INDEX_Q2_COLOR, true },
    { FB_IMAGE_INDEX_Q2_SSS_DIFFUSE, true },
    { FB_IMAGE_INDEX_ALBEDO, false },
    { FB_IMAGE_INDEX_IS_SKY, false },
    { FB_IMAGE_INDEX_NORMAL, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, false },
    { FB_IMAGE_INDEX_UNFILTERED_DIRECT, false },
    { FB_IMAGE_INDEX_UNFILTERED_SPECULAR, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_R, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_G, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_B, false },
    { FB_IMAGE_INDEX_THROUGHPUT, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, false },
    { FB_IMAGE_INDEX_Q2_TRANSPARENT, false },
    { FB_IMAGE_INDEX_Q2_FOG_ACCUM, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, false },
    { FB_IMAGE_INDEX_Q2_SUBSURFACE, false },
}};

constexpr std::array<FbEntry, 16> kGradImgBindings = {{
    { FB_IMAGE_INDEX_NORMAL, true },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, true },
    { FB_IMAGE_INDEX_SURFACE_POSITION, true },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, true },
    { FB_IMAGE_INDEX_Q2_GRAD_L_F_PING, true },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING, true },
    { FB_IMAGE_INDEX_MOTION, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SAVED_SURF_POS, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SAVED_VIEW_DIR, false },
    { FB_IMAGE_INDEX_Q2_COLOR_L_F_S_H, false },
    { FB_IMAGE_INDEX_Q2_COLOR_H_F, false },
    { FB_IMAGE_INDEX_Q2_COLOR_SPEC, false },
    { FB_IMAGE_INDEX_Q2_BASE_COLOR, false },
    { FB_IMAGE_INDEX_Q2_METALLIC, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H_PREV, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, false },
}};

constexpr std::array<FbEntry, 4> kGradAtrousBindings = {{
    { FB_IMAGE_INDEX_Q2_GRAD_L_F_PING, true },
    { FB_IMAGE_INDEX_Q2_GRAD_L_F_PONG, true },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING, true },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG, true },
}};

constexpr std::array<FbEntry, 37> kTemporalBindings = {{
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_SPEC, true },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H, true },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G, true },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_H_F, true },
    { FB_IMAGE_INDEX_Q2_FILTERED_SPEC, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS, true },
    { FB_IMAGE_INDEX_NORMAL, false },
    { FB_IMAGE_INDEX_NORMAL_PREV, false },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, false },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY_PREV, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS_PREV, false },
    { FB_IMAGE_INDEX_DEPTH_GRAD, false },
    { FB_IMAGE_INDEX_MOTION, false },
    { FB_IMAGE_INDEX_SURFACE_POSITION, false },
    { FB_IMAGE_INDEX_SURFACE_POSITION_PREV, false },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, false },
    { FB_IMAGE_INDEX_THROUGHPUT, false },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_SPEC_PREV, false },
    { FB_IMAGE_INDEX_Q2_COLOR_L_F_S_H, false },
    { FB_IMAGE_INDEX_Q2_COLOR_L_F_C_O_C_G, false },
    { FB_IMAGE_INDEX_Q2_COLOR_H_F, false },
    { FB_IMAGE_INDEX_Q2_COLOR_SPEC, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH_PREV, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H_PREV, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G_PREV, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F_PREV, false },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_H_F_PREV, false },
    { FB_IMAGE_INDEX_Q2_FILTERED_SPEC_PREV, false },
    { FB_IMAGE_INDEX_Q2_GRAD_L_F_PONG, false },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, false },
}};

constexpr std::array<FbEntry, 7> kAtrousLfBindings = {{
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_S_H, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_C_O_C_G, true },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, false },
    { FB_IMAGE_INDEX_DEPTH_GRAD, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, false },
}};

constexpr std::array<FbEntry, 27> kAtrousBindings = {{
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PONG_H_F, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS, true },
    { FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS, true },
    { FB_IMAGE_INDEX_Q2_COLOR, true },
    { FB_IMAGE_INDEX_Q2_SSS_DIFFUSE, true },
    { FB_IMAGE_INDEX_ALBEDO, false },
    { FB_IMAGE_INDEX_IS_SKY, false },
    { FB_IMAGE_INDEX_NORMAL, false },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, false },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, false },
    { FB_IMAGE_INDEX_DEPTH_GRAD, false },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, false },
    { FB_IMAGE_INDEX_THROUGHPUT, false },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, false },
    { FB_IMAGE_INDEX_Q2_TRANSPARENT, false },
    { FB_IMAGE_INDEX_Q2_FOG_ACCUM, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H, false },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G, false },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_H_F, false },
    { FB_IMAGE_INDEX_Q2_FILTERED_SPEC, false },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H, false },
    { FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G, false },
    { FB_IMAGE_INDEX_Q2_SUBSURFACE, false },
}};

constexpr std::array<FbEntry, 21> kPrepareFinalBindings = {{
    { FB_IMAGE_INDEX_ALBEDO, false },
    { FB_IMAGE_INDEX_NORMAL, false },
    { FB_IMAGE_INDEX_DEPTH_WORLD, false },
    { FB_IMAGE_INDEX_SURFACE_POSITION, false },
    { FB_IMAGE_INDEX_MOTION, false },
    { FB_IMAGE_INDEX_UNFILTERED_DIRECT, false },
    { FB_IMAGE_INDEX_UNFILTERED_SPECULAR, false },
    { FB_IMAGE_INDEX_PRIMARY_TO_REFL_REFR, false },
    { FB_IMAGE_INDEX_THROUGHPUT, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR, false },
    { FB_IMAGE_INDEX_ACID_FOG, false },
    { FB_IMAGE_INDEX_SCREEN_EMISSION, false },
    { FB_IMAGE_INDEX_GOD_RAYS_FILTERED, false },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_R, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_G, true },
    { FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_B, true },
    { FB_IMAGE_INDEX_FINAL, true },
    { FB_IMAGE_INDEX_BLOOM_INPUT, false },
    { FB_IMAGE_INDEX_Q2_FILTERED_SPEC, false },
    { FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG, false },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, false },
}};

constexpr std::array<std::pair<FramebufferImageIndex, FramebufferImageIndex>, 18> kPrevCopies = {{
    { FB_IMAGE_INDEX_ALBEDO, FB_IMAGE_INDEX_ALBEDO_PREV },
    { FB_IMAGE_INDEX_NORMAL, FB_IMAGE_INDEX_NORMAL_PREV },
    { FB_IMAGE_INDEX_NORMAL_GEOMETRY, FB_IMAGE_INDEX_NORMAL_GEOMETRY_PREV },
    { FB_IMAGE_INDEX_METALLIC_ROUGHNESS, FB_IMAGE_INDEX_METALLIC_ROUGHNESS_PREV },
    { FB_IMAGE_INDEX_SURFACE_POSITION, FB_IMAGE_INDEX_SURFACE_POSITION_PREV },
    { FB_IMAGE_INDEX_VIEW_DIRECTION, FB_IMAGE_INDEX_VIEW_DIRECTION_PREV },
    { FB_IMAGE_INDEX_Q2_COLOR_H_F, FB_IMAGE_INDEX_Q2_COLOR_H_F_PREV },
    { FB_IMAGE_INDEX_Q2_COLOR_SPEC, FB_IMAGE_INDEX_Q2_COLOR_SPEC_PREV },
    { FB_IMAGE_INDEX_Q2_VIEW_DEPTH, FB_IMAGE_INDEX_Q2_VIEW_DEPTH_PREV },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H, FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H_PREV },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G, FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G_PREV },
    { FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F, FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F_PREV },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_H_F, FB_IMAGE_INDEX_Q2_HIST_MOMENTS_H_F_PREV },
    { FB_IMAGE_INDEX_Q2_HIST_MOMENTS_SPEC, FB_IMAGE_INDEX_Q2_HIST_MOMENTS_SPEC_PREV },
    { FB_IMAGE_INDEX_Q2_FILTERED_SPEC, FB_IMAGE_INDEX_Q2_FILTERED_SPEC_PREV },
    { FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS, FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV },
    { FB_IMAGE_INDEX_Q2_RNG_SEED, FB_IMAGE_INDEX_Q2_RNG_SEED_PREV },
    { FB_IMAGE_INDEX_Q2_CLUSTER, FB_IMAGE_INDEX_Q2_CLUSTER_PREV },
}};

// --- Gate 1: Deterministic CPU Math Helpers ---

uint32_t CpuPackRGBE(float r, float g, float b)
{
    auto clean = [](float x) {
        if (!std::isfinite(x)) return (x > 0.0f) ? 4088.0f : 0.0f;
        return std::max(0.0f, x);
    };
    float va[3] = { clean(r), clean(g), clean(b) };
    float maxAbs = std::max({ va[0], va[1], va[2] });
    if (maxAbs <= 0.0f)
    {
        return 0u;
    }

    constexpr float kMaxRgbe = 4088.0f;
    if (maxAbs > kMaxRgbe)
    {
        const float s = kMaxRgbe / maxAbs;
        va[0] *= s;
        va[1] *= s;
        va[2] *= s;
        maxAbs = kMaxRgbe;
    }

    const float exponent = std::clamp(std::floor(std::log2(maxAbs)), -20.0f, 11.0f);
    uint32_t result = static_cast<uint32_t>(exponent + 20.0f) << 27u;
    const float scale = std::ldexp(256.0f, -static_cast<int>(exponent));
    auto quant = [&](float c) {
        return std::min(511u, static_cast<uint32_t>(std::llrint(c * scale)));
    };
    result |= quant(va[0]);
    result |= quant(va[1]) << 9u;
    result |= quant(va[2]) << 18u;
    return result;
}

std::array<float, 3> CpuUnpackRGBE(uint32_t x)
{
    const int exponent = static_cast<int>(x >> 27u) - 20;
    const float scale = std::ldexp(1.0f / 256.0f, exponent);
    return {
        static_cast<float>(x & 0x1FFu) * scale,
        static_cast<float>((x >> 9u) & 0x1FFu) * scale,
        static_cast<float>((x >> 18u) & 0x1FFu) * scale,
    };
}

float Luminance(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

bool CheckConvolveMonotonic(std::span<const uint32_t> steps)
{
    constexpr int kRadius = 32;
    std::vector<double> cur(kRadius * 2 + 1, 0.0);
    cur[kRadius] = 1.0;

    for (uint32_t step : steps)
    {
        std::vector<double> next(cur.size(), 0.0);
        for (int i = 0; i < static_cast<int>(cur.size()); i++)
        {
            const double left = (i - static_cast<int>(step) >= 0) ? cur[i - step] : 0.0;
            const double right = (i + static_cast<int>(step) < static_cast<int>(cur.size())) ? cur[i + step] : 0.0;
            next[i] = 0.25 * left + 0.50 * cur[i] + 0.25 * right;
        }
        cur.swap(next);
    }

    double sum = 0.0;
    for (double v : cur) sum += v;
    if (std::abs(sum - 1.0) > 1e-6) return false;

    for (int k = 0; k < 15; k++)
    {
        if (cur[kRadius + k] + 1e-12 < cur[kRadius + k + 1]) return false;
    }
    return true;
}

void VerifyCpuAtrousImpulse()
{
    const std::array<uint32_t, 4> dyadic = { 1u, 2u, 4u, 8u };
    const std::array<uint32_t, 4> nonDyadic = { 1u, 3u, 7u, 15u };
    Require(CheckConvolveMonotonic(dyadic), "CPU gate: dyadic {1,2,4,8} impulse must be non-increasing");
    Require(!CheckConvolveMonotonic(nonDyadic), "CPU gate: non-dyadic {1,3,7,15} must fail monotonicity");
}

void VerifyCpuPackRgbe()
{
    const std::array<float, 9> vals = { 0.0f, 1e-4f, 1.0f, 100.0f, 2000.0f, 4088.0f, 6000.0f, 50000.0f, 100000.0f };
    float prevLum = -1.0f;
    for (float v : vals)
    {
        const auto u = CpuUnpackRGBE(CpuPackRGBE(v, 0.5f * v, 0.25f * v));
        const float lum = Luminance(u[0], u[1], u[2]);
        Require(std::isfinite(lum) && lum >= 0.0f, "CPU gate: finite packed RGBE");
        Require(lum >= prevLum * 0.99f, "CPU gate: monotonic RGBE saturation");
        if (v >= 1.0f)
        {
            Require(std::abs(u[1] / u[0] - 0.5f) < 0.03f && std::abs(u[2] / u[0] - 0.25f) < 0.03f,
                    "CPU gate: chromaticity preserved on RGBE saturation");
        }
        prevLum = lum;
    }

    const auto nanOut = CpuUnpackRGBE(CpuPackRGBE(std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f));
    const auto infOut = CpuUnpackRGBE(CpuPackRGBE(std::numeric_limits<float>::infinity(), 1.0f, 1.0f));
    Require(nanOut[0] == 0.0f && infOut[0] > 4000.0f && std::isfinite(infOut[0]), "CPU gate: NaN/+Inf RGBE");
}

void VerifyCpuBrdfWeights()
{
    const std::array<float, 7> roughs = { 0.01f, 0.02f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f };
    const std::array<float, 4> cosVals = { 0.05f, 0.25f, 0.7f, 1.0f };

    for (float r : roughs)
    {
        const float alpha = std::max(r * r, 0.005f);
        const float alpha2 = alpha * alpha;
        for (float nv : cosVals)
        {
            const float g1v = (2.0f * nv) / (nv * (2.0f - alpha) + alpha);
            for (float nl : cosVals)
            {
                const float nm = std::min(1.0f, std::sqrt(0.5f * (nv * nl + 1.0f)));
                const float denom = nm * nm * (alpha2 - 1.0f) + 1.0f;
                const float dGgx = alpha2 / (kPi * denom * denom);
                const float g2Modif = 0.5f / ((1.0f - alpha) * (2.0f * nl * nv) + alpha * (nl + nv));
                const float specPdf = (dGgx * g1v) / (4.0f * nv);
                const float brdf = dGgx * g2Modif;
                const float specWeight = (nl * brdf) / std::max(specPdf, 1e-12f);
                Require(specWeight <= 1.0001f && specWeight >= 0.0f,
                        "CPU gate: VNDF spec weight bounded by 1 (w=" + std::to_string(specWeight) + ")");
                if (r == 1.0f)
                {
                    const float diffW = std::min(nl * (1.0f / kPi) * (kPi / nl), 4.0f);
                    Require(std::abs(diffW - 1.0f) < 1e-5f, "CPU gate: Lambertian r=1 exact");
                }
            }
        }
    }
    Require(std::min(0.95f * (1.0f / kPi) * 20.0f, 4.0f) <= 4.0f, "CPU gate: glossy diffWeight clamped at 4");
}

void VerifyCpuGradientMask()
{
    auto evalGetIsGradient = [](int px, int py, uint32_t w, uint32_t h, float fltEnable,
                                const std::vector<uint32_t> &gradBuf, uint32_t bufW, uint32_t bufH) -> bool
    {
        if (fltEnable < 0.5f) return false;
        const int gx = px / 3;
        const int gy = py / 3;
        if (px < 0 || py < 0 || gx >= static_cast<int>(w / 3u) || gy >= static_cast<int>(h / 3u))
        {
            return false;
        }
        Require(static_cast<uint32_t>(gx) < bufW && static_cast<uint32_t>(gy) < bufH,
                "CPU gate: grad read must never go out of bounds");
        const uint32_t u = gradBuf[static_cast<size_t>(gy) * bufW + static_cast<size_t>(gx)];
        if ((u & (1u << 31u)) == 0u) return false;
        const int sx = static_cast<int>(u & 7u);
        const int sy = static_cast<int>((u >> 3u) & 7u);
        return (gx * 3 + sx == px) && (gy * 3 + sy == py);
    };

    const std::array<uint32_t, 3> widths = { 192u, 2560u, 194u }; // % 3 == 0, 1, 2
    for (uint32_t w : widths)
    {
        const uint32_t h = (w == 2560u) ? 1440u : w;
        const uint32_t bufW = (w + 1u) / 3u;
        const uint32_t bufH = (h + 1u) / 3u;
        std::vector<uint32_t> gradBuf(static_cast<size_t>(bufW) * bufH, 0x80000000u);
        gradBuf[0] = (1u << 31u) | (1u << 0u) | (2u << 3u); // valid stratum (1,2) in cell (0,0)
        gradBuf[1] = 0x7fffffffu;                           // bit-31 clear -> never gradient

        Require(evalGetIsGradient(1, 2, w, h, 1.0f, gradBuf, bufW, bufH), "CPU gate: valid strata hit");
        Require(!evalGetIsGradient(1, 2, w, h, 0.0f, gradBuf, bufW, bufH), "CPU gate: fltEnable=0 false");
        Require(!evalGetIsGradient(3, 0, w, h, 1.0f, gradBuf, bufW, bufH), "CPU gate: bit31=0 false");
        Require(!evalGetIsGradient(static_cast<int>(w - 1u), static_cast<int>(h - 1u), w, h, 1.0f,
                                   gradBuf, bufW, bufH),
                "CPU gate: %3!=0 border pixel never reads OOB or stale strata");
    }
}

void RunCpuMathGates()
{
    VerifyCpuAtrousImpulse();
    VerifyCpuPackRgbe();
    VerifyCpuBrdfWeights();
    VerifyCpuGradientMask();
    std::cout << "  CPU math gates: atrous impulse, RGBE saturation, VNDF/diff, grad bounds OK\n";
}

// --- Format & Readback Helpers ---

nvrhi::Format MapVkFormat(VkFormat fmt)
{
    switch (fmt)
    {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return nvrhi::Format::R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return nvrhi::Format::RGBA16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return nvrhi::Format::RG16_FLOAT;
    case VK_FORMAT_R16_SFLOAT: return nvrhi::Format::R16_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return nvrhi::Format::RGBA32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT: return nvrhi::Format::RGBA32_UINT;
    case VK_FORMAT_R32G32_SFLOAT: return nvrhi::Format::RG32_FLOAT;
    case VK_FORMAT_R32_SFLOAT: return nvrhi::Format::R32_FLOAT;
    case VK_FORMAT_R32_UINT: return nvrhi::Format::R32_UINT;
    case VK_FORMAT_R8G8B8A8_UNORM: return nvrhi::Format::RGBA8_UNORM;
    case VK_FORMAT_R8G8_UNORM: return nvrhi::Format::RG8_UNORM;
    case VK_FORMAT_R8_UINT: return nvrhi::Format::R8_UINT;
    default: Require(false, "unsupported framebuffer VkFormat"); return nvrhi::Format::UNKNOWN;
    }
}

bool IsThirdRes(FramebufferImageIndex idx)
{
    const bool isAtrousLf = idx >= FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H && idx <= FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_C_O_C_G;
    const bool isGrad = idx >= FB_IMAGE_INDEX_Q2_GRAD_L_F_PING && idx <= FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV;
    const bool isSavedGrad = (idx == FB_IMAGE_INDEX_Q2_GRAD_SAVED_SURF_POS) || (idx == FB_IMAGE_INDEX_Q2_GRAD_SAVED_VIEW_DIR);
    return isAtrousLf || isGrad || isSavedGrad;
}

float HalfToFloat(uint16_t bits)
{
    const uint32_t exponent = (bits >> 10u) & 31u;
    const uint32_t mantissa = bits & 1023u;
    float value = 0.0f;
    if (exponent == 0u)
    {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    }
    else if (exponent == 31u)
    {
        value = std::bit_cast<float>(0x7F800000u | (mantissa << 13u));
    }
    else
    {
        value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
    }
    return (bits & 0x8000u) != 0u ? -value : value;
}

float UnpackSmallFloat(uint32_t raw, uint32_t mantBits)
{
    const uint32_t mantMask = (1u << mantBits) - 1u;
    const uint32_t mant = raw & mantMask;
    const uint32_t exp = (raw >> mantBits) & 31u;
    if (exp == 0u)
    {
        return std::ldexp(static_cast<float>(mant), -15 - static_cast<int>(mantBits));
    }
    if (exp == 31u)
    {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return std::ldexp(1.0f + static_cast<float>(mant) / static_cast<float>(1u << mantBits),
                      static_cast<int>(exp) - 15);
}

void DecodeRowTexels(nvrhi::Format fmt, const uint8_t *rowBytes, uint32_t width, uint32_t channels, float *outRow)
{
    if (fmt == nvrhi::Format::R11G11B10_FLOAT)
    {
        const auto *src = reinterpret_cast<const uint32_t *>(rowBytes);
        for (uint32_t x = 0; x < width; x++)
        {
            const uint32_t p = src[x];
            outRow[x * 4u + 0u] = UnpackSmallFloat(p & 0x7FFu, 6u);
            outRow[x * 4u + 1u] = UnpackSmallFloat((p >> 11u) & 0x7FFu, 6u);
            outRow[x * 4u + 2u] = UnpackSmallFloat((p >> 22u) & 0x3FFu, 5u);
            outRow[x * 4u + 3u] = 0.0f;
        }
        return;
    }

    const auto *src16 = reinterpret_cast<const uint16_t *>(rowBytes);
    for (uint32_t x = 0; x < width; x++)
    {
        for (uint32_t c = 0; c < channels; c++)
        {
            outRow[x * channels + c] = HalfToFloat(src16[x * channels + c]);
        }
    }
}

std::vector<float> ReadFloatTexture(nvrhi::IDevice *device, nvrhi::ITexture *texture, uint32_t channels)
{
    auto desc = texture->getDesc();
    desc.isUAV = false;
    desc.keepInitialState = false;
    auto staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    Require(staging != nullptr, "create staging texture");
    auto cmd = device->createCommandList();
    cmd->open();
    cmd->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "wait for texture readback");

    size_t pitch = 0;
    const auto *data = static_cast<const uint8_t *>(
        device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    Require(data != nullptr, "map staging texture");

    std::vector<float> out(size_t(desc.width) * desc.height * channels);
    for (uint32_t y = 0; y < desc.height; y++)
    {
        DecodeRowTexels(desc.format, data + y * pitch, desc.width, channels, &out[size_t(y) * desc.width * channels]);
    }
    device->unmapStagingTexture(staging);
    return out;
}

void CheckerToFlat(uint32_t cx, uint32_t cy, uint32_t width, uint32_t &fx, uint32_t &fy)
{
    const uint32_t halfW = width / 2u;
    const bool evenField = cx < halfW;
    fx = evenField ? (cx * 2u + (cy & 1u)) : ((cx - halfW) * 2u + ((cy & 1u) ^ 1u));
    fy = cy;
}

void ClearTextureZero(nvrhi::ICommandList *cmd, nvrhi::ITexture *tex)
{
    const auto fmt = tex->getDesc().format;
    if (fmt == nvrhi::Format::R8_UINT || fmt == nvrhi::Format::R32_UINT || fmt == nvrhi::Format::RGBA32_UINT)
    {
        cmd->clearTextureUInt(tex, nvrhi::AllSubresources, 0u);
    }
    else
    {
        cmd->clearTextureFloat(tex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }
    cmd->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
}

void RequireUavBarriers(nvrhi::ICommandList *cmd, std::span<nvrhi::ITexture *const> textures)
{
    for (nvrhi::ITexture *tex : textures)
    {
        cmd->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }
    cmd->commitBarriers();
}

// --- Test Harness ---

struct PassBundle
{
    nvrhi::BindingLayoutHandle fbLayout;
    nvrhi::BindingSetHandle fbSet;
    nvrhi::ComputePipelineHandle pipeline;
};

struct SynthHarness
{
    nvrhi::IDevice *device = nullptr;
    std::array<nvrhi::TextureHandle, ShFramebuffers_Count> fb{};
    nvrhi::TextureHandle refSpecTex;
    nvrhi::TextureHandle bloomDummyTex;
    nvrhi::TextureHandle volDummyTex;
    nvrhi::SamplerHandle clampSampler;
    nvrhi::BufferHandle globalUniformBuf;
    nvrhi::BufferHandle testParamsBuf;
    nvrhi::BufferHandle tonemappingBuf;

    nvrhi::BindingLayoutHandle guLayout;
    nvrhi::BindingSetHandle guSet;
    nvrhi::BindingLayoutHandle testGbufLayout;
    nvrhi::BindingSetHandle testGbufSet;
    nvrhi::BindingLayoutHandle testShadeLayout;
    nvrhi::BindingSetHandle testShadeSet;
    nvrhi::BindingLayoutHandle pushLayout;
    nvrhi::BindingLayoutHandle pfPushLayout;
    nvrhi::BindingLayoutHandle tmLayout;
    nvrhi::BindingSetHandle tmSet;
    nvrhi::BindingLayoutHandle emptyLayout;
    nvrhi::BindingSetHandle emptySet;
    nvrhi::BindingLayoutHandle volLayout;
    nvrhi::BindingSetHandle volSet;

    PassBundle synthGbuffer;
    PassBundle synthShade;
    PassBundle gradReproj;
    PassBundle adapter;
    PassBundle gradImg;
    PassBundle gradAtrous;
    PassBundle temporal;
    PassBundle atrousLf;
    std::array<PassBundle, 4> atrous{};
    PassBundle prepareFinal;

    void ClearAndPoisonResources();
    void InitResources(nvrhi::IDevice *dev);
    PassBundle BuildPass(const std::string &spvPath, std::span<const FbEntry> entries,
                         bool withPush, nvrhi::IBindingLayout *extraSet2, const uint32_t *specIter);
    PassBundle BuildPrepareFinalPass(const std::string &spvPath);
    void InitPipelines(const std::filesystem::path &probeDir);
    void DispatchBundle(nvrhi::ICommandList *cmd, const PassBundle &pass, nvrhi::IBindingSet *extraSet2,
                        uint32_t gx, uint32_t gy, const uint32_t *pushVal) const;
    void DispatchPrepareFinal(nvrhi::ICommandList *cmd, uint32_t gx, uint32_t gy) const;
    void RecordFilteredChain(nvrhi::ICommandList *cmd, uint32_t fullGroups,
                             uint32_t gradGroups, uint32_t atrousLfGroups,
                             uint32_t temporalGroups) const;
    void RunFrame(const FrameConfig &cfg) const;
};

void SynthHarness::ClearAndPoisonResources()
{
    auto cmd = device->createCommandList();
    cmd->open();
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        ClearTextureZero(cmd, fb[i]);
    }
    cmd->clearTextureFloat(bloomDummyTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    cmd->setTextureState(bloomDummyTex, nvrhi::AllSubresources, nvrhi::ResourceStates::NonPixelShaderResource);
    cmd->clearTextureFloat(volDummyTex, nvrhi::AllSubresources, nvrhi::Color(0.f));
    cmd->setTextureState(volDummyTex, nvrhi::AllSubresources, nvrhi::ResourceStates::NonPixelShaderResource);

    std::vector<uint32_t> poisonU32(size_t(kThirdWidth) * kThirdHeight, 0u);
    std::vector<uint16_t> poisonRg16(size_t(kThirdWidth) * kThirdHeight * 2u, 0u);
    for (uint32_t y = 0; y < kThirdHeight; y++)
    {
        for (uint32_t x = 0; x < kThirdWidth; x++)
        {
            if (x >= kGradWidth || y >= kGradHeight)
            {
                const size_t idx = size_t(y) * kThirdWidth + x;
                poisonU32[idx] = 0xFFFFFFFFu;
                poisonRg16[idx * 2u + 0u] = 0x63D0u; // 1000.0f half
                poisonRg16[idx * 2u + 1u] = 0x63D0u;
            }
        }
    }

    cmd->writeTexture(fb[FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS], 0, 0, poisonU32.data(), kThirdWidth * sizeof(uint32_t));
    cmd->writeTexture(fb[FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV], 0, 0, poisonU32.data(), kThirdWidth * sizeof(uint32_t));
    cmd->writeTexture(fb[FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG], 0, 0, poisonRg16.data(), kThirdWidth * 2u * sizeof(uint16_t));
    RequireUavBarriers(cmd, std::array<nvrhi::ITexture *, 3>{
                                fb[FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS],
                                fb[FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV],
                                fb[FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG] });
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "clear and poison third-res borders");
}

void SynthHarness::InitResources(nvrhi::IDevice *dev)
{
    device = dev;
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        const bool third = IsThirdRes(static_cast<FramebufferImageIndex>(i));
        nvrhi::TextureDesc td;
        td.width = third ? kThirdWidth : kWidth;
        td.height = third ? kThirdHeight : kHeight;
        td.format = MapVkFormat(ShFramebuffers_Formats[i]);
        td.isUAV = true;
        td.initialState = nvrhi::ResourceStates::UnorderedAccess;
        td.keepInitialState = true;
        fb[i] = device->createTexture(td);
        Require(fb[i] != nullptr, "create framebuffer texture");
    }

    nvrhi::TextureDesc refDesc;
    refDesc.width = kWidth;
    refDesc.height = kHeight;
    refDesc.format = nvrhi::Format::RGBA16_FLOAT;
    refDesc.isUAV = true;
    refDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    refDesc.keepInitialState = true;
    refSpecTex = device->createTexture(refDesc);

    nvrhi::TextureDesc d2;
    d2.width = 1;
    d2.height = 1;
    d2.format = nvrhi::Format::RGBA16_FLOAT;
    d2.isUAV = true;
    d2.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    d2.keepInitialState = true;
    bloomDummyTex = device->createTexture(d2);

    d2.dimension = nvrhi::TextureDimension::Texture3D;
    d2.depth = 1;
    volDummyTex = device->createTexture(d2);

    nvrhi::SamplerDesc sd;
    sd.setMinFilter(true).setMagFilter(true).setMipFilter(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    clampSampler = device->createSampler(sd);

    nvrhi::BufferDesc cbuf;
    cbuf.isConstantBuffer = true;
    cbuf.byteSize = sizeof(ShGlobalUniform);
    cbuf.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cbuf.keepInitialState = true;
    globalUniformBuf = device->createBuffer(cbuf);
    cbuf.byteSize = sizeof(SynthTestParams);
    testParamsBuf = device->createBuffer(cbuf);

    nvrhi::BufferDesc tbuf;
    tbuf.byteSize = sizeof(ShTonemapping);
    tbuf.structStride = sizeof(ShTonemapping);
    tbuf.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    tbuf.keepInitialState = true;
    tonemappingBuf = device->createBuffer(tbuf);
    Require(refSpecTex != nullptr && bloomDummyTex != nullptr && volDummyTex != nullptr &&
            clampSampler != nullptr && globalUniformBuf != nullptr && testParamsBuf != nullptr &&
            tonemappingBuf != nullptr, "create aux resources");

    ClearAndPoisonResources();
}

nvrhi::BindingLayoutHandle MakeZeroOffsetLayout(nvrhi::IDevice *device, std::initializer_list<nvrhi::BindingLayoutItem> items)
{
    nvrhi::BindingLayoutDesc d;
    d.visibility = nvrhi::ShaderType::Compute;
    d.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                            .setShaderResourceOffset(0)
                            .setUnorderedAccessViewOffset(0)
                            .setConstantBufferOffset(0)
                            .setSamplerOffset(0));
    for (const auto &it : items) d.addItem(it);
    return device->createBindingLayout(d);
}

PassBundle SynthHarness::BuildPass(const std::string &spvPath, std::span<const FbEntry> entries,
                                     bool withPush, nvrhi::IBindingLayout *extraSet2, const uint32_t *specIter)
{
    nvrhi::BindingLayoutDesc ldesc;
    ldesc.visibility = nvrhi::ShaderType::Compute;
    ldesc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                .setShaderResourceOffset(0)
                                .setUnorderedAccessViewOffset(0)
                                .setConstantBufferOffset(0)
                                .setSamplerOffset(0));

    nvrhi::BindingSetDesc sdesc;
    for (const FbEntry &e : entries)
    {
        const uint32_t slot = e.isUav ? ShFramebuffers_Bindings[e.image] : ShFramebuffers_Sampled_Bindings[e.image];
        if (e.isUav)
        {
            ldesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(slot));
            sdesc.addItem(nvrhi::BindingSetItem::Texture_UAV(slot, fb[e.image]));
        }
        else
        {
            ldesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(slot));
            sdesc.addItem(nvrhi::BindingSetItem::Texture_SRV(slot, fb[e.image]));
        }
    }

    PassBundle out;
    out.fbLayout = device->createBindingLayout(ldesc);
    out.fbSet = device->createBindingSet(sdesc, out.fbLayout);
    Require(out.fbLayout != nullptr && out.fbSet != nullptr, "create fb binding set for " + spvPath);

    nvrhi::ShaderHandle shader = rhi::loadShader(device, spvPath, nvrhi::ShaderType::Compute, spvPath);
    Require(shader != nullptr, "load shader " + spvPath);
    if (specIter != nullptr)
    {
        const auto spec = nvrhi::ShaderSpecialization::UInt32(0, *specIter);
        shader = device->createShaderSpecialization(shader, &spec, 1);
    }

    nvrhi::ComputePipelineDesc pdesc;
    pdesc.CS = shader;
    pdesc.addBindingLayout(out.fbLayout);
    pdesc.addBindingLayout(guLayout);
    if (withPush) pdesc.addBindingLayout(pushLayout);
    if (extraSet2 != nullptr) pdesc.addBindingLayout(extraSet2);
    out.pipeline = device->createComputePipeline(pdesc);
    Require(out.pipeline != nullptr, "create pipeline " + spvPath);
    return out;
}

PassBundle SynthHarness::BuildPrepareFinalPass(const std::string &spvPath)
{
    nvrhi::BindingLayoutDesc ldesc;
    ldesc.visibility = nvrhi::ShaderType::Compute;
    ldesc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                .setShaderResourceOffset(0)
                                .setUnorderedAccessViewOffset(0)
                                .setConstantBufferOffset(0)
                                .setSamplerOffset(0));
    nvrhi::BindingSetDesc sdesc;
    for (const FbEntry &e : kPrepareFinalBindings)
    {
        const uint32_t slot = e.isUav ? ShFramebuffers_Bindings[e.image] : ShFramebuffers_Sampled_Bindings[e.image];
        ldesc.addItem(e.isUav ? nvrhi::BindingLayoutItem::Texture_UAV(slot)
                              : nvrhi::BindingLayoutItem::Texture_SRV(slot));
        sdesc.addItem(e.isUav ? nvrhi::BindingSetItem::Texture_UAV(slot, fb[e.image])
                              : nvrhi::BindingSetItem::Texture_SRV(slot, fb[e.image]));
    }
    ldesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(500));
    ldesc.addItem(nvrhi::BindingLayoutItem::Sampler(502));
    sdesc.addItem(nvrhi::BindingSetItem::Texture_SRV(500, bloomDummyTex));
    sdesc.addItem(nvrhi::BindingSetItem::Sampler(502, clampSampler));

    PassBundle out;
    out.fbLayout = device->createBindingLayout(ldesc);
    out.fbSet = device->createBindingSet(sdesc, out.fbLayout);
    nvrhi::ShaderHandle shader = rhi::loadShader(device, spvPath, nvrhi::ShaderType::Compute, spvPath);
    Require(shader != nullptr && out.fbSet != nullptr, "load CmPrepareFinal");

    nvrhi::ComputePipelineDesc pdesc;
    pdesc.CS = shader;
    pdesc.addBindingLayout(out.fbLayout);
    pdesc.addBindingLayout(guLayout);
    pdesc.addBindingLayout(tmLayout);
    pdesc.addBindingLayout(emptyLayout);
    pdesc.addBindingLayout(volLayout);
    pdesc.addBindingLayout(pfPushLayout);
    out.pipeline = device->createComputePipeline(pdesc);
    Require(out.pipeline != nullptr, "create CmPrepareFinal pipeline");
    return out;
}

void SynthHarness::InitPipelines(const std::filesystem::path &probeDir)
{
    guLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::ConstantBuffer(0) });
    nvrhi::BindingSetDesc guDesc;
    guDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, globalUniformBuf));
    guSet = device->createBindingSet(guDesc, guLayout);

    testGbufLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::ConstantBuffer(0) });
    nvrhi::BindingSetDesc tgDesc;
    tgDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, testParamsBuf));
    testGbufSet = device->createBindingSet(tgDesc, testGbufLayout);

    testShadeLayout = MakeZeroOffsetLayout(device,
        { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_UAV(1) });
    nvrhi::BindingSetDesc tsDesc;
    tsDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, testParamsBuf));
    tsDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, refSpecTex));
    testShadeSet = device->createBindingSet(tsDesc, testShadeLayout);

    pushLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::PushConstants(0, sizeof(uint32_t)) });
    pfPushLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::PushConstants(0, 48u) });

    tmLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0) });
    nvrhi::BindingSetDesc tmDesc;
    tmDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, tonemappingBuf));
    tmSet = device->createBindingSet(tmDesc, tmLayout);

    emptyLayout = MakeZeroOffsetLayout(device, {});
    emptySet = device->createBindingSet(nvrhi::BindingSetDesc(), emptyLayout);

    volLayout = MakeZeroOffsetLayout(device,
        { nvrhi::BindingLayoutItem::Texture_SRV(BINDING_VOLUMETRIC_SAMPLED),
          nvrhi::BindingLayoutItem::Sampler(BINDING_VOLUMETRIC_SAMPLER) });
    nvrhi::BindingSetDesc volDesc;
    volDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(BINDING_VOLUMETRIC_SAMPLED, volDummyTex));
    volDesc.addItem(nvrhi::BindingSetItem::Sampler(BINDING_VOLUMETRIC_SAMPLER, clampSampler));
    volSet = device->createBindingSet(volDesc, volLayout);

    ShTonemapping tm{};
    tm.tmExposureBias = 2.0f;
    tm.tmKneeStart = 1.0f;
    tm.tmWhitePoint = 1.0f;
    tm.kneeW = 1.0f;
    tm.kneeA = 1.0f;
    tm.kneeB = 1.0f;
    tm.adaptedLuminance = 1.0f;
    tm.avgLuminance = 1.0f;
    tm.tonemapType = 0u;
    auto initCmd = device->createCommandList();
    initCmd->open();
    initCmd->writeBuffer(tonemappingBuf, &tm, sizeof(tm));
    initCmd->close();
    device->executeCommandList(initCmd);
    Require(device->waitForIdle(), "init tonemapping buffer");

    const auto spv = [&](const char *name) { return (probeDir / name).string(); };
    synthGbuffer = BuildPass(spv("SynthGbuffer.comp.spv"), kGbufferBindings, false, testGbufLayout, nullptr);
    synthShade = BuildPass(spv("SynthShade.comp.spv"), kShadeBindings, false, testShadeLayout, nullptr);
    gradReproj = BuildPass(spv("CmQ2GradientReproject.comp.spv"), kReprojectBindings, false, nullptr, nullptr);
    adapter = BuildPass(spv("CmQ2Adapter.comp.spv"), kAdapterBindings, false, nullptr, nullptr);
    gradImg = BuildPass(spv("CmQ2GradientImg.comp.spv"), kGradImgBindings, false, nullptr, nullptr);
    gradAtrous = BuildPass(spv("CmQ2GradientAtrous.comp.spv"), kGradAtrousBindings, true, nullptr, nullptr);
    temporal = BuildPass(spv("CmQ2Temporal.comp.spv"), kTemporalBindings, false, nullptr, nullptr);
    atrousLf = BuildPass(spv("CmQ2AtrousLF.comp.spv"), kAtrousLfBindings, true, nullptr, nullptr);
    for (uint32_t i = 0; i < 4; i++)
    {
        atrous[i] = BuildPass(spv("CmQ2Atrous.comp.spv"), kAtrousBindings, false, nullptr, &i);
    }
    prepareFinal = BuildPrepareFinalPass(spv("CmPrepareFinal.comp.spv"));
}

void SynthHarness::DispatchBundle(nvrhi::ICommandList *cmd, const PassBundle &pass, nvrhi::IBindingSet *extraSet2,
                                  uint32_t gx, uint32_t gy, const uint32_t *pushVal) const
{
    nvrhi::ComputeState state;
    state.setPipeline(pass.pipeline);
    state.addBindingSet(pass.fbSet);
    state.addBindingSet(guSet);
    if (extraSet2 != nullptr) state.addBindingSet(extraSet2);
    cmd->setComputeState(state);
    if (pushVal != nullptr) cmd->setPushConstants(pushVal, sizeof(uint32_t));
    cmd->dispatch(gx, gy, 1);
}

void SynthHarness::DispatchPrepareFinal(nvrhi::ICommandList *cmd, uint32_t gx, uint32_t gy) const
{
    const float opticalControl[12] = {};
    nvrhi::ComputeState state;
    state.setPipeline(prepareFinal.pipeline);
    state.addBindingSet(prepareFinal.fbSet);
    state.addBindingSet(guSet);
    state.addBindingSet(tmSet);
    state.addBindingSet(emptySet);
    state.addBindingSet(volSet);
    cmd->setComputeState(state);
    cmd->setPushConstants(opticalControl, sizeof(opticalControl));
    cmd->dispatch(gx, gy, 1);
}

void SynthHarness::RecordFilteredChain(nvrhi::ICommandList *cmd, uint32_t fullGroups,
                                       uint32_t gradGroups, uint32_t atrousLfGroups,
                                       uint32_t temporalGroups) const
{
    DispatchBundle(cmd, gradImg, nullptr, gradGroups, gradGroups, nullptr);
    for (uint32_t iter = 0; iter < 7; iter++)
    {
        DispatchBundle(cmd, gradAtrous, nullptr, gradGroups, gradGroups, &iter);
        if (iter + 1u < 7u)
        {
            const auto p0 = (iter % 2u == 0u) ? FB_IMAGE_INDEX_Q2_GRAD_L_F_PONG : FB_IMAGE_INDEX_Q2_GRAD_L_F_PING;
            const auto p1 = (iter % 2u == 0u) ? FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG : FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING;
            RequireUavBarriers(cmd, std::array<nvrhi::ITexture *, 2>{ fb[p0], fb[p1] });
        }
    }
    DispatchBundle(cmd, temporal, nullptr, temporalGroups, temporalGroups, nullptr);
    for (uint32_t iter = 0; iter < 4; iter++)
    {
        DispatchBundle(cmd, atrousLf, nullptr, atrousLfGroups, atrousLfGroups, &iter);
        if (iter + 1u < 4u)
        {
            const auto p0 = (iter % 2u == 0u) ? FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_S_H : FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H;
            const auto p1 = (iter % 2u == 0u) ? FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_C_O_C_G : FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G;
            RequireUavBarriers(cmd, std::array<nvrhi::ITexture *, 2>{ fb[p0], fb[p1] });
        }
    }
    static constexpr FramebufferImageIndex kAtrousWrites[3][3] = {
        { FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS },
        { FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS },
        { FB_IMAGE_INDEX_Q2_ATROUS_PONG_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS },
    };
    for (uint32_t iter = 0; iter < 4; iter++)
    {
        DispatchBundle(cmd, atrous[iter], nullptr, fullGroups, fullGroups, nullptr);
        if (iter < 3u)
        {
            RequireUavBarriers(cmd, std::array<nvrhi::ITexture *, 3>{
                fb[kAtrousWrites[iter][0]], fb[kAtrousWrites[iter][1]], fb[kAtrousWrites[iter][2]] });
        }
    }
}

void SynthHarness::RunFrame(const FrameConfig &cfg) const
{
    ShGlobalUniform gu{};
    static constexpr float kIdent[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const float camCur[3] = { cfg.camPosX, 0.0f, 3.0f };
    const float camPrev[3] = { cfg.prevCamPosX, 0.0f, 3.0f };
    Matrix::SetNewViewerPosition(gu.view, kIdent, camCur);
    Matrix::SetNewViewerPosition(gu.viewPrev, kIdent, camPrev);
    Matrix::MakeProjectionMatrix(gu.projection, 1.0f, kPi * 0.5f, 0.1f, 1000.0f);
    std::memcpy(gu.projectionPrev, gu.projection, sizeof(gu.projection));
    Matrix::Inverse(gu.invView, gu.view);
    Matrix::Inverse(gu.invProjection, gu.projection);

    gu.renderWidth = static_cast<float>(kWidth);
    gu.renderHeight = static_cast<float>(kHeight);
    gu.upscaledRenderWidth = static_cast<float>(kWidth);
    gu.upscaledRenderHeight = static_cast<float>(kHeight);
    gu.rayLength = 1000.0f;
    gu.coreQ2RTX = 1u;
    gu.frameId = cfg.frameId;
    gu.giBounceRays[0] = cfg.bounceRays;
    gu.fltEnable[0] = cfg.fltEnable;
    gu.debugShowFlags = cfg.debugFlags;
    gu.restirParams[2] = cfg.resetHistory ? 1u : 0u;

    SynthTestParams tp{};
    tp.computeRef = cfg.computeRef;
    tp.lightOn = cfg.lightOn;
    tp.forceZeroHitDist = cfg.forceZeroHitDist;
    tp.coplanarStep = cfg.coplanarStep;
    tp.camPosX = cfg.camPosX;
    tp.prevCamPosX = cfg.prevCamPosX;

    const uint32_t fullGroups = (kWidth + 15u) / 16u;
    const uint32_t gradGroups = (kGradWidth + 15u) / 16u;
    const uint32_t atrousLfGroups = (kThirdWidth + 15u) / 16u;
    const uint32_t temporalGroups = (kWidth + 14u) / 15u;

    auto cmd = device->createCommandList();
    cmd->open();
    cmd->writeBuffer(globalUniformBuf, &gu, sizeof(gu));
    cmd->writeBuffer(testParamsBuf, &tp, sizeof(tp));

    DispatchBundle(cmd, synthGbuffer, testGbufSet, fullGroups, fullGroups, nullptr);
    if (cfg.fltEnable >= 0.5f)
    {
        DispatchBundle(cmd, gradReproj, nullptr, gradGroups, gradGroups, nullptr);
    }
    DispatchBundle(cmd, synthShade, testShadeSet, fullGroups, fullGroups, nullptr);
    DispatchBundle(cmd, adapter, nullptr, fullGroups, fullGroups, nullptr);

    if (cfg.fltEnable >= 0.5f)
    {
        RecordFilteredChain(cmd, fullGroups, gradGroups, atrousLfGroups, temporalGroups);
    }

    if (cfg.runPrepareFinal)
    {
        DispatchPrepareFinal(cmd, fullGroups, fullGroups);
    }

    for (const auto &[srcIdx, dstIdx] : kPrevCopies)
    {
        cmd->copyTexture(fb[dstIdx], nvrhi::TextureSlice(), fb[srcIdx], nvrhi::TextureSlice());
    }
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "execute denoiser frame");
}

// --- Measurement & Statistics ---

struct RegionRect
{
    uint32_t xMin;
    uint32_t xMax;
    uint32_t yMin;
    uint32_t yMax;
    float roS = 1.0f;
};

struct BandStats
{
    double meanHistLen = 0.0;
    double meanLum = 0.0;
    double meanRefLum = 0.0;
    double evenRowLum = 0.0;
    double oddRowLum = 0.0;
    double refEvenRowLum = 0.0;
    double refOddRowLum = 0.0;
    double rmseVsRef = 0.0;
    double rmseUnfiltVsRef = 0.0;
    double stdDevLum = 0.0;
    float maxLum = 0.0f;
    double zebraRatio() const
    {
        const double raw = evenRowLum / std::max(oddRowLum, 1e-4);
        const double ref = refEvenRowLum / std::max(refOddRowLum, 1e-4);
        return raw / std::max(ref, 1e-4);
    }
};

BandStats MeasureRegion(const std::vector<float> &q2Color, const std::vector<float> &filtSpec,
                        const std::vector<float> &unfiltIndir, const std::vector<float> &refSpec,
                        const RegionRect &rect)
{
    double sumHist = 0.0, sumLum = 0.0, sumRefLum = 0.0, sumLumSq = 0.0, sumEven = 0.0, sumOdd = 0.0;
    double sumRefEven = 0.0, sumRefOdd = 0.0, sumSqErr = 0.0, sumSqErrUnfilt = 0.0;
    uint32_t count = 0, evenCount = 0, oddCount = 0, unfiltCount = 0;
    float maxL = 0.0f;

    for (uint32_t cy = 0; cy < kHeight; cy++)
    {
        for (uint32_t cx = 0; cx < kWidth; cx++)
        {
            uint32_t fx = 0, fy = 0;
            CheckerToFlat(cx, cy, kWidth, fx, fy);
            if (fx < rect.xMin || fx >= rect.xMax || fy < rect.yMin || fy >= rect.yMax) continue;

            const size_t p = (size_t(cy) * kWidth + cx) * 4u;
            const float r = q2Color[p], g = q2Color[p + 1], b = q2Color[p + 2];
            const float rr = refSpec[p], rg = refSpec[p + 1], rb = refSpec[p + 2];
            Require(std::isfinite(r) && std::isfinite(g) && std::isfinite(b), "finite denoised output");

            const float lum = Luminance(r, g, b);
            const float refLum = Luminance(rr, rg, rb);
            maxL = std::max(maxL, lum);
            sumLum += lum;
            sumRefLum += refLum;
            sumLumSq += double(lum) * lum;
            sumHist += filtSpec[p + 3];

            if ((fy & 1u) == 0u) { sumEven += lum; sumRefEven += refLum; evenCount++; }
            else { sumOdd += lum; sumRefOdd += refLum; oddCount++; }

            const double dr = r - rr, dg = g - rg, db = b - rb;
            sumSqErr += (dr * dr + dg * dg + db * db) / 3.0;
            if (unfiltIndir[p + 3] > 0.5f)
            {
                const double ur = unfiltIndir[p] * rect.roS - rr;
                const double ug = unfiltIndir[p + 1] * rect.roS - rg;
                const double ub = unfiltIndir[p + 2] * rect.roS - rb;
                sumSqErrUnfilt += (ur * ur + ug * ug + ub * ub) / 3.0;
                unfiltCount++;
            }
            count++;
        }
    }

    Require(count > 0 && evenCount > 0 && oddCount > 0, "non-empty measurement region");
    BandStats s;
    const double n = static_cast<double>(count);
    s.meanHistLen = sumHist / n;
    s.meanLum = sumLum / n;
    s.meanRefLum = sumRefLum / n;
    s.evenRowLum = sumEven / static_cast<double>(evenCount);
    s.oddRowLum = sumOdd / static_cast<double>(oddCount);
    s.refEvenRowLum = sumRefEven / static_cast<double>(evenCount);
    s.refOddRowLum = sumRefOdd / static_cast<double>(oddCount);
    s.rmseVsRef = std::sqrt(sumSqErr / n);
    s.rmseUnfiltVsRef = (unfiltCount > 0) ? std::sqrt(sumSqErrUnfilt / static_cast<double>(unfiltCount)) : 0.0;
    s.stdDevLum = std::sqrt(std::max(0.0, sumLumSq / n - s.meanLum * s.meanLum));
    s.maxLum = maxL;
    return s;
}

BandStats MeasureRegularRegion(const std::vector<float> &regRgba, const RegionRect &rect)
{
    double sumLum = 0.0, sumEven = 0.0, sumOdd = 0.0;
    uint32_t count = 0, evenCount = 0, oddCount = 0;
    float maxL = 0.0f;

    for (uint32_t y = rect.yMin; y < rect.yMax; y++)
    {
        for (uint32_t x = rect.xMin; x < rect.xMax; x++)
        {
            const size_t p = (size_t(y) * kWidth + x) * 4u;
            const float r = regRgba[p], g = regRgba[p + 1], b = regRgba[p + 2];
            Require(std::isfinite(r) && std::isfinite(g) && std::isfinite(b), "finite regular image output");
            const float lum = Luminance(r, g, b);
            maxL = std::max(maxL, lum);
            sumLum += lum;
            if ((y & 1u) == 0u) { sumEven += lum; evenCount++; }
            else { sumOdd += lum; oddCount++; }
            count++;
        }
    }

    Require(count > 0 && evenCount > 0 && oddCount > 0, "non-empty regular region");
    BandStats s;
    s.meanLum = sumLum / static_cast<double>(count);
    s.evenRowLum = sumEven / static_cast<double>(evenCount);
    s.oddRowLum = sumOdd / static_cast<double>(oddCount);
    s.refEvenRowLum = 1.0;
    s.refOddRowLum = 1.0;
    s.maxLum = maxL;
    return s;
}

struct FrameReadback
{
    std::vector<float> q2Color;
    std::vector<float> filtSpec;
    std::vector<float> unfiltIndir;
    std::vector<float> refSpec;
};

FrameReadback ReadFrameBuffers(const SynthHarness &h)
{
    return {
        ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_COLOR], 4),
        ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4),
        ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR], 4),
        ReadFloatTexture(h.device, h.refSpecTex, 4),
    };
}

void CheckAllBandsZebra(const FrameReadback &rb, double minHist, double zebraLo, double zebraHi, uint32_t xMax)
{
    const uint32_t bandH = kHeight / 6u;
    for (uint32_t b = 0; b < 5; b++)
    {
        const uint32_t rxMax = (b == 0) ? 112u : xMax;
        const auto s = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec,
                                     { 12, rxMax, b * bandH + 4, (b + 1) * bandH - 4 });
        const double zr = s.zebraRatio();
        if (s.meanHistLen <= minHist || zr <= zebraLo || zr >= zebraHi)
        {
            std::cerr << "  [CheckAllBandsZebra minHist=" << minHist << " band=" << b
                      << "] histLen=" << s.meanHistLen << " zebra=" << zr
                      << " meanLum=" << s.meanLum << " refLum=" << s.meanRefLum << "\n";
        }
        Require(s.meanHistLen > minHist, "hist_len_spec threshold failed on band " + std::to_string(b));
        Require(zr > zebraLo && zr < zebraHi,
                "zebra striping on band " + std::to_string(b) + " ratio=" + std::to_string(zr));
    }
}

void VerifyBand5ContrastAndFirefly(const FrameReadback &rb)
{
    const uint32_t bandH = kHeight / 6u;
    const auto dirty = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec,
                                     { 8, kWidth, 5 * bandH + 4, kHeight });
    Require(dirty.maxLum < 40.0f, "Band 5 fireflies suppressed (maxLum=" + std::to_string(dirty.maxLum) + ")");

    double sumSmooth = 0.0, sumRough = 0.0;
    uint32_t nSmooth = 0, nRough = 0;
    for (uint32_t cy = 0; cy < kHeight; cy++)
    {
        for (uint32_t cx = 0; cx < kWidth; cx++)
        {
            uint32_t fx = 0, fy = 0;
            CheckerToFlat(cx, cy, kWidth, fx, fy);
            if (fx < 32 || fx >= kWidth - 32 || fy < 5 * bandH + 4 || fy >= kHeight - 4) continue;
            const size_t p = (size_t(cy) * kWidth + cx) * 4u;
            const double lum = Luminance(rb.q2Color[p], rb.q2Color[p + 1], rb.q2Color[p + 2]);
            if (((fx ^ fy) & 1u) != 0u) { sumRough += lum; nRough++; }
            else { sumSmooth += lum; nSmooth++; }
        }
    }
    const double diff = std::abs(sumSmooth / nSmooth - sumRough / nRough);
    Require(diff > 0.02, "Band 5 smooth vs rough checkerboard preserves distinct response (diff=" + std::to_string(diff) + ")");
}

void VerifyStaticHalfRes(const SynthHarness &h)
{
    const uint32_t bandH = kHeight / 6u;
    for (uint32_t f = 0; f < 64; f++)
    {
        FrameConfig cfg;
        cfg.frameId = f;
        cfg.bounceRays = 0.5f;
        cfg.resetHistory = (f == 0);
        cfg.computeRef = (f == 0 || f == 1 || f == 63) ? 1u : 0u;
        cfg.debugFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        h.RunFrame(cfg);

        if (f == 0 || f == 1)
        {
            CheckAllBandsZebra(ReadFrameBuffers(h), 0.5, 0.92, 1.08, kWidth - 12);
        }
    }

    const auto rb = ReadFrameBuffers(h);
    CheckAllBandsZebra(rb, 28.0, 0.95, 1.05, kWidth);

    const auto plateACenter = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec, { 88, 106, 4, bandH - 4 });
    const auto plateASide = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec, { 20, 64, 4, bandH - 4 });
    Require(plateACenter.meanLum > plateASide.meanLum * 4.0, "Plate A mirror edge contrast preserved");

    // Band 0 grazing dielectric highlight monotonicity (center > shoulders; zero RGBE overflow dark hole).
    const auto peak = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec, { 145, 152, 10, 22 });
    const auto shL = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec, { 128, 136, 10, 22 });
    const auto shR = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec, { 160, 168, 10, 22 });
    Require(peak.meanLum > shL.meanLum * 1.15 && peak.meanLum > shR.meanLum * 1.15,
            "Band 0 RGBE dielectric highlight peak monotonic without overflow hole");

    const std::array<float, 4> kBandRoS = { 1.0f, 0.904f, 0.808f, 0.616f };
    for (uint32_t b : { 1u, 2u, 3u })
    {
        const auto sb = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec,
                                      { 16, kWidth - 16, b * bandH + 4, (b + 1) * bandH - 4, kBandRoS[b] });
        const double energyErr = std::abs(sb.meanLum - sb.meanRefLum) / std::max(sb.meanRefLum, 1e-4);
        Require(energyErr < 0.12, "specular energy conservation on band " + std::to_string(b));
        Require(sb.rmseVsRef < sb.rmseUnfiltVsRef * 0.6,
                "denoised RMSE beats 1-spp on band " + std::to_string(b));
    }

    const auto cLeft = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec,
                                     { 24, 88, 2 * bandH + 4, 3 * bandH - 4, kBandRoS[2] });
    const auto cRight = MeasureRegion(rb.q2Color, rb.filtSpec, rb.unfiltIndir, rb.refSpec,
                                      { 106, 170, 2 * bandH + 4, 3 * bandH - 4, kBandRoS[2] });
    const double cRatio = (cLeft.meanLum / std::max(cLeft.meanRefLum, 1e-4)) /
                          std::max(cRight.meanLum / std::max(cRight.meanRefLum, 1e-4), 1e-4);
    Require(cRatio > 0.90 && cRatio < 1.10, "Plate C specular invariant across HF shadow edge");
    Require(cLeft.rmseVsRef < 0.25 && cRight.rmseVsRef < 0.25, "Plate C accurate across shadow halves");
    Require(std::abs(cLeft.stdDevLum - cRight.stdDevLum) < 0.12, "Plate C matched smoothness");

    VerifyBand5ContrastAndFirefly(rb);
    std::cout << "  Static halfRes: F0/F1/F63 zebra, energy, RGBE peak, Plate A/C/5 OK\n";
}

void VerifyIndirectDiffuseHalfResEnergy(const SynthHarness &h)
{
    auto runDiff = [&](float bounceRays) {
        for (uint32_t f = 0; f < 16; f++)
        {
            FrameConfig cfg;
            cfg.frameId = f;
            cfg.bounceRays = bounceRays;
            cfg.resetHistory = (f == 0);
            cfg.debugFlags = DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE;
            h.RunFrame(cfg);
        }
        const auto q2Color = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_COLOR], 4);
        const auto filtSpec = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4);
        const auto unfiltIndir = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR], 4);
        return MeasureRegion(q2Color, filtSpec, unfiltIndir, q2Color, { 20, kWidth - 20, 40, kHeight - 40 });
    };

    const auto halfStats = runDiff(0.5f);
    const auto fullStats = runDiff(1.0f);
    const double relDiff = std::abs(halfStats.meanLum - fullStats.meanLum) / std::max(fullStats.meanLum, 1e-4);
    const double rawZr = halfStats.evenRowLum / std::max(halfStats.oddRowLum, 1e-4);
    std::cout << "  Indirect diffuse halfRes vs fullRes: half=" << halfStats.meanLum
              << " full=" << fullStats.meanLum << " rawE/O=" << rawZr << "\n";
    Require(relDiff < 0.05, "1x indirect diffuse energy conserved at halfRes");
    Require(rawZr > 0.96 && rawZr < 1.04, "no indirect diffuse zebra striping at halfRes");
}

void VerifyStrafeParallaxAndOcclusion(const SynthHarness &h)
{
    const uint32_t bandH = kHeight / 6u;
    auto runStrafe = [&](uint32_t forceZero) {
        for (uint32_t f = 0; f < 36; f++)
        {
            FrameConfig cfg;
            cfg.frameId = f;
            cfg.bounceRays = 0.5f;
            cfg.resetHistory = (f == 0);
            cfg.forceZeroHitDist = forceZero;
            cfg.camPosX = -0.63f + 0.035f * static_cast<float>(f);
            cfg.prevCamPosX = (f == 0) ? cfg.camPosX : (-0.63f + 0.035f * static_cast<float>(f - 1));
            cfg.computeRef = (f == 35) ? 1u : 0u;
            cfg.debugFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
            h.RunFrame(cfg);
        }
        const auto q2Color = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_COLOR], 4);
        const auto filtSpec = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4);
        const auto unfiltIndir = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_UNFILTERED_INDIRECT_SPECULAR], 4);
        const auto refSpec = ReadFloatTexture(h.device, h.refSpecTex, 4);
        return MeasureRegion(q2Color, filtSpec, unfiltIndir, refSpec,
                             { 32, kWidth - 32, 4 * bandH + 4, 5 * bandH - 4 });
    };

    const auto virtStats = runStrafe(0);
    const auto gradStrafe = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG], 2);
    double sumGS = 0.0;
    float maxGS = 0.0f;
    uint32_t cntGS = 0, maxGx = 0, maxGy = 0;
    for (uint32_t gy = (4u * bandH + 4u) / 3u; gy < (5u * bandH - 4u) / 3u; gy++)
    {
        for (uint32_t gx = 16u / 3u; gx < (kWidth - 16u) / 3u; gx++)
        {
            const float g = gradStrafe[(size_t(gy) * kThirdWidth + gx) * 2u + 1u];
            if (g > maxGS) { maxGS = g; maxGx = gx; maxGy = gy; }
            sumGS += g;
            cntGS++;
        }
    }
    const auto surfStats = runStrafe(1);
    std::cout << "  Strafe parallax (Plate E): virtual(hist=" << virtStats.meanHistLen << ", rmse=" << virtStats.rmseVsRef
              << ", meanGrad=" << (sumGS / std::max(cntGS, 1u)) << ", maxGrad=" << maxGS
              << "@(" << maxGx << "," << maxGy << ")"
              << ", zebra=" << virtStats.zebraRatio() << ") vs surfaceCtrl(hist=" << surfStats.meanHistLen
              << ", rmse=" << surfStats.rmseVsRef << ")\n";
    Require(virtStats.meanHistLen > 14.0 && virtStats.meanHistLen > surfStats.meanHistLen * 3.0,
            "Plate E high history during strafe");
    Require(virtStats.zebraRatio() > 0.94 && virtStats.zebraRatio() < 1.06, "Plate E no strafe zebra");
    Require(virtStats.rmseVsRef < surfStats.rmseVsRef * 0.80, "virtual parallax beats surface control");

    // Coplanar depth-step occlusion check: planeDist == 0, Q2ViewDepth jumps 2.4x across x = kWidth / 2.
    for (uint32_t f = 0; f < 16; f++)
    {
        FrameConfig cfg;
        cfg.frameId = f;
        cfg.bounceRays = 0.5f;
        cfg.resetHistory = (f == 0);
        cfg.coplanarStep = 1u;
        cfg.camPosX = -0.5f + 0.08f * static_cast<float>(f);
        cfg.prevCamPosX = (f == 0) ? cfg.camPosX : (-0.5f + 0.08f * static_cast<float>(f - 1));
        cfg.debugFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        h.RunFrame(cfg);
    }
    const auto occlSpec = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4);
    const auto stepCross = MeasureRegion(occlSpec, occlSpec, occlSpec, occlSpec,
                                         { kWidth / 2u - 4u, kWidth / 2u + 4u, 4 * bandH + 4, 5 * bandH - 4 });
    Require(stepCross.meanHistLen < 6.0,
            "coplanar depth step rejects crossing history (hist=" + std::to_string(stepCross.meanHistLen) + ")");
}

void VerifyHistoryLenProbe(const SynthHarness &h)
{
    const uint32_t bandH = kHeight / 6u;
    for (uint32_t f = 0; f < 40; f++)
    {
        FrameConfig cfg;
        cfg.frameId = f;
        cfg.bounceRays = 0.5f;
        cfg.resetHistory = (f == 0);
        cfg.debugFlags = (f == 39) ? QR_DEBUG_DRAW_SPEC_HISTORY_LEN_BIT : 0u;
        cfg.runPrepareFinal = (f == 39);
        h.RunFrame(cfg);
    }

    const auto probeHistFinal = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4);
    const auto filtBefore = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4);
    double sumHistDiff = 0.0, sumHistVal = 0.0;
    uint32_t histN = 0;
    for (uint32_t cy = 0; cy < kHeight; cy++)
    {
        for (uint32_t cx = 0; cx < kWidth; cx++)
        {
            uint32_t fx = 0, fy = 0;
            CheckerToFlat(cx, cy, kWidth, fx, fy);
            if (fy < 4 || fy >= bandH - 4 || fx < 20 || fx >= 112) continue;
            const double expVal = std::clamp(double(filtBefore[(size_t(cy) * kWidth + cx) * 4u + 3u]) / 64.0, 0.0, 1.0);
            const double gotVal = probeHistFinal[(size_t(fy) * kWidth + fx) * 4u];
            sumHistDiff += std::abs(gotVal - expVal);
            sumHistVal += gotVal;
            histN++;
        }
    }
    Require((sumHistDiff / histN) < 0.02 && (sumHistVal / histN) > 0.40,
            "CmPrepareFinal probe 1<<20 matches hist_len_spec/64");

    FrameConfig fltOffHist;
    fltOffHist.frameId = 39;
    fltOffHist.bounceRays = 0.5f;
    fltOffHist.fltEnable = 0.0f;
    fltOffHist.debugFlags = QR_DEBUG_DRAW_SPEC_HISTORY_LEN_BIT;
    fltOffHist.runPrepareFinal = true;
    h.RunFrame(fltOffHist);
    const auto offHistStats = MeasureRegularRegion(ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4),
                                                   { 20, 112, 4, bandH - 4 });
    Require(offHistStats.maxLum < 1e-4f, "CmPrepareFinal 1<<20 returns 0 when fltEnable == 0");
}

void VerifyPoisonedGradBorderProbe(const std::vector<float> &gradPong,
                                   const std::vector<float> &probeGradFinal)
{
    float maxBorderGradDiff = 0.0f;
    float maxBorderGradVal = 0.0f;
    for (uint32_t cy = 0; cy < kHeight; cy++)
    {
        for (uint32_t cx = 0; cx < kWidth; cx++)
        {
            if (cx / 3u < kGradWidth && cy / 3u < kGradHeight) continue;
            Require(gradPong[(size_t(cy / 3u) * kThirdWidth + (cx / 3u)) * 2u + 1u] > 100.0f,
                    "poisoned %3 border texel stays untouched in gradPong");
            uint32_t fx = 0, fy = 0;
            CheckerToFlat(cx, cy, kWidth, fx, fy);
            const uint32_t cgx = std::min(cx / 3u, kGradWidth - 1u);
            const uint32_t cgy = std::min(cy / 3u, kGradHeight - 1u);
            const float expected = std::clamp(
                gradPong[(size_t(cgy) * kThirdWidth + cgx) * 2u + 1u], 0.0f, 1.0f);
            const float got = probeGradFinal[(size_t(fy) * kWidth + fx) * 4u];
            maxBorderGradVal = std::max(maxBorderGradVal, got);
            maxBorderGradDiff = std::max(maxBorderGradDiff, std::abs(got - expected));
        }
    }
    Require(maxBorderGradDiff < 0.02f && maxBorderGradVal < 0.99f,
            "CmPrepareFinal 1<<21 clamps %3 border instead of reading poisoned texels");
}

void VerifyGradProbeAndDecay(const SynthHarness &h)
{
    const uint32_t bandH = kHeight / 6u;
    FrameConfig offCfg;
    offCfg.frameId = 40;
    offCfg.bounceRays = 0.5f;
    offCfg.lightOn = 0;
    offCfg.debugFlags = QR_DEBUG_DRAW_SPEC_GRADIENT_BIT;
    offCfg.runPrepareFinal = true;
    h.RunFrame(offCfg);

    const auto probeGradFinal = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4);
    const auto gradPong = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG], 2);
    const auto filtAfter = ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], 4);
    float maxGradSpec = 0.0f;
    double sumGradDiff = 0.0, sumHistDrop = 0.0;
    uint32_t stripN = 0;
    for (uint32_t cy = 0; cy < kHeight; cy++)
    {
        for (uint32_t cx = 0; cx < kWidth; cx++)
        {
            uint32_t fx = 0, fy = 0;
            CheckerToFlat(cx, cy, kWidth, fx, fy);
            if (fy < 4 || fy >= bandH - 4 || fx < 88 || fx >= 106) continue;
            const float gSpec = gradPong[(size_t(cy / 3u) * kThirdWidth + (cx / 3u)) * 2u + 1u];
            maxGradSpec = std::max(maxGradSpec, gSpec);
            const double gotGrad = probeGradFinal[(size_t(fy) * kWidth + fx) * 4u];
            sumGradDiff += std::abs(gotGrad - std::clamp(double(gSpec), 0.0, 1.0));
            sumHistDrop += filtAfter[(size_t(cy) * kWidth + cx) * 4u + 3u];
            stripN++;
        }
    }
    VerifyPoisonedGradBorderProbe(gradPong, probeGradFinal);
    Require(maxGradSpec > 0.30f && (sumGradDiff / stripN) < 0.02 && (sumHistDrop / stripN) < 8.0,
            "CmPrepareFinal 1<<21 matches grad_hf_spec.y and drops history");

    for (uint32_t f = 41; f <= 46; f++)
    {
        FrameConfig decayCfg;
        decayCfg.frameId = f;
        decayCfg.bounceRays = 0.5f;
        decayCfg.lightOn = 0;
        decayCfg.debugFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        h.RunFrame(decayCfg);
    }
    const auto rbDec = ReadFrameBuffers(h);
    const auto decayStats = MeasureRegion(rbDec.q2Color, rbDec.filtSpec, rbDec.unfiltIndir, rbDec.q2Color,
                                          { 88, 106, 4, bandH - 4 });
    Require(decayStats.meanLum < 0.8, "ghost reflection decays after light-off");

    FrameConfig fltOffGrad;
    fltOffGrad.frameId = 47;
    fltOffGrad.bounceRays = 0.5f;
    fltOffGrad.fltEnable = 0.0f;
    fltOffGrad.lightOn = 0;
    fltOffGrad.debugFlags = QR_DEBUG_DRAW_SPEC_GRADIENT_BIT;
    fltOffGrad.runPrepareFinal = true;
    h.RunFrame(fltOffGrad);
    const auto offGradStats = MeasureRegularRegion(ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4),
                                                   { 88, 106, 4, bandH - 4 });
    Require(offGradStats.maxLum < 1e-4f, "CmPrepareFinal 1<<21 returns 0 when fltEnable == 0");
    std::cout << "  CmPrepareFinal probes OK (grad=" << maxGradSpec << ", decay=" << decayStats.meanLum << ")\n";
}

void VerifyUnfilteredDebugViews(const SynthHarness &h)
{
    const uint32_t bandH = kHeight / 6u;
    FrameConfig unfiltSpecCfg;
    unfiltSpecCfg.frameId = 48;
    unfiltSpecCfg.bounceRays = 0.5f;
    unfiltSpecCfg.fltEnable = 0.0f;
    unfiltSpecCfg.debugFlags = DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR;
    unfiltSpecCfg.runPrepareFinal = true;
    h.RunFrame(unfiltSpecCfg);
    const auto usStats = MeasureRegularRegion(ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4),
                                              { 16, 64, bandH + 4, 2 * bandH - 4 });
    const double usZebra = usStats.evenRowLum / std::max(usStats.oddRowLum, 1e-4);
    Require(usStats.meanLum > 0.05 && usZebra > 0.92 && usZebra < 1.08,
            "CmPrepareFinal UNFILTERED_SPECULAR reconstructs odd halfRes rows");

    FrameConfig unfiltIndirCfg;
    unfiltIndirCfg.frameId = 49;
    unfiltIndirCfg.bounceRays = 0.5f;
    unfiltIndirCfg.fltEnable = 0.0f;
    unfiltIndirCfg.debugFlags = DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT;
    unfiltIndirCfg.runPrepareFinal = true;
    h.RunFrame(unfiltIndirCfg);
    const auto uiStats = MeasureRegularRegion(ReadFloatTexture(h.device, h.fb[FB_IMAGE_INDEX_FINAL], 4),
                                              { 16, 64, bandH + 4, 2 * bandH - 4 });
    const double uiZebra = uiStats.evenRowLum / std::max(uiStats.oddRowLum, 1e-4);
    Require(uiStats.meanLum > 0.15 && uiZebra > 0.95 && uiZebra < 1.05,
            "CmPrepareFinal UNFILTERED_INDIRECT reconstructs odd halfRes rows");
}

void VerifyPrepareFinalAndProbes(const SynthHarness &h)
{
    VerifyHistoryLenProbe(h);
    VerifyGradProbeAndDecay(h);
    VerifyUnfilteredDebugViews(h);
}

std::array<nvrhi::TextureHandle, ShFramebuffers_Count> CreateHighResAtrousTextures(
    nvrhi::IDevice *device, uint32_t hw, uint32_t hh)
{
    const uint32_t htw = (hw + 2u) / 3u;
    const uint32_t hth = (hh + 2u) / 3u;
    std::array<nvrhi::TextureHandle, ShFramebuffers_Count> texs{};
    for (const FbEntry &e : kAtrousBindings)
    {
        if (texs[e.image] != nullptr) continue;
        const bool third = IsThirdRes(e.image);
        nvrhi::TextureDesc td;
        td.width = third ? htw : hw;
        td.height = third ? hth : hh;
        td.format = MapVkFormat(ShFramebuffers_Formats[e.image]);
        td.isUAV = true;
        td.initialState = nvrhi::ResourceStates::UnorderedAccess;
        td.keepInitialState = true;
        texs[e.image] = device->createTexture(td);
        Require(texs[e.image] != nullptr, "create hiRes atrous texture");
    }
    return texs;
}

void PopulateHighResAtrousScene(nvrhi::ICommandList *cmd,
                                const std::array<nvrhi::TextureHandle, ShFramebuffers_Count> &texs,
                                uint32_t hw, uint32_t hh)
{
    for (const FbEntry &e : kAtrousBindings)
    {
        if (texs[e.image] != nullptr) ClearTextureZero(cmd, texs[e.image]);
    }
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_ALBEDO], nvrhi::AllSubresources, nvrhi::Color(1.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_THROUGHPUT], nvrhi::AllSubresources, nvrhi::Color(1.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_DEPTH_GRAD], nvrhi::AllSubresources, nvrhi::Color(250.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_Q2_VIEW_DEPTH], nvrhi::AllSubresources, nvrhi::Color(3.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_VIEW_DIRECTION], nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, -1.f, 3.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_METALLIC_ROUGHNESS], nvrhi::AllSubresources, nvrhi::Color(1.f, 0.5f, 0.f, 0.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_Q2_FILTERED_SPEC], nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, 0.f, 1.f));
    cmd->clearTextureFloat(texs[FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS], nvrhi::AllSubresources, nvrhi::Color(0.f, 0.f, 4.f, 50000.f));

    std::vector<uint32_t> specInit(size_t(hw) * hh, 0u);
    specInit[size_t(722u) * hw + 16u] = CpuPackRGBE(16.0f, 16.0f, 16.0f);
    cmd->writeTexture(texs[FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC], 0, 0, specInit.data(), hw * sizeof(uint32_t));
    for (const FbEntry &e : kAtrousBindings)
    {
        cmd->setTextureState(texs[e.image], nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }
}

void VerifyHighResAtrousNoRinging(nvrhi::IDevice *device, const std::filesystem::path &probeDir)
{
    constexpr uint32_t hw = 64u, hh = 1446u;
    const auto texs = CreateHighResAtrousTextures(device, hw, hh);
    auto cmd = device->createCommandList();
    cmd->open();
    PopulateHighResAtrousScene(cmd, texs, hw, hh);

    auto guLayout = MakeZeroOffsetLayout(device, { nvrhi::BindingLayoutItem::ConstantBuffer(0) });
    nvrhi::BufferDesc cbuf;
    cbuf.isConstantBuffer = true;
    cbuf.byteSize = sizeof(ShGlobalUniform);
    cbuf.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cbuf.keepInitialState = true;
    auto guBuf = device->createBuffer(cbuf);
    nvrhi::BindingSetDesc guDesc;
    guDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, guBuf));
    auto guSet = device->createBindingSet(guDesc, guLayout);

    ShGlobalUniform gu{};
    gu.renderWidth = static_cast<float>(hw);
    gu.renderHeight = static_cast<float>(hh);
    gu.fltEnable[0] = 1.0f;
    gu.debugShowFlags = DEBUG_SHOW_FLAG_ONLY_SPECULAR;
    cmd->writeBuffer(guBuf, &gu, sizeof(gu));

    nvrhi::BindingLayoutDesc ldesc;
    ldesc.visibility = nvrhi::ShaderType::Compute;
    ldesc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                .setShaderResourceOffset(0).setUnorderedAccessViewOffset(0)
                                .setConstantBufferOffset(0).setSamplerOffset(0));
    nvrhi::BindingSetDesc sdesc;
    for (const FbEntry &e : kAtrousBindings)
    {
        const uint32_t slot = e.isUav ? ShFramebuffers_Bindings[e.image] : ShFramebuffers_Sampled_Bindings[e.image];
        ldesc.addItem(e.isUav ? nvrhi::BindingLayoutItem::Texture_UAV(slot) : nvrhi::BindingLayoutItem::Texture_SRV(slot));
        sdesc.addItem(e.isUav ? nvrhi::BindingSetItem::Texture_UAV(slot, texs[e.image])
                              : nvrhi::BindingSetItem::Texture_SRV(slot, texs[e.image]));
    }
    auto fbLayout = device->createBindingLayout(ldesc);
    auto fbSet = device->createBindingSet(sdesc, fbLayout);
    auto baseShader = rhi::loadShader(device, (probeDir / "CmQ2Atrous.comp.spv").string(),
                                      nvrhi::ShaderType::Compute, "CmQ2Atrous");

    static constexpr FramebufferImageIndex kWrites[3][3] = {
        { FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS },
        { FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS },
        { FB_IMAGE_INDEX_Q2_ATROUS_PONG_H_F, FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC, FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS },
    };
    const uint32_t gx = (hw + 15u) / 16u, gy = (hh + 15u) / 16u;
    for (uint32_t iter = 0; iter < 4; iter++)
    {
        const auto spec = nvrhi::ShaderSpecialization::UInt32(0, iter);
        auto sh = device->createShaderSpecialization(baseShader, &spec, 1);
        nvrhi::ComputePipelineDesc pdesc;
        pdesc.CS = sh;
        pdesc.addBindingLayout(fbLayout);
        pdesc.addBindingLayout(guLayout);
        auto pipe = device->createComputePipeline(pdesc);
        nvrhi::ComputeState state;
        state.setPipeline(pipe);
        state.addBindingSet(fbSet);
        state.addBindingSet(guSet);
        cmd->setComputeState(state);
        cmd->dispatch(gx, gy, 1);
        if (iter < 3u)
        {
            RequireUavBarriers(cmd, std::array<nvrhi::ITexture *, 3>{
                texs[kWrites[iter][0]], texs[kWrites[iter][1]], texs[kWrites[iter][2]] });
        }
    }
    cmd->close();
    device->executeCommandList(cmd);
    Require(device->waitForIdle(), "hiRes atrous impulse dispatch");

    const auto out = ReadFloatTexture(device, texs[FB_IMAGE_INDEX_Q2_COLOR], 4);
    std::array<float, 16> profile{};
    for (uint32_t dy = 0; dy < 16u; dy++)
    {
        const size_t p = (size_t(722u + dy) * hw + 16u) * 4u;
        profile[dy] = Luminance(out[p], out[p + 1], out[p + 2]);
    }
    Require(profile[0] > profile[8] && profile[8] > 0.0f, "hiRes >1440p atrous spreads impulse");
    for (uint32_t dy = 0; dy < 15u; dy++)
    {
        Require(profile[dy] + 1e-4f >= profile[dy + 1u],
                "hiRes >1440p atrous non-ringing violated at dy=" + std::to_string(dy));
    }
    std::cout << "  GPU >1440p atrous impulse non-ringing OK (L0=" << profile[0] << " L8=" << profile[8] << ")\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        Require(argc >= 3, "usage: qray_reflections_denoiser_smoke <BuildDir> <ProbeDir>");
        RunCpuMathGates();

        VulkanTestContext gpu;
        Require(gpu.validationActive, "VK_LAYER_KHRONOS_validation must be active");
        auto *device = gpu.context.GetDevice();
        {
            SynthHarness harness;
            harness.InitResources(device);
            harness.InitPipelines(argv[2]);

            VerifyStaticHalfRes(harness);
            VerifyIndirectDiffuseHalfResEnergy(harness);
            VerifyStrafeParallaxAndOcclusion(harness);
            VerifyPrepareFinalAndProbes(harness);
            VerifyHighResAtrousNoRinging(device, argv[2]);
            Require(device->waitForIdle(), "final waitForIdle");
        }
        Require(gpu.errors.load() == 0, "zero Vulkan validation errors");
        std::cout << "PASS: reflections & ASVGF specular denoiser regression suite\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
