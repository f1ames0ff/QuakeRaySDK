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
| **SDK Core & Shared Scene Runtime** | `QuakeRaySDK` (`Include/qray/`, `Source/`, `editor/`, `Tests/`) | Public C ABI (`qray.h`), Vulkan/NVRHI ray-tracing backend, shaders, C++ contracts (`host_contract.h`, `gpu_context.h`); generic `quakerayeditor` UI/gizmos; and the **single shared scene runtime layer** (`EditorSceneRuntime.{h,cpp}`, `EditorDtalPipeline.{h,cpp}`, `EditorOverlayPipeline.{h,cpp}`, `EditorSession`, `EditorMaterial`, `EditorSettings`, `EditorSpatial`, `Scene`, `Assets` + `EditorAssetVfsCallbacks`) plus engine-agnostic hooks (`EditorFormatAdapter`, `QR_EDITOR_ADAPTER_SOURCES`, `QR_EDITOR_ADAPTER_INCLUDE_DIRS`). | Game-specific BSP/shader/model loaders (`Q3Bsp.*`, `Q3Shader.*`, `Q3Model.*`, `Q3EditorAdapter.*`), Quake 2 (`IBSP` v38, `.wal`, `.md2`), GoldSrc, or Source parsers, hardcoded game asset lists (e.g. `flame.mdl flame2.mdl`), or host cvar/filesystem code. |
| **Host Engine Format Adapter & Runtime Dynamics** | Host repo (e.g., `ioq3-rt/code/rendererqray/`, `ioq3-rt/cmake/renderer_qray.cmake`) | Submodule pinning (`code/quakeraysdk`); **one** game format adapter (`Q3EditorAdapter.*`, `Q3Bsp.*`, `Q3Shader.*`, `Q3Model.*`, `Q3AdapterTests.cpp`) compiled into **both** `quakerayeditor` (`QR_EDITOR_ADAPTER_SOURCES`) and the runtime renderer (`renderer_qray`); host VFS glue (`EditorAssetVfsCallbacks`); and **only per-frame gameplay dynamics** layered on top of `EditorSceneRuntime` (players/MD3 models via `ResolveDynamicMaterial` + `BuildEditorDynamicMeshAreaLights`, moving inline brush submodels `*1..*N`, sprites/beams, decals/smoke, dynamic `dlights` via `EditorClusterLightUploadExtras`, 2D HUD) plus parity tests (`qray_authored_tests`). | A parallel host-side BSP/YAML/material/DTAL/sky/light loader or duplicated static scene builder alongside `EditorSceneRuntime` (do NOT recreate `qray_authored.{h,cpp}`-style parallel pipelines). Core SDK or generic `quakerayeditor` UX fixes committed into the host repo. |

### Zero-Duplication Mandate & Separate PRs
- **One static scene pipeline for both targets:** Every feature affecting static BSP loading, material overrides/bakes, DTAL area-light clipping/building, light cluster tables, world overlays, sky/sun, `qray.materials.yaml` / `qray.lights.yaml` loading, or meter conversion (`EditorDrawFrame`) **must** go into the shared layer (`EditorSceneRuntime` / `EditorDtalPipeline` / `EditorOverlayPipeline` in `QuakeRaySDK/editor/`) so `quakerayeditor` and the runtime renderer behave 1-to-1 by construction.
- Generic `Include/`, `Source/`, or shared `editor/` changes go into **QuakeRaySDK** PRs.
- Host VFS callbacks, runtime dynamic entity uploads, and game-specific `EditorFormatAdapter` implementations live in the **host repository** (`ioq3-rt`, etc.) and link/compile the shared layer from `QuakeRaySDK/editor/`.

---

## 2. Quick Integration Checklist (New Engine or Editor Adapter)

