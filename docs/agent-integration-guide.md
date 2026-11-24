# QuakeRaySDK Integration Guide for Agents

> **See also:** [AGENTS.md](../AGENTS.md) | [docs/README.md](README.md) | [embedding.md](embedding.md) | [host-contract.md](host-contract.md) | [spatial-contract.md](spatial-contract.md)

This document is the end-to-end engineering playbook distilled from integrating `QuakeRaySDK` into `ioq3-rt` (id Tech 3 / Quake III Arena runtime + standalone `quakerayeditor` format adapter). Read it completely before modifying `QuakeRaySDK` or wiring a new engine (Quake II, Quake III, GoldSrc, Doom 3, Source, or custom host).

---

## Architecture & Strict Responsibility Boundaries

`QuakeRaySDK` is a source-level Vulkan/NVRHI ray-tracing renderer plus a generic scene/material/light authoring shell (`quakerayeditor`). It intentionally excludes game-specific map/model/shader parsers, filesystem VFS glue, and game logic.

```text
+-------------------------------------------------------------------+
| Host Engine Repository (e.g. ioq3-rt)                             |
|                                                                   |
|  1. Runtime Renderer Bridge (e.g. code/rendererqray/qray_host.cpp)|
|     - CMake links qray::renderer                                  |
|     - Converts Quake units -> meters, maps BSP clusters (+1),     |
|       uploads world geometry, brush submodels, overlays, HUD,     |
|       sky, entities & dlights                                     |
|                                                                   |
|  2. Game-Specific Editor Adapter (code/rendererqray/editor/)      |
|     - Q3EditorAdapter.*, Q3Bsp.*, Q3Shader.*, Q3Model.*           |
|     - Injected into quakerayeditor via CMake variables:           |
|       QR_EDITOR_ADAPTER_SOURCES & QR_EDITOR_ADAPTER_INCLUDE_DIRS  |
+---------------------------------+---------------------------------+
                                  |  Links qray::renderer / compiles against editor/Scene.h
                                  v
+-------------------------------------------------------------------+
| QuakeRaySDK (submodule at e.g. code/quakeraysdk)                  |
|                                                                   |
|  - Include/qray/qray.h          (normative C ABI)                 |
|  - Include/qray/host_contract.h (C++20 contracts)                 |
|  - Include/qray/gpu_context.h   (shared NVRHI/Vulkan device API)  |
|  - Source/                      (Vulkan/NVRHI RT backend + FFX)   |
|  - editor/                      (generic UI, viewport, picking,   |
|                                  gizmos, YAML session, bakes,     |
|                                  baseline BSP29/OBJ/MDL loader,   |
|                                  EditorFormatAdapter hook slot)   |
+-------------------------------------------------------------------+
```

### Decision Matrix: Where Does a Change Belong?

| Change Description | Target Repository | Why |
|---|---|---|
| Ray-tracing pipeline, denoiser, NVRHI pass, public `qray.h` C ABI | **QuakeRaySDK** | Core SDK responsibility. |
| Generic `quakerayeditor` UI, marquee selection, viewport gizmos, YAML session read/write rules, `EditorMaterialBakes`, AI texture generation button, `EditorFormatAdapter` hook fields | **QuakeRaySDK** | Shared across all engines using `quakerayeditor`. |
| Runtime conversion (`qray_host.cpp`, window integration, entity/submodel transforms, game PVS/light style bridge) | **Host Engine Repo** | Specific to host engine structs, cvars, and client/server state. |
| `IBSP` v46 / `.shader` / `.md3` / `.skin` / `.pk3` or `IBSP` v38 / `.wal` / `.md2` loaders and `EditorFormatAdapter` registration (`Q3EditorAdapter.*`, `Q3Bsp.*`, etc.) | **Host Engine Repo** | Game format knowledge must never leak into `QuakeRaySDK`. |
| Hardcoded game model filters (e.g. `flame.mdl flame2.mdl`) or Quake 3 paths inside `QuakeRaySDK/editor/` | **Rejected everywhere** | Violates engine neutrality and breaks user light management. |

---

## Part 1 — Connecting & Embedding QuakeRaySDK in a Host Engine (Runtime)

### 1.1 Submodule & CMake Setup

Mount `QuakeRaySDK` inside the host repository (for example as a submodule at `code/quakeraysdk`) and wire it in CMake (reference pattern: `ioq3-rt/cmake/renderer_qray.cmake`):

