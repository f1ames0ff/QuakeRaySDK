# Editor parity with `qr_editor`

The standalone editor (`renderer/editor/`) is meant to reach feature parity with the in-game light
and material editor (`Quake/qr_editor.c` + `Quake/qr_gui.cpp`). This document is the checklist: what
exists, what is missing, and the evidence for each row.

It is a living document. When a row moves, update the table in the same commit that changes the
code, and cite the function that implements it.

## Reference: what `qr_editor` is

`qr_editor` is a four-tab authoring panel driven by console commands (`qr_editor`,
`qr_editor_stop`) and a free camera. It edits live renderer state and, on Apply, writes the result
back to `qray.materials.yaml`. Its surface is:

| Area | Content | Evidence |
| --- | --- | --- |
| Materials tab | 32 parameters per animation frame of the picked texture, across the `Textures`, `Surface`, `Model` and `Light` sections | `qre_params[PARAM_COUNT]` in `Quake/qr_editor.c` |
| Entities tab | every light the frame generates (material light, legacy dlight, map light), field by field | `QR_GUI_ResetButton ("light_intensity", …)` block in `Quake/qr_editor.c` |
| Custom tab | lights the level does not have: add, duplicate, clone, remove, place, drag, axis gizmo | `QR_GUI_Button ("Add light")` and the `QRE_CustomFieldChanged` block |
| Global tab | 39 settings across the `Global`, `Sky`, `Clouds`, `Sun`, `God rays`, `Volumetric fog` and `Tonemap` groups, plus fog and the level/global scope switch | the `QRE_G_*` row table in `Quake/qr_editor.c` |
| Shared | free camera, crosshair pick, hint overlay, notify toast, modal dialogs, texture browser with previews | `QR_GUI_*` in `Quake/qr_gui.h` |

## Current standalone editor

`EditorApp::DrawPanel` draws one window with these rows: frame/stat readout, scene counts,
cluster-light stats, bounds, camera readout and fly speed, one line of session state, then the
`Apply` / `Cancel` / `Exit` row, then a selection block
that is either a surface (index, triangle, texture name, kind, cluster) or a light (position,
radiance, cluster, `Light key`, `Radius`, `Colour`, `Position`), then `At`, `Clear selection`, a
`Sun` block (intensity, azimuth, elevation, angular size), the scrolling global-settings block
(`Global`, `Clouds`, `Sun disc`, `God rays`, `Volumetric fog`, `Tonemap`, `Level fog`) and the
key hints. The `Scene` tab body itself scrolls: the light and sun blocks are taller than a 720p
window once the panel is fixed at 660 px, and the key hints stay pinned below the scroll area.

## Gap matrix

Legend: **done** ships today, **partial** exists but is narrower than `qr_editor`, **missing** has
no implementation.

