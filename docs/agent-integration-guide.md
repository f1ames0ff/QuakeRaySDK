# QuakeRaySDK Integration Guide for Agents

> **See also:** [AGENTS.md](../AGENTS.md) | [docs/README.md](README.md) | [embedding.md](embedding.md) | [host-contract.md](host-contract.md) | [spatial-contract.md](spatial-contract.md)

This document is the end-to-end engineering playbook distilled from integrating `QuakeRaySDK` into `ioq3-rt` (id Tech 3 / Quake III Arena), `q2pro-rt` (Quake II), and `xash3d-quakeray` (Half-Life / GoldSrc), together with standalone `quakerayeditor` format adapters. Read it completely before modifying `QuakeRaySDK` or wiring another host engine (Return to Castle Wolfenstein / `iortcw`, Jedi Knight II & Jedi Academy / `OpenJK`, Doom 3 / `dhewm3`, Source, or a custom host).

---

## Architecture, Strict Responsibility Boundaries & Single Shared Scene Pipeline

`QuakeRaySDK` is a source-level Vulkan/NVRHI ray-tracing renderer plus a reusable, engine-neutral scene runtime and authoring shell (`editor/`). It intentionally excludes game-specific map/model/shader parsers, engine VFS implementations, and gameplay simulation.

Crucially, **both `quakerayeditor` and the host game runtime renderer (`renderer_qray`) MUST run through one single shared static scene pipeline (`EditorSceneRuntime`) and one single host format adapter (`EditorFormatAdapter`)**. Never implement parallel BSP loaders, material bakers, DTAL builders, light-cluster uploaders, YAML session readers, or sky/overlay pipelines separately for the editor and the game!

```text
+-------------------------------------------------------------------------+
| Host Engine Repository (e.g. ioq3-rt)                                   |
|                                                                         |
|  1. Single Game Format Adapter (code/rendererqray/editor/)              |
|     - Q3EditorAdapter.*, Q3Bsp.*, Q3Shader.*, Q3Model.*                 |
|     - Compiled into quakerayeditor (QR_EDITOR_ADAPTER_SOURCES)          |
|       AND compiled into renderer_qray (identical static parsing)        |
|                                                                         |
|  2. Thin Runtime Dynamics Layer (code/rendererqray/qray_host.cpp)       |
|     - Hooks live engine VFS via EditorAssetSource::SetVfsCallbacks      |
|     - Calls EditorSceneRuntime for ALL static world / authored state    |
|     - Layers ONLY runtime dynamics on top:                              |
|       * Dynamic MD3 / character entities + ResolveDynamicMaterial       |
|       * Moving inline brush submodels (*1..*N via qrUpdateGeometryTransform)|
|       * Dynamic mesh DTAL via BuildEditorDynamicMeshAreaLights          |
|       * Sprites, beams, decals, smoke, dlights (EditorClusterLightUploadExtras)|
|       * 2D HUD / console / menu quads (SWAPCHAIN)                       |
+------------------------------------+------------------------------------+
                                     |  Calls EditorSceneRuntime & links qray::renderer
                                     v
+-------------------------------------------------------------------------+
| QuakeRaySDK (submodule at e.g. code/quakeraysdk)                        |
|                                                                         |
|  - editor/ Shared Scene Pipeline Layer (NO UI dependencies):            |
|      EditorSceneRuntime.{h,cpp}  | EditorDtalPipeline.{h,cpp}           |
|      EditorOverlayPipeline.{h,cpp} | EditorSession.{h,cpp}              |
|      EditorMaterial.{h,cpp}      | EditorSettings.{h,cpp}               |
|      EditorSpatial.{h,cpp}       | EditorLightSelection.{h,cpp}         |
|      Scene.{h,cpp}               | Assets.{h,cpp} (+ EditorAssetVfsCallbacks)|
|  - editor/ UI Shell: EditorApp, EditorPanels, EditorGui, EditorViewport |
|      (uses the exact same EditorSceneRuntime via m_runtime)             |
|  - Include/qray/{qray.h,host_contract.h,gpu_context.h} + Source/ RT core|
+-------------------------------------------------------------------------+
```

### Decision Matrix: Where Does a Change Belong?

| Change Description | Target Repository | Why |
|---|---|---|
| Ray-tracing pipeline, denoiser, NVRHI pass, public `qray.h` C ABI | **QuakeRaySDK** (`Source/`, `Include/`) | Core SDK responsibility. |
| Static world upload, material overrides/bakes, DTAL clipping/building, light cluster uploads, world overlays, sky/sun upload, `qray.materials.yaml`/`qray.lights.yaml` semantics, meter conversion (`EditorDrawFrame`), generic `quakerayeditor` UI/gizmos | **QuakeRaySDK** (`editor/` shared runtime or UI shell) | Shared once by BOTH `quakerayeditor` and the runtime host renderer so visual output is 1-to-1 by construction. |
| `IBSP` v46 / `.shader` / `.md3` / `.skin` / `.pk3` (`ioq3-rt`), `IBSP` v38 / `.wal` / `.md2` (`q2pro-rt`), or `BSP30` / `.wad` / `StudioMDL` `.mdl` (`xash3d-quakeray`), plus `EditorFormatAdapter` registration | **Host Engine Repo** (single adapter) | Game format knowledge never leaks into `QuakeRaySDK`, and sharing one adapter prevents editor/runtime drift. |
| Host VFS callbacks (`SetVfsCallbacks`), live entity/MD3 transforms, moving brush submodels (`*1..*N`), weapon/explosion `dlights`, particles, decals, 2D HUD | **Host Engine Repo** (`qray_host.cpp`) | Specific to runtime game simulation and client frames. |
| Parallel host-side static BSP/material/DTAL/light/sky pipeline duplicating `EditorSceneRuntime`, or hardcoded game filters (`flame.mdl flame2.mdl`) in `QuakeRaySDK/editor/` | **Rejected everywhere** | Causes silent visual divergence between editor and game and doubles maintenance cost. |

---

## Single Shared Static Scene Pipeline (Mandatory Zero-Duplication Pattern)

Before writing host runtime code (`qray_host.cpp`), understand the exact split introduced to keep `quakerayeditor` and the game renderer **100% identical on every static map element, material, light, DTAL emitter, overlay, and sky**:

### 1. Compile the Exact Same Shared Core into Both Targets
In the host CMake (reference: `ioq3-rt/cmake/renderer_qray.cmake`), compile the UI-free shared modules from `QuakeRaySDK/editor/` **and** your host format adapter sources into BOTH:
1. `quakerayeditor` (via `QR_EDITOR_ADAPTER_SOURCES` / `QR_EDITOR_ADAPTER_INCLUDE_DIRS` — `QuakeRaySDK/editor/CMakeLists.txt` already includes all shared modules), AND
2. Your runtime renderer target (`renderer_qray`) and parity test target (`qray_authored_tests`):
   - `Q3EditorAdapter.cpp`, `Q3Bsp.cpp`, `Q3Model.cpp`, `Q3Shader.cpp` (host adapter)
   - `EditorSession.cpp`, `EditorMaterial.cpp`, `EditorSettings.cpp`, `EditorSpatial.cpp`
   - `EditorOverlayPipeline.cpp`, `EditorDtalPipeline.cpp`, `EditorSceneRuntime.cpp`
   - `EditorLightSelection.cpp`, `Scene.cpp`, `Assets.cpp`

Notice that none of those shared runtime files include Dear ImGui, X11, or Win32 windowing — only `EditorApp` / `EditorPanels` / `EditorGui` / `EditorUi` / `EditorPlatform*` depend on GUI libraries.

### 2. Hook Live Engine VFS Instead of Writing a Second Asset Loader
At map init, mount game/mod directories into `EditorAssetSource` **and** bind the host engine's VFS via `EditorAssetSource::SetVfsCallbacks(EditorAssetVfsCallbacks)` (`exists`, `readFile`, `listFiles`):
- `quakerayeditor` resolves assets from loose directories + `.pak`/`.pk3` archives.
- The game runtime (`renderer_qray`) automatically falls back to the active engine VFS (`FS_ReadFile` / pack order / pure checks) through the exact same `EditorAssetSource` API without duplicating any loader logic.

### 3. Call Sequence Shared by Editor & Runtime (`EditorSceneRuntime`)

