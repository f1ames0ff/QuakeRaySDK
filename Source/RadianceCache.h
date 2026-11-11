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
// CPU reference for the world radiance cache: a 1D spatial hash over quantized hit positions with
// distance-based LOD cells, matching the shader in Source/Shaders/RadianceCache.hlsli. The cache
// stores shaded radiance for a short frame window; a collision overwrites the slot, so the GPU
// path is a bounded approximation and not a replacement for the screen-space cache.

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

    // Quantizes the position into a cell whose size is baseCellSize * 2^lod, where lod grows with
    // the distance to the camera, then hashes the LOD and the integer cell.
    static Lookup MakeLookup(const float position[3], const float cameraPosition[3],
                             float baseCellSize, float lodDistance);

    RadianceCache();

    void Clear();
    bool Get(const Lookup &lookup, uint32_t frame, uint32_t maxAge, float outRadiance[3]) const;
    void Put(const Lookup &lookup, uint32_t frame, const float radiance[3]);

    const RadianceCacheEntry &Entry(uint32_t index) const { return entries[index]; }
    const Stats &GetStats() const { return stats; }

private:
    std::vector<RadianceCacheEntry> entries;
    std::vector<uint32_t>          activeFrames;
    mutable Stats                  stats;
};

}
