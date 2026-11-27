# Sky, clouds and atmosphere

`QrDrawFrameSkyParams` is the host-facing sky contract. Fill it each frame and
assign it to `QrDrawFrameInfo.pSkyParams`; the renderer does not read cvars or
retain the pointer after `qrDrawFrame`. The exact field layout is in
`qray/qray.h`.

The host owns configuration names and defaults. QuakeRay's `rt_physical_sky`,
`rt_sky_clouds_*` and `rt_sky_godrays*` cvars are only one adapter's mapping to
this structure.

See [host-feature-matrix.md](host-feature-matrix.md) for the wider feature map,
and [sun-and-god-rays.md](sun-and-god-rays.md) for sun/shaft setup.

## 1. Choose the sky type

| `QrSkyType` | Host supplies | Typical use |
|---|---|---|
| `QR_SKY_TYPE_COLOR` | `skyColorDefault`, multiplier | Fast/fallback sky. |
| `QR_SKY_TYPE_CUBEMAP` | `skyCubemap`, `skyCubemapRotationTransform` | Pre-authored environment cubemap. |
| `QR_SKY_TYPE_RASTERIZED_GEOMETRY` | `SKY` uploads from `qrUploadRasterizedGeometry` | Legacy textured sky geometry. |
| `QR_SKY_TYPE_PROCEDURAL` | Sky colors/sun fields and the cloud block below | Procedural sky with volumetric clouds. |

Quake's `rt_physical_sky 1` selects `QR_SKY_TYPE_PROCEDURAL`. `r_fastsky 1`
selects `COLOR`. Those precedence rules belong to the host.

## 2. Procedural sky and volumetric clouds

For procedural sky, `skyCubemapRotationTransform` is not a rotation. Its first
nine floats are the cloud block. Initialize the complete block.

```c
QrFloat3D skyColor = {{0.45f, 0.62f, 0.85f}};
QrFloat3D sunColor = {{1.00f, 0.88f, 0.70f}};
QrFloat3D cameraPositionMeters = {{0.0f, 0.0f, 2.0f}};

QrDrawFrameSkyParams sky = {0};
sky.skyType = QR_SKY_TYPE_PROCEDURAL;
sky.skyColorDefault = skyColor;
sky.skyColorMultiplier = 1.0f;
sky.sunDiscColor = sunColor;
sky.sunDiscSize = 1.0f;
sky.skyViewerPosition = cameraPositionMeters;
sky.skyAmbientLod = 0.0f;
sky.skyLightMultiplier = 1.0f;
sky.skyNee = QR_TRUE;
sky.skyCloudsQuality = 2u;

float *cloud = &sky.skyCubemapRotationTransform.matrix[0][0];
cloud[0] = 1.0f;       /* linear red */
cloud[1] = 1.0f;       /* linear green */
cloud[2] = 1.0f;       /* linear blue */
cloud[3] = 0.20f;      /* coverage */
cloud[4] = 0.80f;      /* density */
cloud[5] = 0.30f;      /* wind/motion rate */
cloud[6] = 1.0f;       /* draw enabled */
cloud[7] = 3500.0f;    /* base height, meters (140000 Quake units at 40 u/m) */
cloud[8] = 2250.0f;    /* thickness, meters (90000 Quake units at 40 u/m) */

/* For procedural sky this slot is cloud opacity. */
sky.skyColorSaturation = 1.0f;

drawInfo.pSkyParams = &sky; /* drawInfo is the frame's existing QrDrawFrameInfo */
```

The values above are the QuakeRay reference profile, not a mandatory SDK
preset. Quake stores the reference height and thickness as 140000 and 90000
Quake units; at the SDK/editor reference scale of 40 units/m (`0.025 m/unit`,
matching `VulkanDevice.cpp`'s `3500.0 m` / `2250.0 m` defaults and
`EditorSpatial`), those convert to `3500.0 m` and `2250.0 m`. Every other host
must send meters directly.

| Cloud input | Range / meaning |
|---|---|
| Color | Linear RGB. |
| Coverage | `0..1`; `0` removes coverage. |
| Density | Positive density scalar; QuakeRay UI exposes up to `10`. |
| Speed | Non-negative cloud-layer motion rate. |
| Enabled | `0` or `1`; the cloud block is ignored unless the sky is procedural. |
| Height | Base altitude in meters. |
| Thickness | Layer thickness in meters. |
| Opacity | Passed through `skyColorSaturation`, `0..1`. |
| Quality | `0` low, `1` medium, `2` high, `3` ultra; maximum is `QR_SKY_CLOUDS_MAX_QUALITY`. All levels use the volumetric cloud path. |

