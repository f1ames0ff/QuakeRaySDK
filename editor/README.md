# quakerayeditor

Standalone QuakeRay editor executable. It is a host application around
`qray::renderer`, not part of `quakeray.exe`.

## Current stage

The application creates its own Win32 window, creates a `QrInstance`, loads a
Wavefront OBJ or a Quake BSP29 scene, uploads it as a static scene, adds a
directional light and renders the scene through the SDK. The default asset is
`assets/editor_scene.obj`.

For a BSP29 map, the loader reads the world geometry, computes per-face UVs from
the BSP texinfo data and starts the camera at `info_player_start`. Texture
loading, entity presentation and editor-side material authoring are not part of
this stage; the viewport uses a checker material so geometry and UVs are visible.

## Build

```powershell
cmake -S . -B build `
    -DQR_BUILD_EDITOR=ON `
    -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi `
    -DQR_EDITOR_SHADER_DIR=C:/path/to/shaders `
    -DQR_EDITOR_TEXTURE_DIR=C:/path/to/gfx/textures
cmake --build build --config Debug --target quakerayeditor
```

The NVRHI source tree needs the pinned QuakeRay patch
`third_party/nvrhi-max-binding-layouts.patch` for the ray-tracing pipeline.
`build_win.ps1` applies it automatically; a standalone editor build must apply
it before configuring or building NVRHI.

The executable is `quakerayeditor.exe`. The FSR provider DLL, the default OBJ,
the shader directory and the blue-noise texture are copied next to it by the
build when the corresponding source directories are configured.

## Run

```powershell
.\quakerayeditor.exe `
    --scene C:\path\to\e1m1.bsp
```

The editor defaults to `editor_scene.obj`, `shaders/` and
`textures/BlueNoise_LDR_RGBA_128.png` next to the executable. The explicit
options remain available for a different deployment layout:

```powershell
.\quakerayeditor.exe `
    --shaders C:\path\to\shaders `
    --blue-noise C:\path\to\textures\BlueNoise_LDR_RGBA_128.png `
    --scene C:\path\to\e1m1.bsp
```

The BSP loader currently accepts Quake BSP29 files directly. Extracting a map
from a PAK and texture loading are the next adapter steps.

## Next work

The editor core, selection, material/light authoring, PAK handling, Quake
texture loading and sidecar persistence remain. The long-term spatial contract
still needs an explicit world-units and up-axis decision; the current loader
uses Quake units and Z-up.
