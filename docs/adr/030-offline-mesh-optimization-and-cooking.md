---
status: implemented
updated: 2026-09-05
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

Baked glTF node transforms apply inverse-transpose transport to normals and
the model's linear part to tangent directions. Cooked version 16 invalidates artifacts made with the older
tangent transform so corrected source loading cannot reuse those baked results.

Per-material glTF builders grow geometrically because their arena retains old
buffer generations. Finalization reserves merged vertex, index and range upper
bounds once before visiting the material buckets. This bounds retained copying
storage without changing primitive order, deduplication or cooked output policy.

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
