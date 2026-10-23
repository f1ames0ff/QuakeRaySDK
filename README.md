# QuakeRay SDK

Source-level renderer package for QuakeRay. It owns the Vulkan/NVRHI ray-tracing
renderer, the public `qray.h` C ABI, and the CMake package interface used by a
host engine.

The renderer core does not include Quake game logic, map loaders, or host
assets. The QuakeRay engine is the reference host; textures, materials, and
cursor artwork stay in the host repository under `gfx/`. The optional
`quakerayeditor` target is a separate host around `qray::renderer`; it includes
a geometry-only OBJ/BSP29 scene path for editor development.

## Package contents

- `Include/qray/qray.h` — public C ABI, `QR_API_VERSION 1.06.0000`.
- `Source/` — renderer, RHI passes, lighting, shaders, and the FidelityFX API.
- `cmake/qray_rendererConfig.cmake.in` — installed package config.
- `Tests/` — engine-free GPU and policy tests.
- `docs/spatial-contract.md` — meter-valued inputs, orientation, and conversion contract.
- `docs/embedding.md` — host integration guide.

## Requirements

- Windows with Visual Studio 2022 and CMake 3.20 or newer.
- Vulkan SDK with headers new enough for NVRHI (Vulkan 1.4.318 or newer).
- NVRHI source tree or an existing `nvrhi`/`nvrhi_vk` target.
- FidelityFX SDK API headers and `amd_fidelityfx_vk` provider. The in-tree
  provider is used by default; `QR_FFX_SDK_ROOT` selects an installed SDK.

## Standalone build

```powershell
cmake -S . -B build -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi
cmake --build build --config Debug
```

NVRHI supplies the `nvrhi` and `nvrhi_vk` targets. If they are already present
in the parent build, `QR_NVRHI_SOURCE_DIR` can be omitted.

## Optional editor build

```powershell
cmake -S . -B build `
    -DQR_BUILD_EDITOR=ON `
    -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi `
    -DQR_EDITOR_SHADER_DIR=C:/path/to/shaders
cmake --build build --config Debug --target quakerayeditor
```

The editor target is `OFF` by default and does not change the renderer package.
See [editor/README.md](editor/README.md) for its scene path and runtime options.

## Install and consume

```powershell
cmake --install build --prefix C:/path/to/qray-sdk
```

The install tree contains:

```text
include/qray/qray.h
include/ffx/ffx_api/...
lib/renderer.lib
lib/amd_fidelityfx_vk.lib
bin/amd_fidelityfx_vk.dll
lib/cmake/qray_renderer/qray_rendererConfig.cmake
```

A host project provides NVRHI and Vulkan, then links the imported target:

```cmake
find_package(Vulkan REQUIRED)
add_subdirectory(path/to/nvrhi)
find_package(qray_renderer CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE qray::renderer)
```

The package config checks for `nvrhi` and `nvrhi_vk`, resolves the installed
FidelityFX provider, and exposes `qray::renderer`.

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

The SDK is a source-level package. It owns GPU resources and acceleration
structures; a host supplies scene uploads through the `Qr*` contract in
`qray.h`. Embedded/device-sharing integration and non-Windows targets are not
part of the current package.

`rayLength` is expressed in meters up to 250,000 m; the shader sky sentinel
follows `2 * rayLength`,
and the world-depth target is `R32_SFLOAT`. Right-handed and left-handed signed axis permutations with
arbitrary instance rotations and non-uniform scale are supported. Only a
world whose declared up axis is not an axis permutation has to be rotated by
the host before upload; the spatial contract is in
[docs/spatial-contract.md](docs/spatial-contract.md).
