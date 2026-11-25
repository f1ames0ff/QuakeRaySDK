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

#include <qray/gpu_context.h>

#include <string>

namespace qray::gpu
{

QrResult ValidateRequirements(const QrayGpuRequirements *pRequirements,
                              QrayContractDiagnostic *pDiagnostic) noexcept;

bool HasRequiredInstanceExtensions(const QrayGpuRequirements &requirements,
                                   std::string *pError = nullptr);

bool PhysicalDeviceMeetsRequirements(VkPhysicalDevice physicalDevice,
                                     VkSurfaceKHR surface,
                                     const QrayGpuRequirements &requirements,
                                     std::string *pError = nullptr);

QrResult CheckNvrhiFeatures(nvrhi::IDevice *pDevice,
                            const QrayGpuRequirements &requirements,
                            std::string *pError);

QrResult AppendVulkanFeatureChain(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures2 *pFeatures,
                                  const QrayGpuRequirements &requirements,
                                  std::string *pError);

} // namespace qray::gpu
