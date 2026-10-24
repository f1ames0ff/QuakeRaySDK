# Host compatibility contract

This document defines what a source-level host must provide to QuakeRaySDK and
what the SDK deliberately leaves to the host. It also maps renderer features
common in games released roughly through 2005 onto the current API. The
families below are integration profiles, not a promised list of supported
games. Every game still needs its own loader and adapter.

The normative types and functions are in the installed `qray/qray.h`.
[embedding.md](embedding.md) describes the call sequence,
[spatial-contract.md](spatial-contract.md) defines units and axes, and
[api.md](api.md) catalogues the complete API.
[host-feature-matrix.md](host-feature-matrix.md) connects host controls to the
public sky, sun, light-sampling, GI and denoiser records.

The optional C++20 host-driver layer is in `qray/host_contract.h`. Its
`QraySceneAsset`, `QrayFrameView`, `QrayFrameUpdates`, `IQrayHost` and
`QraySidecar` contracts lower to the same C transport. Ownership, supported
updates, failure semantics and the host-owned file-codec boundary are defined
in [host-driver-contract.md](host-driver-contract.md).

## 1. Boundary

QuakeRaySDK owns the Win32 surface integration, Vulkan/NVRHI device, render
targets, acceleration structures, lighting runtime and presentation. A host
owns the game loop, simulation, map/model/texture parsing, visibility,
animation, audio, input and UI construction.

The adapter's required output is a stream of:

- meter-valued triangle geometry with stable IDs and renderer-neutral material
  handles;
- cluster bounds and optional visibility rows;
- per-frame camera, light, raster, particle, decal and portal uploads;
- screen-space UI geometry when the host draws a HUD, menu or console.

The adapter must not hand the SDK a host pointer that remains required after a
call returns. The SDK copies scene uploads synchronously for the duration of
the call; an asynchronous readback destination remains host-owned until its
request completes.

This revision has one renderer instance per window and one drawn camera per
`qrDrawFrame`. Multi-view engines must schedule views as separate draws or
flatten view-dependent effects into the host's material and geometry data.

## 2. Required host data

| Area | Adapter must provide | SDK does not do |
|---|---|---|
| Map/scene | Triangles, 32-bit indices or non-indexed triangles, normals, up to three UV layers, cluster IDs | Parse BSP, UDMF, brushes, meshes, patches or entity definitions |
| Materials | Three RGBA8 logical textures: albedo/alpha, RME and normal; sampler state; the documented blend subset | Execute legacy shader bytecode or an arbitrary multi-stage fixed-function pipeline |
| Lighting | Meter-valued directional, spherical, spot, polygonal, textured-area or DTAL sources | Import entities, evaluate game light styles or infer light sources from textures |
| Visibility | A cluster partition and optional PVS/topology arrays | Build engine-specific portals, cells or BSP leaves |
| Animation | Skin, morph, skeletal, material and texture-coordinate changes at the upload boundary | Decode MDL/MD2/MD3/MD5/skeletal assets or run game animation graphs |
| Raster effects | Camera-facing vertices or particle points, material, pipeline state and explicit order | Parse `.spr`, Quake 3 shader stages, decals, trails or particle scripts |
| UI | Pixel-space quads/triangles and an orthographic projection | Recreate a game's font rasterizer, menu atlas or 2D clipping policy |

All positions, radii, areas, reaches and distances, plus any wave or surface
deformation amplitude that denotes world space, must be converted to meters.
UV scale/offset, angles, colors, normalized directions and cluster indices
remain dimensionless. See [spatial-contract.md](spatial-contract.md) for every
special case.

## 3. Renderer-family profiles

These profiles show where an older renderer needs real interpretation. They
are not alternate implementations inside the SDK.

