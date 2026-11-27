# Repository Instructions for AI Coding Agents (`AGENTS.md`)

> **READ THIS FIRST BEFORE WRITING CODE IN THIS REPOSITORY OR ANY HOST ENGINE:**
>
> **QuakeRaySDK Integration Guide for Agents:** [docs/agent-integration-guide.md](docs/agent-integration-guide.md) — step-by-step guide to embedding `QuakeRaySDK`, connecting runtime renderers, writing external `EditorFormatAdapter` plugins for `quakerayeditor`, strict repository responsibility boundaries, and the full catalogue of pitfalls ("graveyard of gotchas") learned during the `ioq3-rt` (Quake III) integration.
>
> **Full Documentation Map:** [docs/README.md](docs/README.md).

---

## 1. Golden Rules: Strict Responsibility Boundaries & Single Shared Scene Pipeline

Never mix engine-specific game code with SDK/editor core code across PRs or repositories, **and NEVER duplicate static scene loading, material baking, DTAL construction, light clustering, or sky/overlay pipelines between `quakerayeditor` and the game runtime renderer**.

| Layer | Repository | What Belongs Here | What **NEVER** Belongs Here |
|---|---|---|---|
| **SDK Core & Shared Scene Runtime** | `QuakeRaySDK` (`Include/qray/`, `Source/`, `editor/`, `Tests/`) | Public C ABI (`qray.h`), Vulkan/NVRHI ray-tracing backend, shaders (including Relief POM, path-space specular firefly limiting, sub-pixel LEADR bump regularization), C++ contracts (`host_contract.h`, `gpu_context.h`); bundled runtime assets (`Assets/textures/`); generic `quakerayeditor` HiDPI UI (`EditorGui*`), gizmos, and isolated `quakeray_ai_worker` (`ggml-vulkan`); plus the **single shared scene runtime layer** (`EditorSceneRuntime.{h,cpp}`, `EditorDtalPipeline.{h,cpp}`, `EditorOverlayPipeline.{h,cpp}`, `EditorSession`, `EditorMaterial`, `EditorSettings`, `EditorSpatial`, `Scene`, `Assets` + `EditorAssetVfsCallbacks`) and engine-agnostic hooks (`EditorFormatAdapter`, `QR_EDITOR_ADAPTER_SOURCES`, `QR_EDITOR_ADAPTER_INCLUDE_DIRS`). | Game-specific BSP/shader/model loaders (`Q3Bsp.*`, `Q3Shader.*`, `Q3Model.*`, `Q3EditorAdapter.*`), Quake 2 (`IBSP` v38, `.wal`, `.md2`), GoldSrc/Xash3D (`BSP30`, `.wad`, `StudioMDL` `.mdl`, `lights.rad`), Return to Castle Wolfenstein (`iortcw`), Jedi Knight / Jedi Academy (`OpenJK`), or Doom 3 (`dhewm3`) parsers, hardcoded game asset lists (e.g. `flame.mdl flame2.mdl`), or host cvar/filesystem code. |
| **Host Engine Format Adapter & Runtime Dynamics** | Host repo (e.g., `ioq3-rt/code/rendererqray/`, `q2pro-rt/src/refresh/qray/`, `xash3d-quakeray/ref/qray/`) | Submodule pinning (`code/quakeraysdk`); **one** game format adapter (e.g., `Q3EditorAdapter.*`, `Q2EditorAdapter.*`, `HlEditorAdapter.*`) compiled into **both** `quakerayeditor` (`QR_EDITOR_ADAPTER_SOURCES`) and the runtime renderer; host VFS glue (`EditorAssetVfsCallbacks`); and **only per-frame gameplay dynamics** layered on top of `EditorSceneRuntime` (players/weapons/models via `ResolveDynamicMaterial` + `BuildEditorDynamicMeshAreaLights`, moving inline brush submodels `*1..*N`, sprites/beams, decals/smoke, dynamic `dlights` via `EditorClusterLightUploadExtras`, 2D HUD) plus parity tests (`qray_authored_tests`). | A parallel host-side BSP/YAML/material/DTAL/sky/light loader or duplicated static scene builder alongside `EditorSceneRuntime` (do NOT recreate `qray_authored.{h,cpp}`-style parallel pipelines). Core SDK or generic `quakerayeditor` UX fixes committed into the host repo. |

