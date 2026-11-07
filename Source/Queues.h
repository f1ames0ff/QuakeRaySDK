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

#pragma once

#include <vector>

#include "Common.h"

namespace qray
{

struct QrayGpuRequirements;

class Queues
{
public:
    Queues(VkPhysicalDevice physDevice, VkSurfaceKHR surface,
           const QrayGpuRequirements *pRequirements = nullptr);
    ~Queues();

    Queues(const Queues &other) = delete;
    Queues(Queues &&other) noexcept = delete;
    Queues &operator=(const Queues &other) = delete;
    Queues &operator=(Queues &&other) noexcept = delete;

    void SetDevice(VkDevice device);

    void GetDeviceQueueCreateInfos(std::vector<VkDeviceQueueCreateInfo> &outInfos) const;

    uint32_t GetIndexGraphics() const;
    uint32_t GetIndexCompute() const;
    uint32_t GetIndexTransfer() const;
    uint32_t GetGraphicsQueueCount() const;
    uint32_t GetComputeQueueCount() const;
    uint32_t GetTransferQueueCount() const;

    VkQueue GetGraphics() const;
    VkQueue GetCompute() const;
    VkQueue GetTransfer() const;

private:
    struct QueueFamilyRequest
    {
        uint32_t index = 0;
        uint32_t count = 0;
    };

    std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    std::vector<QueueFamilyRequest> queueFamilyRequests;
    std::vector<float> queuePriorities;

    uint32_t indexGraphics;
    uint32_t indexCompute;
    uint32_t indexTransfer;
    uint32_t graphicsQueueCount;
    uint32_t computeQueueCount;
    uint32_t transferQueueCount;

    VkQueue graphics;
    VkQueue compute;
    VkQueue transfer;
};

}
