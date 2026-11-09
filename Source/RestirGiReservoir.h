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
// CPU twin of the indirect ReSTIR reservoir (ReSTIR GI). A reservoir summarises bounce samples
// with p(x) and stores the selected sample's direction, distance and outgoing radiance:
//   weightSum = sum_i target(x_i) / p(x_i)
//   W = weightSum / (M * target(x_selected))
// Reusing a reservoir at another pixel adds target(y) * weightSum / target(x_selected) to the new
// weight sum and its whole sample count to M, exactly like the direct-light twin. The GPU side
// will store this 48-byte record per pixel and slot; the layout, the usable predicate and the
// weight formula have to stay in step with the HLSL.

#pragma once

#include <cmath>
#include <cstdint>

namespace qray
{

inline constexpr uint32_t kRestirGiMaxSamples = 64u;
inline constexpr uint32_t kRestirGiSlots = 2u;
inline constexpr uint32_t kRestirGiMaxAgeFrames = 64u;

struct RestirGiReservoir
{
    // xyz = selected bounce direction, w = hit distance.
    float direction[4] = {};
    // xyz = selected sample outgoing radiance, w = padding.
    float radiance[4] = {};
    float weightSum = 0.0f;
    float selectedTarget = 0.0f;
    uint32_t sampleCount = 0u;
    uint32_t pad = 0u;
};

static_assert(sizeof(RestirGiReservoir) == 48, "RestirGiReservoir has to keep the 48-byte GPU layout");

// Octahedral normal + frame packing: 8+8 bits of normal in the low 16 bits of `pad`, the frame
// in the high 16. Mirrors q2GiPackNormalFrame / q2GiHistoryValid in RestirGi.hlsli.
inline void RestirGiOctEncode(const float n[3], float out[2])
{
    const float sum = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    const float inv = 1.0f / (sum > 1e-6f ? sum : 1e-6f);
    float x = n[0] * inv;
    float y = n[1] * inv;
    const float z = n[2] * inv;

    if (z < 0.0f)
    {
        const float wx = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float wy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = wx;
        y = wy;
    }

    out[0] = x;
    out[1] = y;
}

inline void RestirGiOctDecode(const float e[2], float out[3])
{
    float x = e[0];
    float y = e[1];
    float z = 1.0f - std::abs(x) - std::abs(y);

    if (z < 0.0f)
    {
        const float wx = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float wy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = wx;
        y = wy;
    }

    const float length = std::sqrt(x * x + y * y + z * z);
    const float inv = length > 1e-6f ? 1.0f / length : 0.0f;
    out[0] = x * inv;
    out[1] = y * inv;
    out[2] = z * inv;
}

inline uint32_t RestirGiPackNormalFrame(const float normal[3], uint32_t frame)
{
    float e[2];
    RestirGiOctEncode(normal, e);

    const auto quantize = [](float value)
    {
        float q = value * 0.5f + 0.5f;
        q = q < 0.0f ? 0.0f : (q > 1.0f ? 1.0f : q);
        return uint32_t(q * 255.0f + 0.5f);
    };

    const uint32_t qx = quantize(e[0]);
    const uint32_t qy = quantize(e[1]);
    return ((frame & 0xFFFFu) << 16) | (qy << 8) | qx;
}

inline void RestirGiUnpackNormal(uint32_t packed, float out[3])
{
    const float e[2] = {
        float(packed & 0xFFu) / 255.0f * 2.0f - 1.0f,
        float((packed >> 8) & 0xFFu) / 255.0f * 2.0f - 1.0f
    };
    RestirGiOctDecode(e, out);
}

inline uint32_t RestirGiUnpackFrame(uint32_t packed)
{
    return (packed >> 16) & 0xFFFFu;
}

inline bool RestirGiHistoryValid(const RestirGiReservoir &reservoir, const float currentNormal[3],
                                 uint32_t frame)
{
    const uint32_t age = (frame - RestirGiUnpackFrame(reservoir.pad)) & 0xFFFFu;

    if (age > kRestirGiMaxAgeFrames)
    {
        return false;
    }

    float historyNormal[3];
    RestirGiUnpackNormal(reservoir.pad, historyNormal);

    const float length = std::sqrt(currentNormal[0] * currentNormal[0] +
                                   currentNormal[1] * currentNormal[1] +
                                   currentNormal[2] * currentNormal[2]);
    if (length <= 1e-6f)
    {
        return false;
    }

    return (historyNormal[0] * currentNormal[0] + historyNormal[1] * currentNormal[1] +
            historyNormal[2] * currentNormal[2]) / length >= 0.9f;
}

inline bool RestirGiFinite(float value)
{
    return std::isfinite(value);
}

inline bool RestirGiIsUsable(const RestirGiReservoir &reservoir)
{
    if (reservoir.sampleCount == 0u || reservoir.sampleCount > kRestirGiMaxSamples)
    {
        return false;
    }

    if (!RestirGiFinite(reservoir.weightSum) || !(reservoir.weightSum > 0.0f) ||
        !RestirGiFinite(reservoir.selectedTarget) || !(reservoir.selectedTarget > 0.0f))
    {
        return false;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!RestirGiFinite(reservoir.direction[i]) || !RestirGiFinite(reservoir.radiance[i]))
        {
            return false;
        }
    }