Both `EditorApp` (`m_runtime`) and `qray_host.cpp` (`sceneRuntime`) drive map loading and per-frame scene uploads through the same methods on `EditorSceneRuntime`:

```cpp
// --- Map Load (Identical sequence in EditorApp and qray_host.cpp) ---
sceneRuntime.DestroyGpuResources(qrayInstance);

EditorSceneLoadOptions loadOpts;
loadOpts.assets = &gameAssets;
// Editor uses default (true) to bake doors/platforms statically;
// Runtime sets false because the server updates *1..*N brush submodels dynamically.
loadOpts.includeInlineSubmodels = false;
sceneRuntime.LoadBspSceneMemory(bspBytes, mapName, nullptr, loadOpts, error);

sceneRuntime.materials.Rebuild(sceneRuntime.scene);
sceneRuntime.settings = EditorRenderSettings{};
sceneRuntime.sun = EditorSunState{};
sceneRuntime.ApplySceneRenderDefaults();

// Authored qray.materials.yaml / *.cfg / qray.lights.yaml overrides:
sceneRuntime.ApplyMaterialsDocuments(activeMaterialsDoc, &baseMaterialsDoc);
sceneRuntime.ApplyConfigText(cfgText);
sceneRuntime.ApplyLevelDocument(activeLevelDoc, levelKey);

sceneRuntime.CreateTextureMaterials(qrayInstance, gameAssets, logFn);
sceneRuntime.UploadStaticGeometry(qrayInstance, gameAssets, fallbackMaterial, logFn);
// Note: UploadStaticGeometry triggers BuildEmissiveLights(...) cleanly inside EditorSceneRuntime.
sceneRuntime.UploadWorldClusters(qrayInstance, logFn);
sceneRuntime.CreateSkyboxMaterials(qrayInstance, gameAssets, logFn);
sceneRuntime.CreateScrollingSkyMaterials(qrayInstance, gameAssets, logFn);
```

```cpp
// --- Per-Frame (Static + Sky/Overlays shared; Runtime layers dynamics on top) ---
sceneRuntime.UploadSun(qrayInstance, logFn);
sceneRuntime.UploadLights(qrayInstance, timeSeconds, /*animateLightStyles=*/true, logFn);
sceneRuntime.UploadEmissiveLights(qrayInstance, timeSecondsF, &frameDynamicEmissiveLights, logFn);

EditorClusterLightUploadExtras extras;
extras.extraQuakeUnitSources = &dynamicDlightClusterSources;
extras.extraDynamicEmissiveLights = &frameDynamicEmissiveLights;
sceneRuntime.UploadClusterLightSources(qrayInstance, &extras, logFn);

sceneRuntime.UploadWorldOverlays(qrayInstance, gameAssets, camBasis, timeSecondsF);
// ... host uploads moving *1..*N submodels, MD3 models, sprites, beams, decals, smoke ...
sceneRuntime.UploadSky(qrayInstance, viewerPos, timeSeconds, skyDistance);

sceneRuntime.PopulateDrawFrame(qrayInstance, viewerPos, width, height, frameParams, drawInfo);
EditorDrawFrame(qrayInstance, &drawInfo); // One unified Quake-unit -> meters boundary
```

### 4. What Stays Exclusive to the Game Runtime Layer
The host renderer (`qray_host.cpp`) must **only** own runtime-specific dynamics that do not exist in a static map preview:
- Moving inline brush submodels (`*1..*N` doors, platforms, trains, rotators updated via `qrUpdateGeometryTransform`).
- Live animated entities (`RT_MODEL` `.md3` characters/weapons/projectiles): resolve authored material overrides through `sceneRuntime.ResolveDynamicMaterial(...)` and build optional dynamic DTAL emitters through `BuildEditorDynamicMeshAreaLights(...)` so even dynamic models honor `qray.materials.yaml` identically to static brushes!
- Entities without static presence: sprites (`RT_SPRITE`), beams (`RT_BEAM` / `RT_RAIL_CORE` / `RT_LIGHTNING`), bullet marks/decals, procedural smoke, dynamic weapon/explosion `dlights` passed via `EditorClusterLightUploadExtras`, and 2D `SWAPCHAIN` HUD/console/menus.

> **Hard Rule for Future Agents:** If you fix a bug or add a feature in BSP loading, material baking (`EditorMaterial`), DTAL generation (`EditorDtalPipeline`), shader overlays (`EditorOverlayPipeline`), light persistence (`EditorSession`), sky/sun, or render settings (`EditorSettings`), **put it in the shared layer in `QuakeRaySDK/editor/` or the single host adapter**. Never patch it only in `EditorApp` or only in `qray_host.cpp`.

---

## Part 1 — Connecting & Embedding QuakeRaySDK in a Host Engine (Runtime)

### 1.1 Submodule & CMake Setup

Mount `QuakeRaySDK` inside the host repository (for example as a submodule at `code/quakeraysdk`) and wire it in CMake (reference pattern: `ioq3-rt/cmake/renderer_qray.cmake`):

1. **Patch NVRHI:** Stock NVRHI limits `c_MaxBindingLayouts` to 8; QuakeRay's RT pipelines require at least 13. Apply `cmake/nvrhi-max-binding-layouts.patch` (which raises `c_MaxBindingLayouts` to 16) before building `nvrhi` and `nvrhi_vk`:
   ```bash
   git -C /path/to/nvrhi apply /path/to/QuakeRaySDK/cmake/nvrhi-max-binding-layouts.patch
   ```
