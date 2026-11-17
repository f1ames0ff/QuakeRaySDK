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
// World radiance cache: a 1D spatial+normal hash over quantized hit positions and surface normals
// with distance-based LOD cells and 4-slot linear probing. The volume probe and second-bounce
// paths look up the outgoing shaded radiance of a hit and reuse it across frames; a miss shades
// and inserts. The constants, normal quantization, hash, probing and LOD math match
// Source/RadianceCache.h/.cpp, which the CPU tests cover.

#ifndef RADIANCE_CACHE_HLSLI_
#define RADIANCE_CACHE_HLSLI_

#define Q2_RADIANCE_CACHE_SIZE 262144u
#define Q2_RADIANCE_CACHE_MAX_AGE 600u
#define Q2_RADIANCE_CACHE_MAX_SAMPLES 64u
#define Q2_RADIANCE_CACHE_PROBE_STEPS 4u

struct RadianceCacheEntry
{
    float4 radiance;
    uint   key;
    uint   frame;
    uint2  pad;
};

#if defined(DESC_SET_LIGHT_SOURCES)
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_RADIANCE_CACHE, DESC_SET_LIGHT_SOURCES)]]
RWStructuredBuffer<RadianceCacheEntry> radianceCache;

// One frame tag per hash slot. A slot is "active" in the frame when its first cache insert of the
// frame exchanges the tag; this is the counter the report uses to size the cache-shading dispatch.
// Slots are hash-indexed, so colliding cells share a tag exactly as they share an entry.
[[vk::binding(BINDING_LIGHT_SOURCES_Q2_RADIANCE_CACHE_ACTIVE, DESC_SET_LIGHT_SOURCES)]]
RWStructuredBuffer<uint> radianceCacheActiveTags;
#endif

uint q2RadianceHash(const uint value)
{
    uint x = value;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint q2RadianceQuantizeNormal(const float3 normal)
{
    const float lenSq = dot(normal, normal);
    const float3 n = (isfinite(lenSq) && lenSq > 1e-12) ? (normal * rsqrt(lenSq)) : float3(0.0, 0.0, 1.0);
    const uint3 bins = uint3(clamp(floor((n * 0.5 + 0.5) * 7.0), (float3)0.0, (float3)6.0));
    return bins.x | (bins.y << 3u) | (bins.z << 6u);
}

void q2RadianceCacheLookup(const float3 position, const float3 normal, out uint index, out uint key)
{
    const float3 delta = position - globalUniform.cameraPosition.xyz;
    const float distance = length(delta);
    const float lodDistance = max(globalUniform.radianceCacheParams.z, 0.25);
    const uint lod = uint(exp2(floor(log2(1.0 + distance / lodDistance))));
    const float cell = max(globalUniform.radianceCacheParams.y, 0.01) * float(lod);
    const int3 cellPosition = int3(floor(position / cell));
    const uint normalCode = q2RadianceQuantizeNormal(normal);

    key = q2RadianceHash(normalCode + q2RadianceHash(lod + q2RadianceHash(uint(cellPosition.z) +
        q2RadianceHash(uint(cellPosition.y) + q2RadianceHash(uint(cellPosition.x))))));
    index = key & (Q2_RADIANCE_CACHE_SIZE - 1u);
}

bool q2RadianceCacheGet(const float3 position, const float3 normal, out float3 radiance)
{
    radiance = (float3)0.0;

    if (!(globalUniform.radianceCacheParams.x > 0.0))
    {
        return false;
    }

    uint index;
    uint key;
    q2RadianceCacheLookup(position, normal, index, key);

    const uint maxAge = min(uint(globalUniform.radianceCacheParams.w), Q2_RADIANCE_CACHE_MAX_AGE);

    for (uint step = 0u; step < Q2_RADIANCE_CACHE_PROBE_STEPS; step++)
    {
        const uint slot = (index + step) & (Q2_RADIANCE_CACHE_SIZE - 1u);
        const RadianceCacheEntry entry = radianceCache[slot];

        if (entry.frame == 0u || entry.key != key)
        {
            continue;
        }

        const uint storedFrame = entry.frame - 1u;

        if (globalUniform.frameId >= storedFrame &&
            globalUniform.frameId - storedFrame <= maxAge)
        {
            rayStatsAdd(RAY_STATS_CATEGORY_CACHE_HIT, 1);
            radiance = entry.radiance.xyz;
            return true;
        }
    }

    rayStatsAdd(RAY_STATS_CATEGORY_CACHE_MISS, 1);
    return false;
}

void q2RadianceCachePut(const float3 position, const float3 normal, const float3 radiance)
{
    if (!(globalUniform.radianceCacheParams.x > 0.0))
    {
        return;
    }

    uint index;
    uint key;
    q2RadianceCacheLookup(position, normal, index, key);

    const uint maxAge = min(uint(globalUniform.radianceCacheParams.w), Q2_RADIANCE_CACHE_MAX_AGE);
    uint chosenSlot = index;
    bool foundMatch = false;
    uint freeSlot = index;
    bool foundFree = false;
    uint oldestSlot = index;
    uint oldestFrame = 0xFFFFFFFFu;

    for (uint step = 0u; step < Q2_RADIANCE_CACHE_PROBE_STEPS; step++)
    {
        const uint slot = (index + step) & (Q2_RADIANCE_CACHE_SIZE - 1u);
        const RadianceCacheEntry candidate = radianceCache[slot];

        if (candidate.frame != 0u && candidate.key == key)
        {
            chosenSlot = slot;
            foundMatch = true;
            break;
        }

        const bool empty = (candidate.frame == 0u);
        const uint storedFrame = candidate.frame - 1u;
        const bool expired = !empty && (globalUniform.frameId < storedFrame ||
                                        globalUniform.frameId - storedFrame > maxAge);

        if ((empty || expired) && !foundFree)
        {
            freeSlot = slot;
            foundFree = true;
        }

        if (!empty && candidate.frame < oldestFrame)
        {
            oldestFrame = candidate.frame;
            oldestSlot = slot;
        }
    }

    if (!foundMatch)
    {
        chosenSlot = foundFree ? freeSlot : oldestSlot;
    }

    const uint frameTag = globalUniform.frameId + 1u;
    uint       previousTag;
    InterlockedExchange(radianceCacheActiveTags[chosenSlot], frameTag, previousTag);

    if (previousTag != frameTag)
    {
        rayStatsAdd(RAY_STATS_CATEGORY_CACHE_ACTIVE, 1);
    }

    // Multi-frame reuse: a revisited cell accumulates its radiance with an exponential moving
    // average capped at 64 samples, instead of overwriting the previous value. pad.x is the count.
    const RadianceCacheEntry previous = radianceCache[chosenSlot];
    uint  samples = 1u;
    float3 value = radiance;

    if (previous.key == key && previous.frame != 0u && previous.pad.x > 0u)
    {
        const uint storedFrame = previous.frame - 1u;

        if (globalUniform.frameId >= storedFrame &&
            globalUniform.frameId - storedFrame <= maxAge)
        {
            samples = min(previous.pad.x + 1u, Q2_RADIANCE_CACHE_MAX_SAMPLES);
            value = lerp(previous.radiance.xyz, radiance, 1.0 / float(samples));
        }
    }

    RadianceCacheEntry entry;
    entry.radiance = float4(value, 0.0);
    entry.key = key;
    entry.frame = frameTag;
    entry.pad = uint2(samples, 0u);
    radianceCache[chosenSlot] = entry;
}

#endif