1. **Patch NVRHI:** Stock NVRHI limits `c_MaxBindingLayouts` to 8; QuakeRay's RT pipelines require at least 13. Apply `cmake/nvrhi-max-binding-layouts.patch` (which raises `c_MaxBindingLayouts` to 16) before building `nvrhi` and `nvrhi_vk`:
   ```bash
   git -C /path/to/nvrhi apply /path/to/QuakeRaySDK/cmake/nvrhi-max-binding-layouts.patch
   ```
2. **Build FidelityFX Provider:** Build `libamd_fidelityfx_vk.so` (Linux) via `Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh` or `amd_fidelityfx_vk.dll` (Windows) via `Source/FidelityFX_SDK_1.1.4/windows/build_provider.ps1`, and pass `-DQR_FFX_VK_SO=...` / `-DQR_FFX_VK_RUNTIME_DLL=...`.
3. **Preserve PIC and Disable Cross-Target IPO:** When linking `qray::renderer` into a dynamically loaded renderer module (like `renderer_qray.so` / `renderer_qray.dll`), enable `CMAKE_POSITION_INDEPENDENT_CODE ON` and temporarily set `CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF` around `nvrhi` and `QuakeRaySDK`.
4. **Configure Editor Adapter Injection (when `QR_BUILD_EDITOR=ON`):** Set `QR_EDITOR_ADAPTER_SOURCES`, `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, `QR_EDITOR_SHADER_DIR`, and `QR_EDITOR_TEXTURE_DIR` before `add_subdirectory("${QUAKERAY_SDK_DIR}" ...)` so `quakerayeditor` compiles with your host adapter sources directly into its executable without putting adapter files inside `QuakeRaySDK`.
5. **Deploy Runtime Assets Next to Binaries:** Ensure `shaders/`, `textures/BlueNoise_LDR_RGBA_128.png`, `textures/WaterNormal_n.png`, and `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll` sit beside both your renderer executable and `quakerayeditor`.

### 1.2 Instance Ownership & Spatial Boundary

- Create **one** `QrInstance` per window (`qrCreateInstance` or `qray::qrCreateInstanceWithGpuRequirements`). Never create secondary Vulkan/NVRHI devices or swapchains for the same target.
- Immediately after instance creation — **before any spatial upload or frame draw** — declare your host axis convention via `qrSetSpatialConvention`:
  ```cpp
  QrSpatialConvention spatial{};
  spatial.size = sizeof(QrSpatialConvention);
  spatial.version = QR_SPATIAL_CONVENTION_VERSION;
  spatial.sceneXAxis = QR_AXIS_POSITIVE_X;
  spatial.sceneYAxis = QR_AXIS_POSITIVE_Y;
  spatial.sceneZAxis = QR_AXIS_POSITIVE_Z;
  spatial.hostIsRightHanded = QR_TRUE;
  spatial.reserved0 = 0;
  qrSetSpatialConvention(qrayInstance, &spatial);
  ```
- **Keep unit conversion at one strict boundary:** QuakeRay's canonical editor/host convention is `40 Quake units = 1 meter` (`kEditorUnitsPerMeter = 40.0f`, `0.025f` m/unit). Convert **all** world positions, bounding boxes, light radii, reach, area-light corners/areas, camera position, near/far planes, rayLength, and fog distances to meters in a single spatial conversion layer. Never convert coordinates twice, and keep `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` UI draws in **pixels**.

### 1.3 PVS Cluster Table & Solid-Leaf Boundary Nudging

This is one of the most subtle runtime requirements:

1. **Cluster `0` is Reserved:** QuakeRaySDK reserves cluster index `0` for solid/outside space. If your engine BSP leaf `leaf` stores PVS cluster `c >= 0`, map it to SDK cluster `c + 1` (`worldClusters` array index `1 .. clusterCount - 1`). Negative clusters (`-1`) and out-of-range leaves map to `0` (`QR_CLUSTER_LIGHT_NO_CLUSTER` / unclustered).
2. **Accumulate Cluster AABBs from Geometry:** As you upload static world triangles, accumulate `pClusterMins[cluster]` / `pClusterMaxs[cluster]` (in meters!) so `qrUploadWorldLights` has valid world-space bounds for every non-zero cluster. Note that Q3/Q2/Q1 leaves containing visible faces sometimes extend slightly differently than vis leaves; clamping/expanding bounds tightly around surface vertices keeps cluster bounds accurate.
3. **Solid-Leaf Boundary Nudging (Critical):** Vertices, surface fixture centroids, and map point lights frequently lie right on a brush split plane or 1–2 units inside solid brushwork. A single BSP point-leaf lookup returns `-1` (`cluster = 0`), causing surface lights or entity lights to drop out of PVS illumination completely!
   - **Solution:** Implement a helper like `NearbyBspClusterForPoint(point)` (see `ioq3-rt/code/rendererqray/qray_host.cpp`) that first probes `ClusterForPoint(point)` and, if invalid, probes small offsets (`±1, ±2, ±4, ±8` Quake units) along the 6 axial directions (and optional diagonal neighbors) to recover the adjacent valid PVS cluster (`>= 1`).
4. **Always Set Valid Clusters Before Uploading:** Ensure `pFaces`, vertex `cluster` fields, and `QrClusterLightSource::cluster` use the shifted (`c + 1`) cluster IDs matching `QrWorldLightsUploadInfo`.

### 1.4 Static World, Moving Brush Submodels, Overlays, and Sky

| Scene Category | SDK Mechanism | Key Implementation Notes |
|---|---|---|
| **Static World Geometry** | `qrBeginStaticGeometries` -> `qrUploadGeometry` -> `qrSubmitStaticGeometries` | Group draws by material/pass-through type (`OPAQUE`, `ALPHA_TESTED`, `GLASS`, `WATER`, etc.). Guard `Begin`/`Submit` with RAII (`StaticGeometryGuard`) so early returns never leave static upload open. |
| **Inline Brush Submodels** (`*1`, `*2`, `func_door`, `func_plat`, `func_rotating`, `func_train`, `func_bobbing`, `func_pendulum`) | Static BLAS slots + `qrUpdateGeometryTransform` per frame | Upload submodel geometry in local space once during map load; every frame compute the entity's interpolated world origin/rotation (in meters in the `QrTransform` translation) and call `qrUpdateGeometryTransform`. Skip editor/runtime upload for invisible `func_areaportal` / trigger / origin brush models only. |
| **Multi-Stage Overlays & HUD** | `qrUploadRasterizedGeometry` | Use `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT` for world animated/additive/blended shader passes (upload opaque/cutout before blended, far-to-near), `..._SKY` for sky passes, and `..._SWAPCHAIN` for 2D HUD/console/menu quads with pixel viewport/orthographic matrix. |
| **Animated Material Frames** | `qrCreateAnimatedMaterial` / `qrChangeAnimatedMaterialFrame` | Use SDK animated materials or dynamic per-frame texture selection for `animMap` stages. |
| **Sky & Directional Sun** | `QR_GEOMETRY_VISIBILITY_TYPE_SKY` + `qrCreateCubemap` / procedural sky | Mark sky surfaces so primary rays read sky/cubemap/sun rather than blocking world geometry or casting unwanted solid shadows. |

---

## Part 2 — Writing a Game-Specific `EditorFormatAdapter` for `quakerayeditor`

> **Note:** On the adapter-enabled editor interface (`editor/Scene.h` and `editor/CMakeLists.txt`, paired with `ioq3-rt/code/rendererqray/editor/`), `quakerayeditor` loads game formats through `EditorFormatAdapter` while keeping baseline OBJ/BSP29/MDL support built in.

### 2.1 How the Adapter Plugs In

`QuakeRaySDK/editor/CMakeLists.txt` exposes two cache variables:

- `QR_EDITOR_ADAPTER_SOURCES` — list of host-owned `.cpp` files compiled directly into the `quakerayeditor` executable.
- `QR_EDITOR_ADAPTER_INCLUDE_DIRS` — include directories needed by those adapter sources.

In your adapter `.cpp` file (see `ioq3-rt/code/rendererqray/editor/Q3EditorAdapter.cpp`), self-register the adapter automatically before `main()` using a portable static registrar object:

```cpp
namespace {
struct MyGameAdapterAutoRegistrar {
    MyGameAdapterAutoRegistrar() { RegisterMyGameEditorFormatAdapter(); }
};
[[maybe_unused]] const MyGameAdapterAutoRegistrar g_myGameAdapterAutoRegistrar;
} // namespace

