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

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace qray
{

// Device features that the NVRHI Vulkan backend depends on.
//
// NVRHI builds bindless descriptor layouts with UPDATE_AFTER_BIND and
// PARTIALLY_BOUND bindings and submits work with timeline semaphores, so the
// device must be created with the matching features enabled. NVRHI only reads
// two of them from its device description; the rest have to be enabled by the
// application, otherwise descriptor set layout creation fails validation.
struct NvrhiRequirements
{
    bool timelineSemaphore = false;

    bool descriptorBindingSampledImageUpdateAfterBind = false;
    bool descriptorBindingStorageImageUpdateAfterBind = false;
    bool descriptorBindingStorageBufferUpdateAfterBind = false;
    bool descriptorBindingUniformTexelBufferUpdateAfterBind = false;
    bool descriptorBindingStorageTexelBufferUpdateAfterBind = false;
    bool descriptorBindingUniformBufferUpdateAfterBind = false;
    bool descriptorBindingPartiallyBound = false;
    bool descriptorBindingUpdateUnusedWhilePending = false;

    bool shaderStorageImageArrayNonUniformIndexing = false;

    // The shadow map sampler uses VK_SAMPLER_REDUCTION_MODE_MIN (RhiShadowMapPass), which requires
    // this feature; creating the sampler without it is invalid usage.
    bool samplerFilterMinmax = false;

    // Vulkan 1.3: every pass the RHI layer records is a dynamic rendering pass.
    bool dynamicRendering = false;

    // Names of the requirements the driver does not support. Empty when the
    // physical device can satisfy all of them.
    std::vector<std::string> GetUnsupported() const;

    // Requirements the RHI layer cannot work without: without timeline
    // semaphores it cannot submit, every descriptor set layout it creates
    // uses partially bound bindings, and every pass it records is drawn with
    // dynamic rendering. The texel buffer flavours are left out:
    // NVRHI only needs them for layouts the application describes itself.
    bool IsCriticalSupported() const;
};

// Queries the physical device for the requirements above.
NvrhiRequirements QueryNvrhiRequirements(VkPhysicalDevice physDevice);

// Enables the supported subset of the requirements in the Vulkan 1.2 feature
// chain that is passed to vkCreateDevice.
void ApplyNvrhiRequirements(const NvrhiRequirements &requirements,
                            VkPhysicalDeviceVulkan12Features &features12);

}