    if (!RestirGiFinite(reservoir.direction[3]) || reservoir.direction[3] < 0.0f)
    {
        return false;
    }

    const float lengthSquared = reservoir.direction[0] * reservoir.direction[0] +
                                reservoir.direction[1] * reservoir.direction[1] +
                                reservoir.direction[2] * reservoir.direction[2];
    return lengthSquared > 0.0f;
}

inline float RestirGiWeight(const RestirGiReservoir &reservoir)
{
    if (!RestirGiIsUsable(reservoir))
    {
        return 0.0f;
    }

    return reservoir.weightSum / (float(reservoir.sampleCount) * reservoir.selectedTarget);
}

// Adds one source sample. `weight` is target(x) / p(x) and `target` is target(x). The random
// number `u` in [0, 1) picks the new representative with probability weight / weightSum.
inline void RestirGiAddSample(RestirGiReservoir &reservoir,
                              const float direction[3],
                              float distance,
                              const float radiance[3],
                              float weight,
                              float target,
                              float u)
{
    if (!RestirGiFinite(weight) || weight < 0.0f)
    {
        weight = 0.0f;
    }
    if (!RestirGiFinite(target) || target < 0.0f)
    {
        target = 0.0f;
    }

    reservoir.sampleCount += 1u;
    reservoir.weightSum += weight;

    if (weight > 0.0f && target > 0.0f && u * reservoir.weightSum < weight)
    {
        reservoir.direction[0] = direction[0];
        reservoir.direction[1] = direction[1];
        reservoir.direction[2] = direction[2];
        reservoir.direction[3] = distance;
        reservoir.radiance[0] = radiance[0];
        reservoir.radiance[1] = radiance[1];
        reservoir.radiance[2] = radiance[2];
        reservoir.selectedTarget = target;
    }
}

// Merges a reservoir produced at another pixel or in another frame. `targetAtNewPixel` is the
// target of its selected direction evaluated at the receiving pixel; `u` in [0, 1) is the
// reservoir draw for the merged candidate.
inline void RestirGiMerge(RestirGiReservoir &destination,
                          const RestirGiReservoir &source,
                          float targetAtNewPixel,
                          float u)
{
    if (!RestirGiIsUsable(source) || !RestirGiFinite(targetAtNewPixel) || !(targetAtNewPixel > 0.0f))
    {
        return;
    }

    const float weight = targetAtNewPixel * source.weightSum / source.selectedTarget;

    if (!RestirGiFinite(weight) || weight < 0.0f)
    {
        return;
    }

    destination.sampleCount += source.sampleCount;
    destination.weightSum += weight;

    if (weight > 0.0f && u * destination.weightSum < weight)
    {
        destination.direction[0] = source.direction[0];
        destination.direction[1] = source.direction[1];
        destination.direction[2] = source.direction[2];
        destination.direction[3] = source.direction[3];
        destination.radiance[0] = source.radiance[0];
        destination.radiance[1] = source.radiance[1];
        destination.radiance[2] = source.radiance[2];
        destination.selectedTarget = targetAtNewPixel;
    }
}

// History cap: scales the sample count and the weight sum together, so W is unchanged and old
// history fades out instead of being biased.
inline void RestirGiClampHistory(RestirGiReservoir &reservoir)
{
    if (reservoir.sampleCount <= kRestirGiMaxSamples)
    {
        return;
    }

    const float scale = float(kRestirGiMaxSamples) / float(reservoir.sampleCount);
    reservoir.sampleCount = kRestirGiMaxSamples;
    reservoir.weightSum *= scale;
}

}
