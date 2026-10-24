# Shared GPU context contract

`qray/gpu_context.h` exposes the renderer's NVRHI/Vulkan device to a C++ host.
The SDK creates and owns exactly one device per `QrInstance`; the host receives
a borrowed NVRHI device and the negotiated native Vulkan handles.

This is a source-level C++20 contract over the existing renderer package. The
C ABI in `qray.h` remains the transport for scene and frame data. The GPU
contract does not create a second renderer or a second scene path.

The host must provide the same compatible `nvrhi` and `nvrhi_vk` targets as the
rest of the package:

```cmake
find_package(Vulkan REQUIRED)
add_subdirectory(path/to/nvrhi)
find_package(qray_renderer CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE qray::renderer)
```

## Ownership model

```text
qray::qrCreateInstanceWithGpuRequirements
  SDK selects VkPhysicalDevice
  SDK creates VkDevice and queues
  SDK creates the NVRHI device
  SDK negotiates host requirements and SDK requirements

qray::qrGetGpuContext
  host receives borrowed NVRHI/Vulkan handles

host and SDK both use the same NVRHI device

host releases its own NVRHI resources
SDK releases renderer resources, NVRHI device, VkDevice and VkInstance
```

The host must not destroy the NVRHI device, `VkDevice`, queues, surface or
swapchain. A `QrayGpuContext` and all pointers inside it become invalid when
`qrDestroyInstance` is called.

## Requirements before device creation

Requirements are passed to a separate creation function because
`QrInstanceCreateInfo` has no extensible header:
The examples below assume `using namespace qray;`.

```cpp
QrInstance instance = nullptr;
QrResult result = qray::qrCreateInstanceWithGpuRequirements(
    &instanceInfo,
    &gpuRequirements,
    &instance);
```

The original `qrCreateInstance` remains valid and uses the SDK defaults.

`QrayGpuRequirements` supports:

| Requirement | Meaning |
|---|---|
| `minVulkanApiVersion` | Minimum physical-device Vulkan version; at least `VK_API_VERSION_1_3` |
| `minimumFramesInFlight` | Required frame slots; must not exceed `QrayGpuContractInfo::maximumFramesInFlight` |
| `requiredInstanceExtensions` | Instance extensions that must be present before `vkCreateInstance` |
| `requiredDeviceExtensions` | Device extensions that must be enabled at `vkCreateDevice` |
| `requiredNvrhiFeatures` | NVRHI features checked on the negotiated device |
| `requiredFormats` | Image formats, usage, tiling and sample counts checked before device creation |
| `queues` | Required graphics/present, compute and transfer queue counts |
| `preferredPhysicalDevice` | Candidate tried first; other valid devices may remain fallbacks |
| `pfnValidateDevice` | Optional host callback for limits, memory policy or other native checks |
| `pfnGetVulkanFeatureChain` | Optional host feature-chain extension for native Vulkan features |

Queue defaults are one graphics/present queue, one compute queue and one
transfer queue. A family may serve multiple roles. The dedicated flags require
compute or transfer families without graphics capability. The current maximum
is 16 queues per family and is reported by `qrayGetGpuContractInfo()`.

Format requirements are checked as 2D images:

```cpp
QrayGpuFormatRequirement format = {};
format.format = VK_FORMAT_R16G16B16A16_SFLOAT;
format.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
format.tiling = VK_IMAGE_TILING_OPTIMAL;
format.samples = VK_SAMPLE_COUNT_1_BIT;
format.requiredFormatFeatures =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
```

The SDK merges these requirements with its own RT, NVRHI and presentation
requirements. A candidate that fails any required check is skipped. Creation
returns `QR_CANT_FIND_PHYSICAL_DEVICE` if no candidate satisfies the complete
set.
All requirement arrays, strings, callbacks and callback-owned feature chains
are borrowed only for the synchronous create call.

### Native validation callback

Use `pfnValidateDevice` for requirements that cannot be expressed by the common
fields:

```cpp
QrResult ValidateDevice(VkPhysicalDevice device, void *userData)
{
    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(device, &properties);

    return properties.limits.maxStorageBufferRange >= requiredBytes
        ? QR_SUCCESS
        : QR_FEATURE_UNAVAILABLE;
}
```

Returning anything other than `QR_SUCCESS` rejects the candidate. The callback
runs synchronously before `vkCreateDevice` and must not retain the physical
device outside the call.

### Native feature chain

`pfnGetVulkanFeatureChain` returns a `VkBaseOutStructure` chain. The SDK
preserves its own chain and links the host chain at the end:

```cpp
QrResult GetFeatures(VkPhysicalDevice device, VkBaseOutStructure **ppChain, void *userData)
{
    auto *features = static_cast<VkPhysicalDeviceFooFeaturesEXT *>(userData);
    features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FOO_FEATURES_EXT;
    features->pNext = nullptr;
    features->foo = VK_TRUE;
    *ppChain = reinterpret_cast<VkBaseOutStructure *>(features);
    return QR_SUCCESS;
}
```