2. **Build FidelityFX Provider:** Build `libamd_fidelityfx_vk.so` (Linux) via `Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh` or `amd_fidelityfx_vk.dll` (Windows) via `Source/FidelityFX_SDK_1.1.4/windows/build_provider.ps1`, and pass `-DQR_FFX_VK_SO=...` / `-DQR_FFX_VK_RUNTIME_DLL=...`. Whenever any FidelityFX header under `Source/FidelityFX_SDK_1.1.4/` changes, rebuild this provider shared library and redeploy it beside both binaries—recompiling only the C++ host or renderer does not update it.
3. **Preserve PIC and Disable Cross-Target IPO:** When linking `qray::renderer` into a dynamically loaded renderer module (like `renderer_qray.so` / `renderer_qray.dll`), enable `CMAKE_POSITION_INDEPENDENT_CODE ON` and temporarily set `CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF` around `nvrhi` and `QuakeRaySDK`.
4. **Configure Editor Adapter & AI Worker (when `QR_BUILD_EDITOR=ON`):** Set `QR_EDITOR_ADAPTER_SOURCES`, `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and `QR_EDITOR_SHADER_DIR` (and optionally `-DQR_GGML_SOURCE_DIR=/path/to/ggml` to build the isolated `quakeray_ai_worker` executable linked statically against `ggml-vulkan` + `ggml-cpu`) before `add_subdirectory("${QUAKERAY_SDK_DIR}" ...)` so `quakerayeditor` compiles cleanly without putting game-specific files inside `QuakeRaySDK`.
5. **Deploy Bundled Runtime Assets Next to Binaries:** `BlueNoise_LDR_RGBA_128.png` and `WaterNormal_n.png` are bundled under `QuakeRaySDK/Assets/textures/`, with automatic per-file fallback in `CMakeLists.txt` (`QR_TEST_TEXTURE_DIR`) and `editor/CMakeLists.txt` (alongside `fonts/Roboto-Regular.ttf` and `fonts/fa-solid-900.ttf`). You no longer need an external `../gfx/textures` sibling layout; ensure `shaders/`, `textures/`, `fonts/`, `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`, and (if enabled) `quakeray_ai_worker` sit beside your executables. (For game builds distributing engine packs, include `BlueNoise_LDR_RGBA_128.png` and `WaterNormal_n.png` inside `qray.pkz` / mounted game data as well.)

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
| **Multi-Stage Overlays, Stage Toggles & Mirror Promotion** | `qrUploadRasterizedGeometry` + `EditorMaterialParams` (`disabledStagesMask`, `mirror`) | Use `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT` for world animated/additive/blended passes, `..._SKY` for sky passes, and `..._SWAPCHAIN` for 2D HUD/console/menu quads. `disabledStagesMask` (`disabled_stages` in `qray.materials.yaml`) disables individual shader stages across both `EditorOverlayPipeline` and `EditorSceneRuntime`; `mirror = true` promotes cubemap (`tcGen environment`) / `isOverlayOnly` surfaces into traced mirror surfaces. |
| **Animated Material Frames** | `qrCreateAnimatedMaterial` / `qrChangeAnimatedMaterialFrame` | Use SDK animated materials or dynamic per-frame texture selection for `animMap` stages. |
| **Sky, Sun & Volumetric Light Shafts** | `QR_GEOMETRY_VISIBILITY_TYPE_SKY` + procedural sky + `RhiLightShaftVolumePass` | Mark sky surfaces so primary rays read the procedural AAA sky/celestial system (`docs/sky-and-atmosphere.md`) or cubemap/sun without blocking geometry. Froxel volumetric light shafts use depth-bounds culling (`CmLightShaftDepthBounds`), tightened froxel grids (`128x72x64` / `192x108x96` / `256x144x128`), and stochastic multi-light selection. |
| **Relief POM, PBR Section Toggles & Embedded Masks** | `texture_height` (`EditorTextureSlot::Height`), `parallax_scale` (`layerColors[2].data[2]`), `EditorMaterialParams` (`emissiveEnabled`, `glossEnabled`, `sssEnabled`) | `setupParallaxGeometry` / `computeParallaxOcclusionPrimary` / `computeParallaxOcclusionRefl` / `computeParallaxCavityAO` (`Source/Shaders/HitInfo.hlsli`) run grazing-fade Relief POM and cavity micro-AO (auto-bypassed on glass, water, SSS, and overlay alpha surfaces). `ResolveBakedNormalAlpha` packs `Normal.a` in strict priority: active SSS mask (`sss_enabled` on non-glass) -> glass `transparency` -> `texture_height` luminance -> `255`. Master switches (`emissive_enabled`, `gloss_enabled`, `sss_enabled`) mute sections in YAML without destroying authored masks, and `ApplyEmbeddedPbrMaskSample` / `SampleBakeNormal` preserve embedded `EditorTexture::roughnessMask` / `metallicMask` on normal-mapped `envMap` surfaces. |
| **Specular Anti-Firefly & Normal Detail** | `q2RegularizeDemodulatedSpecular`, `q2AntiFirefly`, Toksvig/LEADR sub-pixel regularization (`BRDF.hlsli`, `CmQ2Adapter.comp.hlsl`, `HitInfo.hlsli`) | Direct and indirect specular passes apply a path-space soft-knee limiter on rough dielectrics (`q2RegularizeDemodulatedSpecular`), `CmQ2Adapter` clamps diffuse fireflies (`q2AntiFirefly`) and sharpens temporal/atrous variance filtering, and `HitInfo.hlsli` limits bump-to-roughness regularization to sub-pixel minification so close-range normal maps stay sharp. Hosts must keep physically accurate `minRoughness` and `normalMapStrength` instead of washing out close-up materials. |
| **HiDPI Inspector Layout Scaling** | `EditorGuiScale`, `EditorGuiFitButtonWidth`, `EditorGuiComboWidth`, `EditorGuiButtonRow` (`editor/EditorGui.h`) | Editor controls scale uniformly across 1080p (`1.0x`) and 4K (`1.5x`) viewports so combos, checkboxes, sliders, modal prompts, and action rows never truncate or overlap. |
| **Transparency Policy** | `EditorMaterialType::Glass`, `alphaTested`, `SSS` | Use physical Glass, binary `alphaTested` cutouts, or subsurface scattering (`SSS`). Do **not** inject stochastic per-pixel alpha transparency masks into primary/secondary rays (previously prototyped and removed due to extreme path-tracing noise). |

### 1.5 Runtime Decals, Sprites, Translucent FX, Particles, Smooth Model Normals, & HDR Overlay Blending

Every classic engine (`ioq3-rt`, `q2pro-rt`, `xash3d-quakeray`) hits the same visual traps when routing combat effects, decals, pickups, and dynamic entities through a physically based HDR path tracer. Follow these seven rules verbatim:

1. **Never Send Translucent Sprites, Beams, or Explosion Models to `qrUploadGeometry`:**
   - `qrUploadGeometry` is strictly for ray-traced opaque geometry, binary `ALPHA_TESTED` cutouts (`alpha >= 0.5`), mirrors, and physical dielectric refraction (`GLASS_REFLECT_REFRACT` / `WATER`).
   - Submitting sprite sheets (`.sp2`, `.spr`, Q3 sprites), beams/lasers, muzzle flashes, or translucent explosion models (`RF_TRANSLUCENT`, `r_explode`, `bulletExplosion`, `rocketExplosion`) to `qrUploadGeometry` turns sprites into shadow-casting black/cutout cards and turns explosions/beams into dark refractive glass blocks.
   - Upload all sprites, beams, translucent FX meshes, decals, and additive shells via **`qrUploadRasterizedGeometry`** (`QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT`) with `QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST` and explicit `blendFuncSrc` / `blendFuncDst`. Upload point particles via **`qrUploadParticles`** with the same blend/depth flags.

2. **Keep Smooth Vertex Normals on Dynamic Models (`.md2`, `.md3`, `.mdl`, `.mdc`) & Curved BSP Geometry:**
   - Passing `QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT` or `QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT` forces flat per-triangle cross-product normals in `Source/Shaders/HitInfo.hlsli` / `Source/Shaders/VertexPreprocessPartial.hlsli`, making characters, weapons, pickups, and statues look like faceted ice/glass figurines.
   - Pass **`flags = 0`** on dynamic character/weapon models (`UploadDynamicGeometry` in `qray_host.cpp`) and curved/mesh BSP batches (`MST_PATCH`, `MST_TRIANGLE_SOUP`) and supply smooth normalized vertex normals in `QrVertex::normal` so primary/reflection rays interpolate smoothly across triangles.

3. **Impact Decals & Scorch Marks (Darken vs Energy Marks, Vertex Fade, Offset, & `cameraNear`):**
   - **Darken / Multiplicative Decals (`GL_ZERO / GL_ONE_MINUS_SRC_COLOR`, `GL_DST_COLOR / GL_ZERO`):** Upload the **untouched raw RGBA texture** with its native blend factors through `qrUploadRasterizedGeometry` (do **not** convert it to luminance-alpha or multiply it by exposure). `ComputeOverlayBlendFlags` (`Source/RHI/RhiRasterOverlayPass.cpp`) automatically sets `blendFlags` bit `0` (disabling `sceneExposure` multiplication in `RsWorld.frag.hlsl` so dimensionless `[0, 1]` transmittance factors never crush to black) and bit `1` (preserving perceptual sRGB attenuation for `ZERO / ONE_MINUS_SRC_COLOR` in linear HDR). Preserve `exactVertex` / time-faded vertex colors; only substitute white `(255, 255, 255)` for `rgbGen identity` or legacy `(0, 0, 0)` RGB with `alpha > 0`.
   - **Energy Scorch Marks (`plasma_mrk`, blaster/BFG marks):** Colored dark-fringed glow decals drawn with `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` in linear HDR subtract wall radiance around the border, creating a dark black ring around a blown-out core. Blend energy marks additively with `QR_BLEND_FACTOR_SRC_ALPHA` / `QR_BLEND_FACTOR_ONE` (e.g. `~0.85` linear scale).
   - **Surface Offset & `cameraNear` Precision:** Push coplanar surface decals outward `+0.25` Quake units (`0.00625 m`) along the receiver face normal (`-0.8` / `-1.6` units for `cutoutFrameUnderlay` / solid backing). Set `QrDrawFrameInfo::cameraNear` to **`1.0–3.0` Quake units** (`0.025–0.075 m`, FOV-clamped so first-person weapons never clip)—never `0.1` units, which drops ~`30x` depth precision (`~1` float32 ULP at 500 units) and lets decals/underlays bleed through solid walls.

4. **Strict Overlay Submission Order (Surface Decals BEFORE Explosions, Sprites, & Smoke):**
   - Upload surface decals/scorch marks **before** dynamic models, explosion sprites, beams, and smoke. Because `RhiRasterOverlayPass` records world draws in upload order with `LessOrEqual` depth against world walls, submitting a darken decal (`burn_med_mrk`) *after* an active fireball (`rocketExplosion`) passes depth at the center and multiplies a pitch-black hole right through the explosion. (Note: `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` and `QR_RASTERIZED_GEOMETRY_STATE_SMOKE` run in dedicated sorted sub-passes after `DEFAULT` world draws; see `Include/qray/qray.h`.)

5. **32-Bit Opaque-Alpha Effect Textures, Black Noise Cleaning, & Missing Projectiles:**
   - Retail `.tga` / `.pcx` / `.wal` sprite sheets often save 4 RGBA channels where **every texel has `alpha == 255`** over a black background. Choosing `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` purely from `channels == 4` draws a **solid black quad** around fireballs, flames, and lasers. Scan pixels on load (`any(alpha < 250)`): if no texel is transparent on an effect sprite, blend additively (`ONE / ONE`).
   - Apply near-black compression cleanup (`CleanAdditiveBlackBackground`: zero `peak <= 6`, smooth linear ramp `6..18`) to flat 2D additive sprites and world effects (`isAdditive && !environmentMap`) so JPEG/DXT/palette noise does not ignite in HDR—**never** apply it to spherical `tcGen environment` reflection textures (`tinfx2b`, `envmapgold2`, `envmaprail`), or translucent pickup sphere gradients disappear.
   - Skip `"gfx/colors/black"` frames in `animMap` sequences, and substitute `255` when an `rgbGen identity` sprite entity passes all-zero `(0, 0, 0, 0)` `shaderRGBA` so flying energy projectiles remain visible.

6. **HDR Overlay Exposure Contract, Additive Shells, `deformVertexes`, & CPU Backface Culling:**
   - `RsWorld.frag.hlsl` already scales non-transmittance overlay draws by `sceneExposure = adaptedLum * exp2(2.0 - tmExposureBias)`. **Never dim 8-bit sRGB textures on the CPU before upload** (multiplying an sRGB shell image by `0.1` before `VK_FORMAT_R8G8B8A8_SRGB` decode crushes linear radiance to `0.1^2.2 ≈ 0.0063`, erasing health spheres and armor-shard shells). Tune brightness in linear float `QrRasterizedGeometryUploadInfo::color` (`~1.0–1.5x`).
   - Evaluate `deformVertexes wave` displacements in entity-local space along `localNormal` before applying `entity.axis` (required for `item_armor_shard` green energy shells to expand `+2.0` units outward cleanly).
   - Because `RhiRasterOverlayPass` uses `RasterCullMode::None`, **cull back-facing triangles on the CPU** (`dot((p1 - p0) x (p2 - p0), p0 - vieworg) < 0` when `!twoSided`) on closed 3D model overlays so front and back hemispheres do not double-add; pass `QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE` on opaque `ONE / ZERO` pickup cores so 3D geometry self-occludes cleanly.

7. **Lit Smoke (`QR_RASTERIZED_GEOMETRY_STATE_PARTICLE`) & Camera-Left Billboard UVs:**
   - Tag smoke sprites/puffs with `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` (plus `smokeLook` weighting) so they receive scene lighting instead of rendering as flat unlit grey pancakes, and compute animation age relative to entity spawn time (`max(0, sceneTime - entityShaderTime)`).
   - In Quake-family axes (`+X` forward, **`+Y` left**, `+Z` up), `viewaxis[1]` points **camera-left**: map `+left` (`-right`) to `u = 0` and `-left` (`+right`) to `u = 1` with CCW winding (`{0, 1, 2, 0, 2, 3}`), otherwise floating numbers (`"1"` score plums) and asymmetric sprites render horizontally mirrored.

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
2. **Tessellate Curved Surfaces (`MST_PATCH`):** Convert biquadratic Bezier 3x3 control patches into indexed triangles (`EditorSurface`), computing smooth vertex normals and continuous UVs so curved archways, domes, and pipes reflect and trace accurately. Never validate patch winding using a global sum of radial vertex normals: on closed 360-degree cylinders/columns (e.g., `q3dm1` pillars and `q3dm2` arches), symmetric normals cancel to `(0, 0, 0)` and rounding noise flips triangles inside-out under backface culling—validate winding locally per quad/triangle against authored normals.
3. **Preserve `faces` on `EditorSurface`:** Populate `EditorSurfaceFace` (`normal`, `cluster`, `positions`, `texCoords`) in addition to the merged `vertices`/`indices`. The editor uses `faces` for triangle/surface picking, cluster resolution, and DTAL area-light bounds.
4. **Skip Invisible Utility Surfaces, Keep Inline Map Submodels:**
   - Exclude compiler-only brush surfaces (`nodraw`, `clip`, `playerclip`, `hint`, `skip`, `areaportal`, `origin`, `caulk` where appropriate, unless meant for sky).
   - Include world model (`models[0]`) **and** visible inline brush models (`models[1..N]`), excluding only submodels owned by explicit hidden/excluded brush entities if needed.
5. **World Clusters, RLE PVS Compression & Sky Portal Filtering (`scene.worldClusters` + `scene.nodes`):**
   - Build `EditorBspNode` planes and `EditorWorldClusters` (`mins`, `maxs`, `flags`, `contents`, `leafCluster`, `visData`, `visOffsets`, `skyVis`, `pvsRowBytes`, `numLeafs`) with solid/outside mapped to `0` and leaf clusters offset by `+1`.
   - When supplying explicit `visData` / `visOffsets` (non-null `pVisData`), `Source/WorldLights.cpp` reads **Quake zero-run RLE compressed** cluster rows (`visUncompressed = false`). For engines whose BSP PVS rows are unshifted or cluster-offset differently (`IBSP` v38 in Quake II, `BSP30` in Half-Life / GoldSrc where leaf PVS references 0-based clusters/leaves), decompress each row into a cluster-shifted bitset (`c + 1`), then re-encode via RLE (`AppendRleCompressedPvsRow` / `AppendCompressedVisRow`) and trail `visData` with `pvsRowBytes + 2` `0xFF` padding bytes so boundary reads never truncate. Passing `pVisData = nullptr` falls back to `visUncompressed = true` (all clusters mutually visible).
   - Keep `SURF_SKY` brushes/portals excluded from solid opaque BVH occlusion (mark them `QR_GEOMETRY_VISIBILITY_TYPE_SKY` and filter them out of opaque surface light extraction even when combined with `SURF_NODRAW`) so sky views and outdoor directional sunlight are never sealed shut.
   - Use solid-leaf boundary nudging when assigning per-vertex / per-face clusters and light clusters (`ResolveEditorLightCluster` / adapter helper).
6. **Consistent Winding & Multi-Engine Geometry (`IBSP` v38/v46, `BSP30`, `.md2`, `.md3`, `.mdl`):**
   - Flip Quake-family clockwise BSP faces consistently (in `IBSP` v38, reverse triangle fans `i0, i+1, i`) and verify alias/skeletal model winding (`.md2`, `.md3`, GoldSrc `StudioMDL` `.mdl`) matches the host convention configured via `qrSetSpatialConvention` so backface culling (`qray_cullBackFaces`) never turns walls or characters inside-out.
   - For GoldSrc (`BSP30` / `.wad`), decode `{`-prefixed palette textures as binary `ALPHA_TESTED` cutouts keying out palette index `255` (gratings, ladders, fences), and convert `lights.rad` texture irradiance definitions into physical `EditorLight` / area-light emitters.
7. **Map Entities & Camera Spawn:** Parse the entities lump for spawn priority (`info_player_deathmatch`, `info_player_start`, `info_player_coop`, `info_player_intermission`) to populate `scene.cameraPosition` and `scene.cameraYawDegrees`, plus `misc_model` static props and sky/sun hints (`hasSun`, `sunColor`, `sunIntensity`, `sunDegrees`, `sunElevation`).

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

### 2.7 Isolated Native Vulkan AI Texture Worker (`ggml-vulkan`) & 4-Tab PBR Pipeline

The editor's local `Generate with AI` pipeline (`EditorAiDialog`, `EditorAiTexture`, `editor/ai_worker/`) exposes a 4-tab PBR authoring flow (**Upscale** via `4x-PBRify_RPLKSRd_V3` [`RealPlksrPipeline`], **De-lighting** via `StableDelight` [`LotusPipeline`], **Normals** via `DeepBump` [`DeepBumpPipeline`], and **Depth** via `Lotus Depth v2` [`LotusPipeline`]) and must never run heavy neural inference inside `quakerayeditor` or via ONNX Runtime / WebGPU (Dawn) (ONNX Runtime removed `ROCmExecutionProvider` in v1.23+, and Dawn WebGPU lacks strict VRAM caps):

1. **Out-of-Process Child Worker (`quakeray_ai_worker`):**
   - `editor/EditorAiDialog.cpp` invokes `quakeray_ai_worker` (`editor/ai_worker/`) via `EditorAiWorkerClient` and `EditorAiWorkerProtocol` (binary NCHW tensor temp files + line-delimited `PROGRESS`, `VRAM`, `PROVIDER`, `DONE`, `ERROR` messages on stdout).
   - Process isolation guarantees 100% VRAM and host RAM reclamation on exit, instantaneous `Cancel` via child termination (`CancelActiveEditorAiWorker`), and survival of `quakerayeditor` with unsaved edits preserved if a driver reset or worker failure occurs.
   - `EditorAiModelDownload` provides a built-in native HTTPS GGUF downloader (libcurl on Linux, WinHTTP on Windows) pointing at dedicated HuggingFace repositories with `.part` staging and streaming SHA-256 verification.
2. **4-Tab PBR Workflow & Upstream Linking:**
   - **Upscale (`4x-PBRify_RPLKSRd_V3`):** Produces `2x`/`4x` super-resolution albedo with detail/blend strength and sharpening controls, plus upstream `Use Upscaled diffuse` wiring into downstream tabs.
   - **De-lighting (`StableDelight`):** Strips baked directional shading and specular highlights cleanly into a flat de-lit diffuse map.
   - **Normals (`DeepBump`):** Fixes tile border centering/mean subtraction and supports `Use De-lit diffuse` so generated normal maps match the de-lit surface.
   - **Depth (`Lotus Depth v2`):** Outputs depth/height feeding `texture_height` (Relief POM), derived normal maps, or gloss/roughness maps. Uses wide wrap/mirror context (`PrepareEditorLotusDepthInputNchw`) and halo-only bilinear tilt removal (`ExtractDetrendedEditorLotusDepthHeight` / `SolveHaloTiltCoefs`) so low-contrast or de-lit inputs preserve interior relief without border washouts.
3. **Native `ggml-vulkan` Execution, FlashAttention, & Memory Discipline:**
   - Models run in unquantized **FP16 GGUF v3** (`tools/export_ai_gguf.py` producing `*-fp16.gguf`) on `ggml-vulkan` with automatic `ggml-cpu` fallback.
   - `GgmlOps::ExactMultiHeadAttention` uses `ggml_flash_attn_ext` with `F16` K/V casts and `GGML_PREC_F32` accumulation when `(dHead % 8) == 0 && dHead >= 8`, avoiding multi-gigabyte $O(N^2)$ attention matrices so `LotusDepth` and `StableDelight` run single-tile (`1/1`) even at `1024 px` within a 16 GB VRAM budget.
   - Diffusion pipelines execute in sequential memory-isolated stages (`VAE Encode -> unload -> ControlNet/UNet -> unload -> VAE Decode -> cleanup`), query `VK_EXT_memory_budget` (`min(0.75 * totalVRAM, freeVRAM - safetyReserve)`), pre-measure exact `ggml` graph buffer bytes before allocation, and apply automatic tiling (or CPU fallback) whenever a stage exceeds budget.
4. **Strict Manual UX Trigger:**
   - Inference must start **only** when the user clicks **Generate** / **Regenerate**. Never auto-run generation on opening the dialog, changing tabs/combos, or finishing a model download. Display live percentage progress (`0–100%`) and stage status reported by the worker.

---

## Part 3 — Graveyard of Pitfalls (What Broke and How to Avoid It)

Every item below happened in real debugging sessions during the Quake III (`ioq3-rt`) integration. Treat each as a hard invariant.

### Pitfall 1: Duplicating Static Scene, Material, DTAL, or Lighting Pipelines Between Editor and Runtime
- **Symptom:** The editor (`quakerayeditor`) and the live game (`renderer_qray`) diverge on the exact same map: different DTAL area lights, missing material bakes, missing overlays/cutouts, different sky/sun defaults, or a bug fixed in one target while still broken in the other.
- **Root Cause:** Building a separate host-side loader/pipeline (e.g. parallel `qray_authored.{h,cpp}` logic parsing `qray.materials.yaml` / `qray.lights.yaml`, building DTAL, or uploading static BSP geometry separately) instead of reusing the editor's pipeline in one shared layer.
- **Fix:** Never duplicate static world pipelines. Extract UI-free shared logic into `QuakeRaySDK/editor/` (`EditorSceneRuntime`, `EditorDtalPipeline`, `EditorOverlayPipeline`, `EditorSession`, `EditorMaterial`, `EditorSettings`, `EditorSpatial`), compile that shared core + the single host `EditorFormatAdapter` into **both** executables, bridge live game VFS with `EditorAssetSource::SetVfsCallbacks`, set `includeInlineSubmodels = false` for runtime moving brush submodels, and enforce 1-to-1 parity via an automated test (`qray_authored_tests` / `TestSteamQ3Tourney1Parity`).

### Pitfall 2: Mixing Adapter Code into `QuakeRaySDK`
- **Symptom:** Game-specific loaders (`Q3Bsp.cpp`, `Q3Shader.cpp`, `Q3Model.cpp`, `Q3EditorAdapter.cpp`) get committed inside `QuakeRaySDK/editor/`, polluting SDK PRs with Quake 3 internals.
- **Root Cause:** Treating `QuakeRaySDK/editor/` as the owner of every game's asset format instead of keeping only the generic adapter hook (`EditorFormatAdapter`) in SDK.
- **Fix:** Keep all game-specific loaders in the host repo (`ioq3-rt/code/rendererqray/editor/`), build them via `QR_EDITOR_ADAPTER_SOURCES` / `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and put only generic SDK/UI features and UI-free shared scene runtime code in `QuakeRaySDK` PRs.

### Pitfall 3: Predefined Surface Lights Without Selection Circles in `Light Editor`
- **Symptom:** Lamps, ceiling panels, torches, and jump pads cast real-time ray-traced light in `QuakeRay`, yet have **no cyan selection circle**, do not appear under `Light Editor -> Entity`, and cannot be turned off or deleted.
- **Root Cause:** Adapter ignored `q3map_surfacelight` brushes or excluded traced surfaces from `scene.lights`, leaving them to DTAL / `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` (`BuildEmissiveLights`). DTAL area lights generated purely from textures do not create `EditorLight` gizmos/entities.
- **Fix:** Extract **all** predefined map emitters (map entities **and** all `q3map_surfacelight > 0` surfaces/fixtures, clustered per fixture) into `scene.lights` and `scene.bspLights` with valid `className` and cluster, and avoid auto-emitting hidden DTAL/ray-only surface light duplicates when `usesExternalLightingModel` is true.

### Pitfall 4: Hardcoded Model Filters & Deleted Standard Lights Returning on Reload
- **Symptom:** Standard map lights were filtered via hardcoded strings like `"flame.mdl flame2.mdl"` into `qray.materials.yaml`, OR deleting all standard lights in `Light Editor` caused every light to reappear on the next map load.
- **Root Cause:** Filtering by engine-specific model names breaks general engines (Quake 2/Quake 3), and omitting the `lights:` YAML block when `m_scene.lights` is empty makes the loader fall back to BSP defaults instead of honoring user deletions.
- **Fix:** Never hardcode game model names in `QuakeRaySDK/editor/`. Store all predefined map lights in `scene.lights` / `scene.bspLights`, serialize empty standard lights explicitly as `lights: []` in `qray.lights.yaml`, and reserve BSP reset strictly for explicit `Regenerate from map`.

### Pitfall 5: Multi-Stage Shaders Rendering Black, Double-Dark, or Leaking Sky
- **Symptom:** Launch pads, teleporters, animated monitors, grates, banners, or flames appear black, show opaque black rectangles instead of glow, or show sky through cutout holes.
- **Root Cause:** Flattening multi-stage shaders to stage 0, keeping lightmap `GL_DST_COLOR GL_ZERO` multiply stages after RT replaced lightmaps, failing to clean black fringes on additive maps (`CleanAdditiveBlackBackground`), or tracing binary alpha cutouts without a backing layer.
- **Fix:** Separate traced base (`EditorSurface`) from animated/blended overlay stages (`EditorOverlaySurface`), strip lightmap/redundant `DST_COLOR ZERO` stages, offset cutout-underlay backing meshes inward with `OffsetEditorVerticesAlongSmoothNormals`, and use `autoSprite`/`autoSprite2` quads where appropriate.

### Pitfall 6: `std::__ios_failure: Is a directory` Crash on Bare Texture Names
- **Symptom:** Crash with `basic_filebuf::underflow error reading the file: Is a directory` when resolving a shader/material named e.g. `default` or a directory stem.
- **Root Cause:** On Linux/POSIX, `std::ifstream` succeeds in *opening* a directory path (such as `./textures/default/`), and only throws when attempting to read bytes from it!
- **Fix:** Always verify `std::error_code ec; std::filesystem::is_regular_file(candidate, ec)` before opening or reading any candidate path on disk (in `Assets.cpp`, `EditorApp.cpp`, and any host adapter helper).

### Pitfall 7: Stale `EditorMaterialBakes` GPU Cache or World Bake Wipe After In-Place Texture Edits
- **Symptom:** Either clicking `Generate with AI` / painting procedural masks leaves the viewport showing the old flat surface until restarting, OR editing a model texture in the `Models` preview tab wipes every unrelated surface on the level white.
- **Root Cause:** `EditorMaterialBakes` caches baked `QrMaterial` handles keyed by texture index (`int32_t albedo/normal/gloss/emissive`) and scalar keys. Mutating an existing `EditorTexture` in place does not change its index (leaving stale bakes active), whereas calling `m_materialBakes.Destroy(m_instance)` across the entire table destroys live `QrMaterial` handles still bound to static world geometry.
- **Fix:** When replacing or updating a texture in place (`RegisterOrUpdateGeneratedTexture`), destroy and recreate only that slot's pool `QrMaterial` (`EditorSceneRuntime::CreatePoolMaterial`), call targeted `InvalidateTexture(m_instance, existing)` on both `m_materialBakes` and `m_previewMaterialBakes` (never global `Destroy()` while static geometry is live), and mark `m_materialsChanged = true` only if a scene surface actually references that texture index.

### Pitfall 8: Broken Multi-Part Models & `./` Path Mismatches in `Models` Tab
- **Symptom:** Quake 3 characters show detached torsos/heads, missing skins, or fail to show up in the `Models` browser.
- **Root Cause:** Zip/PK3 entries or loose filesystem scans with `./models/players/...`, backslash separators, or mixed case miss exact string comparisons, and hiding all `.md3` entries breaks standalone weapon/prop previews.
- **Fix:** Run `NormalizeEditorAssetPath` consistently on catalog paths and `.skin`/shader lookups; synthesize `<dir>.character` entries only for multi-part directories (`lower.md3`/`upper.md3`/`head.md3`) inside `postProcessModelCatalog`.

### Pitfall 9: Quality Gate Rejections (`check_quality.py --strict-tools` & `check_local.sh`)
- **Symptom:** CI fails with missing copyright headers, `clang-tidy`, `cppcheck`, `lizard`, `ruff`, or ASan/UBSan test errors even when a Release build compiles cleanly.
- **Fix:** Follow `QuakeRaySDK`'s strict quality rules from the start and run `tools/check_local.sh --asan`:
  - **GPL copyright headers:** Run `python3 -B tools/copyright_header.py --fix <files>` on every newly added C/C++/HLSL/Python/shell file.
  - **`lizard` limits:** Every modified/added C++ function must stay within `CCN <= 15`, `NLOC <= 120`, `params <= 7`. Split complex parsers/UI panels into small single-purpose helpers and factor test helpers cleanly.
  - **`clang-tidy` pitfalls:**
    - Never use `std::system()` (triggers `cert-env33-c`); use `fork`/`execv`/`waitpid` on POSIX (or `CreateProcessW` on Windows).
    - Never mark a local variable `const` if returning it or moving from it can block NRVO/move (`performance-no-automatic-move`).
    - Never keep a redundant default member initializer in a struct/class aggregate if member construction or clang-tidy flags it; verify with `--compile-commands`.
    - Use `std::lround(...)` instead of `int(x + 0.5f)` (`bugprone-incorrect-roundings`).
  - **`ruff` rules:** Python scripts under `tools/` must pass `ruff check --select=E4,E7,E9,F,B`.

### Pitfall 10: Closed 360° Bezier Patches Flipping Inside-Out & Truncated World Ray Length
- **Symptom:** Cylindrical pillars and arched corridors (`q3dm1` towers, `q3dm2` arches) turn invisible from the outside and look X-rayed from behind, or large arena vistas clip halfway across the map.
- **Root Cause:** Summing radial vertex normals over a symmetric 360-degree `MST_PATCH` produces `(0, 0, 0)`, so a global `dot(sumNormal, authoredNormal) < 0` check flips winding on floating-point `-0.0f` noise under backface culling (`qray_cullBackFaces`). Likewise, configuring primary/shadow `rayLength` too short in meters clips distant map geometry.
- **Fix:** Validate `MST_PATCH` triangle winding locally per quad/triangle against authored normals, and set generous meter-scaled ray lengths matching full map extents.

### Pitfall 11: Half-Res Indirect Noise (`giBounceRays = 0.5f`) & ReSTIR Whole-Scene Flashing
- **Symptom:** Metallic/chrome reflections show heavy horizontal striping/noise, OR firing weapons flashes the entire map (`rt_restir 1`), OR host unit tests fail after changing emitter upload order.
- **Root Cause:** Passing `giBounceRays = 0.5f` leaves half the rows of the specular indirect buffer unpopulated every frame. Separately, enabling global `rt_restir 1` with transient weapon/explosion dlights triggers unstable ASVGF temporal gradient resets across the whole scene.
- **Fix:** Default host indirect resolution cleanly (`giBounceRays = 1.0f` when stability matters), keep clustered light lists (`rt_restir 0`) as the default host path, and write host tests (`TestZeroPowerClusterSourcesFiltered`) to assert **order-independent membership** across point and emissive cluster sources rather than rigid array indices.

### Pitfall 12: Map-Wide Shadow Flickering (~10%) & Bright-Lamp Cluster Starvation (~7.5 Hz)
- **Symptom:** The entire level pulses ~10% darker/brighter every second, or bright lamps right beside walls strobe at ~7.5 Hz.
- **Root Cause:** Multiplying static DTAL emitters in `UploadEditorEmissiveLights` by fast `EvaluateEditorWave` shader animations pulses map lighting whenever a surface carries `rgbGen wave`. Meanwhile, emitting overlapping BSP `light` entities + `q3map_surfacelight` point lights + DTAL floods the per-cluster `Q2_LIGHT_LIST_MAX_PER_CELL` (`128`) fast-slot cap; partitioned `1/8` sampling in `q2SampleClusterLights` then hits nearby bright lamps only once every ~8 frames.
- **Fix:** Treat map lighting consistently without multiplying static DTAL power by high-frequency overlay waves, cluster/deduplicate `q3map_surfacelight` fixture lights per lamp instead of emitting duplicates per polygon face, and ensure `q2SampleClusterLights` evaluates cluster slots and power weights cleanly.

### Pitfall 13: FSR 3.1 "Heat Shimmer", Pixelated Scaling, & Unsynchronized Resolution Resets
- **Symptom:** Enabling FSR Performance/Ultra Performance either blurs walls into shimmering "heat waves", turns low-res and un-upscaled (Lanczos pixelation), bands light across walls, or desyncs framebuffers on preset changes.
- **Root Cause:** Zeroing `fHistoryWeight` and `jitter` disables temporal upscaling completely; restoring accumulation without tightened `RectifyHistory` clipping bounds, proper `jitterPrev`/`ComputeFrameJitter`, and releasing NVRHI targets (`PrepareFramebuffersForResolution` calling `nvrhiFrameSkeleton->ReleaseFramebufferTargets()` before `framebuffers->PrepareForSize`) leaves stale history, ghosting, or mismatched render targets.
- **Fix:** Keep subpixel jitter, `RectifyHistory` bounds, and `PrepareFramebuffersForResolution` synchronized cleanly in `Source/FSR.cpp` and `Source/VulkanDevice.cpp`, and **always rebuild and redeploy `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`** whenever FidelityFX shader headers change.

### Pitfall 14: 4K FPS Collapse From Unculled Froxel Volumetric Light Shafts
- **Symptom:** Small arenas drop from ~200 FPS to ~45 FPS at 4K even with moderate triangle counts (~47k triangles).
- **Root Cause:** Oversized volumetric froxel grids ray-march deep into occluded rooms behind solid geometry (`Compute` threads evaluating lights past primary hit depth).
- **Fix:** Keep tightened volume resolutions in `Source/RHI/NvrhiFrameSkeleton.cpp` (`128x72x64` / `192x108x96` / `256x144x128`), use `CmLightShaftDepthBounds.comp.spv` depth-bounds culling in `RhiLightShaftVolumePass`, and avoid duplicating static light emitters.

### Pitfall 15: Editor UX Regressions — Mirror Cubemaps, Disabled Stages, `Play Level`, & X11 `Tab` Focus
- **Symptom:** Toggling `mirror` keeps the old static cubemap texture; shader stage checkboxes do not toggle individual stages; `Play Level` launches without edited materials/lights/cfg; closing the launched game leaves `Tab` fly-mode broken in X11.
- **Root Cause:** Mirror promotion (`mirror && type == None`) and `disabledStagesMask` checks were bypassed by overlay-only (`isOverlayOnly`) or environment paths; `Play Level` launched outside the active editor/game config; returning from a child game window skipped X11 focus/pointer recovery.
- **Fix:** Propagate `disabledStagesMask` and mirror promotion identically through `EditorSceneRuntime`, `EditorOverlayPipeline`, and `EditorMaterial`; build launch arguments via `EditorPlayLevelRequest` / `buildPlayLevelCommand`; and handle X11 `FocusIn` / pointer-capture recovery (`EditorPlatformX11.cpp` / `EditorApp.cpp`) so `Tab` reliably re-enters fly mode after closing `Play Level`.

### Pitfall 16: Hardcoded `../gfx/textures` Breaking Standalone GPU Tests & Editor Deployment
- **Symptom:** `qray_spatial_twin_gpu` fails with `QR_ERROR_CANT_FIND_BLUE_NOISE`, or `quakerayeditor` post-build fails trying to copy missing `WaterNormal_n.png`.
- **Root Cause:** `CMakeLists.txt` and `editor/CMakeLists.txt` hardcoded `../gfx/textures` (valid only inside `vkquake-rt`) and unconditionally copied both textures whenever the directory matched.
- **Fix:** `QuakeRaySDK/Assets/textures/` bundles `BlueNoise_LDR_RGBA_128.png` and `WaterNormal_n.png`; `CMakeLists.txt` resolves `QR_TEST_TEXTURE_DIR` and `editor/CMakeLists.txt` resolves/copies each texture independently with `Assets/textures` fallback. Keep this per-file fallback intact when editing CMake rules.

### Pitfall 17: In-Process ML Inference (ONNX + WebGPU) Causing `earlyoom` & Vulkan `DeviceLostError`
- **Symptom:** Opening `Generate with AI` or switching model/resolution immediately locks up memory, triggers Linux `earlyoom` (>30 GiB RAM + 16 GiB VRAM exhausted), causes `Not enough memory for command submission`, and crashes `quakerayeditor` with `vk::DeviceLostError`.
- **Root Cause:** Auto-triggering ONNX Runtime `WebGPU Vulkan` (Dawn) inside `quakerayeditor` ran unchunked quadratic attention for `1.8 GB` `Lotus` models (`896x896` with seamless halo) concurrently with the 4K viewport renderer, with no VRAM budget enforcement in Dawn WebGPU and no crash boundary.
- **Fix:** Follow Part 2.7 strictly—use isolated out-of-process `quakeray_ai_worker` on `ggml-vulkan` (`<= 75%` VRAM budget via `ComputeWorkerVramBudget`, 3-stage `LotusPipeline`, automatic tiling, strict manual `Generate` trigger only). Do **not** reintroduce ONNX Runtime, WebGPU, or auto-run inference inside the editor UI process.

### Pitfall 18: HiDPI Panel Clipping, Destructive PBR Toggles, & Wiped `envMap` Masks
- **Symptom:** Controls truncate or overlap at 4K/1440p; unchecking a PBR section erases previously painted mask blocks on YAML save; unit tests testing `EditorMaterialBakes` produce zero gloss/emissive/SSS output; OR adding a normal map wipes embedded `roughnessMask`/`metallicMask` on `envMap` surfaces.
- **Root Cause:** Unscaled ImGui widget dimensions break above 1080p; `emissive_enabled`, `gloss_enabled`, and `sss_enabled` default to `false` unless toggled on, and `SampleBakeNormal` / `ResolveBakeContext` previously overwrote `envMap` roughness/metalness masks.
- **Fix:** Always lay out panels using `EditorGuiScale()`, `EditorGuiFitButtonWidth()`, `EditorGuiComboWidth()`, and `EditorGuiButtonRow()` (`editor/EditorGui.h`); persist `emissive_enabled`/`gloss_enabled`/`sss_enabled` as non-destructive mute flags (and explicitly set them `true` in standalone bake tests like `Tests/SubsurfaceTests.cpp`); and preserve embedded `roughnessMask`/`metallicMask` inside `ApplyEmbeddedPbrMaskSample` on normal-mapped `envMap` surfaces unless gloss overrides or `disabledStagesMask` apply.

### Pitfall 19: `Normal.a` Channel Collisions (Glass / SSS / Relief POM) & Blurred Normal Maps
- **Symptom:** Relief Parallax Occlusion Mapping bends glass panes or SSS geometry (creating swimming/distorted refraction), OR close-range normal maps look flat because host shaders/cvars crank up roughness clamping to suppress glossy fireflies.
- **Root Cause:** `Normal.a` stores three distinct masks (`ResolveBakedNormalAlpha`: active SSS mask -> glass `transparency` -> `texture_height` luminance), and enabling POM on translucent/SSS materials corrupts secondary rays. Separately, host tweaks to `minRoughness` mask close-up specular detail instead of letting the SDK's path-space firefly limiter work.
- **Fix:** Keep Relief POM (`setupParallaxGeometry`, `computeParallaxOcclusionPrimary`, `computeParallaxOcclusionRefl`, `computeParallaxCavityAO`) bypassed automatically on glass, water, SSS, and overlay-alpha surfaces; let `ResolveBakedNormalAlpha` pack `Normal.a` cleanly by material mode; and rely on `q2RegularizeDemodulatedSpecular` (`Source/Shaders/BRDF.hlsli`), `q2AntiFirefly` (`CmQ2Adapter.comp.hlsl`), and sub-pixel-gated LEADR bump-to-roughness regularization (`Source/Shaders/HitInfo.hlsli`) while preserving crisp close-range normal detail.

### Pitfall 20: Multi-Engine Winding Inversions, Raw Uncompressed PVS, & Sealed Sky Portals
- **Symptom:** Porting non-Q3 engines (`q2pro-rt` `IBSP` v38 / `.md2`, `xash3d-quakeray` `BSP30` / `.mdl`) leaves walls or models inside-out, drops dynamic cluster lights across whole corridors, or turns open skies pitch-black.
- **Root Cause:** Fan indices on Q2/GoldSrc faces wound clockwise against DXR culling; unshifted leaf-indexed or uncompressed PVS buffers were uploaded when `Source/WorldLights.cpp` expects zero-run RLE compressed cluster rows (`pVisData != nullptr`); and `SURF_SKY` brushes flagged `SURF_NODRAW` were dropped instead of tagged `QR_GEOMETRY_VISIBILITY_TYPE_SKY`.
- **Fix:** Reverse triangle winding consistently across both BSP faces and `.md2`/`.md3`/`.mdl` models, cluster-shift (`c + 1`) and RLE-compress PVS rows with trailing `0xFF` padding (`pvsRowBytes + 2`), decode GoldSrc `{` index-255 cutouts and `lights.rad` area emitters cleanly, and always emit `SURF_SKY` geometry with `QR_GEOMETRY_VISIBILITY_TYPE_SKY` even when `SURF_NODRAW` is also set.

### Pitfall 21: Sending Sprites, Beams, or Translucent FX to `qrUploadGeometry` & Faceted Models
- **Symptom:** `.sp2`/Q3 sprites render as opaque black/cutout shadow-casting cards, translucent explosions/beams look like dark refractive glass blocks (`GLASS_REFLECT_REFRACT`), or characters/weapons show flat crystalline triangle facets (`EXACT_NORMALS_BIT` / `GENERATE_NORMALS_BIT`).
- **Root Cause:** `qrUploadGeometry` has no alpha blending (only opaque, binary `ALPHA_TESTED`, mirror, or physical dielectric glass), while `EXACT_NORMALS` / `GENERATE_NORMALS` overwrite smooth vertex normals with per-triangle cross products.
- **Fix:** Upload all sprites, beams, `RF_TRANSLUCENT` FX models, decals, and additive shells through `qrUploadRasterizedGeometry` (and particles via `qrUploadParticles`), and keep `flags = 0` with smooth unit normals in `QrVertex::normal` for dynamic models and curved/mesh BSP surfaces.

### Pitfall 22: Black Impact Decals, Dark Energy Rings (`plasma_mrk`), Scorch Holes in Explosions, & `cameraNear` Bleed
- **Symptom:** Bullet/scorch marks render pitch black (`ZERO / ONE_MINUS_SRC_COLOR`), `plasma_mrk` shows a black halo around a blown center, live rocket fireballs have a black hole punched through the middle, or decals/cutout underlays Z-bleed across distance.
- **Root Cause:** Feeding exposure-scaled or luminance-converted decals into multiplicative blending crushes transmittance (`ComputeOverlayBlendFlags` handles raw `ZERO / ONE_MINUS_SRC_COLOR` via `blendFlags` bits `0..1`); `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` subtracts wall radiance on dark-fringed energy glow maps; uploading decals *after* explosions blends scorches over fireballs; and `cameraNear = 0.1` units collapses float32 depth separation (`~1` ULP at 500 units).
- **Fix:** Pass untouched raw RGBA textures for multiplicative decals, switch energy marks to `SRC_ALPHA / ONE`, upload surface decals **before** entities/explosions/smoke, nudge decals (`+0.25` units out; `-0.8/-1.6` for `cutoutFrameUnderlay`), and clamp `cameraNear` to `1.0–3.0` Quake units (`0.025–0.075 m`).

### Pitfall 23: Black Boxes Around 32-Bit Additive Sprites, Missing `0`-RGB Projectiles, & Mirrored Billboards
- **Symptom:** Fireballs, flames, or HUD/world sprites carry solid black square corners, plasma bolts vanish mid-air, or floating sprites (`"1"` score plums, asymmetric FX) are horizontally flipped.
- **Root Cause:** 32-bit effect TGAs with `alpha == 255` everywhere get mistakenly classified as alpha-blended instead of additive; `rgbGen identity` sprites pass `(0, 0, 0, 0)` `shaderRGBA`; and Quake's `viewaxis[1]` points **camera-left**, not right.
- **Fix:** Inspect actual texel alpha (`any(alpha < 250)`) before picking alpha vs additive blend, strip `"gfx/colors/black"` frames, coerce zeroed `rgbGen identity` entity color to `255`, and map `+left` (`viewaxis[1]`) to `u = 0` and `-left` to `u = 1` with CCW winding (`{0, 1, 2, 0, 2, 3}`).

### Pitfall 24: Over-Burned Explosions, Invisible sRGB-Crushed Shells, Double-Added Spheres, & Unlit Smoke
- **Symptom:** Explosions clip harshly, glowing pickup spheres (`yellow_sphere`, `red_sphere`, `energy_grn1`) turn invisible or double-bright, or smoke puffs float as opaque unlit discs.
- **Root Cause:** Scaling 8-bit sRGB textures by `0.1` on CPU crushes linear brightness via `0.1^2.2 ≈ 0.0063` (`RsWorld.frag.hlsl` already scales by `sceneExposure`); `CleanAdditiveBlackBackground` eats soft env-map gradients if applied to `tcGen environment` maps; `RhiRasterOverlayPass` disables hardware culling (`RasterCullMode::None`); and `smokePuff` lacks `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` or entity spawn time.
- **Fix:** Never pre-scale sRGB shell textures on CPU (use linear `info.color`), evaluate `deformVertexes wave` in local space, guard `CleanAdditiveBlackBackground` with `!environmentMap`, cull back-facing triangles on CPU for closed 3D overlays, and upload smoke with `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` (`smokeLook`) and per-entity `sceneTime - shaderTime` age.

---

## Part 4 — Verification & Quality Gate Checklist

Always run these checks before opening or updating any `QuakeRaySDK` PR or host-engine integration PR:

### 1. QuakeRaySDK Unit Tests, Standalone Builds & Strict Static Analysis
```bash
# Fast end-to-end local check (FidelityFX provider + Clang ASan/UBSan build + CPU tests + quality gate)
bash tools/check_local.sh --asan --shaders

# Or configure manually with editor + isolated ggml-vulkan AI worker + compile_commands.json
cmake -S /path/to/QuakeRaySDK -B build/local-quality -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DQR_BUILD_TESTS=ON -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi \
    -DQR_GGML_SOURCE_DIR=/path/to/ggml \
    -DQR_FFX_VK_SO=/path/to/libamd_fidelityfx_vk.so
cmake --build build/local-quality --parallel

# Run all non-GPU unit tests (and standalone GPU suites via bundled Assets/textures/)
ctest --test-dir build/local-quality --output-on-failure -LE gpu
ctest --test-dir build/local-quality --output-on-failure -L gpu

# Verify headers and diff-gated static analysis against origin/main
python3 -B tools/copyright_header.py --check --base origin/main
python3 -B tools/check_quality.py --strict-tools --base origin/main \
    --compile-commands build/local-quality/compile_commands.json
```

### 2. Host Engine Build, Adapter Tests & Editor-vs-Runtime Parity Tests (e.g. `ioq3-rt`)
```bash
# Build host renderer + quakerayeditor (+ quakeray_ai_worker) + adapter & parity tests
cmake --build /path/to/host-build --target renderer_qray quakerayeditor qray_q3_editor_adapter_tests qray_authored_tests --parallel
ctest --test-dir /path/to/host-build --output-on-failure
```
Verify that your parity suite (e.g. `qray_authored_tests` / `TestSteamQ3Tourney1Parity`) confirms identical `surfaces`, `worldClusters`, `emissiveLights` (DTAL), `overlaySurfaces`, `lights`, and `skybox` state between the editor and runtime `EditorSceneRuntime` instances on real game maps, and that all GPU + CPU tests pass (`100%`).

### 3. Live Paired Smoke Test (`quakerayeditor` vs Runtime Game Executable)
Before declaring an adapter or pipeline integration finished:
1. Launch **both** `quakerayeditor --scene <map> --shaders <shaders>` and the runtime engine on the same real map (`q3tourney1`, `q3dm1`, `q3dm10`, etc.) and compare startup logs to verify **1-to-1 match** in static BLAS triangles/vertices, surfaces, DTAL area lights, material bakes, cluster count, and skybox faces (with the runtime only adding dynamic BLAS instances for moving submodels/entities).
2. Verify multi-stage animated surfaces (jump pads, teleporters, banners, grates, flames, chrome), closed curved `MST_PATCH` cylinders (`q3dm1`), `mirror` promotion, Relief POM (`texture_height` / `parallax_scale` + cavity AO), non-destructive PBR master toggles (`emissive_enabled`, `gloss_enabled`, `sss_enabled`), and per-stage visibility toggles render cleanly with zero black/void cutouts.
3. Verify FSR 3.1 presets scale crisply without "heat shimmer" walls or banding, volumetric light shafts hold target 4K frame times without light-cluster strobing, specular firefly limiting keeps close-up normal detail crisp, HiDPI panels (`1080p`/`4K`) fit without clipping, and predefined lights (`q3map_surfacelight` / `lights.rad` fixtures and entity lights) survive **Apply** -> map reload -> **Regenerate from map**.
4. Verify combat & runtime FX in-game: bullet/scorch/plasma decals blend cleanly without black rings on light and dark walls, live rocket explosions and lit smoke trails render cleanly in front of wall scorches, 32-bit additive sprites/projectiles show no black boxes and keep camera-left UVs un-mirrored, pickup spheres/deformed shells glow cleanly without sRGB crushing, and dynamic character/weapon models shade smoothly (`flags = 0`).
5. Verify the **Models** tab shows assembled characters with `.skin` switching without corrupting live world bakes, **Play Level** launches cleanly and restores `Tab` fly-mode after exiting back to the editor, and the 4-tab `Generate with AI` flow (`Upscale`, `De-lighting`, `Normals`, `Depth`) never auto-starts, shows `quakeray_ai_worker` progress cleanly without exhausting VRAM/RAM, and preserves editor stability on `Cancel` or error.
