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
