---
status: partial
updated: 2026-08-24
authority: design
---

# Meshoptimizer Geometry Loading and Packing Specification

## Conclusion

VKR integrates [meshoptimizer](https://github.com/zeux/meshoptimizer) through
the first, offline path. `vkr_mesh_cooker` emits deterministic version-13
`.vkb` artifacts, and the existing mesh worker validates dependency hashes and
decodes them into its result arena. Runtime optimization for uncooked or
dynamic source meshes remains the separate second phase.

Meshoptimizer alone is not a VRAM optimization in this renderer. The current
publishers accept only `VkrVertex3d` (64 bytes) and 16/32-bit input indices,
then Vulkan always allocates and uploads 64-byte vertices plus 32-bit indices.
Using the codec while reconstructing that ABI reduces cooked artifact bytes and
source-format parsing, but leaves resident geometry and upload bytes unchanged.
The current strict dependency policy still reads and hashes authoring inputs at
load time. Tight GPU packing is a separate third phase, after offline cooking
and per-load runtime optimization.

No performance or representative-content size reduction is claimed by this
document. The source/cooked/decoded/upload/resident-byte baseline, runtime
optimizer, packed GPU ABI, production asset conversion, and Release comparison
remain open.

## Current implementation

### Resource scheduling and ownership

`vkr_resource_system_load()` deduplicates an asynchronous request by
`<resource type>|<path>`. Meshes are async-by-default: the worker invokes a
loader's `prepare_async`, queues the payload, and the render-thread
`vkr_resource_system_pump()` invokes `finalize_async` within its request and
upload budgets. A `VkrMeshLoaderResult` owns its CPU data through a dedicated
arena or a returned `VkrArenaPool` chunk; `vkr_mesh_loader_destroy_result()`
destroys/releases that storage after the mesh manager has published it.

This is a sound ownership boundary for a decoded cooked mesh:

```text
source/cooked bytes (worker scope)
  -> decoded result buffers (mesh-result arena)
  -> render-thread material dependency resolution + geometry publication
  -> GPU-owned buffers; loader result released when the resource reference ends
```

The existing load metric records only the requested file's source size. A scene
metric does not include its mesh/material closure, and a mesh metric does not
record decoded CPU bytes, upload bytes, or resident GPU bytes. This remains
prerequisite observability work before any storage or performance conclusion.

### Mesh ingestion and cache

The single `VKR_RESOURCE_TYPE_MESH` loader dispatches `.obj`, `.gltf`, `.glb`,
and `.vkb`. Source requests parse authoring data without writing a sidecar.
Explicit `.vkb` requests enter the version-13 validator/decoder. It remains one
loader because ordinary batch routing is by resource type; a second mesh loader
would not safely route mixed formats. The glTF design documents the same
constraint in [gltf-loader-design.md](gltf-loader-design.md).

For each material bucket, `vkr_mesh_loader_finalize_builder()` currently:

1. deduplicates byte-identical `VkrVertex3d` vertices;
2. generates tangents;
3. computes bounds;
4. appends the resulting vertices and globally offset 32-bit indices to one
   merged payload.

Version-12 raw `.vkb` sidecars are retired. `vkr_mesh_cooker` is the sole
version-13 writer; each material range has separately cache/fetch-optimized and
meshoptimizer-encoded `VkrVertex3d` and `uint32_t` streams. The artifact records
SHA-256 dependency/settings hashes, bounds, material metadata, aligned stream
ranges, encoded/decoded sizes, and CRC32 metadata/stream checksums.

The glTF importer reads every accessor into temporary `VkrVertex3d` and bakes
each node's world transform into its vertices before it emits a primitive.
Consequently, repeated glTF mesh nodes can become separate transformed geometry
inside one loaded resource. Meshoptimizer cannot recover instancing after that
bake; preserving node/mesh sharing is a separate importer and scene-model
change that should be evaluated before making large content-size promises.

### GPU representation

`VkrVertex3d` is a reflected 64-byte interleaved ABI: float32 position and
normal, float32 UV, float32 color, and float32 tangent. The Vulkan asset
publisher allocates the geometry megabuffer using exactly
`vertex_count * sizeof(VkrVertex3d)` and `index_count * sizeof(uint32_t)`;
it sets `VKR_GPU_VERTEX_LAYOUT_3D`. The Metal packet path also takes
`VkrVertex3d` and `uint32_t` indices. This is one shared data contract, not a
loader-local choice.

Therefore the raw baseline for a mesh is:

```text
CPU cache payload and Vulkan resident geometry = 64 * vertex_count + 4 * index_count bytes
```

The loader may pass a 16-bit index buffer, but Vulkan expands it to 32-bit for
the megabuffer. Any claim that a 16-bit cooked index saves Vulkan VRAM is false
until the publisher and draw ABI support it.

The existing address-based vertex-pulling path is the right place to change
this. A `VkrGpuGeometryRow` supplies vertex and index addresses and the shader
uses `first_vertex` to pull records from the megabuffer. In the Vulkan deferred
visibility resolve and Metal G-buffer resolve, the shader reconstructs a
visible triangle by loading three full vertex records. Thus a tighter static
record can reduce megabuffer residency, upload traffic, and geometry reads in
the passes that pull those records. Cache-line behaviour, shader-generated
loads, and decode arithmetic decide the frame-time result, so byte savings do
not prove a large frame-time gain.

## Asset pipeline

### Offline artifact boundary, implemented

The existing `.vkb` extension is the deterministic cooked artifact.
`vkr_mesh_cooker` consumes source OBJ/glTF/GLB and atomically writes immutable
version-13 `.vkb` files. Source formats remain authoring inputs and runtime
source loads do not write beside them. Production scene-reference rewriting
and asset conversion remain open. Generated material files referenced by cooked
ranges are included in the dependency manifest.

The cooker keeps persistent importer state in a source arena and parser/codec
temporaries in an independent scratch arena. Parser scope rollback therefore
cannot invalidate accumulated vertices, ranges, or dependency paths.

The version-13 `.vkb` header contains:

- magic, format version, endianness, and explicit meshoptimizer codec version;
- a content hash of all source/dependency bytes and a cooker-settings hash;
- layout identifier and per-range decode parameters;
- exact byte ranges, encoded/decoded sizes, alignment, and checksum for every
  vertex and index stream;
- range/submesh metadata, material identifiers, bounds, and dependency data,
  covered by a separate metadata checksum.

All untrusted lengths and offsets are validated once at the file boundary
before dependency I/O or result-buffer allocation. Invalid artifacts fail the
request; they never reach a publisher with partly decoded data.

### Cook order per independent renderable range

A range maps to the current material/submesh boundary. The cooker must preserve
triangle ownership, material selection, bounds, and visible appearance. It
processes each range in this order:

1. establish the exact deduplication policy and generate an index buffer;
2. remove degenerate/duplicate triangles only when the chosen renderer
   semantics permit it;
3. optimize index order for vertex reuse;
4. optionally optimize overdraw, behind target-specific evidence because tiled
   GPUs need not benefit;
5. reorder vertices for fetch locality;
6. quantize into the selected packed runtime vertex representation;
7. encode optimized vertex and triangle-index streams with meshoptimizer.

This follows meshoptimizer's required locality-before-codec order. Its codec is
lossless over the bytes supplied to it; quantization is the separately specified
lossy operation. The cooker records quantization error and rejects input that
exceeds the range's configured position, normal/tangent, UV, or color error
budget.

Version 13 deliberately encodes the current 64-byte `VkrVertex3d` layout and
32-bit indices losslessly. Deduplication and tangent generation remain in the
source importer; the cooker then applies vertex-cache and vertex-fetch
optimization per range before codec encoding. It does not remove triangles,
run overdraw optimization, or quantize. ADR-031 owns the later packed layout
and its error budgets.

Pull meshoptimizer through a pinned `vendor/meshoptimizer` git submodule. This
matches the existing `vendor/ktx-software` pattern: the checkout is part of the
source tree, CMake builds it as a static child target, and configure fails with
a direct `git submodule update --init --recursive` instruction when it is
missing. Do not use `FetchContent` or any configure-time network download.

The reviewed dependency is meshoptimizer v1.2 at commit
`9d9890c73011d75920af614485296d1e03e95448`. Its MIT license remains in the
pinned submodule. Updates require an explicit dependency review; do not copy
files into VKR. That would create a snapshot with less upgrade provenance than
the submodule.

meshoptimizer source is C++. The internal `vkr_meshoptimizer_bridge` target
links the static upstream target and exports only VKR-owned `extern "C"`
functions to the C11 loader and cooker. Its header stays private to the mesh
pipeline, and final targets acquire the C++ runtime through target
dependencies. Public VKR headers and per-draw code expose neither C++ nor
meshoptimizer types.

### Runtime `.vkb` load, implemented with observability gaps

`vkr_mesh_loader_create()` remains the sole mesh loader. Its `can_load` and
source dispatch retain `.vkb`; no new resource type, extension, or scene API is
needed.

1. Worker `prepare_async` validates the complete `.vkb` structure, metadata and
   stream checksums, codec headers, and dependency hash.
2. It decodes streams into the result arena in their already packed runtime
   representation, creates the existing submesh ranges, and records material
   dependency requests.
3. `finalize_async` retains its present render-thread material resolution and
   publication contract. Decoded/upload byte metrics and upload-cost projection
   remain open because geometry upload occurs later in mesh publication, not in
   mesh resource finalization.
4. Unload continues to release the loader result, material request references,
   and finally GPU resources through their existing proven-submit retirement
   path.

The decoder is CPU work and belongs to the worker stage. There is no per-draw
decode, allocation, lookup, or fallback branch.

Strict dependency verification currently requires every recorded dependency,
including authoring inputs and generated material files, to remain available to
the runtime and reads those bytes to reject stale output. This is a provenance
contract, not a source-free delivery contract. A production policy for
source-free packages remains part of asset conversion and must be explicit
before cooked assets replace scene references.

Focused tests cover deterministic encoding, SHA-256 metadata, direct loader
dispatch, valid range reconstruction, malformed offsets, checksum and codec
failures, dependency-hash failure, and one load/unload pool return. Cooked-path
async cancellation and repeated scene load/unload coverage remain open.

### Runtime source optimization, second implementation

The runtime path is a separate implementation, not a fallback hidden inside
the cooker and not a way to create a `.vkb` during gameplay. It applies only to
source OBJ/glTF/GLB requests that the caller explicitly opts into. Cooked
`.vkb` always decodes its stored representation and never passes through the
runtime optimizer.

The first runtime slice runs inside mesh `prepare_async` after source parsing
and before the result is retained for finalization. It uses a scoped worker
scratch allocation for remaps and transient buffers, then writes its final
buffers to the existing mesh-result arena. The mesh result retains no runtime
optimizer scratch state.

Its initial operations preserve the current `VkrVertex3d` ABI: vertex remap,
triangle/index cache order, and vertex fetch order. Overdraw optimization is a
separate opt-in setting because the value depends on the target GPU and pass
topology. Quantization, codec encoding, and persistent output wait for the
packed ABI and an explicit development-cache policy; they are not part of the
first runtime slice.

The source request identity stays `<type>|<path>`. One request produces one
optimized mesh result for all deduplicated consumers. The option must be part
of the request identity before the resource system permits different runtime
optimization policies for the same source path. Until that identity change
exists, runtime optimization has one process-wide, immutable policy selected at
resource-system initialization.

Keep the two paths distinct in code and test them independently:

```text
offline: source -> vkr_mesh_cooker -> .vkb -> loader decode
runtime: source -> loader parse -> runtime optimize -> result arena -> publish
```

They may share a small, pure range-optimization helper and common test vectors.
They must not share mutable cache state, source rewriting, or a policy branch in
the per-draw path. Runtime optimization remains disabled for shipping content
until its load-time and frame-time cost is measured against cooked loading.

### Packed GPU ABI, third implementation

After offline cooking and the separately measured runtime source optimizer,
add one new static GPU layout rather than changing `VkrVertex3d` in place. The
first implementation should test and, if the capture and performance gates
pass, adopt this 24-byte record:

| field | storage | decode authority |
| --- | --- | --- |
| word 0 | position X/Y, UNORM16×2 | per-publication scale and bias |
| word 1 | position Z, UNORM16; tangent sign and flags, UNORM16 | per-publication scale and bias |
| normal | octahedral SNORM16×2 | vertex shader |
| tangent | octahedral SNORM16×2 | vertex shader |
| UV | 2×float16 | vertex shader |
| color | RGBA8 UNORM | vertex shader |

Six 32-bit words make the 24-byte layout explicit. Read and decode them as
aligned scalar words; do not assume every 24-byte record has 16-byte vector
alignment. The per-publication decode record holds position scale, bias, and
format flags. The published geometry row needs a GPU address or stable index to
that record, plus a distinct layout tag and stride. `VkrGeometryConfig`'s byte
stride alone is not a layout contract.

This is intentionally one interleaved record. Splitting positions from the
other attributes may help position-only work later, but the current resolve
paths read position, normal, tangent, UV, and color for every reconstructed
triangle. Starting with multiple streams would add address/metadata loads
without avoiding those reads there. Revisit stream splitting only after a
profile shows a position-only pass with enough geometry traffic to pay for it.

The exact layout is accepted only after both selected implementations share
reflection/ABI assertions, the shader conversion preserves visual tolerances,
and the measured target workload benefits. A packed and legacy layout must be
partitioned into the correct pipeline/shader path at publication or batching.
Do not add a layout test in a vertex or per-pixel pull loop. `VkrVertex3d`
remains the compatibility route while non-static and legacy callers migrate.

The initial packed layout still uses 32-bit indices because both current draw
paths consume them: Vulkan binds `VK_INDEX_TYPE_UINT32` and the Metal indirect
command path uses `uint` indices. Support 16-bit index storage only with an
explicit index type in the geometry row and draw encoding. Until that vertical
slice exists, a 16-bit cooked index is decoded or expanded before upload and
does not reduce GPU bytes.

This phase changes upload and resident bytes from the raw 64-byte vertex ABI;
the codec phase does not. It also has to update both Metal and Vulkan publishers,
shader declarations, reflection checks, geometry-row decode metadata, and the
GPU-driven draw path as one vertical slice.

The storage arithmetic is substantial but it is not a frame-time prediction.
At the same vertex count, 24-byte vertices cut the vertex stream by 62.5%. For
a mesh with three 32-bit indices per vertex, total geometry bytes fall from
`64V + 12V = 76V` to `24V + 12V = 36V`, a 52.6% reduction. A later valid
16-bit index path would make that `30V`, a 60.5% reduction. These are exact
byte deltas before megabuffer alignment and fragmentation. They are not a
claim that the renderer runs 52.6% faster.

### glTF compression support

Current `mesh_loader_gltf.c` calls `cgltf_parse_file()` and
`cgltf_load_buffers()`, then reads accessors directly. It has no
`EXT_meshopt_compression` handling. Add this only after cooked `.vkb` is working:

- `EXT_meshopt_compression` is accepted when declared by a glTF asset and every
  compressed buffer view can be validated and decoded before accessor reads.
- Required extensions fail clearly when unsupported; no attempt is made to read
  the fallback placeholder as geometry.
- Decode the declared buffer view to its declared uncompressed length, apply its
  required filter, and then hand ordinary decoded data to the existing importer.
- This input capability is distinct from VKR's `.vkb` codec version. The former
  is interoperability; the latter is the runtime asset contract.

`KHR_meshopt_compression`, meshlets, LOD simplification, GPU-side decode,
skinning, and animation compression are deliberately out of scope. Meshlets
need a mesh-shader/cluster draw contract; bolting their codec onto indexed draws
would add storage without a consumer.

## Rollout and evidence

1. **Baseline.** Instrument source bytes, cooked bytes, decoded CPU bytes,
   upload bytes, GPU live/high-water bytes, triangle/vertex counts, and
   meshoptimizer cache/fetch analyzers. Run the normal Release profile on at
   least Sponza and Bistro before changing assets.
2. **Cooker — implemented.** The pinned static dependency, private bridge,
   `vkr_mesh_cooker`, atomic version-13 writer, deterministic byte comparison,
   malformed-artifact coverage, and current-ABI round trip ship.
3. **Cooked runtime load — partial.** `.vkb` worker decode validates bounded
   metadata, metadata/stream checksums, codecs, decoded bounds, and
   source/dependency hashes before normal material finalization. Focused CPU
   coverage includes direct load/unload;
   cooked-path cancellation and repeated scene load/unload remain open.
4. **Runtime optimizer.** Add the separately tested, opt-in source path with
   the current ABI's remap/cache/fetch operations. Measure cold and warm scene
   load time, request deduplication, and renderer frame time against cooked
   loading before enabling it for any shipping content.
5. **Tight packed GPU ABI.** Add the packed static vertex/decode record and
   branchless per-layout shader/pipeline routing to both selected
   implementations. Measure 24-byte storage against the 64-byte baseline,
   capture parity and quantization error, vertex/resolve GPU time, upload time,
   megabuffer live/high-water bytes, and fragmentation. Test 32-byte padding
   only if the 24-byte scalar-load layout loses to it in the same profile.
6. **16-bit index ABI.** Only if phase 5 leaves meaningful index traffic, add
   typed index publication and draw routing, then repeat phase-5 evidence.
7. **Interop and LOD.** Consider compressed glTF input and error-bounded LODs
   only after the initial artifact and packed ABI show a measured benefit.

## Acceptance criteria

- Every cooked range decodes to valid indices, valid bounds, and the declared
  material/range order; malformed offsets, sizes, hashes, and codec failures
  are rejected before publication.
- Repeated scene load/unload returns loader-arena/pool capacity and material
  references to their baseline; GPU geometry slot reuse remains gated by proven
  completion.
- Visual/capture comparison stays within the approved per-attribute error
  budgets on both selected implementations.
- Runtime optimization preserves the source mesh's material/range order and
  renderable output, has no allocations after `prepare_async` returns, and
  cannot cause two callers with different policies to share a request.
- The after report contains the same Release configuration, scene/camera,
  warm-up policy, artifact hashes, source/cooked/decoded/upload/GPU byte counts,
  and frame-time percentiles as the baseline. A size reduction without a
  measured runtime result is reported as storage-only.
- The packed-ABI report separately records vertex/resolve GPU times, packed
  decode metadata bytes, vertex/index live and high-water bytes, and the
  quantization error maxima. It may call the result a frame-time improvement
  only when the same Release profile shows it.

## Sources

- [meshoptimizer README: C interface, build, ordering, codec, and meshlets](https://github.com/zeux/meshoptimizer)
- [Khronos EXT_meshopt_compression specification](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Vendor/EXT_meshopt_compression)
- [ADR-030](../architecture/adr/030-offline-mesh-optimization-and-cooking.md)
- [ADR-031](../architecture/adr/031-versioned-packed-static-geometry-abi.md)
