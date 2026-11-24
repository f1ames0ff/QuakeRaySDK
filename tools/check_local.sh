#!/usr/bin/env bash
# Fast local verification script for QuakeRaySDK (build + CPU tests + quality gate).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if [[ -f /home/user/Soft/qray-env.sh ]]; then
    # shellcheck disable=SC1091
    . /home/user/Soft/qray-env.sh
fi

usage() {
    cat <<'EOF'
Usage: tools/check_local.sh [options]

Options:
  --local        Check uncommitted workspace changes against HEAD
  --base <ref>   Check git diff <ref>...HEAD (default: origin/main)
  --asan         Build with Clang ASan + UBSan into build/local-quality-asan
  --shaders      Run CheckMatrixReads.py and GenerateShaders.py -rebuild
  --no-build     Skip CMake configure/build and ctest
  --jobs <N>     Max parallel compile jobs (default: QR_JOBS or 6)
  -h, --help     Show this help message
EOF
}

mode=""
base_ref="origin/main"
use_asan=0
run_shaders=0
no_build=0
jobs="${QR_JOBS:-6}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local)
            mode="local"
            shift
            ;;
        --base)
            if [[ $# -lt 2 ]]; then
                echo "error: --base requires a git ref argument" >&2
                exit 2
            fi
            mode="base"
            base_ref="$2"
            shift 2
            ;;
        --asan)
            use_asan=1
            shift
            ;;
        --shaders)
            run_shaders=1
            shift
            ;;
        --no-build)
            no_build=1
            shift
            ;;
        --jobs)
            if [[ $# -lt 2 ]]; then
                echo "error: --jobs requires a number" >&2
                exit 2
            fi
            jobs="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$mode" ]]; then
    if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
        mode="local"
    else
        mode="base"
    fi
fi

start_ts="$(date +%s)"

if [[ "$use_asan" -eq 1 ]]; then
    build_dir="$repo_root/build/local-quality-asan"
else
    build_dir="$repo_root/build/local-quality"
fi

if [[ "$no_build" -eq 0 ]]; then
    nvrhi_dir="${QR_NVRHI_SOURCE_DIR:-$repo_root/../third_party/nvrhi}"
    nvrhi_patch="$repo_root/cmake/nvrhi-max-binding-layouts.patch"
    nvrhi_patched_here=0
    cleanup_nvrhi() {
        if [[ "$nvrhi_patched_here" == "1" ]]; then
            git -C "$nvrhi_dir" apply --reverse "$nvrhi_patch" >/dev/null 2>&1 || true
        fi
    }
    trap cleanup_nvrhi EXIT

    if [[ ! -f "$nvrhi_dir/CMakeLists.txt" ]]; then
        echo "error: NVRHI source directory was not found at $nvrhi_dir" >&2
        echo "       set QR_NVRHI_SOURCE_DIR or pass --no-build for static checks only." >&2
        exit 1
    fi

    if [[ -f "$nvrhi_patch" ]] && ! grep -qE 'c_MaxBindingLayouts[[:space:]]*=[[:space:]]*(1[3-9]|[2-9][0-9])' "$nvrhi_dir/include/nvrhi/nvrhi.h"; then
        git -C "$nvrhi_dir" apply "$nvrhi_patch"
        nvrhi_patched_here=1
    fi

    mkdir -p "$build_dir/ffx"
    QR_FFX_OUT_DIR="$build_dir/ffx" bash "$repo_root/Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh"

    cc_bin="${CC:-$(command -v clang || command -v gcc)}"
    cxx_bin="${CXX:-$(command -v clang++ || command -v g++)}"

    cmake_args=(
        -S "$repo_root"
        -B "$build_dir"
        -G Ninja
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DCMAKE_C_COMPILER="$cc_bin"
        -DCMAKE_CXX_COMPILER="$cxx_bin"
        -DQR_NVRHI_SOURCE_DIR="$nvrhi_dir"
        -DQR_FFX_VK_SO="$build_dir/ffx/libamd_fidelityfx_vk.so"
        -DQR_BUILD_TESTS=ON
    )
    if [[ -n "${VULKAN_HEADERS_DIR:-}" ]]; then
        cmake_args+=(-DVulkan_INCLUDE_DIR="$VULKAN_HEADERS_DIR")
    fi
    if [[ -n "${QR_DXC:-}" ]]; then
        cmake_args+=(-DQR_DXC="$QR_DXC")
    fi
    if [[ "$use_asan" -eq 1 ]]; then
        cmake_args+=(-DQR_ENABLE_SANITIZERS=ON)
    else
        cmake_args+=(-DQR_ENABLE_SANITIZERS=OFF)
    fi
    if command -v ccache >/dev/null 2>&1 && [[ "${QR_NO_CCACHE:-0}" != "1" ]]; then
        cmake_args+=(
            -DCMAKE_C_COMPILER_LAUNCHER=ccache
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
        )
    fi

    cmake "${cmake_args[@]}"
    cmake --build "$build_dir" --parallel "$jobs" --target \
        qray_host_contract_tests \
        qray_gpu_context_tests \
        qray_readback_api_tests \
        qray_swapchain_policy \
        qray_sampler_filter_tests \
        qray_spatial_contract_tests \
        qray_subsurface_tests \
        qray_worldlights_tests \
        qray_light_shaft_tests

    if [[ "$use_asan" -eq 1 ]]; then
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        ctest --test-dir "$build_dir" --output-on-failure -LE gpu
    else
        ctest --test-dir "$build_dir" --output-on-failure -LE gpu
    fi
fi

python3 -B -m unittest discover -s "$repo_root/tools" -p "test_*.py" -v

if [[ "$run_shaders" -eq 0 ]]; then
    if [[ "$mode" == "local" ]]; then
        changed_files="$(git diff --name-only HEAD; git ls-files --others --exclude-standard)"
    else
        changed_files="$(git diff --name-only "${base_ref}...HEAD")"
    fi
    if printf '%s\n' "$changed_files" | grep -qE '^Source/Shaders/.*\.(hlsl|hlsli)$'; then
        run_shaders=1
    fi
fi

quality_args=(--strict-tools)
if [[ "$mode" == "local" ]]; then
    quality_args+=(--local)
else
    quality_args+=(--base "$base_ref")
fi

compile_db=""
for candidate in \
    "$build_dir/compile_commands.json" \
    "$repo_root/build/local-quality/compile_commands.json" \
    "$repo_root/build/quality/compile_commands.json" \
    "$repo_root/build/linux-debug/compile_commands.json"; do
    if [[ -f "$candidate" ]]; then
        compile_db="$candidate"
        break
    fi
done
if [[ -n "$compile_db" ]]; then
    quality_args+=(--compile-commands "$compile_db")
fi
if [[ "$run_shaders" -eq 1 ]]; then
    quality_args+=(--shaders)
fi

python3 -B "$repo_root/tools/check_quality.py" "${quality_args[@]}"

if command -v ccache >/dev/null 2>&1; then
    ccache -s
fi

elapsed=$(( $(date +%s) - start_ts ))
echo "Local QuakeRaySDK checks passed in ${elapsed}s"
