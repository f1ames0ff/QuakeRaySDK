#include "GpuContextInternal.h"

#include "Common.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace qray
{

namespace
{

constexpr uint32_t kMaximumQueuesPerFamily = 16;

QrayGpuContractInfo MakeContractInfo()
{
    QrayGpuContractInfo info = {};
    info.header = { sizeof(QrayGpuContractInfo), kGpuContextContractVersion };
    info.contractVersion = kGpuContextContractVersion;
    info.minimumVulkanApiVersion = VK_API_VERSION_1_3;
    info.maximumFramesInFlight = MAX_FRAMES_IN_FLIGHT;
    info.requiredGraphicsQueues = 1;
    info.maximumQueuesPerFamily = kMaximumQueuesPerFamily;
    return info;
}

QrResult Fail(QrayContractDiagnostic *pDiagnostic, QrResult result, uint32_t code,
              uint32_t index = UINT32_MAX) noexcept
{
    if (pDiagnostic != nullptr)
    {
        pDiagnostic->result = result;
        pDiagnostic->code = code;
        pDiagnostic->index = index;
    }
    return result;
}

QrResult Success(QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pDiagnostic != nullptr)
    {
        pDiagnostic->result = QR_SUCCESS;
        pDiagnostic->code = 0;
        pDiagnostic->index = UINT32_MAX;
    }
    return QR_SUCCESS;
}

template<typename T>
bool ArrayValid(const QrayArrayView<T> &array) noexcept
{
    return array.count == 0 || array.pData != nullptr;
}

bool ValidStringArray(const QrayArrayView<const char *> &array) noexcept
{
    if (!ArrayValid(array))
        return false;

    for (uint32_t i = 0; i < array.count; ++i)
        if (array.pData[i] == nullptr || array.pData[i][0] == '\0')
            return false;
    return true;
}

void SetError(std::string *pError, std::string message)
{
    if (pError != nullptr)
        *pError = std::move(message);
}

bool HasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

bool HasRequiredDeviceExtensions(VkPhysicalDevice physicalDevice,
                                 const QrayGpuRequirements &requirements,
                                 std::string *pError)
{
    if (requirements.requiredDeviceExtensions.count == 0)
        return true;

    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS)
    {
        SetError(pError, "failed to enumerate Vulkan device extensions");
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (count != 0 &&
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) != VK_SUCCESS)
    {
        SetError(pError, "failed to enumerate Vulkan device extensions");
        return false;
    }

    for (uint32_t i = 0; i < requirements.requiredDeviceExtensions.count; ++i)
    {
        const char *name = requirements.requiredDeviceExtensions.pData[i];
        if (!HasExtension(extensions, name))
        {
            SetError(pError, std::string("required Vulkan device extension is unavailable: ") + name);
            return false;
        }
    }
    return true;
}

