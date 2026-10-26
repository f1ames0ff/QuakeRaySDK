# QuakeRaySDK documentation

This is the entry point for every host integrating the renderer. The public
headers remain normative; these documents explain the supported call order,
ownership boundaries and renderer features.

## Choose a reading path

### C host or scene-only host

1. [embedding.md](embedding.md) - minimal end-to-end integration.
2. [api.md](api.md) - function groups, state lifetime and ownership.
3. [spatial-contract.md](spatial-contract.md) - meters, axes and handedness.
4. [host-contract.md](host-contract.md) - adapter boundary and legacy renderer mapping.
5. [host-feature-matrix.md](host-feature-matrix.md) - feature-to-API lookup.

Use `qrCreateInstance`. The host uploads CPU scene data and accepts the SDK's
default device requirements.

### C++ NVRHI host with shared GPU access

1. [gpu-context.md](gpu-context.md) - requirements before device creation,
   ownership and frame callbacks.
2. [embedding.md](embedding.md) - complete renderer lifecycle around the GPU contract.
3. [api.md](api.md) - scene/frame API used together with the shared device.
4. [spatial-contract.md](spatial-contract.md) - required unit conversion.
5. [host-feature-matrix.md](host-feature-matrix.md) - supported renderer features.

Use `qray::qrCreateInstanceWithGpuRequirements` when requirements must be
specified before creation. `qrCreateInstance` plus `qray::qrGetGpuContext` is also
valid when SDK defaults are sufficient. Do not create a second Vulkan/NVRHI
device for the same renderer or window.

### Host-driver and authoring contracts

1. [host-driver-contract.md](host-driver-contract.md) - scene binding, updates,
   sidecar semantics, capability flags and failure behavior.
2. [host-contract.md](host-contract.md) - what the adapter supplies and what
   remains host-owned.
3. [api.md](api.md) - underlying C transport.

The C++ contracts lower to `qray.h`; they do not create a second scene path.

## Complete document map

| Document | Purpose |
|---|---|
| [embedding.md](embedding.md) | First integration, build/package setup, instance creation and frame lifecycle. |
| [gpu-context.md](gpu-context.md) | SDK-owned shared NVRHI/Vulkan device, create-time requirements, resource ownership and frame hooks. |
| [api.md](api.md) | Public function catalogue, state lifetime, synchronization and error rules. |
| [spatial-contract.md](spatial-contract.md) | Meter-valued inputs, axes, handedness and conversion rules. |
| [host-contract.md](host-contract.md) | Host/data boundary, material mapping and pre-2006 engine feature profiles. |
| [host-driver-contract.md](host-driver-contract.md) | `QraySceneAsset`, frame views/updates, `IQrayHost`, `QraySidecar` and `QraySceneBinding`. |
| [host-feature-matrix.md](host-feature-matrix.md) | Fast lookup from renderer feature to public interface. |
| [lights-and-sampling.md](lights-and-sampling.md) | Light records, dlights, DTAL, clusters and NEE. |
| [gi-and-denoiser.md](gi-and-denoiser.md) | GI, ReSTIR, denoiser and temporal history. |
| [sky-and-atmosphere.md](sky-and-atmosphere.md) | Procedural sky, clouds, atmosphere and god rays. |
| [sun-and-god-rays.md](sun-and-god-rays.md) | Visible solar disc, directional sun and shaft controls. |
| [editor-parity.md](editor-parity.md) | Current editor implementation, UI behavior and parity evidence. |

## Contract hierarchy

- `Include/qray/qray.h` is the normative C ABI and scene transport.
- `Include/qray/host_contract.h` is the C++20 source-level
  scene/frame/host/sidecar layer over that ABI.
- `Include/qray/gpu_context.h` is the C++20 shared GPU context
  and frame-hook contract.
- Documents in this directory explain those headers but do not extend their
  ownership or lifetime guarantees.

## Non-negotiable integration rules

- Pass positions, distances, radii and areas in meters.
- Use exactly one instance creation path. Use
  `qray::qrCreateInstanceWithGpuRequirements` for custom requirements, or
  `qrCreateInstance` plus `qray::qrGetGpuContext` when SDK defaults are sufficient.
- The SDK owns the Vulkan/NVRHI device, queues, swapchain and presentation.
- Use one `QrInstance` per window; do not create instances per pass, camera or
  view. Every extra instance creates another device and memory context.
- A host needing NVRHI resources must use the returned device and must not
  create a second device or separate swapchain for the same target.
- Destroy host-created resources before `qrDestroyInstance`; destroy the
  instance before the `HWND`.
- One `qrDrawFrame` represents one camera and one SDK present. Schedule
  additional views as additional frames or flatten them on the host.
