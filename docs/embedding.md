# Embedding QuakeRaySDK renderer

This guide is for a Win32 host that links `qray::renderer` and owns its window,
swapchain, assets and scene. The host may use a right-handed or left-handed
Z-up or Y-up frame, but it must convert all spatial values to meters before
calling the SDK. The renderer maps meter inputs into its private scene frame as
described in [spatial-contract.md](spatial-contract.md).

For the complete function-group catalogue and ownership rules, see
[api.md](api.md). This page is the step-by-step first integration.

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

QuakeRay is the reference host. Its game code keeps Quake units and Z-up, while
`Quake/qr_spatial_adapter.*` converts spatial API payloads to meters, including
the legacy positional-light calibration. `SWAPCHAIN` UI geometry remains in
pixels. Treat the adapter and its identity-frame output as a regression oracle,
not as the definition of the public spatial contract.

The standalone editor is a parked workstream. It is not required for renderer
embedding.

## 6. Validation checklist

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
