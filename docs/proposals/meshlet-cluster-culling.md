---
status: proposed
updated: 2026-10-04
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

Cluster culling can only remove part of this cost: the share of submitted
triangles that a finer frustum, back-face or HZB test rejects.

## Step 0 results

**Reduced visibility vertex function (rejected).** A Metal variant that wrote
only clip position and visible row for `VBuffer.Opaque` and opaque shadow
raster kept depth, visibility primitives and all four cascade maps
bit-identical on a Bistro witness, but moved `VBuffer.Opaque` from 1.59 to
1.57 ms, local shadow depth from 1.25 to 1.21 ms and cascades from 0.46 to
0.45 ms (same case and profile, report
`32802faee24c9086d3ba0391c42b5b76ad0990ea349ef2f0a91f222c4d98ae4d`). That is
within run spread, so the cost lies in fetch, decode, transform and primitive
setup rather than surface varyings. The change was reverted.

**Offline rejection estimate.** A local estimator (cgltf plus vendored
meshoptimizer 1.2) loaded `assets/models/bistro-lights.gltf`, built
64-vertex/124-triangle meshlets (cone weight 0.25) and replayed the run A
orbit at every second measured frame (150 views). For local shadows it took the
five nearest point lights as a stand-in for importance selection and tested
their six 90° faces with the renderer's planes (`near = min(0.05, 0.01·range)`,
`far = range`; `vkr_local_shadow_system.c`), 4,500 face views. Instances are
culled by their bounding sphere per view as in the GPU classify pass; HZB is not
modeled. Blend and transmission materials are excluded, and double-sided
materials get no cone test. The estimator's 1,788.7 visible instances per
camera view match the renderer's 1,788.4.

| Culling unit | Scene candidates | Camera rejected | Local face rejected |
|---|---|---|---|
| Today: instance/submesh | 2,909 | 0 | 0 |
| Cook-time chunks of ~2,000 triangles | 3,470 | 5.1% | 40.9% |
| Cook-time chunks of ~800 triangles | 6,240 | 7.6% | 58.3% |
| Cook-time chunks of ~240 triangles | 17,730 | 9.8% | 69.7% |
| Meshlets, frustum and cone | 69,076 | 13.2% | 76.6% |

Per view, the camera submits 2.96 M triangles and a local face 0.57 M. Cluster
size barely matters: 64/64 and 128/256 meshlets give 13.7% and 11.4% for the
camera and 77.0% and 74.0% for local faces. The back-face cone rejects only
2–3% of single-sided triangles, although 32% of camera triangles face away;
Bistro's back-facing triangles are scattered across clusters rather than
grouped, so cone culling, and with it the main reason for mesh shaders, buys
little here.

Most of the reachable saving is therefore local-shadow frustum waste: large
meshes whose bounds touch a 90° face that sees a small part of them. If cost
scales with submitted triangles, local shadow depth (1.25 ms per frame) could
fall by up to 0.7–1.0 ms and `VBuffer.Opaque` (about 1.55 ms geometry-bound)
by up to 0.1–0.2 ms, before any added culling cost. These are upper bounds;
in-frustum triangles also pay rasterization. Local transmission shadows and
cascades were not estimated.

## Step 1 result: cook-time chunks rejected

The cooker split splittable static glTF ranges above 4,096 triangles into
spatial chunks of about 1,024 triangles (meshoptimizer meshlets grouped by
`meshopt_partitionClusters`). Chunks kept their source range's quantization
bounds, so packed vertices and camera depth stayed bit-identical. Bistro went
from 2,909 to 5,468 candidates, and the cook took 204 s with a 3.6 GB peak.
The change was reverted; its patch is not retained in the repository.

Matched back-to-back runs with the same binary, case and profile as run A
(unsplit report `4fc14822d60b6bd1870032fc2b66436038eeca4b330679592ce834aa134bccee`,
chunked report `87c75a0d28e5f4f5f1a4b9e2b5c5317875030ded31744f3d5e3e9c05e93aa852`):

| Per frame | Unsplit | Chunked |
|---|---|---|
| `VBuffer.Opaque` | 1.564 ms | 1.730 ms |
| `Shadow.Local.<n>` depth | 1.209 ms | 1.014 ms |
| `Shadow.Cascade.*` | 0.449 ms | 0.523 ms |
| `Cull.Encode` + `Cull.Classify` | 0.336 ms | 0.474 ms |
| GPU pass sum | 17.85 ms | 18.25 ms |
| Visible camera draws | 1,788 | 3,459 |

Local shadow depth fell 16%, not the 55% the triangle estimate implied, and
the extra draws cost more in the camera, cascade and culling passes than local
shadows saved. At this granularity per-draw cost, not submitted triangles,
bounds the geometry passes on M1 Pro.