void RegisterMyGameEditorFormatAdapter() {
    EditorFormatAdapter adapter;
    adapter.extraArchiveExtensions = { ".pk3" };
    adapter.extraModelExtensions = { ".md3" };
    adapter.extraBaseGameDirs = { "baseq3" };
    adapter.extraModConfigFiles = { "q3config.cfg" };
    adapter.extraModSubdirs = { "scripts", "models" };
    adapter.modDirectoryDescription = "Mount a Quake / Quake 3 game or mod folder (id1, baseq3, custom mods).";
    adapter.canLoadBspMemory = &IsMyGameBspMemory;
    adapter.loadBspSceneMemory = &LoadMyGameBspSceneMemory;
    adapter.canLoadPreviewModel = &CanLoadMyGamePreviewModel;
    adapter.loadPreviewModel = &LoadMyGamePreviewModel;
    adapter.shouldHideModelCatalogEntry = &ShouldHideMyGameCatalogModelEntry;
    adapter.postProcessModelCatalog = &PostProcessMyGameModelCatalog;
    adapter.resolveModelMaterialPrefix = &ResolveMyGameModelMaterialPrefix;
    adapter.findDefaultPreviewModelIndex = &FindDefaultMyGamePreviewModelIndex;
    adapter.loadExternalImageRgba = &LoadMyGameImageRgba;
    RegisterEditorFormatAdapter(adapter);
}
```

> **Path Hygiene:** Always normalize asset paths using `NormalizeEditorAssetPath(raw)` (converts `\` to `/`, strips leading `/` and `./`, lowercases for case-insensitive VFS lookup). Quake 3 `.skin`, `.shader`, and `.pk3` entries routinely mix `./models/...`, upper/mixed case, and backslashes.

### 2.2 Archive Discovery & External Image Loading

1. **VFS Extensions (`EditorAssetSource` in `editor/Assets.h`):**
   - `extraArchiveExtensions` tells the mod/mount scanner to mount additional zip-style packs (`.pk3`, `.pk4`, etc.) automatically in load order alongside `pak*.pak`.
   - `extraBaseGameDirs`, `extraModConfigFiles`, and `extraModSubdirs` ensure `Mount Game/Mod Folder` recognizes base/mod installations cleanly.
2. **`loadExternalImageRgba(assets, requestedPath, outRgba, outWidth, outHeight, outResolvedPath)`:**
   - First check if a parsed shader script maps `requestedPath` to a concrete stage texture (`qer_editorimage` / primary stage `map`), or fall back to stripping extensions (`StripEditorImageExtension`) and probing `.tga`, `.jpg`, `.jpeg`, `.png` (or engine formats like `.wal` decoded to RGBA8).
   - Never rely blindly on a single extension in BSP/shader filenames: Quake 3 maps often reference `.tga` in shader scripts when the file inside `pak0.pk3` is actually `.jpg` (or vice versa).

### 2.3 Parsing BSP Geometry, Bezier Patches & Clusters

When implementing `loadBspSceneMemory(bytes, sourceName, scene, error, options)`:

1. **Detect Magic/Version Strictly in `canLoadBspMemory`:** Verify header size, 4-byte magic (e.g. `IBSP`), and version (`46` for Q3; return `false` for Q1 BSP29/BSP2 so the built-in loader continues working cleanly).
2. **Tessellate Curved Surfaces (`MST_PATCH`):** Convert biquadratic Bezier 3x3 control patches into indexed triangles (`EditorSurface`), computing smooth vertex normals and continuous UVs so curved archways, domes, and pipes reflect and trace accurately.
3. **Preserve `faces` on `EditorSurface`:** Populate `EditorSurfaceFace` (`normal`, `cluster`, `positions`, `texCoords`) in addition to the merged `vertices`/`indices`. The editor uses `faces` for triangle/surface picking, cluster resolution, and DTAL area-light bounds.
4. **Skip Invisible Utility Surfaces, Keep Inline Map Submodels:**
   - Exclude compiler-only brush surfaces (`nodraw`, `clip`, `playerclip`, `hint`, `skip`, `areaportal`, `origin`, `caulk` where appropriate, unless meant for sky).
   - Include world model (`models[0]`) **and** visible inline brush models (`models[1..N]`), excluding only submodels owned by explicit hidden/excluded brush entities if needed.
5. **World Clusters & PVS (`scene.worldClusters` + `scene.nodes`):**
   - Build `EditorBspNode` planes and `EditorWorldClusters` (`mins`, `maxs`, `flags`, `contents`, `leafCluster`, `visData`, `visOffsets`, `skyVis`, `pvsRowBytes`, `numLeafs`) with solid/outside mapped to `0` and leaf clusters offset by `+1`.
   - Use solid-leaf boundary nudging when assigning per-vertex / per-face clusters and light clusters (`ResolveEditorLightCluster` / adapter helper).
6. **Map Entities & Camera Spawn:** Parse the entities lump for spawn priority (`info_player_deathmatch`, `info_player_start`, `info_player_coop`, `info_player_intermission`) to populate `scene.cameraPosition` and `scene.cameraYawDegrees`, plus `misc_model` static props and sky/sun hints (`hasSun`, `sunColor`, `sunIntensity`, `sunDegrees`, `sunElevation`).

### 2.4 Multi-Stage Shaders: Separating Traced Base vs. Dynamic Overlays

Engines with multi-stage shader scripts (Quake 3 `.shader`) combine diffuse + lightmap + glow/fx/environment/animated passes that cannot simply be flattened into a single RGBA albedo image.

Follow this exact decomposition pipeline (reference: `ioq3-rt/code/rendererqray/editor/Q3Shader.*` & `Q3Bsp.cpp`):

1. **Classify Each Shader:**
   - **Sky (`isSky`):** Surface goes into `EditorSurfaceKind::Sky`, sets skybox/env (`scene.skyboxName`), and marks sky visibility clusters.
   - **Water / Slime / Lava / Glass / Mirror / Fence:** Map to matching `EditorSurfaceKind` (`Water`, `Slime`, `Lava`, `Opaque` + `isGlass`/`isMirrorShader`, `Fence` with `alphaTested`) so path-traced refraction, water caustics, reflections, and alpha cutouts work properly.
   - **Metallic Env Base:** Recognize Quake 3 `tcGen environment` base stages (chrome/gloss metal) cleanly (`metallicEnvBase = true` / `isMetallicEnv = true`) instead of projecting static environment bitmaps as diffuse wallpaper over opaque geometry.
   - **Traced Base + Overlay (`tracedBase = true`, `usesOverlay = true`):** Put the opaque/alpha-tested base stage on `EditorSurface` for BVH ray tracing, and push dynamic/animated/additive/blended overlay stages (starting after `baseStageIndex`, skipping lightmap passes) into `EditorOverlaySurface` (`scene.overlaySurfaces`).
   - **Non-Traced Dynamic Overlay (`isNonTracedOverlay = true`):** For pure additive sprites, volumetric flares, teleporter beams, flames, and forcefields (`autoSprite` / `autoSprite2` / additive-only stages):
     - Emit an `EditorOverlaySurface` for live animation (`animMap`, `tcMod scroll/rotate/stretch/turb/scale`, `rgbGen`/`alphaGen wave`).
     - Still emit an `isOverlayOnly = true` proxy `EditorSurface` (plus crossed quads for `autoSprite` picking) so clicking in the viewport can select the shader material or sprite!
2. **Handle Cutout Frames Over Dark/Additive Voids (`cutoutFrameUnderlay`):**
   - Shaders like floor jump-pads or monitors draw an emissive/animated texture *under* a grating/frame texture with binary alpha holes (`GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA` + `alphaFunc`).
   - If you trace only the alpha-tested frame, primary/secondary rays pass through its cutout holes into empty geometry or sky!
   - **Fix:** Detect when an emissive/dark base sits beneath a cutout stage (`cutoutFrameUnderlay = true` / `isCutoutUnderlay = true`). Offset the underlay geometry slightly behind/inward along smooth vertex normals using `OffsetEditorVerticesAlongSmoothNormals(verts, indices, -0.8f)` (or upload a solid dark backing surface behind traced cutouts) so rays through holes terminate on the lit/backing layer cleanly without z-fighting.
3. **Suppress Redundant Base-Copy Passes:**
   - Shaders written as `[base stage] -> [blendFunc GL_DST_COLOR GL_ZERO $lightmap]` only exist to multiply in baked Quake 3 lightmaps; once `QuakeRay` ray-traces the scene, skip the lightmap multiply stage and any redundant `DST_COLOR ZERO` base copy so the traced base is not darkened twice.

### 2.5 Predefined Map Lights & Light Editor Integration (`usesExternalLightingModel`)

> **Crucial Design Requirement:** Every predefined light source the engine renders must be editable, toggleable, and deletable in `Light Editor`.

When your adapter loads a map whose lighting model comes from the host engine rather than Quake 1 palette fullbrights:

1. **Set `scene.usesExternalLightingModel = true`.**
2. **Populate BOTH `scene.lights` and `scene.bspLights` with ALL predefined map light sources:**
   - **Map point/spot entities:** All `"light"` and `"lightJunior"` entities (parsing `origin`, `_color` / `color`, `light` / `_light`, `style`, and `target` -> `info_null` target entity for directional spotlights).
   - **Map surface & fixture lights (`q3map_surfacelight` / luminous overlays):** In Quake 3 maps (`q3dm17`, `q3tourney2`, etc.), `q3map` bakes `q3map_surfacelight` brushes into lightmaps and strips point `"light"` entities from the BSP! If the adapter ignores `q3map_surfacelight > 0` surfaces (or only emits non-traced flame sprites), those lamps still illuminate the scene via shaders/DTAL, **but have NO cyan selection circle in `Light Editor` and cannot be moved, tuned, disabled, or deleted!**
   - **How to extract fixture lights cleanly:** Collect non-sky surfaces with `def.surfaceLight > 0.0f` (and `autoSprite`/`autoSprite2` light sprites). Group adjacent polygons of the same lamp fixture together (e.g. bounding-radius merge / spatial deduplication within ~24–96 units) rather than emitting dozens of duplicate overlapping lights per brush face, compute area-weighted centroid + normal nudge outward (`+4` units along face normal for non-mesh faces), resolve PVS cluster, derive color/intensity from `q3map_lightRGB` or texture emission, and push an `EditorLight` into both `scene.lights` and `scene.bspLights` with `className = shaderName` (e.g. `textures/gothic_light/gothic_light2_lrg`).
3. **Prevent Hidden Unselectable DTAL Duplicates:**
   - On `usesExternalLightingModel = true` scenes, the editor (`EditorApp.cpp`) intentionally withholds automatic `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` emission on surfaces unless the user explicitly checks `Luma` in Material Editor, and keeps global automatic material-to-DTAL `truelight` synthesis off unless enabled. Meanwhile the lamp's RME fullbright channel preserves the visible visual glow on the mesh itself, while actual scene illumination comes from the selectable `EditorLight`s in `scene.lights`!
4. **Full Persistence Lifecycle (`qray.lights.yaml` & `Regenerate from map`):**
   - `scene.bspLights` holds the immutable baseline extracted from the BSP; `scene.lights` holds the active mutable list shown in `Light Editor -> Entity`.
   - When the user disables (`enabled: false`), edits, adds, or deletes standard map lights and clicks **Apply**, `EditorSession` writes the complete active state (including `class:` metadata and an explicit empty `lights: []` if all lights were deleted) to `qray.lights.yaml`.
   - On next map load, an existing `lights:` block (including `lights: []`) overrides `scene.lights` cleanly so deleted lights stay deleted.
   - Clicking **Global -> Regenerate from map** restores `scene.lights = scene.bspLights` cleanly.

### 2.6 Multi-Part Model Preview & Catalog Assembly

For complex engines like Quake 3 where a player character is composed of `lower.md3` + `upper.md3` + `head.md3` + `animation.cfg` + `*.skin`:

1. **Assemble Multi-Part Characters Virtualized as `.character` (or native composite):**
   - In `postProcessModelCatalog(entries)`, inspect cleaned relative paths (`CleanRelativeAssetPath` + `NormalizeEditorAssetPath`). Whenever a directory contains at least two of `{lower.md3, upper.md3, head.md3}`, synthesize a single virtual entry `<dir>.character` and sort character entries predictably.
   - In `shouldHideModelCatalogEntry`, hide clutter like `_hand.md3`, `_1.md3`, `_2.md3` (first-person view weapon helpers), while keeping standalone weapons/props (`.md3`) selectable.
   - Do **not** blindly hide `lower.md3`/`upper.md3`/`head.md3` if you still want direct access or handle hiding cleanly only when collapsed.
2. **Tag-Based Frame Assembly in `loadPreviewModel`:**
   - Parse `animation.cfg` (`sex`, `headoffset`, `footsteps`, torso/legs frame ranges) to compute frame counts and frame names.
   - Concatenate `lower.md3` -> `upper.md3` (via `tag_torso` rigid 3x4 transform) -> `head.md3` (via `tag_head` rigid 3x4 transform) per `EditorPreviewModelFrame`.
   - Load `*.skin` files (`default`, `red`, `blue`, etc.) into `EditorPreviewModelSkinPreset` so the user can switch character skins inside the Models tab, and bind shader/texture prefixes via `resolveModelMaterialPrefix`.

---

## Part 3 — Graveyard of Pitfalls (What Broke and How to Avoid It)

Every item below happened in real debugging sessions during the Quake III (`ioq3-rt`) integration. Treat each as a hard invariant.

### Pitfall 1: Mixing Adapter Code into `QuakeRaySDK`
- **Symptom:** Game-specific loaders (`Q3Bsp.cpp`, `Q3Shader.cpp`, `Q3Model.cpp`, `Q3EditorAdapter.cpp`) get committed inside `QuakeRaySDK/editor/`, polluting SDK PRs with Quake 3 internals.
- **Root Cause:** Treating `QuakeRaySDK/editor/` as the owner of every game's asset format instead of keeping only the generic adapter hook (`EditorFormatAdapter`) in SDK.
- **Fix:** Keep all game-specific loaders in the host repo (`ioq3-rt/code/rendererqray/editor/`), build them via `QR_EDITOR_ADAPTER_SOURCES` / `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and put only generic SDK/UI features in `QuakeRaySDK` PRs.

