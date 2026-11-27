# QuakeRay SDK

Source-level renderer package for QuakeRay. It owns the Vulkan/NVRHI ray-tracing
renderer, the public `qray.h` C ABI, and the CMake package interface used by a
host engine.

Start with [AGENTS.md](AGENTS.md) and [docs/agent-integration-guide.md](docs/agent-integration-guide.md) (**QuakeRaySDK Integration Guide for Agents**) for strict SDK vs. host boundaries and engine adapter integration rules, or [docs/README.md](docs/README.md) for the complete documentation map.

The renderer core does not include game logic, map loaders, or host-specific
assets. Bundled SDK fallback textures (`BlueNoise_LDR_RGBA_128.png` and
`WaterNormal_n.png`) ship under `Assets/textures/`, while host-specific game
textures, materials, and UI artwork stay in the host repository. The optional
`quakerayeditor` target is a standalone host around `qray::renderer`; it
includes OBJ/BSP loading, archive mounting (`.pak`/`.pk3`/`.pkz`/`.zip`),
pluggable `EditorFormatAdapter` hooks, meter conversion via `EditorSpatial`
(`qrSetSpatialConvention` + 40 units/m boundary), picking, material/light/render
authoring, local Vulkan AI PBR texture generation, and `EditorSceneRuntime`
shared with runtime hosts.

## Package contents

- Package version `0.60.0`; the independently versioned C ABI is
  `QR_API_VERSION 1.07.0000`.
- `Include/qray/qray.h` — public C ABI, `QR_API_VERSION 1.07.0000`.
- `Include/qray/host_contract.h` — C++20 scene/frame/update/host/sidecar
  contracts over the C ABI.
- `Include/qray/gpu_context.h` — SDK-owned shared NVRHI/Vulkan context,
  create-time requirements and render-frame host callbacks.
- `Assets/textures/` — bundled fallback runtime textures
  (`BlueNoise_LDR_RGBA_128.png`, `WaterNormal_n.png`).
- `Source/` — renderer, RHI passes, lighting, shaders, and the FidelityFX API.
- `cmake/qray_rendererConfig.cmake.in` — installed package config.
- `cmake/nvrhi-max-binding-layouts.patch` — required NVRHI pin adjustment for the RT pipelines.
- `Tests/` — engine-free GPU and policy tests.
- `AGENTS.md` — mandatory rules, responsibility boundaries, and quality gates for AI agents.
- `docs/README.md` — entry point and complete reading map for every SDK document.
- `docs/agent-integration-guide.md` — QuakeRaySDK Integration Guide for Agents (runtime & `EditorFormatAdapter` guide and pitfalls).
- `docs/api.md` — concise catalogue of the complete public renderer API.
- `docs/host-contract.md` — adapter boundary and legacy-renderer feature mapping.
- `docs/host-driver-contract.md` — C++20 `QraySceneAsset`, frame views/updates, `IQrayHost`, and `QraySidecar` contracts.
- `docs/gpu-context.md` — device ownership, requirements, shared handles and
  frame protocol for NVRHI hosts.
- `docs/sky-and-atmosphere.md` — procedural sky, volumetric clouds, god rays and units.
- `docs/sun-and-god-rays.md` — visible sun, directional lighting and shafts.
- `docs/lights-and-sampling.md` — light records, dlights, DTAL, clusters and NEE.
- `docs/gi-and-denoiser.md` — GI, ReSTIR, denoiser and temporal lifecycle.
- `docs/host-feature-matrix.md` — feature-to-ABI cross-reference for new hosts.
- `docs/spatial-contract.md` — meter-valued inputs, orientation, and conversion contract.
- `docs/embedding.md` — host integration guide.
- `docs/editor-parity.md` — standalone editor feature parity checklist and verification notes.

## Requirements

- Windows with Visual Studio 2022 (Win32 surface) or Linux with Clang/GCC and
  X11/Xlib headers (`QR_USE_SURFACE_XLIB`), and CMake 3.23 or newer (the
  FidelityFX SDK source build requires 3.23; the renderer's own minimum is 3.20).
- Vulkan SDK with headers new enough for NVRHI (Vulkan 1.4.318 or newer).
- NVRHI source compatible with pin `d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`,
  with `c_MaxBindingLayouts` raised to at least 13 (the patch sets it to 16),
  or an existing compatible `nvrhi`/`nvrhi_vk` target with the same limit.
- FidelityFX SDK 1.1.4 sources, vendored under
  `Source/FidelityFX_SDK_1.1.4/src`. The Vulkan provider is built from those
  sources with `Source/FidelityFX_SDK_1.1.4/windows/build_provider.ps1` on
  Windows or `Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh` on Linux.
  No checked-in `amd_fidelityfx_vk` binary is linked.

## Standalone build

```powershell
git -C C:/path/to/nvrhi apply C:/path/to/QuakeRaySDK/cmake/nvrhi-max-binding-layouts.patch
cmake -S . -B build -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi
cmake --build build --config Debug
```

NVRHI's stock `c_MaxBindingLayouts` limit is 8, while the direct and indirect
RT pipelines need 13. `renderer/cmake/nvrhi-max-binding-layouts.patch` is the
pin-matched change (it raises the limit to 16). A compatible NVRHI fork may
ship the same limit instead of using the patch. Installed packages place the
patch beside the installed SDK documentation.

NVRHI supplies the `nvrhi` and `nvrhi_vk` targets. If they are already present
in the parent build, `QR_NVRHI_SOURCE_DIR` can be omitted.

## Linux build

The provider is built from source first, then the renderer is configured
against it:

```bash
GLSLANG_VALIDATOR=/path/to/glslangValidator \
VULKAN_HEADERS_DIR=/path/to/vulkan/headers \
Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh

cmake -S . -B build/linux-debug -DCMAKE_BUILD_TYPE=Debug \
    -DQR_FFX_VK_SO="$PWD/build/ffx/libamd_fidelityfx_vk.so"
cmake --build build/linux-debug --parallel
```

The host repository's `build_linux.sh` performs both steps. The provider is
loaded through the executable's `RUNPATH`; the `.so` carries the
`libamd_fidelityfx_vk.so` SONAME.

## Optional editor build

```powershell
cmake -S . -B build `
    -DQR_BUILD_EDITOR=ON `
    -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi `
    -DQR_GGML_SOURCE_DIR=C:/path/to/ggml `
    -DQR_EDITOR_SHADER_DIR=C:/path/to/shaders
cmake --build build --config Debug --target quakerayeditor quakeray_ai_worker
```

The editor target is `OFF` by default and does not change the renderer package.
Passing `-DQR_GGML_SOURCE_DIR` builds the standalone `quakeray_ai_worker` helper
used by `quakerayeditor`'s Vulkan AI PBR texture pipeline (4x upscaling, diffuse
de-lighting, normal generation, depth/height/gloss synthesis, and Relief POM
authoring running locally on 16 GB desktop GPUs including AMD Radeon via
`ggml-vulkan`). See [editor/README.md](editor/README.md) for its scene path,
local AI workflow, and runtime options.

