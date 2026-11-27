# quakerayeditor

Standalone QuakeRay editor executable. It is a host application around
`qray::renderer`, not part of `quakeray.exe`.

## Current stage

The application creates its own platform window (Win32 on Windows, X11/Xlib on
Linux), creates a `QrInstance`, loads a Wavefront OBJ or a Quake BSP29 scene,
uploads it as a static scene, adds a directional light and renders the scene
through the SDK. The default asset is `assets/editor_scene.obj`.

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

## Units

The editor is a host, so it states what it hands the SDK in meters: the scene is
authored, picked and saved in Quake units (40 per meter, `kEditorUnitsPerMeter`
in `Scene.h`), and `EditorSpatial.{h,cpp}` converts at the one boundary every
value crosses - vertex positions, cluster bounds, light positions and radii,
area-light corners and areas, and the whole frame block down to fog density,
water transmittance, cloud height and near-DOF focus distance. Each conversion
is the one `Quake/qr_spatial_adapter.cpp` applies to the same field, so a level
lit here is lit the way the game lights it. Nothing else in the editor divides
a length by `kEditorUnitsPerMeter` on its way to the renderer. A setting the
panel authors in meters is stated in units by its builder first - the same
`METRIC_TO_QUAKEUNIT` the game wraps its metric cvars in - so the boundary
stays the only place the two spaces meet.

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
light that never reached a cluster. The gizmo logs its projected axes as
`gizmo axes:` whenever the layout moves and neither drag is running, so a
scripted drag can read the segment it has to grab from the log instead of
guessing; a mouse-down that found no axis within reach logs `gizmo miss:` with
the cursor, the origin and the distance to each axis, which is how a scripted
miss is told apart from a gizmo that never armed. A successful grab logs
`gizmo: grab custom <n> axis <a>` and the light follows with
`gizmo moved to ...`; the marker drag logs `grab:`, each step as
`drag to ... cluster <c>`, and `dropped at ...` or `drag cancelled` when the
drag ends.
`Materials` lists one material per texture name, the way the game keys
`rt_material_t`, and selects the material of a picked surface. Its parameter
block covers the game's `Textures`, `Surface` and `Luma` groups: the texcoord
transform, the three extra textures, bump scale, the material type, roughness,
metalness and its normal-alpha source, base factor, exact normals, transparency,
luma, emission and the emission blend mode. The six parameters the game folds
into a synthesised texture rather than into an upload field are baked by
`EditorMaterialBakes`, which reproduces `gl_texmgr.c`'s synthesis, so they reach
the renderer the way they do in the game. The remaining game parameters have no
SDK field at all and are absent rather than dead sliders;
[`docs/editor-parity.md`](../docs/editor-parity.md) lists them. A change is
published by re-uploading the static scene once the widget is released, so it
shows in the next frame. A row carrying a `*` is a name the session file already
owns, which is the same marker the game's list uses.

The session bar above the tabs holds `Apply`, `Cancel` and `Exit`, and the line
above it names the state: `clean`, or `unsaved` with the number of materials
touched and the level whose section was edited. `Apply` writes the edits into
the material document and the level document, `Cancel` re-reads both files into
the panel so the viewport matches what a reload would show, and `Exit` closes
the window, asking first when there are unsaved edits. The prompt answers with
`Yes` (save, then exit), `No` (exit without writing) and `Escape` or the close
box (stay). Every one of those actions reports through the toast under the
viewport, because the panel is too tall to give the status a row of its own.

The session also logs its contract, so a scripted run does not have to read
pixels: `session: <path> read, <n> entries` and
`session: <path> read, <n> lights for <level>` on startup, `session: touched
<name>` on the first edit of a material, `session: level <level> touched` when
the level section first differs from the file, `session: saved <path>` and
`session: level <level> written to <path>` on a save, `session: changes
discarded`, and `session: exit requested with unsaved changes`, `session:
prompt dismissed` around the prompt. The bar logs its three button
rectangles as `panel session: Apply …` and the prompt logs its two as
`panel prompt: Yes … No …`, both whenever the layout moves.

The writer never loses a line it did not write. It owns the full set of
panel-managed material keys (`OwnedValues` / `IsEmittedMaterialKey` in
`EditorSession.cpp`):

- **Textures & UVs:** `texture_base`, `texture_normals`, `texture_height`,
  `texture_emissive`, `texture_overlay`, `texture_gloss`, `texture_sss`,
  `texture_scale_u/v`, `texture_offset_u/v`
