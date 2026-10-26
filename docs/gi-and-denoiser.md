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

The current indirect shader treats values below `0.75` as half resolution and
below `0.25` as disabled. Hosts should use the meaningful Quake levels:

- `0`: indirect disabled;
- `1`: single bounce;
- `2`: two bounces when `enableSecondBounceForIndirect` is set.

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
`rt_sky_sun_bounce_range=2000` Quake units (`50.8 m`). These are host defaults.

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
illumination.sunBounceRange = 50.8f; /* 2000 Quake units */
illumination.sunBounceScale = 1.0f;

drawInfo.pIlluminationParams = &illumination;
```

## Temporal history

There is no general public "reset history" function. The renderer currently
resets temporal state when returning from a UI-only frame to world rendering.
The FSR path also treats camera displacement greater than `2.5 m` as a cut.
Other discontinuities must be handled by the host's renderer lifecycle:

- recreate the renderer instance when guaranteed global history reset is needed;
- pass the new render resolution; the renderer owns dependent target recreation;
- keep camera positions/times consistent rather than injecting teleports as
  continuous motion;
- verify denoiser off/on transitions on the target scene; no separate API reset
  exists for that toggle.

## Validation

1. Compare GI `0`, `1`, and `2`.
2. Toggle the second bounce independently at level `2`.
3. Compare denoiser on/off and ReSTIR on/off using the same camera path.
4. Exercise `neeLightSamples` `1` and `2`.
5. Teleport, change render resolution, enter/leave UI-only mode and reload a map.
6. Check for stale history or ghosting after each transition.
