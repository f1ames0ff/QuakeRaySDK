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