- **Surface, relief, glass, water & mirror:** `bump_scale`, `parallax_scale`,
  `roughness_override`, `metalness_factor`, `metalness_from_normal_alpha`,
  `base_factor`, `mirror`, `material_glass`, `material_water`, `glass_ior`,
  `glass_thickness`, `transparency`, `glass_color`, `exact_normals`,
  `force_rasterize`, `disabled_stages`
- **Emission, gloss & subsurface blocks and non-destructive toggles:**
  `enabled`, `emissive_enabled`, `gloss_enabled`, `sss_enabled`,
  `emissive_factor`, `luma_blend`, `is_light`, `luma`, `luma_threshold`,
  `luma_feather`, `gloss`, `gloss_threshold`, `gloss_feather`, `sss_strength`,
  `sss_color`, `sss_radius`, `sss`, `sss_threshold`, `sss_feather`
- **Emissive DTAL & light shafts:** `light_styles`, `light_color`,
  `light_brightness`, `light_focus`, `light_focus_soft`, `light_projector`,
  `light_min_area`, `light_shafts`, `light_shafts_power`,
  `light_shafts_distance`, `light_shafts_falloff`

It rewrites only those owned keys in the entries the session touched, dropping
any key that equals its default (while emitting explicit override markers such
as `texture_emissive: NONE` or `mirror: false` when clearing a surface's
built-in default). Comments, unrecognized keys, the entries the session never
touched and unrelated root keys are copied through verbatim at the indent they
came with. A `luma_blend` is written by name (`off`, `normal`, `screen`,
`overlay`, `hard light`, `color dodge`), the same spellings
`RT_MAT_EmissiveBlendName` writes. A file whose root still says `materials:` or
`lights:` is migrated to `qray_materials:` / `qray_lights:`, an existing target
is copied to `qray.backup_materials.yaml` first, and a missing final newline is
added before a new entry so an append cannot glue two records together. `Apply`
reloads the document afterwards, so the list markers follow the file it just
wrote.

The level half of the session writes `qray.lights.yaml`, the root-mapping file
the game reads by level name. Inside the level's own section the editor owns
`fog:` (`color:` as `rrggbb`, `density:`, `enabled:`) and `lights:`
(`origin:`, `radius:`, `intensity:`, `color:` and, for a spot, `dir:`,
`angle_inner:`, `angle_outer:`) plus `style:` when the light names one, and it
keeps every other block the section carries - `sky:`, comments, anything a hand
edit added - verbatim at the indent it had. The other levels' sections are not
touched and a level the file does not know yet is appended, so one file can hold
the whole episode.

The numbers are the file's, not the panel's, because the game reads the same
keys: `origin` is a scene-space position in editor units (40 per metre),
`radius` is metres, and a custom light's `intensity` reaches
`RT_FIXUP_LIGHT_INTENSITY` without the `/ rt_elight_normaliz` a map light takes,
so one file unit is 100 editor points. A light added with the panel defaults
therefore writes `intensity: 2` and `radius: 0.4` and reads back unchanged.
`fog.density` is written as authored - the frame divides it by 64 - and the fog
colour starts white, which is what the engine shows for a level whose section
has no `color:` key. `enabled:` follows the density, because the panel has one
slider and no second switch.

When a level carries predefined map lights (or already had a `lights:` block)
and every light is removed, the writer emits an explicit `lights: []` entry
alongside its `fog:` and `sky:` blocks (`ShouldWriteEmptyLightsBlock` in
`EditorApp.cpp` / `EditorLevelDocument::WriteMerged` in `EditorSession.cpp`)
so reloading the map preserves the empty light set instead of resurrecting
deleted predefined map lights (`Regenerate from map` restores
`m_scene.bspLights`). A level section with no `lights:` block at all leaves the
map's predefined lights intact on load.

`Entities` holds the two light tables the game builds. The `Map` table lists the
lights the BSP spawned, the `Custom` table the ones added in the session; both
clip inside a scrolling child so a long list never pushes the selection block
out of the window. `Add light`, `Duplicate`, `Clone` and `Remove` edit the
custom store, and `Light style` in the selection block names the style the light
animates with. A standalone editor has no `d_lightstylevalue[]` feed, so the
style is authored metadata there and the preview keeps full brightness.

