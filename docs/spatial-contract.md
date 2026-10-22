# Spatial contract

QuakeRaySDK accepts scene data in the host's own units and orientation. The
host declares that convention once, immediately after `qrCreateInstance`, with
`qrSetSpatialConvention`. The renderer normalizes every spatial input into its
existing internal scene space, which is right-handed, `+Z` up, and uses one
scene unit of 0.025 m (40 scene units per meter). A host does not supply
vertices in scene units.

The convention is immutable for the lifetime of the spatial scene. Calling
`qrSetSpatialConvention` after a spatial upload, a fog upload, or the first
drawn frame returns `QR_WRONG_FUNCTION_CALL`. Calling it twice before any
spatial upload replaces the convention.

## Declaration

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

The three axis fields describe the canonical scene axes in the host's
coordinate system. The example above is a Y-up, right-handed host with
`+X` right and `+Z` toward the viewer: canonical `+Y` is host `-Z`, and
canonical `+Z` (up) is host `+Y`.

The identity convention is:

```c
QrSpatialConvention identity = {
    .size = sizeof(QrSpatialConvention),
    .version = QR_SPATIAL_CONVENTION_VERSION,
    .sceneXAxis = QR_AXIS_POSITIVE_X,
    .sceneYAxis = QR_AXIS_POSITIVE_Y,
    .sceneZAxis = QR_AXIS_POSITIVE_Z,
    .hostIsRightHanded = QR_TRUE,
    .unitsPerMeter = QR_SCENE_UNITS_PER_METER,
};
```

QuakeRay uses the identity convention. If the setter is not called at all, the
identity convention is used and existing behavior is preserved.

`unitsPerMeter` is the number of host units in one meter. It must be finite and
greater than zero. The renderer scales lengths by
`QR_SCENE_UNITS_PER_METER / unitsPerMeter`, areas by the square of that scale,
and inverse lengths by its reciprocal. It does not rewrite vertex arrays;
geometry conversion is folded into the instance transform.

The supported conventions are right-handed, axis-aligned signed permutations
of the host axes with a uniform scale. A left-handed host must mirror and
re-wind its assets before upload, or it can pass an equivalent right-handed
frame. Arbitrary rotations, non-uniform scale and mirrored instance transforms
are not part of this contract; the host must bake them into its data.

## Camera and frame data

`QrDrawFrameInfo.view[16]` remains a column-major world-to-view matrix. Camera
space is right-handed, `+Y` up, `-Z` forward. The renderer builds the
projection from `fovYRadians`, the render-resolution aspect, `cameraNear` and
`cameraFar`. The host does not provide a projection matrix.

The host passes camera near/far and `rayLength` in its own units. The renderer
converts them to scene units. The current scene has a 10,000 scene-unit
(250 m) ray/sky envelope; hosts with a larger physical draw distance must keep
their scene inside that envelope until the envelope is made configurable.

`worldUpVector` is optional. A non-identity convention makes the internal up
`+Z`; the host's up vector may still be supplied for compatibility, but it
does not override the normalized scene frame.

The host passes `skyViewerPosition`, `godRaysSkyDirection` and the other
world-space sky fields in host space. The renderer converts them with the same
frame.

## Geometry

`QrVertex.position` and `QrVertex.normal` are local-space host data.
`QrTransform` is the existing row-major 3x4 column-vector affine transform.
The renderer converts the transform to scene space and leaves the vertex
arrays untouched. This applies to static, static-movable, dynamic, brush,
alias, sky, decal and rasterized geometry.

Rasterized geometry follows its render type:

- `DEFAULT` and `SKY` are world-space and receive the frame conversion,
  including a non-null `pViewProjection`.
- `SWAPCHAIN` is screen-space pixels and is never converted. HUD, menus, the
  console and other 2D geometry must use this type.

Particle points are world-space positions and world-space sizes. Their
positions are converted and sizes are scaled; `viewRight` and `viewUp` are
rotated as directions. Particle point clusters are topology identifiers and
are never changed.

## Lights