### Pitfall 2: Predefined Surface Lights Without Selection Circles in `Light Editor`
- **Symptom:** Lamps, ceiling panels, torches, and jump pads cast real-time ray-traced light in `QuakeRay`, yet have **no cyan selection circle**, do not appear under `Light Editor -> Entity`, and cannot be turned off or deleted.
- **Root Cause:** Adapter ignored `q3map_surfacelight` brushes or excluded traced surfaces from `scene.lights`, leaving them to DTAL / `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` (`BuildEmissiveLights`). DTAL area lights generated purely from textures do not create `EditorLight` gizmos/entities.
- **Fix:** Extract **all** predefined map emitters (map entities **and** all `q3map_surfacelight > 0` surfaces/fixtures, clustered per fixture) into `scene.lights` and `scene.bspLights` with valid `className` and cluster, and avoid auto-emitting hidden DTAL/ray-only surface light duplicates when `usesExternalLightingModel` is true.

### Pitfall 3: Hardcoded Model Filters & Deleted Standard Lights Returning on Reload
- **Symptom:** Standard map lights were filtered via hardcoded strings like `"flame.mdl flame2.mdl"` into `qray.materials.yaml`, OR deleting all standard lights in `Light Editor` caused every light to reappear on the next map load.
- **Root Cause:** Filtering by engine-specific model names breaks general engines (Quake 2/Quake 3), and omitting the `lights:` YAML block when `m_scene.lights` is empty makes the loader fall back to BSP defaults instead of honoring user deletions.
- **Fix:** Never hardcode game model names in `QuakeRaySDK/editor/`. Store all predefined map lights in `scene.lights` / `scene.bspLights`, serialize empty standard lights explicitly as `lights: []` in `qray.lights.yaml`, and reserve BSP reset strictly for explicit `Regenerate from map`.

