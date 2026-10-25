# Renderer API guide

See [README.md](README.md) for the recommended integration reading order.

This is the concise host-facing map of the QuakeRaySDK renderer. The normative
C ABI and exact field layouts are in the installed `qray/qray.h`. Start
with [embedding.md](embedding.md) for a walkthrough and use
[host-contract.md](host-contract.md) for the adapter boundary,
[sky-and-atmosphere.md](sky-and-atmosphere.md) for sky/cloud setup and
[spatial-contract.md](spatial-contract.md) for units, axes and handedness.
The [host feature matrix](host-feature-matrix.md) links the focused guides for
sun/god rays, lights/DTAL/NEE, GI/ReSTIR/denoiser and other frame blocks.

Current API version: `QR_API_VERSION 1.06.0000`.

The package is source-level: rebuild the host and SDK together when updating.
`QR_API_VERSION`, `QR_SPATIAL_CONVENTION_VERSION` and the in/out
`QrSpatialConvention` header are compatibility checks for the C ABI. The C++
host/sidecar/GPU contracts use their own `QrayContractHeader` versions. This
revision does not promise a binary plugin ABI.

## Supported integration model

- The current package is a Win32, SDK-owned Vulkan/NVRHI renderer.
- The host creates an `HWND` and supplies it through `QrInstanceCreateInfo`.
- The SDK owns the Vulkan device, acceleration structures, render targets,
  swapchain and presentation. `qrDrawFrame` renders and presents.
- A C++20 host can pass requirements before device creation and retrieve the
  shared NVRHI/Vulkan context through `qray/gpu_context.h`. The SDK remains the
  sole device owner. See [gpu-context.md](gpu-context.md).
- The host owns game logic, scene extraction, assets, window lifetime, input
  and its application frame loop.
- Non-Windows surfaces and host-owned Vulkan device/render-target embedding are
  not part of this API revision.
- All spatial inputs are meters. See [spatial-contract.md](spatial-contract.md).

## Minimal lifecycle

