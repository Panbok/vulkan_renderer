---
status: proposed
updated: 2026-10-01
authority: proposal
---
# Meshlet cluster culling

## Current baseline

World geometry is culled per instance/submesh candidate. Each backend
classifies camera, cascade and local-shadow views against frustum and HZB
history, compacts visible rows and emits Metal ICB or Vulkan indirect-count
draws ([ADR-028](../adr/028-gpu-driven-deferred-visibility-buffer.md)). A
visible candidate rasterizes every triangle of its submesh; nothing finer than
the candidate is rejected before the vertex stage.

Vertex shaders already fetch their own data through GPU addresses: the
visibility vertex shader reads the visible row, geometry row, decode record
and instance row, then decodes one 32-byte packed vertex
([ADR-031](../adr/031-versioned-packed-static-geometry-abi.md);
`vk_visibility_vertex` in
[deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
There is no fixed-function vertex input on either backend. The cooker uses
meshoptimizer 1.2 for vertex-cache/fetch optimization and stream encoding
([ADR-030](../adr/030-offline-mesh-optimization-and-cooking.md)) but builds no
meshlets. Neither backend uses mesh or object/task shaders.

ADR-028 states that the GPU-driven topology does not imply meshlets. This
proposal records why cluster culling is now worth evaluating and the evidence
it must produce.

## Measured motivation

Bistro on Apple M1 Pro, Metal 4, Release at `e8293e4b`; the tree differed only
by the `vendor/ktx-software` patch. Both runs used
`tools/profiles/local-windowed-gpu-single.json`, which is non-authoritative
(`profile.local_only`, `provenance.dirty`) and adds about 1.5 ms of pass
timestamps ([Lighting efficiency](lighting-efficiency.md)). Neither supports a
speed claim; they attribute cost.

| Run | Case | Internal extent | Report SHA-256 |
|---|---|---|---|
| A | `tools/cases/performance/bistro_metal_production_040.case.json`, 5 × 300 frames | 1024×576 | `273f7be03de0b01f43a939cf7fa385d6a7b161902b57167ce3fc5e6a6d39d1c8` |
| B | Same case with `render_scale` 1.0 and 2 repetitions (local copy under `tools/cases/local/`) | 2560×1440 | `1d6f719c7339e2eac1904faab54bab7e5bad383c04eda4d3a1c3e13c5cd89bf1` |

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  ./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/bistro_metal_production_040.case.json \
  --profile tools/profiles/local-windowed-gpu-single.json
```

Run A: frame wall 18.00 ms mean, 29.84 ms p95, GPU-bound (one command-slot
wait per frame). The GPU pass sum is 18.37 ms per frame. Bistro holds 1.77 M
resident triangles (21.2 MB of 32-bit indices) and 1.75 M packed vertices;
1,788 of 2,909 candidates are visible on average.

Per-frame GPU time of the passes that rasterize scene triangles, amortized over
all measured frames (cascade and local-shadow refreshes run on a subset):

| Pass group | Run A ms/frame | Share |
|---|---|---|
| `VBuffer.Opaque` | 1.59 | 8.7% |
| `Shadow.Local.<n>` depth | 1.25 | 6.8% |
| `Shadow.Local.Transmission*` and overflow | 0.88 | 4.8% |
| `VBuffer.Transmission.0`–`.3` | 0.65 | 3.5% |
| `Shadow.Cascade.*` | 0.46 | 2.5% |

Raising the pixel count 6.25× (run B) changed `VBuffer.Opaque` from 1.59 to
1.79 ms (×1.12) and left cascade and local-shadow passes unchanged (×0.99–1.01),
while pixel-bound passes grew ×5.4–6.0 (`GBuffer.Resolve`, `Lighting.Deferred`,
`Temporal.Resolve`). Transmission peels doubled. Counting about 1.55 ms of
`VBuffer.Opaque`, half of the peels and all shadow rows, about 4.4 ms per frame,
24% of the GPU pass sum, is geometry-bound at the production scale. It does
not fall with render scale or MetalFX, so it becomes a fixed floor as
resolution drops. Individual refreshes cost 1.4–2.1 ms per cascade and
0.3–0.9 ms per local light.

Cluster culling can only remove part of this cost. Its saving is the share of
submitted triangles that a cluster-level frustum, back-face cone and HZB test
rejects; that share is not yet measured.

## Proposed change

### Step 0: bound the saving before changing formats

1. Build meshlets offline for the Bistro meshes with
   `meshopt_buildMeshlets` and `meshopt_computeMeshletBounds`, without changing
   the cooked format. For the run A camera path and its shadow views, compute
   the triangle share rejected per view by cluster frustum and cone tests, with
   the same determinant-parity and double-sided rules as the raster buckets.
2. Measure the cheaper alternative on the same case: an opaque, non-cutout
   visibility vertex variant that omits UV and color outputs and their fetches.
   If it removes most of the `VBuffer.Opaque` cost, ship it first.

Continue only if the estimated rejection would remove at least 1 ms per frame
from the geometry-bound groups above, or explicitly accept a smaller gain for
p95 refresh frames.

### Step 1: portable compute cluster culling

Use the same path on both backends, with no new device capability:

- Cook meshlets into a new compatible artifact version: local vertex remap,
  8-bit local triangle indices, and bounds (sphere, cone axis/cutoff). Publish
  meshlet rows beside geometry rows with the same generation and
  completion-gated retirement.
- After instance culling, a compute pass tests the clusters of each visible row
  against the view frustum, back-face cone (single-sided buckets only) and HZB
  under the existing ADR-028 history gates, then compacts surviving clusters per
  bucket.
- Raster the compacted clusters through the existing ICB / indirect-count path.
  The spike chooses between an instanced fixed-size cluster draw and a compacted
  index buffer.
- Deformed instances (nonzero `deformation_address`) keep candidate-level culling
  because cooked bounds do not cover their deformed positions.

### Step 2: optional mesh-shader path

Only after Step 1 is measured, add object/task plus mesh shaders behind an
explicit capability boundary:

- Metal: object and mesh shaders on Apple7+ (M1 Pro). Native indirect mesh
  dispatch requires Apple9 (M3), so M1 launches a CPU-bounded direct grid whose
  object threadgroups read GPU-written counts and exit early.
- Vulkan: optional `VK_EXT_mesh_shader`, which RDNA 2 Windows driver reports
  expose (target: RX 6700 XT; confirm on that machine). The [ADR-023](../adr/023-vulkan-1-4-bindless-capability-profile.md)
  floor does not change; devices without it keep Step 1.

Keep Step 2 only if it beats Step 1 on both target machines with matched
Release evidence. Otherwise Step 1 is the single representation.

## Decision boundaries

- Visibility identity. The G-buffer resolve, picking and transmission resolve
  read `primitive_id` against the submesh index range. Cluster raster must
  either preserve that mapping or change the visibility ID encoding in every
  consumer on both backends in the same change.
- Cluster size (for example 64 vertices / 124 triangles) and the per-triangle
  index format, chosen by the Step 0 rejection rate and memory cost.
- Whether shadow views use cone culling, given double-sided casters and the
  existing caster face policy.
- Overflow policy for compacted cluster capacity, reported like the existing
  visible-row overflow rather than silently truncating.
- Memory: meshlet rows and local indices add a fraction of the 77 MB vertex and
  index data; Step 0 reports the exact size.

## Evidence needed

- Step 0: rejection shares per view and the vertex-variant measurement, with
  commands and report digests.
- Step 1 and 2: authoritative matched Release before/after runs on
  `bistro_metal_production_040` with `tools/profiles/performance-windowed-gpu.json`
  on the M1 Pro, and a matched Bistro Vulkan case on the RX 6700 XT. Report
  `VBuffer.Opaque`, shadow, cascade-refresh and p95 frame time.
- Equivalent output: Bistro snapshot depth, visibility and picking IDs, color
  within policy, matching work-volume rows and new cluster submitted/rejected
  and overflow metrics.
- Native Vulkan execution and synchronization validation on Windows; a Metal
  run does not establish Vulkan behavior.
