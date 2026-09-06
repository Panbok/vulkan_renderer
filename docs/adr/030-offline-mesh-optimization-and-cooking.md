---
status: implemented
updated: 2026-09-06
authority: adr
---
# ADR-030: Versioned meshoptimizer-cooked mesh artifacts

## Status

Accepted.

## Context

Runtime mesh loading needs deterministic validation, compact transfer, and a
stable packed-geometry contract while glTF remains an authoring input.

## Decision

Cook meshes into versioned `.vkb` artifacts. Each artifact records explicit
little-endian fields, source and settings identities, meshoptimizer and codec
versions, ranges, dependencies, decode data, and checksums. The cooker applies
meshoptimizer locality and vertex/index encoding per range. The loader fully
validates the artifact before decoding it into runtime geometry.

The glTF importer also decodes `EXT_meshopt_compression` input buffers. Runtime
mesh loading retains source optimization for uncooked/imported input; cooked
artifacts are the durable interchange boundary.

Editor Bakery invokes the same standalone mesh cooker with explicit input and
output arguments on its cancellable worker. Source `.obj`, `.gltf` and `.glb`
paths produce a sibling `.vkb`. Mesh jobs always rebuild; they do not claim the
incremental checks used by the font and texture cookers. Loading the result still
passes through the existing cooked-artifact validation and scene reload boundary.

Cooked version 17 stores original glTF node indices, names, parent links, exact
local matrices, selected-scene membership, source mesh spans, punctual lights,
camera/skin references, animation count and a source-content fingerprint.
Runtime `.vkb` loading needs no authoring file to recover node identities.
Version 16 artifacts must be recooked because their vertices contain flattened
node transforms and cannot reconstruct the original shared meshes faithfully.

The glTF importer emits mesh-local geometry once per referenced source mesh.
Each primitive retains a range; material merging never crosses source mesh
boundaries. Scene instances apply the original node hierarchy at runtime.
Normal directions therefore use the renderer's inverse-transpose model
transport and tangents use its linear model transport, as for other mesh
instances. Primitive winding and tangent handedness are retained.

Authored decal offsets are measured in source-world meters. Only decal meshes
whose node linear transforms require different corrections receive geometry
variants. Import transforms the world normal offset back to local space before
storing each variant. Equal corrections reuse a variant; ordinary meshes remain
shared. Subsequent editor transforms move this imported geometry normally.
Variants increase stored geometry in proportion to distinct decal corrections,
not the number of ordinary mesh instances.

Finalization reserves merged vertex, index and range upper bounds before packing.
A source containing nodes but no triangle geometry remains a valid resource and
cooked artifact. It retains hierarchy and metadata with zero geometry ranges and
no GPU publication or upload bytes. Empty inputs without nodes remain invalid.

Source metadata shares the loader result arena; scene instantiation copies names
and component values into the scene owner before the loader resource releases.

## Consequences

Artifact compatibility is explicit rather than a native-struct memory image.
Changing packed geometry, codec settings, or meshoptimizer versions requires a
new compatible artifact version and recooking. A failed decode never publishes
partially initialized geometry.

## Alternatives considered

Shipping raw glTF moves conversion and validation cost into every load. Native
structure serialization would make alignment and platform layout part of the
file format.

## Revisit when

Streaming, progressive LOD, or a new GPU geometry ABI requires a different
artifact contract.

## Code evidence

- [artifact contract](../../lib/src/renderer/resources/loaders/vkr_mesh_cooked.h)
- [encode and decode](../../lib/src/renderer/resources/loaders/vkr_mesh_cooked.c)
- [meshoptimizer bridge](../../lib/src/renderer/resources/loaders/vkr_meshoptimizer_bridge.cpp)
- [glTF meshopt input decode](../../lib/src/renderer/resources/loaders/mesh_loader_gltf.c)
