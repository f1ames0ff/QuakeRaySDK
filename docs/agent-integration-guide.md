# QuakeRaySDK Integration Guide for Agents

> **See also:** [AGENTS.md](../AGENTS.md) | [docs/README.md](README.md) | [embedding.md](embedding.md) | [host-contract.md](host-contract.md) | [host-driver-contract.md](host-driver-contract.md) | [gpu-context.md](gpu-context.md) | [spatial-contract.md](spatial-contract.md)

This document is the end-to-end engineering playbook distilled from integrating `QuakeRaySDK` across classic and modern id Tech / GoldSrc engine families (id Tech 1 / Quake, id Tech 2 / Quake II, GoldSrc / Half-Life, id Tech 3 / Quake III Arena, Return to Castle Wolfenstein, Jedi Knight, id Tech 4 / Doom 3, and custom hosts), together with standalone `quakerayeditor` format adapters. Read it completely before modifying `QuakeRaySDK` or wiring a host engine.

---

## Architecture, Strict Responsibility Boundaries & Single Shared Scene Pipeline

`QuakeRaySDK` is a source-level Vulkan/NVRHI ray-tracing renderer plus a reusable, engine-neutral scene runtime and authoring shell (`editor/`). It intentionally excludes game-specific map/model/shader parsers, engine VFS implementations, and gameplay simulation.

Crucially, **both `quakerayeditor` and the host game runtime renderer (`renderer_qray`) MUST run through one single shared static scene pipeline (`EditorSceneRuntime`) and one single host format adapter (`EditorFormatAdapter`)**. Never implement parallel BSP loaders, material bakers, DTAL builders, light-cluster uploaders, YAML session readers, or sky/overlay pipelines separately for the editor and the game!

```text
+-------------------------------------------------------------------------+
| Host Engine Repository (e.g. id Tech 2 / GoldSrc / id Tech 3 / Tech 4)  |
|                                                                         |
|  1. Single Game Format Adapter (host renderer/editor/ folder)           |
|     - <Game>EditorAdapter.*, <Game>Bsp.*, <Game>Shader.*, <Game>Model.* |
|     - Compiled into quakerayeditor (QR_EDITOR_ADAPTER_SOURCES)          |
|       AND compiled into renderer_qray (identical static parsing)        |
|                                                                         |
|  2. Thin Runtime Dynamics Layer (host qray_host.cpp)                    |
|     - Hooks live engine VFS via EditorAssetSource::SetVfsCallbacks      |
|     - Calls EditorSceneRuntime for ALL static world / authored state    |
|     - Layers ONLY runtime dynamics on top:                              |
|       * Dynamic models / characters + ResolveDynamicMaterial            |
|       * Moving inline brush submodels (*1..*N as dynamic world-space    |
|         geometry with per-vertex nudged clusters, or STATIC_MOVABLE)    |
|       * Dynamic mesh DTAL via BuildEditorDynamicMeshAreaLights          |
|       * Sprites, beams, decals, smoke, dlights                          |
|         (EditorClusterLightUploadExtras)                                |
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
| Game-specific BSP/portal/shader/material/model/archive parsing (`IBSP` v38/v46/v47, `BSP30`, `.proc`, `.shader`, `.wal`, `.wad`, `.md2`/`.md3`/`.mdl`/`.mdc`/`.mds`/`.md5mesh`, `.skin`, `.pk3`/`.kpf`), plus `EditorFormatAdapter` registration | **Host Engine Repo** (single adapter) | Game format knowledge never leaks into `QuakeRaySDK`, and sharing one adapter prevents editor/runtime drift. |
| Host VFS callbacks (`SetVfsCallbacks`), live entity/model transforms, moving brush submodels (`*1..*N`), weapon/explosion `dlights`, particles, decals, 2D HUD | **Host Engine Repo** (`qray_host.cpp`) | Specific to runtime game simulation and client frames. |
| Parallel host-side static BSP/material/DTAL/light/sky pipeline duplicating `EditorSceneRuntime`, or hardcoded game filters (`flame.mdl flame2.mdl`) in `QuakeRaySDK/editor/` | **Rejected everywhere** | Causes silent visual divergence between editor and game and doubles maintenance cost. |

---

## Single Shared Static Scene Pipeline (Mandatory Zero-Duplication Pattern)

Before writing host runtime code, understand the exact split introduced to keep `quakerayeditor` and the game renderer **100% identical on every static map element, material, light, DTAL emitter, overlay, and sky**:

### 1. Compile the Exact Same Shared Core into Both Targets
In the host CMake, compile the UI-free shared modules from `QuakeRaySDK/editor/` **and** your host format adapter sources into BOTH:
1. `quakerayeditor` (via `QR_EDITOR_ADAPTER_SOURCES` / `QR_EDITOR_ADAPTER_INCLUDE_DIRS` — `QuakeRaySDK/editor/CMakeLists.txt` already includes all shared modules), AND
2. Your runtime renderer target (`renderer_qray`) and parity test target (`qray_authored_tests`):
   - `<Game>EditorAdapter.cpp`, `<Game>Bsp.cpp`, `<Game>Model.cpp`, `<Game>Shader.cpp` (host adapter)
   - `EditorSession.cpp`, `EditorMaterial.cpp`, `EditorSettings.cpp`, `EditorSpatial.cpp`
   - `EditorOverlayPipeline.cpp`, `EditorDtalPipeline.cpp`, `EditorSceneRuntime.cpp`
   - `EditorLightSelection.cpp`, `Scene.cpp`, `Assets.cpp`

None of those shared runtime files include Dear ImGui, X11, or Win32 windowing — only `EditorApp` / `EditorPanels` / `EditorGui` / `EditorUi` / `EditorPlatform*` depend on GUI libraries.

### 2. Hook Live Engine VFS Instead of Writing a Second Asset Loader
At map init, mount game/mod directories into `EditorAssetSource` **and** bind the host engine's VFS via `EditorAssetSource::SetVfsCallbacks(EditorAssetVfsCallbacks)` (`exists`, `readFile`, `listFiles`):
- `quakerayeditor` resolves assets from loose directories + engine archives (`.pak`, `.pk3`, `.kpf`, `.wad`, etc.).
- The game runtime (`renderer_qray`) automatically falls back to the active engine VFS (pack order / pure checks) through the exact same `EditorAssetSource` API without duplicating any loader logic.

### 3. Call Sequence Shared by Editor & Runtime (`EditorSceneRuntime`)

Both `EditorApp` (`m_runtime`) and a runtime host (`sceneRuntime`) drive map loading and per-frame scene uploads through the same methods on `EditorSceneRuntime`:

```cpp
// --- Map Load (Identical sequence in EditorApp and runtime host) ---
sceneRuntime.DestroyGpuResources(qrayInstance);

EditorSceneLoadOptions loadOpts;
loadOpts.assets = &gameAssets;
// Editor uses default (true) to bake doors/platforms statically;
// Runtime sets false because gameplay updates *1..*N brush submodels dynamically.
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

// CreateTextureMaterials() ALREADY invokes CreateScrollingSkyMaterials(), which calls
// DestroySkyMaterials() + CreateSkyboxMaterials(). Do NOT call CreateSkyboxMaterials()
// or CreateScrollingSkyMaterials() again here: a direct CreateSkyboxMaterials() call
// overwrites skyboxMaterials[0..5] without destroying existing handles (leaking 6 GPU
// materials) and decodes skyboxes 3x.
sceneRuntime.CreateTextureMaterials(qrayInstance, gameAssets, logFn);
sceneRuntime.UploadStaticGeometry(qrayInstance, gameAssets, fallbackMaterial, logFn);
// Note: UploadStaticGeometry triggers BuildEmissiveLights(...) cleanly inside EditorSceneRuntime.
sceneRuntime.UploadWorldClusters(qrayInstance, logFn);
```

```cpp
// --- Per-Frame (Static + Sky/Overlays shared; Runtime layers dynamics on top) ---
sceneRuntime.AdvanceCelestialClock(deltaSeconds);
sceneRuntime.UploadSun(qrayInstance, logFn);
sceneRuntime.UploadLights(qrayInstance, timeSeconds, /*animateLightStyles=*/true, logFn);
sceneRuntime.UploadEmissiveLights(qrayInstance, timeSecondsF, &frameDynamicEmissiveLights, logFn);

EditorClusterLightUploadExtras extras;
extras.extraQuakeUnitSources = &dynamicDlightClusterSources;
extras.extraDynamicEmissiveLights = &frameDynamicEmissiveLights;
sceneRuntime.UploadClusterLightSources(qrayInstance, &extras, logFn);

sceneRuntime.UploadWorldOverlays(qrayInstance, gameAssets, camBasis, timeSecondsF);
// ... host uploads moving *1..*N submodels, dynamic models, sprites, beams, decals, smoke ...
sceneRuntime.UploadSky(qrayInstance, viewerPos, timeSeconds, skyDistance);

sceneRuntime.PopulateDrawFrame(qrayInstance, viewerPos, width, height, frameParams, drawInfo);
EditorDrawFrame(qrayInstance, &drawInfo); // One unified Quake-unit -> meters boundary
```

### 4. What Stays Exclusive to the Game Runtime Layer
The host runtime renderer must **only** own runtime-specific dynamics that do not exist in a static map preview:
- **Moving inline brush submodels (`*1..*N`):** With `includeInlineSubmodels = false`, the proven pattern across id Tech / GoldSrc runtimes is to upload moving doors, platforms, trains, and rotators each frame as `QR_GEOMETRY_TYPE_DYNAMIC` transformed into world space with per-vertex nudged PVS clusters (critical because brush entity origins often sit at `(0, 0, 0)` inside solid brushwork) and without applying alias-model (MD3/MD2) winding reversal. Use `QR_GEOMETRY_TYPE_STATIC_MOVABLE` + `qrUpdateGeometryTransform` only when a submodel is pre-uploaded in local space, its lighting cluster stays valid across motion, and its transform determinant sign never flips.
- **Live animated entities (`.md3` / `.md2` / `.mdl` / `.mdc` / `.mds` / `.md5mesh`):** Resolve authored material overrides through `sceneRuntime.ResolveDynamicMaterial(...)` so dynamic models honor `qray.materials.yaml` identically to static brushes:
  - **NEVER call `sceneRuntime.dynamicMaterialBakes.BeginPass()` / `EndPass()` per frame in a runtime host!** `ResolveDynamicMaterial` (`editor/EditorSceneRuntime.cpp`) returns cached `QrMaterial` handles from `m_dynamicMaterialCache` without marking `entry.used = true` in `dynamicMaterialBakes`; calling `EndPass(instance)` destroys every cached dynamic material on frame 2, causing dangling GPU handles. Let `DestroyGpuResources` release dynamic bakes cleanly on map unload/teardown.
  - Check `dynMat.emitRayOnly`: set `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` when true (and clear it when DTAL handles emission).
  - For dynamic meshes that emit authored DTAL area lights, populate `EditorDynamicMeshDtalRequest` with **world-space Quake-unit** vertices/indices, a resolved `cluster`, a per-frame `budgetRemaining` counter, and a disjoint `nextUniqueId` counter, then call `BuildEditorDynamicMeshAreaLights(req, frameDynamicEmissiveLights)` and pass the result via `EditorClusterLightUploadExtras::extraDynamicEmissiveLights`.
  - When dynamic models use authored glass (`EditorMaterialType::Glass`), convert glass thickness (`layerColors[2].data[1]`) to meters at the spatial boundary, and pass `QrGeometryUploadInfo::clusterOverride` when entity vertices need a single explicit nudged cluster.
- **Entities without static presence:** Sprites, beams/trails, bullet marks/decals, lit particles, volumetric smoke, dynamic weapon/explosion `dlights` (passed via `EditorClusterLightUploadExtras`), and 2D `SWAPCHAIN` HUD/console/menus.
- **Disjoint `uniqueID` allocation:** Allocate all host dynamic geometry and light IDs outside `EditorSceneRuntime`'s reserved ranges (see §1.2).

> **Hard Rule for Future Agents:** If you fix a bug or add a feature in BSP loading, material baking (`EditorMaterial`), DTAL generation (`EditorDtalPipeline`), shader overlays (`EditorOverlayPipeline`), light persistence (`EditorSession`), sky/sun, or render settings (`EditorSettings`), **put it in the shared layer in `QuakeRaySDK/editor/` or the single host adapter**. Never patch it only in `EditorApp` or only in the runtime host.

---

## Part 1 — Connecting & Embedding QuakeRaySDK in a Host Engine (Runtime)

### Minimal Bring-Up Checklist (Recommended Order)

Bring up a new engine integration in six incremental gates so failures stay isolated:

| Step | Milestone | Exit Criterion |
|---|---|---|
| **Step 0** | Non-GL window + `qrCreateInstance` + `qrSetSpatialConvention` + 2D `SWAPCHAIN` UI (`renderUiOnly = QR_TRUE`) | Menu/console renders right-side-up in Vulkan Y-down NDC with `0` validation errors and clean exit/resize. |
| **Step 1** | Single `EditorFormatAdapter` + `EditorSceneRuntime` static world & materials (hook live VFS via `SetVfsCallbacks`) | Static map geometry, textures, skybox/procedural sky, and `quakerayeditor` view match 1-to-1. |
| **Step 2** | PVS cluster table (`visOffsets` 1-shifted, `visData` 0-based, `<= 8192` clusters) + solid-leaf nudging | `qrGetClusterLightStats` reports `unresolved == 0` and realistic cluster grants across corridors and doors. |
| **Step 3** | Dynamic entities, moving `*1..*N` brush submodels, first-person viewmodel (`FIRST_PERSON`) + shadow body (`FIRST_PERSON_VIEWER`), dynamic DTAL | Doors/platforms light cleanly, characters have smooth normals (`flags = 0`), and frame 2+ keeps dynamic materials intact. |
| **Step 4** | Runtime FX: decals before explosions, sprites/beams, lit `PARTICLE` / volumetric `SMOKE`, `lightStyleScales`, underwater `QR_MEDIA_TYPE_WATER`, async readback | Decals, explosions, smoke, animated lightstyles, underwater refraction, and screenshots work without stalls or UAF. |
| **Step 5** | Parity & hardening: headless `QR_STATIC` parity test, paired `quakerayeditor` vs runtime smoke test, `VK_LAYER_KHRONOS_validation` (`0` VUIDs), `Play Level` | 1-to-1 static counts, clean `vid_restart`, zero validation/ASan findings, `Play Level` round-trips cleanly. |

### 1.1 Submodule, CMake, Compiler, Linker, & `dlopen` Packaging

Mount `QuakeRaySDK` inside the host repository (for example as a submodule at `code/quakeraysdk`) and configure it cleanly:

1. **Patch NVRHI:** Stock NVRHI limits `c_MaxBindingLayouts` to 8; QuakeRay's RT pipelines require at least 13. Apply `cmake/nvrhi-max-binding-layouts.patch` (which raises `c_MaxBindingLayouts` to 16) before building `nvrhi` and `nvrhi_vk`:
   ```bash
   git -C /path/to/nvrhi apply /path/to/QuakeRaySDK/cmake/nvrhi-max-binding-layouts.patch
   ```
   Configure NVRHI explicitly before adding it: `NVRHI_BUILD_SHARED OFF`, `NVRHI_WITH_VULKAN ON`, `NVRHI_WITH_DX11 OFF`, `NVRHI_WITH_DX12 OFF`, `NVRHI_FETCH_VULKAN_HEADERS OFF`.
2. **Vulkan Headers `>= 1.4.318`:** Older distro Vulkan headers fail NVRHI compilation. Supply modern Vulkan-Headers `>= 1.4.318` ahead of system include directories using `BEFORE PUBLIC` (and override `Vulkan_INCLUDE_DIR` / `Vulkan::Headers` before `add_subdirectory`).
3. **Build FidelityFX Provider:** Install `glslangValidator` first (required by the Linux build script), then build `libamd_fidelityfx_vk.so` (Linux) via `Source/FidelityFX_SDK_1.1.4/linux/build_provider.sh` or `amd_fidelityfx_vk.dll` (Windows) via `Source/FidelityFX_SDK_1.1.4/windows/build_provider.ps1`, and pass `-DQR_FFX_VK_SO=...` / `-DQR_FFX_VK_RUNTIME_DLL=...`. Whenever any FidelityFX header under `Source/FidelityFX_SDK_1.1.4/` changes, rebuild this provider shared library and redeploy it beside all executables/modules.
4. **C89/C99 Host Projects & C++20 Interop:** Classic C engines (`project(... LANGUAGES C)`) must enable `CXX` (`cxx_std_20`) and gate C-only compiler flags (`-Wstrict-prototypes`, `-Wimplicit-function-declaration`, `-Wold-style-definition`, etc.) with `$<$<COMPILE_LANGUAGE:C>:...>`. When a C++20 translation unit includes legacy C99 engine headers, wrap the `#include` block with `#define restrict __restrict__` (where needed) and `#undef min`, `#undef max`, `#undef random`, `#undef crandom` immediately afterwards so `<algorithm>` / `<random>` stay intact.
5. **PIC, IPO, & `dlopen` Renderer Module Packaging (`.so` / `.dll`):**
   - Enable `CMAKE_POSITION_INDEPENDENT_CODE ON` and temporarily set `CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF` around `nvrhi` and `QuakeRaySDK`.
   - For dynamically loaded renderer modules (`renderer_qray.so`), set `PREFIX ""`, `C_VISIBILITY_PRESET hidden`, `CXX_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN ON`, `LINKER:--exclude-libs,ALL`, `LINKER:--no-undefined`, and `BUILD_RPATH "$ORIGIN"` / `INSTALL_RPATH "$ORIGIN"`.
   - Stage `libamd_fidelityfx_vk.so` directly in the binary/module directory (or link with a bare soname) so `DT_NEEDED` contains no `/` directory prefix.
   - Locate `shaders/` and `textures/` relative to the shared module's directory using `dladdr()` on POSIX (or `GetModuleFileNameW` on Windows) rather than relying on the current working directory.
   - Keep a clean OpenGL renderer fallback if `dlopen` / `LoadLibrary` fails on machines without Vulkan RT support.
   - On minimal Linux build environments lacking `libxi-dev`, either install `libxi-dev` or supply a minimal `XInput2.h` stub if only the editor/headless build references it.
   - **Non-CMake hosts (Meson, Waf, Autotools, Make):** Build `QuakeRaySDK` + `nvrhi` + the shared UI-free `editor/` sources as a CMake sidecar producing static libraries (`QR_STATIC`) and link them into the host build system.
