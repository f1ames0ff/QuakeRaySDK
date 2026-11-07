# Lights, DTAL and direct sampling

Hosts upload logical lights after `qrStartFrame` and before `qrDrawFrame`.
Light upload records are frame inputs; referenced arrays are consumed
synchronously. Re-upload every logical light that should exist in the current
frame.

## Light kinds

| Kind | Public record | Important fields |
|---|---|---|
| Directional | `QrDirectionalLightUploadInfo` | `uniqueID`, `color`, `direction`, `angularDiameterDegrees` |
| Spherical point | `QrSphericalLightUploadInfo` | `uniqueID`, `color`, `position`, `radius`, `normal` |
| Spot | `QrSpotLightUploadInfo` | `uniqueID`, `color`, `position`, `direction`, `radius`, inner/outer half-angles |
| Polygonal | `QrPolygonalLightUploadInfo` | `uniqueID`, `color`, three positions |
| Textured area | `QrTexturedAreaLightUploadInfo` | geometry, material, area, cone/projector fields |
| DTAL group | `QrDtalGroupUploadInfo` + members | bounds, reach, material, power, area and member range |

Positions, radii, reaches and bounds are meters. Areas are square meters.
Directions are unit vectors. Spot cone half-angles are radians. Light color
follows [spatial-contract.md](spatial-contract.md): directional is irradiance,
while spherical/spot/polygonal use relative emitted power and textured-area/DTAL
use radiance.

There can be one directional light per frame. The other light types may be
uploaded multiple times.

## Stable identity and lifetime

`uniqueID` identifies one logical light across moves, fixes, and frames. Keep it
stable while the light changes color, intensity, radius, or position. Use a new
ID only when a different light replaces it.

All ordinary light uploads are frame-scoped. A host with persistent game lights
still re-uploads them each frame. Deferred uploads only change when they are
committed:

```c
qrBeginDeferredLightUploads(instance, slot);
/* worker producers call the ordinary qrUpload...Light functions */
qrEndDeferredLightUploads(instance);
qrFlushDeferredLightUploads(instance);
```

## Quake lights and dlights

Quake's dynamic lights, map lights and emissive geometry are host-side
producers:

| Quake source | Public result |
|---|---|
| `RT_UploadAllDlights` | Per-frame spherical or spot lights derived from `cl_dlights` |
| `RT_UploadAllElights` | Authored map lights |
| `RT_AddEmissiveLight` / alias emissive lights | Emissive surface light registration |
| World-model lights | Polygonal/world-light uploads and cluster sources |

There is no `qrUploadDlight`; another host maps its transient point/cone light
to `qrUploadSphericalLight` or `qrUploadSpotLight`. Quake's
`rt_dlight_intensity` and `rt_dlight_radius` are host defaults, not SDK fields.

## DTAL

DTAL uploads two arrays in one call:

```c
QrDtalGroupUploadBatch batch = {0};
batch.groupCount = groupCount;
batch.pGroups = groups;
batch.memberCount = memberCount;
batch.pMembers = members;
qrUploadDtalGroups(instance, &batch);
```

Each group selects its member slice with `memberBase` and `memberCount`.
`QR_DTAL_MAX_UPLOAD_MEMBERS` is `131072`; `QR_CLUSTER_MAX_REGISTERED_LIGHTS` is
`4095`. The group's `center`, bounds, `reach`, `boundsRadius` and member
positions/areas are metric. Normals, UVs, probabilities and `estimatedPower`
are not converted.

The SDK computes DTAL and cluster LOD internally. Host code chooses the source,
identity and budget, not the internal distance group.

## Clusters and NEE

The simple cluster-light route is:

```text
qrUploadWorldLights
  -> qrUploadClusterLightSources
     or begin/slice/finish/publish/commit
  -> qrDrawFrame
```

Cluster IDs, PVS bytes, sky-visibility bits and IDs are topology, not spatial
units. Omitting both PVS arrays requests the SDK's all-visible fallback.
`rt_enable_pvs=0` is a locked Quake host policy, not a public SDK switch.

NEE evaluates explicit light samples in the direct pass:

- `QrDrawFrameIlluminationParams.neeLightSamples` is clamped to `1..2`.
- `restirEnabled` switches the direct pass to global-light RIS instead of the
  per-cluster list.
- `restirCandidates` is clamped to `1..64`.
- Invalid or lighting-less geometry should be rejected on the host; the SDK
  already drops zero/degenerate area lights.

Quake maps `rt_nee_samples`, `rt_restir` and `rt_restir_candidates` to those
fields. They are host controls, not renderer cvars.

## Validation

1. Move each light across cluster boundaries without changing `uniqueID`.
2. Verify spherical, spot, polygonal, textured-area and DTAL paths separately.
3. Toggle each light and confirm the changed contribution disappears.
4. Test the 4095 registered-light and 131072 DTAL-member limits.
5. Exercise no-PVS fallback and map teardown.
6. Compare cluster lists with `qrGetClusterLightStats` and grants.