### Pitfall 4: Multi-Stage Shaders Rendering Black, Double-Dark, or Leaking Sky
- **Symptom:** Launch pads, teleporters, animated monitors, grates, banners, or flames appear black, show opaque black rectangles instead of glow, or show sky through cutout holes.
- **Root Cause:** Flattening multi-stage shaders to stage 0, keeping lightmap `GL_DST_COLOR GL_ZERO` multiply stages after RT replaced lightmaps, failing to clean black fringes on additive maps (`CleanAdditiveBlackBackground`), or tracing binary alpha cutouts without a backing layer.
- **Fix:** Separate traced base (`EditorSurface`) from animated/blended overlay stages (`EditorOverlaySurface`), strip lightmap/redundant `DST_COLOR ZERO` stages, offset cutout-underlay backing meshes inward with `OffsetEditorVerticesAlongSmoothNormals`, and use `autoSprite`/`autoSprite2` quads where appropriate.

### Pitfall 5: `std::__ios_failure: Is a directory` Crash on Bare Texture Names
- **Symptom:** Crash with `basic_filebuf::underflow error reading the file: Is a directory` when resolving a shader/material named e.g. `default` or a directory stem.
- **Root Cause:** On Linux/POSIX, `std::ifstream` succeeds in *opening* a directory path (such as `./textures/default/`), and only throws when attempting to read bytes from it!
- **Fix:** Always verify `std::error_code ec; std::filesystem::is_regular_file(candidate, ec)` before opening or reading any candidate path on disk (in `Assets.cpp`, `EditorApp.cpp`, and any host adapter helper).

