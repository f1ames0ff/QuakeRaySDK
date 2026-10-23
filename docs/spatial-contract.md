# Spatial contract

QuakeRaySDK accepts all spatial lengths and positions in meters, areas in
square meters, and inverse lengths per meter. The host declares only its axis
orientation and handedness once, immediately after `qrCreateInstance`, with
`qrSetSpatialConvention`; there is no host-unit scale in the API. Each host
converts its native units to meters before calling the SDK. The renderer then
maps the data into its private right-handed, `+Z`-up scene space at a 1:1
scale. Internal spatial quantities are meters as well; hosts must not pass
values in another unit.

The convention is immutable for the lifetime of the spatial scene. Calling
`qrSetSpatialConvention` after a spatial upload, a fog upload, or the first
drawn frame returns `QR_WRONG_FUNCTION_CALL`. Calling it twice before any
spatial upload replaces the convention. Call the setter on the thread that
created the instance and before worker uploads or frames begin; the convention
is not synchronized for concurrent replacement. A call rejected before it
uploads data does not lock the convention.

## Declaration

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
    .reserved0 = 0,
};
```

`reserved0` and all entries of `reserved` must be zero. QuakeRay uses the
identity orientation, but its host adapter converts Quake units to meters
before each spatial API call. If the setter is not called, the SDK assumes
identity axes and meter-valued inputs; it does not accept legacy Quake-unit
values.

The supported conventions are right-handed or left-handed, axis-aligned signed
permutations of the host axes. Unit scale is fixed by the meter contract and is
not configurable. A left-handed host keeps its native coordinates; the
renderer mirrors the frame into the canonical right-handed scene and reverses
triangle winding for indexed geometry, polygon lights and world-light faces.
The host supplies the view matrix in the same handedness as its world (its
native world-to-view matrix); the renderer converts it to canonical camera
space.

Only the convention itself is restricted to an axis permutation: the world's
own axes have to be a signed permutation of the scene axes, so a world whose
"up" is an arbitrary rotated vector cannot be expressed and has to be rotated
into one before it is handed over. Instance transforms are not restricted.
`SpatialFrame::Transform` evaluates the linear map `scale * hostToScene * M`,
which is exact for any finite, non-singular `M`, so arbitrary instance rotations
and non-uniform instance scales are converted without loss and the host does
not have to bake them. Normals use the inverse-transpose of `M3x3`; tangents use the linear map,
are re-orthonormalized against the transformed normal and flip their
handedness with a negative model determinant. Likewise `ReversesWinding`
compares `det(M3x3)` against the frame
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

`QrDrawFrameInfo.view[16]` remains a column-major world-to-view matrix whose
translation is expressed in meters. Camera space is right-handed, `+Y` up,
`-Z` forward. The renderer builds the projection from `fovYRadians`, the
render-resolution aspect, `cameraNear` and `cameraFar`. The host does not
provide a projection matrix.

`QrMatrix3D` is row-major. For a cubemap sky,
`skyCubemapRotationTransform` maps host-world directions to cubemap lookup
directions; the renderer composes it as `R_host * H^T`, where `H` maps
host-world directions into the canonical scene frame.

The host passes camera near/far and `rayLength` in meters. The renderer keeps
them in meters. `rayLength` is host-controlled up to the safety ceiling of
250,000 m; the shader sky sentinel follows `2 * rayLength`. The world-depth
buffer is `R32_SFLOAT`, so the ray envelope
does not rely on a 16-bit depth range. A host still has to keep its scene
inside the declared ray length and provide a finite positive value.

`worldUpVector` is optional. A non-identity convention makes the internal up
`+Z`; the host's up vector may still be supplied for compatibility, but it
does not override the normalized scene frame.

The host passes `skyViewerPosition` and procedural cloud altitude/thickness in
meters. Directions and rotations remain dimensionless and are converted only
for the axis convention.

## Geometry

`QrVertex.position` is local-space data in meters; `QrVertex.normal` is a
dimensionless direction. Smoke raster batches use `normal[1]` as a puff radius
in meters, as described below. `QrTransform` is the existing row-major 3x4
column-vector affine transform: its linear part is dimensionless and its
translation is in meters. The renderer maps it into the scene orientation for static,
static-movable, dynamic, brush, alias, sky, decal and rasterized geometry.
World-light face vertices and smoke raster vertices are handled separately as
described below.

Rasterized geometry follows its render type:

- `DEFAULT` is world-space in meters and receives the frame conversion,
  including a non-null `pViewProjection` built for meter-valued positions.
  `SKY` receives the transform conversion but its
  `pViewProjection` and viewport must remain null.
- `SWAPCHAIN` is screen-space pixels and is never converted. HUD, menus, the
  console and other 2D geometry must use this type.

`QR_RASTERIZED_GEOMETRY_STATE_FORCE_LINE_LIST` is exempt from triangle winding
reversal: line segments have no front face, so their uploaded pair order is
preserved even when the frame or transform mirrors the scene.

Particle points are world-space positions and sizes in meters. Their
positions are converted and sizes are scaled; `viewRight` and `viewUp` are
rotated as directions. Particle point clusters are topology identifiers and
are never changed.

Smoke raster batches additionally carry puff radii in meters in the vertex
normal channel and spatial noise scales in `smokeNoise`. The renderer consumes
those fields as meters; `smokeLook` remains dimensionless. The
smoke wind direction is host-supplied through
`qrSetSmokeWindDirection`; it is passed in host scene space, rotated by the
frame, and normalized internally. If the setter is never called, the existing
Quake scene-space wind is used, so the identity host is unchanged.

## Lights

Light positions, radii, reaches and bounds are in meters; areas are in square
meters. Directions are unit vectors and are rotated by the frame. Angles are
not converted.

Light color follows the existing per-type convention:

- directional: irradiance/radiance, not scaled;
- spherical, spot and polygonal: relative power; the SDK encodes the emitting
  area on a meter basis, so the host must not pre-divide it;
- textured area and DTAL: radiance, unchanged; their `area` is square meters.
  A second host must pass radiance for these types, not a Quake-style
  area-fixed power.

The Quake host adapter removes its legacy `RT_QUAKE_LIGHT_AREA_INTENSITY_FIX`
from spherical, spot and polygonal light colors before upload. That factor
belongs to Quake's unit-area calibration, not to the SDK contract. Other
hosts pass relative power directly and must not apply the Quake 1600 factor.

Cluster light source origins, reaches and radii are converted, as is
`topUpReach` even when the dynamic source array is empty. `power` is a relative
selection weight and is not converted. Cluster identifiers, PVS
bytes, visibility offsets, sky visibility bits and `clusterGeneration` are
topology and are not changed.

DTAL group centers, bounds, reaches, radii and areas are spatial quantities in
meters or square meters. DTAL member positions and areas use the same units;
member normals, UVs, probability fields and `estimatedPower` are not converted.

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
`halfExtinctionDistance` are meter-valued lengths.

Level fog density and volumetric scattering are per meter; the SDK keeps them
on that meter basis. Fog volume `pointA` and `pointB` keep their
order; `softface` parity continues to name the `pointA`/`pointB` side while its
axis is remapped to canonical space. The `acidDensity` coefficient is an
artistic dimensionless density multiplier; it is not converted. Hosts that
need physical unit invariance must author it together with the acid color.

Water and acid colors are transmittance over one meter. The 1:1 scene scale
leaves them unchanged apart from the lower clamp. `waterWaveSpeed` is meters per
second and `waterTextureAreaScale` is a meter-valued length.

Procedural cloud altitude and thickness are meters, carried in the existing
`skyCubemapRotationTransform` settings bag. For a cubemap sky the same field is
a rotation and is composed with the frame rotation. Hosts must keep these
uses separate.

## Clusters and visibility

The host owns the cluster partition. Cluster indices, PVS rows and
`clusterGeneration` are independent of the spatial convention. Cluster bounds
are converted as AABBs, with the axis mapping applied per component. The
renderer’s cluster constants (0.4 m per grid cell and source quantum; 0.8 m
margin) remain fixed and therefore
represent the same physical dimensions for every host.

A non-BSP host still has to provide clusters, but it may omit both the PVS
offset table and PVS bytes. In that case the renderer synthesizes all-visible
PVS rows, so every cluster receives the lights its light sources reach and the
existing top-up pass remains available. A host with a real visibility relation
may supply the tables as before. The `Q2_MAX_CLUSTERS` limit is separate from
units and orientation.

## QuakeRay adapter

QuakeRay declares the identity orientation:

```c
QrSpatialConvention spatial = {
    .size = sizeof(QrSpatialConvention),
    .version = QR_SPATIAL_CONVENTION_VERSION,
    .sceneXAxis = QR_AXIS_POSITIVE_X,
    .sceneYAxis = QR_AXIS_POSITIVE_Y,
    .sceneZAxis = QR_AXIS_POSITIVE_Z,
    .hostIsRightHanded = QR_TRUE,
    .reserved0 = 0,
};
qrSetSpatialConvention(vulkan_globals.instance, &spatial);
```

The game keeps its native Quake-unit data and Z-up orientation. The host-side
`Quake/qr_spatial_adapter.*` layer converts spatial API payloads to meters
before calling the SDK, while leaving `SWAPCHAIN` pixel uploads untouched. Its
thread-local scratch buffers are valid through the SDK calls, which copy upload
payloads synchronously. This adapter preserves the existing game scene; the
public API itself never accepts Quake units.

## Integration checklist

1. Convert the host’s spatial lengths and positions to meters and areas to
   square meters before calling the SDK; express inverse lengths per meter.
2. Choose the handedness and canonical `X`, `Y`, `Z` axes, then declare them
   with all reserved fields zero.
3. Build the camera view as column-major in the host world's handedness with
   `-Z` forward; pass its translation, camera near/far and `rayLength` in
   meter-based coordinates.
4. Upload geometry in meter-valued local space and transforms in the
   documented row-major 3x4 layout. Keep transform linear terms dimensionless.
5. Upload light positions/directions/radii/areas using the units and per-type
   color convention above.
6. Upload cluster bounds/reaches in meters and leave cluster IDs/PVS unchanged.
   Omit both PVS tables for an all-visible non-BSP scene.
7. Pass the smoke wind direction with `qrSetSmokeWindDirection` in host scene
   space.
8. Keep UI/SWAPCHAIN geometry in pixels.
9. Supply cloud altitude/thickness and sky viewer position in meters. Validate
   identity, Y-up and mirrored meter scenes against each other.
