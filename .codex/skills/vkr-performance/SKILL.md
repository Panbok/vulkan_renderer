---
name: vkr-performance
description: The measured-performance workflow for the VKR renderer. Use when investigating frame-time or hitch regressions, optimizing a renderer hot path, adding or reading per-pass timings, comparing before/after numbers, running the backend benchmark harness, or validating any claim that a change made the renderer faster or slower.
---

# VKR Performance

## Why this skill exists

**Performance is correctness here.** A frame that misses its budget is a failed
frame, and a per-draw allocation, blocking wait, string construction, or lock is
a defect rather than a style preference.

That principle is only useful with its counterweight: **an unmeasured
performance claim is not a result.** This skill defines what counts as evidence
in this repository, which instruments exist, and what they do and do not prove.

This project has **no Tracy, no external profiler integration, and no dedicated
benchmark binary.** Do not write instructions or reports that assume otherwise.
Everything below is a real, present API or script.

## Instruments that exist

### Per-pass CPU and GPU time

```c
bool8_t vkr_rg_get_timings(const VkrRenderGraph *graph,
                           const VkrRgPassTiming **out_timings,
                           uint32_t *out_count);
```

`VkrRgPassTiming` (`lib/src/renderer/vkr_render_graph.h`) carries `cpu_ms`,
`gpu_ms`, and `gpu_valid`. **`gpu_ms` is meaningless unless `gpu_valid` is
true**, and it reflects the *last completed frame*, not the current one —
GPU results are buffered. Never report a `gpu_ms` without checking `gpu_valid`.

The backend timestamp path is `vkr_renderer_rg_timing_begin_frame` /
`_begin_pass` / `_end_pass` / `_get_results` (`vkr_renderer.h`). It requires
device timestamp support; absence is an unavailable instrument, not a
regression.

### Frame work-volume metrics

`VkrRendererFrameMetrics` is an **out-parameter of
`vkr_renderer_submit_packet()`**, not a separate getter, and it counts *work*,
not time:

- `world` (`VkrWorldBatchMetrics`): `draws_collected`, `opaque_draws`,
  `transparent_draws`, `opaque_batches`, `draws_issued`, `batches_created`,
  `draws_merged`, `indirect_draws_issued`, `avg_batch_size`, `max_batch_size`.
- `shadow` (`VkrShadowMetrics`): per-cascade opaque/alpha draw calls, set-1
  descriptor binds, and opaque/alpha batch counts.

These are the right numbers for throughput work, because they change
deterministically and do not depend on GPU or driver. Use them to prove a
batching or culling change did what you claim — `draws_issued` falling while
`draws_collected` holds is real evidence — then confirm with timing.

Note that `draws_merged` is currently hardcoded to zero in
`passes/vkr_pass_world.c` and `batches_created` mirrors the opaque count. Merging
is not implemented, so a nonzero `draws_merged` means someone wired step 2 below.

### Upload stalls

```c
bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRendererFrontendHandle renderer, VkrRendererUploadWaitStats *out_stats);
```

Uploads currently submit and wait on a fence (architecture spec §7.2), so this
is the first place to look for a hitch during scene load or streaming. The call
**resets** the counters — read it once per sample window, not in a loop.

### Graph resource pressure

`vkr_rg_get_resource_stats()` and `vkr_rg_log_resource_stats(graph, label)` —
live and peak graph-owned resource counts and sizes. Rising peak across frames
means the graph is recreating resources, which is a hitch source.

### CPU memory

`vkr_allocator_print_global_statistics()` — tagged local/global totals. Read
`vkr-memory` before interpreting these; bulk `arena_destroy()` does not
decrement global counters without
`vkr_allocator_release_global_accounting()`, so apparent growth may be an
accounting artifact rather than a leak.

### Instance stream occupancy

`VkrInstanceBufferPool` has a fixed 65,536-instance capacity and reports
overflow. Occupancy near capacity, or any overflow report, is a signal — the
pass drops work rather than growing.

## Harness

```sh
# Release build + timed cases, writes summary.csv
tools/benchmark_multithreaded_backend.sh

# Fast subset
tools/benchmark_multithreaded_backend.sh --smoke
```

