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
// The weighted-reservoir record and merge of Source/RestirReservoir.h. The direct raygen stores
// one record per packed pixel and time slot; slot parity follows globalUniform.frameId, so the
// buffer is a two-slot ring and every dispatch reads the other slot's previous frame.
//
// The math is the CPU twin's: weightSum = sum target(x_i) / p(x_i), W = weightSum /
// (sampleCount * target(selected)), and a reused reservoir contributes its selected light with
// weight target(y) * weightSum / target(x_selected), carrying its source-sample count.

#ifndef RESTIR_HLSLI_
#define RESTIR_HLSLI_

#define Q2_RESTIR_MAX_SAMPLES 64u

struct ShRestirReservoir
{
    uint  lightIndex;
    uint  sampleCount;
    float weightSum;
    float selectedTarget;
};

#if defined(DESC_SET_LIGHT_SOURCES)
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_RESTIR_RESERVOIRS, DESC_SET_LIGHT_SOURCES)]]
RWStructuredBuffer<ShRestirReservoir> restirReservoirs;
#endif

bool q2RestirIsFinite(const float value)
{
    return isfinite(value);
}

bool q2RestirIsUsable(const ShRestirReservoir reservoir)
{
    return reservoir.lightIndex < uint(LIGHT_INDEX_NONE) &&
           reservoir.sampleCount > 0u &&
           reservoir.sampleCount <= Q2_RESTIR_MAX_SAMPLES &&
           q2RestirIsFinite(reservoir.weightSum) &&
           reservoir.weightSum > 0.0 &&
           q2RestirIsFinite(reservoir.selectedTarget) &&
           reservoir.selectedTarget > 0.0;
}

float q2RestirWeight(const ShRestirReservoir reservoir)
{
    if (!q2RestirIsUsable(reservoir))
    {
        return 0.0;
    }

    return reservoir.weightSum / (float(reservoir.sampleCount) * reservoir.selectedTarget);
}

void q2RestirAddSample(inout ShRestirReservoir reservoir, const uint lightIndex,
                       float weight, float target, const float u)
{
    if (!q2RestirIsFinite(weight) || weight < 0.0)
    {
        weight = 0.0;
    }
    if (!q2RestirIsFinite(target) || target < 0.0)
    {
        target = 0.0;
    }

    reservoir.sampleCount += 1u;
    reservoir.weightSum += weight;

    if (weight > 0.0 && target > 0.0 && u * reservoir.weightSum < weight)
    {
        reservoir.lightIndex = lightIndex;
        reservoir.selectedTarget = target;
    }
}

void q2RestirMerge(inout ShRestirReservoir destination, const ShRestirReservoir source,
                   const float targetAtNewPixel, const float u)
{
    if (!q2RestirIsUsable(source) ||
        !q2RestirIsFinite(targetAtNewPixel) || !(targetAtNewPixel > 0.0))
    {
        return;
    }

    const float contribution = targetAtNewPixel * (source.weightSum / source.selectedTarget);

    if (!q2RestirIsFinite(contribution) || !(contribution > 0.0))
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

void q2RestirClampSamples(inout ShRestirReservoir reservoir, const uint maxSamples)
{
    if (maxSamples == 0u || reservoir.sampleCount <= maxSamples)
    {
        return;
    }

    const float scale = float(maxSamples) / float(reservoir.sampleCount);
    reservoir.weightSum *= scale;
    reservoir.sampleCount = maxSamples;
}

#endif
