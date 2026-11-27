# Sun and god rays

The sun affects four separate products:

| Product | Host input |
|---|---|
| Visible sky disc | `QrDrawFrameSkyParams.sunDiscColor`, `sunDiscSize` |
| Scene illumination | `qrUploadDirectionalLight` with `QrDirectionalLightUploadInfo` |
| Indirect sunlight | `QrDrawFrameIlluminationParams.sunBounceRange`, `sunBounceScale` |
| God rays / shafts | `QrDrawFrameSkyParams.godRays*` |

Do not assume that changing the visible disc changes scene lighting or that
uploading a directional light creates a visible disc.

## Directional light

`QrDirectionalLightUploadInfo` contains:

- `uniqueID`: stable identity for the logical sun;
- `color`: irradiance/radiance color, whose magnitude is the intensity;
- `direction`: direction vector normalized by the renderer;
- `angularDiameterDegrees`: source angular diameter, non-negative.

Only one directional light can be registered in a frame. Upload it after
`qrStartFrame` and before `qrDrawFrame`. The pointer is consumed synchronously.

Nearly all values in QuakeRay are meters, but directional light has no position
or radius. Angles remain degrees in the public upload record.

## Visible sun

For `QR_SKY_TYPE_PROCEDURAL`, set:

```c
sky.sunDiscColor = sunDiscColor;
sky.sunDiscSize = 1.0f;
```

`sunDiscSize` is a multiplier on the renderer's baseline `0.025`-radian angular
radius. `sunDiscColor` belongs to the visible sky, while
`QrDirectionalLightUploadInfo.color` belongs to scene lighting.

The Quake host maps these from `rt_sky_sun_color`, `rt_sky_sun_disc` and
`rt_sky_sun_size`. When `rt_sky_time_enable` is off, `rt_physical_sun` controls
whether the host uploads the directional light; when `rt_sky_time_enable` is on,
the host computes solar/lunar ephemeris from 24-hour time (`rt_sky_time`),
automatically enables directional lighting (transmitted sunlight by day/sunset,
soft moonlight at night), and populates the `proceduralSun*`, `proceduralMoon*`,
`proceduralStar*`, and `atmosphere*` fields of `QrDrawFrameSkyParams`.

Quake reference defaults are `rt_physical_sky=1`, `rt_physical_sun=0`,
`rt_sky_sun=1`, disc scale `4`, disc size `1`, god rays enabled with intensity
`1` and quality `2`. These are host configuration defaults, not SDK defaults.

## God rays

God rays use these `QrDrawFrameSkyParams` fields:

| Field | Meaning |
|---|---|
| `godRaysEnabled` | Non-zero enables shafts when a sun source and scene bounds exist. |
| `godRaysIntensity` | Multiplier; values below zero are clamped to zero. |
| `godRaysQuality` | Actively wired per frame: clamped to `0..4` (defaults to `2` when `pSkyParams` is null) and mapped in `VulkanDevice.cpp` to `sunStepScale` (`{2.0, 1.5, 1.0, 0.75, 0.5}`) for `CmGodRays.comp.hlsl` (smaller scale = finer ray-march steps, higher quality). Note that `QrInstanceCreateInfo.godRaysQuality` is not consumed; set `pSkyParams->godRaysQuality` per frame. |
| `godRaysFromSkyTexture` | Use `RASTERIZED_GEOMETRY` sky's precomputed bright point instead of the directional light. |
| `godRaysSkyDirection`, `godRaysSkyColor` | Host-computed bright-point direction/color for raster sky shafts. |

The normal procedural path takes direction and color from the uploaded
directional light. If no directional light exists, shafts are disabled. The
raster-sky path can instead supply `godRaysFromSkyTexture` and the corresponding
direction/color. God rays also require scene AABB data; an empty scene cannot
create a shadow-map depth source.

Host inputs follow the SDK meter convention (40 Quake units per meter,
`0.025 m/unit`, matching `VulkanDevice.cpp` and `EditorSpatial`). Note that
`CmGodRays.comp.hlsl` still retains Q2RTX's internal `Q2_UNIT_IN_METERS = 0.0254`
constant for its internal march-step heuristic (`getStep`); hosts should still
convert all public API lengths at `0.025 m/unit` (40 u/m). Also note that for
`QrDrawFrameVolumetricParams`, only `volumetricFar` (meters) is active under
`coreQ2RTX = 1u` (bounding the froxel light-shaft frustum), while the remaining
legacy volumetric fog fields are bypassed in `CmPrepareFinal.comp.hlsl`.

Quake's `rt_sky_godrays`, `rt_sky_godrays_intensity` and
`rt_sky_godrays_quality` map to the first three fields. Its
`rt_sky_godrays_sky_threshold` is used inside the host to find the brightest
raster-sky point; there is no threshold field in the SDK.

## Minimal setup

```c
QrFloat3D sunColor = {{1.0f, 0.88f, 0.70f}};
QrFloat3D sunDirection = {{0.2f, 0.3f, -1.0f}};
QrFloat3D sunDiscColor = {{1.0f, 0.95f, 0.85f}};

QrDirectionalLightUploadInfo sun = {0};
sun.uniqueID = 1;
sun.color = sunColor;
sun.direction = sunDirection;
sun.angularDiameterDegrees = 0.05f;
qrUploadDirectionalLight(instance, &sun);

QrDrawFrameSkyParams sky = {0};
sky.skyType = QR_SKY_TYPE_PROCEDURAL;
sky.sunDiscColor = sunDiscColor;
sky.sunDiscSize = 1.0f;
sky.godRaysEnabled = QR_TRUE;
sky.godRaysIntensity = 1.0f;
sky.godRaysQuality = 2u;

QrDrawFrameIlluminationParams illumination = {0};
illumination.giBounceRays = 1.0f;
illumination.sunBounceRange = 50.0f; /* 2000 Quake units at 40 u/m (0.025 m/unit) */
illumination.sunBounceScale = 1.0f;

drawInfo.pSkyParams = &sky;
drawInfo.pIlluminationParams = &illumination;
```

## Validation

1. Toggle the visible disc without changing directional light.
2. Toggle directional light with a fixed visible disc.
3. Disable the directional light and confirm god rays do not fall back to a
   nonexistent sun.
4. Test procedural shafts and, if supported, raster-sky bright-point shafts.
5. Confirm map reloads and empty scenes do not reuse stale sun/shaft state.