Output lands in `build/_validation/multithreaded_backend/perf/` —
`summary.csv` plus per-case logs under `logs/`.

Environment overrides: `VKR_BENCH_BUILD_TYPE` (default `Release`),
`VKR_BENCH_FORCE_BUILD`, `VKR_BENCH_SKIP_BUILD`, `VKR_BENCH_AUTOCLOSE_SECONDS`
(default 8), `VKR_BENCH_MAX_WAIT_SECONDS` (default 45).

Release runs use `./build_release.sh` and `build_release/app/vulkan_renderer`;
Debug and RelWithDebInfo use `./build.sh <type>` and `build/app/vulkan_renderer`.

Correctness is a separate gate — see `vkr-validation`. A faster frame that fails
the validation layer is not an improvement.

## Evidence policy

**Release only.** Debug timings are not evidence. Debug builds carry different
inlining, assertion, and validation-layer costs, and Debug-to-Debug comparison
does not predict Release behavior.

**Same configuration or the comparison is void.** Record and match all of:

| Field | Why it matters |
|---|---|
| Build type and compiler | Inlining and assertion cost |
| GPU and driver version | Everything |
| Resolution | Fill-bound passes scale with it |
| Scene / asset set | Draw count, material count, texture residency |
| Swapchain image count | Frame-stream indexing behaves differently (see gap below) |
| Present mode | FIFO caps at refresh rate and hides CPU wins |
| Editor enabled | Changes graph topology — the editor composite is an alternative pass, not additive |
| Cascade count | Changes the shadow repeat expansion |

**Report what you did not measure.** State the instrument, the sample window,
the number of runs, and the variance. One run is an observation, not a result.

**Present mode masks CPU improvements.** If the app is FIFO-locked at refresh
rate, `cpu.frame` improvements will not show up in frame time. Measure the CPU
submit path directly rather than concluding "no change".

**Timestamps perturb.** Enabling per-pass GPU timestamps adds queries to the
command stream. Compare timestamp-on against timestamp-on, never against a
timestamp-off baseline.

## Optimization order

When the goal is throughput rather than a specific regression, follow the ranked
plan in `docs/architecture/renderer-architecture-spec.md` §8 P2 and ADR-013
(**Proposed** — read it before adding a caller):

1. **Cull before materializing world payloads.** `vkr_frustum` exists and is
   tested but has no production call site. Bounds must be correctly transformed
   — non-uniform scale requires conservatively expanding spheres or transforming
   AABBs.
2. **Real instancing next.** Consecutive draws sharing pipeline, material,
   geometry buffers, and index range collapse into one indexed draw with
   `instance_count > 1` when the instance records are contiguous.
   `VkrDrawBatcher` exists and is tested but is unwired.
3. **MDI only for meaningful binding-state groups.** Multiple indirect commands
   share one call only while pipeline, descriptors, and vertex/index buffers stay
   compatible. Per-material descriptor sets and per-geometry buffers limit
   grouping today — bindless or material tables and shared mega-buffers may be
   prerequisites for a large win. `vkr_indirect_draw_alloc` has no caller.
4. **Keep camera and shadow visibility separate.** A camera-culled list cannot be
   reused for CSM: off-camera objects still cast visible shadows.

Known hitch sources before you go hunting for a new one: synchronous upload
fence waits (§7.2), pipeline variant creation outside the prebuilt set, graph
resource recreation on description change, and readback ring wrap blocking on a
still-pending slot (§7.3).

## Reporting template

```
Change:        <what>
Config:        Release / <GPU> / <driver> / <WxH> / <scene> / <N> swapchain images /
               present <mode> / editor <on|off> / <N> cascades
Instrument:    <vkr_rg_get_timings | work-volume metrics | upload wait stats | benchmark harness>
Runs:          <N>, <window> frames each
Before:        <metric> = <value> (spread <lo>–<hi>)
After:         <metric> = <value> (spread <lo>–<hi>)
Not measured:  <what this run does not cover>
Invariants:    <what still proves lifetime/ownership/completion correctness>
```

A report without the `Config` and `Not measured` lines is investigative
evidence. It can guide the next step; it cannot support a claim.
