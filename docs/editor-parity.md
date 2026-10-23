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
cluster-light stats, bounds, camera readout and fly speed, `Reset camera`, and a selection block
that is either a surface (index, triangle, texture name, kind, cluster) or a light (position,
radiance, cluster, `Light key`, `Radius`), then `At`, `Clear selection`, a `Sun` block
(intensity, azimuth, elevation, angular size), the scrolling global-settings block
(`Global`, `Clouds`, `Sun disc`, `God rays`, `Volumetric fog`, `Tonemap`, `Level fog`) and the
key hints.

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
| Light colour authoring | missing | `EditorLight::color` is derived from intensity today |
| Light position and offset authoring | missing | |
| Light spot direction and inner/outer angles | missing | `qrUploadSpotLight` exists in the SDK; the editor never calls it |
| Light style authoring | missing | |
| Light list for the Entity tab | missing | needs a list widget and the light table the game builds |
| Add / duplicate / clone / remove a custom light | missing | the SDK has no custom-light store; this is editor state |
| Placement mode (aim and drop a light) | missing | |
| Light drag and axis gizmo | missing | |
| Global settings table (39 rows) | done | `EditorSettings.cpp` `BuildEditorFrameParams` fills `QrDrawFrameSkyParams`, `QrDrawFrameTonemappingParams`, `QrDrawFrameVolumetricParams` and `QrDrawFrameLevelFogParams`; `DrawGlobalSettings` draws seven groups |
| Fog authoring | done | `Level fog` group: colour, density (divided by 64 like the worldspawn key), sky blend |
| Cloud authoring | done | `Clouds` group, packed into `skyCubemapRotationTransform` per `QrDrawFrameSkyParams` |
| God rays authoring | done | `God rays` group, gated on the procedural sky like the game |
| Tonemap authoring | done | `Tonemap` group including the LPM percentiles the game hard-codes |
| Volumetric fog authoring | done | `Volumetric fog` group; the scattering source follows the authored sun |
| Level/global scope switch | missing | no config file is written yet |
| Apply / Cancel / Exit with a dirty prompt | missing | |
| Save back to `qray.materials.yaml` | missing | |
| Notify toast | missing | `PrintMessage` writes to `editor.log` instead |
| Modal dialogs | missing | |
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

## Order of work

1. **Light authoring.** Colour, offset, spot cone and style first, then the light list, then the
   custom-light store with add/clone/remove.
2. **Session lifecycle.** Apply, Cancel, Exit and the `qray.materials.yaml` writer, plus the
   notify toast and the dialogs they need.
3. **Viewport aids.** Crosshair, hint overlay, gizmo and drag.

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
