---
status: accepted
updated: 2026-08-25
authority: adr
---

# ADR-030: Offline mesh optimization and cooked geometry artifacts

## Status

Accepted and implemented.

Offline cooking, worker validation/decode, mandatory runtime source
optimization, versioned packed geometry, lifecycle/byte/locality metrics,
production scene conversion, and `EXT_meshopt_compression` input decode are
implemented.

## Context

Before this decision, the mesh loader parsed OBJ/glTF/GLB at runtime,
deduplicated each material bucket, generated tangents, and wrote a local
version-12 `.vkb` sidecar containing raw `VkrVertex3d` and `uint32_t` data.
That cache prevented repeat source parsing but provided no compressed delivery,
reproducible build artifact, content hash, or explicit compatibility contract.

The resource system already separates worker CPU preparation from render-thread
finalization and GPU publication. Scene loading requests meshes as deduplicated
async dependencies, so a new mesh format can fit the current request/handle
contract. Meshoptimizer supplies C-callable algorithms for indexing, locality
optimization, quantization support, and fast vertex/index codecs, but its
codecs require optimized, quantized data to be effective.

## Decision

Adopt meshoptimizer as a pinned, static dependency of a new offline
`vkr_mesh_cooker` tool. The cooker is the first implementation and emits
deterministic versioned `.vkb` artifacts; the runtime mesh loader decodes `.vkb`
in `prepare_async` and keeps the present resource type, mesh-result ownership,
material dependencies, and render-thread finalization model.

The dependency is `vendor/meshoptimizer` v1.2 at commit
`9d9890c73011d75920af614485296d1e03e95448`. CMake builds it as a static child
target, following `vendor/ktx-software`; a missing checkout fails configure
with the normal submodule-initialization instruction. Configure performs no
network download.

Meshoptimizer source remains behind the internal C++
`vkr_meshoptimizer_bridge` target and a VKR-owned `extern "C"` interface. The
C11 loader and cooker call that bridge; no public VKR header or hot path exposes
meshoptimizer or C++ types.

Every OBJ/glTF/GLB source load applies per-range cache and vertex-fetch
optimization on the mesh worker. The optimizer uses request-local scratch and
commits only final packed data to the result arena. There is no configuration
or environment-variable bypass. Source loads never write `.vkb`, and cooked
requests never rerun runtime optimization.

Version-14 `.vkb` preserves importer deduplication and tangent generation,
optimizes cache and vertex-fetch order per material range, quantizes into the
ADR-031 static geometry ABI, and meshoptimizer-encodes packed vertices and
`uint32_t` indices. It records codec/library and layout versions, SHA-256
dependency/settings identity, aligned stream directories, encoded and decoded
sizes, optimized bounds, material/range metadata, quantization budgets and
observed maxima, and CRC32 metadata/stream checksums.

The loader validates bounded metadata, metadata/stream checksums, codec headers,
quantization maxima, decoded packed bounds, and local indices before
publication. Dependency paths, sizes, and SHA-256 data remain embedded build
provenance covered by the metadata checksum; runtime decode performs no
authoring-source I/O. Version-12 and version-13 artifacts are stale;
`vkr_mesh_cooker` is the sole version-14 writer.

Production scene manifests reference `.vkb`. `tools/cook_vkr_meshes.sh` and
`tools/cook_vkr_meshes.bat` build the cooker, regenerate selected or default
mesh artifacts, then run the shared texture packer once so glTF-generated
material derivatives have `.vkt` siblings before the command succeeds.
Repository assets are passed to the cooker from the repository root with
normalized repository-relative identity. Generated material IDs therefore do
not depend on the checkout directory, host platform, or whether the caller
spelled the input as an absolute path.
Authoring dependency identity remains recorded in the artifact but is not a
runtime dependency. Normal renderer builds still do not cook meshes; they do run
the shared incremental texture packer after successful compilation.

The source-free statement applies to geometry decode. Scene-level metadata may
remain an explicit runtime dependency: Bistro names `mesh.gltf_light_source`
so its cooked `.vkb` geometry can retain the glTF punctual-light nodes without
reopening the glTF during mesh decode. The scene loader treats that explicit
metadata path as required and fails the scene request if it cannot be parsed.

