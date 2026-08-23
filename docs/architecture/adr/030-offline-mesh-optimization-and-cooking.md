---
status: proposed
updated: 2026-08-23
authority: adr
---

# ADR-030: Offline mesh optimization and cooked geometry artifacts

## Status

Proposed.

## Context

The mesh loader parses OBJ/glTF/GLB at runtime, deduplicates each material
bucket, generates tangents, and writes a local `.vkb` sidecar containing raw
`VkrVertex3d` and `uint32_t` data. The cache prevents repeat source parsing but
does not provide compressed delivery, a reproducible build artifact, content
hash validation, or an explicit compatibility contract.

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

Pull the dependency through a `vendor/meshoptimizer` git submodule pinned to a
reviewed upstream commit. CMake builds it as a static child target, following
the existing `vendor/ktx-software` integration. A missing checkout fails
configure with the normal submodule-initialization instruction. CMake does not
download meshoptimizer during configure.

Meshoptimizer source remains behind a small internal C++ bridge with a
VKR-owned `extern "C"` interface. The bridge and cooker can link the upstream
static target. C11 loader code calls the bridge. No public VKR header or hot
path exposes meshoptimizer or C++ types.

After cooked loading has shipped and has a measured baseline, implement a
second, opt-in runtime source-optimization path for OBJ/glTF/GLB. It runs on the
mesh loader's worker during `prepare_async`, uses only scoped scratch data, and
writes the final current-ABI mesh result to the normal result arena. It does not
write `.vkb`, mutate source files, or run for `.vkb` requests. Its first slice
is remap, vertex-cache order, and vertex-fetch order. Overdraw requires a
separate measured opt-in. The cooked and runtime paths remain separate
implementations; they may share pure range-level algorithms and test vectors.

The cooker processes each material/submesh range in this order: deduplicate,
filter only semantically removable triangles, optimize vertex cache, optionally
optimize overdraw with target evidence, optimize vertex fetch, quantize into a
declared runtime layout, then encode vertex and triangle-index streams.

The `.vkb` extension is retained. The cooker bumps `VKR_MESH_CACHE_VERSION` and
the new `.vkb` payload owns codec version, content and settings hashes,
stream directory, bounds, decode parameters, material/range metadata, and
checksums. The loader validates all untrusted byte ranges before allocation or
decode. A runtime load never rewrites source assets or silently cooks them.

The current version-12 raw `.vkb` cache becomes stale at the version bump. A
development asset-build step regenerates it. Production accepts only the cooked
version. `vkr_mesh_cooker` is its writer; the runtime source optimizer does not
update the artifact.

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
- Codec compression reduces delivery/storage and CPU source-read work. It does
  not, by itself, reduce the current 64-byte GPU vertex ABI or 32-bit Vulkan
  index allocation. After offline cooking and runtime optimization, ADR-031
  adds the separate packed GPU ABI vertical slice.
- Metrics must distinguish source, cooked, decoded, uploaded, and resident
  bytes. File size alone is not a frame-time result.
- Meshoptimizer source is C++ although its header/API is C-compatible. Keep
  C++ restricted to the bridge/dependency/tool targets; VKR's public C
  interfaces stay C11.
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
- A content pipeline adopts a different authoritative offline optimizer with a
  demonstrably better measured result and equivalent C11 integration boundary.