## Install and consume

```powershell
cmake --install build --prefix C:/path/to/qray-sdk
```

The install tree contains:

```text
include/qray/qray.h
include/qray/host_contract.h
include/qray/gpu_context.h
include/ffx/ffx_api/...
lib/renderer.lib                 (Windows) / lib/librenderer.a (Linux)
lib/amd_fidelityfx_vk.lib        (Windows)
bin/amd_fidelityfx_vk.dll        (Windows)
lib/libamd_fidelityfx_vk.so      (Linux, when present)
lib/cmake/qray_renderer/qray_rendererConfig.cmake
share/doc/quakeray/...           (AGENTS.md, docs/*.md, NVRHI patch)
```

The imported target carries `QR_STATIC` plus `QR_USE_SURFACE_WIN32` and
`NOMINMAX` on Windows, or `QR_USE_SURFACE_XLIB` on Linux, so a host includes
the C ABI normally without redefining those switches:

```cmake
find_package(Vulkan REQUIRED)
add_subdirectory(path/to/nvrhi)
find_package(qray_renderer CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE qray::renderer)
```

The package config checks for `nvrhi` and `nvrhi_vk`, resolves the installed
Windows `qray::ffx` target, and exposes `qray::renderer`.

## Spatial contract

The renderer maps meter-valued host data into a right-handed, `+Z`-up internal
space at a 1:1 scale. The host declares only its axes
and handedness with `qrSetSpatialConvention`; all reserved fields must be
zero, and there is no configurable unit scale. Omitting the call selects
identity axes and still requires meter-valued inputs. Right-handed and
left-handed signed axis permutations are supported. QuakeRay's host adapter
converts native Quake units to meters before spatial SDK calls;
`SWAPCHAIN` uploads remain pixel-based. `qrSetSmokeWindDirection` supplies
smoke wind in host scene space, and a cluster upload that omits both PVS tables
is treated as all-visible. The world-depth target is `R32_SFLOAT`;
`rayLength` is bounded by the 250,000 m safety ceiling.
The full field-by-field conversion rules and the second-host checklist are in
[docs/spatial-contract.md](docs/spatial-contract.md).

## Frame readback

The public API exposes asynchronous readback of the final LDR image:

```c
qrGetFrameReadbackRequirements(...);
qrRequestFrameReadback(...);
qrGetFrameReadbackStatus(...);
```

The first call returns the current width, height, row pitch and buffer size for
`RGBA8_UNORM` or `BGRA8_UNORM`. A request is made during a frame, before
`qrDrawFrame`; the SDK copies the final composited image, including UI, into a
staging texture and completes it after the corresponding frame slot has been
submitted. The host owns `pDst` and must keep it alive until the status reports
done.

The path does not call `vkDeviceWaitIdle`. Requests use the frame-slot ring, so
the host should poll `qrGetFrameReadbackStatus`; a slot is busy while its
previous request is queued or pending. `includeUi = QR_FALSE` and
`QR_FRAME_READBACK_FORMAT_RGBA16F` are not supported in this revision and return
`QR_FEATURE_UNAVAILABLE`.

## Scope

The SDK is a source-level package supporting Windows (Win32) and Linux
(X11/Xlib via `QR_USE_SURFACE_XLIB`). It owns the Vulkan/NVRHI device,
swapchain, GPU resources, and acceleration structures; a host supplies scene
uploads through the `Qr*` C ABI in `qray.h` (or the C++20 `host_contract.h`
layer) and may share the SDK-owned Vulkan/NVRHI device and frame callbacks
through `Include/qray/gpu_context.h` (see [docs/gpu-context.md](docs/gpu-context.md)).
Host-owned external `VkDevice` injection and native Wayland surfaces are not
part of the current package.

`rayLength` is expressed in meters up to 250,000 m; the shader sky sentinel
follows `2 * rayLength`,
and the world-depth target is `R32_SFLOAT`. Right-handed and left-handed signed axis permutations with
arbitrary instance rotations and non-uniform scale are supported. Only a
world whose declared up axis is not an axis permutation has to be rotated by
the host before upload; the spatial contract is in
[docs/spatial-contract.md](docs/spatial-contract.md).