A selected light carries a three-axis gizmo, and the light itself can be dragged
through the viewport. The two drags are deliberately different, because the game
offers both. Dragging an arrow moves the light along that axis in whole units:
the axis direction is frozen at mouse-down so the drag cannot curve away from
the axis it started on, a click within 8 px of the gizmo origin reselects
instead of grabbing, and an axis shorter than 8 px on screen is not grabbable at
all. Dragging the light marker flies it along the pick ray to 8 units in front
of the surface under the cursor, or to 128 units in front of the camera when the
ray hits nothing, mirroring `QRE_UpdateLightDrag`. Every step re-resolves the
cluster the light contributes to. `Escape` or losing focus rolls the light back
to where the drag started. The order at mouse-down is gizmo, then marker, then
surface pick, so the narrow arrows win over the larger marker beneath them.

The target is feature parity with the in-game editor (`qr_editor`); the current
state and the remaining gap are listed in
[`docs/editor-parity.md`](../docs/editor-parity.md).

## Build

### Windows

```powershell
cmake -S . -B build `
    -DQR_BUILD_EDITOR=ON `
    -DQR_NVRHI_SOURCE_DIR=C:/path/to/nvrhi `
    -DQR_EDITOR_SHADER_DIR=C:/path/to/shaders `
    -DQR_EDITOR_TEXTURE_DIR=C:/path/to/gfx/textures
cmake --build build --config Debug --target quakerayeditor
```

### Linux (X11/Xlib)

First build the FidelityFX Vulkan provider (`libamd_fidelityfx_vk.so`), then
configure and build `quakerayeditor`:

```bash
Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh
cmake -S . -B build/linux-editor -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi \
    -DQR_FFX_VK_SO="$PWD/build/ffx/libamd_fidelityfx_vk.so" \
    -DQR_EDITOR_SHADER_DIR=/path/to/shaders \
    -DQR_EDITOR_TEXTURE_DIR=/path/to/gfx/textures
cmake --build build/linux-editor --target quakerayeditor --parallel
```

The NVRHI source tree needs the pinned QuakeRay patch
`cmake/nvrhi-max-binding-layouts.patch` for the ray-tracing pipeline.
`build_win.ps1` applies it automatically; a standalone editor build must apply
it before configuring or building NVRHI.

The optional target is `-DQR_GGML_SOURCE_DIR=/path/to/ggml`, which builds the isolated
`quakeray_ai_worker` executable beside `quakerayeditor` with Vulkan Compute support
(`ggml-vulkan`).

The executable is `quakerayeditor.exe` on Windows and `quakerayeditor` on Linux.
The FSR provider (`amd_fidelityfx_vk.dll` / `libamd_fidelityfx_vk.so`), the
default OBJ, the fonts, the shader directory, `quakeray_ai_worker` (when configured)
and the blue-noise texture are copied next to it by the build when the corresponding
source directories are configured.

## Local AI PBR Texture Pipeline & Parallax Mapping

Old games ship low-resolution diffuse textures (often `64x64` or `128x128`) with
baked light and shadows, no normal maps, and no surface height or roughness data.
Instead of writing external Python batch scripts or relying on CUDA-only tools,
`quakerayeditor` includes an in-panel **Generate with AI** workflow built around
`quakeray_ai_worker` on pure Vulkan Compute (`ggml-vulkan`). The idea is straightforward:
you click the icon next to `texture_base`, `texture_normals`, `texture_height`, or
`texture_gloss` right while inspecting a wall in the level, generate updated maps
locally, tweak strength/contrast/orientation in real time, and apply them as
non-destructive override TGAs under `textures/ai_generated/`.

Everything runs locally on any Vulkan 1.2+ desktop GPU (tested on AMD Radeon
graphics cards with 16 GB VRAM, with CPU fallback when needed). Because diffusion
models can spike VRAM if kept resident alongside the ray-traced viewport,
inference runs out-of-process with stage-by-stage loading (encode -> unload ->
UNet/ControlNet -> unload -> decode) and automatic tiling capped at 75% VRAM.
Canceling frees memory immediately by terminating the worker without risking unsaved
scene edits.

Inside **AI Texture Generator**, four tabs follow the natural order for upgrading a
legacy surface:

1. **Upscale (`4x-PBRify_RPLKSRd_V3`):** Enlarges small or DXT-compressed diffuse
   textures `2x` or `4x` while cleaning block artifacts. You can inspect a
   split before/after view or detail diff, blend strength against the original,
   and pass the result straight into the next tabs (*Use Upscaled diffuse*).
2. **De-lighting (`StableDelight`):** Removes baked directional highlights and
   shadows from `texture_base` while keeping albedo detail intact, so relit
   materials do not double-shade under ray-traced lights.