| Representative family | Typical source data | Required adapter work |
|---|---|---|
| Classic BSP/lightmap, including Quake and GoldSrc | BSP faces, lightmaps, indexed models, sprites, palette textures | Decode palette/8-bit formats to RGBA8; tessellate faces; flatten or bake lightmaps; map leaves to clusters; split alpha-tested and blended sprites; synthesize dynamic-light sources |
| BSP plus vertex-animated models, including Quake II-era renderers | BSP, PVS, lightmaps, vertex models, shader-like surface flags | Decode model frames on the host; convert quads/triangle strips to triangles; map light flags to light types; preserve transparent ordering |
| Multi-stage shader BSP, including Quake III-era renderers | BSP plus curves, blend stages, `tcMod`/`deformVertexes`, MD3/skeletal models | Tessellate curves; evaluate or pre-bake shader stages; fold transforms/deforms into vertices or texture data; approximate unsupported blends; keep separate view ranges |
| Material-and-skeletal renderer, including Doom 3-era engines | Material programs, normal/specular maps, skeletal meshes, shadow volumes | Convert material inputs to albedo/RME/normal; skin on the host; replace shadow volumes with RT shadows; provide alpha cutouts explicitly |
| BSP/lightmap plus static props and displacements, including Source-era 2004/2005 data | Group/submodel geometry, lightmaps, large static props, skeletal props, VTF textures, HUD | Flatten instances; decode/convert textures to RGBA8; bake or replace lightmaps; upload animated props per frame; map water/glass to pass-through types; send UI to `SWAPCHAIN` |

Game extensions still matter. FTE models, MD5 skeletal animation, Quake 3
shader programs and engine-specific portal systems are not detected from raw
geometry. The adapter either expands them into the documented inputs or rejects
that feature with a host-side diagnostic.

## 4. Material mapping

The renderer's stable material contract is narrower than the fixed-function
and early-shader pipelines of this period.

| Source concept | Supported mapping |
|---|---|
| Opaque texture | `pDataAlbedoAlpha`; `QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE` |
| Alpha-tested cutout | Albedo alpha plus `ALPHA_TESTED`; traced geometry may also set `QR_GEOMETRY_UPLOAD_ALPHA_TRANSMISSION_BIT` |
| Roughness/metallic/emission | `pDataRoughnessMetallicEmission` using `QrTextureSwizzling`, or per-instance defaults |
| Tangent-space normal map | `pDataNormal`; renderer preprocess derives the tangent basis |
| Animated texture frames | `qrCreateAnimatedMaterial` and `qrChangeAnimatedMaterialFrame` |
| Lightmap multiply/add | Bake it into material data, use the renderer's RT lighting, or submit an explicitly authored emissive surface |
| Texture scale/offset | `QR_GEOMETRY_UPLOAD_TEXCOORD_TRANSFORM_BIT` for layer 0, or transform vertex UVs when multiple stages must be combined |
| Water, acid, mirror, glass or portals | Matching `QrGeometryPassThroughType` and the material/frame parameters |
| Screen-space HUD/menu | `QR_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN` with pixel positions |
| Sky | Cubemap, procedural sky or `SKY` raster geometry; see [sky-and-atmosphere.md](sky-and-atmosphere.md) |

The renderer has three logical material layers and three UV layers. A source
shader that combines more textures must be precomposed or split into multiple
surfaces; it cannot supply an arbitrary shader graph.

## 5. Transparency and ordering

The raster list preserves upload order only inside each emitted category.
`DEFAULT` records are partitioned into ordinary world draws, lit particle
geometry, point-particle batches and smoke; those categories are recorded in
that fixed order. `SKY` and `SWAPCHAIN` remain separate ordered streams. The
host must:

1. Upload opaque and alpha-tested cutouts before blended draws.
2. Upload blended world draws from far to near for the current view.
3. If overlapping vertices or particle points within one upload need a
   specific order, sort them on the host; draw order cannot repair ordering
   inside a triangle or point batch.
4. Use separate ordered ranges for views whose depth projections differ.
5. Upload `SWAPCHAIN` overlays in painter's order.

The renderer serializes collector writes internally, so concurrent upload calls
do not corrupt state. That lock does not define their relative order. If two
draws from different host threads overlap and their blend order is visible,
the host must order the calls itself.

Common mappings are:

| Legacy mode | Raster mapping |
|---|---|
| Alpha over | `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` |
| Premultiplied alpha | `ONE / ONE_MINUS_SRC_ALPHA` |
| Additive alpha | `SRC_ALPHA / ONE` |
| Additive color | `SRC_COLOR / ONE` or `ONE / ONE` |
| Modulate | `DST_COLOR / ZERO` |
| Destination-alpha easing | Not exposed; approximate with a supported source/destination pair |