### Pitfall 6: Stale `EditorMaterialBakes` GPU Cache After In-Place Texture Edits
- **Symptom:** Clicking `Generate with AI` or painting procedural masks updates `scene.textures` on CPU (and writes TGA to disk), yet the viewport shows the old flat surface until restarting the editor.
- **Root Cause:** `EditorMaterialBakes` (`m_materialBakes`, `m_previewMaterialBakes`) caches baked `QrMaterial` handles keyed by texture index (`int32_t albedo/normal/gloss/emissive`) and scalar keys. Mutating an existing `EditorTexture`'s `rgba` bytes in place does not change its index, so `m_materialBakes.Get(...)` returns the stale GPU handle.
- **Fix:** When replacing or creating a texture in place (`RegisterOrUpdateGeneratedTexture`), destroy the old pool `QrMaterial`, recreate it from the updated pixels, and call `m_materialBakes.Destroy(m_instance)` and `m_previewMaterialBakes.Destroy(m_instance)` before re-uploading static geometry / refreshing preview.

### Pitfall 7: Broken Multi-Part Models & `./` Path Mismatches in `Models` Tab
- **Symptom:** Quake 3 characters show detached torsos/heads, missing skins, or fail to show up in the `Models` browser.
- **Root Cause:** Zip/PK3 entries or loose filesystem scans with `./models/players/...`, backslash separators, or mixed case miss exact string comparisons, and hiding all `.md3` entries breaks standalone weapon/prop previews.
- **Fix:** Run `NormalizeEditorAssetPath` consistently on catalog paths and `.skin`/shader lookups; synthesize `<dir>.character` entries only for multi-part directories (`lower.md3`/`upper.md3`/`head.md3`) inside `postProcessModelCatalog`.

