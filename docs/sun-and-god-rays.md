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
`rt_sky_sun_size`. `rt_physical_sun` controls whether the host uploads the
directional light; it is not an SDK field.

Quake reference defaults are `rt_physical_sky=1`, `rt_physical_sun=0`,
`rt_sky_sun=1`, disc scale `4`, disc size `1`, god rays enabled with intensity
`1` and quality `2`. These are host configuration defaults, not SDK defaults.

## God rays

God rays use these `QrDrawFrameSkyParams` fields:

| Field | Meaning |
|---|---|
| `godRaysEnabled` | Non-zero enables shafts when a sun source and scene bounds exist. |
| `godRaysIntensity` | Multiplier; values below zero are clamped to zero. |
| `godRaysQuality` | Present in the API, but the current RHI god-rays path does not consume it. |
| `godRaysFromSkyTexture` | Use `RASTERIZED_GEOMETRY` sky's precomputed bright point instead of the directional light. |
| `godRaysSkyDirection`, `godRaysSkyColor` | Host-computed bright-point direction/color for raster sky shafts. |

The normal procedural path takes direction and color from the uploaded
directional light. If no directional light exists, shafts are disabled. The
raster-sky path can instead supply `godRaysFromSkyTexture` and the corresponding
direction/color. God rays also require scene AABB data; an empty scene cannot
create a shadow-map depth source.

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

QrDrawFrameIlluminationParams illumination = {0};
illumination.giBounceRays = 1.0f;
illumination.sunBounceRange = 50.8f; /* 2000 Quake units */
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
