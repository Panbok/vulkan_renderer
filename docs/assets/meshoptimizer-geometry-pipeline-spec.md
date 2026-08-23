---
status: proposed
updated: 2026-08-23
authority: design
---

# Meshoptimizer Geometry Loading and Packing Specification

## Conclusion

VKR should integrate [meshoptimizer](https://github.com/zeux/meshoptimizer)
through two separate paths. Ship deterministic offline cooking first. Then add
per-load runtime optimization for uncooked or dynamic source meshes. The current
resource-system split has the correct seam for both: a worker prepares CPU mesh
bytes and the render thread finalizes dependencies and publishes geometry.

Meshoptimizer alone is not a VRAM optimization in this renderer. The current
publishers accept only `VkrVertex3d` (64 bytes) and 16/32-bit input indices,
then Vulkan always allocates and uploads 64-byte vertices plus 32-bit indices.
Using the codec while reconstructing that ABI reduces source/disk bytes and
parsing time, but leaves resident geometry and upload bytes unchanged. Tight
GPU packing is a separate third phase, after offline cooking and per-load
runtime optimization.

No performance or size reduction is claimed by this document. Establish the
baseline and apply the acceptance gates before treating the proposal as a
result.

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
record decoded CPU bytes, upload bytes, or resident GPU bytes. Those additions
are prerequisite observability work for this proposal.

### Mesh ingestion and cache

The single `VKR_RESOURCE_TYPE_MESH` loader dispatches `.obj`, `.gltf`, and
`.glb` inside `vkr_mesh_loader_parse_source()`. It is intentionally one loader:
the resource system routes ordinary batch loading by resource type, so a second
mesh loader would not safely route mixed source formats. The glTF design
documents the same constraint in [gltf-loader-design.md](gltf-loader-design.md).

For each material bucket, `vkr_mesh_loader_finalize_builder()` currently:

1. deduplicates byte-identical `VkrVertex3d` vertices;
2. generates tangents;
3. computes bounds;
4. appends the resulting vertices and globally offset 32-bit indices to one
   merged payload.

The sidecar `.vkb` cache (magic `VKMH`, currently version 12) persists those
same merged `VkrVertex3d` and `uint32_t` arrays verbatim, together with source
paths, dependency mtimes, and submesh metadata. It avoids repeated parse work,
but it is not compressed and is not a production asset contract: it is written
during runtime loading beside source data and invalidated from source mtimes.

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

## Proposed asset pipeline

### Offline artifact boundary, first implementation

Promote the existing `.vkb` extension into the deterministic cooked artifact.
`vkr_mesh_cooker` consumes source OBJ/glTF/GLB and writes versioned immutable
`.vkb` files. Source formats remain authoring inputs. Production scenes refer
to `.vkb` explicitly, or the asset build rewrites their references atomically.
The runtime does not silently cook source assets, write beside them, or fall
back to source parsing in a shipped build.

The cooker replaces the current raw-cache payload by bumping
`VKR_MESH_CACHE_VERSION`. The `.vkb` header must contain:

- magic, format version, endianness, and explicit meshoptimizer codec version;
- a content hash of all source/dependency bytes and a cooker-settings hash;
- layout identifier and per-range decode parameters;
- exact byte ranges, encoded/decoded sizes, alignment, and checksum for every
  vertex and index stream;
- range/submesh metadata, material identifiers, bounds, and dependency data.

All untrusted lengths and offsets are validated once at the file boundary
before allocation. Invalid artifacts fail the request; they never reach a
publisher with partly decoded data.

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

Pull meshoptimizer through a pinned `vendor/meshoptimizer` git submodule. This
matches the existing `vendor/ktx-software` pattern: the checkout is part of the
source tree, CMake builds it as a static child target, and configure fails with
a direct `git submodule update --init --recursive` instruction when it is
missing. Do not use `FetchContent` or any configure-time network download.

The submodule pin is the reviewed upstream commit. Record its release/tag and
commit in the asset-pipeline documentation, preserve its MIT license text, and
update it only through an explicit dependency review. Do not copy its source
files into VKR. That would create a snapshot with less upgrade provenance than
the submodule.

meshoptimizer source is C++, although its header/API is C-compatible. Build a
small internal C++ bridge that links the static upstream target and exports only
VKR-owned `extern "C"` functions to C11 loader and cooker code. The bridge
header stays private to the mesh pipeline. CMake enables C++ for the bridge and
cooker targets, and the final link uses the C++ runtime through the target
dependency. Public VKR headers and per-draw code do not expose C++ or
meshoptimizer types.

### Runtime `.vkb` load

`vkr_mesh_loader_create()` remains the sole mesh loader. Its `can_load` and
source dispatch retain `.vkb`; no new resource type, extension, or scene API is
needed.

1. Worker `prepare_async` validates the `.vkb` directory and dependency hash.
2. It decodes streams into the result arena in their already packed runtime
   representation, creates the existing submesh ranges, and records material
   dependency requests.
3. `finalize_async` retains its present render-thread material resolution and
   publication contract. It must account for decoded/upload bytes in its
   `VkrResourceAsyncFinalizeCost` before publication.
4. Unload continues to release the loader result, material request references,
   and finally GPU resources through their existing proven-submit retirement
   path.

The decoder is CPU work and belongs to the worker stage. There is no per-draw
decode, allocation, lookup, or fallback branch.

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
2. **Cooker.** Add the pinned `vendor/meshoptimizer` submodule, build it as a
   static dependency, build `vkr_mesh_cooker`, and add
   deterministic byte-for-byte and malformed-artifact tests. Compare decoded
   vertex/index bytes with the canonical pre-quantization representation.
3. **Cooked runtime load.** Add `.vkb` worker decode with bounded per-request memory,
   source/dependency hash validation, and CPU-only tests. Exercise cancellation,
   failure, and repeated load/unload paths.
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
