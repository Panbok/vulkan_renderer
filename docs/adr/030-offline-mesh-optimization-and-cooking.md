---
status: implemented
updated: 2026-09-07
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

The tool-owned glTF importer also decodes `EXT_meshopt_compression` input
buffers. Runtime mesh loading accepts cooked `.vkb` artifacts; it does not
import or optimize source geometry. Worker preparation decodes the artifact
and requests materials; render-thread finalization resolves material references
before publication.

Editor Bakery invokes the same standalone mesh cooker with explicit input and
output arguments on its cancellable worker. Source `.obj`, `.gltf` and `.glb`
paths produce a sibling `.vkb`. Mesh jobs always rebuild; they do not claim the
incremental checks used by the font and texture cookers. Loading the result still
passes through the existing cooked-artifact validation and scene reload boundary.

glTF MASK materials with unit vertex alpha receive material-specific base-color
`.vkt` variants during cooking. The importer supplies cutoff and base-color
alpha factor to the shared texture-cooking library, then publishes the material
reference after texture success. Equal source/policy recipes share outputs;
generated texture contents participate in mesh dependency hashing. The
[texture decision](012-texture-compression-pipeline.md) owns filtering, cache
identity and unsupported-input limits. This work runs within the mesh-cooker
process, preserving Bakery cancellation ownership. Compatible glTF normal/MR
inputs use the same library to publish paired material recipes, including
factor-only roughness and compatible prepared specular-glossiness inputs. Both
texture files join mesh dependency hashing before the new material paths and
folded factors are emitted.

Cooked version 17 stores original glTF node indices, names, parent links, exact
local matrices, selected-scene membership, source mesh spans, punctual lights,
camera/skin references, animation count and a source-content fingerprint.
Runtime `.vkb` loading needs no authoring file to recover node identities.
Scene-specific light range overrides are resolved by the offline mesh cooker
through repeatable `--light-range <definition>=<meters>` arguments. Main Bistro
uses `bistro-lights-main.vkb` with six 5 m overrides; scenes without those
adjustments retain `bistro-lights.vkb`. The [cook scripts](../../tools/cook_vkr_meshes.sh)
retain the exact inputs. Runtime scene loading rejects source-light import
fields and instantiates the already-resolved punctual values from cooked nodes.
Changed scene or source fingerprints can invalidate existing editor override
sidecars. The loader preserves those files and rejects conflicting overrides;
they must be reapplied against the updated source identity.
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

- [artifact contract](../../runtime/src/assets/vkr_mesh_cooked.h)
- [decode](../../runtime/src/assets/vkr_mesh_cooked_decode.c) and
  [encode](../../tools/assets/vkr_mesh_encode.c)
- [meshoptimizer bridge](../../tools/assets/vkr_meshoptimizer_bridge.cpp)
- [glTF meshopt input decode](../../tools/assets/mesh_loader_gltf.c)
