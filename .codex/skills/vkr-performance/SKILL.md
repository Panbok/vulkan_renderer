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
bool8_t vkr_rg_get_pass_timings(const VkrRenderGraph *graph,
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
  `transparent_draws`, `opaque_batches`, `draws_issued`,
  `draw_calls_issued`, `batches_created`, `draws_merged`,
  `indirect_draws_issued`, `indirect_calls_issued`, `avg_batch_size`, and
  `max_batch_size`.
- `shadow` (`VkrShadowMetrics`): per-cascade opaque/alpha draw calls, set-1
  descriptor binds, opaque/alpha batch counts, and opaque indirect-command and
  indirect-call counts.

These are deterministic submission metrics; they do not depend on GPU or
driver. Hold logical work and captures constant while using them to prove that
an API-submission change reduced calls, then confirm with timing.

`draws_issued` counts logical indexed commands after CPU instancing;
`draw_calls_issued` counts the actual direct or indirect Vulkan calls.
`draws_merged` is derived from logical commands versus submitted binding-state
batches; despite its name, it does not report the earlier CPU instancing merge.
The indirect counters distinguish commands carried from MDI calls recorded.
Inspect all four when evaluating batching: a lower API-call count alone does
not prove that visibility, ordering, or logical work stayed equivalent.

`VkrVisibilityStats` (`lib/src/renderer/vkr_visibility.h`) covers the extraction
stage instead: tested/culled objects, missing bounds, opaque draws before/after
CPU merging, merge opportunities/run length, and distinct geometry/material
groups. Use those counters for culling or instancing claims, and pair them with
the frame metrics and image validation so rejected or merged work does not hide
a rendering change.

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

## Current throughput baseline

The ranked P2 plan in `docs/architecture/renderer-architecture-spec.md` §8 and
ADR-013 is shipped; ADR-013 is **Accepted (partial)** pending native Vulkan
validation. Production payload construction performs conservative camera and
union-of-cascades visibility classification. Opaque world work uses a complete
instancing key, and the world/shadow passes use bounded MDI groups with direct
fallback. `vkr_indirect_draw_alloc` is called by the shared pass submission
path.

The older `VkrDrawBatcher` module still has no production caller, but that is
not evidence that batching is absent: P2 shipped through the visibility and
pass-local batching paths. Before proposing another throughput layer, read
ADR-013's implemented behavior and measure which current boundary dominates:

1. **Visibility granularity.** Sponza's material-merged submeshes have poor
   spatial locality, while San Miguel demonstrates useful CPU culling. Improve
   content/bounds granularity only from representative measurements.
2. **Instancing compatibility.** Local reflection-probe descriptor selection is
   position-dependent and intentionally prevents unsafe merges. Preserve that
   binding-context invariant or move the state into per-instance data first.
3. **MDI group reach.** Multiple indirect commands share one call only while
   pipeline, descriptors, and vertex/index buffers remain compatible.
   Per-material descriptor sets and per-geometry buffers limit world grouping;
   shared buffers or a material table may be prerequisites for a larger win.
4. **Separate visibility domains.** Camera visibility cannot replace the union
   of shadow-cascade visibility: off-camera objects may still cast visible
   shadows.

Known hitch sources before you go hunting for a new one: synchronous upload
fence waits (§7.2), pipeline variant creation outside the prebuilt set, graph
resource recreation on description change, and readback ring wrap blocking on a
still-pending slot (§7.3).

## Reporting template

```
Change:        <what>
Config:        Release / <GPU> / <driver> / <WxH> / <scene> / <N> swapchain images /
               present <mode> / editor <on|off> / <N> cascades
Instrument:    <vkr_rg_get_pass_timings | work-volume metrics | upload wait stats | benchmark harness>
Runs:          <N>, <window> frames each
Before:        <metric> = <value> (spread <lo>–<hi>)
After:         <metric> = <value> (spread <lo>–<hi>)
Not measured:  <what this run does not cover>
Invariants:    <what still proves lifetime/ownership/completion correctness>
```

A report without the `Config` and `Not measured` lines is investigative
evidence. It can guide the next step; it cannot support a claim.