3. **Normals (`DeepBump`):** Produces crisp tangent-space detail normals from
   high-frequency edges (with optional Poisson height integration for lit
   previews and `texture_normals`).
4. **Depth (`Lotus Depth v2`):** Estimates relative surface relief using a single-tile
   FlashAttention pass on a wide seamless/mirror border canvas and halo-based tilt
   removal so de-lit or low-contrast textures keep solid interior relief without
   washed-out borders. Output can be applied as `texture_height` (Relief POM
   parallax occlusion mapping), `texture_normals`, or synthesized `texture_gloss`.

Applied `texture_height` maps and the `parallax_scale` slider drive Relief Parallax
Occlusion Mapping (linear ray march + binary search + cavity micro-AO) in primary
and reflection rays by packing height into the baked normal map alpha channel when
subsurface scattering and glass transparency are inactive.

## Run

```powershell
.\quakerayeditor.exe `
    --scene C:\path\to\e1m1.bsp
```

```bash
./quakerayeditor --scene /path/to/e1m1.bsp
```

The editor defaults to `editor_scene.obj`, `shaders/` and
`textures/BlueNoise_LDR_RGBA_128.png` in the working directory or next to the
executable. The explicit options remain available for a different deployment
layout:

```powershell
.\quakerayeditor.exe `
    --shaders C:\path\to\shaders `
    --blue-noise C:\path\to\textures\BlueNoise_LDR_RGBA_128.png `
    --scene C:\path\to\e1m1.bsp `
    --materials C:\path\to\qray.materials.yaml `
    --lights C:\path\to\qray.lights.yaml `
    --tab Entities `
    --max-frames 10
```

`--materials` names the material document the session reads on start and writes
on `Apply`; without it the editor targets `qray.materials.yaml` next to the
scene. `--lights` names the level document the same way, defaulting to
`qray.lights.yaml` next to the scene, and its previous generation is copied to
`qray.backup_lights.yaml` before a write. Both paths are logged on startup -
`session: <path> read, <n> entries` and
`session: <path> read, <n> lights for <level>` - which is also how a scripted
session tells what the files already carried.

`--tab` opens that tab on the first frame, so a scripted session does not have to
click the tab bar. It takes the tab label, not a pixel position, because the tab
rectangles move with the panel; the panel logs them as `panel tabs:` on the
first frame, next to the `light list rect:`, `panel rect:` and `style items:`
lines that let a script find the widgets without reading pixels.

`--max-frames <N>` exits cleanly after rendering `N` frames, useful for
automated smoke checks.

The built-in loader reads Wavefront OBJ, Quake BSP29, and IDPO v6 `.mdl` files
directly from loose directories as well as `.pak`, `.pkz`, `.pk3`, and `.zip`
archives mounted via `EditorAssetSource` (`AddPak`, `AddZip`,
`MountEditorGameDirectory`). Engine-specific map, model, and texture formats
(such as alternate BSP versions, skeletal/morph models, or `.wal` textures)
plug in through `EditorFormatAdapter` (`QR_EDITOR_ADAPTER_SOURCES`).

## Next work

The feature parity checklist and evidence against the in-game editor live in
[`docs/editor-parity.md`](../docs/editor-parity.md); that document is the
source of truth for editor capabilities. Global settings, the complete material
authoring table (including `texture_overlay`, `texture_height`/`parallax_scale`
Relief POM, `luma`/`gloss`/`sss` colour-key/polygon/brush blocks, non-destructive
`*_enabled` toggles, `force_rasterize`, and `light_*` / `light_shafts*` DTAL
controls), light authoring (colour, position, spot cone, aim-and-drop placement,
axis gizmo, marker drag), archive mounting (`.pak`/`.pk3`/`.pkz`/`.zip`), local
Vulkan AI PBR generation, and the session lifecycle (`qray.materials.yaml`,
`qray.lights.yaml` with explicit `lights: []` empty persistence, `sky:`/`fog:`
blocks, and scope switching) are implemented.

The current loader uses Quake units and Z-up (`kEditorUnitsPerMeter = 40.0f`).
`QR_API_VERSION 1.07.0000` accepts the Z-up orientation via
`qrSetSpatialConvention`, and every spatial input reaches the SDK in meters
through one boundary: `EditorSpatial.{h,cpp}` is the editor's counterpart of
`Quake/qr_spatial_adapter.cpp`, and no other file divides a length by
`kEditorUnitsPerMeter` on its way to the renderer.