6. **Configure Editor Adapter & AI Worker (when `QR_BUILD_EDITOR=ON`):** Set `QR_EDITOR_ADAPTER_SOURCES`, `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and `QR_EDITOR_SHADER_DIR` (and optionally `-DQR_GGML_SOURCE_DIR=/path/to/ggml` to build the isolated `quakeray_ai_worker` executable linked statically against `ggml-vulkan` + `ggml-cpu`) before `add_subdirectory("${QUAKERAY_SDK_DIR}" ...)` so `quakerayeditor` compiles cleanly without placing game-specific files inside `QuakeRaySDK`.
7. **Deploy Bundled Runtime Assets Next to Binaries:** `BlueNoise_LDR_RGBA_128.png` and `WaterNormal_n.png` are bundled under `QuakeRaySDK/Assets/textures/`, with automatic per-file fallback in `CMakeLists.txt` (`QR_TEST_TEXTURE_DIR`) and `editor/CMakeLists.txt` (alongside `fonts/Roboto-Regular.ttf` and `fonts/fa-solid-900.ttf`). Ensure `shaders/`, `textures/`, `fonts/`, `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`, and (if enabled) `quakeray_ai_worker` sit beside your executables. When distributing game data packs, use the host engine's **native** mounted archive format (`.pk3`/`.pk3dir` for id Tech 3 / RtCW, `.pkz`/`.pak`/`.kpf` for Quake 1 / Quake II, loose mounted data / `.wad` for GoldSrc — id Tech 3 engines ignore `.pkz`).
8. **Shader Rebuilds:** Whenever modifying HLSL shaders under `Source/Shaders/`, rebuild strictly from inside `Source/Shaders/` (`python3 -B CheckMatrixReads.py && python3 -B GenerateShaders.py -rebuild`) and verify deployed `.spv` copies match `Build/*.spv` (see Part 4 §1).

#### Copy-Pasteable CMake & Linker Template (Runtime `.so` + Editor Adapter)

```cmake
# 1. Enable C++20 alongside C and gate C-only flags
enable_language(CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 2. Override Vulkan headers (>= 1.4.318) before NVRHI/SDK if distro headers are older
set(Vulkan_INCLUDE_DIR "${VULKAN_HEADERS_DIR}/include" CACHE PATH "" FORCE)

# 3. Configure NVRHI & QuakeRaySDK with IPO temporarily off
set(_saved_ipo "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)

set(QR_NVRHI_SOURCE_DIR "${NVRHI_DIR}" CACHE PATH "" FORCE)
set(QR_FFX_VK_SO "${FFX_DIR}/libamd_fidelityfx_vk.so" CACHE FILEPATH "" FORCE)
set(QR_BUILD_EDITOR ON CACHE BOOL "" FORCE)
set(QR_EDITOR_ADAPTER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/qray/editor/MyGameAdapter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/qray/editor/MyGameBsp.cpp"
    CACHE STRING "" FORCE)
set(QR_EDITOR_ADAPTER_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src/qray/editor" CACHE STRING "" FORCE)
add_subdirectory("${QUAKERAY_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/quakeraysdk")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION "${_saved_ipo}")

# 4. Runtime renderer module (dlopen .so) sharing exact adapter + UI-free SDK editor core
add_library(renderer_qray MODULE
    src/qray/qray_host.cpp
    ${QR_EDITOR_ADAPTER_SOURCES}
    "${QUAKERAY_SDK_DIR}/editor/Assets.cpp"
    "${QUAKERAY_SDK_DIR}/editor/Scene.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorSession.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorMaterial.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorSettings.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorSpatial.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorLightSelection.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorOverlayPipeline.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorDtalPipeline.cpp"
    "${QUAKERAY_SDK_DIR}/editor/EditorSceneRuntime.cpp")
target_compile_features(renderer_qray PRIVATE cxx_std_20)
target_include_directories(renderer_qray BEFORE PUBLIC "${VULKAN_HEADERS_DIR}/include")
target_include_directories(renderer_qray PRIVATE "${QUAKERAY_SDK_DIR}/editor")
target_compile_definitions(renderer_qray PRIVATE
    QR_STATIC
    $<IF:$<BOOL:${WIN32}>,QR_USE_SURFACE_WIN32,QR_USE_SURFACE_XLIB>)
target_link_libraries(renderer_qray PRIVATE qray::renderer)
set_target_properties(renderer_qray PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
if(UNIX AND NOT APPLE)
    target_link_options(renderer_qray PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
    # Copy libamd_fidelityfx_vk.so beside the module so DT_NEEDED resolves via $ORIGIN
    add_custom_command(TARGET renderer_qray POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QR_FFX_VK_SO}" "$<TARGET_FILE_DIR:renderer_qray>/libamd_fidelityfx_vk.so")
endif()
```

### 1.2 Windowing, Instance Ownership, Frame Lifecycle, Parameter Contracts, & Spatial Boundary

#### Windowing, GL Suppression, Wayland, X11 Macro Isolation, & Present Mode
- **Supported window surfaces:** The SDK currently supports Win32 (`QR_USE_SURFACE_WIN32`) and Xlib (`QR_USE_SURFACE_XLIB`) only — **there is no native Wayland surface**. On Wayland Linux desktops, force XWayland with `SDL_VIDEODRIVER=x11` (including in `EditorLaunchCommand::envOverrides` returned by `buildPlayLevelCommand`).
- **Suppress OpenGL / GLX / WGL contexts:** Never attach an OpenGL context (`SDL_WINDOW_OPENGL`, legacy `REF_GL`) to the window handed to `qrCreateInstance`, or Vulkan swapchain creation fails. Create a Vulkan/plain window (`SDL_WINDOW_VULKAN`, or a software-window init path on engines whose video subsystem otherwise creates GL) and skip all GLX/WGL/`SwapBuffers` calls while the QuakeRay renderer is active.
- **Isolate X11 headers:** `<X11/Xlib.h>` and `<SDL_syswm.h>` define unprefixed macros that break C++ enums and NVRHI headers. Include them in a single dedicated translation unit and immediately `#undef None Bool True False Status Success Always Convex Complex TileShape`.
- **`QR_PRESENT_MODE_IMMEDIATE` fallback:** On Linux drivers/compositors that do not expose `VK_PRESENT_MODE_IMMEDIATE_KHR`, `qrStartFrame` returns `QR_FEATURE_UNAVAILABLE` (producing black frames and log spam if unhandled). Catch `QR_FEATURE_UNAVAILABLE` on `QR_PRESENT_MODE_IMMEDIATE` and retry with `QR_PRESENT_MODE_VSYNC` and `maxFrameLatency = 1`.

#### Instance Ownership, Shared GPU Context, & Spatial Boundary
- Create **one** `QrInstance` per window (`qrCreateInstance` or `qray::qrCreateInstanceWithGpuRequirements`). Never create a secondary Vulkan/NVRHI device or swapchain for the same target.
- **Shared/external `VkDevice` rules (see [gpu-context.md](gpu-context.md) & [host-driver-contract.md](host-driver-contract.md)):** The SDK always creates and owns the single Vulkan/NVRHI device. C++20 hosts needing custom device extensions, features, queues, or formats must negotiate them upfront via `qray::qrCreateInstanceWithGpuRequirements`, obtain borrowed NVRHI/Vulkan handles via `qrGetGpuContext`, and synchronize host queue work cleanly around SDK frame boundaries.
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
- **Mirrored spatial frames (`det < 0`):** If `spatialFrame.IsMirrored()` (for example a left-handed host mapped onto right-handed scene space), `qrUploadWorldLights` requires **both** `pFaces` and `pFaceVertices` (`Source/VulkanDevice.cpp`) so the SDK can reverse face vertex winding; passing only one throws `QR_WRONG_ARGUMENT`.
- **Keep unit conversion at one strict boundary:** QuakeRay's canonical editor/host convention is `40 Quake units = 1 meter` (`kEditorUnitsPerMeter = 40.0f`, `0.025f` m/unit). Convert **all** world positions, bounding boxes, light radii, reach, area-light corners/areas, camera position, near/far planes, `rayLength`, glass thickness, and fog distances to meters in a single spatial conversion layer. Never convert coordinates twice, and keep `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` UI draws in **pixels**.
- **Exact Quake-axis -> `QrDrawFrameInfo::view` formula (column-major):** For a Quake camera at `origin` (converted to meters) with orthonormal basis `axis[0] = forward`, `axis[1] = left`, `axis[2] = up`, derive OpenGL/Vulkan view axes `right = -axis[1]`, `up = axis[2]`, `back = -axis[0]`:
  ```cpp
  // Column-major 4x4: view[col * 4 + row]
  const float r[3] = { -axis[1][0], -axis[1][1], -axis[1][2] }; // +X right
  const float u[3] = {  axis[2][0],  axis[2][1],  axis[2][2] }; // +Y up
  const float b[3] = { -axis[0][0], -axis[0][1], -axis[0][2] }; // +Z backward (-forward)
  view[0]  = r[0]; view[4]  = r[1]; view[8]  = r[2]; view[12] = -(r[0]*pos[0] + r[1]*pos[1] + r[2]*pos[2]);
  view[1]  = u[0]; view[5]  = u[1]; view[9]  = u[2]; view[13] = -(u[0]*pos[0] + u[1]*pos[1] + u[2]*pos[2]);
  view[2]  = b[0]; view[6]  = b[1]; view[10] = b[2]; view[14] = -(b[0]*pos[0] + b[1]*pos[1] + b[2]*pos[2]);
  view[3]  = 0.0f; view[7]  = 0.0f; view[11] = 0.0f; view[15] = 1.0f;
  ```
- **Core SDK numerical & ABI invariants:** Shaders normalize metric vectors via `safeNormalize`, square perceptual roughness (`alpha = roughness^2`) inside GGX, enforce fixed-width `uint32_t` enums via `QR_ENUM_UINT32` (safe under Clang `-fsanitize=enum`), and enforce a `25 µm` (`2.5e-5 m`) ray clearance / `TMin` floor (`Source/Shaders/RayClearance.hlsli`).

#### Mandatory `QrInstanceCreateInfo` & `QrDrawFrameInfo` Contracts & Radiometric Calibration

Zero-initializing structs or passing `nullptr` for optional sub-parameter blocks triggers subtle defaults that produce black screens, blown-out highlights, flat normals, or broken lighting:

| Scope & Field | Required Value / Contract | What Breaks If Wrong |
|---|---|---|
| `createInfo.allowGeometryWithSkyFlag` | `QR_TRUE` when using `QR_GEOMETRY_VISIBILITY_TYPE_SKY` | Sky surfaces fail or throw `QR_WRONG_ARGUMENT` (mutually exclusive with `WORLD_2` / `RAY_CULL_WORLD_2_BIT`). |
| `createInfo.pShaderFolderPath` | Path **ending with trailing `/`** | Shader path concatenation fails to find `.spv` binaries. |
| `createInfo.original*TextureIsSRGB` | Albedo = `QR_TRUE`; RME = `QR_FALSE`; Normal = `QR_FALSE` | sRGB-decoding RME/Normal corrupts roughness, emission, and tangent normals. |
| `createInfo.pbrTextureSwizzling` | `QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE` | Swaps R/M/E channels across all materials. |
| `createInfo.rasterizedVertexColorGamma` | `QR_TRUE` | Classic 8-bit sRGB vertex colors wash out on raster overlays/HUD. |
| `createInfo.rasterizedSkyCubemapSize` | `> 0` (e.g. `512` or `1024`) | Sky cubemap baking disabled. |
| `createInfo.pBlueNoiseFilePath` | Strictly `128x128x64` `R8G8B8A8_UNORM` (`BlueNoise_LDR_RGBA_128.png`) | Instance creation fails (`QR_ERROR_CANT_FIND_BLUE_NOISE` or format mismatch). |
| `drawInfo.rayCullMaskWorld` | Non-zero bitmask including `WORLD_0` (and `SKY` if enabled) | `0` blanks the entire ray-traced world. |
| `drawInfo.rayLength` | Full map diagonal in meters (`>= max(4096, mapDiag * 2.5) / 40`) | `0` clamps to `0.0025 m` (black/clipped world). |
| `drawInfo.worldUpVector` | Explicit host up axis (`{0, 0, 1}` for Z-up Quake) | `{0, 0, 0}` silently falls back to Y-up `(0, 1, 0)`, rotating sky/sun/water. |
| Swapchain / `pRenderResolutionParams->customRenderSize.width` | **Even integer** (`width % 2 == 0`) | `RenderResolutionHelper::Width()` rounds odd widths up (`w + w % 2`) to satisfy `gu->renderWidth % 2 == 0`, causing a 1-px mismatch on odd widths. |
| `drawInfo.currentTime` | Monotonically advancing seconds | Time jumps `> 1 s` or camera teleports `> 2.5 m` reset eye adaptation. |
| `drawInfo.pTonemappingParams` (+ `drawInfo.disableEyeAdaptation`) | **Non-null explicit AgX** (`tonemapType = 4`, `tonemapPower = 0.9f`, `luminanceWhitePoint = 20.0f`, `minLogLuminance = -8.0f`, `maxLogLuminance = 0.0f`, `exposureBias = 0.0f`; set `drawInfo.disableEyeAdaptation = QR_TRUE` directly on `QrDrawFrameInfo` for deterministic tests) | `nullptr` leaves `sky.exposureParams` zeroed, clamping `tmWhitePoint` to **`1.05f`** (`Source/Tonemapping.cpp`) and `chromaPreservation = 0` — crushing HDR highlights! |
| `drawInfo.pBloomParams` | **Non-null** with `bloomIntensity = 0.0f`; use `postEffectParams.pBloom` instead | `nullptr` activates legacy bloom (`bloomIntensity = 1.0f`, `bloomEmissionMultiplier = 64.0f`), massively over-blooming emissives. |
| `drawInfo.pTexturesParams` | If non-null, initialize `normalMapStrength`, `emissionMapBoost`, **all 64 `lightStyleScales` to `1.0f`**, and `samplerAnisotropy` (`0` = default 16x clamped to device limit, `1` = off, `2`/`4`/`8`/`16`) | Zero-initialized struct zeroes normal maps, emission, and every lightstyle; setting `samplerAnisotropy = 1` disables anisotropic filtering! |
| `drawInfo.pIlluminationParams` | Explicit `cellWorldSize = 2.0f` (meters), `giBounceRays = 1.0f`, `restirEnabled = QR_FALSE` | `nullptr` sets `cellWorldSize = 0.025 m` instead of `2.0 m`. |
| `pSkyParams` radiometry | `skyColorMultiplier = 0.1f`, `skyLightMultiplier` in `0..8` | Uncalibrated values (e.g. `64`) saturate shadows/ASVGF; opaque raster skybox hides procedural sky behind it. |

#### Frame Lifecycle, `vid_restart`, Resource Caps, `uniqueID` Namespaces, & Config Shadowing
- **Strict per-frame lifecycle:** Call `qrStartFrame` (if a previous call aborted early and returns `QR_FRAME_WASNT_ENDED`, close/recover cleanly) -> optional `qrAcquireFrame` -> geometry, light, raster, and sky uploads -> `qrDrawFrame`.
  - Raster uploads outside a started frame are **silently dropped** (`Source/VulkanDevice.cpp`); `qrChangeAnimatedMaterialFrame` throws `QR_FRAME_WASNT_STARTED`.
  - `qrCreateMaterial` / `qrUpdateMaterial` calls issued before a frame queue into staging buffers and must be flushed before `qrDestroyInstance`.
- **`vid_restart`, teardown order, & 4096 texture limit:**
  - Always call `sceneRuntime.DestroyGpuResources(instance)` **before** `qrDestroyInstance(instance)` (or call `sceneRuntime.ResetGpuHandlesOnly()` if the instance was already destroyed).
  - Across `vid_restart`, recreate UI and lazy-cached `QrMaterial` handles while preserving engine handle (`qhandle_t`) tables so game VMs keep valid indices.
  - Always free per-map materials on map unload — leaking materials across level transitions hits the hard `TEXTURE_COUNT_MAX = 4096` cap (`Source/Const.h`).
- **One directional light per frame:** Guard `qrUploadDirectionalLight` so it runs at most once per frame (`Source/LightManager.cpp` throws `Only one directional light is allowed`).
- **Never copy `EditorFrameParams`:** `EditorFrameParams::postEffects` (`editor/EditorSettings.h`) stores self-referential pointers (`pBloom`, `pVignette`, `pFilmGrain`, `pSharpen`, `pCrt`) to sibling members inside the same struct. Copying `EditorFrameParams` leaves dangling pointers into the source object; populate it in place via `BuildEditorFrameParams` / `PopulateDrawFrame`.
- **`qrUpdateGeometryTransform` determinant sign flip:** `VulkanDevice::UpdateGeometryTransform` throws `QR_CANT_UPDATE_TRANSFORM` if a transform changes the determinant sign (reverses triangle winding). Re-upload the geometry instead of calling `qrUpdateGeometryTransform`.
- **Multiple `RenderScene` calls & `renderUiOnly`:** Classic engines often call `RenderScene` multiple times per frame for 3D HUD models (`RDF_NOWORLDMODEL`), skybox portals (`RDF_SKYBOXPORTAL`), or mirrors/cameras. Guard secondary calls so they never overwrite the main world camera, and set `drawInfo.renderUiOnly = QR_TRUE` on pure menu/console/loading frames.
- **Reserved `uniqueID` namespaces & duplicate behavior:**
  - Duplicate static geometry `uniqueID` throws `QR_WRONG_ARGUMENT`; duplicate **dynamic** geometry `uniqueID` in the same frame **silently drops the draw** (`Source/VulkanDevice.cpp`).
  - `EditorSceneRuntime`, `EditorOverlayPipeline`, `EditorDtalPipeline`, and `EditorApp` reserve the following ranges:

    | Reserved `uniqueID` Range | Owner |
    |---|---|
    | `1 .. N` | Static world surfaces (`surfaceIndex + 1`) |
    | `0x100000000ull + surfaceIndex + 1` | Cutout backing surfaces (`EditorSceneRuntime`) |
    | `0x50000000ull + (overlaySurfaceIndex << 8) + (stageIndex & 0xFF)` | Dynamic emissive overlays (`EditorOverlayPipeline`) |
    | `2` | Sun directional light (`EditorSceneRuntime::UploadSun`) |
    | `0xFFFFull + i` (`kEditorFirstLightUniqueID`) | Map point/spot lights |
    | `0x1FFFFull + i` (`kEditorFirstCustomLightUniqueID`) | Custom authored lights |
    | `0x2FFFFull` / `0x30000ull` | Editor torch / preview light |
    | `0x3FFFFull + i` (`kEditorFirstEmissiveLightUniqueID`) | Static DTAL emissive area lights |
    | `0x70000000ull+` / `0x71000000ull+` | Editor preview model & preview backing |
    | `(1ull << 60) \| ((surfaceIndex + 1) << 32) \| faceIndex` | DTAL proxy point lights (`EditorDtalPipeline.cpp`) |
    | `(2ull << 60) \| entityId` | Static BSP alias instances (`Scene.cpp`) |

  - **Host rule:** Allocate all runtime dynamic geometry, dynamic submodels, dynamic dlights, and dynamic mesh DTAL emitters in disjoint upper ranges (for example `>= 0x4000000000000000ull`).
- **Archived config cvar shadowing (`q3config.cfg` / `config.cfg`):** Engine cvars marked `CVAR_ARCHIVE` persist stale values in user configs, silently overriding updated code defaults (e.g. `restir`, `skyboxRadius`, `envShellScale`) and stomping editor-authored `config.cfg` on map load! Load authored map configs **first**, clear the `modified` flags on RT cvars immediately after map load, and per frame apply engine cvars with `onlyModified = true` so only explicit live user changes override authored scenes. For authored/custom UI testing and unbiased headless benchmarks, pass `+set sv_pure 0`, `+set fs_homepath <build_dir>`, and `+set com_maxfpsUnfocused 0`.

### 1.3 PVS Cluster Table, 8192 Cap, Solid-Leaf Nudging, & Cluster Light Budgets

Cluster visibility and light list partitioning are among the most subtle runtime contracts:

1. **Cluster `0` vs `QR_CLUSTER_LIGHT_NO_CLUSTER` (`~0u`):**
   - For geometry vertices (`QrVertex::cluster`) and world cluster arrays (`worldClusters`), cluster index `0` is reserved for solid/outside space: engine PVS cluster `c >= 0` maps to SDK cluster `c + 1` (`1 .. clusterCount - 1`), and solid/outside maps to `0`.
   - For light sources (`QrClusterLightSource::cluster` and `QrClusterLightSource::pClusters`), **"unclustered / resolve by position"** is **`QR_CLUSTER_LIGHT_NO_CLUSTER` (`~0u` / `0xFFFFFFFFu`, `Include/qray/qray.h`)** — **NOT `0`**! Passing `0` in `QrClusterLightSource::cluster` treats the light as belonging to cluster `0` rather than unclustered (`Source/ClusterLightLists.cpp`).
2. **Exact PVS Indexing Contract (`visOffsets` & `skyVis` 1-Shifted; `visData` Bits 0-Based):**
   - `visOffsets` is **1-shifted**: `visOffsets[0] = -1` (solid cluster `0` has no vis row), and engine cluster `c` (`>= 0`) stores its byte offset at `visOffsets[c + 1]`.
   - `skyVis` is **1-shifted**: bit `c` of byte `c / 8` is `1` when SDK cluster `c` can see the sky (`Source/WorldLights.h`).
   - Bits inside Quake zero-run RLE `visData` **MUST remain 0-based** (`bit b` -> SDK cluster `b + 1`), because `Source/ClusterLightLists.cpp` (`DecompressVis` consumer) **already adds `+ 1`** (`const uint32_t c = (j << 3) + k + 1;`). Shifting bits inside `visData` by `+ 1` double-shifts visibility to `c + 2` and breaks lighting across every corridor!
   - Trailing `0xFF` padding at the end of `visData` is **not required**: `DecompressVis` bounds-checks `pIn < pEnd` and zero-fills truncated rows (appending `0xFF` bytes falsely marks truncated tails visible).
   - Passing `pVisData = nullptr` falls back to `visUncompressed = true` (all clusters mutually visible).
3. **Hard SDK Cluster Cap (`Q2_MAX_CLUSTERS = 8192`) & Spatial Cluster Folding:**
   - `Source/Generated/ShaderCommonC.h` (`Q2_MAX_CLUSTERS = 8192`) and `Source/ClusterLightLists.cpp` clamp `numClusters` to `8192` — **surfaces in clusters `>= 8192` silently receive zero cluster lights!**
   - Engines whose BSPs index PVS by leaf (`BSP30` Half-Life, `BSP29`/`BSP2`, large maps exceeding 8191 leaves) **must** fold leaves into a spatial grid (`<= 8191` non-solid clusters) and remap PVS rows into the folded cluster space.
   - Populate `EditorWorldClusters` with leaf `0` as the solid sentinel (`kBspContentsSolid = -2`), shift engine leaves `i -> i + 1` and negative BSP node children by `-1`, and initialize empty-cluster `mins`/`maxs` to valid world bounds rather than `±1e30`.
4. **Accumulate Cluster AABBs from Geometry:** As static world triangles upload, accumulate `pClusterMins[cluster]` / `pClusterMaxs[cluster]` (in meters) so `qrUploadWorldLights` has valid world-space bounds for every non-zero cluster.
5. **Solid-Leaf Boundary Nudging (Critical):** Vertices, surface fixture centroids, and map point lights frequently lie right on a brush split plane or 1–2 units inside solid brushwork, causing a raw point-leaf lookup to return `-1` (`cluster = 0`) and drop out of PVS illumination. Implement a helper (`NearbyBspClusterForPoint(point)`) that probes `ClusterForPoint(point)` first and, if invalid, probes small offsets (`±1, ±2, ±4, ±8` Quake units) along the 6 axial directions (and optional diagonals) to recover the adjacent valid PVS cluster (`>= 1`).
6. **Cluster-Light Starvation, `kLightMinColorSum` Symmetry, Dynamic Power Units, & Why `rt_restir 1` Flashes:**
   - **Identical dim-light filtering:** Filter `!std::isfinite(power) || power <= 0.0f` and `r + g + b < LightManager::kLightMinColorSum` (`0.0001f`) **identically** before both `qrUploadSphericalLight` and `qrUploadClusterLightSources`. If a dim/zero dlight enters cluster lists while `LightManager::AddSphericalLight` discards it, cluster partitions and overflow-tail `beta` shift, causing a ~3% whole-scene brightness dip on every weapon shot.
   - **Dynamic cluster power units:** Scale dynamic cluster `power` by `kEditorUnitsPerMeter^2` (`1600.0f` at `40 u/m`) so dynamic dlights match static cluster light power units.
   - **Physical `reach` & zero double-emission:** Compute light `reach` physically from emitter intensity rather than using a fixed `1000u` (`25 m`) for every dim bulb, and never double-emit both a BSP point light and a DTAL emitter for the same fixture — overflowing `Q2_LIGHT_LIST_MAX_PER_CELL` (`128` slots) causes strobing and 4K FPS drops.
   - **Why `rt_restir 1` flashes:** The SDK's `rt_restir 1` mode is **1-frame uniform RIS** (no temporal/spatial reservoir reuse or visibility reuse). Adding or removing a transient weapon dlight (`N -> N + 1` lights) remaps RNG candidate indices on every 3×3 ASVGF gradient pixel, flashing the whole scene. Keep clustered light lists (`rt_restir 0`) as the default host path.

### 1.4 Static World, Moving Brush Submodels, Overlays, Materials, Lightstyles, & Diagnostics

| Scene Category | SDK Mechanism | Key Implementation Notes |
|---|---|---|
| **Static World Geometry** | `qrBeginStaticGeometries` -> `qrUploadGeometry` -> `qrSubmitStaticGeometries` | Group draws by material/pass-through type (`OPAQUE`, `ALPHA_TESTED`, `GLASS`, `WATER`, etc.). Guard `Begin`/`Submit` with RAII (`StaticGeometryGuard`) so early returns never leave static upload open. |
| **Inline Brush Submodels** (`*1..*N`: doors, plats, trains, rotators) | `includeInlineSubmodels = false` + per-frame `QR_GEOMETRY_TYPE_DYNAMIC` world-space upload (or `STATIC_MOVABLE` + `qrUpdateGeometryTransform`) | Proven dynamic pattern: upload moving `*1..*N` brush geometry every frame in world space as `QR_GEOMETRY_TYPE_DYNAMIC` with per-vertex nudged PVS clusters (critical when entity `origin` is `(0, 0, 0)` in solid space) and skip MD3/MD2 winding flips (`!isBspSubmodel`). Use `STATIC_MOVABLE` + `qrUpdateGeometryTransform` only for local-space slots whose cluster stays valid and whose determinant sign never flips. Skip invisible `func_areaportal` / trigger / origin brushes. |
| **Double-Sided (`cull none`) Traced Geometry** | Duplicated back-face triangles on `twoSided` surfaces | Under `rayCullBackFacingTriangles = QR_TRUE`, thin banners, grates, foliage, and arches vanish from behind. Duplicate vertices with negated normals and reversed triangle indices (`i0, i2, i1`) on all ray-traced `twoSided` geometry (world, dynamic models, preview). |
| **Multi-Stage Overlays, Stage Toggles & Mirror Promotion** | `qrUploadRasterizedGeometry` + `EditorMaterialParams` (`disabledStagesMask`, `mirror`) | Use `..._DEFAULT` for world animated/additive/blended passes, `..._SKY` for sky passes (`pViewProjection == nullptr`, `pViewport == nullptr`, zero scissor), and `..._SWAPCHAIN` for 2D HUD/console/menus (**Vulkan Y-down NDC**; never pass `DEPTH_TEST \| DEPTH_WRITE` on `SWAPCHAIN`). `disabledStagesMask` disables individual stages across both `EditorOverlayPipeline` and `EditorSceneRuntime`; `mirror = true` promotes cubemap / `isOverlayOnly` surfaces into traced mirrors. |
| **Animated & Updateable Materials** | `qrCreateAnimatedMaterial` / `qrChangeAnimatedMaterialFrame`; `QR_MATERIAL_CREATE_UPDATEABLE_BIT` | Use animated materials for `animMap` stages (must call `qrChangeAnimatedMaterialFrame` inside a started frame). Use `QR_MATERIAL_CREATE_UPDATEABLE_BIT` + `qrCanUpdateMaterialContents` / `qrUpdateMaterialContents` for cinematics/dynamic video textures; retain CPU `uiSourceRgba` buffers when rebuilding UI materials across texture-filter changes. |
| **Sky, Sun & Volumetric Light Shafts** | `QR_GEOMETRY_VISIBILITY_TYPE_SKY` + procedural sky + `RhiLightShaftVolumePass` | Mark sky surfaces so primary rays read the procedural AAA sky/celestial system ([sky-and-atmosphere.md](sky-and-atmosphere.md)) or cubemap/sun without blocking geometry. Froxel volumetric light shafts use depth-bounds culling (`CmLightShaftDepthBounds`), tightened froxel grids (`128x72x64` / `192x108x96` / `256x144x128`), and stochastic multi-light selection. |
| **Relief POM, Auto-Mirror, PBR Toggles & Bump/Gloss Quality** | `texture_height` (`EditorTextureSlot::Height`), `parallax_scale` (`layerColors[2].data[2]`), `EditorMaterialParams` (`emissiveEnabled`, `glossEnabled`, `sssEnabled`) | Relief POM (`setupParallaxGeometry` in `Source/Shaders/HitInfo.hlsli`) runs only when a normal map is bound, layer-2 albedo is unbound (`materials[2][MATERIAL_ALBEDO_ALPHA_INDEX] == MATERIAL_NO_TEXTURE`), the surface is not glass/water/SSS, and `parallax_scale` (clamped `[0, 0.25]`) `> 1e-4`. `ResolveBakedNormalAlpha` packs `Normal.a` in strict priority: active SSS mask -> glass `transparency` -> `texture_height` luminance -> `255`. *(Current SDK caveat: `EditorMaterialBakes::InvalidateTexture` matches `albedo, gloss, normal, emissive, overlay, sssTex` and does not yet check `k.heightTex`, so regenerating a height map in place needs explicit bake invalidation.)* `IsChromeCapableMaterial` (`editor/EditorMaterial.cpp`) automatically promotes opaque materials with `glossEnabled` and explicit gloss or `roughnessOverride < 0.02f` to `QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR` with `REFL_REFR_ALBEDO_MULTIPLY_BIT`. |
| **Specular Anti-Firefly & 5 Causes of Weak Bump/Gloss** | `q2RegularizeDemodulatedSpecular`, `q2AntiFirefly`, Toksvig/LEADR (`BRDF.hlsli`, `CmQ2Adapter.comp.hlsl`, `HitInfo.hlsli`) | Avoid the 5 common causes of flat bump/gloss: (1) downsampling high-res normal/gloss maps to low-res albedo (`ComputeBakeTargetDimensions` preserves up to `2048 px`); (2) positive sampler `mipLodBias` blurring normals; (3) `CmQ2Atrous.comp.hlsl` blending ultra-rough specular into diffuse+indirect across `FAKE_ROUGH_SPECULAR_THRESHOLD` (`0.62`) to `0.62 + 0.22 = 0.84` (`ShaderCommonHLSLFunc.hlsli`); (4) sub-pixel Toksvig/LEADR (`HitInfo.hlsli`, scaled by `min(normalMapStrength, 1.5)^2` when normal footprint > 1 px); and (5) AI gloss synthesis (`SynthesizeGlossFromDiffuseAndHeight`) stretching 2nd–98th percentiles into `[0.10, 0.92]` on flat inputs. Keep `minRoughness` and `normalMapStrength` physically accurate. |
| **HiDPI Inspector Layout Scaling** | `EditorGuiScale`, `EditorGuiFitButtonWidth`, `EditorGuiComboWidth`, `EditorGuiButtonRow` (`editor/EditorGui.h`) | Editor controls scale uniformly across 1080p (`1.0x`) and 4K (`1.5x`) viewports so combos, checkboxes, sliders, modal prompts, and action rows never truncate or overlap. |
| **Transparency & Cutout Policy** | `ALPHA_TESTED`, `GLASS_REFLECT_REFRACT`, `SSS`, `ALPHA_TRANSMISSION_BIT` | `RtAlphaTest.rahit.hlsl` discards cutouts when `(r + g + b) / 3 * a + a < 0.5` (bright white texels pass down to `alpha = 0.25`; black requires `0.5`). `QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT` remains active in `VertexCollector.cpp` and `RtAlphaTest*.rahit.hlsl` for stochastic foliage/fences, though binary `ALPHA_TESTED`, physical `GLASS`, or `SSS` are preferred to avoid high path-tracing variance. |

#### 2D `SWAPCHAIN`, `SKY`, `TEXCOORD_TRANSFORM`, Blend Limits, Lightstyles, Media, & Diagnostics
- **2D `SWAPCHAIN` projection (Vulkan Y-down NDC):** Unlike OpenGL Y-up NDC, `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` uses Vulkan top-down NDC (`proj[0] = 2/W`, `proj[5] = +2/H`, `proj[12] = -1`, `proj[13] = -1`). Never set `QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST` or `QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE` on `SWAPCHAIN` draws (throws `QR_WRONG_ARGUMENT`).
- **`SKY` overlays & unsupported blend ops:** `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY` requires `pViewProjection == nullptr`, `pViewport == nullptr`, and zero scissor. Across all raster draws, `DST_ALPHA`, `ONE_MINUS_DST_ALPHA`, subtract/min/max blend ops, and separate alpha blend factors are unsupported (`Include/qray/qray.h`).
- **`TEXCOORD_TRANSFORM` required for `texCoordOffset`:** `QrRasterizedGeometryUploadInfo::texCoordOffset` (for `tcMod` scrolling/transforms) is **silently ignored** unless `QR_RASTERIZED_GEOMETRY_STATE_TEXCOORD_TRANSFORM` (`256`) is set in `pipelineState` (`Source/RasterizedDataCollector.cpp`). Note also that `RsWorld.frag.hlsl` tints `outScreenEmission` by `saturate(color.rgb * vertColor.rgb)`.
- **Runtime lightstyles, underwater media, & first-person visibility masks:**
  - Feed animated lightstyle tables into `frameParams.textures.lightStyleScales[0..63]` (`QR_LIGHT_STYLE_COUNT`) paired with 1-based `QrVertex::lightStyles` (dimming range `< 255.5 / 256`).
  - Set `reflectRefract.typeOfMediaAroundCamera = QR_MEDIA_TYPE_WATER` when the camera is submerged (`RDF_UNDERWATER` / water contents).
  - Tag first-person viewmodel weapons with `QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON`, and tag the player's own body (shadow/reflection-only, invisible to primary rays) with `QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER` (or skip third-person player entities when first-person).
- **Gameplay queries, cvar forwarding, & runtime diagnostics:**
  - Implement engine gameplay queries (`R_LightPoint`, `MarkFragments`) directly over `EditorSceneRuntime` (`scene.lights`, `scene.surfaces`) instead of keeping a separate CPU BSP copy.
  - Forward host `rt_*` console cvars into `EditorRenderSettings` via `ApplyEditorGlobalSettingText`.
  - Use `qrGetFrameStatsEx` and `qrGetClusterLightStats` to verify PVS resolution (`unresolved == 0`), cluster grants/denials, and active cluster counts.
  - Note DTAL coplanar merging (`BrushDtalCluster` in `editor/EditorDtalPipeline.cpp`) and default `rt_dtal_proxy = false` (`editor/EditorSettings.h`).

### 1.5 Runtime Decals, Sprites, Translucent FX, Particles, Smoke, Smooth Normals, & HDR Overlay Blending

Every classic engine hits the same visual traps when routing combat effects, decals, pickups, and dynamic entities through a physically based HDR path tracer. Follow these seven rules verbatim:

1. **Never Send Translucent Sprites, Beams, or Explosion Models to `qrUploadGeometry`:**
   - `qrUploadGeometry` is strictly for ray-traced opaque geometry, `ALPHA_TESTED` cutouts (`(r + g + b) / 3 * a + a >= 0.5`), mirrors, and physical dielectric refraction (`GLASS_REFLECT_REFRACT` / `WATER`).
   - Submitting sprite sheets (`.sp2`, `.spr`, Q3 sprites), beams/lasers, muzzle flashes, or translucent explosion models (`RF_TRANSLUCENT`, `r_explode`, `bulletExplosion`, `rocketExplosion`) to `qrUploadGeometry` turns sprites into shadow-casting black/cutout cards and turns explosions/beams into dark refractive glass blocks.
   - Upload all sprites, beams, translucent FX meshes, decals, and additive shells via **`qrUploadRasterizedGeometry`** (`QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT`) with `QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST` and explicit `blendFuncSrc` / `blendFuncDst`.
   - Upload point particles via **`qrUploadParticles`** (`QrParticleUploadInfo`), noting that **`QrParticleUploadInfo` has no `blendFuncSrc`/`blendFuncDst` fields** — point particles use fixed `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` blending (`Include/qray/qray.h`).

2. **Keep Smooth Vertex Normals on Dynamic Models (`.md2`, `.md3`, `.mdl`, `.mdc`, `.mds`, `.md5mesh`) & Curved BSP Geometry:**
   - Passing `QR_GEOMETRY_UPLOAD_EXACT_NORMALS_BIT` or `QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT` forces flat per-triangle cross-product normals in `Source/Shaders/HitInfo.hlsli` / `Source/Shaders/VertexPreprocessPartial.hlsli`, making characters, weapons, pickups, and statues look like faceted ice/glass figurines.
   - Pass **`flags = 0`** on dynamic character/weapon models and curved/mesh BSP batches (`MST_PATCH`, `MST_TRIANGLE_SOUP`) and supply smooth normalized vertex normals in `QrVertex::normal` so primary/reflection rays interpolate smoothly across triangles.

3. **Impact Decals & Scorch Marks (Blob-Shadow Filtering, Prism Clipping, Darken vs Energy, `qrUploadDecal`, & `cameraNear`):**
   - **Drop fake engine blob shadows:** Classic engines submit fake circular ground decals (`markShadow`, `projectionShadow`, `textures/sfx/shadow`) that float awkwardly over genuine ray-traced shadows; filter them out during decal/overlay ingestion.
   - **Clip decals with a Sutherland–Hodgman convex prism, not 4-corner raycasts:** Project decals by clipping against CPU world triangles (`EditorSceneRuntime::scene.surfaces`, including `cutoutFrameUnderlay` receivers, guarding NaN vertices) using a 6-plane Sutherland–Hodgman prism rather than a 4-corner raycast (which slices marks diagonally across corners/edges). Orient the outward `+0.25` Quake-unit offset (`0.00625 m`; `-0.8` / `-1.6` units for `cutoutFrameUnderlay` / solid backing) toward `centroid - vieworg` — **never** toward camera `viewaxis[0]`, which flips decals at wide FOV edges.
   - **Surface raster decals vs `qrUploadDecal` (`RhiDecalPass`):** `qrUploadDecal` (`QrDecalUploadInfo`) projects an oriented unit box `[-0.5, 0.5]^3` in G-buffer space (ideal for PBR decal textures that modify surface albedo/normal/roughness cleanly across arbitrary geometry); use clipped `qrUploadRasterizedGeometry` surface decals when you need classic engine blend modes (`ZERO / ONE_MINUS_SRC_COLOR`, `SRC_ALPHA / ONE`) or per-vertex time fading.
   - **Darken / Multiplicative Decals (`GL_ZERO / GL_ONE_MINUS_SRC_COLOR`, `GL_DST_COLOR / GL_ZERO`):** Upload the **untouched raw RGBA texture** with its native blend factors through `qrUploadRasterizedGeometry` (do **not** convert it to luminance-alpha or multiply it by exposure). `ComputeOverlayBlendFlags` (`Source/RHI/RhiRasterOverlayPass.cpp`) automatically sets `blendFlags` bit `0` (disabling `sceneExposure` multiplication in `RsWorld.frag.hlsl` so dimensionless `[0, 1]` transmittance factors never crush to black) and bit `1` (preserving perceptual sRGB attenuation for `ZERO / ONE_MINUS_SRC_COLOR` in linear HDR). Preserve `exactVertex` / time-faded vertex colors; only substitute white `(255, 255, 255)` for `rgbGen identity` or legacy `(0, 0, 0)` RGB with `alpha > 0`.
   - **Energy Scorch Marks (`plasma_mrk`, blaster/BFG marks):** Colored dark-fringed glow decals drawn with `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` in linear HDR subtract wall radiance around the border, creating a dark black ring around a blown-out core. Blend energy marks additively with `QR_BLEND_FACTOR_SRC_ALPHA` / `QR_BLEND_FACTOR_ONE` (e.g. `~0.85` linear scale).
   - **`cameraNear` Precision:** Set `QrDrawFrameInfo::cameraNear` to **`1.0–3.0` Quake units** (`0.025–0.075 m`, FOV-clamped so first-person weapons never clip) — never `0.1` units, which drops ~`30x` depth precision (`~1` float32 ULP at 500 units) and lets decals/underlays bleed through solid walls.

4. **Strict Overlay Submission Order (Surface Decals BEFORE Explosions, Sprites, & Smoke):**
   - Upload surface decals/scorch marks **before** dynamic models, explosion sprites, beams, and smoke. Because `RhiRasterOverlayPass` records world draws in upload order with `LessOrEqual` depth against world walls, submitting a darken decal (`burn_med_mrk`) *after* an active fireball (`rocketExplosion`) passes depth at the center and multiplies a pitch-black hole right through the explosion. (`QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` and `QR_RASTERIZED_GEOMETRY_STATE_SMOKE` run in dedicated sub-passes after `DEFAULT` world draws; see `Include/qray/qray.h`.)

5. **32-Bit Opaque-Alpha Effect Textures, Black Noise Cleaning, & Missing Projectiles:**
   - Retail `.tga` / `.pcx` / `.wal` sprite sheets often save 4 RGBA channels where **every texel has `alpha == 255`** over a black background. Choosing `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` purely from `channels == 4` draws a **solid black quad** around fireballs, flames, and lasers. Scan pixels on load (`any(alpha < 250)`): if no texel is transparent on an effect sprite, blend additively (`ONE / ONE`).
   - Apply near-black compression cleanup (`CleanAdditiveBlackBackground`: zero `peak <= 6`, smooth linear ramp `6..18`) to flat 2D additive sprites and world effects (`isAdditive && !environmentMap`) so JPEG/DXT/palette noise does not ignite in HDR — **never** apply it to spherical `tcGen environment` reflection textures (`tinfx2b`, `envmapgold2`, `envmaprail`), or translucent pickup sphere gradients disappear.
   - Skip `"gfx/colors/black"` frames in `animMap` sequences, and substitute `255` when an `rgbGen identity` sprite entity passes all-zero `(0, 0, 0, 0)` `shaderRGBA` so flying energy projectiles remain visible.

6. **HDR Overlay Exposure Contract, Additive Shells, `deformVertexes`, & CPU Backface Culling:**
   - `RsWorld.frag.hlsl` already scales non-transmittance overlay draws by `sceneExposure = adaptedLum * exp2(2.0 - tmExposureBias)`. **Never dim 8-bit sRGB textures on the CPU before upload** (multiplying an sRGB shell image by `0.1` before `VK_FORMAT_R8G8B8A8_SRGB` decode crushes linear radiance to `0.1^2.2 ≈ 0.0063`, erasing health spheres and armor-shard shells). Tune brightness in linear float `QrRasterizedGeometryUploadInfo::color` (`~1.0–1.5x`).
   - Evaluate `deformVertexes wave` displacements in entity-local space along `localNormal` before applying `entity.axis` (required for `item_armor_shard` green energy shells to expand `+2.0` units outward cleanly).
   - Because `RhiRasterOverlayPass` uses `RasterCullMode::None`, **cull back-facing triangles on the CPU** (`dot((p1 - p0) x (p2 - p0), p0 - vieworg) < 0` when `!twoSided`) on closed 3D model overlays so front and back hemispheres do not double-add; pass `QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST | QR_RASTERIZED_GEOMETRY_STATE_DEPTH_WRITE` on opaque `ONE / ZERO` pickup cores so 3D geometry self-occludes cleanly.

7. **Lit Particles (`PARTICLE`), Glass Sprite Proxies (`PARTICLE_SPRITE`), Volumetric `SMOKE`, `smokeLook`, & Camera-Left UVs:**
   - **Choose the right state flag (and never mix `SMOKE` with `PARTICLE*`):**
     - `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE` (`64`): selects the lit particle pipeline (`RsParticle.frag.hlsl`).
     - `QR_RASTERIZED_GEOMETRY_STATE_PARTICLE_SPRITE` (`128`): tags sprite particles even when particle lighting is disabled so traced glass stand-ins work; pair with `qrSetParticleProxyGate` and `glassParticles` so sprites behind glass panes do not double-draw over their refracted copy.
     - `QR_RASTERIZED_GEOMETRY_STATE_SMOKE` (`32`): volumetric procedural smoke (`RsSmoke.vert.hlsl` / `RsSmoke.frag.hlsl`). **Mutually exclusive** with `PARTICLE` and `PARTICLE_SPRITE` — combining them throws `QR_WRONG_ARGUMENT` (`Source/VulkanDevice.cpp`). All three flags are valid only with `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT`.
   - **`smokeLook` zero-trap (lit `PARTICLE`):** `smokeLook` maps to `particleLook = {debugFlag, directGain, lightGain, lightFloor}` (`RsParticle.frag.hlsl`). Leaving `smokeLook` zeroed produces **pitch-black particles**; setting `smokeLook.data[0] > 0.5f` activates false-color RGB debug output! Set `QrVertex::cluster = 0xFFFFFFFFu` (`QR_CLUSTER_LIGHT_NO_CLUSTER` in C/C++, matching the HLSL `QR_PARTICLE_CLUSTER_SELF_LIT` define in `Source/Shaders/HLSL/RsParticle*.vert.hlsl`) for self-lit embers/sparks, and compute smoke/effect animation age relative to entity spawn time (`max(0, sceneTime - entityShaderTime)`).
   - **Volumetric `SMOKE` vertex packing (`RsSmoke.vert.hlsl`):** Pack each billboard quad as: `position` = puff world center (meters), `texCoord` = corner `(-1..+1)`, `normal` = `{age01, sizeMeters, seed}`, `texCoordLayer1` = `{alpha, density}`, `cluster` = shifted PVS cluster, `material = QR_NO_MATERIAL`. Sort puffs back-to-front on the CPU, populate `smokeNoise` (`xyz` spatial noise frequencies in `m^-1`, `w` advection rate in `m/s`) and `smokeLook`, and optionally call `qrSetSmokeWindDirection`.
   - **Camera-left billboard UVs:** In Quake-family axes (`+X` forward, **`+Y` left**, `+Z` up), `viewaxis[1]` points **camera-left**: map `+left` (`-right`) to `u = 0` and `-left` (`+right`) to `u = 1` with CCW winding (`{0, 1, 2, 0, 2, 3}`), otherwise floating numbers (`"1"` score plums) and asymmetric sprites render horizontally mirrored.

### 1.6 Asynchronous Frame Readback & VM-Safe Screenshots

Use `qrRequestFrameReadback` / `qrGetFrameReadbackStatus` (instead of blocking `qrRequestScreenshot`, which calls `waitForIdle` and stalls the GPU pipeline) for screenshots, capture harnesses, and streaming:

1. **Never free `readbackBuffer` before `qrDestroyInstance`:** Frame readback writes asynchronously via GPU DMA; freeing or reallocating the destination buffer while a request may still be in flight causes write-after-free on exit or resize. Keep the buffer alive until after `qrDestroyInstance` (or until `qrGetFrameReadbackStatus` confirms completion and no new request is issued).
2. **Gate behind a keep-alive counter:** Request `qrRequestFrameReadback` only when an active screenshot/capture consumer is pending — never copy ~33 MB/frame unconditionally on every frame.
3. **Pump frames while polling (never spin-wait):** `qrGetFrameReadbackStatus` only advances as frames execute on the GPU. Waiting in a synchronous spin-loop without submitting frames (`qrStartFrame` / `qrDrawFrame`) **deadlocks**! Always pump frames while polling until `done == QR_TRUE`.
4. **Current format/mode limits & resize/row orientation:**
   - `includeUi == QR_FALSE`, `RGBA16F`, and `frameId != QR_FRAME_ID_LATEST` (`0`) return `QR_FEATURE_UNAVAILABLE` (`Source/RHI/NvrhiFrameSkeleton.cpp`).
   - Handle `QR_FRAME_NOT_READY` / failure cleanly across window resizes by canceling and re-issuing the readback request at the new dimensions.
   - Vulkan readback rows are **top-down**; vertically flip rows when handing pixels to OpenGL-expecting engine screenshot writers.
5. **Sanitize screenshot paths from game VMs:** Reject or strip `/`, `\`, `..`, and `:` from filenames supplied by console commands or untrusted game VMs before opening files on disk.

---

## Part 2 — Writing a Game-Specific `EditorFormatAdapter` for `quakerayeditor`

> **Note:** `quakerayeditor` loads game formats through `EditorFormatAdapter` (`editor/Scene.h` and `editor/CMakeLists.txt`) while keeping baseline OBJ/BSP29/MDL support built in.

### 2.1 How the Adapter Plugs In (`buildPlayLevelCommand` & `System` Tab Config)

`QuakeRaySDK/editor/CMakeLists.txt` exposes two cache variables:
- `QR_EDITOR_ADAPTER_SOURCES` — list of host-owned `.cpp` files compiled directly into the `quakerayeditor` executable.
- `QR_EDITOR_ADAPTER_INCLUDE_DIRS` — include directories needed by those adapter sources.

In your adapter `.cpp` file, self-register the adapter automatically before `main()` using a portable static registrar object:

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
    adapter.buildPlayLevelCommand = &BuildMyGamePlayLevelCommand;
    RegisterEditorFormatAdapter(adapter);
}
```

- **`buildPlayLevelCommand(request, outCommand, outError)` (`editor/Scene.h`):** Populate `EditorLaunchCommand` (`executable`, `workingDirectory`, `arguments`, `envOverrides`) from `EditorPlayLevelRequest` (`executableDir`, `gameExecutable`, `gameDir`, `scenePath`, `levelName`, `shaderFolder`, `blueNoisePath`, `waterNormalPath`, `materialsPath`, `lightsPath`, `configPath`, `multiplayer`):
  - Resolve the game executable via `--game-exe` / `QRAY_GAME_EXE` (passed through `request.gameExecutable`) or sibling search beside `request.executableDir`.
  - Forward edited `materialsPath`, `lightsPath`, and `configPath` plus `envOverrides` (e.g. `{"SDL_VIDEODRIVER", "x11"}` on Linux Wayland so the launched game opens an Xlib Vulkan surface cleanly).
  - `EditorApp` automatically saves dirty session files before launching, pauses the editor viewport while the game runs, and resumes viewport + X11 focus capture when the child exits.
- **`System` tab `config.cfg` persistence:** `EditorApp::SaveSystemConfigSession` persists editor-authored system/render settings via line-merged updates into `<gamePath>/config.cfg` so unrelated engine settings remain intact.

> **Path Hygiene:** Always normalize asset paths using `NormalizeEditorAssetPath(raw)` (converts `\` to `/`, strips leading `/` and `./`, lowercases for case-insensitive VFS lookup). Archive entries and `.skin`/`.shader` scripts routinely mix `./models/...`, upper/mixed case, and backslashes.

### 2.2 Archive Discovery & External Image Loading

1. **VFS Extensions (`EditorAssetSource` in `editor/Assets.h`):**
   - `extraArchiveExtensions` tells the mod/mount scanner to mount additional zip-style packs (`.pk3`, `.pk4`, `.kpf`, etc.) automatically in load order alongside `pak*.pak`.
   - `extraBaseGameDirs`, `extraModConfigFiles`, and `extraModSubdirs` ensure `Mount Game/Mod Folder` recognizes base/mod installations cleanly.
2. **`loadExternalImageRgba(assets, requestedPath, outRgba, outWidth, outHeight, outResolvedPath)`:**
   - First check if a parsed shader script maps `requestedPath` to a concrete stage texture (`qer_editorimage` / primary stage `map`), or fall back to stripping extensions (`StripEditorImageExtension`) and probing `.tga`, `.jpg`, `.jpeg`, `.png` (or engine formats like `.wal`/WAD3 decoded to RGBA8).
   - Never rely blindly on a single extension in BSP/shader filenames: shader scripts often reference `.tga` when the file inside an archive is actually `.jpg` (or vice versa).

### 2.3 Parsing BSP Geometry, Bezier Patches, Clusters, & Props

When implementing `loadBspSceneMemory(bytes, sourceName, scene, error, options)`:

1. **Detect Magic/Version Strictly in `canLoadBspMemory`:** Verify header size, 4-byte magic (e.g. `IBSP`), and version (`46` for Q3, `47` for RtCW, `38` for Q2; return `false` for Q1 BSP29/BSP2 so the built-in loader continues working cleanly).
2. **Tessellate Curved Surfaces (`MST_PATCH`) & Validate Winding Properly:**
   - Step strictly by **2** across overlapping **3×3 biquadratic sub-patches** (`u = 0, 2, ...`, `v = 0, 2, ...`) — never evaluate an entire control grid as one high-degree Bezier surface.
   - Weld boundary vertex normals along closed cylinder/dome seams (`dist^2 < 1e-4`), handle degenerate cone apexes without NaN tangents/normals, and never validate patch winding using a global sum of radial vertex normals (on closed 360° pillars/arches, symmetric normals cancel to `(0, 0, 0)` and rounding noise flips triangles inside-out under backface culling — validate winding locally per quad/triangle against authored normals).
   - For planar BSP faces, validate winding **per surface** against the BSP face normal (`lightmapVecs[2]` in id Tech 3) instead of blindly flipping every face.
3. **Preserve `faces`, Spatial Prop Clustering, & `twoSided` Duplication:**
   - Populate `EditorSurfaceFace` (`normal`, `cluster`, `positions`, `texCoords`) in addition to the merged `vertices`/`indices`. The editor uses `faces` for triangle/surface picking, cluster resolution, and DTAL area-light bounds.
   - Spatially cluster BSP-baked `MST_TRIANGLE_SOUP` props (`misc_model`) via Union-Find (`4u` AABB padding) into distinct `EditorAliasInstance` records so individual props remain selectable and cleanly bounded.
   - For ray-traced `twoSided` (`cull none`) geometry, duplicate vertices with negated normals and reversed triangle indices (`i0, i2, i1`) so RT backface culling never erases back sides of thin banners, grates, foliage, or arches.
4. **Skip Invisible Utility Surfaces, Keep Inline Map Submodels:**
   - Exclude compiler-only brush surfaces (`nodraw`, `clip`, `playerclip`, `hint`, `skip`, `areaportal`, `origin`, `caulk` where appropriate, unless meant for sky).
   - Honor `options.includeInlineSubmodels`: include world model (`models[0]`) and, when true, visible inline brush models (`models[1..N]`), excluding only hidden/trigger brush entities.
5. **World Clusters, 0-Based RLE `visData`, `8192` Cap, & Sky Portal Filtering (`scene.worldClusters` + `scene.nodes`):**
   - Build `EditorBspNode` planes and `EditorWorldClusters` (`mins`, `maxs`, `flags`, `contents`, `leafCluster`, `visData`, `visOffsets`, `skyVis`, `pvsRowBytes`, `numLeafs`) with solid leaf `0` set to `kBspContentsSolid = -2`, engine leaves shifted `i -> i + 1`, negative BSP node children shifted by `-1`, and empty-cluster `mins`/`maxs` filled with valid world bounds (never `±1e30`).
   - **Exact PVS indexing:** Shift `visOffsets` (`visOffsets[0] = -1`, engine cluster `c -> visOffsets[c + 1]`) and `skyVis` (bit `c` = SDK cluster `c`), while keeping bits inside zero-run RLE `visData` **0-based** (`bit b` -> SDK cluster `b + 1`), because `Source/ClusterLightLists.cpp` already adds `+ 1`. Trailing `0xFF` padding is not needed (`DecompressVis` bounds-checks `pIn < pEnd` and zero-fills truncated rows). Passing `pVisData = nullptr` falls back to all-visible.
   - **Respect `Q2_MAX_CLUSTERS = 8192`:** If the engine map has more than `8191` non-solid leaves/clusters, fold them into a spatial grid (`<= 8191` clusters) and remap PVS rows before uploading so distant areas never lose lighting silently.
   - Keep `SURF_SKY` brushes/portals excluded from solid opaque BVH occlusion (mark them `QR_GEOMETRY_VISIBILITY_TYPE_SKY` and filter them out of opaque surface light extraction even when combined with `SURF_NODRAW`) so sky views and outdoor directional sunlight are never sealed shut.
   - Use solid-leaf boundary nudging when assigning per-vertex / per-face clusters and light clusters (`ResolveEditorLightCluster` / adapter helper).
6. **Consistent Winding & Classic Format Quirks:**
   - Flip Quake-family clockwise BSP faces consistently (in `IBSP` v38, reverse triangle fans `i0, i+1, i`) and verify alias/skeletal model winding matches the host convention configured via `qrSetSpatialConvention` so backface culling never turns walls or characters inside-out.
   - For GoldSrc (`BSP30` / `.wad`), decode `{`-prefixed palette textures as binary `ALPHA_TESTED` cutouts keying out palette index `255` (bleeding opaque neighbor RGB into alpha-0 texels to prevent dark bilinear halos), and convert `lights.rad` texture irradiance definitions into physical `EditorLight` / area-light emitters.
7. **Map Entities & Camera Spawn:** Parse the entities lump for spawn priority (`info_player_deathmatch`, `info_player_start`, `info_player_coop`, `info_player_intermission`) to populate `scene.cameraPosition` and `scene.cameraYawDegrees`, plus `misc_model` static props and sky/sun hints (`hasSun`, `sunColor`, `sunIntensity`, `sunDegrees`, `sunElevation`, `_sun 1`).

### 2.4 Multi-Stage Shaders: Separating Traced Base vs. Dynamic Overlays & Parser Traps

Engines with multi-stage shader scripts (id Tech 3 `.shader`) combine diffuse + lightmap + glow/fx/environment/animated passes that cannot simply be flattened into a single RGBA albedo image.

Follow this decomposition pipeline:

1. **Classify Each Shader:**
   - **Sky (`isSky`):** Surface goes into `EditorSurfaceKind::Sky`, sets skybox/env (`scene.skyboxName`), and marks sky visibility clusters.
   - **Water / Slime / Lava / Glass / Mirror / Fence:** Map to matching `EditorSurfaceKind` (`Water`, `Slime`, `Lava`, `Opaque` + `isGlass`/`isMirrorShader`, `Fence` with `alphaTested`) so path-traced refraction, water caustics, reflections, and alpha cutouts work properly.
   - **Metallic Env Base (`metallicEnvBase = true`):** Recognize `tcGen environment` base stages (chrome/gloss metal) instead of projecting static environment bitmaps as diffuse wallpaper. Support inverted-alpha lacquer layers (`GL_ONE_MINUS_SRC_ALPHA GL_SRC_ALPHA` on armored characters/pickups), composite `decalOverlayMaps` on top, generate per-pixel roughness/metallic masks, lift overly dark chrome albedo, and preserve dynamic model `metallicEnvAlphaMask` (e.g. shiny metallic armor pickups).
   - **Traced Base + Overlay (`tracedBase = true`, `usesOverlay = true`):** Put the opaque/alpha-tested base stage on `EditorSurface` for BVH ray tracing, and push dynamic/animated/additive/blended overlay stages (starting after `baseStageIndex`, skipping lightmap passes) into `EditorOverlaySurface` (`scene.overlaySurfaces`). Separate pre-base and post-base overlay passes cleanly, and copy `overlayVerts` before applying base vertex scaling/offsets.
   - **Non-Traced Dynamic Overlay (`isNonTracedOverlay = true`):** For pure additive sprites, volumetric flares, teleporter beams, flames, and forcefields (`autoSprite` / `autoSprite2` / additive-only stages):
     - Emit each non-traced overlay as its own `EditorOverlaySurface` (do not merge unrelated overlays across props so picking and animation stay exact).
     - Still emit an `isOverlayOnly = true` proxy `EditorSurface` with cross-quads for viewport picking: rotate `autoSprite2` cross-quads around the quad's **major axis**, and rotate flat-in-Z `autoSprite` cross-quads around X.
2. **Handle Cutout Frames Over Dark/Additive Voids (`cutoutFrameUnderlay`):**
   - Shaders like floor jump-pads or monitors draw an emissive/animated texture *under* a grating/frame texture with binary alpha holes (`GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA` + `alphaFunc`).
   - If you trace only the alpha-tested frame, rays pass through its cutout holes into empty geometry or sky.
   - **Fix:** Detect when an emissive/dark base sits beneath a cutout stage (`isCutoutUnderlay = true`). Offset the underlay geometry slightly behind/inward along smooth vertex normals using `OffsetEditorVerticesAlongSmoothNormals(verts, indices, -0.8f)` (or upload a solid dark backing surface behind traced cutouts) so rays through holes terminate on the lit/backing layer cleanly without z-fighting.
3. **Suppress Redundant Base-Copy Passes:**
   - Shaders written as `[base stage] -> [blendFunc GL_DST_COLOR GL_ZERO $lightmap]` exist solely to multiply baked lightmaps; once `QuakeRay` ray-traces the scene, skip the lightmap multiply stage and any redundant `DST_COLOR ZERO` base copy so the traced base is not darkened twice.
4. **Multi-Layer Shader & `.shader` Parser Traps:**
   - **`wetGlossAlphaMask`:** Alpha-sandwich wet-wall shaders use their alpha channel as a wetness/gloss mask — **never** classify them as cutout holes.
   - **`compositeUnderlayBase`:** When an opaque base carries an alpha-blended decal/detail layer underneath or merged into the base pass, alpha-composite them on the CPU into the base texture so the traced albedo matches.
   - **`$whiteimage` / `*white`:** Synthesize a `1x1` opaque white texture and skip degenerate pure-white stage-0 passes when a real textured stage follows.
   - **`.skin` binding, direct images, & `forceOpaqueAlpha`:** Match `.skin` lines against the mesh **`surfaceName`** (not `shaderName`); provide a `GetDirectImageMaterial` bypass when loading raw image paths without shader-name redirection; and restrict `forceOpaqueAlpha` strictly to ray-traced opaque base materials (**never** 2D HUD textures, which need real alpha!).
   - **Cutout pixel scan & fringe sharpening:** Classify binary cutouts by inspecting actual texels (`>= 0.5%` pixels with `alpha < 128` and `>= 10%` with `alpha >= 128`) and sharpen soft-alpha fringes on cutout masks so edges do not dissolve under the `RtAlphaTest` threshold.
   - **`.shader` parser syntax pitfalls:** `alphaFunc` takes **1 token** (`GT0`, `LT128`, `GE128`), not 2; guard nested block scanning with a `HasBraceBefore` check so malformed shader files do not swallow subsequent shader definitions; reset sticky `alphaFunc` state at every stage boundary; and parse `clampMap` identically to `map`.

### 2.5 Predefined Map Lights, `is_light`, `truelight`, Sky, & Persistence (`usesExternalLightingModel`)

> **Crucial Design Requirement:** Every predefined light source the engine renders must be editable, toggleable, and deletable in `Light Editor`.

When your adapter loads a map whose lighting model comes from the host engine rather than Quake 1 palette fullbrights:

1. **Set `scene.usesExternalLightingModel = true`.**
2. **Populate BOTH `scene.lights` and `scene.bspLights` with ALL predefined map light sources:**
   - **Map point/spot entities:** All `"light"` and `"lightJunior"` entities (parsing `origin`, `_color` / `color`, `light` / `_light`, `style`, `_cone` spotlight half-angle, and `target` -> `targetname` / `info_null` direction), plus `_sun 1` sky directional hints.
   - **Map surface & fixture lights (`q3map_surfacelight` / `lights.rad` / luminous overlays):** Map compilers bake `q3map_surfacelight` brushes into lightmaps and strip point `"light"` entities. Extract non-sky surfaces with `surfaceLight > 0.0f` (and `autoSprite`/`autoSprite2` light sprites):
     - Subdivide long luminous surfaces (`> 96u`) into `128u` grid buckets so strip lights illuminate evenly instead of collapsing to one midpoint.
     - Merge adjacent polygons of the same fixture within `24u` (`dedup`), and attenuate close neighboring fixtures within `48u` (`0.65x`) so clusters do not blow out.
     - Compute area-weighted centroid + outward normal push (`+4u` along face normal for planar brushes, **`push = 0`** for `MST_TRIANGLE_SOUP` mesh fixtures so curved bulbs are not pushed into adjacent walls), resolve PVS cluster, derive color/intensity from `q3map_lightRGB` or texture emission, and push an `EditorLight` into both `scene.lights` and `scene.bspLights` with `className = shaderName`.
3. **3-Tier `rt_truelight`, `is_light` Hard-Switch, `defaultEmissiveTexture`, & `EMISSION_RAY_ONLY`:**
   - **`rt_truelight` modes:** `0` = legacy map lights only; `1` = map lights + explicitly enabled/authored DTAL + dynamic lights (default); `2` = pure DTAL emissive lighting without legacy map point lights.
   - **`is_light` hard-switch:** When a user enables `is_light` in Material Editor, switch that material exclusively from ray-only emissive GI to NEE/DTAL even when `light_brightness == 0`, emitting uniform `light_color` when `texture_emissive == NONE`. *(Current SDK caveat: the `light_focus` UI slider spans `0..180°`, while `EditorDtalPipeline.cpp` treats it as a **half-angle** and disables focus or resets projectors to `45°` whenever `> 90°` — author focus values in `0..90°`.)*
   - **`defaultEmissiveTexture` & clearing lamps cleanly:** Single-stage `q3map_surfacelight` shaders (e.g. `spotlamp_l`) must populate `EditorSurface::defaultEmissiveTexture = def.map` so clearing a lamp via `texture_emissive: NONE` + `is_light = false` zeroes baked RME emission, disables automatic DTAL generation, and persists `texture_emissive: NONE` cleanly in YAML.
   - **Never lose YAML deletions (`IsEmittedMaterialKey`):** Every managed YAML material key **must** be listed in `IsEmittedMaterialKey` (`editor/EditorSession.cpp`), otherwise `EditorMaterialsDocument::WriteMerged` resurrects cleared keys from the existing file on Save!
   - **Indirect bounce & dark-room noise:** Never withhold `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` solely because `!params.luma` or set `kEditorDefaultRoughness` (`0.8f`) to `1.0f` (both kill soft indirect bounce). In dark rooms where ray-only emissive surfaces are noisy, switch key fixtures to `is_light`/DTAL and keep `rt_filmgrain 0`. Whenever an authored YAML override exists (`hasAuthoredOverride`), skip host procedural RME synthesis (`skipProceduralRme`) so authored maps stay intact.
4. **Cloud-Only Skyboxes & Procedural Sky Parity:**
   - For cloud-only sky shaders (`skyparms - <height> -` with no 6-sided box), synthesize a clean skybox backdrop and maintain sun-disc atlas parity when `rt_physical_sun 0`.
   - Call `sceneRuntime.AdvanceCelestialClock(deltaSeconds)` every frame so time-of-day sky animation advances smoothly, honor `sky.proceduralAtmosphereConfigured`, and note that procedural cloud parameters (`color`, `coverage/scale/wind`, `altitude`, `thickness`) are packed inside `QrDrawFrameSkyParams::skyCubemapRotationTransform` (`Source/VulkanDevice.cpp`).
5. **Full Persistence Lifecycle (`qray.lights.yaml` & `Regenerate from map`):**
   - `scene.bspLights` holds the immutable baseline extracted from the BSP; `scene.lights` holds the active mutable list shown in `Light Editor -> Entity`.
   - When the user disables (`enabled: false`), edits, adds, or deletes standard map lights and clicks **Apply**, `EditorSession` writes the complete active state (including `class:` metadata and an explicit empty `lights: []` if all lights were deleted) to `qray.lights.yaml`.
   - On next map load, an existing `lights:` block (including `lights: []`) overrides `scene.lights` cleanly so deleted lights stay deleted. Clicking **Global -> Regenerate from map** restores `scene.lights = scene.bspLights`.

### 2.6 Multi-Part Model Preview, MD3 Safety, & Catalog Assembly

For engines where player characters are composed of `lower.md3` + `upper.md3` + `head.md3` + `animation.cfg` + `*.skin`:

1. **Assemble Multi-Part Characters Virtualized as `.character`:**
   - In `postProcessModelCatalog(entries)`, inspect cleaned relative paths (`CleanRelativeAssetPath` + `NormalizeEditorAssetPath`). Whenever a directory contains at least two of `{lower.md3, upper.md3, head.md3}`, synthesize a single virtual entry `<dir>.character` and sort character entries predictably.
   - In `shouldHideModelCatalogEntry`, hide clutter like `_hand.md3`, `_1.md3`, `_2.md3` (first-person view weapon helpers), while keeping standalone weapons/props (`.md3`) selectable.
2. **Tag-Based Frame Assembly & MD3 Safety in `loadPreviewModel`:**
   - **Validate triangle indices against `numVerts`:** Corrupted or custom MD3 assets can contain out-of-range triangle vertex indices; validate every index `< surface.numVerts` before indexing to prevent out-of-bounds crashes.
   - **Parse `animation.cfg` leg-frame offset subtraction:** In id Tech 3 `animation.cfg`, leg animation indices start *after* torso-only animations in the shared frame table — subtract the torso-only frame skip (`legsStart - torsoStart`) from leg animation `firstFrame` values so `lower.md3` plays the right frames!
   - Concatenate `lower.md3` -> `upper.md3` (via `tag_torso` rigid 3×4 transform) -> `head.md3` (via `tag_head` rigid 3×4 transform) per `EditorPreviewModelFrame`.
   - Load `*.skin` files (`default`, `red`, `blue`, etc.) into `EditorPreviewModelSkinPreset` (matching by `surfaceName`) and bind shader/texture prefixes via `resolveModelMaterialPrefix`.

### 2.7 Isolated Native Vulkan AI Texture Worker (`ggml-vulkan`) & 4-Tab PBR Pipeline

The editor's local `Generate with AI` pipeline (`EditorAiDialog`, `EditorAiTexture`, `editor/ai_worker/`) exposes a 4-tab PBR authoring flow (**Upscale** via `4x-PBRify_RPLKSRd_V3` [`RealPlksrPipeline`], **De-lighting** via `StableDelight` [`LotusPipeline`], **Normals** via `DeepBump` [`DeepBumpPipeline`], and **Depth** via `Lotus Depth v2` [`LotusPipeline`]) and must never run heavy neural inference inside `quakerayeditor` or via ONNX Runtime / WebGPU (Dawn) (ONNX Runtime removed `ROCmExecutionProvider` in v1.23+, and Dawn WebGPU lacks strict VRAM caps):

1. **Out-of-Process Child Worker (`quakeray_ai_worker`) & Packaging Caveats:**
   - `editor/EditorAiDialog.cpp` invokes `quakeray_ai_worker` (`editor/ai_worker/`) via `EditorAiWorkerClient` and `EditorAiWorkerProtocol` (binary NCHW tensor temp files + line-delimited `PROGRESS`, `VRAM`, `PROVIDER`, `DONE`, `ERROR` messages on stdout).
   - Process isolation guarantees 100% VRAM and host RAM reclamation on exit, instantaneous `Cancel` via child termination (`CancelActiveEditorAiWorker`), and survival of `quakerayeditor` with unsaved edits preserved if a driver reset or worker failure occurs. On Linux, `EditorAiWorkerClient.cpp` sets `prctl(PR_SET_PDEATHSIG, SIGKILL)` on the child so worker orphans never outlive a crashed editor; place `quakeray_ai_worker` at the expected path relative to `quakerayeditor`.
   - `EditorAiModelDownload` provides a built-in native HTTPS GGUF downloader (WinHTTP on Windows, and **`dlopen("libcurl.so.4")` / `libcurl.so` at runtime on Linux** — note that because it is loaded dynamically, `ldd quakerayeditor` will **not** list `libcurl`; ensure `libcurl4` is installed in target Linux environments) with `.part` staging and streaming SHA-256 verification.
2. **4-Tab PBR Workflow, Non-Destructive Outputs, & GGUF Exporter Rules:**
   - Save all generated textures non-destructively under `textures/ai_generated/` so source game assets are never overwritten.
   - **Upscale (`4x-PBRify_RPLKSRd_V3`):** Produces `2x`/`4x` super-resolution albedo with detail/blend strength and sharpening controls, plus upstream `Use Upscaled diffuse` wiring into downstream tabs.
   - **De-lighting (`StableDelight`):** Strips baked directional shading and specular highlights cleanly into a flat de-lit diffuse map.
   - **Normals (`DeepBump`):** Centers and wrap-pads tiles cleanly before inference and supports `Use De-lit diffuse` so generated normal maps match the de-lit surface.
   - **Depth (`Lotus Depth v2`):** Outputs depth/height feeding `texture_height` (Relief POM), derived normal maps, or gloss/roughness maps. Uses wide wrap/mirror context (`PrepareEditorLotusDepthInputNchw`) and halo-only bilinear tilt removal (`ExtractDetrendedEditorLotusDepthHeight` / `SolveHaloTiltCoefs`) so low-contrast or de-lit inputs preserve interior relief without border washouts.
   - **GGUF exporter invariants (`tools/export_ai_gguf.py`):** Keep all tensor names strictly `<= 63` bytes (`GGML_MAX_NAME = 64` including NUL terminator), keep text embeddings in **FP32**, and export weights as unquantized **FP16 GGUF v3** (`*-fp16.gguf`).
3. **Native `ggml-vulkan` Execution, FlashAttention, & Memory Discipline:**
   - `GgmlOps::ExactMultiHeadAttention` uses `ggml_flash_attn_ext` with `F16` K/V casts and `GGML_PREC_F32` accumulation when `(dHead % 8) == 0 && dHead >= 8`, avoiding multi-gigabyte $O(N^2)$ attention matrices so `LotusDepth` and `StableDelight` run single-tile (`1/1`) even at `1024 px` within a 16 GB VRAM budget.
   - Diffusion pipelines execute in sequential memory-isolated stages (`VAE Encode -> unload -> ControlNet/UNet -> unload -> VAE Decode -> cleanup`), query `VK_EXT_memory_budget` (`min(0.75 * totalVRAM, freeVRAM - safetyReserve)`), pre-measure exact `ggml` graph buffer bytes before allocation, and apply automatic tiling (or CPU fallback) whenever a stage exceeds budget.
4. **Strict Manual UX Trigger:**
   - Inference must start **only** when the user clicks **Generate** / **Regenerate**. Never auto-run generation on opening the dialog, changing tabs/combos, or finishing a model download. Display live percentage progress (`0–100%`) and stage status reported by the worker.

### 2.8 Non-Q3 Engine Family Adaptation Notes & Cross-Engine Matrix

Different classic engine families expose substantially different world formats, visibility structures, model pipelines, and build constraints. Plan around these architectural differences before writing an adapter or runtime renderer:

#### Cross-Engine Format & Adaptation Matrix

| Engine Family | World / Visibility Format | Cluster Cap Strategy | Textures / Palettes / Cutouts | Models & Skinning | Render / Blend Modes | Build & ABI Notes |
|---|---|---|---|---|---|---|
| **Quake 1 (id Tech 1)** | `BSP29` / `BSP2` / `2PSB` (leaf PVS) | Built-in loader; fold large `BSP2` maps `<= 8191` | Embedded 8-bit miptex + `palette.lmp`; `{` cutouts | `.mdl` / `.spr` (morph frames) | Opaque, water warp, alpha cutouts, additive sprites | C99 + CMake/Make; `.pak` / `.pkz` |
| **Quake II (id Tech 2)** | `IBSP` v38 (cluster PVS) | Native clusters (`<= 8191`); 0-based RLE `visData` | `.wal` + `pics/colormap.pcx` 768B palette; bleed opaque RGB into alpha-0; `mtexinfo_t::next` chains | `.md2` (morph), `.sp2` sprites | `SURF_TRANS33/66`, `SURF_WARP` (`TURB_WARP_BIT`), `RF_TRANSLUCENT` | Meson/CMake (sidecar if Meson); `.pak` / `.pkz` / `.kpf` |
| **Half-Life (GoldSrc)** | `BSP30` (leaf PVS; often `> 8192` leaves) | **Must fold leaves** into spatial grid `<= 8191` & remap PVS | `WAD3` per-miptex 256-color palettes (`"wad"` key); `{` index-255 cutout (bleed RGB!), `~`, `+0~`, `+0..9` | `StudioMDL` `.mdl` (CPU bone skinning, 128 bones, `*t.mdl`, `*01.mdl`) | Full `kRender*` (Normal, TransColor, TransTexture, Glow, TransAlpha, TransAdd), `STUDIO_NF_*`, `triangleapi_t` lines | Waf/CMake (sidecar if Waf); `REF_SOFTWARE` window to avoid GL |
| **Quake III (id Tech 3)** | `IBSP` v46 (cluster PVS) + `MST_PATCH` + `MST_TRIANGLE_SOUP` | Native clusters (`<= 8191`); 0-based RLE `visData` | Multi-stage `.shader`, `.tga`/`.jpg`, `q3map_surfacelight`, `.skin` | `.md3` (morph + `tag_*` multi-part + `animation.cfg`) | Arbitrary GL blend pairs (approximate unsupported ops), `tcMod`, `rgbGen`, `deformVertexes` | C99 host + C++20 `.so` (`dlopen`), `.pk3` / `.pk3dir` only |
| **RtCW / Jedi Knight (id Tech 3 forks)** | `IBSP` v47 (RtCW) + runtime `SetFog`/`fogvars` | Native clusters (`<= 8191`) | `.shader` + case-sensitive `Main/` on Linux | `MDC` / `MDS` / Ghoul2 skeletal + `.md3` | Multi-stage `.shader` + runtime fog & distance cull | **Separate incompatible SP vs MP renderer ABIs** (2 `.so` targets) |
| **Doom 3 (id Tech 4)** | ASCII `.proc` portal areas (no binary BSP/PVS) | Area-graph clusters or `pVisData = nullptr` | `.mtr` material scripts, native normal/specular maps | `.md5mesh` / `.md5anim` (CPU multi-weight skinning) | All-dynamic `renderLight_t` (no lightmaps), 3D GUI surfaces, mirror/camera subviews | C++98/11 -> C++20 migration, strip `SDL_WINDOW_OPENGL`, 64-bit host vs 32-bit `gamex86.dll` mods |

#### Engine-Family Specific Blockers & Patterns
- **id Tech 4 (Doom 3 family):**
  - Maps store ASCII `.proc` portal areas rather than binary BSP trees/PVS — either synthesize clusters from portal areas/adjacencies or pass `pVisData = nullptr` (all clusters visible).
  - Lighting is 100% dynamic (`renderLight_t`) with no baked lightmaps; upload shadow-casting dynamic lights cleanly while **excluding stencil shadow volumes** (`ParseShadowModel` / shadow meshes) from ray-tracing BVH uploads!
  - CPU-skin multi-weight `.md5mesh` skeletal geometry before `qrUploadGeometry`.
  - Render 3D in-world GUI surfaces (`tr_guisurf`) via world raster overlays and guard mirror/remote-camera subviews (`tr_subview`) so they do not corrupt the main camera transform or tonemapper history.
  - Upgrade legacy C++98/11 compiler flags to C++20 for SDK headers, remove hardcoded `SDL_WINDOW_OPENGL` creation, and remember that 64-bit engine builds cannot load legacy 32-bit `gamex86.dll` mod binaries.
- **GoldSrc (Half-Life family):**
  - Map all six `kRender*` modes cleanly (`kRenderNormal` -> traced opaque; `kRenderTransAlpha` -> `ALPHA_TESTED`; `kRenderTransTexture` / `kRenderTransColor` -> physical glass or raster alpha blend; `kRenderTransAdd` / `kRenderGlow` -> additive raster overlay).
  - Load `WAD3` archives referenced by worldspawn `"wad"` keys (each miptex carries its own trailing 256-color palette). Interpret texture name prefixes: `{` = index-255 binary cutout (**bleed RGB from opaque neighbors into alpha-0 texels** to stop dark bilinear fringes), `~` = emissive light texture, `+0~` / `+0..9` = toggleable/animated texture sequences.
  - Extract `light_environment` as directional sun, honor `spawnflags & 1` (initially off) on light entities, and fold `BSP30` leaf PVS into `<= 8191` spatial clusters (`Q2_MAX_CLUSTERS`).
  - CPU-skin `StudioMDL` vertices through up to `128` bone transforms and upload with an identity `QrTransform`; resolve `*t.mdl` external texture companion files, hide split-sequence `*01.mdl` files from editor model catalogs, honor `STUDIO_NF_CHROME` / `STUDIO_NF_MASKED` / `STUDIO_NF_ADDITIVE`, and map `triangleapi_t` line modes to `QR_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST`.
- **id Tech 3 Forks (Return to Castle Wolfenstein / Jedi Knight family):**
  - Support `IBSP` v47 header version (RtCW) and skeletal/compressed models (`MDC`, `MDS`, Ghoul2) alongside `.md3`.
  - **Dual incompatible SP vs MP renderer ABIs:** Build separate SP and MP renderer shared libraries with explicit compile-time ABI struct-size/export verification — loading an SP renderer `.so` into an MP binary (or vice versa) crashes immediately.
  - Forward runtime `SetFog` / `fogvars` state into SDK fog parameters, and normalize case-sensitive asset directory names (e.g. `Main/` vs `main/`) on Linux.
- **id Tech 2 (Quake II family):**
  - Supply `palette768` (trailing 768 bytes of `pics/colormap.pcx`) to `LoadBspSceneMemory` for `.wal` texture decoding.
  - **Opaque-neighbor RGB bleeding into alpha-0 texels:** Index-255 transparent cutouts and UI sprites often store dark/magenta RGB in transparent pixels, causing black bilinear fringes when sampled on GPU. Dilate RGB from opaque neighbors into every `alpha == 0` texel on upload (applies equally to GoldSrc `{` textures!).
  - Register `.kpf` alongside `.pak`/`.pkz` in `extraArchiveExtensions`, flag `SURF_WARP` liquids with `QR_GEOMETRY_UPLOAD_TURB_WARP_BIT`, and walk `mtexinfo_t::next` chains for animated brush textures.
- **Non-Z-Up, Left-Handed, or Non-`40 u/m` Hosts:**
  - Declare exact host axes and handedness in `qrSetSpatialConvention` before any uploads (`sceneXAxis`, `sceneYAxis`, `sceneZAxis`, `hostIsRightHanded`).
  - When the axis mapping is mirrored (`det < 0`), always pass both `pFaces` and `pFaceVertices` to `qrUploadWorldLights`.
  - Centralize your single `unitsPerMeter` constant (default `40.0f` for Quake-scale hosts) and scale every metric field (`rayLength`, `cameraNear`, `cameraFar`, `light.reach`, `light.radius`, `cellWorldSize`, `fog`, `volumetricFar`, glass thickness) by `1 / unitsPerMeter`, and scale dynamic cluster light power by `unitsPerMeter^2`.

---

## Part 3 — Graveyard of Pitfalls (What Broke and How to Avoid It)

### Quick Triage Table: Symptom -> Cause -> Fix

| Symptom | Root Cause | Fix (Reference) |
|---|---|---|
| Black screen + `QR_FEATURE_UNAVAILABLE` log spam on Linux | `QR_PRESENT_MODE_IMMEDIATE` unsupported or GL context attached to Vulkan window | Fall back to `QR_PRESENT_MODE_VSYNC` + `maxFrameLatency = 1`; use non-GL window & `SDL_VIDEODRIVER=x11` (§1.2, Pitfall 25) |
| Crash or white/missing dynamic models starting on **frame 2** | Host called `dynamicMaterialBakes.BeginPass()/EndPass()` per frame | Remove `BeginPass`/`EndPass` in runtime host; free only in `DestroyGpuResources` (§1 #4, Pitfall 7) |
| World lighting vanishes in distant rooms / corridors | `visData` bits shifted `+ 1` (double-shifted to `c + 2`), or `numClusters > 8192`, or `cluster = 0` passed instead of `QR_CLUSTER_LIGHT_NO_CLUSTER` | Keep RLE `visData` **0-based** (`visOffsets`/`skyVis` 1-shifted), fold leaves `<= 8191`, use `~0u` for unclustered lights (§1.3, Pitfall 20) |
| ~3% whole-scene brightness dip on every weapon shot | Dim/zero dlight uploaded to cluster lists while dropped by `AddSphericalLight`, or `rt_restir 1` reshuffling candidates | Filter `power <= 0` & `kLightMinColorSum` (`0.0001f`) identically; scale dynamic power `1600x`; keep `rt_restir 0` (§1.3, Pitfalls 11–12) |
| Blown-out/crushed HDR highlights or washed-out emissives | `pTonemappingParams == nullptr` (`tmWhitePoint = 1.05f`) or `pBloomParams == nullptr` (legacy `64x` bloom) | Pass explicit AgX `pTonemappingParams` (`whitePoint = 20.0`) & `bloomIntensity = 0` + `postEffectParams.pBloom` (§1.2, Pitfall 26) |
| Flat normals, zero emission, or pitch-black world with `pTexturesParams` | Zero-initialized `pTexturesParams` zeroes `normalMapStrength` & all 64 `lightStyleScales` | Initialize `normalMapStrength`, `emissionMapBoost`, and all 64 `lightStyleScales = 1.0f` (§1.2, Pitfall 26) |
| 2D HUD upside down or `QR_WRONG_ARGUMENT` on UI draw | OpenGL Y-up ortho matrix or `DEPTH_TEST \| DEPTH_WRITE` on `SWAPCHAIN` | Use Vulkan Y-down NDC (`proj[5] = +2/H, proj[13] = -1`); clear depth flags on `SWAPCHAIN` (§1.4, Pitfall 29) |
| Scrolling `tcMod` overlay frozen in place | `QR_RASTERIZED_GEOMETRY_STATE_TEXCOORD_TRANSFORM` (`256`) bit missing | Set `TEXCOORD_TRANSFORM` whenever `texCoordOffset` is used (§1.4, Pitfall 29) |
| Banners, grates, foliage, or arches vanish from behind | Single-sided `cull none` geometry under `rayCullBackFacingTriangles = QR_TRUE`, or closed `MST_PATCH` normal-sum flip | Duplicate `twoSided` traced geometry with inverted normals/indices; validate `MST_PATCH` per quad (§1.4, Pitfalls 10, 28) |
| Pitch-black bullet decals, dark halo on plasma marks, or black hole in explosions | Exposure-scaled/converted darken decals, `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` on energy marks, or decals submitted after explosions | Upload raw darken textures, use `SRC_ALPHA/ONE` for energy marks, submit decals **before** explosions, clamp `cameraNear = 1..3u` (§1.5, Pitfall 22) |
| Solid black boxes around sprites, pitch-black smoke, or mirrored `"1"` sprites | 32-bit opaque-alpha sprite classified as alpha-blend, zeroed `smokeLook`, or `viewaxis[1]` mapped to `u = 1` | Scan `any(alpha < 250)`, populate non-zero `smokeLook`, map camera-left `+Y` to `u = 0` (§1.5, Pitfalls 23–24) |
| Deadlock on screenshot or crash/UAF on exit | Spin-waiting `qrGetFrameReadbackStatus` without pumping frames, or freeing `readbackBuffer` before `qrDestroyInstance` | Pump frames while polling; keep `readbackBuffer` alive past `qrDestroyInstance` (§1.6, Pitfall 27) |
| `VUID-09600` / `VK_ERROR_DEVICE_LOST` on storage textures | `VK_IMAGE_LAYOUT_GENERAL` image wrapped via `wrapEngineTexture` | Use `rhi::wrapEngineStorageImage` and verify `0` VUIDs under `VK_LAYER_KHRONOS_validation` (Part 4, Pitfall 30) |
| Shimmering/banding walls under FSR Performance | Stale FFX `.so`, `PrepareForSize` after `FillUniform`, or missing `interpolate_lf` plane distance | Keep `PrepareForSize` before `FillUniform`, capped `fHistoryWeight`, plane-distance `interpolate_lf`, rebuild FFX `.so` (Pitfall 13) |
| Edited `config.cfg` or updated code defaults ignored on map load | Archived `q3config.cfg` stomping authored settings on every frame | Load authored files first, clear RT cvar `modified` flags, apply with `onlyModified = true` (§1.2, Pitfall 29) |
| Dynamic entity silently missing or static upload throws `QR_WRONG_ARGUMENT` | `uniqueID` collision with `EditorSceneRuntime`/`EditorDtalPipeline`/`EditorOverlayPipeline` | Allocate host dynamic IDs in `>= 0x4000000000000000ull` (§1.2, Pitfall 26) |

Every item below happened in real integration and debugging sessions. Treat each as a hard invariant.

### Pitfall 1: Duplicating Static Scene, Material, DTAL, or Lighting Pipelines Between Editor and Runtime
- **Symptom:** The editor (`quakerayeditor`) and the live game (`renderer_qray`) diverge on the exact same map: different DTAL area lights, missing material bakes, missing overlays/cutouts, different sky/sun defaults, or a bug fixed in one target while still broken in the other.
- **Root Cause:** Building a separate host-side loader/pipeline parsing `qray.materials.yaml` / `qray.lights.yaml`, building DTAL, or uploading static BSP geometry separately instead of reusing the editor's pipeline in one shared layer.
- **Fix:** Never duplicate static world pipelines. Keep UI-free shared logic in `QuakeRaySDK/editor/` (`EditorSceneRuntime`, `EditorDtalPipeline`, `EditorOverlayPipeline`, `EditorSession`, `EditorMaterial`, `EditorSettings`, `EditorSpatial`), compile that shared core + the single host `EditorFormatAdapter` into **both** executables, bridge live game VFS with `EditorAssetSource::SetVfsCallbacks`, set `includeInlineSubmodels = false` for runtime moving brush submodels, and enforce 1-to-1 parity via an automated test (`qray_authored_tests`).

### Pitfall 2: Mixing Adapter Code into `QuakeRaySDK`
- **Symptom:** Game-specific loaders (`<Game>Bsp.cpp`, `<Game>Shader.cpp`, `<Game>Model.cpp`, `<Game>EditorAdapter.cpp`) get committed inside `QuakeRaySDK/editor/`, polluting SDK PRs with game-specific internals.
- **Root Cause:** Treating `QuakeRaySDK/editor/` as the owner of every game's asset format instead of keeping only the generic adapter hook (`EditorFormatAdapter`) in SDK.
- **Fix:** Keep all game-specific loaders in the host repo, build them via `QR_EDITOR_ADAPTER_SOURCES` / `QR_EDITOR_ADAPTER_INCLUDE_DIRS`, and put only generic SDK/UI features and UI-free shared scene runtime code in `QuakeRaySDK` PRs.

### Pitfall 3: Predefined Surface Lights Without Selection Circles in `Light Editor`
- **Symptom:** Lamps, ceiling panels, torches, and jump pads cast real-time ray-traced light in `QuakeRay`, yet have **no cyan selection circle**, do not appear under `Light Editor -> Entity`, and cannot be turned off or deleted.
- **Root Cause:** Adapter ignored `q3map_surfacelight` / `lights.rad` brushes or excluded traced surfaces from `scene.lights`, leaving them to DTAL / `QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT` (`BuildEmissiveLights`). DTAL area lights generated purely from textures do not create `EditorLight` gizmos/entities.
- **Fix:** Extract **all** predefined map emitters (map entities **and** all `q3map_surfacelight > 0` / `lights.rad` surfaces/fixtures, bucketed and deduplicated per fixture) into `scene.lights` and `scene.bspLights` with valid `className` and cluster, and avoid auto-emitting hidden DTAL/ray-only surface light duplicates when `usesExternalLightingModel` is true.

### Pitfall 4: Hardcoded Model Filters & Deleted Standard Lights Returning on Reload
- **Symptom:** Standard map lights were filtered via hardcoded strings like `"flame.mdl flame2.mdl"` into `qray.materials.yaml`, OR deleting all standard lights in `Light Editor` caused every light to reappear on the next map load.
- **Root Cause:** Filtering by engine-specific model names breaks general engines, and omitting the `lights:` YAML block when `m_scene.lights` is empty makes the loader fall back to BSP defaults instead of honoring user deletions.
- **Fix:** Never hardcode game model names in `QuakeRaySDK/editor/`. Store all predefined map lights in `scene.lights` / `scene.bspLights`, serialize empty standard lights explicitly as `lights: []` in `qray.lights.yaml`, and reserve BSP reset strictly for explicit `Regenerate from map`.

### Pitfall 5: Multi-Stage Shaders Rendering Black, Double-Dark, or Leaking Sky
- **Symptom:** Launch pads, teleporters, animated monitors, grates, banners, or flames appear black, show opaque black rectangles instead of glow, or show sky through cutout holes.
- **Root Cause:** Flattening multi-stage shaders to stage 0, keeping lightmap `GL_DST_COLOR GL_ZERO` multiply stages after RT replaced lightmaps, failing to clean black fringes on additive maps (`CleanAdditiveBlackBackground`), misclassifying `wetGlossAlphaMask` as cutouts, or tracing binary alpha cutouts without a backing layer.
- **Fix:** Separate traced base (`EditorSurface`) from animated/blended overlay stages (`EditorOverlaySurface`), strip lightmap/redundant `DST_COLOR ZERO` stages, offset cutout-underlay backing meshes inward with `OffsetEditorVerticesAlongSmoothNormals`, and follow §2.4's shader/parser rules.

### Pitfall 6: `std::__ios_failure: Is a directory` Crash on Bare Texture Names
- **Symptom:** Crash with `basic_filebuf::underflow error reading the file: Is a directory` when resolving a shader/material named e.g. `default` or a directory stem.
- **Root Cause:** On Linux/POSIX, `std::ifstream` succeeds in *opening* a directory path (such as `./textures/default/`), and only throws when attempting to read bytes from it.
- **Fix:** Always verify `std::error_code ec; std::filesystem::is_regular_file(candidate, ec)` before opening or reading any candidate path on disk (in `Assets.cpp`, `EditorApp.cpp`, and any host adapter helper).

### Pitfall 7: Stale `EditorMaterialBakes`, World Bake Wipe, & Frame-2 Dynamic Material Destruction
- **Symptom:** (a) In-place texture edits (`Generate with AI` / procedural masks) leave stale flat surfaces until restart; OR (b) editing a preview texture wipes unrelated level surfaces white; OR (c) every dynamic entity material crashes or turns invalid on **frame 2** of a runtime host.
- **Root Cause:** `EditorMaterialBakes` caches baked `QrMaterial` handles keyed by texture indices. Mutating a texture in place keeps the same index; calling global `m_materialBakes.Destroy(instance)` wipes live static geometry handles; and in a runtime host, `ResolveDynamicMaterial` returns cached handles from `m_dynamicMaterialCache` without setting `entry.used = true` in `dynamicMaterialBakes`, so calling `dynamicMaterialBakes.BeginPass()/EndPass(instance)` per frame destroys every cached dynamic material on frame 2! *(Also note current SDK caveat: `EditorMaterialBakes::InvalidateTexture` matches `albedo, gloss, normal, emissive, overlay, sssTex` and does not yet check `k.heightTex` — explicitly invalidate height-map bakes when regenerating POM depth in place.)*
- **Fix:** Never call `dynamicMaterialBakes.BeginPass()/EndPass()` in a runtime host (let `DestroyGpuResources` clean up on map unload/exit). For in-place editor texture edits, recreate only that slot via `EditorSceneRuntime::CreatePoolMaterial` and call targeted `InvalidateTexture(m_instance, existing)` on `m_materialBakes` and `m_previewMaterialBakes`.

### Pitfall 8: Broken Multi-Part Models, Out-of-Bounds MD3 Indices, & `./` Path Mismatches
- **Symptom:** Multi-part characters show detached torsos/heads, wrong leg animations, missing skins, crashes on custom MD3s, or fail to appear in the `Models` browser.
- **Root Cause:** Archive/filesystem paths mix `./`, `\`, and mixed case; `.skin` entries bind `surfaceName` (not `shaderName`); `animation.cfg` leg frame indices include the torso-only frame offset; and custom MD3 triangles can reference out-of-range vertex indices.
- **Fix:** Run `NormalizeEditorAssetPath` consistently; validate every MD3 index `< numVerts`; subtract the torso-only frame offset from leg animations in `animation.cfg`; bind `.skin` entries by `surfaceName`; and synthesize `<dir>.character` entries inside `postProcessModelCatalog`.

### Pitfall 9: Quality Gate Rejections (`check_quality.py --strict-tools` & `check_local.sh`)
- **Symptom:** CI fails with missing copyright headers, `clang-tidy`, `cppcheck`, `lizard`, `ruff`, or ASan/UBSan test errors even when a Release build compiles cleanly, OR editor code passes local `check_local.sh` and later breaks `-DQR_BUILD_EDITOR=ON`.
- **Fix:** Follow `QuakeRaySDK`'s strict quality rules from the start (and remember `tools/check_local.sh` and `quality-linux.yml` omit `-DQR_BUILD_EDITOR=ON`, while `check_quality.py` skips `dxc` if `dxc` is off `PATH` — see Part 4 §2):
  - **GPL copyright headers:** Run `python3 -B tools/copyright_header.py --fix <files>` on every newly added C/C++/HLSL/Python/shell file.
  - **`lizard` limits:** Every modified/added C++ function must stay within `CCN <= 15`, `NLOC <= 120`, `params <= 7`.
  - **`clang-tidy` pitfalls:** Never use `std::system()` (`cert-env33-c`; use `fork`/`execv`/`waitpid` or `CreateProcessW`); avoid `const` on returned/moved locals (`performance-no-automatic-move`); use `std::lround(...)` instead of `int(x + 0.5f)` (`bugprone-incorrect-roundings`); verify against `--compile-commands`.
  - **`ruff` rules:** Python scripts under `tools/` must pass `ruff check --select=E4,E7,E9,F,B`.

### Pitfall 10: Closed 360° Bezier Patches Flipping Inside-Out & Uncoordinated `rayLength` / `cameraFar` / `skyboxRadius`
- **Symptom:** Cylindrical pillars and arched corridors turn invisible from outside and look X-rayed from behind, or large arena vistas clip halfway across the map or clip skyboxes.
- **Root Cause:** Evaluating a full Bezier grid as one high-degree patch, summing radial vertex normals over a symmetric 360° `MST_PATCH` (which cancels to `(0, 0, 0)` and flips winding on `-0.0f` noise), blindly flipping planar faces without checking `lightmapVecs[2]`, or leaving `rayLength`, `cameraFar`, and `skyboxRadius` uncoordinated.
- **Fix:** Tessellate `MST_PATCH` stepping by `2` over overlapping 3×3 sub-patches, weld seam normals (`dist^2 < 1e-4`), validate winding locally per quad/triangle (and planar faces against `lightmapVecs[2]`), and scale distances together:
  - `EffectiveRayLength = max(cvar, 4096, mapDiag * 2.5)` (Quake units, converted to meters)
  - `EffectiveCameraFar = max(EffectiveRayLength, 65536)`
  - `skyboxRadius = 32768`, passing the exact same far distance to `UploadSky`.

### Pitfall 11: Half-Res Indirect Noise (`giBounceRays = 0.5f`), ReSTIR Flashing, & Test Ordering
- **Symptom:** Metallic/chrome reflections show softer/noisier indirect bounces under `giBounceRays = 0.5f`, OR firing weapons flashes the entire map (`rt_restir 1`), OR host unit tests fail after changing emitter upload order.
- **Root Cause:** At `giBounceRays = 0.5f`, `Source/Shaders/HLSL/CmQ2Adapter.comp.hlsl` (`q2FetchReconstructedIndirectSpec`) performs vertical bilateral reconstruction across alternating rows — which saves trace cost on diffuse rooms but blurs/adds variance on glossy and metallic reflections. Separately, `rt_restir 1` is **1-frame uniform RIS** (no temporal/spatial reservoir reuse), so adding/removing a transient dlight (`N -> N + 1`) remaps RNG candidate indices on every 3×3 ASVGF gradient pixel and flashes the whole scene.
- **Fix:** Default host `giBounceRays = 1.0f` when glossy/metallic clarity matters, keep clustered light lists (`rt_restir 0`) as the default host path, and write host cluster-light tests (`TestZeroPowerClusterSourcesFiltered`) to assert **order-independent membership** rather than rigid array indices.

### Pitfall 12: Map-Wide Shadow Flickering (~10%), Per-Shot Brightness Dips (~3%), & Bright-Lamp Starvation
- **Symptom:** The entire level pulses ~10% darker/brighter, every weapon shot dips whole-scene brightness ~3%, or bright lamps beside walls strobe at ~7.5 Hz.
- **Root Cause:** (1) Modulating static DTAL power by fast `EvaluateEditorWave` shader waves pulses scene lighting; (2) dim/zero dlights entering `qrUploadClusterLightSources` while `LightManager::AddSphericalLight` drops them (`< kLightMinColorSum = 0.0001f`) perturbs cluster partition weights / overflow-tail `beta`; (3) forgetting the `kEditorUnitsPerMeter^2` (`1600x`) power scale on dynamic cluster sources; (4) assigning a flat `1000u` `reach` or double-emitting BSP point lights + DTAL floods the per-cluster `Q2_LIGHT_LIST_MAX_PER_CELL` (`128`) fast-slot cap.
- **Fix:** Do not modulate static DTAL power by high-frequency overlay waves; filter `!isfinite || power <= 0` and `kLightMinColorSum` identically across spherical + cluster uploads; scale dynamic cluster power by `1600x`; compute physical `reach` from brightness; merge coplanar DTAL quads (`BrushDtalCluster`, default `rt_dtal_proxy = false`); and deduplicate surface fixture lights without double-emitting BSP + DTAL.

### Pitfall 13: FSR 3.1 "Heat Shimmer", Frame-0 Missing Jitter, 6×3 Performance Banding, & Unsynchronized Resets
- **Symptom:** Enabling FSR Performance/Ultra Performance blurs walls into shimmering "heat waves", produces 6×3 light banding on flat walls, skips jitter on frame 0/resize, turns low-res and pixelated, or desyncs framebuffers on preset changes.
- **Root Cause:** Running `FillUniform` before `PrepareForSize` on frame 0/resize skips subpixel jitter; uncapped `fHistoryWeight` causes shimmer while zeroing history disables temporal upscaling; naive depth weighting in `interpolate_lf` bleeds across 6×3 checkerboard/low-res blocks; and omitting `nvrhiFrameSkeleton->ReleaseFramebufferTargets()` before `framebuffers->PrepareForSize` leaves stale NVRHI targets.
- **Fix:** Ensure `PrepareForSize` runs before `FillUniform` on frame 0 and every resize; keep capped `fHistoryWeight` (`0.25` / `0.08` in `ffx_fsr3upscaler_accumulate.h`), tightened `RectifyHistory` bounds, and plane-distance `interpolate_lf` using `framebufSurfacePosition_Sampled` (`Source/Shaders/HLSL/CmQ2Atrous.comp.hlsl`); release NVRHI targets before resizing; and **always rebuild and redeploy `libamd_fidelityfx_vk.so` / `amd_fidelityfx_vk.dll`** whenever FidelityFX shader headers change.

### Pitfall 14: 4K FPS Collapse From Unculled Froxel Volumetric Light Shafts
- **Symptom:** Small arenas drop from ~200 FPS to ~45 FPS at 4K even with moderate triangle counts (~47k triangles).
- **Root Cause:** Oversized volumetric froxel grids ray-march deep into occluded rooms or empty zero-density space, or unstable `volumeCameraFar` / mismatched workgroup sizes break temporal froxel accumulation.
- **Fix:** Keep tightened volume resolutions (`128x72x64` / `192x108x96` / `256x144x128`), keep `[numthreads(4, 4, 4)]` in `CmLightShaftVolume.comp.hlsl`, keep `volumeCameraFar` stable across frames, preserve `CmLightShaftDepthBounds` (`cell.z > maxActiveSlice`) and zero-medium (`scatteringSigma <= 1e-6 && extinctionSigma <= 1e-6`) early-outs, and avoid duplicating static light emitters.

### Pitfall 15: Editor UX Regressions — Mirror Cubemaps, Disabled Stages, `Play Level`, & X11 `Tab` Focus
- **Symptom:** Toggling `mirror` keeps the old static cubemap texture; shader stage checkboxes do not toggle individual stages; `Play Level` launches without edited materials/lights/cfg; closing the launched game leaves `Tab` fly-mode broken in X11.
- **Root Cause:** Mirror promotion (`mirror && type == None`) and `disabledStagesMask` checks were bypassed by overlay-only (`isOverlayOnly`) or environment paths; `Play Level` launched outside the active editor/game config; returning from a child game window skipped X11 focus/pointer recovery.
- **Fix:** Propagate `disabledStagesMask` and mirror promotion identically through `EditorSceneRuntime`, `EditorOverlayPipeline`, and `EditorMaterial`; implement `buildPlayLevelCommand` (passing edited paths & `SDL_VIDEODRIVER=x11` in `envOverrides`); and handle X11 `FocusIn` / pointer-capture recovery (`EditorPlatformX11.cpp` / `EditorApp.cpp`) so `Tab` reliably re-enters fly mode after closing `Play Level`.

### Pitfall 16: Hardcoded `../gfx/textures` Breaking Standalone GPU Tests & Editor Deployment
- **Symptom:** `qray_spatial_twin_gpu` fails with `QR_ERROR_CANT_FIND_BLUE_NOISE`, or `quakerayeditor` post-build fails trying to copy missing `WaterNormal_n.png`.
- **Root Cause:** Hardcoding sibling-repo `../gfx/textures` paths instead of relying on bundled `Assets/textures/`.
- **Fix:** `QuakeRaySDK/Assets/textures/` bundles `BlueNoise_LDR_RGBA_128.png` and `WaterNormal_n.png`; `CMakeLists.txt` resolves `QR_TEST_TEXTURE_DIR` and `editor/CMakeLists.txt` resolves/copies each texture independently with `Assets/textures` fallback. Keep this per-file fallback intact when editing CMake rules.

### Pitfall 17: In-Process ML Inference (ONNX + WebGPU) Causing `earlyoom` & Vulkan `DeviceLostError`
- **Symptom:** Opening `Generate with AI` or switching model/resolution immediately locks up memory, triggers Linux `earlyoom` (>30 GiB RAM + 16 GiB VRAM exhausted), causes `Not enough memory for command submission`, and crashes `quakerayeditor` with `vk::DeviceLostError`.
- **Root Cause:** Running ONNX Runtime / WebGPU (Dawn) inside `quakerayeditor` executed unchunked quadratic attention concurrently with the 4K viewport renderer with no VRAM cap and no crash isolation.
- **Fix:** Follow Part 2.7 strictly — use isolated out-of-process `quakeray_ai_worker` on `ggml-vulkan` (`<= 75%` VRAM budget, 3-stage `LotusPipeline`, FlashAttention, automatic tiling, strict manual `Generate` trigger only, `prctl(PR_SET_PDEATHSIG, SIGKILL)` on Linux).

### Pitfall 18: HiDPI Panel Clipping, Destructive PBR Toggles, & Wiped `envMap` Masks
- **Symptom:** Controls truncate or overlap at 4K/1440p; unchecking a PBR section erases previously painted mask blocks on YAML save; unit tests testing `EditorMaterialBakes` produce zero gloss/emissive/SSS output; OR adding a normal map wipes embedded `roughnessMask`/`metallicMask` on `envMap` surfaces.
- **Root Cause:** Unscaled ImGui widget dimensions break above 1080p; `emissive_enabled`, `gloss_enabled`, and `sss_enabled` default to `false` unless toggled on, and `SampleBakeNormal` / `ResolveBakeContext` previously overwrote `envMap` roughness/metalness masks.
- **Fix:** Lay out panels using `EditorGuiScale()`, `EditorGuiFitButtonWidth()`, `EditorGuiComboWidth()`, and `EditorGuiButtonRow()` (`editor/EditorGui.h`); persist `emissive_enabled`/`gloss_enabled`/`sss_enabled` as non-destructive mute flags (explicitly setting them `true` in standalone bake tests); and preserve embedded `roughnessMask`/`metallicMask` inside `ApplyEmbeddedPbrMaskSample`.

### Pitfall 19: `Normal.a` Collisions, Relief POM Preconditions, & 5 Causes of Weak Bump/Gloss
- **Symptom:** Relief POM distorts glass/SSS or silently never activates, OR normal/gloss maps look flat and washed out up close.
- **Root Cause:** `Normal.a` packs three mutually exclusive masks (`ResolveBakedNormalAlpha`: active SSS -> glass transparency -> `texture_height`); Relief POM (`setupParallaxGeometry`) silently disables whenever no normal map is bound, layer-2 albedo IS bound, or `parallax_scale` (`layerColors[2].data[2]`, clamped `[0, 0.25]`) is zero; and weak bump/gloss stems from downsampling bakes, positive `mipLodBias`, `FAKE_ROUGH_SPECULAR_THRESHOLD` (`0.62..0.84`) roughness fade, sub-pixel Toksvig/LEADR (`min(normalMapStrength, 1.5)^2`), or low-contrast AI gloss percentile stretches. Also remember `IsChromeCapableMaterial` auto-promotes opaque gloss / `roughnessOverride < 0.02f` to `MIRROR`.
- **Fix:** Keep Relief POM bypassed on glass/water/SSS/overlay-alpha, bind a normal map with layer-2 albedo unbound when using POM, preserve up to `2048 px` bake resolution (`ComputeBakeTargetDimensions`), keep `mipLodBias <= 0`, and rely on `q2RegularizeDemodulatedSpecular` + sub-pixel LEADR instead of raising `minRoughness`.

### Pitfall 20: Multi-Engine Winding Inversions, Double-Shifted `visData`, `8192` Cluster Overflow, & Sealed Sky Portals
- **Symptom:** Porting id Tech 2 / GoldSrc / id Tech 3 engines leaves walls or models inside-out, drops cluster lights across corridors, leaves distant map halves unlit, or turns open skies pitch-black.
- **Root Cause:** (1) Clockwise fan/model winding against DXR culling; (2) shifting bits inside RLE `visData` by `c + 1` (double-shifting to `c + 2` because `Source/ClusterLightLists.cpp` already adds `+ 1`), or appending unnecessary trailing `0xFF` bytes; (3) exceeding `Q2_MAX_CLUSTERS = 8192` on leaf-indexed BSPs; (4) passing `0` instead of `QR_CLUSTER_LIGHT_NO_CLUSTER` (`~0u`) on unclustered lights; and (5) dropping `SURF_SKY` brushes flagged `SURF_NODRAW`.
- **Fix:** Reverse CW winding consistently across BSP faces and models; keep `visOffsets` and `skyVis` 1-shifted while keeping RLE `visData` bits **0-based** with no trailing `0xFF` padding; fold leaf-indexed maps to `<= 8191` spatial clusters; use `~0u` for unclustered lights; bleed opaque neighbor RGB into alpha-0 index-255 cutouts; and always emit `SURF_SKY` geometry with `QR_GEOMETRY_VISIBILITY_TYPE_SKY` even when `SURF_NODRAW` is set.

### Pitfall 21: Sending Sprites/Explosions to `qrUploadGeometry`, Particle Blend Misconceptions, & Faceted Models
- **Symptom:** Sprites render as shadow-casting cards, explosions/beams look like refractive glass blocks, cutouts clip differently than expected, or characters/weapons show flat crystalline facets.
- **Root Cause:** `qrUploadGeometry` has no alpha blending (`RtAlphaTest.rahit.hlsl` uses `(r+g+b)/3 * a + a < 0.5`), `QrParticleUploadInfo` has no custom blend-factor fields (fixed `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`), and `EXACT_NORMALS_BIT` / `GENERATE_NORMALS_BIT` overwrite smooth vertex normals with flat per-triangle cross products.
- **Fix:** Upload sprites, beams, translucent FX, decals, and additive shells via `qrUploadRasterizedGeometry` (point particles via `qrUploadParticles` accepting fixed alpha blending), and pass `flags = 0` with smooth unit normals in `QrVertex::normal` on dynamic models and curved/mesh BSP surfaces.

### Pitfall 22: Black Decals, Fake Blob Shadows, Diagonal Decal Slicing, Energy Rings, Explosion Holes, & `cameraNear` Bleed
- **Symptom:** Bullet marks render pitch black, fake circular shadows hover over RT shadows, decals slice diagonally across edges or flip at screen edges, `plasma_mrk` shows a black ring, fireballs have a black hole in the center, or decals Z-bleed across walls.
- **Root Cause:** Exposure-scaling darken decals, forgetting to filter engine blob shadows (`markShadow`/`projectionShadow`), projecting decals with 4-corner raycasts or orienting offsets along `viewaxis[0]`, using `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` on energy glow decals, submitting decals *after* explosions, and setting `cameraNear = 0.1u`.
- **Fix:** Drop fake engine blob shadows; project decals via Sutherland–Hodgman convex prism clipping against CPU world triangles (including `cutoutFrameUnderlay`); orient `+0.25u` offset toward `centroid - vieworg`; upload untouched raw darken textures (`ZERO / ONE_MINUS_SRC_COLOR`), use `SRC_ALPHA / ONE` for energy marks, submit decals **before** explosions/smoke, and clamp `cameraNear` to `1.0–3.0` Quake units (`0.025–0.075 m`).

### Pitfall 23: Black Boxes Around 32-Bit Additive Sprites, Missing `0`-RGB Projectiles, & Mirrored Billboards
- **Symptom:** Fireballs, flames, or HUD/world sprites carry solid black square corners, plasma bolts vanish mid-air, or floating sprites (`"1"` score plums, asymmetric FX) are horizontally flipped.
- **Root Cause:** 32-bit effect TGAs with `alpha == 255` everywhere get classified as alpha-blended instead of additive; `rgbGen identity` sprites pass `(0, 0, 0, 0)` `shaderRGBA`; and Quake's `viewaxis[1]` points **camera-left**, not right.
- **Fix:** Inspect actual texel alpha (`any(alpha < 250)`) before picking alpha vs additive blend, strip `"gfx/colors/black"` frames, coerce zeroed `rgbGen identity` entity color to `255`, and map `+left` (`viewaxis[1]`) to `u = 0` and `-left` to `u = 1` with CCW winding (`{0, 1, 2, 0, 2, 3}`).

### Pitfall 24: Over-Burned Explosions, sRGB-Crushed Shells, Double Spheres, Pitch-Black Smoke, & `SMOKE`/`PARTICLE` Conflict
- **Symptom:** Explosions clip harshly, glowing pickup spheres turn invisible or double-bright, smoke puffs render pitch black or trigger RGB debug colors, or `qrUploadRasterizedGeometry` throws `QR_WRONG_ARGUMENT` on smoke.
- **Root Cause:** Scaling 8-bit sRGB textures on CPU crushes linear brightness (`0.1^2.2 ≈ 0.0063`); `CleanAdditiveBlackBackground` eats soft `tcGen environment` gradients; `RhiRasterOverlayPass` disables hardware culling; zeroed `smokeLook` makes lit particles pitch black while `smokeLook.x > 0.5` activates debug RGB; and `QR_RASTERIZED_GEOMETRY_STATE_SMOKE` is mutually exclusive with `PARTICLE` / `PARTICLE_SPRITE`.
- **Fix:** Tune overlay brightness in linear `info.color`, evaluate `deformVertexes wave` in local space, guard `CleanAdditiveBlackBackground` with `!environmentMap`, CPU-cull backfaces on closed 3D overlays, never combine `SMOKE` with `PARTICLE*`, populate non-zero `smokeLook` (with `debugFlag = 0`, and `cluster = 0xFFFFFFFFu` / `QR_CLUSTER_LIGHT_NO_CLUSTER` matching HLSL `QR_PARTICLE_CLUSTER_SELF_LIT` for self-lit embers), and use `PARTICLE_SPRITE` + `qrSetParticleProxyGate` + `glassParticles` for particles behind glass.

### Pitfall 25: Windowing / OpenGL Collisions, Wayland Failures, X11 Macro Pollution, & `IMMEDIATE` Black Screens
- **Symptom:** `qrCreateInstance` fails on swapchain creation, Linux Wayland fails to obtain an Xlib display/window, C++ headers explode with cryptic syntax errors on `None`/`Status`/`Bool`, or `qrStartFrame` spams `QR_FEATURE_UNAVAILABLE` with a black screen.
- **Root Cause:** Creating an OpenGL context (`SDL_WINDOW_OPENGL` / `REF_GL`) on the Vulkan window; running native Wayland when the SDK supports only Win32/Xlib; leaking `<X11/Xlib.h>` macros into C++ headers; and requesting `QR_PRESENT_MODE_IMMEDIATE` on drivers that only expose `FIFO`/`MAILBOX`.
- **Fix:** Create a non-GL/Vulkan window (`SDL_WINDOW_VULKAN` or software window mode), force `SDL_VIDEODRIVER=x11` under Wayland (including in `buildPlayLevelCommand`), isolate X11 headers in one TU with immediate `#undef None Bool True False Status Success Always Convex Complex TileShape`, and fall back to `QR_PRESENT_MODE_VSYNC` + `maxFrameLatency = 1` on `QR_FEATURE_UNAVAILABLE`.

### Pitfall 26: Null/Zero `QrDrawFrameInfo` Parameter Traps, `EditorFrameParams` Copy Dangling Pointers, & `uniqueID` Collisions
- **Symptom:** Highlights clip brutally at `1.05` (`pTonemappingParams == nullptr`), legacy bloom blows out the frame (`pBloomParams == nullptr`), normal maps and all lightstyles go dead (zeroed `pTexturesParams`), post-effects crash after copying `EditorFrameParams`, or dynamic entities silently disappear.
- **Root Cause:** Uninitialized/null sub-blocks invoke incompatible fallback defaults; `EditorFrameParams::postEffects` holds internal pointers into the struct itself; and duplicate dynamic `uniqueID`s are silently dropped while duplicate static `uniqueID`s throw `QR_WRONG_ARGUMENT`.
- **Fix:** Populate explicit AgX `pTonemappingParams`, explicit `pBloomParams` (`bloomIntensity = 0`) + `postEffectParams.pBloom`, non-zero `pTexturesParams` (all 64 `lightStyleScales = 1.0f`), explicit `pIlluminationParams` (`cellWorldSize = 2.0f`), never copy `EditorFrameParams`, and allocate host dynamic IDs in `>= 0x4000000000000000ull`.

### Pitfall 27: Async Readback Exit Use-After-Free & Spin-Loop Deadlocks
- **Symptom:** Taking a screenshot freezes the game forever, or exiting after capturing a frame crashes inside the Vulkan driver / ASan reports heap-use-after-free.
- **Root Cause:** Polling `qrGetFrameReadbackStatus` in a tight loop without submitting frames starves GPU execution (deadlock), and freeing `readbackBuffer` before `qrDestroyInstance` leaves an active GPU DMA pointing at freed memory.
- **Fix:** Pump frames (`qrStartFrame`/`qrDrawFrame`) while polling readback status, gate requests behind a keep-alive counter, keep `readbackBuffer` alive until after `qrDestroyInstance`, handle resize resets + Y-flip, and sanitize screenshot filenames against `/`, `\`, `..`, `:`.

### Pitfall 28: Missing Back Faces on `cull none` Surfaces, `is_light` / YAML Resurrection, & `light_focus > 90°`
- **Symptom:** Thin banners/grates vanish from one side; clearing a single-stage `q3map_surfacelight` lamp still leaves it glowing or resurrects keys after Save; dark rooms look excessively noisy; or setting `light_focus > 90°` suddenly turns off cone focusing.
- **Root Cause:** Ray-traced backface culling drops single-sided `cull none` meshes; missing `defaultEmissiveTexture = def.map` prevents `texture_emissive: NONE` from suppressing base glow; forgetting to register a managed YAML key in `IsEmittedMaterialKey` lets `WriteMerged` resurrect old lines; and `EditorDtalPipeline.cpp` treats `light_focus` as a half-angle (`<= 90°`).
- **Fix:** Duplicate `twoSided` traced geometry with inverted normals/indices; populate `defaultEmissiveTexture` on single-stage surface lights; register every managed material key in `IsEmittedMaterialKey`; use `is_light` + `rt_filmgrain 0` in dark rooms and skip host procedural RME (`skipProceduralRme`) when `hasAuthoredOverride` is set; and author `light_focus` within `0..90°`.

### Pitfall 29: Archived Config Shadowing, Inverted 2D `SWAPCHAIN`, & Ignored `texCoordOffset`
- **Symptom:** Updated RT code defaults or editor-authored `config.cfg` values silently revert on map load; 2D HUD draws upside-down or throws `QR_WRONG_ARGUMENT`; or scrolling `tcMod` world overlays stay static.
- **Root Cause:** `CVAR_ARCHIVE` files (`q3config.cfg`) overwrite authored settings; `SWAPCHAIN` uses Vulkan Y-down NDC and forbids depth flags; and `RasterizedDataCollector.cpp` ignores `texCoordOffset` unless `QR_RASTERIZED_GEOMETRY_STATE_TEXCOORD_TRANSFORM` (`256`) is set.
- **Fix:** Load authored configs first, clear RT cvar `modified` flags, apply cvars per frame with `onlyModified = true` (and test with `+set sv_pure 0 +set fs_homepath <build> +set com_maxfpsUnfocused 0`); build Vulkan Y-down 2D projection (`proj[5] = +2/H, proj[13] = -1`) without depth state on `SWAPCHAIN`; and set `TEXCOORD_TRANSFORM` whenever `texCoordOffset` is non-zero.

### Pitfall 30: Broken Shader Rebuilds, HLSL Row-Major Matrix Reads, & Storage-Image `VK_ERROR_DEVICE_LOST`
- **Symptom:** `GenerateShaders.py` fails from repo root, compiled shaders rotate normals/cameras wrong, deployed `editor/shaders/*.spv` stays stale, or storage images trigger `VUID-09600`/`00344` and `VK_ERROR_DEVICE_LOST`.
- **Root Cause:** `GenerateShaders.py` uses relative paths assuming CWD is `Source/Shaders/`; HLSL `m[i]` reads a row whereas GLSL `m[i]` reads a column; Windows SDK `dxc.exe` lacks SPIR-V support; and wrapping `VK_IMAGE_LAYOUT_GENERAL` images via `wrapEngineTexture` instead of `wrapEngineStorageImage` violates Vulkan image layout rules.
- **Fix:** Run `CheckMatrixReads.py` and `GenerateShaders.py -rebuild` strictly from inside `Source/Shaders/`, use `getColumn(m, i)` / `transpose(m)[i]` in HLSL, use Vulkan SDK `dxc` (`QR_DXC`), update `Source/Shaders/Reflection/` via `CheckShaderProperties.py --record --force` on interface edits, verify byte-for-byte parity of `editor/shaders/*.spv`, wrap storage images with `rhi::wrapEngineStorageImage`, and test with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` (`0` VUIDs).

---

## Part 4 — Verification, Shader Builds, Validation, & Quality Gate Checklist

Always run these checks before opening or updating any `QuakeRaySDK` PR or host-engine integration PR:

### 1. Shader Rebuild Pipeline & Reflection Verification
Whenever any `.hlsl` / `.hlsli` file under `Source/Shaders/` changes, run strictly **from inside `Source/Shaders/`** (running from the repository root fails because `OUTPUT_FOLDER_PATH = "../../Build/"`; avoid spaces in paths; note `-gencomm` invokes bare `python`; on Windows set `QR_DXC` to **Vulkan SDK `dxc`**, not Windows SDK `dxc.exe` which lacks SPIR-V):

```bash
cd /path/to/QuakeRaySDK/Source/Shaders
python3 -B CheckMatrixReads.py
python3 -B GenerateShaders.py -rebuild
python3 -B CheckShaderProperties.py
```

- **HLSL matrix-read rule:** `floatRxC` mirrors GLSL `matCxR`, so `m[i]` reads a **row** in HLSL instead of a column — always spell single-index column reads as `getColumn(m, i)` or `transpose(m)[i]`.
- **Reflection recordings:** If you intentionally change shader descriptor bindings, push-constant layouts, or block offsets, refresh `Source/Shaders/Reflection/` with `python3 -B CheckShaderProperties.py --record --force` and commit the updated recordings.
- **Verify deployed `.spv` copies:** Confirm every copied `editor/shaders/*.spv` matches the rebuilt `Build/*.spv` byte-for-byte before testing.

### 2. QuakeRaySDK Unit Tests, Vulkan Validation, Sanitizers, & Quality Gate Caveats

```bash
# 1. Fast end-to-end local check (FidelityFX provider + Clang ASan/UBSan build + CPU tests + quality gate)
#    CAVEAT: tools/check_local.sh and quality-linux.yml do NOT pass -DQR_BUILD_EDITOR=ON!
bash tools/check_local.sh --asan --shaders

# 2. Manual build INCLUDING editor + isolated ggml-vulkan AI worker + compile_commands.json
cmake -S /path/to/QuakeRaySDK -B build/local-quality -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DQR_BUILD_TESTS=ON -DQR_BUILD_EDITOR=ON \
    -DQR_NVRHI_SOURCE_DIR=/path/to/nvrhi \
    -DQR_GGML_SOURCE_DIR=/path/to/ggml \
    -DQR_FFX_VK_SO=/path/to/libamd_fidelityfx_vk.so
cmake --build build/local-quality --parallel

# 3. CPU/policy tests (always pass -LE gpu under Clang ASan/LSan: system Vulkan loaders leak under LSan)
ctest --test-dir build/local-quality --output-on-failure -LE gpu

# 4. Standalone GPU suites (disable third-party implicit layers like Steam/MangoHud/OBS)
VK_LOADER_LAYERS_DISABLE=~implicit~ ctest --test-dir build/local-quality --output-on-failure -L gpu

# 5. Live Vulkan Validation check (require 0 VUID errors)
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LOADER_LAYERS_DISABLE=~implicit~ \
    ./build/local-quality/editor/quakerayeditor --scene <map> --shaders <shaders>

# 6. Strict headers and diff-gated static analysis against origin/main
#    CAVEAT: check_quality.py silently skips dxc if dxc is not on PATH, even with --strict-tools --shaders!
which dxc && python3 -B tools/copyright_header.py --check --base origin/main
python3 -B tools/check_quality.py --strict-tools --shaders --base origin/main \
    --compile-commands build/local-quality/compile_commands.json
```

- **Important CI & PR merge-order caveats:**
  - Because `tools/check_local.sh` and `quality-linux.yml` omit `-DQR_BUILD_EDITOR=ON`, always run step 2 + step 6 whenever editing `editor/*.cpp` so `clang-tidy`, `cppcheck`, `lizard`, and editor unit tests actually cover your changes.
  - Ensure Vulkan SDK `dxc` is on `PATH` before relying on `--strict-tools --shaders`.
  - **Submodule merge order:** Always merge `QuakeRaySDK` PRs **first**, then update the host repository's submodule pointer to the merged `main` commit (GitHub squash-merge creates a new commit SHA and invalidates pre-merge branch SHAs).

### 3. Host Engine Build, Headless `QR_STATIC` Parity Harness, `qray_probe`, & UI VM Limits

```bash
# Build host renderer + quakerayeditor (+ quakeray_ai_worker) + adapter & parity tests
cmake --build /path/to/host-build --target renderer_qray quakerayeditor qray_editor_adapter_tests qray_authored_tests --parallel
ctest --test-dir /path/to/host-build --output-on-failure
```

- **Headless GPU-less parity harness (`QR_STATIC` + stubbed `qr*`):** Compile `EditorSceneRuntime` + your host `EditorFormatAdapter` into a standalone test binary with `-DQR_STATIC` linked against lightweight stub `qr*` C ABI functions. That lets CI run real-map parity checks without a GPU and verify 1-to-1 identity of `surfaces`, `worldClusters`, `emissiveLights` (DTAL), `overlaySurfaces`, `lights`, and `skybox` between editor and runtime instances.
- **In-game center-ray probe (`qray_probe`):** Implement a lightweight console diagnostic command casting a CPU ray along the camera center against `EditorSceneRuntime::scene.surfaces`, printing hit surface index, material/shader name, cluster ID, PVS stats (`qrGetClusterLightStats`), and active emitter details — invaluable when diagnosing single-surface shading or missing lights in live maps.
- **Engine UI VM constraints:** Classic engine UI VMs (e.g. `q3lcc`) often require strict C89; renderer-selection cvars (`cl_renderer`) are latched and require `vid_restart`, while live `rt_*` cvars take effect immediately; and some host forks ship without UI VM source (expose settings through console cvars and `config.cfg`).
- **Deterministic test launch flags:** Run host benchmarks and automated map tests with `+set sv_pure 0 +set fs_homepath <build_dir> +set com_maxfpsUnfocused 0`.

### 4. Live Paired Smoke Test (`quakerayeditor` vs Runtime Game Executable)
Before declaring an adapter or pipeline integration finished:
1. Launch **both** `quakerayeditor --scene <map> --shaders <shaders>` and the runtime engine on the same real map under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` (`0` VUID errors) and compare startup logs to verify **1-to-1 match** in static BLAS triangles/vertices, surfaces, DTAL area lights, material bakes, cluster count, and skybox faces (with the runtime only adding dynamic BLAS instances for moving submodels/entities).
2. Verify multi-stage animated surfaces (jump pads, teleporters, banners, grates, flames, chrome), closed curved `MST_PATCH` cylinders, double-sided (`cull none`) traced geometry, `mirror` promotion, Relief POM (`texture_height` / `parallax_scale` + cavity AO), non-destructive PBR master toggles (`emissive_enabled`, `gloss_enabled`, `sss_enabled`), and per-stage visibility toggles render cleanly with zero black/void cutouts.
3. Verify FSR 3.1 presets scale crisply without "heat shimmer" walls or 6×3 banding, volumetric light shafts hold target 4K frame times without light-cluster strobing or ~3% per-shot brightness dips, specular firefly limiting keeps close-up normal detail crisp, HiDPI panels (`1080p`/`4K`) fit without clipping, and predefined lights (`q3map_surfacelight` / `lights.rad` fixtures and entity lights) survive **Apply** -> map reload -> **Regenerate from map**.
4. Verify combat & runtime FX in-game: bullet/scorch/plasma decals blend cleanly without black rings or fake blob shadows, live rocket explosions and lit/volumetric smoke trails render cleanly in front of wall scorches, 32-bit additive sprites/projectiles show no black boxes and keep camera-left UVs un-mirrored, pickup spheres/deformed shells glow cleanly without sRGB crushing, dynamic character/weapon models shade smoothly (`flags = 0`) across frame 2+, and async readback screenshots succeed without deadlocking or UAF on exit.
5. Verify the **Models** tab shows assembled characters with `.skin` switching without corrupting live world bakes, **Play Level** launches cleanly (including under Wayland via `SDL_VIDEODRIVER=x11`) and restores `Tab` fly-mode after exiting back to the editor, `vid_restart` recovers cleanly, and the 4-tab `Generate with AI` flow (`Upscale`, `De-lighting`, `Normals`, `Depth`) never auto-starts, shows `quakeray_ai_worker` progress cleanly without exhausting VRAM/RAM, and preserves editor stability on `Cancel` or error.