For input interoperability, glTF buffer views using
`EXT_meshopt_compression` decode on the loader worker before accessor reads.
The supported modes are `ATTRIBUTES`, `TRIANGLES`, and `INDICES`, with the
meshoptimizer `NONE`, `OCTAHEDRAL`, `QUATERNION`, `EXPONENTIAL`, and `COLOR`
filters. `KHR_meshopt_compression` is rejected rather than misinterpreted.

## Consequences

- Source format parsing and expensive mesh optimization move out of startup and
  load hitches for cooked content. Worker decode remains asynchronous and
  bounded by a mesh result's lifetime.
- Runtime optimization adds mandatory CPU work to development, dynamic, and
  uncooked source loads. Production scenes use pre-optimized `.vkb`, so their
  runtime path pays only validation and decode.
- The project gains a reproducible production geometry artifact and an asset
  build dependency. Cooker settings become versioned content decisions.
- Retaining `.vkb` preserves the existing loader extension and type-only batch
  routing; creating a second `VKR_RESOURCE_TYPE_MESH` loader is still unsafe.
- Codec compression reduces cooked artifact storage; ADR-031 packing separately
  halves static vertex upload and residency bytes. Version 14 is a source-free
  runtime geometry contract: freshness is decided by the offline cook step,
  while runtime validates the self-contained artifact. Indices remain 32-bit.
- `.vkb` is the persistent geometry cache and live resource requests deduplicate
  within one process. Harness cold/warm cache modes refer only to the GPU
  pipeline cache; separate harness repetitions do not share decoded meshes,
  transcoded textures, or GPU residency.
- Metrics distinguish source, cooked, decoded, uploaded, and resident bytes,
  vertex/index/decode-metadata occupancy and high-water values, range and
  vertex/index counts, runtime-optimization counts, cache ACMR/ATVR, and fetch
  overfetch. File size alone is not a frame-time result.
- Meshoptimizer source is C++ although its header/API is C-compatible. Keep
  C++ restricted to the dependency and bridge targets; VKR's public interfaces
  and cooker remain C11.
- The submodule commit is part of the reproducible asset pipeline. Updating it
  requires source, license, codec-compatibility, and cooker-regression review.

## Alternatives Considered

**Run meshoptimizer on every runtime source load first.** Rejected as the first
implementation because it repeats content-build work without producing a stable
packaging artifact. It is accepted as mandatory behavior for uncooked
OBJ/glTF/GLB after the offline artifact path; production `.vkb` remains the
preferred path.

**Promote `.vkb` without changing its contract.** Rejected. The present cache
has runtime writes, mtime-only identity, and raw ABI coupling. This ADR instead
keeps the extension while replacing its versioned payload and writer policy.

**Copy meshoptimizer source into VKR.** Rejected. It loses direct upstream
provenance and makes updates a manual source transplant. A pinned submodule has
the same offline build property and retains the exact reviewed source revision.

**Fetch meshoptimizer during CMake configure.** Rejected. Builds must not
depend on the network or silently change their dependency source.

**Use a general-purpose compressor alone.** Rejected. It does not improve
vertex/index locality or establish a packed runtime representation. It may be
layered over encoded `.vkb` streams later if measurements justify its decode
cost.

**Use glTF `EXT_meshopt_compression` as the only artifact format.** Rejected.
That extension is valuable input interoperability, but VKR needs metadata and a
layout contract matched to its renderer. Support it after cooked `.vkb`, not
instead of `.vkb`.

## Revisit When

- A measured Release comparison shows that cooker/decode overhead exceeds its
  storage and load benefit on representative content.
- Mandatory runtime source optimization causes unacceptable cold load time on
  development or dynamic content; change the algorithm or source workflow
  rather than adding a per-request bypass.
- A second production source format or remote-streaming use case requires a
  broader artifact/container boundary.
- Remote or untrusted artifact distribution requires a signed publication
  manifest; CRC32 and embedded source provenance detect corruption and identify
  inputs but are not an authenticity boundary.
- A content pipeline adopts a different authoritative offline optimizer with a
  demonstrably better measured result and equivalent C11 integration boundary.
