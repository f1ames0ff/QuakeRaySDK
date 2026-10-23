# Renderer API guide

This is the concise host-facing map of the QuakeRaySDK renderer. The normative
C ABI and exact field layouts are in [qray.h](../Include/qray/qray.h). Start
with [embedding.md](embedding.md) for a walkthrough and use
[spatial-contract.md](spatial-contract.md) for units, axes and handedness.

Current API version: `QR_API_VERSION 1.06.0000`.

## Supported integration model

- The current package is a Win32, SDK-owned Vulkan/NVRHI renderer.
- The host creates an `HWND` and supplies it through `QrInstanceCreateInfo`.
- The SDK owns the Vulkan device, acceleration structures, render targets,
  swapchain and presentation. `qrDrawFrame` renders and presents.
- The host owns game logic, scene extraction, assets, window lifetime, input
  and its application frame loop.
- Non-Windows surfaces and host-owned Vulkan device/render-target embedding are
  not part of this API revision.
- All spatial inputs are meters. See [spatial-contract.md](spatial-contract.md).

## Minimal lifecycle

```text
qrCreateInstance
  qrSetSpatialConvention
  create materials / cubemaps
  upload static geometry
  upload world partitions and scene lights

repeat each frame:
  qrStartFrame
  upload dynamic geometry / overlays / particles / frame lights
  qrDrawFrame

qrDestroyInstance before destroying the HWND
```

There is no separate `qrEndFrame`: `qrDrawFrame` ends, submits and presents the
frame. If `qrStartFrame` succeeds, call `qrDrawFrame` before starting another
frame.

## Ownership and lifetime

| Item | Owner | Lifetime rule |
|---|---|---|
| `HWND` | Host | Must outlive `QrInstance`. Destroy the instance first. |
| Vulkan/NVRHI device, swapchain, render targets, BLAS/TLAS | SDK | Owned and released by `qrDestroyInstance`. |
| `QrMaterial`, `QrCubemap` | SDK object, host handle | Destroy with their API when no scene object references them; otherwise instance teardown releases them. |
| Geometry `uniqueID` | Host-assigned | Stable for the object's rendered lifetime; changing winding sign requires re-upload. |
| Upload pointers (`pVertices`, `pLights`, frame parameter pointers, readback `pDst`) | Host | Must remain valid for the duration of the call. The SDK copies scene uploads synchronously. |
| Readback destination | Host | Must remain valid until `qrGetFrameReadbackStatus` reports done. |
| Shaders, blue-noise asset, FSR DLL | Host deployment | Paths are supplied to instance creation; files must exist at runtime. |

Use one render/frame thread unless a narrower concurrency rule is explicitly
documented. Upload-lane and dynamic-shape calls are diagnostics or tuning, not
a general promise that arbitrary host containers are thread-safe.

## Public handles and common types

- `QrInstance` - renderer/device/swapchain instance.
- `QrMaterial` - immutable or updateable material handle; `QR_NO_MATERIAL` is
  the null value.
- `QrCubemap` - cubemap handle; `QR_EMPTY_CUBEMAP` is the null value.
- `QrResult` - every fallible call returns a status. Use
  `qrGetResultDescription` for logs.
- `QrFloat2D`, `QrFloat3D`, `QrFloat4D`, `QrExtent2D`, `QrExtent3D`,
  `QrRect2D`, `QrViewport`, `QrTransform`, `QrMatrix3D` - shared value types.
- `QrBool32` uses `QR_TRUE` / `QR_FALSE`; zero initialization is safe.

## API catalogue

### Instance, spatial frame and global scene state

| Function | Purpose |
|---|---|
| `qrCreateInstance` | Create the renderer from `QrInstanceCreateInfo`. |
| `qrDestroyInstance` | Destroy all SDK GPU resources; call before the `HWND`. |
| `qrSetSpatialConvention` | Declare host axes and handedness before spatial uploads. |
| `qrGetSpatialConvention` | Read the active convention; initialize its `size` and `version` before the call. |
| `qrSetSmokeWindDirection` | Set persistent smoke advection direction in host scene space. |
| `qrGetSmokeWindDirection` | Read the current smoke wind direction. |

Instance creation also selects texture limits, shader and blue-noise paths,
texture override conventions, color spaces and optional feature budgets. See
`QrInstanceCreateInfo` for the complete field list.

### Ray-traced geometry

| Function | Purpose |
|---|---|
| `qrBeginStaticGeometries` | Open a static world submission window. |
| `qrUploadGeometry` | Upload static, static-movable or dynamic triangle geometry described by `QrGeometryUploadInfo`. |
| `qrSubmitStaticGeometries` | Close the static window and build/update the static acceleration structures. |
| `qrUpdateGeometryTransform` | Change the transform of stable static-movable or dynamic geometry. |
| `qrUpdateGeometryTexCoords` | Patch one or more UV layers without re-uploading the mesh. |

