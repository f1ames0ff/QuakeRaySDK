# quakerayeditor

Standalone QuakeRay editor executable. It is a host application around
`qray::renderer`, not part of `quakeray.exe`.

## Current stage

The application creates its own Win32 window, creates a `QrInstance`, loads a
Wavefront OBJ or a Quake BSP29 scene, uploads it as a static scene, adds a
directional light and renders the scene through the SDK. The default asset is
`assets/editor_scene.obj`.

For a BSP29 map, the loader reads the world geometry, computes per-face UVs from
the BSP texinfo data, decodes the miptex entries of the texture lump into RGBA
materials and starts the camera at `info_player_start`. Every surface is
uploaded with the world cluster its BSP leaf resolves to, and the map light
entities are resolved to their clusters and pushed through
`qrUploadClusterLightSources`, so the cluster light path the game uses is the
path the editor previews.

Clicking in the viewport picks geometry or a light. A surface pick reports the
selected triangle, its texture, its kind and its cluster; a light pick reports
the light entity. The selection is outlined in the viewport and shown in the
panel. Picking walks the uploaded surfaces rather than the BSP, which is enough
for one pick per click.

A picked light is authored from the panel with the four fields the map entity
keys the game honours map to: `Light key` (`light`, which becomes the radiance
through the same scale the game applies), `Radius` (`rt_elight_radius`),
`Colour` (a per-light tint on that radiance) and `Position` (which re-resolves
the cluster the light contributes to). All four reach the renderer through the
per-frame upload, so a change shows in the next frame.

Global render settings are authored from the scrolling block below the sun:
`Global`, `Clouds`, `Sun disc`, `God rays`, `Volumetric fog`, `Tonemap` and
`Level fog`. `EditorSettings.cpp` derives the frame parameters from those values
exactly as `Quake/gl_vidsdl.c` does, and `EditorApp::Frame` hands the four
structs to `qrDrawFrame`, so the panel edits the same fields the game's cvars
feed.

The panel has three tabs: `Scene` (frame stats, the selection and the light
authoring above), `Render` (the global settings above) and `Materials`. The
`Scene` body scrolls, because the selection and sun blocks are taller than a
720p window once the panel is fixed at 660 px; the key hints stay pinned below
the scroll area. The light fields log their screen rectangle to `editor.log`
whenever the selection or the scroll offset changes, so a scripted session can
find them without reading pixels.
`Materials` lists one material per texture name, the way the game keys
`rt_material_t`, and selects the material of a picked surface. Its parameter
block covers what the SDK upload info can express today: the texcoord transform,
the material type, roughness, metalness, base factor, exact normals, luma and
emission. The game's remaining parameters have no SDK field yet and are absent
rather than dead sliders; [`docs/editor-parity.md`](../docs/editor-parity.md)
lists them. A change is published by re-uploading the static scene once the
widget is released, so it shows in the next frame.

The target is feature parity with the in-game editor (`qr_editor`); the current
state and the remaining gap are listed in
[`docs/editor-parity.md`](../docs/editor-parity.md).

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
from a PAK remains an adapter step.

## Next work

The remaining gap against the in-game editor is tracked in
[`docs/editor-parity.md`](../docs/editor-parity.md); that list, not this section,
is the source of truth for what is left. The global settings block, the
material parameter table and light colour/position authoring are done as far as
the SDK upload info goes; the next block is the rest of light authoring (spot
cone, style, the entity list, the custom-light store), then the session
lifecycle that writes `qray.materials.yaml` back. The material parameters the
game has and the SDK cannot express yet are listed in that document and need
new upload fields first.

PAK handling, wal textures from the game archive and sidecar persistence also
remain. The current loader uses Quake units and Z-up; `QrSpatialConvention`
already accepts that as a host convention, but the editor does not yet author in
a different one.
