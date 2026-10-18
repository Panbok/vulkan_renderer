---
status: implemented
updated: 2026-10-18
authority: adr
---

# ADR-028: One GPU-driven world topology

## Status

Accepted. Both pipeline classes run the GPU culling, classification and
indirect draw encoding. The visibility buffer, G-buffer resolve, deferred
lighting and transmission peels belong to the desktop pipeline, which Vulkan
implements; the tiled pipeline shades forward instead
([ADR-087](087-gpu-class-graphics-pipelines.md), decisions 5 and 6). Since
2026-10-11 the desktop pipeline gives each culling view one row per candidate
and each state bucket an exact command partition; native Vulkan runs show the
smaller tables with unchanged work counts, and no capture compared the
output (Evidence).

## Context

CPU-built world draw lists duplicate classification and make submission cost
scale with candidate count. Visibility, material evaluation and transparency
need explicit shared GPU data and graph dependencies.

## Decision

Build a bounded static-first main instance/submesh candidate stream plus a
transmission side stream, with stable identity and nonzero publication generations. Each backend packs completion-
protected GPU tables, classifies camera and cascade views (then, while dynamic
casters exist, one dynamic-caster view per cascade, ADR-041, then local
faces; the static/dynamic caster filter applies to every view), compacts state buckets
and emits native indirect commands: Metal ICBs and Vulkan indirect-count draws.
There is no CPU opaque/transmission/shadow draw fallback or legacy world selector.

In the desktop pipeline, opaque/cutout raster writes visibility identity and
depth. Compute resolves geometry/material data into the G-buffer, evaluates
GTAO, and shades HDR lighting. Separate emissive and barycentric/LOD debug
images exist only when their capture channels are requested. Four native
resolve variants compile out unrequested writes; temporal, normal,
albedo/specular and scene-color outputs remain present. Capture-enabled
variants retain initialized background and invalid-surface values.
Depth-writing visibility and picking passes use strict greater-than tests on
reversed camera depth and shadow passes strict less-than tests on forward
depth ([ADR-109](109-reversed-camera-depth.md)); depth-read-only blend and
text accept equal depth. Culling sphere scale
uses the conservative bound `sqrt(||A^T A||_infinity)`, prepared once per
native instance into `normal_column1.w`. The bound preserves sheared affine
transforms; CPU blend/caster bounds use the same helper. HZB reductions keep each cell's farthest depth, the minimum under reversed
depth, and include an unpaired source row or column in the last destination
texel. The
desktop graph also builds HZB history, handles visibility-based picking, and
schedules ADR-018's four transmission peels. Compacted transmission shading
retains a fullscreen diagnostic launch shape, not a second world renderer.

Ordinary alpha blend is a narrow CPU exception: conservative camera culling and
back-to-front sorting produce feature-local direct draws. UI and text have their
own streams. Shadow candidates are not derived from the camera-culled blend list.

Opaque/cutout and single/double-sided buckets are partitioned by determinant
parity, for eight buckets. Native front-face state follows each partition.
Direct blend draws split contiguous instance parity runs while preserving
source order. Existing material bucket metrics sum both parity partitions.
Nearly singular transforms keep the existing prepared normal-basis fallback; a
negative determinant selects reflected winding. `VKR_FRUSTUM_DISABLED=1` zeros
packed native frustum planes for reference captures without adding
per-candidate validation.

In the desktop pipeline each culling view owns `C` rows of the
classification, visible-row and indirect-command tables, `C` being the
candidate capacity, because a view classifies each candidate at most once. A
candidate's state bucket is fixed when it is packed, so the host counts the
packed candidates of each bucket and gives the bucket a command partition of
exactly that size in every view's rows, in bucket order
(`VkrVulkanCommandPartitions`, the last 64 bytes of the 272-byte
`VkrVulkanCullRoot`). The prefix pass clamps each bucket to its partition and
encoding writes command `view * C + first + local index`; raster takes the
partition's first row for its argument offset and its size for
`maxDrawCount`, and an empty bucket records no draw. Static bucket counts
belong to the static residency they were counted for, and a frame whose
committed residency differs repacks the static rows
([`vkr_vulkan_draws.c`](../../renderer/src/vulkan/vkr_vulkan_draws.c)).
`vkr_candidate_residency_static_repack_reasons`
([`vkr_candidate_residency.h`](../../renderer/src/vkr_candidate_residency.h))
names the stale inputs, and the packet metrics report them with the
publications that moved the publication generation
(`packet.static_repack_reasons`, `packet.publication_changes`). A
disagreement between counts and rows would show as an overflow count, not as
a write outside the partition. The transmission stream partitions its own
candidates for its one view. The tiled pipeline keeps a stride of eight
times the smaller of `C` and 65,536 rows per view
([ADR-002](002-render-graph.md)) and packs its buckets into it one after
another, so a bucket never overflows.