Static geometry normally uploads once per map. Movable and dynamic geometry can
be uploaded again after `qrStartFrame`; use stable `uniqueID` values so history,
lights and renderer-side matching remain coherent.

### Rasterized scene, overlays, particles, decals and portals

| Function | Purpose |
|---|---|
| `qrUploadRasterizedGeometry` | Upload `DEFAULT`, `SKY` or `SWAPCHAIN` raster geometry using the pipeline state in the payload. |
| `qrUploadParticles` | Upload per-frame camera-facing or smoke particle points. |
| `qrUploadDecal` | Upload a decal transform and material. |
| `qrUploadPortal` | Upload the two-sided transform data for a portal. |
| `qrSetParticleProxyGate` | Tell the renderer whether particle proxies may be traced behind glass. |

`DEFAULT` and `SKY` are world-space. `SWAPCHAIN` is screen-space in pixels and
is used for HUD, menus and other overlays. `FORCE_LINE_LIST` geometry is not
triangle-rewound. The raster upload limits come from `QrInstanceCreateInfo`.

### Materials and cubemaps

| Function | Purpose |
|---|---|
| `qrCreateMaterial` | Create a material from albedo/alpha, roughness-metallic-emission and normal textures or a relative path. |
| `qrCreateAnimatedMaterial` | Create a material with multiple switchable frames. |
| `qrChangeAnimatedMaterialFrame` | Select the active frame of an animated material. |
| `qrUpdateMaterialContents` | Replace texture contents for an updateable material. |
| `qrCanUpdateMaterialContents` | Query whether a material accepts an update of the given size. |
| `qrDestroyMaterial` | Destroy a material handle when no remaining scene object uses it. |
| `qrCreateCubemap` | Create a cubemap from six faces or relative face paths. |
| `qrDestroyCubemap` | Destroy a cubemap handle. |

Materials are referenced by `QrLayeredMaterial` in geometry and by raster,
decal and area-light payloads. The primary texture contract is three logical
inputs: albedo/alpha, combined roughness/metallic/emission and normal.

### Scene and dynamic lights

| Function | Purpose |
|---|---|
| `qrUploadDirectionalLight` | Upload sun/global directional light via `QrDirectionalLightUploadInfo`. |
| `qrUploadSphericalLight` | Upload a point/spherical light. |
| `qrUploadSpotLight` | Upload a cone light. |
| `qrUploadPolygonalLight` | Upload a three-vertex polygonal light. |
| `qrUploadTexturedAreaLight` | Upload one textured area light. |
| `qrUploadTexturedAreaLights` | Upload a batch of textured area lights. |
| `qrUploadDtalGroups` | Upload dynamic textured area-light groups and their members. |
| `qrBeginDeferredLightUploads` | Open an optional balanced deferred light upload batch. |
| `qrEndDeferredLightUploads` | Close the deferred upload batch. |
| `qrFlushDeferredLightUploads` | Commit the deferred upload batch to the active frame. |

Light types use the color and area conventions in
[spatial-contract.md](spatial-contract.md). For ordinary hosts, upload the
lights relevant to the current frame after `qrStartFrame`. Deferred batching is
an optional optimization for hosts with parallel scene producers.

### Clusters, world visibility and world-light faces

| Function | Purpose |
|---|---|
| `qrUploadWorldLights` | Upload cluster bounds, flags, optional PVS tables, sky visibility and emissive world faces. |
| `qrUploadClusterLightSources` | Convenience call: process the complete cluster light-source set synchronously. |
| `qrBeginClusterLightSources` | Start the sliced cluster-light pipeline for a host-managed task graph. |
| `qrRunClusterLightSourceSlice` | Process one source-top-up slice. |
| `qrFinishClusterLightSources` | Merge source slices and prepare list publication. |
| `qrRunClusterListPublishSlice` | Publish one slice of the cluster lists. |
| `qrCommitClusterListPublication` | Commit the published cluster lists for rendering. |
| `qrGetClusterLightStats` | Read cluster build/publication counters and timings. |
| `qrGetClusterLightGrants` | Read how many clusters each frame light was granted or denied. |
| `qrGetClusterLightList` | Inspect one cluster's fast light list. |
| `qrGetClusterLightTail` | Inspect the statistical overflow tail of one cluster. |

Hierarchy:

```text
qrUploadWorldLights
  -> one cluster-light source update per frame/when lights change
  -> qrDrawFrame
```

`qrUploadClusterLightSources` is the simple path. Hosts that already run a
task graph can use the begin/slice/finish/publish/commit sequence directly.
Cluster IDs and PVS bytes are topology, not spatial units. Omitting both PVS
tables requests the SDK's all-visible fallback.

### Frame lifecycle, fog and global availability

