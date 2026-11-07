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
// The direct-light ReSTIR pass. It builds a reservoir from the frame's candidates (the world-grid
// cell when the grid is available, the global light array otherwise), then merges the previous
// frame's reservoir at the reprojected pixel and four spatial neighbours from that same history
// slot. Only the history slot is read, so the dispatch has no intra-frame reservoir race; the
// current slot is written once per pixel at the end.
//
// What is deliberately conservative in this first cut:
//   * Temporal reprojection uses the G-buffer motion vectors (frame N-1) and validates the
//     previous surface position and normal. A moving object is rejected instead of smeared.
//   * History is capped at Q2_RESTIR_MAX_SAMPLES source samples; the clamp scales weightSum and
//     sampleCount together, so the estimator is unchanged and old history fades out.
//   * Spatial taps come from the history slot too (4 taps, 2 pixels away). They are validated
//     against the same world position and normal, so an edge does not leak a neighbour's light.
//   * The target is the existing globalLightTarget; with the grid the candidate source pdf is the
//     grid sample's pdf, so target/pdf is the RIS weight over the grid distribution.

#ifndef RESTIR_SAMPLING_HLSLI_
#define RESTIR_SAMPLING_HLSLI_

#include "Restir.hlsli"

#define Q2_RNG_RESTIR_TEMPORAL 448
#define Q2_RNG_RESTIR_SPATIAL_BASE 452
#define Q2_RESTIR_SPATIAL_TAPS 4u
#define Q2_RESTIR_SPATIAL_STEP 2

uint q2RestirPixelIndex(const int2 pix, const uint2 resolution)
{
    const uint x = min(uint(max(pix.x, 0)), resolution.x - 1u);
    const uint y = min(uint(max(pix.y, 0)), resolution.y - 1u);
    return y * resolution.x + x;
}

int2 q2RestirSpatialOffset(const uint tap)
{
    if (tap == 0u)
    {
        return int2(Q2_RESTIR_SPATIAL_STEP, 0);
    }
    if (tap == 1u)
    {
        return int2(-Q2_RESTIR_SPATIAL_STEP, 0);
    }
    if (tap == 2u)
    {
        return int2(0, Q2_RESTIR_SPATIAL_STEP);
    }

    return int2(0, -Q2_RESTIR_SPATIAL_STEP);
}

bool q2RestirHistoryUsable(const ShRestirReservoir reservoir)
{
    return q2RestirIsUsable(reservoir) &&
           reservoir.lightIndex >= uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) &&
           reservoir.lightIndex < uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) + globalUniform.lightCount;
}

void q2RestirStoreEmpty(const int2 pix)
{
    const uint2 resolution = uint2(uint(globalUniform.renderWidth), uint(globalUniform.renderHeight));

    if (resolution.x == 0u || resolution.y == 0u)
    {
        return;
    }

    ShRestirReservoir empty;
    empty.lightIndex = uint(LIGHT_INDEX_NONE);
    empty.sampleCount = 0u;
    empty.weightSum = 0.0;
    empty.selectedTarget = 0.0;

    const uint parity = globalUniform.frameId & 1u;
    const uint pixelCount = resolution.x * resolution.y;
    restirReservoirs[parity * pixelCount + q2RestirPixelIndex(pix, resolution)] = empty;
}

