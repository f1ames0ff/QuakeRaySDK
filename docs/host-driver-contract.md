# Host driver contract

`qray/host_contract.h` is an optional C++20 source-level layer over the
existing `qray.h` C ABI. It defines the host-facing scene, frame, update,
callback and authoring-intent contracts without adding another renderer or
scene-transfer path.

## Versioning

All extensible top-level descriptors begin with `QrayContractHeader`:

```cpp
struct QrayContractHeader
{
    uint32_t size;
    uint32_t version;
};
```

The current source contract is version `1.0` (`0x00010000`). A reader accepts
a descriptor whose `size` covers the current structure and whose major version
matches. A different major version is unsupported. Fields are append-only;
published fields are never reinterpreted.

This version is independent from `QR_API_VERSION`, the renderer package
version and the sidecar schema version.

## Contracts

### QraySceneAsset

`QraySceneAsset` is a borrowed, immutable load-time view. It describes:

- persistent materials;
- static and static-movable geometry;
- authored light intent;
- DTAL group and member arrays;
- world cluster topology, PVS and world-light faces;
- persistent fog volumes.

`QraySceneBinding::Bind` prevalidates the complete asset before opening the
static-geometry window. It creates materials through `qrCreateMaterial` or
`qrCreateAnimatedMaterial`, lowers surfaces through `qrUploadGeometry`, then
uploads world topology and fog through their existing calls.

The host must not release asset arrays until `Bind` returns. The binding does
not retain geometry, material or face pointers. It copies only the small
frame-scoped light and DTAL records that must be replayed every frame.

Static binding is not transactional. `qrBeginStaticGeometries` clears the old
static state immediately and the C ABI has no abort or rollback call. A failure
after that point poisons the binding; the host must recreate the scene or the
renderer instance.

### QrayFrameView

`QrayFrameView` references the existing `QrStartFrameInfo` and
`QrDrawFrameInfo` payloads. It adds a nonzero stable `viewId` and recognized
flags for diagnostics. The payloads and all nested parameter blocks are
borrowed for `QraySceneBinding::RenderFrame`.

The current renderer supports one drawn camera per frame. Projection remains
renderer-owned, a traced-camera viewport is not exposed, and
`QRAY_FRAME_VIEW_RESET_HISTORY` currently returns `QR_FEATURE_UNAVAILABLE`.
Multi-view hosts schedule separate frames.

### QrayFrameUpdates

`QrayFrameUpdates` is a set of typed borrowed arrays. It dispatches in this
order:

1. scene-authored frame lights and DTAL, unless replacements are supplied;
2. transforms and UV patches for `STATIC_MOVABLE` geometry;
3. material texture-content updates;
4. dynamic traced geometry;
5. rasterized geometry, particles and decals;
6. portals;
7. replacement frame lights and DTAL;
8. cluster light sources;
9. fog volumes;
10. `qrDrawFrame`.

Material-content updates target persistent material keys and call
`qrUpdateMaterialContents`; they never re-upload static geometry. Scalar
material changes, geometry removal and material-handle replacement are not
ordinary deltas and require a new scene bind.

Arrays and their pointed data must remain valid until `RenderFrame` returns.
The binding never retains them. Raster upload order is preserved.

### IQrayHost

`IQrayHost` is an optional control-plane interface implemented by the host.
The renderer does not own or call it automatically.

The required capability query returns a versioned `QrayHostCapabilities`.
Optional operations default to `QR_FEATURE_UNAVAILABLE`:

- runtime-to-source reverse lookup;
- source-to-runtime resolution;
- frame refresh;
- light-style evaluation;
- sidecar application with one result per entry;
- authored-light loading;
- picking.

Callbacks run synchronously on the host's render/owner thread. They must not
re-enter the binding or call the renderer while a callback is active. The host
object must outlive the period during which callers use it.

### QraySidecar

`QraySidecar` is versioned authoring intent for material, light and DTAL
edits. It stores semantic values and durable source/authoring identities. It
must never serialize `QrMaterial`, runtime geometry/light `uniqueID` values,
cluster indices or GPU handles.

The SDK validates an already-decoded sidecar:

- schema and contract versions;
- nonzero scene key and matching bound-scene fingerprint;
- nonzero and unique authoring IDs;
- target key domains;
- known patch flags, finite scalar values and supported entry kinds.

File parsing, comments, unknown-field preservation and atomic writes remain
host-owned because those policies depend on the authoring file format. The
host's `IQrayHost::ApplySidecar` resolves entries and fills
`QraySidecarEntryResult` rows with `Applied`, `Rebound`, `Unchanged`,
`Orphaned`, `Ambiguous`, `Unsupported` or `Invalid`.

The host must validate the complete sidecar before applying any entry. A result
capacity smaller than the entry count returns `QR_WRONG_ARGUMENT` without
partial application. A sidecar from a different scene or geometry/dependency
fingerprint is rejected as stale and must be rebased by the host.

## Lifecycle

```text
qrCreateInstance
qrSetSpatialConvention
{
  QraySceneBinding::Bind

  per frame:
    QraySceneBinding::RenderFrame

  QraySceneBinding::Reset or scope exit
}
qrDestroyInstance
```

`Reset`, move assignment and destruction release material handles owned by the
binding. The binding must be reset or destroyed before `qrDestroyInstance`;
the SDK cannot safely identify a renderer instance after the host has already
destroyed it.

The binding is move-only and single-owner. Calls are serialized on one frame
owner thread; it is not a replacement for the host's task graph.

## Explicit boundaries

- `qray.h` remains the only scene transport.
- The SDK owns the device, swapchain, render targets, materials, BLAS and TLAS.
- Host payloads are borrowed for their documented synchronous call.
- Static rebind can wait for device idle and must not be hidden in `Refresh`.
- Contract version 1.0 represents one traced camera per `RenderFrame`.
  Additional views are host-owned composition through additional draws or
  flattened inputs. Arbitrary projection, traced-camera viewports, general
  history reset, incremental static removal and material-handle replacement
  likewise stay outside this contract rather than being renderer defects.

## Conformance

`qray_host_contract_tests` uses a link-time fake transport to assert:

- scene validation and exact bind call order;
- stable-key to runtime-ID/material resolution;
- destructor and move-assignment material ownership;
- world-light face material lowering;
- source-key update dispatch and material-content updates;
- missing per-frame material references rejected before frame start;
- unsupported-history rejection before a frame starts;
- sidecar duplicate-ID, unknown-flag and default-capability checks.

The installed `Tests/PackageConsumer` compiles and links the public contract
through `qray::renderer`.