```text
qrCreateInstance                         (SDK-default requirements)
  or qray::qrCreateInstanceWithGpuRequirements (host-specified requirements)
qray::qrGetGpuContext                    (optional shared GPU access)
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

Use only one creation path. A host that needs the shared NVRHI device must not
create a second device after `qrCreateInstance`.

There is no separate `qrEndFrame`: `qrDrawFrame` ends, submits and presents the
frame. If `qrStartFrame` succeeds, call `qrDrawFrame` before starting another
frame.

### Instance creation

The create-info struct itself is borrowed only for instance creation, through
either `qrCreateInstance` or `qray::qrCreateInstanceWithGpuRequirements`. Its
Win32 surface must describe an `HWND` that outlives the instance, and the file
callbacks plus `pUserLoadFileData` must remain callable for the instance
lifetime. Keep every string and path alive through the creation call; do not
assume the SDK retains those character buffers.

The create info must contain a valid Win32 surface and a non-zero
`rasterizedSkyCubemapSize`; both albedo-layer limits must not exceed the
renderer's three-layer material limit. A host should also set an application
name, shader folder, blue-noise file, file callbacks and sensible raster vertex
and index capacities.

`pConfigPath` selects the renderer's local configuration file and may be null.
The full set of texture-override paths and color-space flags controls optional
asset overrides; they are not required for a host that creates materials from
in-memory RGBA8 data or its own UTF-8 relative texture paths.

## Ownership and lifetime

| Item | Owner | Lifetime rule |
|---|---|---|
| `HWND` | Host | Must outlive `QrInstance`. Destroy the instance first. |
| Vulkan/NVRHI device, swapchain, render targets, BLAS/TLAS | SDK | Owned and released by `qrDestroyInstance`. |
| Shared `QrayGpuContext` | SDK | Borrowed through `qrGetGpuContext`; invalid after `qrDestroyInstance`. |
| Host-created NVRHI/Vulkan resources | Host | Use the returned device; release before `qrDestroyInstance`. |
| `QrMaterial`, `QrCubemap` | SDK object, host handle | Destroy with their API when no scene object references them; otherwise instance teardown releases them. |
| Geometry `uniqueID` | Host-assigned | Stable for the object's rendered lifetime; changing winding sign requires re-upload. |
| Upload pointers (`pVertices`, `pLights`, frame parameter pointers, readback `pDst`) | Host | Must remain valid for the duration of the call. The SDK copies scene uploads synchronously. |
| Readback destination | Host | Must remain valid until `qrGetFrameReadbackStatus` reports done. |
| Shaders, blue-noise asset, FSR DLL | Host deployment | Paths are supplied to instance creation; files must exist at runtime. |

Use one render/frame thread unless a narrower concurrency rule is explicitly
documented. Upload-lane and dynamic-shape calls are diagnostics or tuning, not
a general promise that arbitrary host containers are thread-safe.

### State lifetime

| State | Lifetime |
|---|---|
| Spatial convention | Instance lifetime after the first spatial/fog upload or drawn frame; set it before either. |
| Static and static-movable geometry, cluster topology, world-light faces | Persistent until explicitly replaced or the instance is destroyed. |
| Dynamic geometry | Current frame; upload it again after every `qrStartFrame` in which it is visible. |
| Rasterized geometry, particle points, decals and portals | Current frame; the queues are reset at the frame boundary. |
| Frame lights and deferred-light batch | Current frame. |
| Fog volumes, smoke wind, material and cubemap handles | Persistent until replaced/destroyed or the instance is destroyed. |
| `QrDrawFrameInfo` and its parameter blocks | Borrowed only for `qrDrawFrame`. |

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
- `packedColor` in `QrVertex` and `QrParticlePoint` is little-endian RGBA8:
  red in bits 0..7, green in 8..15, blue in 16..23 and alpha in 24..31.

## API catalogue

### Host driver contract layer

`qray/host_contract.h` is an optional C++20 source-level layer over the C ABI.
It defines `QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`,
`IQrayHost` and `QraySidecar`, plus `QraySceneBinding` for lowering scene and
frame records into the existing `qr*` calls.

The C ABI remains normative for transport and resource ownership. The contract
layer does not create a second scene path. It prevalidates borrowed input,
creates and owns scene materials, preserves source/runtime identity separation
and dispatches only supported updates. See
[host-driver-contract.md](host-driver-contract.md).

### Shared GPU context

`qray/gpu_context.h` is the optional NVRHI/Vulkan contract. It is independent
from the C scene transport and is versioned by `kGpuContextContractVersion`.

| Function | Purpose |
|---|---|
| `qray::qrCreateInstanceWithGpuRequirements` | Create the SDK-owned device after merging host requirements |
| `qray::qrGetGpuContext` | Retrieve borrowed NVRHI and native Vulkan handles |
| `qray::qrSetGpuFrameHooks` | Record host work in `BeforeRenderer` or `BeforePresent` |
| `qray::qrayValidateGpuRequirements` | Validate a requirements descriptor without creating a device |
| `qray::qrayGetGpuContractInfo` | Query limits, contract version and minimum Vulkan version |
| `qray::qrayGetGpuContextContractVersion` | Query the numeric GPU-context contract version |

See [gpu-context.md](gpu-context.md) for ownership, callbacks and the complete
creation sequence.

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
| `qrSetParticleProxyGate` | Control traced particle proxies behind reflection/refraction surfaces. `gate`: `0` force open, `1` automatic, `2` force closed; `glassParticles` enables proxy capture for the frame. |

`DEFAULT` and `SKY` are world-space. `SWAPCHAIN` is screen-space in pixels and
is used for HUD, menus and other overlays. `FORCE_LINE_LIST` geometry is not
triangle-rewound. The raster upload limits come from `QrInstanceCreateInfo`.
`SWAPCHAIN` has no depth attachment and rejects depth-test/depth-write state.
For `SWAPCHAIN`, positions use a top-left pixel origin and `pViewProjection`
converts them to clip space; `scissor` uses the same pixel origin. A
full-viewport orthographic matrix has `m[0] = 2/width`,
`m[5] = 2/height`, `m[10] = -1`, `m[12] = m[13] = -1` and `m[15] = 1`
in the column-major array.

Raster entries preserve upload order only inside each emitted category. The
overlay records ordinary `DEFAULT` world draws, lit particle geometry,
point-particle batches and smoke in that fixed order; `SKY` and `SWAPCHAIN`
remain separate ordered streams. The SDK does not sort opaque, alpha-tested or
blended draws. Submit opaque/alpha-tested first, then blended far-to-near, and
sort vertices/points inside a batch before upload when their overlap makes
order observable. The full contract is in the public header and the host
pattern is in
[embedding.md](embedding.md#6-sprite-and-effect-adapter-pattern). Blend state is
a source/destination pair from `QrBlendFactor` with an add operation;
DST_ALPHA, subtract/min/max and separate alpha equations are not exposed.
Concurrent upload calls are made safe by the renderer lock, but their relative
order is unspecified; a host must serialize calls itself when blending makes
that order observable.

`SMOKE` and particle state are valid only for `DEFAULT` geometry. `SMOKE` and
`PARTICLE` are mutually exclusive. `qrUploadParticles` is a specialized
point-sprite batch and always uses `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`; use
`qrUploadRasterizedGeometry` when another blend equation is required.

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
inputs: albedo/alpha, combined roughness/metallic/emission and normal. An
in-memory texture is one tightly packed RGBA8 level of
`QrMaterialCreateInfo.size`; the SDK generates mipmaps unless
`QR_MATERIAL_CREATE_DONT_GENERATE_MIPMAPS_BIT` is set. The
`QrTextureSwizzling` value defines the RME channel order. Albedo follows the
instance's original/override sRGB flags; material data and normals are linear.
A host that owns palette, 16-bit or compressed source formats must decode them
before this upload.

Cubemap faces are RGBA8 and square, with `sideSize` applying to in-memory
faces. The face order in `QrCubemapFaceData` and `QrCubemapFacePaths` is the
field order: `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z`. Supply either six in-memory
faces or six relative paths; the host owns any source-image decoding.

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

World-light vertices and cluster topology are persistent scene state. An
ordinary host uploads the complete light set relevant to the current frame
after `qrStartFrame`; stable light `uniqueID` values let diagnostics and
cluster grant queries identify those submissions. The SDK does not parse the
host's entity or map format.

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

For interactive presentation use `QR_PRESENT_MODE_VSYNC` and
`maxFrameLatency = 1`. This enables waitable FIFO pacing. A latency of `0`
allows several submitted frames to precede their display and is intended for
throughput measurements, not normal gameplay.

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
| `QrDrawFrameSkyParams` | Sky type, sun disc, clouds, god rays and cubemap rotation; see [sky-and-atmosphere.md](sky-and-atmosphere.md). |
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
  `qray::qrCreateInstanceWithGpuRequirements`, and on `qrSetSpatialConvention`
  failure.
- Zero-initialize public structs before filling them.
- Call `qrSetSpatialConvention` before the first spatial upload or frame.
- The `qr*` C ABI does not accept host-owned Vulkan objects. Device sharing is
  the separate C++ `gpu_context.h` contract: create host resources on the
  returned device and use the documented frame callbacks.
- Upload arrays are borrowed for the call. Do not free them while the call is
  executing.
- Destroy the renderer instance before the Win32 window.
- Deploy compiled shaders, the blue-noise texture and the FidelityFX provider
  next to the executable or configure their paths explicitly.
- Link through `qray::renderer`; the target exports the required `QR_STATIC`,
  `QR_USE_SURFACE_WIN32` and `NOMINMAX` compile definitions. Do not mix a
  DLL-style `qray.h` declaration with the static `renderer.lib`.

## Conformance expectations

The engine-free tests exercise spatial conversion, frame readback, world-light
fallback, mirrored scenes, clouds, post effects, dynamic geometry and
non-uniform vertex transforms. A new host adapter should preserve the API
contract rather than reproducing QuakeRay's native units or coordinate frame.
The adapter-side feature mapping and unsupported legacy effects are in
[host-contract.md](host-contract.md).
