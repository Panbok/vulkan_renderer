---
status: investigation
updated: 2026-08-01
authority: investigation
---
# Renderer Metrics Phase 1b Verification

This record verifies phase 1b of
[renderer-harness-and-metrics-spec.md](renderer-harness-and-metrics-spec.md):
explicit logical GPU allocation owners and their metrics aggregates. It is not
evidence that device-memory pooling or phases 2-6 ship.

## Configuration

- Source base: `038a74951d6cbaa05b608342db6c4fe7f02d3c4e`, dirty with the
  phase-1b implementation under test, as revised by the post-implementation
  review and counter-schema correction described in
  [Reviewed shape](#reviewed-shape). The gates below state whether they cover
  the complete corrected schema or the preceding resource-owner refactor.
- Build: Release, Apple Clang 21.0.0, C11, `-march=native`; the paired binaries
  differed only by `VKR_METRICS_ENABLED=0` versus `1`.
- Binary SHA-256: disabled
  `2ac3ae7a0484a3b8826877c04a146e568f32b357bdd0fcc9d23859a91d67e489`;
  enabled
  `d7e90d51fc33f2f39fecd1716dc2cbdd2b93293b504ca8bcc9514cb0d9eecb09`.
- Host: MacBookPro18,3, Apple M1 Pro, 16 GiB, macOS 26.5.2 build 25F84.
- Vulkan: Apple M1 Pro through MoltenVK driver 0.2.2019, Vulkan 1.2.296,
  Vulkan SDK 1.4.313.0.
- Timing workload: sample application's default one-cube scene and fixed
  camera, 800x600 logical / 1600x1200 framebuffer pixels, windowed target,
  three images and three frames in flight, frame limiter disabled, warm shared
  pipeline cache, GPU pass timing and event subjects disabled.

## Implemented boundary

`VkrGpuAllocationOwner` is part of buffer, texture, and render-target creation
descriptions. Production constructors declare one of eleven fixed buckets:
`unknown`, `mesh`, `texture`, `font`, `render_graph`, `shader`, `instance`,
`indirect`, `staging`, `readback`, or `swapchain`. Buffer resize retains the
original owner. Images pass the declared owner to the centralized allocation
wrapper. Backend-only staging, readback, and swapchain allocations use explicit
fixed owners.

The live `VkDeviceMemory` table stores the owner beside handle, size, and memory
type. Allocation and free update per-owner live/peak/total bytes and allocation
counts with no name lookup, inferred classification, allocation, or lock. A bad
enum value is normalized to the reportable `unknown` bucket. Saturation retains
the existing `live_totals_exact=false` contract; owner live/peak rows are marked
inexact while cumulative totals remain available.

The renderer registry pre-registers 66 stable rows:
`memory.gpu.owner.<owner>.bytes.{live,peak,allocated}` and
`memory.gpu.owner.<owner>.allocations.{live,peak,created}`. Live/peak rows are
gauges. The backend's lifetime totals remain the authoritative source for
tracker diagnostics, while the adapter publishes their differences as
per-frame counters. The rows are fixed cardinality and leave the device-
dependent memory-type/heap rows to consume the remaining catalog capacity.

## Reviewed shape

A post-implementation review restructured the code without changing a published
name, value, or unit. The report contract above is what the runtime gates below
confirm; these are the seams a later phase inherits:

- `VkrGpuAllocationOwnerTotals` is one struct per owner, held as an array in
  both the backend tracker and the public `VkrDeviceMemoryStats`. The six
  parallel per-statistic arrays each struct previously carried are gone, so an
  allocation touches one cache line, the snapshot copy is a single `MemCopy`,
  and a new statistic is one field rather than one array per struct plus a copy.
- `vkr_gpu_allocation_owner_normalize()` is the single out-of-range coercion,
  used by both the allocate and free paths.
- Owner rows are table-driven. `VkrGpuOwnerMetricRow` names the row set once;
  a file-local description table owns each row's name suffix, kind, unit, and
  whether the handle table's exactness applies to it. Registration and
  collection loop over that table instead of repeating the row set in a macro
  and again in the inexactness marking, which is what previously made it
  possible to add a row and forget its mark.
- A post-review schema correction removed running-total gauges. The aggregate
  allocation count and both per-owner cumulative sources retain last-seen
  baselines and publish interval counters. A failed device-memory pull marks
  those counters unavailable; the next successful pull refreshes baselines but
  remains unavailable rather than absorbing work from the missing interval.
- The owner name table uses designated initializers and a `_Static_assert` on
  the bucket count, so a new enumerator fails the build rather than leaving a
  `NULL` that only `snprintf` would find.
- `vulkan_image_create()` takes a `VulkanImageDescription` instead of fifteen
  positional arguments. The mip/layer pair and four adjacent enums were
  transposable without a diagnostic; all twelve call sites now name their
  fields.
- `renderer_vulkan_resize_texture()` reads the owner retained on the live
  `VulkanImage` rather than a parallel copy in the texture description, which
  matches `vulkan_buffer_resize()` and removes a field that was written but
  never read.

## Functional and correctness gates

- `./build_test.sh`: exit 0 across every registered suite. Deterministic tests
  cover two colliding handles, deletion/reinsertion, mixed mesh/font
  attribution, live/peak/total bytes and counts, invalid-owner normalization,
  table saturation/inexactness, cumulative first/subsequent/reset deltas, the
  aggregate allocation-created counter, and all 66 owner rows by name, unit,
  kind, scalar, and domain.
- Source audit: one raw `vkAllocateMemory` call and one raw `vkFreeMemory` call
  remain, both inside the tracked wrapper. Every production buffer/texture
  description constructor declares an owner. No classification reads debug
  names, usage flags, or memory types.
- `./build.sh Debug`: exit 0. The warnings were the pre-existing
  `vkr_dmemory.c` C23-label warning and `vkr_rg_debug.c` const-qualification
  warning.
- Corrected-schema Debug validation run: exit 0 after four seconds with
  validation layers active and `METRICS_JSON status=pass`. The 222-row artifact
  contained all 66 owner rows as 44 gauges plus 22 counters, the aggregate
  `memory.gpu.allocations.created` counter, zero old `.total` metric rows, no
  missing required metrics or snapshot/event drops, and a pass table matching
  the snapshot. The final settled frame correctly reported zero allocation
  work in its interval rather than a process-running total.
- Debug validation-layer Sponza run: exit 0 after 45 seconds; scene ready in
  27.171 seconds, no validation messages, no upload waits, and
  `METRICS_JSON status=pass`. This is the gate that covers the twelve
  `vulkan_image_create()` call sites the review rewrote: a transposed extent,
  mip, layer, sample, view-type, or aspect argument would surface here, and the
  CPU suite would not see it.
- Release Sponza run: exit 0 after 14 seconds; `boot.scene` was 4.100 seconds,
  no upload waits, and `METRICS_JSON status=pass`.
- The earlier Sponza artifacts used the superseded absolute-total gauge schema;
  they remain tracker-attribution evidence, not current catalog evidence. Each
  reported 207 live allocations and 2,380,889,280 live bytes, exactly equal to
  the sum of all owner live rows; the backend's 350 cumulative allocations
  likewise equalled its owner totals. Loaded-scene values reproduced the
  pre-review implementation exactly: mesh 50 /
  268,039,576 bytes; texture 98 / 1,920,805,632; font 10 / 55,924,040; render
  graph 3 / 100,663,296; shader 39 / 11,065,056; instance 3 / 15,728,640;
  indirect 3 / 983,040; swapchain 1 / 7,680,000 live against 15,360,000
  cumulative across one recreation. `unknown` and `readback` were zero live, and
  transient staging returned to zero live after peaking at 175,938,744 bytes.
  Every owner row reported `availability=valid`.
- `VKR_METRICS_ENABLED=OFF ./build_release.sh`: exit 0.
- `VKR_SKIP_BUILD=1 ./validate_pipeline_cache.sh`: exit 0. Cold and warm runs
  each published 23 pipeline events; summed creation time was 51.034 ms cold
  and 15.789 ms warm.

Generated JSON remains under `/tmp` in the local validation workspace; it is
not a baseline and is not committed.

## Overhead gate

Five isolated pairs used alternating BA/AB order. Each process ran for six
seconds. `BENCHMARK_SAMPLE` publishes interval means; the first interval was
discarded. Delta is `(enabled / disabled - 1) * 100`, so positive is a
regression.

| Block | Order | Disabled mean ms | Enabled mean ms | Paired delta |
|---:|:---:|---:|---:|---:|
| 1 | BA | 8.335476 | 8.434571 | +1.188834% |
| 2 | AB | 8.336524 | 8.323476 | -0.156516% |
| 3 | BA | 8.329619 | 8.324286 | -0.064025% |
| 4 | AB | 8.338905 | 8.308619 | -0.363189% |
| 5 | BA | 8.326524 | 8.342524 | +0.192157% |

Median paired delta is **-0.064025%**; observed range is -0.363189% to
+1.188834%. The median clears the 1% gate and does not support a central
regression on this workload; the positive first-pair outlier is retained and
prevents the stronger claim that every isolated pair stayed below 1%. The
spread straddles zero, so the negative median is not evidence that metrics made
rendering faster.

The comparison isolates the per-frame metrics collector, including the 66 owner
rows. It does not isolate the review refactor, because that refactor is present
in both variants — the disabled binary compiles the collector out but keeps the
tracker. Owner bookkeeping runs only on device allocation/free cold paths, so
neither this table nor its predecessor makes any frame-time claim about those
operations, and the improved locality of the per-owner struct is a
code-structure argument rather than a measured one.
