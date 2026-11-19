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
// The CPU twin of Shaders/Restir.hlsli: the weighted-reservoir merge the direct raygen runs for
// temporal and spatial light reuse. The GPU side stores this exact 16-byte record per pixel and
// slot, so the struct layout, the valid-sample predicate and the weight formula have to stay in
// step with the HLSL.
//
// A reservoir summarises M source samples drawn with p(x):
//   weightSum = sum_i target(x_i) / p(x_i)
// and the estimator of the target integral is
//   W * target(x_selected),  W = weightSum / (M * target(x_selected)).
//
// Reusing a reservoir at a new pixel with a new target t(y) adds its selected sample with the
// importance ratio t(y) / p(x_selected). The reservoir itself only carries the aggregate, so the
// ratio uses the standard ReSTIR identity
//   t(y) / p(x_selected) = t(y) * W * M / target(x_selected),
// i.e. the candidate contributes t(y) * weightSum / target(x_selected) to the new weight sum and
// its whole source-sample count to the new M.

#pragma once

#include <cmath>
#include <cstdint>

namespace qray
{

inline constexpr uint32_t kRestirLightIndexNone = 32767u; // LIGHT_INDEX_NONE
inline constexpr uint32_t kRestirMaxSamples = 64u;

struct RestirReservoir
{
    uint32_t lightIndex = kRestirLightIndexNone;
    uint32_t sampleCount = 0;
    float    weightSum = 0.0f;
    float    selectedTarget = 0.0f;
};

static_assert(sizeof(RestirReservoir) == 16, "ShRestirReservoir has to keep the 16-byte GPU layout");

inline bool RestirIsFinite(float value)
{
    return std::isfinite(value);
}

inline bool RestirIsUsable(const RestirReservoir &reservoir)
{
    return reservoir.lightIndex < kRestirLightIndexNone &&
           reservoir.sampleCount > 0u &&
           reservoir.sampleCount <= kRestirMaxSamples &&
           RestirIsFinite(reservoir.weightSum) &&
           reservoir.weightSum > 0.0f &&
           RestirIsFinite(reservoir.selectedTarget) &&
           reservoir.selectedTarget > 0.0f;
}

inline float RestirWeight(const RestirReservoir &reservoir)
{
    if (!RestirIsUsable(reservoir))
    {
        return 0.0f;
    }

    return reservoir.weightSum / (float(reservoir.sampleCount) * reservoir.selectedTarget);
}

// Adds one source sample. `weight` is target(x) / p(x) and `target` is target(x). The random
// number `u` in [0, 1) picks the new representative with probability weight / weightSum.
inline void RestirAddSample(RestirReservoir &reservoir,
                            uint32_t lightIndex,
                            float weight,
                            float target,
                            float u)
{
    if (!RestirIsFinite(weight) || weight < 0.0f)
    {
        weight = 0.0f;
    }
    if (!RestirIsFinite(target) || target < 0.0f)
    {
        target = 0.0f;
    }

    reservoir.sampleCount += 1u;
    reservoir.weightSum += weight;

    if (weight > 0.0f && target > 0.0f && u * reservoir.weightSum < weight)
    {
        reservoir.lightIndex = lightIndex;
        reservoir.selectedTarget = target;
    }
}

// Merges a reservoir produced at another pixel or in another frame, with `targetAtNewPixel` the
// target of its selected light evaluated at the receiving pixel. `u` in [0, 1) is the reservoir
// draw for the merged candidate.
inline void RestirMerge(RestirReservoir &destination,
                        const RestirReservoir &source,
                        float targetAtNewPixel,
                        float u)
{
    if (!RestirIsUsable(source) || !RestirIsFinite(targetAtNewPixel) || !(targetAtNewPixel > 0.0f))
    {
        return;
    }

    const float contribution = targetAtNewPixel * (source.weightSum / source.selectedTarget);

    if (!RestirIsFinite(contribution) || !(contribution > 0.0f))
    {
        return;
    }

    destination.sampleCount += source.sampleCount;
    destination.weightSum += contribution;

    if (u * destination.weightSum < contribution)
    {
        destination.lightIndex = source.lightIndex;
        destination.selectedTarget = targetAtNewPixel;
    }
}

// Bounds the history length without changing the estimator: scaling weightSum and sampleCount by
// the same factor leaves W = weightSum / (sampleCount * target) untouched.
inline void RestirClampSamples(RestirReservoir &reservoir, uint32_t maxSamples)
{
    if (maxSamples == 0u || reservoir.sampleCount <= maxSamples)
    {
        return;
    }

    const float scale = float(maxSamples) / float(reservoir.sampleCount);
    reservoir.weightSum *= scale;
    reservoir.sampleCount = maxSamples;
}

}
