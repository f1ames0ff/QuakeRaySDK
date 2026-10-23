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
| Surface material read-out | done | texture name and pass-through kind |
| Material parameters (32 of them, per animation frame) | missing | needs a host-side material parameter model; the SDK's `qrCreateMaterial` takes a descriptor, not editable named params |
| Texture browser with previews and search | missing | `QR_GUI_TextureBrowser` equivalent |
| Texture UV scale/offset authoring | missing | `qrUpdateGeometryTexCoords` exists in the SDK |
| Luma mask authoring and preview | missing | |
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

## Order of work

1. **Material parameters.** The largest block and the reason the editor exists. It needs the
   parameter table (`qre_params`) and the animation-frame model ported next to the editor, then
   the `Textures`, `Surface`, `Model` and `Light` sections drawn from that table.
2. **Light authoring.** Colour, offset, spot cone and style first, then the light list, then the
   custom-light store with add/clone/remove.
3. **Session lifecycle.** Apply, Cancel, Exit and the `qray.materials.yaml` writer, plus the
   notify toast and the dialogs they need.
4. **Viewport aids.** Crosshair, hint overlay, gizmo and drag.

Global settings were the one mechanical block, and they are done: `EditorSettings.cpp` owns the
table and `DrawGlobalSettings` draws it.

## Verification

Every row marked done must be exercised by a harness that proves the change reaches the renderer,
not only the panel. A slider that moves is not evidence: the light authoring path was accepted
only after a frame diff showed 9105 changed samples against a 57-sample control noise floor.

The global settings were accepted the same way. Dragging `Global / Brightness` from 1.00 to the
slider maximum changed 178615 of 921600 sampled pixels, against 309 for two consecutive frames
taken without touching the panel.
