# Embedding QuakeRaySDK renderer

See [README.md](README.md) for the complete documentation map.

This guide is for a Windows (Win32) or Linux (X11/Xlib via
`QR_USE_SURFACE_XLIB`) host that links `qray::renderer` and owns its window,
assets and scene. The SDK owns the Vulkan/NVRHI device, swapchain and
presentation. The host may use a right-handed or left-handed
Z-up or Y-up frame, but it must convert all spatial values to meters before
calling the SDK. The renderer maps meter inputs into its private scene frame as
described in [spatial-contract.md](spatial-contract.md).

For the complete function-group catalogue and ownership rules, see
[api.md](api.md). For the host/adapter boundary and a mapping of familiar
pre-2006 renderer features, see [host-contract.md](host-contract.md). This page
is the step-by-step first integration; sky and volumetric-cloud settings are
described in [sky-and-atmosphere.md](sky-and-atmosphere.md), and
[host-feature-matrix.md](host-feature-matrix.md) links the advanced lighting
guides.
Hosts that want the implemented high-level source contract can use
`qray/host_contract.h` and `QraySceneBinding`; its lifecycle, supported update
set, sidecar boundary and failure semantics are in
[host-driver-contract.md](host-driver-contract.md). The raw `qr*` calls remain
available for engines with their own scene and task scheduling.

NVRHI-based hosts that use the shared GPU context include
`qray/gpu_context.h`. Hosts that need custom requirements pass them before
creation; afterwards they receive the SDK-owned NVRHI/Vulkan context as a
borrowed shared handle.
See [gpu-context.md](gpu-context.md) for the required ordering, frame callbacks
and destruction protocol.
The SDK-default device from `qrCreateInstance` can also be retrieved as shared
GPU context; the dedicated function is required only to negotiate host
requirements. Do not create a second NVRHI/Vulkan device for the same host
render target. If the host only uploads CPU scene data, do not use the shared
GPU context.

## 1. Build and link

The SDK is built as a static library on Windows and Linux. The host provides
NVRHI and Vulkan:

```cmake
find_package(Vulkan REQUIRED)
add_subdirectory(path/to/nvrhi)
find_package(qray_renderer CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE qray::renderer)
```

The SDK owns the shader sources/build script and installs the FidelityFX
headers, provider library (`amd_fidelityfx_vk.lib`/`.dll` plus `qray::ffx` on
Windows, `libamd_fidelityfx_vk.so` on Linux when present), and SDK docs. The
host integration step is deployment: copy the FSR provider shared library
(`amd_fidelityfx_vk.dll` on Windows or `libamd_fidelityfx_vk.so` on Linux),
compiled shaders and blue-noise texture next to the executable and pass their
paths to instance creation.
An installed SDK package therefore does not require the host to implement
NVRHI, shader compilation or FidelityFX tooling; the host must only provide
compatible `nvrhi`/`nvrhi_vk` build targets. The `qray::renderer` target exports
`QR_STATIC` plus `QR_USE_SURFACE_WIN32` and `NOMINMAX` on Windows, or
`QR_USE_SURFACE_XLIB` on Linux; keep those switches on the target instead of
copying them into unrelated host-wide definitions.

## 2. Create the instance

Fill `QrInstanceCreateInfo`, including the platform surface
(`pWin32SurfaceInfo` on Windows or `pXlibSurfaceCreateInfo` on Linux), file
callbacks, shader folder, blue-noise path, texture limits and optional
overridden texture paths. A C host or scene-only consumer creates the instance
with `qrCreateInstance`. A C++ NVRHI host that needs custom device requirements
uses `qray::qrCreateInstanceWithGpuRequirements` with the requirements described
in [gpu-context.md](gpu-context.md).

Immediately after creation, declare the host frame:

```c
QrSpatialConvention spatial = {
    .size = sizeof(QrSpatialConvention),
    .version = QR_SPATIAL_CONVENTION_VERSION,
    .sceneXAxis = QR_AXIS_POSITIVE_X,
    .sceneYAxis = QR_AXIS_NEGATIVE_Z,
    .sceneZAxis = QR_AXIS_POSITIVE_Y,
    .hostIsRightHanded = QR_TRUE,
    .reserved0 = 0,
};
QrResult result = qrSetSpatialConvention(instance, &spatial);
```

The axes above describe a right-handed Y-up host. QuakeRay's reference host
uses `POSITIVE_X`, `POSITIVE_Y`, `POSITIVE_Z`; its adapter converts Quake units
to meters. There is no scale field in the convention. If this call is omitted,
the SDK assumes identity axes and meter-valued inputs. The call must happen
before the first spatial upload or drawn frame.

## 3. Upload the scene

Geometry, light, cluster and fog calls take meter-valued spatial inputs. The
SDK applies only the axis/handedness conversion; its internal scene scale is
1:1:

- convert all positions, translations, radii, reaches, fog distances and
  camera distances to meters; convert areas to square meters and inverse
  lengths to per-meter values;
- upload geometry in meter-valued local space with a row-major 3x4
  `QrTransform` whose linear part is dimensionless and translation is in
  meters;