bool q2RestirSurfaceMatches(const Surface previous, const Surface current)
{
    const float3 delta = previous.position - current.position;
    const float distanceToCamera = length(current.position - globalUniform.cameraPosition.xyz);
    const float tolerance = 0.05 + 0.002 * distanceToCamera;

    if (!q2RestirIsFinite(tolerance) || dot(delta, delta) > tolerance * tolerance)
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

void q2RestirMergeHistory(inout ShRestirReservoir reservoir, const uint historyBase,
                          const uint2 resolution, const int2 historyRegular,
                          const Surface surf, const float phongExp, const float phongScale,
                          const float phongWeight, const uint seed, const uint salt)
{
    const uint historyIndex = historyBase +
        q2RestirPixelIndex(getCheckerboardPix(historyRegular), resolution);
    const ShRestirReservoir history = restirReservoirs[historyIndex];

    if (!q2RestirHistoryUsable(history))
    {
        return;
    }

    const float target = globalLightTarget(lightSources[history.lightIndex], surf.position,
                                           surf.normal, surf.toViewerDir,
                                           phongExp, phongScale, phongWeight);

    q2RestirMerge(reservoir, history, target, rnd16(seed, salt));
}

void q2SampleRestirLights(const uint seed, const int2 pix, const int2 regularPix, const Surface surf,
                          const int gridCell, const float phongExp, const float phongScale,
                          const float phongWeight,
                          out ShRestirReservoir outReservoir, out float outWeight)
{
    outReservoir.lightIndex = uint(LIGHT_INDEX_NONE);
    outReservoir.sampleCount = 0u;
    outReservoir.weightSum = 0.0;
    outReservoir.selectedTarget = 0.0;
    outWeight = 0.0;

    const uint2 resolution = uint2(uint(globalUniform.renderWidth), uint(globalUniform.renderHeight));

    if (resolution.x == 0u || resolution.y == 0u)
    {
        return;
    }

    const uint pixelCount = resolution.x * resolution.y;
    const uint parity = globalUniform.frameId & 1u;
    const uint currentBase = parity * pixelCount;
    const uint historyBase = (parity ^ 1u) * pixelCount;
    const uint currentIndex = currentBase + q2RestirPixelIndex(pix, resolution);

    const uint count = globalUniform.lightCount;

    if (count == 0u)
    {
        restirReservoirs[currentIndex] = outReservoir;
        return;
    }

    const uint candidates = min(min(max(globalUniform.restirParams.y, 1u), count),
                                Q2_RESTIR_MAX_CANDIDATES);

    /* Initial candidates: the grid distribution when the world-space grid covers this point,
       uniform over the light array otherwise. In both cases the weight is target / sourcePdf,
       which makes the reservoir's W equal to the legacy path's 1 / lightPdf for one frame. */
    for (uint j = 0u; j < candidates; j++)
    {
        const uint salt = (uint)Q2_RNG_RESTIR_BASE + j * 4u;

        if (gridCell >= 0)
        {
            const float3 rng = float3(
                rnd24(seed, salt),
                rnd24(seed, salt + 1u),
                rnd24(seed, salt + 2u));

            uint lightIndex = uint(LIGHT_INDEX_NONE);
            uint lightSlot = 0u;
            float lightPdf = 0.0;

            q2SampleGridLights(gridCell, surf.position, surf.normal, surf.toViewerDir,
                               phongExp, phongScale, phongWeight, rng,
                               lightIndex, lightSlot, lightPdf);

            if (lightIndex != uint(LIGHT_INDEX_NONE) && lightPdf > 0.0)
            {
                const float target = globalLightTarget(lightSources[lightIndex], surf.position,
                                                       surf.normal, surf.toViewerDir,
                                                       phongExp, phongScale, phongWeight);
                q2RestirAddSample(outReservoir, lightIndex, target / lightPdf, target,
                                  rnd16(seed, salt + 3u));
            }
        }
        else
        {
            const uint lightIndex = (uint)LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET +
                min((uint)(rnd16(seed, salt) * float(count)), count - 1u);
            const float target = globalLightTarget(lightSources[lightIndex], surf.position,
                                                   surf.normal, surf.toViewerDir,
                                                   phongExp, phongScale, phongWeight);

            q2RestirAddSample(outReservoir, lightIndex, target * float(count), target,
                              rnd16(seed, salt + 1u));
        }
    }

    // An empty grid cell or all-zero targets must not take direct lighting away: fall back to the
    // uniform array.
    if (!q2RestirIsUsable(outReservoir))
    {
        const uint fallbackCandidates = min(min(max(globalUniform.restirParams.y, 1u), count),
                                            Q2_RESTIR_MAX_CANDIDATES);

        for (uint j = 0u; j < fallbackCandidates; j++)
        {
            const uint salt = (uint)Q2_RNG_RESTIR_BASE + j * 4u;
            const uint lightIndex = (uint)LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET +
                min((uint)(rnd16(seed, salt + 2u) * float(count)), count - 1u);
            const float target = globalLightTarget(lightSources[lightIndex], surf.position,
                                                   surf.normal, surf.toViewerDir,
                                                   phongExp, phongScale, phongWeight);

            q2RestirAddSample(outReservoir, lightIndex, target * float(count), target,
                              rnd16(seed, salt + 3u));
        }
    }

    /* History: the previous frame's slot has every pixel (the direct pass dispatches over the
       whole packed image), and the frame-id parity is what makes the two slots a ring. */
    if (globalUniform.frameId > 0u && globalUniform.restirParams.z == 0u)
    {
        const float2 resolutionF = float2(resolution);
        const float2 currentUV = (float2(regularPix) + 0.5) / resolutionF;
        const float2 motion = framebufMotionDlss_Sampled.Load(int3(regularPix, 0)).rg;

        float2 previousUV = currentUV + motion;

        if (!all(isfinite(previousUV)))
        {
            previousUV = currentUV;
        }

        const bool reprojectionValid =
            all(previousUV >= (float2)0.0) && all(previousUV < (float2)1.0);
        const int2 maxPixel = int2(resolution) - int2(1, 1);
        const int2 historyRegular = reprojectionValid
            ? clamp(int2(floor(previousUV * resolutionF)), int2(0, 0), maxPixel)
            : regularPix;

        if (reprojectionValid)
        {
            const Surface previousSurface = fetchGbufferSurface_NoAlbedoViewDir_Prev(historyRegular);

            if (q2RestirSurfaceMatches(previousSurface, surf))
            {
                q2RestirMergeHistory(outReservoir, historyBase, resolution, historyRegular, surf,
                                     phongExp, phongScale, phongWeight, seed,
                                     Q2_RNG_RESTIR_TEMPORAL);
            }
        }

        for (uint tap = 0u; tap < Q2_RESTIR_SPATIAL_TAPS; tap++)
        {
            const int2 tapPixel = clamp(historyRegular + q2RestirSpatialOffset(tap),
                                        int2(0, 0), maxPixel);
            const Surface tapSurface = fetchGbufferSurface_NoAlbedoViewDir_Prev(tapPixel);

            if (!q2RestirSurfaceMatches(tapSurface, surf))
            {
                continue;
            }

            q2RestirMergeHistory(outReservoir, historyBase, resolution, tapPixel, surf,
                                 phongExp, phongScale, phongWeight, seed,
                                 Q2_RNG_RESTIR_SPATIAL_BASE + tap);
        }
    }

    q2RestirClampSamples(outReservoir, Q2_RESTIR_MAX_SAMPLES);

    if (!q2RestirIsUsable(outReservoir))
    {
        outReservoir.lightIndex = uint(LIGHT_INDEX_NONE);
        outReservoir.sampleCount = 0u;
        outReservoir.weightSum = 0.0;
        outReservoir.selectedTarget = 0.0;
        outWeight = 0.0;
    }
    else
    {
        outWeight = q2RestirWeight(outReservoir);
    }

    restirReservoirs[currentIndex] = outReservoir;
}

#endif
