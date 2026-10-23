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
| Global tab | 45 settings across the `Global`, `Sky`, `Clouds`, `Sun`, `God rays`, `Volumetric fog`, `Tonemap` and `Water / Acid` groups, plus fog and the level/global scope switch | the `QRE_G_*` and `qre_water[]` row tables in `Quake/qr_editor.c` |
| Shared | free camera, crosshair pick, hint overlay, notify toast, modal dialogs, texture browser with previews | `QR_GUI_*` in `Quake/qr_gui.h` |

## Current standalone editor

`EditorApp::DrawPanel` draws one window with these rows: frame/stat readout, scene counts,
cluster-light stats, bounds, camera readout and fly speed, one line of session state, then the
`Apply` / `Cancel` / `Exit` row, then a selection block
that is either a surface (index, triangle, texture name, kind, cluster) or a light (position,
radiance, cluster, `Light key`, `Radius`, `Colour`, `Position`), then `At`, `Clear selection`, a
`Sun` hint that points at the `Render` tab and the scrolling global-settings block
(`Global`, `Sky`, `Clouds`, `Sun`, `God rays`, `Volumetric fog`, `Tonemap`, `Water / Acid`,
`Level fog`).
`DrawViewportOverlay` draws the crosshair, the key hints and the drag prompt over the viewport, so
they stay on screen with the panel closed, the way the game draws them over the map.

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
| Sun intensity, azimuth, elevation, angular size | done | `Sun` group: `Light` (`rt_physical_sun`), `Light strength` (`rt_sky_sun`), `Pitch` / `Yaw` (`rt_sky_sun_pitch` / `rt_sky_sun_yaw`) and `Disc scale` / `Disc colour` / `Disc size`; the direction reaches `QrDrawFrameVolumetricParams::volume.sourceDirection` as the game's `+forward` |
| Surface material read-out | done | texture name and pass-through kind; a surface pick also selects its material in the `Materials` tab |
| Material parameters (32 of them, per animation frame) | partial | `EditorMaterial.{h,cpp}`; 23 of the game's parameters, one material per texture name, applied by re-uploading the static scene. The six that need a texture the SDK cannot carry are baked by `EditorMaterialBakes`, which synthesises the albedo/normal/roughness-metallic-emission triplet the way `gl_texmgr.c` does. The nine that remain are listed under [Materials](#materials) |
| Texture browser with previews and search | done | `DrawTextureBrowser`; a grid of the scene's own textures with a name filter, the picked texture outlined in the game's orange, and `Select` / `NONE` writing `texture_base` |
| Texture UV scale/offset authoring | done | `Scale U/V` and `Offset U/V` feed the layer-0 texcoord transform of the upload (`QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT`), which is one pass with the geometry it changes. `qrUpdateGeometryTexCoords` is implemented for an embedder that already has its vertices uploaded: it writes the layers into the collector's staging buffer, and the values reach the device at the next static submit, the same contract `qrUpdateGeometryTransform` has |
| Luma mask authoring and preview | partial | `Luma` sets `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT`, `Emissive` sets the emission scale and the `Luma blend` combo picks the composite mode. There is no colour-keyed mask block and no preview: the game's `luma` key is a list of per-colour threshold/feather/blend blocks with no SDK field behind it |
| Glass and water pass-through parameters | done | `PassThroughTypeForSurface` picks the type and, with `Material` set to `glass`, `Glass IOR`, `Glass thickness`, `Glass colour` and `Transparency` are the pane's own parameters; `Mirror` is the surface's. Water has no per-material parameters in the game either -- its look is the System tab's `Water / Acid` group, which the editor's `Water / Acid` group carries |
| Light colour authoring | done | `Colour` (`ColorEdit3`) writes `EditorLight::tint`; `SetEditorLightTint` rebuilds radiance as `intensity * kEditorLightIntensityScale * tint` |
| Light position and offset authoring | done | `Position` (`DragFloat3`) moves the light and re-resolves its cluster through `ResolveEditorLightCluster`; `Place at pick` drops it 8 units in front of the picked surface along the pick ray and aims the cone down that ray |
| Light spot direction and inner/outer angles | done | `Spot` turns the cone on, `Spot dir` is three separate `##dirx/##diry/##dirz` fields, `Inner deg`/`Outer deg` the falloff, all through `qrUploadSpotLight`. Measured on `e1m1` at radius 300: spot off→on changes 52820 viewport pixels (mean 3.77) against a 1360/1.01 noise floor, direction mean 138.41, outer 0→60° mean 77.19, inner 0→40° mean 43.20 |
| Light style authoring | done | `Light style` combo over the 12 `rt_custom_style_names`; a map light also offers `none`, a custom light does not, the way the game's Custom tab does not. `EditorLight::style` is authored metadata: a standalone editor has no `d_lightstylevalue[]` feed, so the preview keeps full brightness and the style names the animation the light gets once saved. Selecting style 3 logs `custom 0: style 3 candle` |
| Light list for the Entity tab | done | `DrawLightList`: a `Map` and a `Custom` table, both inside a clipped `BeginChild` so the lists scroll instead of pushing the selection block out of the window. Each row shows key, intensity, radius and style; a row click selects the light the selection block below edits |
| Add / duplicate / clone / remove a custom light | done | `Add light`, `Duplicate`, `Clone`, `Remove` over an editor-side `EditorCustomLight` store. Duplicate copies the light and offsets it by 32 units on Z; Clone reproduces it at the picked surface; Remove drops the newest. Measured on `e1m1`: add changes 9196 viewport pixels (mean 2.00), duplicate 20264 (3.76), clone 6638 (1.24), remove 19565 (2.63) against a 1360/1.01 noise floor |
| Placement mode (aim and drop a light) | done | `Place at pick` on a surface pick; the light lands 8 units off the surface its cluster comes from |
| Light drag and axis gizmo | done | Two mechanics, both mirroring `QRE_CustomGizmoBegin`/`QRE_CustomGizmoMove` and `QRE_UpdateLightDrag`. Dragging an arrow moves the light along that axis in whole units: the axis direction is frozen at mouse-down so a straight drag cannot curve, the origin is a dead zone so a click near it reselects instead of grabbing, and axes shorter than 8 px on screen are not grabbable. Dragging the light itself flies it along the pick ray to 8 units in front of the surface, or to 128 units when the ray misses all geometry, and re-resolves the cluster on every step. Escape or focus loss rolls the light back to where the drag started. Measured on `e1m1`: an axis drag of +60 px moves custom light 0 from `480 -256 110` to `490 -256 110` (cluster 1133, 4656 changed viewport pixels, mean 3.73), a grab drag from `490 -256 110` to `550 -103 135` across clusters 1120/1129/1130 changes 183261 pixels (mean 25.11) against a 1360/1.01 noise floor |
| Global settings table (54 rows) | done | `EditorSettings.cpp` `BuildEditorFrameParams` fills `QrDrawFrameSkyParams`, `QrDrawFrameTonemappingParams`, `QrDrawFrameVolumetricParams`, `QrDrawFrameLevelFogParams` and `QrDrawFrameReflectRefractParams`; `DrawGlobalSettings` draws nine groups, and the sun's cvars live in the `Sun` group |
| Water and acid authoring | done | `Water / Acid` group: `Wave speed` (`rt_water_speed`), `Wave strength` (`rt_water_normstren`), `Wave sharpness` (`rt_water_normsharp`), `Wave scale` (`rt_water_scale`), `Water colour` (`rt_water_color`) and `Acid colour` (`rt_water_acidcolor`), with the game's ranges from `qre_water[]` and its cvar defaults. `BuildEditorFrameParams` writes them into `QrDrawFrameReflectRefractParams`: speed and area scale through `kEditorUnitsPerMeter` as `METRIC_TO_QUAKEUNIT` does, and both colours raised to `1/kEditorUnitsPerMeter` because the shader reads a per-scene-unit transmittance. The block's other nine fields are the game's cvar defaults -- depth 2, IOR 1.52/1.33, glass shadows and particles on, portal twirl on, denoise off, `acidDensity` and `turbWarpStrength` zero as `editor_active` leaves them. Before this, the editor sent no block at all and the renderer fell back to its own defaults, so water came out at the fallback colour with no glass particles and no twirl. The game keeps these six as cvars and writes none of them into the level file, so the panel holds them for the session only: `Apply` still writes the materials, the lights and the `sky:`/`fog:` blocks alone |
| Fog authoring | done | `Level fog` group: colour, density (divided by 64 like the worldspawn key), sky blend |
| Cloud authoring | done | `Clouds` group, packed into `skyCubemapRotationTransform` per `QrDrawFrameSkyParams` |
| God rays authoring | done | `God rays` group including `Sky threshold`; the shafts follow the authored sun and the panel no longer gates the effect on the procedural sky, which is the game's own condition on the sky texture rather than on the cvar |
| Tonemap authoring | done | `Tonemap` group including the LPM percentiles the game hard-codes |
| Volumetric fog authoring | done | `Volumetric fog` group; the scattering source follows the authored sun |
| Level/global scope switch | done | the `Sky` group's `Global settings` box selects the scope of the `sky:` block, which is written per key like the game's `CaptureScope`; see [Sky block](#sky-block) |
| Apply / Cancel / Exit with a dirty prompt | done | `DrawSessionBar`, `ApplySession`, `DiscardSession`, `RequestExit` and `DrawSessionPrompt`. `WM_CLOSE` goes through the same prompt; `Yes` saves and exits, `No` exits without writing, `Escape` or the close box dismisses the prompt and keeps the session |
| Save back to `qray.materials.yaml` | done | `EditorMaterialsDocument::WriteMerged` in `EditorSession.cpp`; `--materials <file>` picks the target, and the previous generation is copied to `qray.backup_materials.yaml` before a write |
| Notify toast | done | `DrawNotification` under the viewport: `saved …`, `changes discarded`, `nothing to save` and `session reset, defaults restored`, held six seconds and faded over the last one |
| Session reset | done | Clicking the tab that is already open asks first, the way `QR_GUI_Tabs` returns `reset = 1` for a re-click in `qr_editor.c`, and `ResetSession` then does what `QRE_ResetAll` does: the materials file, the lights file and both backups are removed, and the panel goes back to a first run's state. The panel's own values are replaced rather than "left alone", because an absent level section deliberately keeps what the panel holds -- fog, the sun and every `sky:` key would otherwise survive the file they came from. See [Reset](#reset) |
| Modal dialogs | partial | two prompts: the save-on-exit prompt (`DrawSessionPrompt`) and the reset prompt (`DrawResetPrompt`). Both fix the width and let ImGui fit the height, so the buttons cannot end up outside a window that clips them. The game's remaining popups have no counterpart yet: `regen_prompt` ("Regenerate from map") needs a light-model flag `EditorLight` does not carry, and the texture chooser is the browser rather than a modal |
| Crosshair and hint overlay | done | `DrawViewportOverlay` in `ImGui::GetForegroundDrawList()`: the game's four-tick crosshair with its shadow, the hint block and the drag prompt below. See [Viewport overlay](#viewport-overlay) |

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
| `texture_base` | `Browse...` over the texture browser, `NONE` to clear | `geomMaterial.layerMaterials[0]`, but only when the name resolves to a texture the loaded scene carries; an unknown name leaves the material as the BSP loader derived it |
| `texture_scale_u/v`, `texture_offset_u/v` | `Scale U/V`, `Offset U/V` | layer-0 texcoord transform, with `QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT` asserted only when the transform is not identity |
| `texture_normals` | `Normal##tex` combo over the scene's textures | the baked normal map handed to `QrMaterialCreateInfo::pDataNormal`, with `bump_scale` folded into its XY |
| `texture_emissive` | `Emissive##tex` combo | the baked emission channel of the RME texture |
| `texture_gloss` | `Gloss##tex` combo | the baked roughness channel of the RME texture, `1-gloss/255` |
| `bump_scale` | `Bump scale##surf` | scales the normal map's XY about the 128 midpoint, as `gl_texmgr.c` resamples it |
| `metalness_from_normal_alpha` | `Metalness from normal alpha##surf` | fills the baked metalness from the normal map's alpha times `Metalness` |
| `transparency` | `Transparency##surf` | the baked normal texture's alpha, which `HitInfo.hlsli` reads as `h.transparency` |
| `luma_blend` | `Luma blend##luma` combo `Cvar`..`Color dodge` | the RME alpha as `emissive_blend + 1`, so `Cvar` writes 0 and the shader falls back to `emissionBlendMode` |
| `material` | type combo `none`/`glass`/`water` | `QrGeometryPassThroughType`; `none` keeps the kind the BSP loader derived |
| `roughness_override` | `Roughness override` | `defaultRoughness`; the checkbox writes `kEditorDefaultRoughness`, clearing it returns the `-1` sentinel. A mirror pins it to 0 whatever the override says, which is the tooltip's "mirror forces 0; glass keeps its override" |
| `metalness_factor` | `Metalness` | `defaultMetallicity` |
| `base_factor` | `Base factor` | `layerColors[0]` as a grey albedo tint |
| `mirror` | `Mirror` | `QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR` plus `QR_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT`; glass and water outrank it, exactly as `r_world.c`'s `is_mirror` does |
| `glass_ior` | `Glass IOR` | `layerColors[2].x`, the field `RtAlphaTest.rahit.hlsl` reads; 0 keeps the engine's `rt_refr_glass` |
| `glass_thickness` | `Glass thickness` | `layerColors[2].y`, shifted view and light through the pane |
| `glass_color` | `Glass colour` | `layerColors[1].xyz`, multiplied into the transmitted view |
| `exact_normals` | `Exact normals` | `QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT` |
| `is_light` | `Luma` | `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT`; this is the one key the panel reaches the traced-glow flag through, so it writes the file's `is_light` |
| `emissive_factor` | `Emissive` | `defaultEmission`, clamped to [0, 1] by the renderer |

The three glass rows are gated on the material being `glass`, and they write nothing otherwise:
the game's pane reads them out of the layer colors only under `material_glass`, and a stray
index of refraction on a solid surface would only bend light through a wall.

`ApplyEditorMaterial` writes those fields into a `QrGeometryUploadInfo`. They are read at upload
time, so an edit is published by rebuilding the static scene: `EditorApp::Frame` watches
`m_materialsChanged`, waits until no widget is active, and calls `UploadStaticGeometry` once per
change (`materials applied: static scene re-uploaded` in `editor.log`).

Deliberately absent, because the SDK upload info has no field for them and a slider that cannot
change the picture would be a lie: `texture_overlay`, `force_rasterize`, `luma` and the `light_*`
group (`light_styles`, `light_color`, `light_brightness`, `light_emissive_focus`,
`light_emissive_soften`, `light_emissive_projection`). The game's emissive range is 0..5; the SDK
clamps emission to [0, 1].

### Material bakes

Six of the game's parameters are not upload fields at all: the game folds them into the three
textures it hands the tracer. `texture_normals`, `texture_emissive` and `texture_gloss` name extra
images, `bump_scale` scales the normal map's XY, `metalness_from_normal_alpha` fills the billed
metalness from that map's alpha, `transparency` rides in that same alpha, and `luma_blend` is the
RME texture's alpha. `EditorMaterialBakes` reproduces the synthesis `gl_texmgr.c` performs, so
these reach the renderer exactly as they do in the game.

The bakes are keyed by everything they read, so two materials that differ only in `bump_scale` do
not share one. `Get` returns `QR_NO_MATERIAL` when a material keeps the scene's own textures, so a
scene with no extra textures allocates nothing. The cache is rebuilt around
`qrBeginStaticGeometries`/`qrSubmitStaticGeometries`: `BeginPass` clears the used flags, `EndPass`
destroys the entries the pass did not ask for, and the frame logs `material bakes: N` so a change
that stops baking is visible in `editor.log`.

The source images come from the loaded scene, so a name the scene does not carry resolves to
nothing and the bake is skipped rather than substituted with a placeholder. A material with no
`texture_base` of its own uses its surface's texture as albedo, which is what the game's
`TexMgr_ReloadImagesForMaterial` does for a material that never named one.

The editor also passes `pTexturesParams` now, which it used not to: it sets
`emissionBlendMode = 1` where the game's `rt_emis_blend` defaults to 1, and leaves the rest at the
SDK's fallback defaults. Without the block the renderer forced the blend to 0, so every material
whose `luma_blend` is `Cvar` came out unblended where the game draws it normal.

### Texture browser

`Browse...` in the `Textures` group opens `DrawTextureBrowser`, the `QR_GUI_TextureBrowser`
equivalent. The grid is the scene's own texture set -- `LoadEditorSceneBsp` already decodes every
BSP texture into `EditorTexture`, and `CreateTextureMaterials` uploads each as a `QrMaterial`, so a
tile is an `ImGui::Image` over that handle and needs no new SDK entry point. The tile geometry is
the game's: 128 px images, a 10 px gap, the name under each one clipped with an ellipsis, and rows
virtualised so a 1400-texture map draws only what fits.

The filter is a case-insensitive substring over the texture name, which is the same test
`QRGuiTextHas` performs; the counter reads `N of M textures`. Opening the browser points the
selection at the material's current `texture_base` when the scene carries it, and at the
material's own texture name otherwise -- the game's `QrTextureBrowserBaseName` match, minus the
folder tree, because a standalone editor has no mods to tree.

`Select` writes the picked name and `NONE` clears the field, which is the same thing the game's
browser returns: the key's default is empty, so `WriteMerged` drops `texture_base` entirely.
`Escape`, `Cancel` and the close box abandon the pick.

A `texture_base` that names nothing in the scene is kept in the file but not applied, and the panel
says `not in this scene` beside the row, so a file authored against another map is not silently
rewritten by opening it here.

### Viewport overlay

`DrawViewportOverlay` is the editor's `QR_GUI_DrawCrosshair` / `QR_GUI_DrawHint` /
`QR_GUI_LabelBottomRight`, drawn into `ImGui::GetForegroundDrawList()` after the toast so it sits
over the panel and over everything else the frame drew.

The crosshair is the game's four ticks around the centre of the drawable: inner ends 4 px out,
7 px long, 1.6 px thick, an `IM_COL32(0,0,0,130)` shadow at thickness + 2 under an
`IM_COL32(255,255,255,225)` line. It only shows while the right button holds the mouse, because
that is the state in which a click resolves through `CursorDirection`'s centre ray instead of the
cursor position -- the game's crosshair always marks where the primary attack goes, and here the
same mark has to agree with what `Pick` will resolve.

The hint block is one blue title line (`IM_COL32(140,205,255,235)`) and four light ones
(`IM_COL32(228,234,244,220)`), each with a one-pixel `IM_COL32(0,0,0,160)` shadow, 3 px line
spacing and 14 px padding from the edges. The game owns the window's bottom-left corner for it;
here the panel owns that corner, so the block starts 14 px to the right of the panel's measured
rect and moves into the corner when `F1` closes the panel -- the anchor follows `m_panelVisible`
rather than a constant. The lines name the editor's keys: LMB picks, RMB drag looks, WASD flies,
`Q`/`E` drop and rise, `Shift` multiplies by four, `F1` toggles the panel, `Ctrl+O` resets the
camera and `Escape` cancels. The old copies of the key list in the panel footer are gone.

Two states suppress or replace the block, both of them the game's own: the texture browser is the
game's `choosing` state and draws no hints over the grid, and a drag in flight prints
`Release LMB to drop the light, Esc to return it` (or the axis-move wording) in the bottom-right in
the game's `IM_COL32(255,230,150,235)`, which is the one state the panel cannot describe because
the light is no longer where the file says.

## Session

`--materials <file>` names the document the session reads on start and writes on Apply; without
it the editor targets `qray.materials.yaml` next to the scene. `EditorMaterialsDocument` reads
every entry the file carries into the material table, so the panel opens on the file's values
rather than on the defaults, and `HasMaterial` decides which list rows carry the `*` marker the
game's list uses for "the file owns this name".

The table is keyed by the textures the scene carries, so an entry the file holds for a name this
map does not use is read but not shown -- a session file shared between levels lists every name
any of them uses. `LoadMaterialsSession` reports the count it matched (`read, 1 entries`), which is
the number to compare against the scene's texture list rather than against the file's line count.

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
`texture_base`, `texture_scale_u/v`, `texture_offset_u/v`, `roughness_override`,
`metalness_factor`, `base_factor`, `emissive_factor`, `is_light`, `exact_normals`,
`material_glass` and `material_water`.

### Level document

`--lights <file>` names the level document; without it the editor targets `qray.lights.yaml`
next to the scene, which is the file the game loads from `<gamedir>`. The file is a root mapping
keyed by level name (`e1m1`) and the editor owns three blocks inside the level's own section:

| Block | Keys | Scale |
| --- | --- | --- |
| `sky:` | the 25 `rt_sky_names[]` cvars, see [Sky block](#sky-block) | the value text the game's own writer produces |
| `fog:` | `color:` as `rrggbb`, `density:`, `enabled:` | `density` is the authored value; the frame divides it by 64 |
| `lights:` | `- origin:` x y z, `radius:`, `intensity:`, `color:`, `spot:`, `dir:`, `angle_inner:`, `angle_outer:`, `style:` | `origin` in scene units (40 per metre), `radius` in metres, `intensity` in file units of 100 editor points |

The scales are the game's own: `origin` is a scene-space position, `radius` defaults to
`RT_CUSTOM_RADIUS_DEFAULT` metres, and a custom light's `intensity` reaches
`RT_FIXUP_LIGHT_INTENSITY` without the `/ rt_elight_normaliz` a map light takes, so one file unit
is 100 editor points. A new light's 2.0 units and 0.4 metres therefore round-trip through the
panel unchanged, and the fog colour starts white because that is what the engine shows for a
level whose section carries no `color:` key.

The level merge follows the material rules, with the level's own section as the unit of the
write: every block it carries that the editor does not own (a hand-written `hint:`, comments, any
key the panel has no field for) is copied verbatim at the indent it had, the other levels'
sections are not touched, and a section the file does not have yet is appended. A level with no
custom light left keeps its `fog:` and `sky:` blocks and loses `lights:`, which is how the game
reads "no custom lights here"; the editor reads it the same way, where a missing `lights:` block
leaves the panel's own lights alone and only an existing block replaces them.

`Apply` writes both documents, reloads them and reports through the toast. The first Apply also
creates a target that does not exist yet, even when only the level was edited, because the game's
apply writes the whole merged session and a scripted run expects the file it named to appear.
`Cancel` re-reads the files and re-uploads the static scene, so a discarded edit leaves the
viewport matching them; `Exit` closes without a prompt when the session is clean, and otherwise
opens `DrawSessionPrompt`, whose `Yes` saves first and `No` exits without writing. `Escape` and the
close box dismiss the prompt.

### Reset

Clicking the tab that is already open is the game's reset gesture: `QR_GUI_Tabs` returns
`reset = 1` for a re-click on the selected tab, and `qr_editor.c` answers it with `reset_prompt`,
the same `Yes`/`No` question `QRE_ResetAll` sits behind. The standalone editor asks with
`DrawResetPrompt` and, on `Yes`, calls `ResetSession`, which removes the materials file, the lights
file and both backups and then reports `session: reset, removed …`.

It removes files rather than editing them, so the panel has to be told what a first run would
show, and that is a default-constructed `EditorRenderSettings`, `EditorSunState` and
`EditorLevelSky`: nothing but a session load or the panel itself ever writes those members, so
`{}` is exactly the state a missing file gives. This matters because an absent level section
deliberately leaves the panel's values alone -- without the assignment the fog, the sun and every
`sky:` key the removed file had set would survive the file itself, and the viewport would still
show them.

A removal that fails aborts before anything is reloaded, which is the order the game uses, so a
half-removed session never looks reset.

### Sky block

`sky:` carries the cvars the game's light editor drives from its Global tab, in the order
`Quake/rt_lights.c` `rt_sky_names[]` lists them, and it is written the way
`RT_SkySettings_WriteLevel` writes it: two-space block indent, four-space key indent, one entry
per key the level overrides, and no block at all when the level overrides nothing.

```text
e1m1:
  sky:
    rt_sky_brightness: 3.25
    rt_sky_color: 0 169 255
  fog:
    ...
```

The scope is the game's: the `sky:` block *is* the level's override set, and a level without one
follows the config. Three rules reproduce that, all of them inside `EditorApp::LevelSky` and
`AdoptSkyScope`:

| Rule | Behaviour |
| --- | --- |
| A key nobody touched | written back with the text the file already holds, so a load and a save of the same level is byte for byte -- `1.5` does not become `1.50000` through the panel's float |
| A key edited to what the global scope says | dropped from the block, which is the comparison `RT_SkySettings_CaptureScope` makes |
| Saving in the global scope | drops the level's set, so switching back to the level afterwards has nothing to resurrect -- `RT_SkySettings_CommitSession` |

The scope the file shows is the scope the session is in: a level that carries `sky:` opens with
`Global settings` off, and a save re-derives the box from the file it just wrote, which is the
state the game's editor finds when it reopens. The baseline a level key is dropped against is the
panel's own defaults, because a standalone editor has no config of its own; the game reads the
same comparison against the user's config, so a level that pins a cvar equal to its default is
written as an override by the game on a machine whose config differs, and dropped here.

A line inside the block with no slot -- a cvar a newer engine knows, or a comment -- is written
back after the managed keys exactly as it was spelled, indentation included, which is what a light
entry's own extra keys already do and why a key that carries children keeps them. It survives only
while the block does: the block's presence is the level's scope, so a save that drops the block for
the global scope takes those lines with it, the way the game's `RT_SkySettings_WriteLevel` writes
only the names it knows.

The sun is part of the block, not a separate control: `Sun / Light` is `rt_physical_sun`,
`Light strength` is `rt_sky_sun` in units of `kEditorSkySunRadiance` (the 1600 x 0.001 fixup the
game applies), and `Pitch` / `Yaw` are `rt_sky_sun_pitch` / `rt_sky_sun_yaw` -- the game's own
cvar angles, so they round-trip without an angle conversion. `rt_sky_sun_disc`,
`rt_sky_godrays_quality`, `rt_sky_sun_preset` and the two `rt_sky_sun_bounce_*` cvars are not in
`rt_sky_names[]` and stay out of the block, exactly as the game's writer leaves them out.

## Order of work

1. **Light authoring.** Colour, offset, the spot cone, the light style, the light list and the
   custom-light store with add/duplicate/clone/remove are done, as are both drags and the axis
   gizmo.
2. **Session lifecycle.** Done. Apply, Cancel, Exit and both writers -- `qray.materials.yaml` and
   the level document (`sky:` / `fog:` / `lights:`) -- plus the whole sky scope, the notify toast,
   the save prompt and the reset prompt behind a re-click on the open tab. Nothing of the write
   path is left.
3. **Viewport aids.** The gizmo, the marker drag and the overlay are done: the crosshair, the hint
   block and the drag prompt are drawn over the viewport and the key list is no longer panel text.

Global settings were the one mechanical block, and they are done: `EditorSettings.cpp` owns the
table and `DrawGlobalSettings` draws it. The material parameter table is in place and the SDK
fields it can express are wired; the texture browser is done as well, so a material's base texture
is picked from a grid of previews rather than typed. What is left of the table is the list of
unsupported parameters above, which needs SDK upload fields before it can be authored.

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
| `texture_base` through the browser | `Browse...` on `*slime0` opened the grid; the filter `wall` narrowed it; `Select` logged `texture browser: texture_base '*slime0' -> uwall1_2`, the row's 23x23 preview drew 23 distinct colours, and the live re-upload changed 18914 viewport pixels (mean 7.6188) |
| Saved `texture_base` | `qray.materials.yaml` gained `texture_base: uwall1_2` directly under `- name: *slime0`, ahead of every other key |
| `NONE` | the second save left `- name: *slime0` with no keys, so a cleared value is dropped rather than written empty |

The viewport overlay was measured on 1280x720 `PrintWindow` captures of the same scene, with the
window offset (8, 31) taken into account so client coordinates convert to image ones:

| Check | Measurement |
| --- | --- |
| Crosshair while the mouse is held | the centre row reads 7 lit pixels at x-11..x-5 and 7 at x+4..x+11 around a gap of 8, all `218,218,218` -- the game's `255,255,255` at 225 alpha over the dark wall -- with the shadow line 2 px wider |
| Crosshair after release | the 25x25 box around the centre changed 0 pixels against the held frame, so the mark belongs to the look state and not to the scene |
| Hint block with the panel open | 4499 glyph pixels starting at image column 415, which is client 407 = the panel's right edge (392) + 14, in five rows 20 px apart (602, 621, 641, 661, 681); the band above it changes 0 pixels, so the block is not scene brightness |
| Hint block after `F1` | the same 4499 glyph pixels at image column 22 = client 14, the window corner, and 0 at the panel edge -- the anchor follows the panel's measured rect |
| Drag prompt | a press on map light 7 (`grab: light 7 at 688 96 80`) drew 843 warm pixels inside 969..1256 x 684..697, the bottom-right corner with 14 px padding; `Escape` and the button release both returned it to 0 |

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

The sky block and its scope were accepted on a scratch level document whose `e1m1` section carried
six `sky:` keys in non-canonical text (`1.5`, `140`, `0 169 255`) plus a `fog:` block, and an
`other_level:` section the editor does not touch. Every sky control logs its rectangle as
`panel rect: sky rt_sky_brightness 22,351 354,374`, so a scripted run clicks a field by name:

| Check | Result |
| --- | --- |
| Launch on a level with `sky:` | `session: … read, 0 lights for e1m1, sky scope level` -- the box opens off |
| `Apply` with no edit | refused, the document stays byte-identical |
| `Sky brightness` 1.25 -> 3.25, `Apply` | only `rt_sky_brightness` changed; `1.5`, `140`, `0 169 255`, `0.6` and `other_level:` stayed verbatim, block still before `fog:` |
| `Sky scale` -> 2.5 on a level with no `rt_sky` key | `rt_sky: 2.5` inserted first, the other six keys untouched |
| `Global settings` on, `Apply` | `sky:` removed from `e1m1`, `other_level:` and `fog:` kept, log `sky: global settings` |
| `Global settings` off again, `Apply` | still no block -- the global save dropped the level's set |
| `Sky scale` -> 2.5 in level scope after that | a one-key block `rt_sky: 2.5` |
| `Sky brightness` -> 1, the panel default, `Apply` | `rt_sky_brightness` dropped from the block, the other keys untouched |
| `Sky brightness` 3.5, `Apply`, on a block holding a comment, `rt_sky_future_key: 7` and `rt_sky_future_group:` with a `child:` under it | all three lines written back after the managed keys at their own indentation; `other_level:` and `fog:` untouched |
| `Global settings` on with those lines present, `Apply` | the whole `sky:` block goes, unknown lines with it, `other_level:` and `fog:` kept |

The reset was verified with a scripted window session (`scenes/e1m1.bsp`, a materials file holding
`- name: tech08_1` with `emissive_factor: 2.0` and a lights file with an `e1m1:` `fog:` block at
density 0.060 in red). The prompt's layout goes to `editor.log` as
`panel reset prompt: window … Yes … No …`, so a script can click a button by its own rectangle
instead of by a guessed position:

| Check | Result |
| --- | --- |
| Session read | `read, 1 entries` for the `- name:` form the game writes; the entry then reaches the frame (below) |
| Entry for a name the scene lacks | read as `0 entries`, which is correct: the table is keyed by scene textures, not by file lines |
| Re-click the open `Materials` tab | `session: reset requested from the Materials tab`, prompt `window 352,218 912,463`, `Yes 362,432 628,455`, `No 636,432 902,455` -- both buttons inside the window |
| `Escape` | `session: reset dismissed`, both files still on disk |
| `Yes` | `session: reset, removed …`, both files gone, log line reads the absent target back as `absent, new file on apply, 0 lights for e1m1, sky scope global` |
| Defaults restored, measured against a defaults-only launch | the post-reset frame differs from a launch with no session files in 6354 of 88080 sampled viewport pixels (mean 2.27), and all of that residual is the bottom-centre region where the toast draws; without the fix the same comparison is 42738 samples (mean 37.75), the fog and the emissive still on screen |

That last row is also the evidence that the loaded values reached the renderer: the frame *before*
the reset differs from the defaults-only launch by the fog and the emissive the files named, which
is only possible if the session was applied to the scene.

