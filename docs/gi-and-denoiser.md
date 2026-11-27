# Indirect lighting, ReSTIR and denoiser

Indirect lighting is configured by `QrDrawFrameIlluminationParams`; the
renderer records direct, indirect, gradient reprojection and denoising itself.
There are no separate "enable GI" or "run denoiser" calls.

## Frame fields

| Field | Meaning | Default / clamp |
|---|---|---|
| `giBounceRays` | Indirect ray count/level; `0` disables indirect | Clamped `0..2` |
| `enableSecondBounceForIndirect` | Enables the second indirect bounce | Boolean |
| `maxBounceShadows` | Public legacy field; current passes do not read it | No effect |
| `neeLightSamples` | Direct NEE samples | Clamped `1..2` |
| `restirEnabled` | Use global-light RIS instead of per-cluster lists | `0` |
| `restirCandidates` | Candidates per RIS sample | Clamped `1..64`, default `8` |
| `denoiserEnabled` | Enable ASVGF/denoiser path | Renderer fallback `1` |
| `cellWorldSize` | ReSTIR/grid cell size in meters | Minimum `2.5e-5` |
| `directDiffuseSensitivityToChange`, `indirectDiffuseSensitivityToChange`, `specularSensitivityToChange` | Temporal change sensitivity | Clamped `0..1` |
| `q2DepthGradMode` | Depth-gradient mode | Renderer fallback `1` |
| `q2LightStatsMode` | Light-statistics diagnostic mode | Renderer fallback `1` |
| `reflRefrEarlyOut` | Reflection/refraction early-out | Boolean, fallback `1` |
| `sunBounceRange` | Sun reach into one indirect bounce, meters | `0` disables indirect sunlight |
| `sunBounceScale` | Multiplier on indirect sunlight | Non-negative |
| `fixedAlbedo` | Diagnostic fixed albedo override; `0` is normal | Non-negative |
| `lightUniqueIdIgnoreFirstPersonViewerShadows` | Optional pointer to one light ID | May be null |

The current indirect shader treats `giBounceRays` in `[0.25, 0.75)` as half
resolution and values `< 0.25` as disabled. Hosts should use the meaningful Quake levels:

- `0`: indirect disabled;
- `0.5`: half-resolution indirect rays (alternating even/odd scanlines with
  separate `UnfilteredIndirectSpecular` storage, `1x` physical radiance for
  indirect diffuse/sky, parity-locked ASVGF gradient tracing with saved
  current-frame `SurfacePosition`/`ViewDirection`, and bilateral vertical row
  reconstruction for both `UnfilteredIndirectSpecular` and
  `UnfilteredIndirectSH` in `CmQ2Adapter` and unfiltered debug views);
- `1`: single bounce;
- `2`: two bounces when `enableSecondBounceForIndirect` is set.

## Specular denoiser and debug probes

The ASVGF specular branch reprojects reflected radiance via the virtual reflected
world point (`framebufViewDirection.w` blended smoothly back to surface motion
for rough, curved, or moving reflectors), tracks dedicated `rg32f` temporal
moments in `Q2HistMomentsSpec`, and filters with independent specular variance
and `hist_len_spec` edge-stopping in `CmQ2Atrous`.

Two public diagnostic bits in `QrDebugDrawFlagBits` (`debugShowFlags`) aid
reflection tuning and are evaluated post-tonemap and post-fog in `CmPrepareFinal`
(returning `0.0` when `denoiserEnabled == 0`):

- `QR_DEBUG_DRAW_SPEC_HISTORY_LEN_BIT` (`1u << 20`): visualizes
  `saturate(hist_len_spec / 64.0)` (specular history length);
- `QR_DEBUG_DRAW_SPEC_GRADIENT_BIT` (`1u << 21`): visualizes
  `saturate(grad_hf_spec.y)` (specular temporal gradient).

## Quake mapping

| Quake control | Public destination |
|---|---|
| `rt_gi_level` | `giBounceRays`; `>=1.5` also sets `enableSecondBounceForIndirect` |
| `rt_nee_samples` | `neeLightSamples`, rounded to 1 or 2 |
| `rt_restir` | `restirEnabled` |
| `rt_restir_candidates` | `restirCandidates` |
| `rt_denoiser` | `denoiserEnabled` |
| `rt_sky_sun_bounce_range` | `sunBounceRange`, converted from Quake units to meters |
| `rt_sky_sun_bounce_scale` | `sunBounceScale` |
| `rt_sensit_dir`, `rt_sensit_indir`, `rt_sensit_spec` | Three sensitivity fields |
| `rt_q2_depthgrad`, `rt_q2_lightstats` | Diagnostic mode fields |
| `rt_reflrefr_earlyout` | `reflRefrEarlyOut` |

`rt_indir2bounces` is ignored regardless of value; the bounce count now comes
from `rt_gi_level`. `rt_shadowrays` writes `maxBounceShadows`, but no current
pass reads that field.

Quake reference defaults are `rt_gi_level=1`, `rt_nee_samples=2`,
`rt_restir=0`, `rt_restir_candidates=8`, `rt_denoiser=1` and
`rt_sky_sun_bounce_range=2000` Quake units (`50.0 m` at the SDK/editor
convention of 40 units/m = `0.025 m/unit`, matching `VulkanDevice.cpp`'s
fallback `sunBounceRange = 50.0f`). These are host defaults.

