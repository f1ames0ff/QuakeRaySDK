# Embedding QuakeRaySDK renderer

This guide is for a Win32 host that links `qray::renderer` and owns its window,
swapchain, assets and scene. The host may use its own units and a right-handed
Z-up or Y-up frame; the renderer converts its inputs into the internal spatial
frame described in [spatial-contract.md](spatial-contract.md).

## 1. Build and link

The SDK is a static library on Windows. The host provides NVRHI and Vulkan:

```cmake
find_package(Vulkan REQUIRED)
add_subdirectory(path/to/nvrhi)
find_package(qray_renderer CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE qray::renderer)
```

The installed package also exports `qray::ffx`. The host must deploy the FSR
provider DLL, compiled shaders and the blue-noise texture next to the
executable.

## 2. Create the instance

Fill `QrInstanceCreateInfo`, including the Win32 surface, file callbacks,
shader folder, blue-noise path, texture limits and optional overridden texture
paths. Create the instance with `qrCreateInstance`.

Immediately after creation, declare the host frame:

```c
QrSpatialConvention spatial = {
    .size = sizeof(QrSpatialConvention),
    .version = QR_SPATIAL_CONVENTION_VERSION,
    .sceneXAxis = QR_AXIS_POSITIVE_X,
    .sceneYAxis = QR_AXIS_NEGATIVE_Z,
    .sceneZAxis = QR_AXIS_POSITIVE_Y,
    .hostIsRightHanded = QR_TRUE,
    .unitsPerMeter = 1.0f,
};
QrResult result = qrSetSpatialConvention(instance, &spatial);
```

The axes above describe a right-handed Y-up host. QuakeRay uses
`POSITIVE_X`, `POSITIVE_Y`, `POSITIVE_Z` and `QR_SCENE_UNITS_PER_METER`.
Omitting this call selects the identity convention and preserves the legacy
Quake-compatible path. The call must happen before the first spatial upload or
drawn frame.

## 3. Upload the scene

Geometry, light, cluster and fog calls take host-space values. The SDK converts
them at the API boundary:

- upload geometry in local space with a row-major 3x4 `QrTransform`;
- upload cluster bounds, light positions/radii/reaches and fog distances in
  host units;
- use the per-type light color convention from the spatial contract;
- build the view as a column-major right-handed world-to-view matrix, with
  `-Z` forward and `+Y` up in view space;
- keep `QrDrawFrameInfo.cameraNear`, `cameraFar` and `rayLength` in host units;
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

For each frame:

```text
qrStartFrame
  -> upload dynamic geometry, particles and raster overlays for this frame
  -> fill QrDrawFrameInfo (column-major view, FOV, near/far/ray length,
     up vector, sky/fog/volumetric/material parameters)
  -> qrDrawFrame
```

The renderer owns the projection, acceleration structures, render targets and
presentation. The host owns window messages, input, simulation and asset
loading. Do not submit a projection matrix; `QrDrawFrameInfo.view` is
world-to-view only.

Use `qrGetFrameReadbackRequirements`, `qrRequestFrameReadback` and
`qrGetFrameReadbackStatus` for asynchronous final LDR capture. The readback
path is not a scene inspection API and includes the composited UI.

## 5. QuakeRay as the reference adapter

QuakeRay is the reference host. It keeps Quake units, Z-up and the existing
light calibration, declares the identity convention, and uses the current
shader/asset pipeline. Treat it as a regression oracle, not as the definition
of the public spatial contract.

The standalone editor is a parked workstream. It is not required for renderer
embedding.

## 6. Validation checklist

- Identity convention: QuakeRay's fixed camera, cluster lists and final image
  remain unchanged.
- Y-up/meter scene: compare against an independently authored Z-up/Quake-unit
  twin; check cluster lists, area lights, fog, water, particles, clouds and
  raster overlays.
- Swapchain/UI geometry is pixel-positioned and never world-transformed.
- Left-handed or arbitrarily rotated hosts convert their assets at import;
  they must not pass a mirrored scene as a right-handed one.
- Keep the physical draw distance inside the current 250 m ray/sky envelope
  until the envelope is made configurable.