### Pitfall 8: Quality Gate Rejections (`check_quality.py --strict-tools`)
- **Symptom:** CI fails with `clang-tidy`, `cppcheck`, `lizard`, or `ruff` errors even when the code compiles cleanly.
- **Fix:** Follow `QuakeRaySDK`'s strict static-analysis rules from the start:
  - **`lizard` limits:** Every modified/added C++ function must stay within `CCN <= 15`, `NLOC <= 120`, `params <= 7`. Split complex parsers/UI panels into small single-purpose helpers and factor test helpers cleanly.
  - **`clang-tidy` pitfalls:**
    - Never use `std::system()` (triggers `cert-env33-c`); use `fork`/`execv`/`waitpid` on POSIX (or `CreateProcessW` on Windows).
    - Never mark a local variable `const` if returning it or moving from it can block NRVO/move (`performance-no-automatic-move`).
    - Use `std::lround(...)` instead of `int(x + 0.5f)` (`bugprone-incorrect-roundings`).
  - **`ruff` rules:** Python scripts under `tools/` must pass `ruff check --select=E4,E7,E9,F,B`.

---

## Part 4 — Verification & Quality Gate Checklist

Always run these checks before opening or updating any `QuakeRaySDK` PR or host-engine integration PR:

### 1. QuakeRaySDK Unit Tests & Strict Static Analysis
```bash
# Build SDK + unit tests with compile_commands.json
cmake -S /path/to/QuakeRaySDK -B build/local-quality -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DQR_BUILD_TESTS=ON -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi \
    -DQR_FFX_VK_SO=/path/to/libamd_fidelityfx_vk.so
cmake --build build/local-quality --parallel

# Run all non-GPU unit tests (including qray_editor_multiselect_tests)
ctest --test-dir build/local-quality --output-on-failure -LE gpu

# Run diff-gated quality gate against origin/main
python3 -B tools/check_quality.py --strict-tools --base origin/main \
    --compile-commands build/local-quality/compile_commands.json
```

### 2. Host Engine Build & External Adapter Tests (e.g. `ioq3-rt`)
```bash
# Build host renderer + quakerayeditor + host adapter tests (e.g. qray_q3_editor_adapter_tests)
cmake --build /path/to/host-build --target renderer_qray quakerayeditor qray_q3_editor_adapter_tests --parallel
ctest --test-dir /path/to/host-build --output-on-failure -LE gpu
```

### 3. Live Visual & Session Persistence Smoke Test
Before declaring an adapter or editor integration finished:
1. Launch `quakerayeditor --scene <map> --shaders <shaders>` against real game data (`pak0.pk3`, custom maps like `q3dm17`, `q3dm1`).
2. Verify multi-stage animated surfaces (jump pads, teleporters, banners, grates, flames, chrome) render cleanly with zero black/void cutouts.
3. Verify predefined lights (`q3map_surfacelight` fixtures and entity lights) show cyan selection circles, can be edited/disabled/deleted, survive **Apply** -> map reload (without resurrecting deleted lights), and restore on **Regenerate from map**.
4. Verify the **Models** tab shows assembled characters with `.skin` switching alongside standalone props.
