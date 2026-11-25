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

#include "RayStats.h"

#include <cstring>

#include "Utils.h"

namespace qray
{

RayStats::RayStats(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator)
    : device(_device)
    , allocator(_allocator)
{
    CreateBuffers(allocator);
}

RayStats::~RayStats()
{
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (mapped[i])
        {
            buffers[i].TryUnmap();
            mapped[i] = nullptr;
        }
        buffers[i].Destroy();
    }
}

void RayStats::CreateBuffers(std::shared_ptr<MemoryAllocator> &allocator)
{
    const VkDeviceSize size = RAY_STATS_CATEGORY_COUNT * sizeof(uint32_t);

    // The RHI layer wraps these buffers for the primary pass's ray-stats set
    // (RHI/RhiRtPrimaryPass.cpp): NVRHI's native-buffer wrap queries the device address of every
    // buffer when the device has BDA (vulkan-buffer.cpp:215-220), which
    // VUID-VkBufferDeviceAddressInfo-buffer-02601 forbids without the bit.
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        buffers[i].Init(allocator, size,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        "RayStats buffer");
        mapped[i] = buffers[i].Map();
        if (mapped[i] != nullptr)
        {
            std::memset(mapped[i], 0, size);
        }
    }
}

void RayStats::Reset(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    if (mapped[frameIndex] != nullptr)
    {
        memset(mapped[frameIndex], 0, RAY_STATS_CATEGORY_COUNT * sizeof(uint32_t));
    }
}

uint32_t RayStats::GetRays(uint32_t frameIndex) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    const uint32_t *p = static_cast<const uint32_t *>(mapped[frameIndex]);
    uint32_t total = 0;
    for (uint32_t i = 0; i < RAY_STATS_CATEGORY_COUNT; i++)
    {
        total += p[i];
    }
    return total;
}

void RayStats::GetRaysPerCategory(uint32_t frameIndex, uint32_t *pOutCounts) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    if (pOutCounts == nullptr)
    {
        return;
    }

    const uint32_t *p = static_cast<const uint32_t *>(mapped[frameIndex]);
    for (uint32_t i = 0; i < RAY_STATS_CATEGORY_COUNT; i++)
    {
        pOutCounts[i] = p[i];
    }
}

VkBuffer RayStats::GetBuffer(uint32_t frameIndex) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    return buffers[frameIndex].GetBuffer();
}

}
