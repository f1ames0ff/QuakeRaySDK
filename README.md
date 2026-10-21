# QuakeRay SDK

Source-level renderer package for QuakeRay. It owns the Vulkan/NVRHI ray-tracing
renderer, the public `qray.h` C ABI, and the CMake package interface used by a
host engine.

The SDK does not include Quake game logic, map loaders, or host assets. The
QuakeRay engine is the reference host; textures, materials, and cursor artwork
stay in the host repository under `gfx/`.

## Package contents

- `Include/qray/qray.h` — public C ABI, `QR_API_VERSION 1.03.0000`.
- `Source/` — renderer, RHI passes, lighting, shaders, and the FidelityFX API.
- `cmake/qray_rendererConfig.cmake.in` — installed package config.
- `Tests/` — engine-free GPU and policy tests.

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

## Scope

The SDK is a source-level package. It owns GPU resources and acceleration
structures; a host supplies scene uploads through the `Qr*` contract in
`qray.h`. Embedded/device-sharing integration and non-Windows targets are not
part of the current package.