## Minimal frame setup

```c
QrDrawFrameIlluminationParams illumination = {0};
illumination.cellWorldSize = 2.0f; /* meters */
illumination.giBounceRays = 1.0f;
illumination.enableSecondBounceForIndirect = QR_FALSE;
illumination.neeLightSamples = 2u;
illumination.restirEnabled = QR_FALSE;
illumination.restirCandidates = 8u;
illumination.denoiserEnabled = QR_TRUE;
illumination.sunBounceRange = 50.0f; /* 2000 Quake units at 40 u/m (0.025 m/unit) */
illumination.sunBounceScale = 1.0f;

drawInfo.pIlluminationParams = &illumination;
```

## Temporal history

There is no general public "reset history" function. The renderer automatically
triggers a one-frame temporal history reset (`restirParams[2] = 1`) when
returning from a UI-only frame to world rendering, when render resolution /
framebuffer targets are recreated (`PrepareForSize`), and when `denoiserEnabled`
transitions from `0` to `1`. The FSR path also treats camera displacement
greater than `2.5 m` as a cut. Other discontinuities (such as in-world camera
teleports without a UI or resolution transition) must be handled by the host:

- recreate the renderer instance when a guaranteed full reset is needed;
- pass the new render resolution; the renderer owns dependent target recreation
  and resets temporal history alongside it;
- keep camera positions/times consistent rather than injecting teleports as
  continuous motion.

## Denoiser & Color-Fidelity Shader Invariants

When customizing host lighting parameters or modifying denoiser / tonemap shaders, preserve these physical and numerical invariants (detailed in [agent-integration-guide.md Pitfalls 31–32](agent-integration-guide.md#pitfall-31-pale-pink-red-lights-washed-out-indirect-gi-chroma--over-driven-screen-emission)):

1. **Indirect YCoCg SH chroma scaling (`CmQ2Adapter` / `Q2Asvgf`):** `q2IrradianceToSH` stores `L00` SH coefficients (carrying the `0.282095` SH basis factor), while `q2SHToIrradiance` expects radiance-scaled chroma before applying `0.282095`. `CmQ2Adapter.comp.hlsl` applies `Q2_LF_CHROMA_TO_RADIANCE = 1.0 / 0.282095` when writing `framebufQ2ColorLF_COCG` so colored bounces do not desaturate ~3.54× (e.g., red lights turning pale pink) and `LIGHT_CHROMA_BOOST` stays at `1.0` (`RaygenCommon.hlsli`).
2. **Direct vs. indirect spherical solid-angle clamping & emission range:** Keep `Q2_DIRECT_MAX_SPHERE_SOLID_ANGLE = 2.0 * M_PI` (`RtRaygenDirect.rgen.hlsl`) and isolate the `0.02` firefly cap to indirect bounces (`Q2_INDIR_MAX_SPHERE_SOLID_ANGLE`, `RtQ2Indirect.rgen.hlsl`). Keep `pTexturesParams->emissionMaxScreenColor` physically bounded (`4.0f` editor/host default, `1.5f` SDK fallback; evaluated in `CmPrepareHdr.comp.hlsl`) and `pTonemappingParams->chromaPreservation` positive (`0.8f` default in `Source/Tonemapping.cpp`, evaluated in `CmPrepareFinal.comp.hlsl`).
3. **Pixel-normalized view-space depth gradients (`fwidth_depth`):** Keep `q2DepthGradMode = 1u` so `HitInfo.hlsli` + `RaygenPrimary.hlsli` emit `depthGrad = 1.0 / max(Q2_DEPTH_GRAD_MIN_STEP, gradDepth.z)` (`Q2_DEPTH_GRAD_MIN_STEP = 0.0025` meters in `Q2Asvgf.hlsli`, equivalent to `0.1` Quake units at `40 u/m`). Keeping `dist_z` in pixels prevents distance/grazing edge breakdown around corners and viewmodels.
4. **LF deflicker bounds-before-load & temporal anti-ghosting:** `CmQ2AtrousLF.comp.hlsl` (`deflicker_image`) requires `Q2_FLT_ATROUS_DEFLICKER_LF >= 1.0` (`2.0`), `[field_left, field_right)` / `[0, height)` bounds checks **before** reading neighbors (avoiding out-of-bounds black/`NaN` poisoning), relative-depth (`< 0.1`) + geometric-normal (`dot > 0.5`) neighbor validation against foreground weapon silhouettes, and trimming the brightest neighbor (`num_neighbors >= 3`). `CmQ2Temporal.comp.hlsl` pairs this with gradient-driven LF/HF motion antilag (`Q2_FLT_ANTILAG_LF = 0.2`, `Q2_FLT_ANTILAG_HF = 1.0`), grazing tangent-plane history validation (`diff_plane_dist < 0.025` m), and a 5×5 spatial moment fallback (`hist_len_hf < 4.0`) for disoccluded screen edges and uncovered pixels behind the weapon.

## Validation

1. Compare GI `0`, `1`, and `2`.
2. Toggle the second bounce independently at level `2`.
3. Compare denoiser on/off and ReSTIR on/off using the same camera path.
4. Exercise `neeLightSamples` `1` and `2`.
5. Teleport, change render resolution, enter/leave UI-only mode and reload a map.
6. Check for stale history or ghosting after each transition.