1. **Read the contracts before coding:** [docs/agent-integration-guide.md](docs/agent-integration-guide.md), [docs/spatial-contract.md](docs/spatial-contract.md), [docs/host-contract.md](docs/host-contract.md), and [docs/embedding.md](docs/embedding.md).
2. **CMake & dependencies:** Apply `cmake/nvrhi-max-binding-layouts.patch` (`c_MaxBindingLayouts` >= 13; patch raises it to 16), build FidelityFX (`libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`), and link `qray::renderer`. Inject editor adapters into `quakerayeditor` via `QR_EDITOR_ADAPTER_SOURCES` and `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and compile the **exact same adapter + `EditorSceneRuntime` sources** into the host runtime target (see `ioq3-rt/cmake/renderer_qray.cmake`).
3. **Reuse `EditorSceneRuntime` — never write a parallel static loader:** Hook the engine VFS via `EditorAssetSource::SetVfsCallbacks`, pass `EditorSceneLoadOptions` (e.g. `includeInlineSubmodels = false` in runtime when brush submodels move dynamically), call `EditorSceneRuntime::{LoadBspSceneMemory, CreateTextureMaterials, UploadStaticGeometry, UploadWorldClusters, UploadSun, UploadLights, UploadEmissiveLights, UploadClusterLightSources, UploadWorldOverlays, UploadSky, PopulateDrawFrame}`, and layer **only** runtime dynamic entities/HUD/dlights on top (`ResolveDynamicMaterial`, `BuildEditorDynamicMeshAreaLights`, `EditorClusterLightUploadExtras`).
4. **Meters & single spatial boundary:** Call `qrSetSpatialConvention` once before uploading geometry/lights; route all scene/frame uploads through `EditorSceneRuntime` and `EditorSpatial` (`EditorDrawFrame`, `1 m = 40 Quake units`, `kEditorUnitsPerMeter = 40.0f`). Never convert coordinates twice.
5. **PVS clusters & solid-leaf nudging:** Cluster `0` is reserved by the SDK for solid/outside; map engine PVS cluster `c >= 0` to `c + 1` (and nudge surface vertices and lights slightly off brush planes so solid-leaf hits still resolve to an adjacent valid cluster).
6. **Predefined lights & Light Editor parity:** Set `scene.usesExternalLightingModel = true` when the engine supplies its own lighting model, emit **all** predefined map lights (`light` / `lightJunior` entities **and** all `q3map_surfacelight` / luminous surface fixtures, not just flame sprites) into both `scene.lights` and `scene.bspLights` with their cluster resolved, and do not silently duplicate them as unselectable DTAL lights.
7. **Multi-stage shaders:** Split ray-traced base stages (`EditorSurface`) from animated/blended overlay stages (`EditorOverlaySurface`), generate `isCutoutUnderlay` backings where cutout frames sit over dark/additive voids, and avoid directory-open crashes with `std::filesystem::is_regular_file`.
8. **Cache invalidation & Parity Verification:** Whenever texture pixels change in place (`Generate with AI`, brush/procedural edits), destroy/rebuild stale materials and clear `materialBakes` / `previewMaterialBakes` via `.Destroy(m_instance)`. Always run an editor-vs-runtime parity check (`qray_authored_tests` + live map comparison).

---

## 3. Top Pitfalls to Avoid Immediately

See [docs/agent-integration-guide.md § Part 3](docs/agent-integration-guide.md#part-3--graveyard-of-pitfalls-what-broke-and-how-to-avoid-it) for full root causes and code fixes:

- **Pitfall 1 (Duplicated Static Pipeline):** Never write separate BSP/YAML/material-bake/DTAL/light/sky pipelines for the editor and the runtime renderer. Both targets must invoke `EditorSceneRuntime`, `EditorDtalPipeline`, `EditorOverlayPipeline`, and the same `EditorFormatAdapter`.
- **Pitfall 2 (Wrong Repo):** Never put `Q3Bsp.*` / `Q3Shader.*` / `Q3Model.*` / `Q3EditorAdapter.*` inside `QuakeRaySDK/editor/`. Keep them in `<host_repo>/code/.../editor/` and inject via `QR_EDITOR_ADAPTER_SOURCES`.
- **Pitfall 3 (Unselectable Predefined Lights):** Leaving `q3map_surfacelight` fixtures out of `scene.lights`/`scene.bspLights` makes them illuminate the map without cyan selection circles in `Light Editor` and impossible to toggle or delete.
- **Pitfall 4 (Lost Deletions / Hardcoded Model Filters):** Never hardcode `flame.mdl flame2.mdl` filters or drop empty `lights:` keys; persist standard map lights cleanly in `qray.lights.yaml` (writing `lights: []` when all are deleted) and restore defaults via `Regenerate from map`.
- **Pitfall 5 (Black / Leaking Shaders):** Multi-stage shaders (launch pads, teleporters, monitors, grates, flames) need base-vs-overlay separation, additive black cleaning, and `isCutoutUnderlay` backing geometry.
- **Pitfall 6 (`Is a directory` Crash):** Always guard `std::filesystem::is_regular_file(path, ec)` before reading files when resolving extensionless shader/texture names like `default`.
- **Pitfall 7 (Stale `EditorMaterialBakes`):** Call `materialBakes.Destroy(instance)` whenever generated or procedural texture pixels are replaced in place.
- **Pitfall 8 (Hidden Multi-Part Models / `./` Paths):** Normalize paths with `NormalizeEditorAssetPath` and collapse multi-mesh characters cleanly in `postProcessModelCatalog` without hiding standalone `.md3` props.
- **Pitfall 9 (CI Quality Gate Failures):** Comply with `clang-tidy`, `cppcheck`, `lizard` (`CCN <= 15`, `NLOC <= 120`, `params <= 7`), and `ruff`. Never call `std::system()`.

---

## 4. Mandatory Verification Commands (Before Every Push / PR)

```bash
# 1. Configure & build SDK + tests (compile_commands.json required for clang-tidy)
cmake -S . -B build/local-quality -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DQR_BUILD_TESTS=ON -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi -DQR_FFX_VK_SO=/path/to/libamd_fidelityfx_vk.so
cmake --build build/local-quality --parallel

# 2. Run CPU/policy unit tests
ctest --test-dir build/local-quality --output-on-failure -LE gpu

# 3. Run the strict diff-gated static analysis & complexity check against origin/main
python3 -B tools/check_quality.py --strict-tools --base origin/main \
    --compile-commands build/local-quality/compile_commands.json
```
