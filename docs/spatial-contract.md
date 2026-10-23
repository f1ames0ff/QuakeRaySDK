# Spatial contract

QuakeRaySDK accepts scene data in the host's own units and orientation. The
host declares that convention once, immediately after `qrCreateInstance`, with
`qrSetSpatialConvention`. The renderer normalizes the spatial inputs described
below into its existing internal scene space, which is right-handed, `+Z` up, and uses one
scene unit of 0.025 m (40 scene units per meter). A host does not supply
vertices in scene units.

The convention is immutable for the lifetime of the spatial scene. Calling
`qrSetSpatialConvention` after a spatial upload, a fog upload, or the first
drawn frame returns `QR_WRONG_FUNCTION_CALL`. Calling it twice before any
spatial upload replaces the convention. Call the setter on the thread that
created the instance and before worker uploads or frames begin; the convention
is not synchronized for concurrent replacement.

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
and inverse lengths by its reciprocal. Ordinary geometry conversion is folded
into the instance transform. World-light face vertices and smoke raster
vertices are copied and rewritten because they carry spatial values without an
instance transform for those values.

The supported conventions are right-handed or left-handed, axis-aligned signed
permutations of the host axes with a uniform scale. A left-handed host keeps
its native coordinates; the renderer mirrors the frame into the canonical
right-handed scene and reverses triangle winding for indexed geometry, polygon
lights and world-light faces. The host must supply the view matrix in the same
handedness as its world (its native world-to-view matrix); the renderer
converts it to the canonical camera space.

Only the convention itself is restricted to an axis permutation: the world's
own axes have to be a signed permutation of the scene axes, so a world whose
"up" is an arbitrary rotated vector cannot be expressed and has to be rotated
into one before it is handed over. Instance transforms are not restricted.
`SpatialFrame::Transform` evaluates the linear map `scale * hostToScene * M`,
which is exact for any `M`, so arbitrary instance rotations and non-uniform
instance scales are converted without loss and the host does not have to bake
them. Likewise `ReversesWinding` compares `det(M3x3)` against the frame
determinant, so an instance whose own determinant is negative — mirrored
geometry, or a non-uniform scale with an odd number of negative factors — is
handled by the same rule regardless of how the frame is oriented. What the
host does own is the orientation of its authored data: reflecting positions
without reversing the triangle order changes what the triangles face, and the
renderer does not undo that, it only cancels the flip of its own scene
transform.

The winding reversal keeps the host's authored winding authoritative: the
renderer cancels the orientation flip that the composed scene transform
introduces, so a triangle the host marks as front-facing is front-facing in the
scene as well, and the rule applies to indexed and non-indexed triangle lists
alike. The host therefore still owns its own front faces, exactly as its
rasterizer sees them, including the reflected-copy case described above.
`qray_spatial_twin_gpu` covers both halves: it renders the same scene through a
right-handed Y-up frame and through a mirrored frame whose triangle order was
reversed at export, and requires both images to match.

Because the flip is baked into the uploaded index buffer, `qrUpdateGeometryTransform`
cannot change it afterwards: it raises `QR_CANT_UPDATE_TRANSFORM` when the new
transform would flip winding relative to the upload. A host that needs to change
that sign re-uploads the geometry.

## Camera and frame data

`QrDrawFrameInfo.view[16]` remains a column-major world-to-view matrix. Camera
space is right-handed, `+Y` up, `-Z` forward. The renderer builds the
projection from `fovYRadians`, the render-resolution aspect, `cameraNear` and
`cameraFar`. The host does not provide a projection matrix.

The host passes camera near/far and `rayLength` in its own units. The renderer
converts them to scene units. `rayLength` is host-controlled up to the safety
ceiling of 10,000,000 scene units (250 km); the shader sky sentinel follows
`2 * rayLength`. The world-depth buffer is `R32_SFLOAT`, so the ray envelope
does not rely on a 16-bit depth range. A host still has to keep its scene
inside the declared ray length and provide a finite positive value.

`worldUpVector` is optional. A non-identity convention makes the internal up
`+Z`; the host's up vector may still be supplied for compatibility, but it
does not override the normalized scene frame.

The host passes `skyViewerPosition`, `godRaysSkyDirection` and the other
world-space sky fields in host space. The renderer converts them with the same
frame.

## Geometry

`QrVertex.position` and `QrVertex.normal` are local-space host data for
ordinary geometry. Smoke raster batches use `normal[1]` as a world-space puff
radius, as described below. `QrTransform` is the existing row-major 3x4
column-vector affine transform.
The renderer converts the transform to scene space for static, static-movable,
dynamic, brush, alias, sky, decal and rasterized geometry. World-light face
vertices and smoke raster vertices are handled separately as described below.

Rasterized geometry follows its render type:

- `DEFAULT` is world-space and receives the frame conversion, including a
  non-null `pViewProjection`. `SKY` receives the transform conversion but its
  `pViewProjection` and viewport must remain null.
- `SWAPCHAIN` is screen-space pixels and is never converted. HUD, menus, the
  console and other 2D geometry must use this type.

Particle points are world-space positions and world-space sizes. Their
positions are converted and sizes are scaled; `viewRight` and `viewUp` are
rotated as directions. Particle point clusters are topology identifiers and
are never changed.

Smoke raster batches additionally carry world-space puff radii in the vertex
normal channel and world-space noise scales in `smokeNoise`. The renderer
scales those fields when the convention is non-identity; `smokeLook` remains
dimensionless. The smoke wind direction is host-supplied through
`qrSetSmokeWindDirection`; it is passed in host scene space, rotated by the
frame, and normalized internally. If the setter is never called, the existing
Quake scene-space wind is used, so the identity host is unchanged.

## Lights

Light positions, radii, reaches, bounds and areas are host lengths/areas.
Directions are unit vectors and are rotated by the frame. Angles are not
converted.

Light color follows the existing per-type convention:

- directional: irradiance/radiance, not scaled;
- spherical, spot and polygonal: relative power, multiplied by the frame
  scale squared;
- textured area and DTAL: radiance, not multiplied; their `area` is scaled
  by the frame scale squared. A second host must pass radiance for these
  types, not a Quake-style area-fixed power.

The Quake host applies its own `RT_QUAKE_LIGHT_AREA_INTENSITY_FIX` before
upload. Because the Quake convention is identity, the new conversion does not
change that value. A second host should provide relative light values and let
the SDK apply the unit factor; it must not apply the Quake 1600 factor itself.

Cluster light source origins, reaches and radii are converted, as is
`topUpReach` even when the dynamic source array is empty. `power` is a relative
selection weight and is not converted. Cluster identifiers, PVS
bytes, visibility offsets, sky visibility bits and `clusterGeneration` are
topology and are not changed.

DTAL group centers, bounds, reaches, radii and areas are converted. DTAL
member basis vectors and origins are converted; member UVs and probability
fields are not.

### Light coverage in the GPU twin

`qray_spatial_twin_gpu` renders without a top-level acceleration structure, so
its path-traced passes are skipped and its reference frame is lit by the
procedural sky and the directional light alone. Removing the spherical or the
polygonal light from that frame leaves the image bit-identical, which was
verified by probing both. Those two types are therefore executed through the
conversion and upload path — enough to catch a bad transform, a rejected
payload or a crash — but their shading is not part of the equivalence gate.
The polygonal light is the one type whose emitted side comes from the vertex
order, so a mirrored frame reorders it in `SpatialPolygonalLight`. That rule is
covered only by that single code path; no test asserts it directly, because the
twin frame cannot make the light visible.

## Atmosphere, fog and water

`cellWorldSize`, `sunBounceRange`, `volumetricFar`, fog volume corners and
`halfExtinctionDistance` are lengths and are converted.

Level fog density is per host unit and is divided by the frame scale. Fog
volume `pointA` and `pointB` keep their order; `softface` parity continues to
name the `pointA`/`pointB` side while its axis is remapped to canonical space.
The `acidDensity` coefficient is an artistic dimensionless density multiplier;
it is not converted. Hosts that need physical unit invariance must author it
together with the acid color.

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

A non-BSP host still has to provide clusters, but it may omit both the PVS
offset table and PVS bytes. In that case the renderer synthesizes all-visible
PVS rows, so every cluster receives the lights its light sources reach and the
existing top-up pass remains available. A host with a real visibility relation
may supply the tables as before. The `Q2_MAX_CLUSTERS` limit is separate from
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

1. Choose the host handedness and declare the canonical `X`, `Y`, `Z` axes
   and `unitsPerMeter`.
2. Build the camera view as column-major in the host world's handedness with
   `-Z` forward, and pass camera near/far and `rayLength` in host units.
3. Upload geometry in host local space and transforms in the documented
   row-major 3x4 layout. Do not pre-scale vertices and transforms together.
4. Upload light positions/directions/radii/areas in host units and use the
   per-type color convention above.
5. Upload cluster bounds/reaches in host units and leave cluster IDs/PVS
   unchanged. Omit both PVS tables for an all-visible non-BSP scene.
6. Pass the host smoke wind direction with `qrSetSmokeWindDirection` in host
   scene space.
7. Keep UI/SWAPCHAIN geometry in pixels.
8. Supply cloud altitude/thickness and sky viewer fields in host units.
9. Validate the result with a Y-up/meter scene and its independently authored
   Z-up/Quake-unit twin; compare cluster lists and final readback.
