#!/usr/bin/env bash
#
# Build the AMD FidelityFX FSR 3.1 Vulkan provider for Linux from the
# FidelityFX SDK 1.1.4 sources that are vendored under
# renderer/Source/FidelityFX_SDK_1.1.4/src.
#
# The Windows SDK builds the provider with MSVC and FidelityFX_SC.exe. This
# script replaces the shader compiler with a Linux glslangValidator driver and
# links only the FSR 3.1 upscaler provider (frame generation is not used by the
# renderer).
#
# Requirements:
#   - a C++17 compiler
#   - glslangValidator (set GLSLANG_VALIDATOR or put it in PATH)
#   - Vulkan headers (set VULKAN_HEADERS_DIR, or VULKAN_SDK/include)
#
# Output:
#   <out-dir>/libamd_fidelityfx_vk.so
#
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk_root="${QR_FFX_SDK_ROOT:-$(cd "$script_dir/../src" && pwd)}"
out_dir="${QR_FFX_OUT_DIR:-$PWD/build/ffx}"
out_dir="$(mkdir -p "$out_dir" && cd "$out_dir" && pwd)"
build_dir="$out_dir/build"
perm_dir="$out_dir/permutations"

cc="${CC:-gcc}"
cxx="${CXX:-g++}"
ar="${AR:-ar}"

if [[ ! -f "$sdk_root/sdk/src/backends/vk/ffx_vk.cpp" ]]; then
    echo "error: FidelityFX SDK 1.1.4 sources were not found under $sdk_root" >&2
    echo "       set QR_FFX_SDK_ROOT to a directory containing sdk/ and ffx-api/" >&2
    exit 1
fi

if [[ -z "${GLSLANG_VALIDATOR:-}" ]]; then
    if command -v glslangValidator >/dev/null 2>&1; then
        GLSLANG_VALIDATOR="$(command -v glslangValidator)"
    elif [[ -n "${VULKAN_SDK:-}" && -x "$VULKAN_SDK/bin/glslangValidator" ]]; then
        GLSLANG_VALIDATOR="$VULKAN_SDK/bin/glslangValidator"
    else
        echo "error: glslangValidator was not found; set GLSLANG_VALIDATOR" >&2
        exit 1
    fi
fi

if [[ -z "${VULKAN_HEADERS_DIR:-}" ]]; then
    if [[ -n "${VULKAN_SDK:-}" && -f "$VULKAN_SDK/include/vulkan/vulkan.h" ]]; then
        VULKAN_HEADERS_DIR="$VULKAN_SDK/include"
    elif [[ -f /usr/include/vulkan/vulkan.h ]]; then
        VULKAN_HEADERS_DIR=/usr/include
    else
        echo "error: Vulkan headers were not found; set VULKAN_HEADERS_DIR" >&2
        exit 1
    fi
fi

echo "FidelityFX SDK : $sdk_root"
echo "Output         : $out_dir"
echo "glslang        : $GLSLANG_VALIDATOR"
echo "Vulkan headers : $VULKAN_HEADERS_DIR"

mkdir -p "$build_dir" "$perm_dir"

# ---------------------------------------------------------------------------
# 1. Build the Linux permutation generator.
# ---------------------------------------------------------------------------
"$cc" -c -O2 -I"$script_dir/shader_compiler/spirv-reflect" \
    "$script_dir/shader_compiler/spirv-reflect/spirv_reflect.c" \
    -o "$build_dir/spirv_reflect.o"

"$cxx" -std=c++17 -O2 \
    -I"$script_dir/shader_compiler" \
    -I"$script_dir/shader_compiler/md5" \
    -I"$script_dir/shader_compiler/spirv-reflect" \
    -I"$script_dir/shader_compiler/tiny-process" \
    "$script_dir/shader_compiler/glsl_compiler.cpp" \
    "$script_dir/shader_compiler/driver.cpp" \
    "$script_dir/shader_compiler/tiny-process/process.cpp" \
    "$script_dir/shader_compiler/tiny-process/process_unix.cpp" \
    "$script_dir/shader_compiler/md5/md5.cpp" \
    "$build_dir/spirv_reflect.o" \
    -o "$build_dir/fsr-sc" -ldl -pthread