| Function | Purpose |
|---|---|
| `qrStartFrame` | Acquire a frame slot and swapchain image from `QrStartFrameInfo`. |
| `qrDrawFrame` | Consume `QrDrawFrameInfo`, render, submit and present. |
| `qrSetFogVolumes` | Replace the persistent local fog-volume set. |
| `qrIsSuspended` | Query whether presentation is currently suspended (for example when minimized). |
| `qrIsRenderUpscaleTechniqueAvailable` | Query FSR/DLSS/fallback availability before selecting frame resolution settings. |

After `qrStartFrame`, upload all frame-scoped geometry, overlays, particles and
frame lights, then call `qrDrawFrame` exactly once.

### Frame payload

The required camera portion of `QrDrawFrameInfo` contains the column-major
world-to-view matrix, vertical FOV, near/far planes, ray length, optional up
vector, ray-cull mask and current time. The renderer constructs projection and
owns all output images. Optional pointer blocks select rendering features:

| Block | Controls |
|---|---|
| `QrDrawFrameRenderResolutionParams` | Native/custom render size, upscaler and sharpen technique. |
| `QrDrawFrameIlluminationParams` | Bounces, denoiser, light sampling, ReSTIR and GI controls. |
| `QrDrawFrameVolumetricParams` | Optional atmospheric/volumetric in-scattering. |
| `QrDrawFrameTonemappingParams` | Exposure and tone curve. |
| `QrDrawFrameBloomParams` | Bloom intensity and threshold. |
| `QrDrawFrameReflectRefractParams` | Water, acid, glass, portals and their media properties. |
| `QrDrawFrameSkyParams` | Sky type, sun disc, clouds, god rays and cubemap rotation. |
| `QrDrawFrameTexturesParams` | Normal strength, material sampling, emission and light-style behavior. |
| `QrDrawFrameLevelFogParams` | Global level fog color and density. |
| `QrDrawFrameDebugParams` | Diagnostic visualization flags. |
| `QrDrawFramePostEffectsParams` | Wipe, blur, tint, CRT, DOF, vignette, film grain and gameplay effects. |

Pass `nullptr` for optional blocks to use renderer defaults. The SDK does not
retain these pointers after `qrDrawFrame`.

### Readback, screenshots and diagnostics

| Function | Purpose |
|---|---|
| `qrGetFrameReadbackRequirements` | Get final-image dimensions, pitch and buffer size for a supported format. |
| `qrRequestFrameReadback` | Queue final LDR pixels into a host buffer. |
| `qrGetFrameReadbackStatus` | Poll an asynchronous readback request. |
| `qrRequestScreenshot` | File-based screenshot convenience; not a hot-path API. |
| `qrGetFrameStats` | Read legacy ray/FPS counters. |
| `qrGetFrameStatsEx` | Read detailed frame, GPU, CPU, raster and dynamic-BLAS statistics. |
| `qrGetCpuPassName` | Map a `QrCpuPassIndex` to a display name. |
| `qrGetGpuPassName` | Map a GPU pass index to a display name. |
| `qrGetAdapterInfo` | Read Vulkan adapter, driver and API identities. |
| `qrGetResultDescription` | Convert a `QrResult` to a stable human-readable description. |

Readback is asynchronous and returns the composited final image. Do not wait
for device idle; poll the request. `qrRequestScreenshot` may synchronize and
should be used outside performance-sensitive frames.

### Upload and dynamic-shape instrumentation

These calls are optional tuning/diagnostics for advanced hosts:

| Function | Purpose |
|---|---|
| `qrUploadLanePush` | Attribute subsequent uploads to a `QrUploadLane`. |
| `qrUploadLanePop` | Leave the current lane. |
| `qrSetUploadLockTracing` | Enable upload-lock timing collection. |
| `qrUploadLockStatsReset` | Clear upload-lock statistics. |
| `qrGetUploadLockStats` | Read `QrUploadLockStats`. |
| `qrSetDynamicShapeSortedReuse` | Enable/disable sorted dynamic-BLAS shape reuse. |
| `qrSetDynamicShapeHeadroom` | Set dynamic shape allocation headroom. |
| `qrSetDynamicShapeRefit` | Enable/disable refit policy where supported. |

They are not required for a first correct integration.

## Error and initialization rules

- Check every `QrResult`; fail initialization on `qrCreateInstance` or
  `qrSetSpatialConvention` failure.
- Zero-initialize public structs before filling them.
- Call `qrSetSpatialConvention` before the first spatial upload or frame.
- Do not pass host-owned Vulkan objects; this revision does not expose device
  sharing.
- Upload arrays are borrowed for the call. Do not free them while the call is
  executing.
- Destroy the renderer instance before the Win32 window.
- Deploy compiled shaders, the blue-noise texture and the FidelityFX provider
  next to the executable or configure their paths explicitly.

## Conformance expectations

The engine-free tests exercise spatial conversion, frame readback, world-light
fallback, mirrored scenes, clouds, post effects, dynamic geometry and
non-uniform vertex transforms. A new host adapter should preserve the API
contract rather than reproducing QuakeRay's native units or coordinate frame.
