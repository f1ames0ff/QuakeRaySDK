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
// CPU reference for the world radiance cache: a 1D spatial+normal hash over quantized hit
// positions and surface normals with distance-based LOD cells and 4-slot linear probing,
// matching the shader in Source/Shaders/RadianceCache.hlsli. The cache stores outgoing shaded
// radiance for a short frame window; expired or oldest probe slots are reclaimed when full, so the
// GPU path is a bounded approximation and not a replacement for the screen-space cache.

#pragma once

#include <cstdint>
#include <vector>

namespace qray
{

struct RadianceCacheEntry
{
    // xyz = radiance, w unused; the 16-byte block keeps the HLSL and C++ layouts identical.
    float    radiance[4];
    uint32_t key;
    uint32_t frame;
    uint32_t pad[2];
};

static_assert(sizeof(RadianceCacheEntry) == 32, "RadianceCacheEntry has to keep the 32-byte layout");

class RadianceCache
{
public:
    static constexpr uint32_t kEntryCount = 1u << 18;
    static constexpr uint32_t kMaxAgeFrames = 600;
    static constexpr uint32_t kMaxSamples = 64;
    static constexpr uint32_t kProbeSteps = 4u;

    struct Lookup
    {
        uint32_t index;
        uint32_t key;
        uint32_t lod;
    };

    // The counters the volume pass reports through the ray-stats categories. `activeCells` counts
    // the hash slots whose first insert of the current frame happened, matching the shader's
    // per-slot frame tag.
    struct Stats
    {
        uint64_t lookups = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t puts = 0;
        uint64_t activeCells = 0;
    };

    // The shader's hash: a 32-bit mix, stable across CPU and HLSL.
    static uint32_t Hash(uint32_t value);

    // Quantizes a world normal into 7 bins per axis (9 bits), falling back to +Z when degenerate.
    static uint32_t QuantizeNormal(const float normal[3]);

    // Quantizes the position into a cell whose size is baseCellSize * 2^lod and mixes the
    // quantized surface normal into the hash key so opposite/orthogonal walls stay isolated.
    static Lookup MakeLookup(const float position[3], const float normal[3],
                             const float cameraPosition[3],
                             float baseCellSize, float lodDistance);
    static Lookup MakeLookup(const float position[3], const float cameraPosition[3],
                             float baseCellSize, float lodDistance);

    RadianceCache();

    void Clear();
    bool Get(const Lookup &lookup, uint32_t frame, uint32_t maxAge, float outRadiance[3]) const;
    void Put(const Lookup &lookup, uint32_t frame, const float radiance[3],
             uint32_t maxAge = kMaxAgeFrames);

    const RadianceCacheEntry &Entry(uint32_t index) const { return entries[index]; }
    const Stats &GetStats() const { return stats; }

private:
    std::vector<RadianceCacheEntry> entries;
    std::vector<uint32_t>          activeFrames;
    mutable Stats                  stats;
};

}
