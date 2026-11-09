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
// GPU counterpart of Source/RestirGiReservoir.h: the 48-byte indirect reservoir, the target/pdf
// estimator, the merge identity and the 64-sample history cap. The buffer is indexed by the
// regular render pixel and two history slots; the pass writes the current slot and reads the
// previous one.

#ifndef RESTIR_GI_HLSLI_
#define RESTIR_GI_HLSLI_

#define Q2_GI_RESERVOIR_SLOTS 2u
#define Q2_GI_MAX_SAMPLES 64u

struct ShRestirGiReservoir
{
    float4 direction;
    float4 radiance;
    float  weightSum;
    float  selectedTarget;
    uint   sampleCount;
    uint   pad;
};

[[vk::binding(0, DESC_SET_GI_RESERVOIRS)]]
RWStructuredBuffer<ShRestirGiReservoir> giReservoirs;

bool q2GiUsable(const ShRestirGiReservoir reservoir)
{
    if (reservoir.sampleCount == 0u || reservoir.sampleCount > Q2_GI_MAX_SAMPLES)
    {
        return false;
    }

    if (!isfinite(reservoir.weightSum) || !(reservoir.weightSum > 0.0) ||
        !isfinite(reservoir.selectedTarget) || !(reservoir.selectedTarget > 0.0))
    {
        return false;
    }

    for (int i = 0; i < 3; i++)
    {
        if (!isfinite(reservoir.direction[i]) || !isfinite(reservoir.radiance[i]))
        {
            return false;
        }
    }

    if (!isfinite(reservoir.direction.w) || reservoir.direction.w < 0.0)
    {
        return false;
    }

    const float3 direction = reservoir.direction.xyz;
    return dot(direction, direction) > 0.0;
}

float q2GiWeight(const ShRestirGiReservoir reservoir)
{
    if (!q2GiUsable(reservoir))
    {
        return 0.0;
    }

    return reservoir.weightSum / (float(reservoir.sampleCount) * reservoir.selectedTarget);
}

void q2GiAddSample(inout ShRestirGiReservoir reservoir,
                   const float3 direction, const float distance, const float3 radiance,
                   const float weight, const float target, const float u)
{
    float safeWeight = weight;
    float safeTarget = target;

    if (!isfinite(safeWeight) || safeWeight < 0.0)
    {
        safeWeight = 0.0;
    }
    if (!isfinite(safeTarget) || safeTarget < 0.0)
    {
        safeTarget = 0.0;
    }

    reservoir.sampleCount += 1u;
    reservoir.weightSum += safeWeight;

    if (safeWeight > 0.0 && safeTarget > 0.0 && u * reservoir.weightSum < safeWeight)
    {
        reservoir.direction = float4(direction, distance);
        reservoir.radiance = float4(radiance, 0.0);
        reservoir.selectedTarget = safeTarget;
    }
}

void q2GiMerge(inout ShRestirGiReservoir destination,
               const ShRestirGiReservoir source,
               const float targetAtNewPixel, const float u)
{
    if (!q2GiUsable(source) || !isfinite(targetAtNewPixel) || !(targetAtNewPixel > 0.0))
    {
        return;
    }

    const float weight = targetAtNewPixel * source.weightSum / source.selectedTarget;

    if (!isfinite(weight) || weight < 0.0)
    {
        return;
    }

    destination.sampleCount += source.sampleCount;
    destination.weightSum += weight;

    if (weight > 0.0 && u * destination.weightSum < weight)
    {
        destination.direction = source.direction;
        destination.radiance = source.radiance;
        destination.selectedTarget = targetAtNewPixel;
    }
}

void q2GiClampHistory(inout ShRestirGiReservoir reservoir)
{
    if (reservoir.sampleCount <= Q2_GI_MAX_SAMPLES)
    {
        return;
    }

    const float scale = float(Q2_GI_MAX_SAMPLES) / float(reservoir.sampleCount);
    reservoir.sampleCount = Q2_GI_MAX_SAMPLES;
    reservoir.weightSum *= scale;
}

bool q2GiSurfaceMatches(const Surface previous, const Surface current)
{
    const float3 delta = previous.position - current.position;
    const float distanceToCamera = length(current.position - globalUniform.cameraPosition.xyz);
    const float tolerance = 0.05 + 0.002 * distanceToCamera;

    if (!isfinite(tolerance) || dot(delta, delta) > tolerance * tolerance)
    {
        return false;
    }

    const float previousLengthSq = dot(previous.normalGeom, previous.normalGeom);
    const float currentLengthSq = dot(current.normalGeom, current.normalGeom);

    if (!(previousLengthSq > 1e-8) || !(currentLengthSq > 1e-8))
    {
        return false;
    }

    return dot(previous.normalGeom * rsqrt(previousLengthSq),
               current.normalGeom * rsqrt(currentLengthSq)) > 0.9;
}

uint q2GiPixelIndex(const int2 regularPix)
{
    const uint width = uint(globalUniform.renderWidth);
    const uint height = uint(globalUniform.renderHeight);
    const uint x = min(uint(max(regularPix.x, 0)), width - 1u);
    const uint y = min(uint(max(regularPix.y, 0)), height - 1u);
    return y * width + x;
}

uint q2GiSlotBase(const uint slot)
{
    return slot * uint(globalUniform.renderWidth) * uint(globalUniform.renderHeight);
}

#endif
