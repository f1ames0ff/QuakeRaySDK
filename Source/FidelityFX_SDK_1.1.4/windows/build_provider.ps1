# Build the AMD FidelityFX FSR 3.1 Vulkan provider from the FidelityFX SDK
# 1.1.4 sources on Windows. This is the Windows counterpart of
# ../linux/build_provider.sh: the engine links this output instead of a
# checked-in provider binary.
#
# The SDK's own build system is used, including its bundled FidelityFX_SC.exe
# shader compiler. The sources are copied to the output directory first, and
# the local FidelityFX 1.1.4 patches are applied there only when a marker is
# missing, so an already-patched vendored tree is left untouched.
param(
    [string]$SdkRoot = "",
    [string]$OutDir = "",
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SdkRoot) {
    $SdkRoot = (Resolve-Path (Join-Path $scriptDir "..\src")).Path
}
if (-not $OutDir) {
    $OutDir = Join-Path (Get-Location) "build\ffx"
}
$SdkRoot = (Resolve-Path $SdkRoot).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

if (-not (Test-Path (Join-Path $SdkRoot "sdk\CMakeLists.txt")) -or
    -not (Test-Path (Join-Path $SdkRoot "ffx-api\CMakeLists.txt"))) {
    throw "FidelityFX SDK 1.1.4 sources were not found under $SdkRoot (expected sdk\ and ffx-api\)."
}

# build_win.ps1 already enters the Visual Studio environment. Make the script
# usable on its own as well.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe was not found. Install Visual Studio Build Tools with the C++ workload."
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "Visual Studio Build Tools with the C++ workload are not installed."
    }
    $devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
    $envLines = & $env:ComSpec /c "`"$devCmd`" -arch=x64 -host_arch=x64 >nul 2>&1 && set"
    foreach ($line in $envLines) {
        if ($line -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

$src = Join-Path $OutDir "src"
if (Test-Path $src) {
    Remove-Item -Recurse -Force $src
}
New-Item -ItemType Directory -Force -Path $src | Out-Null
Copy-Item -Recurse -Force (Join-Path $SdkRoot "sdk") $src
Copy-Item -Recurse -Force (Join-Path $SdkRoot "ffx-api") $src

# The vendored tree is already patched; external SDK checkouts get the same
# changes here. Each patch is skipped when its marker is already present.
$patchChecks = @(
    @{ File = "sdk\include\FidelityFX\gpu\fsr3upscaler\ffx_fsr3upscaler_callbacks_glsl.h";
       Pattern = "rgba16f\) uniform image2D\s+rw_luma_history";
       Patch = "0001-fsr3-luma-history-rgba16f.patch" },
    @{ File = "sdk\include\FidelityFX\host\ffx_fsr3upscaler.h";
       Pattern = "FFX_SDK_DEFAULT_CONTEXT_SIZE \* 4";
       Patch = "0002-fsr3-context-size-linux-wchar.patch" },
    @{ File = "sdk\src\backends\vk\ffx_vk.cpp";
       Pattern = "scratchAlignment";
       Patch = "0003-vk-scratch-alignment.patch" },
    @{ File = "ffx-api\include\ffx_api\ffx_api.h";
       Pattern = "#if defined\(_WIN32\)";
       Patch = "0004-ffx-api-entry-linux.patch" },
    @{ File = "ffx-api\src\ffx_provider_fsr3upscale.cpp";
       Pattern = "0xF5A5CA1Eull";
       Patch = "0005-ffx-provider-msvc-literal.patch" }
)

foreach ($check in $patchChecks) {
    $file = Join-Path $src $check.File
    if (-not (Test-Path $file)) {
        throw "The SDK source is missing $($check.File)."
    }
    if (-not (Select-String -Path $file -Pattern $check.Pattern -Quiet)) {
        $patch = Join-Path $scriptDir "..\patches\$($check.Patch)"
        if (-not (Test-Path $patch)) {
            throw "Missing FidelityFX patch $patch."
        }
        Push-Location $src
        try {
            & git apply $patch
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to apply $patch."
            }
        }
        finally {
            Pop-Location
        }
    }
}

$build = Join-Path $OutDir "build"
if (Test-Path $build) {
    Remove-Item -Recurse -Force $build
}
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutDir "amd_fidelityfx_vk.dll")
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $OutDir "amd_fidelityfx_vk.lib")

Write-Host "Configuring FidelityFX FSR 3.1 Vulkan provider..." -ForegroundColor Yellow
cmake -S (Join-Path $src "ffx-api") -B $build -G $Generator -A x64 `
    -DFFX_API_BACKEND=VK_X64 `
    -DFFX_AUTO_COMPILE_SHADERS=ON
if ($LASTEXITCODE -ne 0) {
    throw "FidelityFX provider configure failed."
}

Write-Host "Building FidelityFX FSR 3.1 Vulkan provider..." -ForegroundColor Yellow
cmake --build $build --config $Config --target amd_fidelityfx_vk --parallel
if ($LASTEXITCODE -ne 0) {
    throw "FidelityFX provider build failed."
}

$bin = Join-Path $src "ffx-api\bin"
# CMAKE_DEBUG_POSTFIX adds a "d" suffix for Debug builds; the engine always
# consumes the canonical amd_fidelityfx_vk.* names.
$dll = Get-ChildItem -Path $bin -Recurse -Filter "amd_fidelityfx_vk*.dll" |
    Sort-Object { $_.Name -eq "amd_fidelityfx_vk.dll" } -Descending |
    Select-Object -First 1
$lib = Get-ChildItem -Path $bin -Recurse -Filter "amd_fidelityfx_vk*.lib" |
    Sort-Object { $_.Name -eq "amd_fidelityfx_vk.lib" } -Descending |
    Select-Object -First 1
if (-not $dll -or -not $lib) {
    throw "The FidelityFX provider build completed but amd_fidelityfx_vk.dll/.lib were not found under $bin."
}

Copy-Item -Force $dll.FullName (Join-Path $OutDir "amd_fidelityfx_vk.dll")
Copy-Item -Force $lib.FullName (Join-Path $OutDir "amd_fidelityfx_vk.lib")

Write-Host "Built $OutDir\amd_fidelityfx_vk.dll" -ForegroundColor Green
Write-Host "Built $OutDir\amd_fidelityfx_vk.lib" -ForegroundColor Green
