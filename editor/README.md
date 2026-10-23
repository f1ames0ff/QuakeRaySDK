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

A picked light is authored from the panel with the fields the map entity keys
the game honours map to: `Light key` (`light`, which becomes the radiance
through the same scale the game applies), `Radius` (`rt_elight_radius`),
`Colour` (a per-light tint on that radiance) and `Position` (which re-resolves
the cluster the light contributes to). `Place at pick` is the aim-and-drop
control: it stands the light 8 units off the picked surface along the pick ray,
aims the cone down that same ray and takes the surface's cluster when the
stand-off point falls outside every leaf. `Spot` turns the cone on - the light
is then uploaded through `qrUploadSpotLight` instead of
`qrUploadSphericalLight` - and `Spot dir` (three separate fields, one per axis),
`Inner deg` and `Outer deg` shape it. All of them reach the renderer through the
per-frame upload, so a change shows in the next frame.

Global render settings are authored from the scrolling block below the sun:
`Global`, `Clouds`, `Sun disc`, `God rays`, `Volumetric fog`, `Tonemap` and
`Level fog`. `EditorSettings.cpp` derives the frame parameters from those values
exactly as `Quake/gl_vidsdl.c` does, and `EditorApp::Frame` hands the four
structs to `qrDrawFrame`, so the panel edits the same fields the game's cvars
feed.

The panel has four tabs: `Scene` (frame stats, the selection and the light
authoring above), `Entities` (the map and custom light lists), `Render` (the
global settings above) and `Materials`. The
`Scene` body scrolls, because the selection and sun blocks are taller than a
720p window once the panel is fixed at 660 px; the key hints stay pinned below
the scroll area. The light fields log their screen rectangle to `editor.log`
whenever the selection or the scroll offset changes, so a scripted session can
find them without reading pixels. Selecting or re-aiming a light also logs the
cluster list it landed in and the radiance the upload carries
(`EditorApp::LogLightCluster`), which is what tells a dark viewport apart from a
light that never reached a cluster.
`Materials` lists one material per texture name, the way the game keys
`rt_material_t`, and selects the material of a picked surface. Its parameter
block covers what the SDK upload info can express today: the texcoord transform,
the material type, roughness, metalness, base factor, exact normals, luma and
emission. The game's remaining parameters have no SDK field yet and are absent
rather than dead sliders; [`docs/editor-parity.md`](../docs/editor-parity.md)
lists them. A change is published by re-uploading the static scene once the
widget is released, so it shows in the next frame.

`Entities` holds the two light tables the game builds. The `Map` table lists the
lights the BSP spawned, the `Custom` table the ones added in the session; both
clip inside a scrolling child so a long list never pushes the selection block
out of the window. `Add light`, `Duplicate`, `Clone` and `Remove` edit the
custom store, and `Light style` in the selection block names the style the light
animates with. A standalone editor has no `d_lightstylevalue[]` feed, so the
style is authored metadata there and the preview keeps full brightness.

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
    --scene C:\path\to\e1m1.bsp `
    --tab Entities
```

`--tab` opens that tab on the first frame, so a scripted session does not have to
click the tab bar. It takes the tab label, not a pixel position, because the tab
rectangles move with the panel; the panel logs them as `panel tabs:` on the
first frame, next to the `light list rect:`, `panel rect:` and `style items:`
lines that let a script find the widgets without reading pixels.

The BSP loader currently accepts Quake BSP29 files directly. Extracting a map
from a PAK remains an adapter step.

## Next work

The remaining gap against the in-game editor is tracked in
[`docs/editor-parity.md`](../docs/editor-parity.md); that list, not this section,
is the source of truth for what is left. The global settings block, the
material parameter table and light authoring - colour, position, the spot cone
and the aim-and-drop placement - are done as far as the SDK upload info goes.
Light style, the two light lists and the custom-light store are done as well, so
the next block is the session lifecycle that writes `qray.materials.yaml` back,
then the material parameters the game has and the SDK cannot express yet; those
need new upload fields first.

PAK handling, wal textures from the game archive and sidecar persistence also
remain. The current loader uses Quake units and Z-up; `QrSpatialConvention`
already accepts that as a host convention, but the editor does not yet author in
a different one.