### Zero-Duplication Mandate & Separate PRs
- **One static scene pipeline for both targets:** Every feature affecting static BSP loading, material overrides/bakes (including Relief POM `texture_height`/`parallax_scale`, non-destructive PBR master toggles `emissive_enabled`/`gloss_enabled`/`sss_enabled`, and embedded masks), DTAL area-light clipping/building, light cluster tables, world overlays, sky/sun, `qray.materials.yaml` / `qray.lights.yaml` loading, or meter conversion (`EditorDrawFrame`) **must** go into the shared layer (`EditorSceneRuntime` / `EditorDtalPipeline` / `EditorOverlayPipeline` in `QuakeRaySDK/editor/`) so `quakerayeditor` and the runtime renderer behave 1-to-1 by construction.
- Generic `Include/`, `Source/`, or shared `editor/` changes go into **QuakeRaySDK** PRs.
- Host VFS callbacks, runtime dynamic entity uploads, and game-specific `EditorFormatAdapter` implementations live in the **host repository** (`ioq3-rt`, `q2pro-rt`, `xash3d-quakeray`, etc.) and link/compile the shared layer from `QuakeRaySDK/editor/`.

---

## 2. Quick Integration Checklist (New Engine or Editor Adapter)

1. **Read the contracts before coding:** [docs/agent-integration-guide.md](docs/agent-integration-guide.md), [docs/spatial-contract.md](docs/spatial-contract.md), [docs/host-contract.md](docs/host-contract.md), and [docs/embedding.md](docs/embedding.md).
2. **CMake & dependencies:** Apply `cmake/nvrhi-max-binding-layouts.patch` (`c_MaxBindingLayouts` >= 13; patch raises it to 16), build FidelityFX (`libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`), and link `qray::renderer`. When `QR_BUILD_EDITOR=ON`, optionally pass `-DQR_GGML_SOURCE_DIR=/path/to/ggml` to build `quakeray_ai_worker`, inject editor adapters into `quakerayeditor` via `QR_EDITOR_ADAPTER_SOURCES` and `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, compile the **exact same adapter + `EditorSceneRuntime` sources** into the host runtime target, and preserve bundled `Assets/textures/` (`BlueNoise_LDR_RGBA_128.png`, `WaterNormal_n.png`) fallback rules.
3. **Reuse `EditorSceneRuntime` — never write a parallel static loader:** Hook the engine VFS via `EditorAssetSource::SetVfsCallbacks`, pass `EditorSceneLoadOptions` (e.g. `includeInlineSubmodels = false` in runtime when brush submodels move dynamically), call `EditorSceneRuntime::{LoadBspSceneMemory, CreateTextureMaterials, UploadStaticGeometry, UploadWorldClusters, UploadSun, UploadLights, UploadEmissiveLights, UploadClusterLightSources, UploadWorldOverlays, UploadSky, PopulateDrawFrame}`, and layer **only** runtime dynamic entities/HUD/dlights on top (`ResolveDynamicMaterial`, `BuildEditorDynamicMeshAreaLights`, `EditorClusterLightUploadExtras`).
4. **Meters & single spatial boundary:** Call `qrSetSpatialConvention` once before uploading geometry/lights; route all scene/frame uploads through `EditorSceneRuntime` and `EditorSpatial` (`EditorDrawFrame`, `1 m = 40 Quake units`, `kEditorUnitsPerMeter = 40.0f`). Never convert coordinates twice.
5. **PVS clusters, RLE compression, & solid-leaf nudging:** Cluster `0` is reserved by the SDK for solid/outside; map engine PVS cluster `c >= 0` to `c + 1` (and nudge surface vertices and lights slightly off brush planes so solid-leaf hits still resolve to an adjacent valid cluster). When supplying non-null `pVisData`, encode rows in Quake zero-run RLE matching `Source/WorldLights.cpp` (`pvsRowBytes + 2` trailing `0xFF` bytes), and keep `SURF_SKY` portals tagged `QR_GEOMETRY_VISIBILITY_TYPE_SKY` even when combined with `SURF_NODRAW`.
6. **Predefined lights & Light Editor parity:** Set `scene.usesExternalLightingModel = true` when the engine supplies its own lighting model, emit **all** predefined map lights (`light` / `lightJunior` entities **and** all `q3map_surfacelight` / `lights.rad` / luminous surface fixtures, not just flame sprites) into both `scene.lights` and `scene.bspLights` with their cluster resolved, and do not silently duplicate them as unselectable DTAL lights.
7. **Multi-stage shaders & Relief POM `Normal.a` packing:** Split ray-traced base stages (`EditorSurface`) from animated/blended overlay stages (`EditorOverlaySurface`), generate `isCutoutUnderlay` backings where cutout frames sit over dark/additive voids, avoid directory-open crashes with `std::filesystem::is_regular_file`, and let `ResolveBakedNormalAlpha` pack `Normal.a` by mode (SSS mask -> glass `transparency` -> `texture_height` luminance for Relief POM).
8. **Targeted Cache Invalidation & Parity Verification:** Whenever texture pixels change in place (`Generate with AI`, brush/procedural edits), rebuild only that slot via `EditorSceneRuntime::CreatePoolMaterial` and call targeted `InvalidateTexture(m_instance, existing)` on `m_materialBakes` and `m_previewMaterialBakes` (never global `.Destroy(m_instance)` while static geometry is live, or the map turns white). Always run an editor-vs-runtime parity check (`qray_authored_tests` + live map comparison).
9. **Runtime FX, Decals, & Smooth Model Normals (See Guide §1.5):** Route all sprites, beams, `RF_TRANSLUCENT` FX models, additive shells, and decals through `qrUploadRasterizedGeometry` (and particles via `qrUploadParticles`)—never `qrUploadGeometry`. Keep `flags = 0` with smooth normals on dynamic models; upload surface decals **before** explosions/entities with untouched raw textures on darken blends and `SRC_ALPHA`/`ONE` on energy marks; clamp `cameraNear` to `1.0–3.0` Quake units; verify `any(alpha < 250)` on 32-bit effect sprites; never pre-scale sRGB shell textures on CPU; cull backfaces on closed 3D overlays; mark smoke `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE`; and map camera-left `viewaxis[1]` to `u = 0`.

---

## 3. Top Pitfalls to Avoid Immediately

See [docs/agent-integration-guide.md § Part 3](docs/agent-integration-guide.md#part-3--graveyard-of-pitfalls-what-broke-and-how-to-avoid-it) for full root causes and code fixes:

- **Pitfall 1 (Duplicated Static Pipeline):** Never write separate BSP/YAML/material-bake/DTAL/light/sky pipelines for the editor and the runtime renderer. Both targets must invoke `EditorSceneRuntime`, `EditorDtalPipeline`, `EditorOverlayPipeline`, and the same `EditorFormatAdapter`.
- **Pitfall 2 (Wrong Repo):** Never put game-specific adapters/loaders (`Q3Bsp.*`, `Q2Bsp.*`, `HlBsp.*`, `Q3EditorAdapter.*`, etc.) inside `QuakeRaySDK/editor/`. Keep them in `<host_repo>/...` and inject via `QR_EDITOR_ADAPTER_SOURCES`.
- **Pitfall 3 (Unselectable Predefined Lights):** Leaving `q3map_surfacelight` / `lights.rad` fixtures out of `scene.lights`/`scene.bspLights` makes them illuminate the map without cyan selection circles in `Light Editor` and impossible to toggle or delete.
- **Pitfall 4 (Lost Deletions / Hardcoded Model Filters):** Never hardcode `flame.mdl flame2.mdl` filters or drop empty `lights:` keys; persist standard map lights cleanly in `qray.lights.yaml` (writing `lights: []` when all are deleted) and restore defaults via `Regenerate from map`.
- **Pitfall 5 (Black / Leaking Shaders):** Multi-stage shaders (launch pads, teleporters, monitors, grates, flames) need base-vs-overlay separation, additive black cleaning, and `isCutoutUnderlay` backing geometry.
- **Pitfall 6 (`Is a directory` Crash):** Always guard `std::filesystem::is_regular_file(path, ec)` before reading files when resolving extensionless shader/texture names like `default`.
- **Pitfall 7 (Stale `EditorMaterialBakes` vs Map-Wide White Wipe):** Recreate only the edited pool material (`CreatePoolMaterial`) and call targeted `InvalidateTexture(m_instance, existing)` on `m_materialBakes` / `m_previewMaterialBakes` — never global `Destroy(m_instance)` while static world geometry holds live handles.
- **Pitfall 8 (Hidden Multi-Part Models / `./` Paths):** Normalize paths with `NormalizeEditorAssetPath` and collapse multi-mesh characters cleanly in `postProcessModelCatalog` without hiding standalone `.md3` props.
- **Pitfall 9 (CI Quality Gate Failures):** Comply with `copyright_header.py`, `clang-tidy`, `cppcheck`, `lizard` (`CCN <= 15`, `NLOC <= 120`, `params <= 7`), and `ruff`. Never call `std::system()`.
- **Pitfall 10 (Closed 360° Bezier Patches & Short Rays):** Validate `MST_PATCH` winding locally per quad/triangle against authored normals (global radial sums cancel to zero) and pass full-map metric `rayLength`.
- **Pitfall 11 (Half-Res Specular Noise & ReSTIR Flashing):** Keep cluster light lists (`rt_restir 0`) as default and assert order-independent membership in host cluster-light tests.
- **Pitfall 12 (Shadow Flicker & Light Cluster Starvation):** Never modulate static DTAL power by fast shader waves or flood per-cluster `128`-slot light caps with duplicate per-face fixtures.
- **Pitfall 13 (FSR 3.1 Shimmer & Resizes):** Keep subpixel jitter, `RectifyHistory`, and `PrepareFramebuffersForResolution` synced; rebuild/redeploy `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll` whenever FidelityFX headers change.
- **Pitfall 14 (4K Light Shaft Collapse):** Keep tightened froxel grids and depth-bounds culling (`CmLightShaftDepthBounds`) intact.
- **Pitfall 15 (Editor UX Regressions):** Propagate `disabledStagesMask` / `mirror` through shared runtime pipelines and recover X11 `FocusIn` pointer capture after `Play Level`.
- **Pitfall 16 (`../gfx/textures` Breakage):** Rely on bundled `Assets/textures/` (`BlueNoise_LDR_RGBA_128.png`, `WaterNormal_n.png`) fallback rather than sibling-repo paths.
- **Pitfall 17 (In-Process ML OOM / DeviceLost):** Run the 4-tab `Generate with AI` workflow exclusively via out-of-process `quakeray_ai_worker` (`ggml-vulkan`, 75% VRAM budget, manual trigger only — never ONNX/WebGPU).
- **Pitfall 18 (HiDPI Clipping & Destructive PBR Toggles):** Scale widgets via `EditorGuiScale`/`EditorGuiFitButtonWidth`/`EditorGuiComboWidth`/`EditorGuiButtonRow`, preserve `emissive_enabled`/`gloss_enabled`/`sss_enabled` across saves (enable them in unit bake tests), and keep embedded `envMap` `roughnessMask`/`metallicMask`.
- **Pitfall 19 (`Normal.a` Collisions & Over-Clamped Roughness):** Keep Relief POM disabled on glass/water/SSS/overlay-alpha surfaces and let `q2RegularizeDemodulatedSpecular` + sub-pixel LEADR preserve close-up normal detail instead of raising `minRoughness`.
- **Pitfall 20 (Multi-Engine Winding, RLE PVS, & Sky Portals):** Flip CW winding consistently on BSP + `.md2`/`.md3`/`.mdl` models, cluster-shift + RLE-compress `pVisData`, decode `{` GoldSrc cutouts and `lights.rad`, and tag `SURF_SKY` portals `QR_GEOMETRY_VISIBILITY_TYPE_SKY`.
- **Pitfall 21 (Sprites/Explosions in `qrUploadGeometry` & Faceted Models):** Never submit sprites, beams, or `RF_TRANSLUCENT` explosions to raytraced `GLASS_REFLECT_REFRACT` / `ALPHA_TESTED`; keep `flags = 0` + smooth unit normals on dynamic models.
- **Pitfall 22 (Black Decals, Energy Rings, Explosion Holes, & `cameraNear` Bleed):** Pass raw textures on `ZERO / ONE_MINUS_SRC_COLOR` decals, use `SRC_ALPHA / ONE` on energy scorch marks, upload decals **before** explosions/smoke, and keep `cameraNear` at `1.0–3.0` Quake units.
- **Pitfall 23 (Black 32-Bit Additive Boxes, `0`-RGB Projectiles, & Mirrored Billboards):** Check actual pixel alpha (`< 250`) rather than channel count, coerce zeroed `rgbGen identity` entity color to `255`, and map camera-left `+Y` to `u = 0`.
- **Pitfall 24 (Burned Explosions, sRGB-Crushed Shells, Double Spheres, & Unlit Smoke):** Never dim sRGB textures on CPU (`0.1^2.2`), keep `CleanAdditiveBlackBackground` off `tcGen environment` maps, CPU-cull backfaces on closed 3D overlays, and tag smoke with `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE`.

---

## 4. Mandatory Verification Commands (Before Every Push / PR)

```bash
# 1. Fast end-to-end check (FidelityFX provider + Clang ASan/UBSan + CPU tests + quality gate)
bash tools/check_local.sh --asan --shaders

# 2. Full manual build with editor + isolated ggml-vulkan AI worker + compile_commands.json
cmake -S . -B build/local-quality -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DQR_BUILD_TESTS=ON -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi -DQR_GGML_SOURCE_DIR=/path/to/ggml \
    -DQR_FFX_VK_SO=/path/to/libamd_fidelityfx_vk.so
cmake --build build/local-quality --parallel

# 3. Run CPU/policy unit tests and standalone GPU suites
ctest --test-dir build/local-quality --output-on-failure -LE gpu
ctest --test-dir build/local-quality --output-on-failure -L gpu

# 4. Strict headers and diff-gated static analysis against origin/main
python3 -B tools/copyright_header.py --check --base origin/main
python3 -B tools/check_quality.py --strict-tools --base origin/main \
    --compile-commands build/local-quality/compile_commands.json
```
