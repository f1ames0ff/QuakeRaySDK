# FidelityFX SDK 1.1.4 (source build)

This directory contains the AMD FidelityFX SDK 1.1.4 sources used to build the
FSR 3.1 Vulkan provider. The provider is compiled from these sources on both
Windows and Linux; the renderer never links a checked-in `amd_fidelityfx_vk`
binary.

## Layout

```text
src/sdk/        FidelityFX SDK 1.1.4 host + GPU sources and shader sources
src/ffx-api/    FidelityFX API sources and public headers
linux/          Linux provider build (glslangValidator permutation generator)
windows/        Windows provider build (SDK CMake + bundled FidelityFX_SC.exe)
patches/        The local FidelityFX 1.1.4 source changes
```

`src/sdk/tools/binary_store` keeps only the two tools the SDK's Windows VK
build needs: `FidelityFX_SC.exe` and `glslangValidator.exe`. The SDK's DX12-only
PIX runtime binaries and the rest of the shader-compiler build tree are not
vendored.

## Local patches

The vendored tree already includes these changes; the patch files are kept so
that an external SDK 1.1.4 checkout can be brought to the same state. From the
SDK root (the directory containing `sdk/` and `ffx-api/`):

```bash
for p in /path/to/QuakeRaySDK/renderer/Source/FidelityFX_SDK_1.1.4/patches/*.patch; do
    git apply "$p"
done
```

`QR_FFX_SDK_ROOT` then points at that external root for both the provider build
and the engine configure step.

1. `0001-fsr3-luma-history-rgba16f.patch` — `rw_luma_history` is declared
   `rgba16f`, matching the `R16G16B16A16_FLOAT` resource the upscaler creates.
   The SDK ships an `rgba8` declaration, which is an undefined storage-image
   format mismatch.
2. `0002-fsr3-context-size-linux-wchar.patch` — the opaque FSR3 upscaler
   context is four times the SDK default. The private context embeds
   `wchar_t` resource names; on Linux `wchar_t` is 32-bit, so the private
   structure (about 818 KiB) no longer fits the SDK's 128 Ki *uint32_t*
   context. The Windows build fits, and the larger context is harmless there.
3. `0003-vk-scratch-alignment.patch` — every array inside the Vulkan backend
   scratch buffer starts on a 16-byte boundary. GCC lowers some aggregate
   stores to aligned SSE moves, and the SDK's original 8-byte/4-byte layout
   produced an unaligned `movaps` in `CreateBackendContextVK` at `-O2`.
4. `0004-ffx-api-entry-linux.patch` — `FFX_API_ENTRY` is empty on non-Windows
   compilers instead of `__declspec(dllexport)`.
5. `0005-ffx-provider-msvc-literal.patch` — replaces the MSVC-only
   `0xF5A5'CA1Eui64` literal with `0xF5A5CA1Eull`.

## Linux build

```bash
GLSLANG_VALIDATOR=/path/to/glslangValidator \
VULKAN_HEADERS_DIR=/path/to/vulkan/headers \
renderer/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh
```

Output: `build/ffx/libamd_fidelityfx_vk.so` (or `QR_FFX_OUT_DIR`). The script
builds a small permutation generator from `linux/shader_compiler`, compiles all
40 FSR3 upscaler permutations (10 passes × base/`_wave64`/`_16bit`/
`_wave64_16bit`) and then links the FSR3-only provider. The generated
permutation headers are cached under `<out-dir>/permutations`; delete that
directory or set `QR_FFX_FORCE_SHADERS=1` after changing shader sources.

The Linux provider is built at `-O2 -g` by default. Override the flags with
`FSR_PROVIDER_CXXFLAGS` (for example `FSR_PROVIDER_CXXFLAGS="-O0 -g"`).

## Windows build

```powershell
renderer\Source\FidelityFX_SDK_1.1.4\windows\build_provider.ps1 `
    -OutDir build\ffx -Config Debug
```

Output: `amd_fidelityfx_vk.dll` and `amd_fidelityfx_vk.lib` in the output
directory. The script uses AMD's own SDK CMake and bundled shader compiler, and
applies the patches above to a copy of the sources when needed. The top-level
`build_win.ps1` invokes it automatically and passes the resulting paths to the
renderer CMake build.

## Engine integration

CMake consumes:

- `QR_FFX_SDK_ROOT` — this `src` directory by default.
- `QR_FFX_VK_LIBRARY` / `QR_FFX_VK_RUNTIME_DLL` — Windows source-build outputs.
- `QR_FFX_VK_SO` — Linux source-build output.

`build_linux.sh` performs the Linux provider build and engine build in one
step. `build_win.ps1` performs the Windows provider build before configuring
the engine.
