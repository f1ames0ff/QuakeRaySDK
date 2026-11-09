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

#include "RadianceCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qray
{

RadianceCache::RadianceCache()
    : entries(kEntryCount)
{
}

uint32_t RadianceCache::Hash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

RadianceCache::Lookup RadianceCache::MakeLookup(const float position[3], const float cameraPosition[3],
                                                float baseCellSize, float lodDistance)
{
    const float dx = position[0] - cameraPosition[0];
    const float dy = position[1] - cameraPosition[1];
    const float dz = position[2] - cameraPosition[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    const float safeDistance = std::max(lodDistance, 1.0f);
    const uint32_t lod = uint32_t(std::exp2(std::floor(std::log2(1.0f + distance / safeDistance))));
    const float cell = std::max(baseCellSize, 0.01f) * float(lod);

    const int32_t x = int32_t(std::floor(position[0] / cell));
    const int32_t y = int32_t(std::floor(position[1] / cell));
    const int32_t z = int32_t(std::floor(position[2] / cell));

    const uint32_t key = Hash(lod + Hash(uint32_t(z) + Hash(uint32_t(y) + Hash(uint32_t(x)))));

    Lookup lookup;
    lookup.index = key & (kEntryCount - 1u);
    lookup.key = key;
    lookup.lod = lod;
    return lookup;
}

void RadianceCache::Clear()
{
    std::memset(entries.data(), 0, entries.size() * sizeof(RadianceCacheEntry));
}

bool RadianceCache::Get(const Lookup &lookup, uint32_t frame, uint32_t maxAge, float outRadiance[3]) const
{
    const RadianceCacheEntry &entry = entries[lookup.index];

    if (entry.frame == 0u || entry.key != lookup.key)
    {
        return false;
    }

    const uint32_t storedFrame = entry.frame - 1u;

    if (frame < storedFrame || frame - storedFrame > std::min(maxAge, kMaxAgeFrames))
    {
        return false;
    }

    outRadiance[0] = entry.radiance[0];
    outRadiance[1] = entry.radiance[1];
    outRadiance[2] = entry.radiance[2];
    return true;
}

void RadianceCache::Put(const Lookup &lookup, uint32_t frame, const float radiance[3])
{
    RadianceCacheEntry &entry = entries[lookup.index];
    entry.radiance[0] = radiance[0];
    entry.radiance[1] = radiance[1];
    entry.radiance[2] = radiance[2];
    entry.key = lookup.key;
    entry.frame = frame + 1u;
    std::memset(entry.pad, 0, sizeof(entry.pad));
}

}
