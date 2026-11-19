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
#include <limits>

namespace qray
{

RadianceCache::RadianceCache()
    : entries(kEntryCount)
    , activeFrames(kEntryCount, 0u)
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

uint32_t RadianceCache::QuantizeNormal(const float normal[3])
{
    const float lenSq = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
    const float len = std::sqrt(lenSq);
    const bool valid = std::isfinite(len) && len > 1e-6f;
    const float nx = valid ? (normal[0] / len) : 0.0f;
    const float ny = valid ? (normal[1] / len) : 0.0f;
    const float nz = valid ? (normal[2] / len) : 1.0f;

    const auto bin = [](float v)
    {
        return uint32_t(std::clamp(std::floor((v * 0.5f + 0.5f) * 7.0f), 0.0f, 6.0f));
    };

    return bin(nx) | (bin(ny) << 3u) | (bin(nz) << 6u);
}

RadianceCache::Lookup RadianceCache::MakeLookup(const float position[3], const float normal[3],
                                                const float cameraPosition[3],
                                                float baseCellSize, float lodDistance)
{
    const float dx = position[0] - cameraPosition[0];
    const float dy = position[1] - cameraPosition[1];
    const float dz = position[2] - cameraPosition[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    const float safeDistance = std::max(lodDistance, 0.25f);
    const uint32_t lod = uint32_t(std::exp2(std::floor(std::log2(1.0f + distance / safeDistance))));
    const float cell = std::max(baseCellSize, 0.01f) * float(lod);

    const int32_t x = int32_t(std::floor(position[0] / cell));
    const int32_t y = int32_t(std::floor(position[1] / cell));
    const int32_t z = int32_t(std::floor(position[2] / cell));
    const uint32_t normalCode = QuantizeNormal(normal);

    const uint32_t key = Hash(normalCode +
        Hash(lod + Hash(uint32_t(z) + Hash(uint32_t(y) + Hash(uint32_t(x))))));

    Lookup lookup;
    lookup.index = key & (kEntryCount - 1u);
    lookup.key = key;
    lookup.lod = lod;
    return lookup;
}

RadianceCache::Lookup RadianceCache::MakeLookup(const float position[3], const float cameraPosition[3],
                                                float baseCellSize, float lodDistance)
{
    static constexpr float kDefaultNormal[3] = { 0.0f, 0.0f, 1.0f };
    return MakeLookup(position, kDefaultNormal, cameraPosition, baseCellSize, lodDistance);
}

void RadianceCache::Clear()
{
    std::memset(entries.data(), 0, entries.size() * sizeof(RadianceCacheEntry));
    std::fill(activeFrames.begin(), activeFrames.end(), 0u);
    stats = Stats{};
}

bool RadianceCache::Get(const Lookup &lookup, uint32_t frame, uint32_t maxAge, float outRadiance[3]) const
{
    stats.lookups++;

    const uint32_t effectiveMaxAge = std::min(maxAge, kMaxAgeFrames);

    for (uint32_t step = 0u; step < kProbeSteps; step++)
    {
        const uint32_t slot = (lookup.index + step) & (kEntryCount - 1u);
        const RadianceCacheEntry &entry = entries[slot];

        if (entry.frame == 0u || entry.key != lookup.key)
        {
            continue;
        }

        const uint32_t storedFrame = entry.frame - 1u;

        if (frame >= storedFrame && frame - storedFrame <= effectiveMaxAge)
        {
            stats.hits++;
            outRadiance[0] = entry.radiance[0];
            outRadiance[1] = entry.radiance[1];
            outRadiance[2] = entry.radiance[2];
            return true;
        }
    }

    stats.misses++;
    return false;
}

void RadianceCache::Put(const Lookup &lookup, uint32_t frame, const float radiance[3], uint32_t maxAge)
{
    stats.puts++;

    const uint32_t effectiveMaxAge = std::min(maxAge, kMaxAgeFrames);
    uint32_t matchedSlot = lookup.index;
    bool     foundMatch = false;
    uint32_t freeSlot = lookup.index;
    bool     foundFree = false;
    uint32_t oldestSlot = lookup.index;
    uint32_t oldestFrame = std::numeric_limits<uint32_t>::max();

    for (uint32_t step = 0u; step < kProbeSteps; step++)
    {
        const uint32_t slot = (lookup.index + step) & (kEntryCount - 1u);
        const RadianceCacheEntry &candidate = entries[slot];

        if (candidate.frame != 0u && candidate.key == lookup.key)
        {
            matchedSlot = slot;
            foundMatch = true;
            break;
        }

        const bool empty = (candidate.frame == 0u);
        const uint32_t storedFrame = candidate.frame - 1u;
        const bool expired = !empty && (frame < storedFrame || frame - storedFrame > effectiveMaxAge);

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

    const uint32_t chosenSlot = foundMatch ? matchedSlot : (foundFree ? freeSlot : oldestSlot);
    const uint32_t frameTag = frame + 1u;

    if (activeFrames[chosenSlot] != frameTag)
    {
        activeFrames[chosenSlot] = frameTag;
        stats.activeCells++;
    }

    RadianceCacheEntry &entry = entries[chosenSlot];

    uint32_t samples = 1u;
    float    value[3] = { radiance[0], radiance[1], radiance[2] };

    if (entry.key == lookup.key && entry.frame != 0u && entry.pad[0] > 0u)
    {
        const uint32_t storedFrame = entry.frame - 1u;

        if (frame >= storedFrame && frame - storedFrame <= effectiveMaxAge)
        {
            samples = std::min<uint32_t>(entry.pad[0] + 1u, kMaxSamples);
            const float blend = 1.0f / float(samples);
            for (int i = 0; i < 3; i++)
            {
                value[i] = entry.radiance[i] + (radiance[i] - entry.radiance[i]) * blend;
            }
        }
    }

    entry.radiance[0] = value[0];
    entry.radiance[1] = value[1];
    entry.radiance[2] = value[2];
    entry.key = lookup.key;
    entry.frame = frameTag;
    std::memset(entry.pad, 0, sizeof(entry.pad));
    entry.pad[0] = samples;
}

}