HZB rejection requires completed compatible history, matching world epoch and
unjittered view-projection and the exact raster projection that produced depth;
camera motion cannot silently relax those gates. Vulkan maps NDC Y directly to
its positive-height framebuffer; Metal keeps its native clip conversion. Different
jitter phases do not prove current-sample coverage. The graph skips HZB construction
when raster and unjittered projections differ, or HZB is explicitly disabled.
Both culling passes declare history reads only under that same enable condition;
disabled history has neither a producer nor a declared consumer in the graph.
History rejection metrics distinguish disabled, invalid, incomplete, world,
extent, camera and raster-grid failures. Candidate
capacity is checked before recording. Completion-gated GPU diagnostics expose
visible/bucket/overflow/resolve-invalid counts; overflow is not permission to
silently claim a complete frame. Resolve-invalid counts degenerate triangles and
non-finite normal frames. The G-buffer and transmission resolves read
visible rows, primitives and vertices without bounds checks: encode assigns
rows below the visible count, raster emits only the draw's primitives,
publication resolves geometry/material/instance rows and mesh loading checks
indices. A malformed row is therefore undefined behavior, not a rejected pixel.

Metal batches ICB resets before a device-visible blit-to-dispatch intra-pass
barrier. Command-generation-to-indirect-execution dependencies remain separate.

## Consequences

World work has one topology and one representation. Indirect submission and
visibility resolve require ABI and native GPU evidence. Exact HZB history gates
limit reuse under motion. A GPU-driven topology does not imply mesh shaders,
meshlets or automatic LOD; ranges with detail levels select them during
culling ([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md)).
Desktop culling tables take 56 bytes per candidate and culling view in each
frame slot; up to 65,536 candidates that is an eighth of the eight buckets
of `min(C, 65536)` rows they replaced on 2026-10-11. They are `GROW_ONLY`
(ADR-002), so a session keeps the backing of its largest view count.

## Alternatives considered

The CPU batching/MDI plan in former ADR-013 was replaced. Weighted OIT does not
preserve ordered refraction. ADR-032 declined a second visibility phase for its
measured workload.

## Revisit when

Candidate scale, deforming geometry or measured visibility cost justifies a new
classification policy without weakening visibility correctness. A session's
peak culling view count holds enough memory that the culling tables should
shrink after a lower peak.

## Implementation

[`vkr_visibility.c`](../../renderer/src/vkr_visibility.c),
[`vkr_gpu_abi.h`](../../renderer/src/vkr_gpu_abi.h),
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c),
[`vkr_metal_packet_commands.inc`](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc),
and [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).

## Evidence

Culling rows per candidate (2026-10-11) ran on native Vulkan on 2026-10-12:
Release, RX 6700 XT, driver 26.6.3, Windows 10 19045, Ryzen 5 2600, a dirty
tree on `4eaeecbb` and local profiles, so no result is authoritative.
Harness runs used
`build_release/tools/vkr_harness.exe snapshot|profile --case tools/cases/local/<case>.case.json --profile tools/profiles/<profile>.json`
with the graphics validation variables unset.

- Builds before and after the change set alternated under
  `local-offscreen-perf-audit-gpu` (before, after, after again on a rebuild;
  two or five children each). `memory.gpu.owner.render_graph.bytes.live`,
  constant within each run, fell from 789.66 to 655.87 MiB in
  `ssr_tier_bistro_cost` (640x360), from 1,454.70 to 1,320.90 MiB in
  `ssgi_bistro_on_profile_local` (1280x720; both cases were deleted with SSR
  and SSGI), from 3,977.66 to 3,678.50 MiB in
  `sparse_volume_bistro_street_on_timing` (2560x1440) and from 2,111.32 to
  1,536.53 MiB in `local_shadow_bistro_vulkan_walk_ultra` (1280x720).
  Render-graph images did not change, so the whole fall is in buffers. Every
  `draw.*` and `visibility.*` row matched in all phases, and `Cull.*` timing
  stayed within spread.
- `local_shadow_bistro_vulkan_street_capture` (snapshot, `local-offscreen`,
  `sha256:d4418437aae0fc15429d36f97f4742d1b03f49007bc85030aa0dbe78828606d0`)
  and `local_shadow_bistro_vulkan_walk_ultra` (profile, `local-offscreen`,
  `sha256:c9763acdab237627e15866ad31fd241ce5a70bb200113bad4795970350cf98cf`)
  reported no visible-row, transmission, compact-pixel or cascade overflow.
- A copy of `ssr_ghost_under_bar` (deleted with SSR) with longer timeouts,
  while SSR was still in the frame, ran under
  `local-offscreen-gpu-single` with `VK_LAYER_KHRONOS_validation`, its core
  and synchronization checks enabled by a layer settings file, and logged no
  message
  (`sha256:4c08ff5cb67021c8df1e94609156bf05e5486a33b681e4c45d6547187f6e971b`).
  No layer status line confirmed that the synchronization checks ran.
- The Testbed project's "Level Design Test" scene is not a harness case. In a
  headless Release editor, against single runs of 2026-10-11 rather than a
  matched alternation, its render graph took 1,762 MiB instead of 4,940 MiB at
  the default layout and 2,449 MiB instead of 5,626 MiB with the Scene view
  maximized, and the Draws window showed graph buffers at 422.69 MB instead
  of 3,105.44 MB at the lobby. These figures are indicative. They are not
  harness reports.

`test_draw_table_capacity_and_json_sizes` in
[`render_graph_barrier_test.c`](../../tests/src/render_graph_barrier_test.c)
expects a 20-byte table of 240 bytes for a capacity of four candidates and
three culling views; a source that ignores the views (80 bytes) or keeps the
bucket stride (1,920 bytes) fails it. No depth or G-buffer capture compared
the output before and after the change, and the Metal tables are unchanged.
