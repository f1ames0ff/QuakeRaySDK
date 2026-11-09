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
// World radiance cache (first slice): a 1D spatial hash over quantized hit positions with
// distance-based LOD cells. The volume probe raygen looks up the shaded radiance of a hit and
// reuses it for a few frames; a miss shades and inserts. The constants, hash and LOD math match
// Source/RadianceCache.h/.cpp, which the CPU tests cover. Collisions overwrite the slot, so the
// cache is a bounded approximation.

#ifndef RADIANCE_CACHE_HLSLI_
#define RADIANCE_CACHE_HLSLI_

#define Q2_RADIANCE_CACHE_SIZE 262144u
#define Q2_RADIANCE_CACHE_MAX_AGE 600u

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

void q2RadianceCacheLookup(const float3 position, out uint index, out uint key)
{
    const float3 delta = position - globalUniform.cameraPosition.xyz;
    const float distance = length(delta);
    const float lodDistance = max(globalUniform.radianceCacheParams.z, 1.0);
    const uint lod = uint(exp2(floor(log2(1.0 + distance / lodDistance))));
    const float cell = max(globalUniform.radianceCacheParams.y, 0.01) * float(lod);
    const int3 cellPosition = int3(floor(position / cell));

    key = q2RadianceHash(lod + q2RadianceHash(uint(cellPosition.z) +
        q2RadianceHash(uint(cellPosition.y) + q2RadianceHash(uint(cellPosition.x)))));
    index = key & (Q2_RADIANCE_CACHE_SIZE - 1u);
}

bool q2RadianceCacheGet(const float3 position, out float3 radiance)
{
    radiance = (float3)0.0;

    if (!(globalUniform.radianceCacheParams.x > 0.0))
    {
        return false;
    }

    uint index;
    uint key;
    q2RadianceCacheLookup(position, index, key);

    const RadianceCacheEntry entry = radianceCache[index];

    if (entry.frame == 0u || entry.key != key)
    {
        rayStatsAdd(RAY_STATS_CATEGORY_CACHE_MISS, 1);
        return false;
    }

    const uint storedFrame = entry.frame - 1u;
    const uint maxAge = min(uint(globalUniform.radianceCacheParams.w), Q2_RADIANCE_CACHE_MAX_AGE);

    if (globalUniform.frameId < storedFrame || globalUniform.frameId - storedFrame > maxAge)
    {
        rayStatsAdd(RAY_STATS_CATEGORY_CACHE_MISS, 1);
        return false;
    }

    rayStatsAdd(RAY_STATS_CATEGORY_CACHE_HIT, 1);
    radiance = entry.radiance.xyz;
    return true;
}

void q2RadianceCachePut(const float3 position, const float3 radiance)
{
    if (!(globalUniform.radianceCacheParams.x > 0.0))
    {
        return;
    }

    uint index;
    uint key;
    q2RadianceCacheLookup(position, index, key);

    const uint frameTag = globalUniform.frameId + 1u;
    uint       previousTag;
    InterlockedExchange(radianceCacheActiveTags[index], frameTag, previousTag);

    if (previousTag != frameTag)
    {
        rayStatsAdd(RAY_STATS_CATEGORY_CACHE_ACTIVE, 1);
    }

    RadianceCacheEntry entry;
    entry.radiance = float4(radiance, 0.0);
    entry.key = key;
    entry.frame = frameTag;
    entry.pad = uint2(0u, 0u);
    radianceCache[index] = entry;
}

#endif