- use the per-type light color convention from the spatial contract;
- build the view as a column-major world-to-view matrix in the host world's
  handedness, with meter-valued translation, `-Z` forward and `+Y` up in view
  space;
- keep `QrDrawFrameInfo.cameraNear`, `cameraFar` and `rayLength` in meters;
- pass the smoke wind direction with `qrSetSmokeWindDirection` in host scene
  space;
- use `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` for HUD, menus and other
  screen-space geometry.

A minimal upload order is:

```text
qrCreateInstance
  -> qrSetSpatialConvention
  -> qrCreateMaterial / qrCreateCubemap
  -> qrBeginStaticGeometries
       qrUploadGeometry (static world)
     qrSubmitStaticGeometries
  -> qrUploadWorldLights / qrUploadClusterLightSources
  -> qrUploadDirectionalLight / point / spot / area / DTAL
  -> qrSetFogVolumes
```

The exact order can vary for dynamic content, but the spatial convention cannot
change after spatial state exists.

## 4. Frame loop

For a host with a task graph, the recommended sequence splits frame start from
swapchain acquisition:

```text
qrStartFrame
  -> run concurrently:
       renderer worker: qrAcquireFrame
       host: prepare/upload dynamic geometry, particles and raster overlays,
             and fill QrDrawFrameInfo (column-major view, FOV,
             near/far/ray length, up vector, sky/fog/volumetric/material
             parameters)
  -> join host producer work
  -> qrDrawFrame
```

`qrStartFrame` establishes and prepares the frame but does not acquire its
swapchain image. After a successful start, one caller can run
`qrAcquireFrame` on a worker while the host performs CPU producer work. The
acquire call performs the swapchain acquire and waits for the waitable-FIFO
present dependency. Its wait can therefore overlap the host's producer work and
renderer scene recording. All producers must be joined before `qrDrawFrame`, but
the acquire may still be running; `qrDrawFrame` records the scene first and joins
it before the present pass. Pending pre-frame material commands are submitted
after acquire and before the main frame submission.

This worker exception applies specifically to `qrAcquireFrame`. It does not
make other uploads or arbitrary host containers thread-safe; continue to
follow each producer call's ordering and synchronization rules. The SDK owns
the swapchain and acquired image, while the host owns the task graph, producer
lifetimes and the producer join. Use one explicit-acquire caller per frame; the
SDK's acquire gate makes a later `qrDrawFrame` join idempotently at present.

The original single-threaded sequence remains compatible:

```text
qrStartFrame
  -> upload dynamic geometry, particles and raster overlays for this frame
  -> fill QrDrawFrameInfo
  -> qrDrawFrame
```

When `qrAcquireFrame` is omitted, `qrDrawFrame` calls it lazily after scene
recording and before the present pass. Existing hosts therefore do not need to
adopt the explicit split.
Before a successful `qrStartFrame`, `qrAcquireFrame` returns
`QR_FRAME_WASNT_STARTED`; a suspended instance follows the existing `qr*`
`Call` behavior. Check the result and retain the requirement to call
`qrDrawFrame` before starting another frame after a successful start.

The renderer owns the projection, acceleration structures, render targets and
presentation. The host owns window messages, input, simulation and asset
loading. Do not submit a projection matrix; `QrDrawFrameInfo.view` is
world-to-view only.

For normal interactive output, request `QR_PRESENT_MODE_VSYNC` with
`maxFrameLatency = 1`. The SDK then uses the waitable FIFO path, and
`qrAcquireFrame` (including the lazy acquire inside `qrDrawFrame`) waits for
the previous displayed frame before acquiring the next image. Scene recording
can overlap that wait, which prevents submitted FPS from outrunning an uneven
display cadence without serializing all backend recording behind it.
`maxFrameLatency = 0` leaves the host free to queue ahead and is appropriate
only for explicit benchmarks or diagnostics. Mailbox requests
`VK_PRESENT_MODE_MAILBOX_KHR` directly. VSync, Adaptive and Immediate map
directly to their Vulkan modes. The SDK reports `QR_FEATURE_UNAVAILABLE`
instead of substituting an unrelated mode.

Use `qrGetFrameReadbackRequirements`, `qrRequestFrameReadback` and
`qrGetFrameReadbackStatus` for asynchronous final LDR capture. The readback
path is not a scene inspection API and includes the composited UI.

## 5. QuakeRay as the reference adapter

QuakeRay is the reference host. Its game code keeps Quake units and Z-up, while
`Quake/qr_spatial_adapter.*` converts spatial API payloads to meters, including
the legacy positional-light calibration. `SWAPCHAIN` UI geometry remains in
pixels. Treat the adapter and its identity-frame output as a regression oracle,
not as the definition of the public spatial contract.

The standalone `quakerayeditor` is an independent host built on
`qray::renderer`. It converts native units to meters at one boundary
(`EditorSpatial.{h,cpp}`, 40 units/m), loads OBJ/BSP scenes and archives via
`EditorAssetSource` and pluggable `EditorFormatAdapter` hooks, authors
materials/lights/render settings/local Vulkan AI PBR textures, and shares
`EditorSceneRuntime` with runtime hosts.

