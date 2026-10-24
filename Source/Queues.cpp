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

#include "Queues.h"

#include <qray/gpu_context.h>

#include <algorithm>

#include "QrException.h"

using namespace qray;

namespace
{
    bool IsComputeQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    }

    bool IsDedicatedComputeQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
               (flags & VK_QUEUE_COMPUTE_BIT) != 0;
    }

    bool IsTransferQueue(VkQueueFlags flags)
    {
        return (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    }

    bool IsDedicatedTransferQueue(VkQueueFlags flags)
    {
        return (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 &&
               (flags & VK_QUEUE_TRANSFER_BIT) != 0;
    }
}

Queues::Queues(VkPhysicalDevice physDevice, VkSurfaceKHR surface,
               const QrayGpuRequirements *pRequirements)
    : queueFamilyProperties{}
    , queueFamilyRequests{}
    , queuePriorities{}
    , indexGraphics(UINT32_MAX)
    , indexCompute(UINT32_MAX)
    , indexTransfer(UINT32_MAX)
    , graphicsQueueCount(1)
    , computeQueueCount(1)
    , transferQueueCount(1)
    , graphics(VK_NULL_HANDLE)
    , compute(VK_NULL_HANDLE)
    , transfer(VK_NULL_HANDLE)
{
    const QrayGpuRequirements defaultRequirements = {};
    const QrayGpuRequirements &requirements =
        pRequirements != nullptr ? *pRequirements : defaultRequirements;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &familyCount, nullptr);
    assert(familyCount > 0);

    queueFamilyProperties.resize(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &familyCount, queueFamilyProperties.data());

    uint32_t graphicsSelectionScore = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++)
    {
        VkBool32 presentSupported = VK_FALSE;
        VK_CHECKERROR(vkGetPhysicalDeviceSurfaceSupportKHR(physDevice, i, surface, &presentSupported));

        const VkQueueFlags flags = queueFamilyProperties[i].queueFlags;
        const uint32_t queueCount = queueFamilyProperties[i].queueCount;

        if (presentSupported && (flags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
            queueCount >= requirements.queues.graphicsQueueCount)
        {
            const uint32_t score = 1u +
                ((flags & VK_QUEUE_COMPUTE_BIT) != 0 ? 1u : 0u) +
                ((flags & VK_QUEUE_TRANSFER_BIT) != 0 ? 1u : 0u);
            if (indexGraphics == UINT32_MAX || score > graphicsSelectionScore)
            {
                indexGraphics = i;
                graphicsSelectionScore = score;
            }
        }

        if (IsComputeQueue(flags) && queueCount >= requirements.queues.computeQueueCount)
        {
            const bool dedicated = IsDedicatedComputeQueue(flags);
            const bool requireDedicated =
                (requirements.queues.flags & QRAY_GPU_QUEUE_REQUIRE_DEDICATED_COMPUTE) != 0;

            if ((!requireDedicated || dedicated) &&
                (indexCompute == UINT32_MAX || dedicated))
                indexCompute = i;
        }

        if (IsTransferQueue(flags) && queueCount >= requirements.queues.transferQueueCount)
        {
            const bool dedicated = IsDedicatedTransferQueue(flags);
            const bool requireDedicated =
                (requirements.queues.flags & QRAY_GPU_QUEUE_REQUIRE_DEDICATED_TRANSFER) != 0;

            if ((!requireDedicated || dedicated) &&
                (indexTransfer == UINT32_MAX || dedicated))
                indexTransfer = i;
        }
    }

    if (indexGraphics == UINT32_MAX)
        throw QrException(QR_CANT_FIND_PHYSICAL_DEVICE, "No graphics/present queue satisfies the GPU requirements");

    if (indexCompute == UINT32_MAX)
        throw QrException(QR_CANT_FIND_PHYSICAL_DEVICE, "No compute queue satisfies the GPU requirements");

    if (indexTransfer == UINT32_MAX)
        throw QrException(QR_CANT_FIND_PHYSICAL_DEVICE, "No transfer queue satisfies the GPU requirements");

    graphicsQueueCount = requirements.queues.graphicsQueueCount;
    computeQueueCount = requirements.queues.computeQueueCount;
    transferQueueCount = requirements.queues.transferQueueCount;

    const auto appendFamily = [this](uint32_t index, uint32_t count)
    {
        const auto existing = std::find_if(queueFamilyRequests.begin(), queueFamilyRequests.end(),
            [index](const QueueFamilyRequest &request) { return request.index == index; });
        if (existing == queueFamilyRequests.end())
            queueFamilyRequests.push_back({ index, count });
        else
            existing->count = std::max(existing->count, count);
    };

    appendFamily(indexGraphics, graphicsQueueCount);
    appendFamily(indexCompute, computeQueueCount);
    appendFamily(indexTransfer, transferQueueCount);

    uint32_t maximumQueueCount = 1;
    for (const QueueFamilyRequest &request : queueFamilyRequests)
        maximumQueueCount = std::max(maximumQueueCount, request.count);
    queuePriorities.assign(maximumQueueCount, 1.0f);
}

Queues::~Queues()
{}

void Queues::SetDevice(VkDevice device)
{
    vkGetDeviceQueue(device, indexGraphics, 0, &graphics);
    vkGetDeviceQueue(device, indexCompute, 0, &compute);
    vkGetDeviceQueue(device, indexTransfer, 0, &transfer);
}

void Queues::GetDeviceQueueCreateInfos(std::vector<VkDeviceQueueCreateInfo> &outInfos) const
{
    for (const QueueFamilyRequest &request : queueFamilyRequests)
    {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = request.index;
        queueInfo.queueCount = request.count;
        queueInfo.pQueuePriorities = queuePriorities.data();
        outInfos.push_back(queueInfo);
    }
}

uint32_t Queues::GetIndexGraphics() const
{
    return indexGraphics;
}

uint32_t Queues::GetIndexCompute() const
{
    return indexCompute;
}

uint32_t Queues::GetIndexTransfer() const
{
    return indexTransfer;
}

uint32_t Queues::GetGraphicsQueueCount() const
{
    return graphicsQueueCount;
}

uint32_t Queues::GetComputeQueueCount() const
{
    return computeQueueCount;
}

uint32_t Queues::GetTransferQueueCount() const
{
    return transferQueueCount;
}

VkQueue Queues::GetGraphics() const
{
    return graphics;
}

VkQueue Queues::GetCompute() const
{
    return compute;
}

VkQueue Queues::GetTransfer() const
{
    return transfer;
}