Only source/destination factors from `QrBlendFactor` and the add equation are
exposed. Destination-alpha factors, subtract/min/max, logic operations,
separate color/alpha equations and per-target independent blending require a
core API addition or a host-side fallback outside the raster path.

Traced particle stand-ins behind reflection/refraction surfaces currently
support the alpha-over, premultiplied-over, alpha-add, color-add and
inverse-alpha modes implemented by `ParticleProxies.h`; other raster blend pairs
remain raster-only. Proxies are captured only for uploads carrying
`QR_RASTERIZED_GEOMETRY_STATE_PARTICLE_SPRITE`, and tracing also depends on the
frame's glass-particle flag and `qrSetParticleProxyGate`. Set the gate before
the particle uploads it should affect. Raster particle geometry supports the
documented blend subset, while `qrUploadParticles` point batches always use
regular alpha-over. The traced proxy samples the albedo texture at LOD 0;
minified raster sampling can therefore differ in filtering even when the alpha
formula is the same. `SMOKE` and particle states are valid only for `DEFAULT`
uploads and `SMOKE` is mutually exclusive with either particle bit.

## 6. Host-owned renderer composition

The following operations are deliberately outside the SDK renderer contract.
They are architectural host responsibilities, not missing SDK features or
conformance failures:

- **Skeletal, morph and vertex deformation:** the host evaluates the animation
  graph and uploads final, meter-valued geometry. The SDK does not run engine
  animation systems.
- **Stencil, render-to-texture and multi-pass effects:** the host performs them
  in its own renderer or precomposes their final contribution into supported
  materials, geometry or UI uploads.
- **Blend equations outside the exposed subset:** the host precomposes, splits
  or approximates the effect, or renders that layer in a host-owned compositor.
  The SDK rejects or leaves outside its path rather than silently changing a
  visually required equation.
- **Multiple traced cameras, multi-view and dynamic cubemap targets:** one
  `qrDrawFrame` call represents one camera. The host schedules additional draws,
  flattens the view-dependent result, or performs the composition itself.
- **Portal recursion and reflection/refraction cameras:** the host either uses
  the documented pass-through behavior or owns the recursive camera work.

The adapter capability report must classify an unsupported effect as
host-owned, an SDK core gap or out-of-scope integration. Effects that do not
affect the tested scene may be declared unsupported, but a required layer must
not be silently discarded.

## 7. Acceptance matrix

A host adapter is ready for integration review when all applicable rows have
evidence:

| Check | Evidence |
|---|---|
| Package | A clean consumer links `qray::renderer` without adding `QR_STATIC`, `QR_USE_SURFACE_WIN32` or `NOMINMAX` manually |
| Spatial | Identity, Y-up and mirrored meter scenes match the SDK's twin test |
| Geometry | Indexed and non-indexed triangles, arbitrary instance transforms, UV layers and stable IDs render without validation errors |
| Opaque/cutout | Alpha-tested silhouettes and depth behavior match the host reference |
| Transparency | Alpha-over, additive, premultiplied or modulate draws use the documented order and supported factors |
| Lighting | Static and per-frame lights reach the expected clusters; omitted PVS uses the all-visible fallback only when intended |
| Atmosphere | Sky, fog, water/glass and one local light source are exercised |
| UI | HUD/menu text and clipping remain pixel-aligned at two window sizes |
| Frame | `qrStartFrame` and `qrDrawFrame` are balanced; no host pointer outlives its documented lifetime |
| QuakeRay host | The parent build and CTest suite pass against the exact SDK checkout used by the host |

The QuakeRay host is the reference integration, not proof that another game's
material, visibility or asset decoder is correct. A second real host is not a
release prerequisite for contract version 1.0: the SDK claim covers the
source-level contract and the QuakeRay production path. Each external host
validates its own adapter, captures and unsupported-feature report.

In the SDK source tree, `Tests/PackageConsumer` is the minimal
installed-package probe. Configure it against an install prefix with
`QR_NVRHI_SOURCE_DIR` pointing at the pinned, patched NVRHI checkout; it
deliberately does not define the SDK's public compile switches itself and fails
at compile time if `nvrhi::c_MaxBindingLayouts < 13`.
