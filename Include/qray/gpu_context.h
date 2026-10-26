// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#ifndef QRAY_GPU_CONTEXT_H_
#define QRAY_GPU_CONTEXT_H_

#include "qray/host_contract.h"
#include "qray/qray.h"

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

namespace qray
{

inline constexpr uint32_t kGpuContextContractMajor = 1;
inline constexpr uint32_t kGpuContextContractMinor = 0;
inline constexpr uint32_t kGpuContextContractVersion =
    (kGpuContextContractMajor << 16) | kGpuContextContractMinor;

enum QrayGpuQueueRequirementFlagBits : uint32_t
{
    QRAY_GPU_QUEUE_REQUIRE_DEDICATED_COMPUTE = 1u << 0,
    QRAY_GPU_QUEUE_REQUIRE_DEDICATED_TRANSFER = 1u << 1,
};

struct QrayGpuQueueRequirements
{
    // The graphics family must support graphics, compute and transfer work and
    // be able to present to the host surface.
    uint32_t graphicsQueueCount = 1;
    uint32_t computeQueueCount = 1;
    uint32_t transferQueueCount = 1;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct QrayGpuFormatRequirement
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageCreateFlags imageFlags = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormatFeatureFlags requiredFormatFeatures = 0;
};

// Optional validation hook for requirements that are too broad to enumerate
// in this structure, such as limits or memory-budget policy. Called for every
// candidate physical device before the logical device is created.
using PFN_qrayValidateGpuDevice =
    QrResult (QRCONV *)(VkPhysicalDevice physicalDevice, void *pUserData);

// Optional extension point for native Vulkan feature structures. The host
// returns the head of a feature chain; the SDK preserves its own chain and
// links the host chain at the end. The chain and all nested pointers must stay
// alive until qrCreateInstanceWithGpuRequirements returns.
using PFN_qrayGetVulkanFeatureChain =
    QrResult (QRCONV *)(VkPhysicalDevice physicalDevice,
                       VkBaseOutStructure **ppFeatureChain,
                       void *pUserData);

struct QrayGpuRequirements
{
    QrayContractHeader header;

    uint32_t minVulkanApiVersion = VK_API_VERSION_1_3;
    uint32_t minimumFramesInFlight = 1;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;

    QrayArrayView<const char *> requiredInstanceExtensions;
    QrayArrayView<const char *> requiredDeviceExtensions;
    QrayArrayView<nvrhi::Feature> requiredNvrhiFeatures;
    QrayArrayView<QrayGpuFormatRequirement> requiredFormats;
    QrayGpuQueueRequirements queues = {};

    VkPhysicalDevice preferredPhysicalDevice = VK_NULL_HANDLE;

    PFN_qrayValidateGpuDevice pfnValidateDevice = nullptr;
    PFN_qrayGetVulkanFeatureChain pfnGetVulkanFeatureChain = nullptr;
    void *pUserData = nullptr;
};

enum QrayGpuCapabilityBits : uint64_t
{
    QRAY_GPU_CAP_SHARED_NVRHI_DEVICE = 1ull << 0,
    QRAY_GPU_CAP_NATIVE_VULKAN = 1ull << 1,
    QRAY_GPU_CAP_TIMELINE_SEMAPHORE = 1ull << 2,
    QRAY_GPU_CAP_ADDITIONAL_QUEUES = 1ull << 3,
};

struct QrayGpuCapabilities
{
    QrayContractHeader header;
    uint64_t flags = 0;
    uint32_t framesInFlight = 0;
    uint32_t vulkanApiVersion = 0;
    nvrhi::GraphicsAPI graphicsApi = nvrhi::GraphicsAPI::VULKAN;
    uint32_t graphicsQueueCount = 0;
    uint32_t computeQueueCount = 0;
    uint32_t transferQueueCount = 0;
    uint32_t reserved = 0;
};

struct QrayNativeVulkanInfo
{
    QrayContractHeader header;

    // All native handles are borrowed from QrInstance and become invalid at
    // qrDestroyInstance.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = 0;
    uint32_t graphicsQueueIndex = 0;
    uint32_t graphicsQueueCount = 0;

    VkQueue computeQueue = VK_NULL_HANDLE;
    uint32_t computeQueueFamily = 0;
    uint32_t computeQueueIndex = 0;
    uint32_t computeQueueCount = 0;

    VkQueue transferQueue = VK_NULL_HANDLE;
    uint32_t transferQueueFamily = 0;
    uint32_t transferQueueIndex = 0;
    uint32_t transferQueueCount = 0;

    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
};

struct QrayGpuContext
{
    QrayContractHeader header;

    // Both pointers are borrowed from QrInstance and become invalid at
    // qrDestroyInstance. The host must not destroy or outlive them.
    nvrhi::IDevice *pDevice = nullptr;
    nvrhi::vulkan::IDevice *pVulkanDevice = nullptr;

    QrayGpuCapabilities capabilities = {};
    QrayNativeVulkanInfo nativeVulkan = {};
};

enum class QrayGpuFramePhase : uint32_t
{
    BeforeRenderer = 0,
    BeforePresent = 1,
};

struct QrayGpuFrameCallbackInfo
{
    QrayContractHeader header;
    QrayGpuFramePhase phase = QrayGpuFramePhase::BeforeRenderer;
    uint32_t frameIndex = 0;
    nvrhi::ICommandList *pCommandList = nullptr;
    nvrhi::ITexture *pOutputColor = nullptr;
    nvrhi::ITexture *pBackBuffer = nullptr;
};

// Record-only callback. The command list is already open and owned by the SDK;
// the host must not close, open, submit or wait on it. Submission is performed
// by the SDK after the callback returns.
using PFN_qrayGpuFrameCallback =
    void (QRCONV *)(const QrayGpuFrameCallbackInfo *pInfo, void *pUserData);

struct QrayGpuFrameHooks
{
    QrayContractHeader header;
    PFN_qrayGpuFrameCallback pfnCallback = nullptr;
    void *pUserData = nullptr;
    uint32_t reserved = 0;
};

struct QrayGpuContractInfo
{
    QrayContractHeader header;
    uint32_t contractVersion = kGpuContextContractVersion;
    uint32_t minimumVulkanApiVersion = VK_API_VERSION_1_3;
    uint32_t maximumFramesInFlight = 0;
    uint32_t requiredGraphicsQueues = 1;
    uint32_t maximumQueuesPerFamily = 0;
    uint32_t reserved = 0;
};

QRAPI QrResult QRCONV qrCreateInstanceWithGpuRequirements(
    const QrInstanceCreateInfo *pInfo,
    const QrayGpuRequirements *pGpuRequirements,
    QrInstance *pResult);

QRAPI QrResult QRCONV qrGetGpuContext(
    QrInstance qrInstance,
    QrayGpuContext *pContext);

QRAPI QrResult QRCONV qrSetGpuFrameHooks(
    QrInstance qrInstance,
    const QrayGpuFrameHooks *pHooks);

QRAPI QrResult QRCONV qrayValidateGpuRequirements(
    const QrayGpuRequirements *pRequirements,
    QrayContractDiagnostic *pDiagnostic = nullptr) noexcept;

QRAPI const QrayGpuContractInfo *QRCONV qrayGetGpuContractInfo() noexcept;

QRAPI uint32_t QRCONV qrayGetGpuContextContractVersion() noexcept;

} // namespace qray

#endif
