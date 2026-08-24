---
status: partial
updated: 2026-08-24
authority: adr
---

# ADR-030: Offline mesh optimization and cooked geometry artifacts

## Status

Accepted (partial).

The pinned dependency, private bridge, deterministic version-13 cooker, and
worker-side `.vkb` validation/decode are implemented. Runtime source
optimization, packed/quantized geometry, production asset conversion,
source/cooked/decoded/upload/resident metrics, and matched Release evidence
remain open.

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

After cooked loading has shipped and has a measured baseline, implement a
second, opt-in runtime source-optimization path for OBJ/glTF/GLB. It runs on the
mesh loader's worker during `prepare_async`, uses only scoped scratch data, and
writes the final current-ABI mesh result to the normal result arena. It does not
write `.vkb`, mutate source files, or run for `.vkb` requests. Its first slice
is remap, vertex-cache order, and vertex-fetch order. Overdraw requires a
separate measured opt-in. The cooked and runtime paths remain separate
implementations; they may share pure range-level algorithms and test vectors.

The implemented version-13 cooker preserves importer deduplication and tangent
generation, then optimizes vertex-cache order and vertex-fetch order before
encoding each material/submesh range. It does not remove triangles, optimize
overdraw, or quantize. Those operations remain evidence- or ADR-031-gated.
Persistent importer state and scoped parser/codec scratch use independent
arenas so temporary rollback cannot invalidate accumulated cook input.

The `.vkb` extension is retained. Version 13 owns codec/library versions,
SHA-256 content and settings hashes, aligned stream directories, encoded and
decoded sizes, bounds, material/range metadata, and CRC32 metadata and stream
checksums. The loader validates bounded metadata, checksums and codec headers,
dependency hashes, decoded bounds, vertex values, and local indices before
publication. Generated material files referenced by a range participate in the
dependency manifest. A runtime source load never rewrites source assets or
silently cooks them.

Version-12 raw `.vkb` files are stale. `vkr_mesh_cooker` is the sole version-13
writer, and source requests no longer write sidecars. Production reference
rewriting and asset conversion remain open; the runtime source optimizer will
not update artifacts.

Treat tight GPU geometry packing as the third implementation, after these two
paths. ADR-031 owns that ABI change. It must not be folded into codec work or
the first runtime optimizer slice.

## Consequences

- Source format parsing and expensive mesh optimization move out of startup and
  load hitches for cooked content. Worker decode remains asynchronous and
  bounded by a mesh result's lifetime.
- Runtime optimization gives development, dynamic, and uncooked content a
  deliberate path. It adds CPU work to load time, so its policy must be explicit
  and its cold/warm costs measured against cooked loading.
- The project gains a reproducible production geometry artifact and an asset
  build dependency. Cooker settings become versioned content decisions.
- Retaining `.vkb` preserves the existing loader extension and type-only batch
  routing; creating a second `VKR_RESOURCE_TYPE_MESH` loader is still unsafe.
- Resource deduplication keys only contain type and path today. Different
  runtime policies cannot share a request. The first runtime implementation
  therefore has one immutable process-wide policy; a future per-request policy
  must extend the identity key before it is enabled.
- Codec compression reduces cooked artifact storage, but strict dependency
  verification still reads and hashes all recorded dependencies at runtime. It
  does not, by itself, provide source-free delivery or reduce the current
  64-byte GPU vertex ABI or 32-bit Vulkan index allocation. Asset conversion
  must choose an explicit production dependency policy before rewriting scene
  references. After offline cooking and runtime optimization, ADR-031 adds the
  separate packed GPU ABI vertical slice.
- Metrics must distinguish source, cooked, decoded, uploaded, and resident
  bytes. File size alone is not a frame-time result.
- Meshoptimizer source is C++ although its header/API is C-compatible. Keep
  C++ restricted to the dependency and bridge targets; VKR's public interfaces
  and cooker remain C11.
- The submodule commit is part of the reproducible asset pipeline. Updating it
  requires source, license, codec-compatibility, and cooker-regression review.

## Alternatives Considered

**Run meshoptimizer on every runtime source load first.** Rejected as the first
implementation. It repeats content-build work on the user-facing load path and
does not produce a stable packaging artifact. It is accepted later as a
separate, opt-in path for uncooked/dynamic content, with its own measurement and
request-identity rules.

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
- Runtime source optimization shows no net benefit, causes unacceptable cold
  load time, or needs request-key variation that the resource system cannot
  express safely.
- A second production source format or remote-streaming use case requires a
  broader artifact/container boundary.
- Production asset conversion needs source-free packages; replace strict
  authoring-input verification with an explicit signed/manifested publication
  contract rather than silently weakening runtime validation.
- A content pipeline adopts a different authoritative offline optimizer with a
  demonstrably better measured result and equivalent C11 integration boundary.