| Feature | State | Notes |
| --- | --- | --- |
| Load a BSP map with per-surface textures | done | `LoadEditorSceneBsp` |
| Free-fly camera and fly speed | done | `UpdateCamera` |
| Pick a surface under the cursor | done | `PickSurface` |
| Pick a light under the cursor | done | `PickLight` |
| Selection overlay | done | `DrawSelectionOverlay` |
| Map light `light` key authoring | done | `SetEditorLightIntensity` via `kEditorLightIntensityScale` |
| Map light radius authoring | done | `Radius` slider, reaches `qrUploadSphericalLight` every frame |
| Sun intensity, azimuth, elevation, angular size | done | `UploadSun` → `qrUploadDirectionalLight` |
| Surface material read-out | done | texture name and pass-through kind; a surface pick also selects its material in the `Materials` tab |
| Material parameters (32 of them, per animation frame) | partial | `EditorMaterial.{h,cpp}`; 13 of the game's parameters, one material per texture name, applied by re-uploading the static scene. Unsupported fields are listed under [Materials](#materials) |
| Texture browser with previews and search | missing | `QR_GUI_TextureBrowser` equivalent |
| Texture UV scale/offset authoring | partial | `Scale U/V` and `Offset U/V` feed the layer-0 texcoord transform of the upload (`QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT`); `qrUpdateGeometryTexCoords` is not used because `VertexCollector::UpdateTexCoords` still asserts |
| Luma mask authoring and preview | partial | `Luma` sets `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` and `Emissive` sets the emission scale; there is no mask texture and no preview |
| Glass and water pass-through parameters | partial | `PassThroughTypeForSurface` picks the type; the parameters are not editable |
| Light colour authoring | done | `Colour` (`ColorEdit3`) writes `EditorLight::tint`; `SetEditorLightTint` rebuilds radiance as `intensity * kEditorLightIntensityScale * tint` |
| Light position and offset authoring | done | `Position` (`DragFloat3`) moves the light and re-resolves its cluster through `ResolveEditorLightCluster`; `Place at pick` drops it 8 units in front of the picked surface along the pick ray and aims the cone down that ray |
| Light spot direction and inner/outer angles | done | `Spot` turns the cone on, `Spot dir` is three separate `##dirx/##diry/##dirz` fields, `Inner deg`/`Outer deg` the falloff, all through `qrUploadSpotLight`. Measured on `e1m1` at radius 300: spot off→on changes 52820 viewport pixels (mean 3.77) against a 1360/1.01 noise floor, direction mean 138.41, outer 0→60° mean 77.19, inner 0→40° mean 43.20 |
| Light style authoring | done | `Light style` combo over the 12 `rt_custom_style_names`; a map light also offers `none`, a custom light does not, the way the game's Custom tab does not. `EditorLight::style` is authored metadata: a standalone editor has no `d_lightstylevalue[]` feed, so the preview keeps full brightness and the style names the animation the light gets once saved. Selecting style 3 logs `custom 0: style 3 candle` |
| Light list for the Entity tab | done | `DrawLightList`: a `Map` and a `Custom` table, both inside a clipped `BeginChild` so the lists scroll instead of pushing the selection block out of the window. Each row shows key, intensity, radius and style; a row click selects the light the selection block below edits |
| Add / duplicate / clone / remove a custom light | done | `Add light`, `Duplicate`, `Clone`, `Remove` over an editor-side `EditorCustomLight` store. Duplicate copies the light and offsets it by 32 units on Z; Clone reproduces it at the picked surface; Remove drops the newest. Measured on `e1m1`: add changes 9196 viewport pixels (mean 2.00), duplicate 20264 (3.76), clone 6638 (1.24), remove 19565 (2.63) against a 1360/1.01 noise floor |
| Placement mode (aim and drop a light) | done | `Place at pick` on a surface pick; the light lands 8 units off the surface its cluster comes from |
| Light drag and axis gizmo | done | Two mechanics, both mirroring `QRE_CustomGizmoBegin`/`QRE_CustomGizmoMove` and `QRE_UpdateLightDrag`. Dragging an arrow moves the light along that axis in whole units: the axis direction is frozen at mouse-down so a straight drag cannot curve, the origin is a dead zone so a click near it reselects instead of grabbing, and axes shorter than 8 px on screen are not grabbable. Dragging the light itself flies it along the pick ray to 8 units in front of the surface, or to 128 units when the ray misses all geometry, and re-resolves the cluster on every step. Escape or focus loss rolls the light back to where the drag started. Measured on `e1m1`: an axis drag of +60 px moves custom light 0 from `480 -256 110` to `490 -256 110` (cluster 1133, 4656 changed viewport pixels, mean 3.73), a grab drag from `490 -256 110` to `550 -103 135` across clusters 1120/1129/1130 changes 183261 pixels (mean 25.11) against a 1360/1.01 noise floor |
| Global settings table (39 rows) | done | `EditorSettings.cpp` `BuildEditorFrameParams` fills `QrDrawFrameSkyParams`, `QrDrawFrameTonemappingParams`, `QrDrawFrameVolumetricParams` and `QrDrawFrameLevelFogParams`; `DrawGlobalSettings` draws seven groups |
| Fog authoring | done | `Level fog` group: colour, density (divided by 64 like the worldspawn key), sky blend |
| Cloud authoring | done | `Clouds` group, packed into `skyCubemapRotationTransform` per `QrDrawFrameSkyParams` |
| God rays authoring | done | `God rays` group, gated on the procedural sky like the game |
| Tonemap authoring | done | `Tonemap` group including the LPM percentiles the game hard-codes |
| Volumetric fog authoring | done | `Volumetric fog` group; the scattering source follows the authored sun |
| Level/global scope switch | partial | the level scope is authored (`fog:` and `lights:` in `qray.lights.yaml`, see [Level document](#level-document)); the switch in the game's Global tab selects the scope of the sky, sun and cloud settings, and those are not written yet |
| Apply / Cancel / Exit with a dirty prompt | done | `DrawSessionBar`, `ApplySession`, `DiscardSession`, `RequestExit` and `DrawSessionPrompt`. `WM_CLOSE` goes through the same prompt; `Yes` saves and exits, `No` exits without writing, `Escape` or the close box dismisses the prompt and keeps the session |
| Save back to `qray.materials.yaml` | done | `EditorMaterialsDocument::WriteMerged` in `EditorSession.cpp`; `--materials <file>` picks the target, and the previous generation is copied to `qray.backup_materials.yaml` before a write |
| Notify toast | done | `DrawNotification` under the viewport: `saved …`, `changes discarded` and `nothing to save`, held six seconds and faded over the last one |
| Modal dialogs | partial | the save-on-exit prompt (`DrawSessionPrompt`) is the only one; the game's other popups have no counterpart |
| Crosshair and hint overlay | partial | the hints are panel text, not an overlay |

## Materials

`EditorMaterialTable` keys one `EditorMaterialParams` by texture name, so two surfaces that share a
texture share a material, the way the game keys `rt_material_t`. `Rebuild` runs after the scene
loads; `For(surface)` returns the material of a surface and the defaults for a texture the table
does not know. A surface pick calls `IndexOf(picked.textureName)` and moves the `Materials` tab to
that material, so the panel edits what was clicked.

The tab is the third of `Scene`, `Render`, `Materials`. The list of material names scrolls; the
parameter block below it is a second scrolling child with the `Textures`, `Surface`, `Model` and
`Luma` headers the game uses.

| Game parameter | Editor field | Reaches the renderer as |
| --- | --- | --- |
| `texture_scale_u/v`, `texture_offset_u/v` | `Scale U/V`, `Offset U/V` | layer-0 texcoord transform, with `QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT` asserted only when the transform is not identity |
| `material` | type combo `none`/`glass`/`water` | `QrGeometryPassThroughType`; `none` keeps the kind the BSP loader derived |
| `roughness_override` | `Roughness override` | `defaultRoughness`; the checkbox writes `kEditorDefaultRoughness`, clearing it returns the `-1` sentinel |
| `metalness_factor` | `Metalness` | `defaultMetallicity` |
| `base_factor` | `Base factor` | `layerColors[0]` as a grey albedo tint |
| `exact_normals` | `Exact normals` | `QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT` |
| `luma` | `Luma` | `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` |
| `default_emission` | `Emissive` | `defaultEmission` |

`ApplyEditorMaterial` writes those fields into a `QrGeometryUploadInfo`. They are read at upload
time, so an edit is published by rebuilding the static scene: `EditorApp::Frame` watches
`m_materialsChanged`, waits until no widget is active, and calls `UploadStaticGeometry` once per
change (`materials applied: static scene re-uploaded` in `editor.log`).

Deliberately absent, because the SDK upload info has no field for them and a slider that cannot
change the picture would be a lie: `bump_scale`, `metalness_from_normal_alpha`, `mirror`,
`transparency`, `glass_ior`, `glass_thickness`, `glass_color`, `force_rasterize`, the `light_*`
group and `luma_blend`. The game's emissive range is 0..5; the SDK clamps emission to [0, 1].

## Session

`--materials <file>` names the document the session reads on start and writes on Apply; without
it the editor targets `qray.materials.yaml` next to the scene. `EditorMaterialsDocument` reads
every entry the file carries into the material table, so the panel opens on the file's values
rather than on the defaults, and `HasMaterial` decides which list rows carry the `*` marker the
game's list uses for "the file owns this name".

`WriteMerged` is the write path, and its rule is that a save never loses a line it did not write:

| Situation | Behaviour |
| --- | --- |
| Entry the session touched | its owned keys are rewritten from the panel values; a key equal to the default is dropped |
| Key the session does not own (`bump_scale`, `luma:`, `material_glass`, comments) | copied verbatim, at the indent it had |
| Entry the session did not touch | untouched, including a name the editor never showed |
| Root written by the game (`materials:`, `lights:`) | migrated to `qray_materials:` / `qray_lights:`, other root keys untouched |
| File ends without a newline | one is added before the new entry, so an append cannot glue two records together |
| Target file exists | copied to `qray.backup_materials.yaml` first |

The owned keys are the fields the SDK upload info can express and the panel can edit:
`texture_scale_u/v`, `texture_offset_u/v`, `roughness_override`, `metalness_factor`,
`base_factor`, `emissive_factor`, `is_light`, `exact_normals`, `material_glass` and
`material_water`.

### Level document

`--lights <file>` names the level document; without it the editor targets `qray.lights.yaml`
next to the scene, which is the file the game loads from `<gamedir>`. The file is a root mapping
keyed by level name (`e1m1`) and the editor owns two blocks inside the level's own section:

| Block | Keys | Scale |
| --- | --- | --- |
| `fog:` | `color:` as `rrggbb`, `density:`, `enabled:` | `density` is the authored value; the frame divides it by 64 |
| `lights:` | `- origin:` x y z, `radius:`, `intensity:`, `color:`, `spot:`, `dir:`, `angle_inner:`, `angle_outer:`, `style:` | `origin` in scene units (40 per metre), `radius` in metres, `intensity` in file units of 100 editor points |

The scales are the game's own: `origin` is a scene-space position, `radius` defaults to
`RT_CUSTOM_RADIUS_DEFAULT` metres, and a custom light's `intensity` reaches
`RT_FIXUP_LIGHT_INTENSITY` without the `/ rt_elight_normaliz` a map light takes, so one file unit
is 100 editor points. A new light's 2.0 units and 0.4 metres therefore round-trip through the
panel unchanged, and the fog colour starts white because that is what the engine shows for a
level whose section carries no `color:` key.

The level merge follows the material rules, with the level's own section as the unit of the
write: every block it carries that the editor does not own (`sky:`, comments, a block a hand edit
added) is copied verbatim at the indent it had, the other levels' sections are not touched, and a
section the file does not have yet is appended. A level with no custom light left keeps its
`fog:` block and loses `lights:`, which is how the game reads "no custom lights here"; the editor
reads it the same way, where a missing `lights:` block leaves the panel's own lights alone and
only an existing block replaces them.

`Apply` writes both documents, reloads them and reports through the toast. The first Apply also
creates a target that does not exist yet, even when only the level was edited, because the game's
apply writes the whole merged session and a scripted run expects the file it named to appear.
`Cancel` re-reads the files and re-uploads the static scene, so a discarded edit leaves the
viewport matching them; `Exit` closes without a prompt when the session is clean, and otherwise
opens `DrawSessionPrompt`, whose `Yes` saves first and `No` exits without writing. `Escape` and the
close box dismiss the prompt.

## Order of work

1. **Light authoring.** Colour, offset, the spot cone, the light style, the light list and the
   custom-light store with add/duplicate/clone/remove are done, as are both drags and the axis
   gizmo.
2. **Session lifecycle.** Done. Apply, Cancel, Exit and both writers -- `qray.materials.yaml` and
   the level scope (`fog:` / `lights:`) -- plus the notify toast and the save prompt. What remains
   of the write path is the sky block and the scope switch that decides whether it belongs to the
   level or to the global settings.
3. **Viewport aids.** The gizmo and the marker drag are done; the crosshair and the hint overlay
   are still panel text.

Global settings were the one mechanical block, and they are done: `EditorSettings.cpp` owns the
table and `DrawGlobalSettings` draws it. The material parameter table is in place and the SDK
fields it can express are wired; what is left of it is the list of unsupported parameters above,
which needs SDK upload fields before it can be authored.

## Verification

Every row marked done must be exercised by a harness that proves the change reaches the renderer,
not only the panel. A slider that moves is not evidence: the light authoring path was accepted
only after a frame diff showed 9105 changed samples against a 57-sample control noise floor.

The material panel was verified the same way, with `PrintWindow` captures of the live editor
(1280x720 window, viewport sampled on a 3-pixel grid, luminance = mean of R+G+B):

| Check | Measurement |
| --- | --- |
| Control noise between two idle frames | 667 changed samples (sum of abs RGB delta above 6) |
| `Metalness` drag | 137269 changed samples, mean 5.572 -- the field reaches the frame |
| `Base factor`, material 0.08 -> 3.6 | viewport luminance 222.13 vs 268.935, delta 46.805 on the picked material |
| Static re-upload after an edit | `editor.log`: `geometry upload: 68 surfaces` / `submit static scene: QR_SUCCESS` / `materials applied: static scene re-uploaded` per change |

An earlier run of the same `Base factor` measurement showed a delta of 0.122, which was read as a
dead slider. The cause was the selection, not the field: the drag targeted material index 0, whose
surfaces are not in the camera frustum. Repeating it on the material a viewport pick selected
produced the 46.805 above. The lesson is part of the harness rule -- a negative result has to name
the surface it measured.


The global settings were accepted the same way. Dragging `Global / Brightness` from 1.00 to the
slider maximum changed 178615 of 921600 sampled pixels, against 309 for two consecutive frames
taken without touching the panel.

Light authoring was verified on light 8 of `scenes/e1m1.bsp` (picked in the viewport, selection
overlay and `editor.log` both named light 8), with the viewport region (x >= 400) compared
separately from the panel so that the panel text and swatch cannot fake a hit:

| Check | Measurement |
| --- | --- |
| Control noise, two idle frames | 461 changed samples of 921600 (mean 0.1424) |
| `Position` drag, +10 units on the second component | 529101 of 580800 viewport samples changed, mean 42.4271 |
| `Position` drag log | `light 8: position 480 -220 168 cluster 1133` -- the entry carries the re-resolved cluster |
| `Colour` edit to magenta | 561695 of 580800 viewport samples changed, mean 84.1652 |
| `Colour` edit log | `light 8: colour 1.000000 0.000000 1.000000` |

The light block sits below the panel fold on a 720p window, so the `Scene` tab scrolls; the light
fields emit `panel rect: …` into `editor.log` whenever the selection or the scroll offset changes,
which is how a scripted session finds them without reading pixels. A `DragFloat3` over the panel
is enough to move a light, and `Ctrl+Click` on a colour channel accepts a typed value.

The session writer was accepted against a hand-written file that carries everything the editor
does not own: a header comment, `bump_scale`, a nested `luma:` block, a second entry with a
non-standard three-space indent and its own `material_glass`, and the game's `materials:` /
`lights:` root keys. One Apply kept every one of those lines, migrated the two root keys to
`qray_materials:` / `qray_lights:` and wrote `qray.backup_materials.yaml`; a second Apply on the
result was byte-identical, which is the idempotence the game's merge writer also has.

The reader was checked on the same file: `slime0 read=1 type=0 scaleU=2.0000 offsetU=2.9200`.
The panel-to-file path was measured through the GUI: selecting `*slime0`, moving `Scale V` to 16
and pressing `Apply` wrote

```text
  - name: *slime0
    texture_scale_u: 2
    texture_scale_v: 16
    texture_offset_u: 2.92
    # the game only: a bump scale and nested luma blocks
    bump_scale: 0.25
    luma:
      - color: ff0000
```

which is the proof that the write starts from what the file carried (`texture_scale_u: 2`,
`texture_offset_u: 2.92` survived an Apply that never touched them) and that unchanged keys are
not rewritten from the defaults.

The exit prompt was driven through `WM_CLOSE` and the panel buttons: `Escape` logged
`session: prompt dismissed` and kept the session, `No` logged `session: changes discarded` and the
process exited with the file untouched, `Yes` logged `session: saved <path>` before the exit and
the file carried the edit. The prompt is a fixed 440x148 window for the reason the panel layout
logs exist: an `AlwaysAutoResize` prompt moved under the cursor between the capture and the click,
which made a scripted click miss a button that had rendered in that spot a frame earlier.

The level writer was accepted on a scratch document the same way. `Add light` on the Entities tab
logged `session: level e1m1 touched`, and the Apply that followed wrote

```text
e1m1:
  fog:
    color: ffffff
    density: 0
    enabled: false
  lights:
    - origin: 480 -256 110
      radius: 0.4
      intensity: 2
      color: ffffff
```

together with a header-only `qray_materials.yaml`, so the file a run names exists after the first
save even when the level was the only edit. A second Apply left the document byte-identical,
because the fingerprint is re-rendered from the state rather than read from the dirty latch.

The reader was then given a section the editor does not own: `sky: e1m1_sky` inside `e1m1`, an
`ff8000` fog with `density: 0.5` and `enabled: true`, and a second level `e1m2`. A fresh launch
logged `read, 1 lights for e1m1`, one more `Add light` plus `Apply` rewrote the fog with the values
that were read rather than with the defaults, kept `sky:` and the whole `e1m2:` section verbatim
and wrote `qray.backup_lights.yaml` as the previous generation. Removing the last light and
applying again dropped the `lights:` block and left `fog:`, `sky:` and `e1m2:` in place, which is
the empty-store shape the game reads back as "no custom lights on this level".