Clouds need a supplied sun/global light for their expected sun lighting and
god rays. Register the directional light through `qrUploadDirectionalLight`;
the sky's `sunDiscColor` and `sunDiscSize` control the visible disc rather than
the scene light itself.

## 3. Common sky fields

| Field | Meaning |
|---|---|
| `skyColorDefault`, `skyColorMultiplier` | Base atmosphere scattering color and final brightness. |
| `sunDiscColor`, `sunDiscSize` | Visible sun disc; size is relative to the SDK's baseline angular radius. |
| `skyViewerPosition` | Camera/eye position in meters; important for clouds and atmosphere. |
| `skyAmbientLod`, `skyLightMultiplier`, `skyNee` | Sky lighting and direct-sampling controls. |
| `godRaysEnabled`, `godRaysIntensity`, `godRaysQuality` | Optional sun shafts; `godRaysQuality` (`0..4`, clamped, default `2`) actively maps to `sunStepScale` (`{2.0, 1.5, 1.0, 0.75, 0.5}`). |
| `godRaysFromSkyTexture`, `godRaysSkyDirection`, `godRaysSkyColor` | Destination of a host-provided sky-texture sun estimate. |
| `proceduralAtmosphereConfigured` | Non-zero enables Rayleigh/Mie/ozone atmosphere, 3-tier starfield, phased moon, elevation-driven cloud sunset/night lighting and exact 3D cloud eclipse of solar/lunar discs. |
| `proceduralSunEnabled`, `proceduralSunDirection`, `proceduralSunTint`, `proceduralSunStrength` | Explicit procedural sun direction toward the sun, pure linear star spectrum tint, and cloud/sky illumination strength (independent of whether directional world light is uploaded). |
| `proceduralMoonDirection`, `proceduralMoonColor`, `proceduralMoonSize`, `proceduralMoonPhase` | Procedural moon direction toward the body, linear tint*intensity, size scale, and illumination phase (`0..1`, or `< 0` auto from sun-moon angle). |
| `proceduralStarsEnabled`, `proceduralStarDensity`, `proceduralStarIntensity`, `celestialRotationRadians` | Toggle, density (`0..4`), brightness (`0..5`), and diurnal celestial sphere rotation for the 3-tier night starfield. |
| `atmosphereDensity`, `atmosphereMie`, `atmosphereOzone` | Rayleigh/ozone thickness (`0..4`, `0` gives a dark void for space skies), aerosol haze/aureole (`0..4`), and Chappuis ozone crimson-blue sunset separation (`0..4`). |

`QrDrawFrameVolumetricParams` is separate from the procedural cloud layer.
Because `VulkanDevice.cpp` runs with `coreQ2RTX = 1u`, `applyVolumetrics()` in
`CmPrepareFinal.comp.hlsl` early-returns: only `volumetricFar` (meters, used by
`ResolveVolumetricCameraFar` to bound volumetric / froxel light shafts) is
active, while `enable`, `useSimpleDepthBased`, `ambientColor`, `scaterring`,
`sourceColor`, `sourceDirection`, and `sourceAssymetry` are currently bypassed.

## 4. Volumetric cloud occlusion in light shafts

When `QR_SKY_TYPE_PROCEDURAL` and volumetric clouds are active, `RhiCloudsPass` computes a 3D optical-depth shadow volume (`cloudShadowVolume`) bound to direct RT sunlight, indirect GI sunlight, `CmGodRays` shafts, and `CmLightShaftVolume` (`rt_light_shafts_sun`) froxel shafts so moving clouds cast consistent volumetric shadows into the world.

## 5. Minimal validation

1. Render procedural sky with clouds disabled.
2. Enable clouds and verify coverage, height and thickness change the image.
3. Move the camera and verify cloud perspective follows
   `skyViewerPosition`.
4. Exercise quality `0` and `3`.
5. Toggle the directional sun and god rays independently.
6. Check that no cloud block is supplied while `skyType` is not
   `PROCEDURAL`.