Chunking also exposed a contract: deferred specular antialiasing, SSR, SSGI and
subsurface treat "neighbor has the same visible row" as "same surface" when
estimating normal variance (`gpu_draws.metal`, `deferred.slang`, `ssr.metal`,
`ssgi.*`, `subsurface.*`). Chunk seams follow creases, so those passes lost
their variance there and final color changed in about 3% of pixels of a Bistro
night view. Any finer-than-range draw unit must keep a surface identity those
passes can compare.

The investigation found and fixed a separate defect: cooked range extents are
absolute, but mesh-level bound unions read them as center-relative, which
shifted mesh bounds and inflated directional cascade depth ranges.

## Local shadow fill bursts

Since `818afd1d` and `9a4ce4c3`, local faces no longer redraw for texture
streaming or transmission-layer changes, so frames stay below 16 ms of GPU
while moving through Bistro ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)).
The remaining spikes follow a camera jump into an area whose lights are not
yet cached: the cache draws its face budget (High: 30) every frame until every
light is valid. In Bistro that is about 13 frames of 9–16 ms of GPU each, 17
frames above the steady state's 9–10 ms.

Measured on the M1 Pro, Release, High preset, with `VKR_RG_GPU_TIMING=1`
(which adds pass timestamps) and a 22 s Metal System Trace, in the editor
after an 8 s warm-up and a jump to eye (-2, 13, 6) looking at (-20, 6, 6):

| Observation | Result |
|---|---|
| Opaque face cost at 1×, ½× and ¼× face size (1024² faces became 256², 256² became 128²) | 0.325, 0.319, 0.316 ms per face |
| A face pass with nothing to draw | about 0.01 ms |
| Shadow raster time by stage (trace) | about 80% vertex, 20% fragment |
| Visible draws per local face during the burst | median 96 (42–198) |
| Cost per draw, local face | about 3.5 µs (0.34 ms ÷ 96) |
| Cost per draw, directional cascade | about 0.7 µs (about 2 ms ÷ 2,850 draws) |

Faces are therefore not fill- or pass-bound. A local face's draw costs five
times a cascade draw because it draws large meshes near the light whole, so
per-draw overhead is at most about a fifth of a face and the rest is vertex
work, the 77% frustum waste estimated above. Rendering a light's six faces in
one pass (vertex amplification or multiview) would remove only that fifth,
and an offline mesh-LOD estimate at a 1-texel threshold keeps 76% of a local
face's triangles, since lights sit close to what they shadow. Removing out-of-face clusters is the
lever with the most reach for these bursts, and also for the steady local
depth cost above.

## Remaining option

Compute cluster culling only pays if surviving clusters are compacted into a
few draws per bucket (for example a compacted index buffer), so draw count does
not grow with cluster count. It must also keep the range-relative
`primitive_id` mapping and the same-surface identity above on both backends.

Scope it to local shadow views first. Their depth passes write only depth, so
the visible-row identity, `primitive_id` and the same-surface contract of the
camera passes do not apply, and camera and cascade views keep today's
per-candidate draws and draw counts, which is where Step 1 lost. Per face,
cull the clusters of each visible candidate against the face frustum, compact
the surviving triangles or cluster ranges, and draw them in a bounded number
of indirect draws. Transmission faces (about a third of a burst's local cost)
can follow the same path. The reachable saving is up to about 70% of local
face depth: roughly 0.2 ms per face, or 5–7 ms of each burst frame, besides
the steady 0.7–1.0 ms above. Camera raster would follow only if the identity
work is done, for 0.1–0.2 ms. Mesh shaders remain out of scope for Bistro
(cone culling 2–3%).

Independently, faces whose view does not reach the camera frustum could wait
until it does, which shortens a burst rather than cheapening it. It requires a
light to show with some faces still undrawn, which today's rule (a light is
shadowed once every face is valid) does not allow.

Pixel-bound passes (`Lighting.Deferred`, `Shadow.LocalMask`,
`Temporal.Resolve`, tonemap and UI) remain the larger steady-state costs.

## Decision boundaries

- Whether a compacted-cluster path for local shadow views alone is worth its
  cooker data, culling pass and backend work for the burst saving, and whether
  camera views follow with their visibility-identity and ABI changes.
- Whether a light may show before all its faces are drawn, so faces outside
  the view can wait.
- Draw-count budgets: any finer culling unit must be measured against per-draw
  cost in `VBuffer.Opaque`, cascades and `Cull.*`, not only triangles.

## Evidence needed

- The camera-jump burst above: frames, GPU time per burst frame and per face,
  and draws and submitted triangles per face, before and after.
- Authoritative matched Release before/after runs on
  `bistro_metal_production_040` with `tools/profiles/performance-windowed-gpu.json`
  on the M1 Pro, and a matched Bistro Vulkan case on the RX 6700 XT. Report
  local shadow, `VBuffer.Opaque`, cascade, `Cull.*` and p95 frame time.
- Equivalent output: Bistro depth, shadow-cascade and final-color captures within
  policy, picking, matching work-volume rows and candidate overflow metrics.
- Native Vulkan execution and synchronization validation on Windows; a Metal
  run does not establish Vulkan behavior.