bool HasRequiredQueues(VkPhysicalDevice physicalDevice,
                       VkSurfaceKHR surface,
                       const QrayGpuRequirements &requirements,
                       std::string *pError)
{
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    if (familyCount == 0)
    {
        SetError(pError, "physical device has no queue families");
        return false;
    }

    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    const QrayGpuQueueRequirements &queues = requirements.queues;
    bool graphicsFound = false;
    bool computeFound = false;
    bool transferFound = false;

    for (uint32_t i = 0; i < familyCount; ++i)
    {
        VkBool32 presentSupported = VK_FALSE;
        if (surface != VK_NULL_HANDLE)
            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &presentSupported);

        const VkQueueFlags flags = families[i].queueFlags;
        const bool graphics = (flags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                              (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                              (flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                              presentSupported == VK_TRUE &&
                              families[i].queueCount >= queues.graphicsQueueCount;
        const bool compute = (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                             families[i].queueCount >= queues.computeQueueCount;
        const bool transfer = (flags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                              families[i].queueCount >= queues.transferQueueCount;

        graphicsFound = graphicsFound || graphics;
        if ((queues.flags & QRAY_GPU_QUEUE_REQUIRE_DEDICATED_COMPUTE) != 0)
            computeFound = computeFound || (compute && (flags & VK_QUEUE_GRAPHICS_BIT) == 0);
        else
            computeFound = computeFound || compute;

        if ((queues.flags & QRAY_GPU_QUEUE_REQUIRE_DEDICATED_TRANSFER) != 0)
            transferFound = transferFound || (transfer &&
                (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0);
        else
            transferFound = transferFound || transfer;
    }

    if (!graphicsFound)
    {
        SetError(pError, "physical device has no graphics/present queue that satisfies the requested count");
        return false;
    }
    if (!computeFound)
    {
        SetError(pError, "physical device has no compute queue that satisfies the request");
        return false;
    }
    if (!transferFound)
    {
        SetError(pError, "physical device has no transfer queue that satisfies the request");
        return false;
    }
    return true;
}

bool HasRequiredFormats(VkPhysicalDevice physicalDevice,
                        const QrayGpuRequirements &requirements,
                        std::string *pError)
{
    for (uint32_t i = 0; i < requirements.requiredFormats.count; ++i)
    {
        const QrayGpuFormatRequirement &required = requirements.requiredFormats.pData[i];

        if (required.requiredFormatFeatures != 0)
        {
            VkFormatProperties formatProperties = {};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, required.format, &formatProperties);

            const VkFormatFeatureFlags available =
                required.tiling == VK_IMAGE_TILING_LINEAR
                    ? formatProperties.linearTilingFeatures
                    : formatProperties.optimalTilingFeatures;

            if ((available & required.requiredFormatFeatures) != required.requiredFormatFeatures)
            {
                SetError(pError, "physical device does not expose required format features for format " +
                                     std::to_string(uint32_t(required.format)));
                return false;
            }
        }

        VkImageFormatProperties imageProperties = {};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice,
            required.format,
            VK_IMAGE_TYPE_2D,
            required.tiling,
            required.usage,
            required.imageFlags,
            &imageProperties);

        if (result != VK_SUCCESS ||
            (imageProperties.sampleCounts & required.samples) != required.samples)
        {
            SetError(pError, "physical device does not support the requested image format/sample combination for format " +
                                 std::to_string(uint32_t(required.format)));
            return false;
        }
    }
    return true;
}

} // namespace

namespace gpu
{

QrResult ValidateRequirements(const QrayGpuRequirements *pRequirements,
                              QrayContractDiagnostic *pDiagnostic) noexcept
{
    if (pRequirements == nullptr)
        return Success(pDiagnostic);

    try
    {
        if (pRequirements->header.size < sizeof(QrayGpuRequirements) ||
            (pRequirements->header.version >> 16) != kGpuContextContractMajor)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 1);

        if (pRequirements->minVulkanApiVersion < VK_API_VERSION_1_3 ||
            pRequirements->minimumFramesInFlight == 0 ||
            pRequirements->minimumFramesInFlight > MAX_FRAMES_IN_FLIGHT ||
            pRequirements->reserved0 != 0 || pRequirements->reserved1 != 0)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 2);

        if (!ValidStringArray(pRequirements->requiredInstanceExtensions) ||
            !ValidStringArray(pRequirements->requiredDeviceExtensions) ||
            !ArrayValid(pRequirements->requiredNvrhiFeatures) ||
            !ArrayValid(pRequirements->requiredFormats))
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 3);

        const QrayGpuQueueRequirements &queues = pRequirements->queues;
        if (queues.graphicsQueueCount == 0 ||
            queues.graphicsQueueCount > kMaximumQueuesPerFamily ||
            queues.computeQueueCount == 0 ||
            queues.computeQueueCount > kMaximumQueuesPerFamily ||
            queues.transferQueueCount == 0 ||
            queues.transferQueueCount > kMaximumQueuesPerFamily ||
            (queues.flags & ~(QRAY_GPU_QUEUE_REQUIRE_DEDICATED_COMPUTE |
                              QRAY_GPU_QUEUE_REQUIRE_DEDICATED_TRANSFER)) != 0 ||
            queues.reserved != 0)
            return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 4);

        for (uint32_t i = 0; i < pRequirements->requiredFormats.count; ++i)
        {
            const QrayGpuFormatRequirement &format = pRequirements->requiredFormats.pData[i];
            if (format.format == VK_FORMAT_UNDEFINED || format.usage == 0 ||
                (format.tiling != VK_IMAGE_TILING_OPTIMAL &&
                 format.tiling != VK_IMAGE_TILING_LINEAR) ||
                format.samples == 0)
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 5, i);
        }

        for (uint32_t i = 0; i < pRequirements->requiredNvrhiFeatures.count; ++i)
        {
            if (uint32_t(pRequirements->requiredNvrhiFeatures.pData[i]) >
                uint32_t(nvrhi::Feature::EnhancedBarriers))
                return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 6, i);
        }

        return Success(pDiagnostic);
    }
    catch (...)
    {
        return Fail(pDiagnostic, QR_WRONG_ARGUMENT, 7);
    }
}