## 6. Sprite and effect adapter pattern

The SDK is a renderer, not a sprite subsystem. A host that loads `.spr` files,
Quake 3 shader effects or an equivalent format owns the adapter:

```text
parse the effect / select the animation frame
  -> build a camera-facing quad or point list in meters
  -> choose the material (albedo/alpha) and the blend state
  -> choose a traced or raster path
  -> submit opaque/alpha-tested draws before blended draws, far-to-near
  -> optionally register an emitter light
```

- **Traced path (`ALPHA_TESTED` cutouts):** `qrUploadGeometry` with
  `QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED` gives binary cutout visibility,
  cutout shadows, and GI for solid silhouette geometry (grates, fences,
  foliage, ironwork). In `RtAlphaTest.rahit.hlsl` and
  `RtAlphaTestShadow.rahit.hlsl`, default cutout rejection tests
  `(color.r + color.g + color.b) / 3 * color.a + color.a < 0.5`
  (`ALPHA_THRESHOLD = 0.5`, where `color` is the sampled layer-0 albedo/alpha
  multiplied by `layerColors[0]`) — note that bright RGB pixels pass down to
  `alpha = 0.25`, while black pixels cut off at `alpha = 0.5`. Setting
  `QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT` (mapped to
  `GEOM_INST_FLAG_ALPHA_TRANSMISSION` in `VertexCollector.cpp`) switches the
  any-hit shader to stochastic per-primitive/frame-hashed binary transmission
  (`rand < 1.0 - color.a`), NOT smooth ordered alpha blending. Because
  stochastic alpha transmission introduces noise and cannot represent additive
  or modulate blends, avoid routing soft translucent FX, decals, beams, or
  blended sprites through `qrUploadGeometry`.
- **Raster path (preferred for translucent FX, decals, and soft sprites):**
  `qrUploadRasterizedGeometry` with
  `QR_RASTERIZED_GEOMETRY_STATE_BLEND_ENABLE` and
  `QR_RASTERIZED_GEOMETRY_STATE_DEPTH_TEST` (no depth write) gives clean soft
  alpha, additive, and modulate blending for smoke, fire, explosions, beams,
  decals, soft sprites, and screen overlays. These draws do not participate in
  GI or sun shadows; add a light with `qrUploadSphericalLight` (or the host's
  own emitter path) if the effect should illuminate the scene.
  Use `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` for screen-space effects
  and upload them in painter's order.
- **Particles:** `qrUploadParticles` builds camera-facing point sprites in one
  batch with fixed alpha-over blending. `packedColor.a` is a coverage
  multiplier in the lit path; sort the points back-to-front before upload if
  they use alpha blending. Use rasterized geometry when another blend equation
  is required.
- **Ordering:** the SDK preserves upload order inside each emitted category and
  never sorts geometry.
  Submit opaque and alpha-tested cutouts first, then blended draws from far to
  near. Sort vertices/points inside a batch before upload when their overlap
  makes order observable. Keep different views in separate ordered ranges.
  Within `DEFAULT`, ordinary world, lit particle, point-particle and smoke
  categories are emitted in a fixed sequence, so interleaving those categories
  in upload order is not preserved. Concurrent calls are safe but their
  relative order is unspecified, so serialize order-sensitive uploads. The
  full contract is in the installed `qray/qray.h` next to the raster upload
  API.
- **Blend subset:** the SDK exposes source/destination pairs from
  `QrBlendFactor` with an add operation. It does not expose DST_ALPHA,
  subtract/min/max or a separate alpha equation; approximate a legacy mode
  with the supported factors or keep it out of the SDK raster path.

As an adapter example, a host can route oriented, translucent, or
force-rasterized sprites, decals, and beams to `qrUploadRasterizedGeometry`
with the matching blend and depth state, reserving `qrUploadGeometry`
(`ALPHA_TESTED`) for hard-edged cutouts that need RT shadows and GI (or glass /
water pass-through). Smoke is sorted by the host before upload. The SDK does
not parse `.spr`, choose animation frames, build billboards or create emitter
lights for the host.

## 7. Validation checklist

- Identity, Y-up and mirrored meter scenes must produce equivalent results.
  `qray_spatial_twin_gpu` covers geometry, camera, directional and spherical
  lights, level fog, particles, clouds, water and final readback.
- The Quake adapter must preserve the existing game image while converting its
  native data at the API boundary.
- Swapchain/UI geometry is pixel-positioned and never world-transformed.
- Left-handed hosts keep their native coordinates and view matrix; the
  renderer mirrors the frame and re-winds triangles. Only the world convention
  itself must be an axis-aligned signed permutation; instance transforms may
  carry arbitrary rotations and non-uniform scales, which the frame converts
  exactly. Reflected host data still needs its own triangle order reversed.
- Keep `rayLength` positive and finite. It is expressed in meters and is
  bounded by the 250,000 m safety ceiling; the depth buffer is `R32_SFLOAT`.
- Exercise the host's opaque, alpha-tested and blended ordering, screen-space
  UI, sky, particles and one dynamic-light path against the feature matrix in
  [host-contract.md](host-contract.md).