# ---------------------------------------------------------------------------
# 2. Generate the 40 permutation headers (10 passes x 4 variants).
# ---------------------------------------------------------------------------
passes=(
    ffx_fsr3upscaler_accumulate_pass
    ffx_fsr3upscaler_autogen_reactive_pass
    ffx_fsr3upscaler_debug_view_pass
    ffx_fsr3upscaler_luma_instability_pass
    ffx_fsr3upscaler_luma_pyramid_pass
    ffx_fsr3upscaler_prepare_inputs_pass
    ffx_fsr3upscaler_prepare_reactivity_pass
    ffx_fsr3upscaler_rcas_pass
    ffx_fsr3upscaler_shading_change_pass
    ffx_fsr3upscaler_shading_change_pyramid_pass
)
variants=("" "_wave64" "_16bit" "_wave64_16bit")

shader_dir="$sdk_root/sdk/src/backends/vk/shaders/fsr3upscaler"
gpu_include="$sdk_root/sdk/include/FidelityFX/gpu"

shader_stamp="$perm_dir/.stamp"
needs_shaders=1
if [[ "${QR_FFX_FORCE_SHADERS:-0}" != "1" ]] && [[ -f "$shader_stamp" ]] && [[ -z "$(find "$shader_dir" "$gpu_include" -newer "$shader_stamp" -print -quit)" ]]; then
    if [[ "$(find "$perm_dir" -maxdepth 1 -name '*_permutations.h' | wc -l)" -eq 40 ]]; then
        needs_shaders=0
    fi
fi

if [[ "$needs_shaders" -eq 1 ]]; then
    rm -f "$perm_dir"/*_permutations.h "$perm_dir"/*.h
    mkdir -p "$build_dir/gen"
    for pass in "${passes[@]}"; do
        for variant in "${variants[@]}"; do
            name="${pass}${variant}"
            half=0
            [[ "$variant" == *16bit* ]] && half=1
            temp="$build_dir/gen/$name"
            rm -rf "$temp"
            mkdir -p "$temp"
            "$build_dir/fsr-sc" "$GLSLANG_VALIDATOR" "$shader_dir/$pass.glsl" \
                "$name" "$temp" "$half" "$gpu_include" >/dev/null
            cp -f "$temp"/*.h "$perm_dir/"
        done
    done
    touch "$shader_stamp"
else
    echo "shader permutations are up to date"
fi

# ---------------------------------------------------------------------------
# 3. Build the FSR 3.1-only Vulkan provider.
# ---------------------------------------------------------------------------
provider_cxxflags=(${FSR_PROVIDER_CXXFLAGS:--O2 -g})
echo "provider flags : ${provider_cxxflags[*]}"
rm -f "$out_dir/libamd_fidelityfx_vk.so"
"$cxx" -std=c++17 -fPIC -shared -Wl,-soname,libamd_fidelityfx_vk.so "${provider_cxxflags[@]}" \
    -include "$script_dir/compat.h" \
    -DFFX_BACKEND_VK -DFFX_API_VK -DFFX_FSR3 -DFFX_FSR3UPSCALER \
    -DFFXDLL_VERSION_API=VK -DFFXDLL_VERSION_MAJOR=1 -DFFXDLL_VERSION_MINOR=0 -DFFXDLL_VERSION_PATCH=1 \
    -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR \
    -I"$sdk_root/sdk/include" \
    -I"$sdk_root/sdk/src/backends/vk" \
    -I"$sdk_root/sdk/src/backends/shared" \
    -I"$sdk_root/sdk/src/shared" \
    -I"$sdk_root/sdk/src/components" \
    -I"$sdk_root/sdk/src/components/fsr3upscaler" \
    -I"$sdk_root/ffx-api/include" \
    -I"$sdk_root/ffx-api/src" \
    -I"$perm_dir" \
    -I"$VULKAN_HEADERS_DIR" \
    "$sdk_root/sdk/src/shared/ffx_assert.cpp" \
    "$sdk_root/sdk/src/shared/ffx_message.cpp" \
    "$sdk_root/sdk/src/shared/ffx_breadcrumbs_list.cpp" \
    "$sdk_root/sdk/src/shared/ffx_object_management.cpp" \
    "$sdk_root/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp" \
    "$sdk_root/sdk/src/backends/vk/ffx_vk.cpp" \
    "$sdk_root/sdk/src/backends/shared/ffx_shader_blobs.cpp" \
    "$sdk_root/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp" \
    "$sdk_root/ffx-api/src/ffx_api.cpp" \
    "$sdk_root/ffx-api/src/backends.cpp" \
    "$sdk_root/ffx-api/src/validation.cpp" \
    "$sdk_root/ffx-api/src/ffx_provider_fsr3upscale.cpp" \
    "$script_dir/ffx_provider.cpp" \
    "$script_dir/stub_fi_vk.cpp" \
    -o "$out_dir/libamd_fidelityfx_vk.so" -ldl -pthread -lm

echo "built $out_dir/libamd_fidelityfx_vk.so"
