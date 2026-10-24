# Host feature matrix

This index connects a host-visible feature to the public SDK interface. Quake
cvar names are examples of one adapter's configuration; they are not exported
by the SDK.

| Feature | Public interface | Quake-host controls / notes |
|---|---|---|
| Initialization | `qrCreateInstance`, `qrSetSpatialConvention`, `qrStartFrame` | Host owns window, device lifetime and spatial convention. |
| Geometry | `qrUploadGeometry`, `qrUploadRasterizedGeometry`, `qrUploadParticles` | Current raster/dynamic uploads are frame-scoped. |
| Static scene | `qrBeginStaticGeometries`, `qrSubmitStaticGeometries` | Map lifetime until invalidation. |
| Sky | `QrDrawFrameSkyParams.skyType` | `rt_physical_sky`, `r_fastsky`; see [sky-and-atmosphere.md](sky-and-atmosphere.md). |
| Clouds | Procedural cloud block | `rt_sky_clouds*`; values are packed into `skyCubemapRotationTransform`. |
| Sun disc | `QrDrawFrameSkyParams.sunDisc*` | `rt_sky_sun_color`, `rt_sky_sun_disc`, `rt_sky_sun_size`; see [sun-and-god-rays.md](sun-and-god-rays.md). |
| Sun lighting | `qrUploadDirectionalLight` | `rt_physical_sun`, `rt_sky_sun`, pitch/yaw; one directional light max. |
| God rays | `QrDrawFrameSkyParams.godRays*` | `rt_sky_godrays*`; needs a directional light or raster-sky bright point. |
| Spherical/spot lights | `qrUploadSphericalLight`, `qrUploadSpotLight` | `RT_UploadAllDlights`, custom lights; per-frame upload. |
| Polygonal/textured area lights | `qrUploadPolygonalLight`, `qrUploadTexturedAreaLight(s)` | Geometry and material are host-supplied. |
| DTAL | `qrUploadDtalGroups` | Group/member arrays, per-frame; see [lights-and-sampling.md](lights-and-sampling.md). |
| Cluster light lists | `qrUploadWorldLights`, `qrUploadClusterLightSources`, sliced API | Topology and PVS belong to host; IDs are not meters. |
| NEE | `neeLightSamples` | `rt_nee_samples`, clamped to `1..2`. |
| ReSTIR | `restirEnabled`, `restirCandidates` | `rt_restir`, `rt_restir_candidates`; global-light RIS path. |
| Indirect GI | `giBounceRays`, `enableSecondBounceForIndirect` | `rt_gi_level`; `rt_indir2bounces` is ignored. |
| Denoiser | `denoiserEnabled` | `rt_denoiser`; no public history-reset function. |
| Sun in GI | `sunBounceRange`, `sunBounceScale` | `rt_sky_sun_bounce_range/scale`; range is meters. |
| Reflection/refraction | `QrDrawFrameReflectRefractParams` | `rt_reflrefr_depth`, IOR, water/acid, glass flags. |
| Fog | `QrDrawFrameLevelFogParams`, `qrSetFogVolumes` | Level fog in inverse meters; local volumes in meters. |
| Volumetric fog | `QrDrawFrameVolumetricParams` | `rt_volume_*`; distinct from clouds. |
| Raster order | Upload order inside emitted categories | Transparent ordering is host-owned; see [api.md](api.md#rasterized-scene-overlays-particles-decals-and-portals). |
| Screen effects | `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN`, post-effect blocks | Host builds UI/HUD geometry; painter order is preserved. |
| Particles/smoke | `qrUploadParticles`, raster geometry | `r_particles_points`; alpha, ordering and emitter lights are host choices. |
| Decals | `qrUploadDecal` | Frame-scoped upload; material owns alpha. |
| Resolution/upscaler | `QrDrawFrameRenderResolutionParams` | Query availability with `qrIsRenderUpscaleTechniqueAvailable`. |
| Tone mapping/bloom/post | Corresponding `QrDrawFrame*Params` blocks | All are per-frame pointers or embedded values. |
| Diagnostics | `qrGetFrameStats`, `qrGetClusterLightStats`, debug flags | Values are measurements; they are not quality/feature switches. |

## Lifecycle

```text
qrCreateInstance
  -> qrSetSpatialConvention
  -> persistent materials/cubemaps/static scene
  -> per frame: qrStartFrame
       dynamic geometry, raster data, lights, particles, decals
       fill QrDrawFrameInfo and pointer blocks
       qrDrawFrame
```

Static resources live until replacement or teardown. Dynamic geometry, raster
data, particles, decals, ordinary lights and frame parameter blocks are
frame-scoped. Materials, cubemaps and fog volumes are persistent handles.

## Contract boundaries

- Cvars are Quake configuration, not public renderer API.
- `rt_enable_pvs=0` is a locked Quake host policy; it is not an SDK capability.
- `r_tasks` is retired; the engine render frame is task-based.
- `maxBounceShadows` and `godRaysQuality` are currently present but not consumed
  by the active RHI paths.
- There is no public general-purpose temporal-history reset.
- The SDK is source-level; there is no promised binary plugin ABI.
- Binary host-specific formats and asset decoders stay in the adapter.

For complete function and lifetime rules, use [api.md](api.md),
[embedding.md](embedding.md), and [host-contract.md](host-contract.md).