The host must have checked support against the selected physical device and
must keep the chain alive until the create function returns. The callback must
not replace or unlink SDK-owned `pNext` entries.

## Retrieving the shared context

```cpp
QrayGpuContext context = {};
context.header = {
    sizeof(QrayGpuContext),
    qray::kGpuContextContractVersion,
};
QR_CHECK(qray::qrGetGpuContext(instance, &context));

nvrhi::IDevice *device = context.pDevice;
```

The returned fields include:

- borrowed `nvrhi::IDevice*` and `nvrhi::vulkan::IDevice*`;
- `VkInstance`, `VkPhysicalDevice`, `VkDevice` and surface;
- graphics, compute and transfer queues with family indices and counts;
- actual frames in flight and Vulkan API version;
- native physical-device properties and memory properties;
- capability flags for the shared device, native Vulkan, timeline semaphores
  and additional queues.

The context returns queue index 0 for each SDK role. If a requested queue count
is greater than one, the host can call `vkGetDeviceQueue` with the reported
family index and another index below the reported count.

Host-created NVRHI resources are legal on this device:

```cpp
nvrhi::BufferDesc desc;
desc.byteSize = 256;
desc.initialState = nvrhi::ResourceStates::CopyDest;
nvrhi::BufferHandle buffer = device->createBuffer(desc);
```

The host owns and releases every such resource. The SDK does not track or
destroy unknown host resources. Release them before `qrDestroyInstance`.

## Frame protocol

Install at most one frame callback:

```cpp
QrayGpuFrameHooks hooks = {};
hooks.header = { sizeof(hooks), qray::kGpuContextContractVersion };
hooks.pfnCallback = RecordHostWork;
hooks.pUserData = &hostRenderer;
QR_CHECK(qray::qrSetGpuFrameHooks(instance, &hooks));
```

The callback is record-only and runs synchronously on the frame owner thread.
It receives an already-open NVRHI command list. It must not:

- open, close, submit or wait on the command list;
- call `qrDrawFrame`, destroy the device or change frame hooks recursively;
- retain the command list or textures after the callback returns;
- submit work on another thread while the SDK is recording.

The SDK defines two phases:

| Phase | Position | Useful work |
|---|---|---|
| `BeforeRenderer` | After the slot fence wait and command-list open, before RT scene recording | Host uploads, copies, setup and auxiliary compute |
| `BeforePresent` | After scene, composition, UI and post-UI; before the SDK present draw | Host post-process or overlay recording into `pOutputColor` |

`BeforeRenderer` has no output-color texture. `BeforePresent` exposes
`pOutputColor` and the acquired `pBackBuffer`. The SDK performs state
transitions and the actual queue submission after the callback.
`pOutputColor` can be null on a frame where the SDK has no valid composed
target; hosts must handle that case. Use `pOutputColor` for pre-present
composition; the SDK's present draw subsequently overwrites `pBackBuffer`.

Pass `nullptr` to `qrSetGpuFrameHooks` to remove the callback.
Install or remove hooks outside `qrStartFrame`/`qrDrawFrame`, on the frame
owner thread.

## Submission and synchronization

The SDK owns the main frame submission and present. Host command lists submitted
outside the callback must be serialized by the host and submitted on the same
owner thread. Same-queue submissions are ordered by submission order.

Do not call `vkDeviceWaitIdle` or an equivalent full-device wait in the normal
frame path. Use NVRHI queue completion or SDK-provided frame boundaries.

The current contract has one renderer camera and one SDK present per
`qrDrawFrame`. Additional host work can be recorded in the two callback phases.
A host-owned frame graph and host-owned presentation remain outside this
contract.

## Destruction order

```text
destroy all host-created NVRHI resources
qrSetGpuFrameHooks(instance, nullptr)        optional
qrDestroyInstance(instance)
```

The callback must not remain installed after its user data is released. Do not
destroy the window while the instance is alive. The SDK destroys its renderer
resources, NVRHI device, Vulkan device, surface and instance in that order.

## Compatibility and limits

- `qrayGetGpuContractInfo()` reports contract version, maximum frames in flight,
  queue limits and minimum Vulkan API version before creation.
- `qrayValidateGpuRequirements()` validates a descriptor without creating a
  device.
- New fields are append-only and guarded by `QrayContractHeader`.
- The contract is C++-only because it exposes NVRHI interfaces. C hosts keep
  using `qray.h`; a C bridge can hide the NVRHI pointer when needed.
- External host-owned `VkDevice` injection is not part of this contract. The SDK
  is the sole device owner.

## Reference validation

`qray_gpu_context_tests` covers descriptor validation and contract metadata.
`qray_spatial_twin_gpu` creates the instance through the GPU requirements path,
retrieves the shared NVRHI/Vulkan handles, creates a host NVRHI buffer and
verifies both frame callback phases while rendering the full RT test scene.
