# quakerayeditor

Standalone QuakeRay editor executable. It is a host application around
`qray::renderer`, not part of `quakeray.exe`.

## Current stage

The application creates its own Win32 window, creates a `QrInstance`, loads a
Wavefront OBJ scene, uploads it as a static scene, adds a directional light and
renders the scene through the SDK. The default asset is
`assets/editor_scene.obj`.

## Build

```powershell
cmake -S . -B build -DQR_BUILD_EDITOR=ON -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi
cmake --build build --config Debug --target quakerayeditor
```

The NVRHI source tree needs the pinned QuakeRay patch
`third_party/nvrhi-max-binding-layouts.patch` for the ray-tracing pipeline.
`build_win.ps1` applies it automatically; a standalone editor build must apply
it before configuring or building NVRHI.

The executable is `quakerayeditor.exe`. The FSR provider DLL and the default OBJ
are copied next to it by the build.

## Run

The editor needs the compiled shader folder and the blue-noise texture. The
QuakeRay build produces the shaders in `build/Debug/qray_stage/shaders` before
packing; point the editor at any directory containing the `.spv` files.

```powershell
.\quakerayeditor.exe `
    --shaders C:\path\to\shaders `
    --blue-noise C:\path\to\textures\BlueNoise_LDR_RGBA_128.png `
    --scene C:\path\to\editor_scene.obj
```

Without `--scene`, `editor_scene.obj` next to the executable is loaded.

## Next work

The editor still uses the temporary OBJ scene path in Quake-like Z-up units.
The Quake BSP adapter, editor core, selection, material/light authoring and
sidecar persistence are the next extraction steps.