Light positions, radii, reaches, bounds and areas are host lengths/areas.
Directions are unit vectors and are rotated by the frame. Angles are not
converted.

Light color follows the existing per-type convention:

- directional: irradiance/radiance, not scaled;
- spherical, spot and polygonal: relative power, multiplied by the frame
  scale squared;
- textured area and DTAL: radiance, not multiplied; their `area` is scaled
  by the frame scale squared.

The Quake host applies its own `RT_QUAKE_LIGHT_AREA_INTENSITY_FIX` before
upload. Because the Quake convention is identity, the new conversion does not
change that value. A second host should provide relative light values and let
the SDK apply the unit factor; it must not apply the Quake 1600 factor itself.

Cluster light source origins, reaches and radii are converted. `power` is a
relative selection weight and is not converted. Cluster identifiers, PVS
bytes, visibility offsets, sky visibility bits and `clusterGeneration` are
topology and are not changed.

DTAL group centers, bounds, reaches, radii and areas are converted. DTAL
member basis vectors and origins are converted; member UVs and probability
fields are not.

## Atmosphere, fog and water

`cellWorldSize`, `sunBounceRange`, `volumetricFar`, fog volume corners and
`halfExtinctionDistance` are lengths and are converted.

Level fog density is per host unit and is divided by the frame scale. Fog
volumes keep their `softface` meaning after the axis mapping; the renderer
remaps `softface` to the corresponding canonical axis and sign.

Water and acid colors are transmittance over one host unit. The renderer
raises them to the reciprocal frame scale, matching the existing Quake
per-meter conversion. `waterWaveSpeed` and `waterTextureAreaScale` are lengths
and are scaled.

Procedural cloud altitude and thickness are carried in the existing
`skyCubemapRotationTransform` settings bag and are scaled. For a cubemap sky
the same field is a rotation and is composed with the frame rotation. Hosts
must keep these uses separate.

## Clusters and visibility

The host owns the cluster partition. Cluster indices, PVS rows and
`clusterGeneration` are independent of the spatial convention. Cluster bounds
are converted as AABBs, with the axis mapping applied per component. The
renderer's cluster constants (16-unit cell, 16-unit quantum, 32-unit margin)
remain calibrated to the internal scene and therefore become physically
stable across host units.

A non-BSP host still has to provide clusters and a visibility relation. The
no-PVS path and the `Q2_MAX_CLUSTERS` limit are separate contracts from
units and orientation.

## QuakeRay adapter

QuakeRay declares the identity convention:

```c
QrSpatialConvention spatial = {
    .size = sizeof(QrSpatialConvention),
    .version = QR_SPATIAL_CONVENTION_VERSION,
    .sceneXAxis = QR_AXIS_POSITIVE_X,
    .sceneYAxis = QR_AXIS_POSITIVE_Y,
    .sceneZAxis = QR_AXIS_POSITIVE_Z,
    .hostIsRightHanded = QR_TRUE,
    .unitsPerMeter = QR_SCENE_UNITS_PER_METER,
};
qrSetSpatialConvention(vulkan_globals.instance, &spatial);
```

All existing Quake data remains in Quake units and Z-up. The identity path is
the regression oracle: no conversion should change QuakeRay's CPU inputs or
rendered image.

## Integration checklist

1. Choose right-handed host axes; declare the canonical `X`, `Y`, `Z` axes
   and `unitsPerMeter`.
2. Build the camera view as column-major, right-handed, `-Z` forward, with
   camera near/far and `rayLength` in host units.
3. Upload geometry in host local space and transforms in the documented
   row-major 3x4 layout. Do not pre-scale vertices and transforms together.
4. Upload light positions/directions/radii/areas in host units and use the
   per-type color convention above.
5. Upload cluster bounds/reaches in host units and leave cluster IDs/PVS
   unchanged.
6. Keep UI/SWAPCHAIN geometry in pixels.
7. Supply cloud altitude/thickness and sky viewer fields in host units.
8. Validate the result with a Y-up/meter scene and its independently authored
   Z-up/Quake-unit twin; compare cluster lists and final readback.