bool HasRequiredInstanceExtensions(const QrayGpuRequirements &requirements,
                                   std::string *pError)
{
    if (requirements.requiredInstanceExtensions.count == 0)
        return true;

    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
    {
        SetError(pError, "failed to enumerate Vulkan instance extensions");
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (count != 0 &&
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS)
    {
        SetError(pError, "failed to enumerate Vulkan instance extensions");
        return false;
    }

    for (uint32_t i = 0; i < requirements.requiredInstanceExtensions.count; ++i)
    {
        const char *name = requirements.requiredInstanceExtensions.pData[i];
        if (!HasExtension(extensions, name))
        {
            SetError(pError, std::string("required Vulkan instance extension is unavailable: ") + name);
            return false;
        }
    }
    return true;
}

bool PhysicalDeviceMeetsRequirements(VkPhysicalDevice physicalDevice,
                                     VkSurfaceKHR surface,
                                     const QrayGpuRequirements &requirements,
                                     std::string *pError)
{
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    if (properties.apiVersion < requirements.minVulkanApiVersion)
    {
        SetError(pError, "physical device Vulkan API version is below the requested minimum");
        return false;
    }

    if (!HasRequiredDeviceExtensions(physicalDevice, requirements, pError) ||
        !HasRequiredQueues(physicalDevice, surface, requirements, pError) ||
        !HasRequiredFormats(physicalDevice, requirements, pError))
        return false;

    if (requirements.pfnValidateDevice != nullptr)
    {
        try
        {
            const QrResult hostResult =
                requirements.pfnValidateDevice(physicalDevice, requirements.pUserData);
            if (hostResult != QR_SUCCESS)
            {
                SetError(pError, "host physical-device validation rejected the candidate");
                return false;
            }
        }
        catch (...)
        {
            SetError(pError, "host physical-device validation threw an exception");
            return false;
        }
    }

    return true;
}

QrResult CheckNvrhiFeatures(nvrhi::IDevice *pDevice,
                            const QrayGpuRequirements &requirements,
                            std::string *pError)
{
    if (pDevice == nullptr)
    {
        SetError(pError, "NVRHI device is null");
        return QR_GRAPHICS_API_ERROR;
    }

    for (uint32_t i = 0; i < requirements.requiredNvrhiFeatures.count; ++i)
    {
        const nvrhi::Feature feature = requirements.requiredNvrhiFeatures.pData[i];
        if (!pDevice->queryFeatureSupport(feature))
        {
            SetError(pError, "NVRHI feature is unavailable: " +
                                 std::to_string(uint32_t(feature)));
            return QR_FEATURE_UNAVAILABLE;
        }
    }
    return QR_SUCCESS;
}

QrResult AppendVulkanFeatureChain(VkPhysicalDevice physicalDevice,
                                  VkPhysicalDeviceFeatures2 *pFeatures,
                                  const QrayGpuRequirements &requirements,
                                  std::string *pError)
{
    if (requirements.pfnGetVulkanFeatureChain == nullptr)
        return QR_SUCCESS;

    VkBaseOutStructure *hostChain = nullptr;
    try
    {
        const QrResult result = requirements.pfnGetVulkanFeatureChain(
            physicalDevice, &hostChain, requirements.pUserData);
        if (result != QR_SUCCESS)
        {
            SetError(pError, "host Vulkan feature-chain callback failed");
            return result;
        }
    }
    catch (...)
    {
        SetError(pError, "host Vulkan feature-chain callback threw an exception");
        return QR_GRAPHICS_API_ERROR;
    }

    if (hostChain == nullptr)
        return QR_SUCCESS;

    VkBaseOutStructure *tail = reinterpret_cast<VkBaseOutStructure *>(pFeatures);
    while (tail->pNext != nullptr)
        tail = tail->pNext;
    tail->pNext = hostChain;
    return QR_SUCCESS;
}

} // namespace gpu

QrResult qrayValidateGpuRequirements(const QrayGpuRequirements *pRequirements,
                                     QrayContractDiagnostic *pDiagnostic) noexcept
{
    return gpu::ValidateRequirements(pRequirements, pDiagnostic);
}

const QrayGpuContractInfo *qrayGetGpuContractInfo() noexcept
{
    static const QrayGpuContractInfo info = MakeContractInfo();
    return &info;
}

uint32_t qrayGetGpuContextContractVersion() noexcept
{
    return kGpuContextContractVersion;
}

} // namespace qray
