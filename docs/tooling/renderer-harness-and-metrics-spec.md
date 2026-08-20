---
status: implemented
updated: 2026-08-14
authority: spec
---
# Renderer Metrics Module and Automation Harness

> **Status note.** Phases 1, 1b, 2, 2b, 3, 4, 5, and 6 ship: the centralized registry and
> renderer adapter feed a structured `vkr_harness profile` runtime with strict
> manifests, deterministic cameras, isolated repetitions, fingerprints, and
> atomic reports. Reviewed CPU/GPU performance profiles replace the retired
> grep/awk benchmark, and dependency-resolved automation boot reports its actual
> subsystem mask. `vkr_harness snapshot` now publishes canonical direct color,
> depth, shadow-layer, and picking-ID captures through a declared graph pass and
> bounded asynchronous ring. Phase 5 adds auxiliary forward-render debug
> replays, canonical comparison and diffs, combined `autotest`, and
> profile-scoped immutable baseline generations with digest-confirmed guarded
> promotion. Phase 6 adds target-neutral attachment/state queries and a true
> ordinary-image offscreen target that creates no window, surface, swapchain, or
> present queue.
> Evidence is recorded in
> [renderer-metrics-phase1-verification.md](renderer-metrics-phase1-verification.md)
> and
> [renderer-metrics-phase1b-verification.md](renderer-metrics-phase1b-verification.md).
> Phase-2 and Phase-2b evidence is recorded in
> [renderer-harness-phase2-verification.md](renderer-harness-phase2-verification.md)
> and
> [renderer-harness-phase2b-verification.md](renderer-harness-phase2b-verification.md).
> Phase-3 evidence is recorded in
> [renderer-harness-phase3-verification.md](renderer-harness-phase3-verification.md).
> Phase-4 evidence is recorded in
> [renderer-harness-phase4-verification.md](renderer-harness-phase4-verification.md).
> Phase-5 evidence is recorded in
> [renderer-harness-phase5-verification.md](renderer-harness-phase5-verification.md).
> Phase-6 evidence is recorded in
> [renderer-harness-phase6-verification.md](renderer-harness-phase6-verification.md).

## 1. Problem

The renderer already produced most of the numbers an automated workflow needed,
but lacked a machine-readable transport. Phases 1-6 implement that transport,
replace the legacy scrape path, add dependency-resolved boot, and add direct
and auxiliary capture, comparison, autotest orchestration, and guarded baseline
promotion. The final phase removes WSI from offscreen execution while retaining
the same graph and packet path used by the application.

Before this series, every signal lived in its own struct with its own reader,
and the only transport was `log_info` plus a shell pipeline:

- The former `tools/benchmark_multithreaded_backend.sh` launched the app with
  `VKR_BENCHMARK_LOG=1`, greps a single `BENCHMARK_SUMMARY` line out of stdout,
  and splits it with `awk` into an eight-column CSV. Adding a metric means
  editing a `snprintf`, a `grep`, and an `awk` field index in three files.
- `app/src/main.c` carried benchmark accumulation inline with the editor, gizmo,
  and picking demo code: `VKR_AUTOCLOSE_SECONDS`, `VKR_METRICS_INTERVAL_SECONDS`,
  `VKR_BENCHMARK_LOG`, `VKR_BENCHMARK_LABEL`, `VKR_RG_GPU_TIMING`,
  `VKR_ASSERT_NO_UPLOAD_WAITS`, `VKR_AUTOLOAD_SCENE`, `VKR_SCENE_MEM_VERBOSE`.
  Eight environment knobs feeding hand-rolled accumulators.
- Image capture was limited to **one pixel** — the picking readback ring. There
  was no scripted camera, no asset-load or pipeline-creation timing that
  survived the process, and no way to run without a visible window.

`.codex/skills/vkr-performance/SKILL.md` states the governing rule — *an
unmeasured performance claim is not a result*. Phase 0 corrected its invalid
timing symbol and pre-P2 batching/status guidance; Phase 2b now routes that skill
through the structured harness after establishing metric/pass/workload parity.

**Goal:** one bounded publication contract every subsystem can feed, and one
binary that reads it, so an agent can run a case, obtain a structured report,
compare compatible evidence, and reach a verdict without a human watching a
window. Baseline promotion remains an explicit reviewed mutation; ordinary
runs and comparisons do not need human supervision.

## 2. What already exists

This work is mostly consolidation. Reuse these; do not rebuild them.

| Capability | Symbol / location |
|---|---|
| Per-pass CPU ms, GPU ms, `gpu_valid` | `vkr_rg_get_pass_timings()`, `VkrRgPassTiming` |
| GPU timestamp query plumbing | `rg_timing_begin_frame` / `_begin_pass` / `_end_pass` / `_get_results` in `VkrRendererBackendInterface` |
| Draw and batch work volume | `VkrRendererFrameMetrics` (out-parameter of `vkr_renderer_submit_packet()`) |
| Frustum-culling counters | `VkrVisibilityStats`, `renderer/vkr_visibility.h` |
| Device memory: live/peak/total allocations, per-type, per-heap, budget | `vkr_renderer_get_device_memory_stats()`, `VkrDeviceMemoryStats` |
| CPU allocator statistics, 14 memory tags, scope tracking | `vkr_allocator_get_statistics()`, `VkrAllocatorStatistics` |
| Graph resource live/peak counts and bytes | `vkr_rg_get_resource_stats()` |
| Upload stall counters | `vkr_renderer_get_and_reset_upload_wait_stats()` |
| Bounded command-slot reuse waits | `vkr_renderer_get_and_reset_command_slot_wait_count()` |
| Pipeline creation time (measured, then discarded into a log line) | `vulkan_graphics_graphics_pipeline_create()` in `vulkan/vulkan_pipeline.c` |
| Pipeline bind and descriptor telemetry | `VkrPipelineRegistry.stats` |
| Image→buffer copy with checked mip/layer/aspect regions and row layout | `vulkan_image_copy_to_buffer_region()` |
| Completion-gated readback ring | `vkr_capture_ring` shared by the Metal and bindless Vulkan implementations |
| A **declared** graph readback pass — the pattern to copy | `Picking.Readback` in `assets/render_graphs/main.rendergraph.json`, lowered privately by each selected implementation |
| Debug channels: normals, unlit, lighting | `VkrRenderMode`, consumed by the production Metal and bindless Vulkan packet shaders |
| Shadow debug: cascades, factor, depth | `RendererFrontend.shadow_debug_mode` packetized through `VkrGpuDebugPayload` and frame constants |
| Offscreen editor color target | conditional graph resource `scene_color`, realized by the selected implementation |
| Monotonic timer | `vkr_platform_get_absolute_time()` |

### 2.1 Backend topology and deferred targets

Metal and Windows Vulkan now default to the deferred visibility-buffer
topology; explicit `VKR_DEFERRED_ENABLED=0` retains forward only for diagnosis
until the separately authorized P21 retirement. Harness
provenance and effective configuration publish `world_renderer`, and that value
enters the environment fingerprint so evidence from the two topologies cannot
be compared under one identity.

Both backends realize opaque/transmission visibility attachments and
material-resolve G-buffer targets. Their direct channels are diagnostic evidence
for the migration. `normals`, `unlit`, lighting-only, visibility,
G-buffer, emissive, and resolve-debug channels are captured by replaying a
settled frame with the corresponding render mode or direct graph resource.
Shadow replay modes 1–3 are likewise packet data, not harness-only globals:
Metal deferred opaque/transmission and diagnostic forward emit cascade
selection, comparison factor, or sampled map depth from the lighting shadow
sample. A requested shadow mode therefore changes the replayed image while
mode zero retains ordinary final color.

Existing telemetry is not uniformly safe to pull. Per-allocator
`vkr_allocator_get_statistics()` reads non-atomic fields and cannot be sampled
concurrently with that allocator's owner. GPU pass results refer to a completed
earlier submission and currently lack an exported source frame/submit serial.
Capture capability remains format/surface dependent. Catalog preflight and
backend initialization reject unsupported color/depth transfer sources rather
than letting the registry assume they exist.

## 3. `VkrMetrics` — the centralized module

**Locations:** `lib/src/core/vkr_metrics.{h,c}` owns generic storage and
publication; `lib/src/renderer/vkr_renderer_metrics.{h,c}` owns renderer-specific
registration and pulling. Core must not include the private `RendererFrontend`
representation merely because the renderer is one producer.

### 3.1 Shape

A registry of pre-declared typed slots addressed by a 32-bit handle containing
a `uint16_t` slot index and `uint16_t` registry generation. Catalog capacity is
sized so the fixed set plus the worst-case device-dependent rows (two per
memory type, three per heap, seven per shadow cascade) always fit; running out
of slots degrades the report and is never allowed to fail startup, because a
renderer that will not boot on an unusual memory layout is worse than a report
missing per-heap rows.
`Application` owns the registry and passes it to subsystems during
initialization; there is no process-global instance. Registration copies names
into registry-owned storage, rejects duplicates/exhaustion, and ends with an
explicit seal. The hot path performs an indexed operation. No hashing, name
lookup, string construction, allocation, mutex, or recoverable kind check occurs
per draw or per pass.

```c
typedef uint32_t VkrMetricId; /**< generation:16 | slot:16 */
typedef struct VkrMetricsFrame VkrMetricsFrame;

typedef struct VkrMetricsSnapshotView {
  const VkrMetricsFrame *frame;
  uint32_t buffer_index;
  uint64_t publication_serial;
} VkrMetricsSnapshotView;

typedef enum VkrMetricKind {
  VKR_METRIC_KIND_COUNTER,  /**< Interval u64; cumulative sources are differenced. */
  VKR_METRIC_KIND_GAUGE,    /**< Instantaneous u64 or f64. */
  VKR_METRIC_KIND_DURATION, /**< Integer ns; aggregate shape depends on writer. */
} VkrMetricKind;

typedef enum VkrMetricDomain {
  VKR_METRIC_DOMAIN_FRAME,
  VKR_METRIC_DOMAIN_RENDERGRAPH,
  VKR_METRIC_DOMAIN_DRAW,
  VKR_METRIC_DOMAIN_MEMORY_CPU,
  VKR_METRIC_DOMAIN_MEMORY_GPU,
  VKR_METRIC_DOMAIN_ASSET,
  VKR_METRIC_DOMAIN_PIPELINE,
  VKR_METRIC_DOMAIN_JOB,
  VKR_METRIC_DOMAIN_UPLOAD,
  VKR_METRIC_DOMAIN_BOOT,

  VKR_METRIC_DOMAIN_COUNT,
} VkrMetricDomain;

typedef enum VkrMetricWriter {
  VKR_METRIC_WRITER_RENDER_THREAD,
  VKR_METRIC_WRITER_CONCURRENT,
} VkrMetricWriter;

typedef struct VkrMetricDescription {
  String8 name; /**< Stable lowercase dotted name. */
  VkrMetricDomain domain;
  VkrMetricKind kind;
  VkrMetricUnit unit;       /**< Enum: count, bytes, ns, ratio, and so on. */
  VkrMetricScalar scalar;   /**< u64 or f64; counters/durations are u64. */
  VkrMetricWriter writer;
  bool8_t required_when_enabled;
} VkrMetricDescription;

bool8_t vkr_metrics_register(VkrMetrics *metrics,
                             const VkrMetricDescription *description,
                             VkrMetricId *out_id);
bool8_t vkr_metrics_seal(VkrMetrics *metrics);

void vkr_metrics_counter_add(VkrMetrics *metrics, VkrMetricId id,
                             uint64_t value);
void vkr_metrics_gauge_set_u64(VkrMetrics *metrics, VkrMetricId id,
                               uint64_t value);
void vkr_metrics_gauge_set_f64(VkrMetrics *metrics, VkrMetricId id,
                               float64_t value);
void vkr_metrics_duration_add_ns(VkrMetrics *metrics, VkrMetricId id,
                                 uint64_t duration_ns);

bool8_t vkr_metrics_snapshot_acquire(const VkrMetrics *metrics,
                                     VkrMetricsSnapshotView *out_view);
void vkr_metrics_snapshot_release(const VkrMetrics *metrics,
                                  VkrMetricsSnapshotView *view);
```

`VKR_METRIC_ID_INVALID` is never returned as success. Debug builds validate the
ID, kind, scalar, registry generation, and calling-thread policy. Release hot
calls are `static inline` indexed operations after those invariants were proven
at registration. Concurrent gauge descriptors are restricted to `u64`; C does
not guarantee a floating atomic is lock-free. Concurrent ratios are derived
from integer sum/count slots during finalization.

### 3.2 Storage and publication

One `VkrMetrics` instance is allocated from a dedicated arena at initialization:
a fixed descriptor/slot array (initially 256), renderer-thread accumulators,
cumulative concurrent atomics, previous concurrent snapshots, three published
frame snapshots, and the event ring.

Renderer-thread writers update the active plain accumulator. Concurrent writers
never receive a frame-buffer pointer: counters are cumulative atomic adds,
gauges are atomic exchanges, and durations aggregate cumulative integer
nanosecond sum/count. At `vkr_metrics_end_frame()`, the render thread snapshots
the cumulative values and derives interval deltas. Cumulative minima and maxima
cannot be differenced; concurrent duration slots therefore expose exact
sum/count deltas only. Producers that need exact per-operation extrema publish
bounded events, which the harness aggregates across the run. A job can start in
frame N and finish in N+1 without racing a buffer clear.

The render thread publishes into one of three fixed `VkrMetricsFrame` buffers.
`snapshot_acquire()` pins the current buffer with an atomic reader count and
returns an immutable view; `snapshot_release()` unpins it. The writer fills only
a non-current, unpinned buffer, then publishes its index with release semantics.
If both candidates remain pinned, the writer leaves the previous snapshot
current, increments `metrics.snapshot_publications_dropped`, and never waits.
Double buffering or a sequence retry around non-atomic payload fields is not a
valid C data-race strategy.

### 3.3 Threading

Renderer-thread slots are plain writes. Slots written from loader or job threads
— asset loads, parallel upload — are registered concurrent and go through
`core/vkr_atomic.h`.

The writer policy is **per slot** specifically so the draw path never pays for
an atomic it does not need. Registering a single-writer slot and later writing it
from a worker is a correctness bug; the registration site names its owner and a
contended test covers every concurrent primitive.

### 3.4 Pull collectors

Most existing data is already aggregated somewhere better than a raw counter.
The renderer adapter snapshots it once immediately after
`vkr_renderer_submit_packet()` and before the next
`vkr_renderer_prepare_frame()`:

```c
typedef struct VkrRendererMetricsCollectContext {
  VkrRendererFrontendHandle renderer;
  const VkrRendererFrameMetrics *frame_metrics; /**< submit_packet out-parameter */
  const VkrVisibilityStats *visibility;
  uint64_t cpu_frame_index;
  uint64_t submit_serial;
} VkrRendererMetricsCollectContext;
```

It pulls `vkr_renderer_get_device_memory_stats()`, `vkr_rg_get_pass_timings()`,
`vkr_rg_get_resource_stats()`, `VkrPipelineRegistry` telemetry, the atomic global
allocator snapshot, and `vkr_renderer_get_and_reset_upload_wait_stats()`.
It also resets `vkr_renderer_get_and_reset_command_slot_wait_count()` into
`frame.command_slot_waits`. The upload count is a subset of that total rather
than an independent wait category.

**Those wait calls reset their counters.** The collector therefore becomes
their single caller. Any second caller silently steals samples, so this
ownership must be stated at the call site.

Per-allocator `vkr_allocator_get_statistics()` is deliberately absent from the
context: its fields are non-atomic. An allocator owner may publish a local
snapshot from its own thread at a safe point; otherwise reports use the atomic
global tag totals. Bulk arena destruction still requires
`vkr_allocator_release_global_accounting()` or the global numbers are accounting
history, not live memory.

Device-memory metrics preserve both `heap_usage_valid` and
`live_totals_exact`. Phase 1b adds a fixed `VkrGpuAllocationOwner` to resource
creation descriptions, retained Vulkan resources, and the live allocation
table. The tracker retains live/peak/lifetime-total bytes and allocation counts.
The metrics adapter publishes live/peak gauges plus per-frame
`bytes.allocated` and `allocations.created` counter deltas under
`memory.gpu.owner.*` for `unknown`, `mesh`, `texture`, `font`, `render_graph`,
`shader`, `instance`, `indirect`, `staging`, `readback`, and `swapchain`.
Allocation and free use the stored enum directly; no path infers ownership from
debug names, usage flags, or memory types. `unknown` is a reportable first-class
bucket so a missed or external caller is visible rather than misattributed.

### 3.5 Per-pass table

Pass timings are dynamically sized and keyed by name. `VkrMetricsPassTable` is
sized when graph topology changes and copies names into adapter-owned storage;
`VkrRgPassTiming.name` itself is borrowed only until the next graph begin. A
resize or condition change that preserves names does not allocate in execution.

CPU pass samples carry the current CPU frame index. GPU timing plumbing gains
the completed source frame index/submit serial alongside its values, because
buffered GPU results otherwise look as if they belong to the CPU frame that
happened to collect them. Each sample preserves `culled`, `disabled`, and
`gpu_valid`; invalid GPU values are absent samples, never zero milliseconds.

### 3.6 Event log

Asset loads and pipeline creations have unbounded cardinality and low frequency,
which makes them a poor fit for fixed slots. They use a bounded multi-producer,
single-consumer ring:

```c
typedef struct VkrMetricEvent {
  VkrMetricId source;
  uint8_t subject_length;
  char subject[VKR_METRIC_EVENT_SUBJECT_MAX];
  uint64_t start_ns;
  uint64_t duration_ns;
  uint64_t bytes;
  uint32_t thread_id;
  bool8_t subject_truncated;
} VkrMetricEvent;
```

4096 entries are allocated once, and
`VKR_METRIC_EVENT_SUBJECT_MAX <= UINT8_MAX`. A per-entry sequence protocol
publishes a fully written record without a mutex. The subject is copied on the
cold path;
the ring never retains a borrowed `String8` or performs concurrent arena
interning. Overflow drops the event and increments `metrics.events_dropped`;
truncation is recorded per event. Both counts appear in every report, and any
gate that requires complete events becomes `incomplete` after a drop.

### 3.7 Overhead policy

`VKR_METRICS_ENABLED` (default 1) compiles every call to nothing when 0. At
runtime `VkrMetricsConfig` gates `pass_gpu_timings` — off by default, because
timestamp queries perturb the command stream and a timestamp-on run may only be
compared against another timestamp-on run — and event subjects. Counts required
for work-volume and overflow correctness remain enabled together.

The report records both compile-time and runtime instrumentation flags. A
timestamp-on or event-subject-on run is a different comparison configuration.

**Acceptance gate:** run at least five paired blocks, each containing one
isolated Release process with `VKR_METRICS_ENABLED=0` and one with `1`, on one
fixed case (at least ten processes total). Balance AB/BA order across blocks to
limit thermal/order bias. Record every repetition, paired deltas, median/spread,
build/compiler, power/thermal state, CPU/GPU/driver, target, resolution, scene,
image count, sample window, and all instrumentation flags. If the measured
regression exceeds 1%, or uncertainty/noise is too large to resolve that budget,
the always-on set remains runtime-disabled. One process is an observation, not
an overhead result.

### 3.8 Instrumentation points to add

| Signal | Location |
|---|---|
| `frame.wall`, `cpu.frame_work`, `cpu.update`, `frame.limiter_sleep` | explicit boundaries in `application_start()` in `lib/src/application.h` |
| `cpu.render_prepare`, `cpu.render_submit` | callers around `vkr_renderer_prepare_frame()` and `vkr_renderer_submit_packet()` |
| `cpu.backend_present` | inside the windowed target completion path; absent for offscreen |
| `boot.*` — instance, device, target, systems, graph, scene | `vkr_renderer_initialize()`, `vkr_renderer_systems_initialize()`, harness/application boot |
| Pipeline creation ms, as an event (replacing the existing `log_info`) | `vulkan_graphics_graphics_pipeline_create()` |
| Shader load and SPIR-V reflection ms | `vulkan_shaders.c`, `resources/loaders/shader_loader.c` |
| Texture, mesh, material, font, and scene load ms and bytes, as events | prepare/finalize seams in `renderer/resources/loaders/` |
| Job queue depth, worker busy ratio, jobs completed | `core/vkr_job_system.c` |
| Instance buffer occupancy and overflow (fixed 65 536 capacity) | `renderer/vkr_instance_buffer.c` |

**Names carry no unit suffix.** Durations are stored and published in
nanoseconds, and the catalog `unit` is the contract. A name ending `_ms` on a
slot holding nanoseconds is wrong by a factor of a million in every consumer
that trusts it, and once a baseline exists the name cannot be corrected
without a migration — so `VkrMetricUnit` deliberately has no millisecond
member and a report writer converts for display.

Cumulative sources are published as per-frame deltas under `VKR_METRIC_KIND_COUNTER`,
not as running totals under a `_total` gauge; averaging a running total
describes nothing. A counter nobody incremented publishes a valid zero, because
"no draws this frame" and "draws were not measured" are different claims and
only the second one is missing evidence.

Counter and duration names describe one interval; gauges describe state at the
collection boundary. `frame.wall` includes limiter sleep and window-event work;
`cpu.frame_work` excludes sleep; `cpu.render_submit` includes graph execute,
command-buffer end, queue submit, and windowed present unless the backend
publishes the nested present duration. FPS is derived from a declared frame-time
series, not maintained as a second clock. The first catalog version and each
metric's exact start/end boundary land beside the code; the names above are not
permission to time overlapping regions and add them as if they were disjoint.

### 3.9 JSON writer

`core/vkr_json.h` is a **reader**. There is no writer.

Add `lib/src/core/vkr_json_writer.{h,c}`: emit-only, sink-based, with a fixed
scratch buffer, correct escaping for length-prefixed `String8`, bounded nesting,
and hard rejection of non-finite floats. It writes to a temporary file and
atomically renames only after a complete document; it does not retain the whole
report in an arena or allocate per token.

The case reader and report writer stay dependency-free. JSON Schema validation
is a development/test gate; the runtime parser still implements every required
structural and semantic check because draft-07 `default` annotations do not fill
values for it.

### 3.10 The HUD becomes a consumer

`app/src/main.c` currently computes FPS, frame time, and memory strings itself on
its own clocks, in parallel with everything above. Rewire it to read the
published metrics frame. The harness copies each published frame into storage
pre-sized from the manifest so percentiles and run reports require no renderer
allocation.

One source of truth, and the on-screen overlay becomes a continuous check that
collection actually works.

## 4. Boot profiles and headless operation

### 4.1 Fast boot

```c
typedef enum VkrBootProfile {
  VKR_BOOT_PROFILE_FULL = 0,
  VKR_BOOT_PROFILE_AUTOMATION,
} VkrBootProfile;
```

Boot profile and output target are orthogonal. `AUTOMATION` means a dependency-
resolved subsystem plan; it does not mean "no window." During the windowed
phases the harness uses a hidden window. True headless execution begins only
when the `OFFSCREEN` target in §4.2 ships.

`vkr_renderer_systems_initialize()` gains a `VkrSubsystemPlan`, built once from
the case's render features, capture-channel requirements, and assertion metric
requirements. It closes dependencies before initialization and fails preflight
on an impossible combination. The effective mask is recorded in the report.
Fonts, UI, gizmo, editor viewport, and picking can be omitted only when no
requested workload or capture depends on them. Skybox, shadows, editor mode, or
any other feature that changes rendered work is case configuration, not a boot
optimization that may be silently skipped.

The frame limiter is disabled for profiling. Windowed profiles request
IMMEDIATE, then verify and report the **actual** selected mode. If IMMEDIATE is
unavailable, an authoritative profile is unavailable rather than silently
falling back to FIFO. Snapshot-only profiles may deliberately use FIFO, but it
remains part of their comparison fingerprint.

**IMMEDIATE is not an incidental detail.** Under FIFO the application is pinned
to the refresh rate and every CPU-side improvement measures as zero.
`vkr-performance` calls this out, and the current benchmark script does not
control for it.

**Acceptance gate:** `boot.*`, resident CPU/GPU bytes, and functional case
coverage justify the profile. A skip that produces no measured boot or memory
benefit is not retained merely to make the mask larger. Full and automation
profiles are distinct comparison configurations.

**Implemented Phase-3 plan.** The current automation base retains fonts because
scene files may contain world text and the case manifest cannot independently
prove their absence. For the focused Sponza profile, UI, editor, gizmo, and
picking are omitted while the case-required skybox and shadows remain. The
renderer-reported closure is persisted as canonical hexadecimal in raw samples,
reports, and workload identity; paired evidence and its non-authoritative local
limits are recorded in the Phase-3 verification document.

### 4.2 True offscreen

A target-neutral configuration is selected before Vulkan instance/device
creation:

```c
typedef enum VkrPresentTargetKind {
  VKR_PRESENT_TARGET_WINDOWED = 0,
  VKR_PRESENT_TARGET_OFFSCREEN,
} VkrPresentTargetKind;

typedef struct VkrPresentTargetConfig {
  VkrPresentTargetKind kind;
  uint32_t width;
  uint32_t height;
  uint32_t image_count;
} VkrPresentTargetConfig;
```

Inside `renderer/vulkan/`, a private `VulkanPresentTarget` has two
implementations. `swapchain` owns the current surface, WSI extensions, present
queue, acquire/render-complete semaphores, tri-state acquire/present, and
recreation behavior. `offscreen` owns N color/depth image pairs and requires no
surface, present queue, swapchain extension, acquire semaphore, or render-
complete semaphore.

Acquisition returns an image index plus optional submit wait/signal semaphores
and preserves `OK`, `SKIP`, and `FAILED`. The common image-in-flight fence table
guards both implementations; offscreen round-robin may select an image only
after its associated fence completes. The seam is a `VulkanPresentTargetOps`
table -- create/destroy/resize, per-frame begin/acquire/complete/cancel, plus
the WSI capability, recovery policy, and terminal state each implementation
declares -- so the frame path asks the target what to do instead of asking
which kind it is. Target completion calls presentation only
on the windowed path. The graph compiler obtains the target's terminal
access/layout: windowed output ends at `PRESENT`/`PRESENT_SRC_KHR`; offscreen
output ends in the target's retained state. Any capture read precedes that
terminal graph edge, and completion injects no hidden barrier. Capture is never
performed by `present()`; it is declared graph work in §5.

The graph's legacy `"import": "swapchain"` and `"swapchain_depth"` names resolve
through target-neutral backend handle, initial-state, extent, count, and format
queries. `vkr_renderer_window_*`/`swapchain_*` are retired outright rather than
kept as compatibility wrappers: graph compilation, frame setup, capture, shadow,
picking, and the editor viewport all reach the target's attachments through
`vkr_renderer_present_target_*`, so one seam describes both target kinds. Import layout/access
is never hard-coded to `UNDEFINED` when the offscreen target retained contents.

Two invariants this must not break, both documented in the architecture
specification §7.4:

- the split between per-image objects (render-complete semaphores, image-in-flight
  fence references) and per-frame-in-flight objects (acquire semaphores, submit
  fences);
- the resize and recovery path, including `frame_recovery_required`.

The offscreen target has no resize event and no `VK_ERROR_OUT_OF_DATE_KHR`.
Extent changes are explicit recreations outside an active frame. Each offscreen
image retains a known layout/access state for the next import instead of being
reset to `UNDEFINED` merely because the swapchain path discards contents.

`VkrCamera` holds a `VkrWindow *` for input and aspect ratio. Harness cameras
receive explicit extent/aspect and direct poses; interactive input remains an
optional window concern. Backend device selection also becomes target-aware:
offscreen mode must not create a surface, require a present queue, or query WSI
formats.

Reports record target kind, actual image count, extent, formats, color space,
and—only for a windowed target—actual present mode. Windowed and offscreen
results are never directly comparable. Authoritative performance cases are
serialized per physical GPU even offscreen; removing a window does not remove
device contention.

Rationale and alternatives: [ADR-014](../architecture/adr/014-offscreen-present-target.md).

## 5. Capture and visual snapshots

### 5.1 Frontend API

```c
typedef uint16_t VkrCaptureChannelId;
typedef uint64_t VkrCaptureRequestId;

typedef struct VkrCaptureItemRequest {
  VkrCaptureChannelId channel;
  uint32_t mip;
  uint32_t layer;
} VkrCaptureItemRequest;

typedef struct VkrCaptureBatchRequest {
  VkrCaptureRequestId request_id;
  const VkrCaptureItemRequest *items;
  uint32_t item_count;
} VkrCaptureBatchRequest;

typedef struct VkrCaptureItemResult {
  VkrCaptureChannelId channel;
  char producer_resource[64];
  uint32_t width;
  uint32_t height;
  uint64_t row_pitch;
  VkrTextureFormat format;
  VkrCaptureValueKind value_kind;
  VkrColorSpace color_space;
  VkrImageOrigin origin;
  const uint8_t *data;
  uint64_t data_size;
  uint32_t mip;
  uint32_t layer;
} VkrCaptureItemResult;

typedef enum VkrCaptureStatus {
  VKR_CAPTURE_STATUS_NOT_FOUND = 0,
  VKR_CAPTURE_STATUS_PENDING,
  VKR_CAPTURE_STATUS_READY,
  VKR_CAPTURE_STATUS_FAILED,
} VkrCaptureStatus;

typedef struct VkrCapturePollResult {
  VkrRendererError error; /**< Stable failure reason when status is FAILED. */
  const VkrCaptureItemResult *items;
  uint32_t item_count;
  uint64_t source_frame_index;
  uint64_t submit_serial;
} VkrCapturePollResult;

VkrCaptureStatus vkr_renderer_capture_poll(
    VkrRendererFrontendHandle renderer, VkrCaptureRequestId request_id,
    VkrCapturePollResult *out_result);
bool8_t vkr_renderer_capture_release(VkrRendererFrontendHandle renderer,
                                     VkrCaptureRequestId request_id);
```

Channel strings resolve to IDs during manifest preflight. Requests travel on the
packet's existing `VkrGpuDebugPayload`, extended with an optional batch request.
The packet is the value-like input channel (ADR-004); capture must not become a
side-channel setter. Result data is borrowed only after `READY` and remains valid
until the matching explicit release—not merely until an unrelated next request.
Request IDs are nonzero and unique within one renderer generation; a stale or
duplicate ID is rejected. Unknown, duplicate, unavailable, or out-of-range
channel/mip/layer requests fail before `prepare_frame()`.

### 5.2 Backend: generalizing the readback ring

The pixel-copy path is a starting point, not a full-frame API.
`vulkan_image_copy_to_buffer_ex()` currently fixes mip and layer to zero and the
picking slot is an eight-byte, one-pixel record. Add a **separate capture-batch
ring** with these rules:

1. One slot represents a batch and contains aligned ranges for every requested
   channel. Capacity and range metadata are reserved during manifest/case setup,
   outside the timed window; capture execution never grows a buffer.
2. Ring capacity is configurable and at least the backend frame-in-flight count.
   A full ring makes `vkr_renderer_submit_packet()` return
   `VKR_RENDERER_ERROR_CAPTURE_BUSY`; the render thread never inherits picking's
   indefinite wrap wait. Reservation occurs immediately after packet validation
   and before retained-state mutation or graph construction. Existing
   frame-cancel recovery consumes/releases the acquired target image. The
   harness drains and retries without advancing case time, case-frame index, or
   measurement samples. It polls with the manifest timeout and reports timeout
   as an incomplete run.
3. Slot states are explicit: `IDLE → RESERVED → RECORDED → SUBMITTED → READY →
   ACQUIRED → IDLE`, with `FAILED` and `ABANDONED` terminal side paths. A
   successful queue submit associates the slot with its submit serial/fence.
   Non-coherent mappings are invalidated before `READY`. Polling `READY` pins the
   borrowed result as `ACQUIRED`; release returns it to `IDLE`. A pre-submit
   cancel or submit failure becomes an observable `FAILED` tombstone until
   release. Releasing a submitted-but-pending request marks it `ABANDONED`, but
   the slot is not reusable until its fence proves GPU completion.
4. Extend the copy helper with mip, base layer, layer count, aspect, and checked
   byte/row-pitch calculation. Reject multisampled, compressed, combined
   depth/stencil, or unsupported conversions until a catalog entry defines how
   to canonicalize them.
5. The cancel/failure rollback that currently covers picking must cover every
   `RECORDED` capture in the unsubmitted primary buffer. A failed submit cannot
   leave a request pending forever, and a successfully submitted request cannot
   be reused before proven GPU completion.

### 5.3 Graph: a declared `Capture.Readback` pass

Modelled on `Picking.Readback` and gated on capture work, so a non-capturing
frame allocates nothing and transitions nothing. A static JSON pass cannot list
request-dependent reads without also referencing unavailable conditional
resources. After JSON realization, a capture overlay builder appends one
`Capture.Readback` graph pass, one declared image read per directly capturable
item, and an imported external staging-buffer range with declared transfer
writes. This remains graph-declared work; the executor receives both sources
and destinations from pass context rather than discovering hidden resources
while recording.

**The pass must declare its `TRANSFER_SRC` reads.** An undeclared capture copy
would reproduce the same state-authority defect that still makes runtime IBL
baking incomplete. The graph compiler owns the producer→copy visibility and
layout transition; render-pass final layouts are not a substitute for a declared
consumer.

A fixed `VkrCaptureChannelDescription` catalog maps each ID to source resource,
required graph condition/subsystems, aspect, value kind, canonical encoding, and
`capture_version`. Adding a direct channel extends this table and its producer
publication; no string switch is added to the executor. `depth` resolves to
`opaque_vbuffer_depth` for deferred, `scene_depth` for retained-forward editor,
or `swapchain_depth` for retained-forward fullscreen. Effective renderer/editor
configuration is recorded in the workload fingerprint rather than inferred
after capture.

The JSON-owned `scene_depth`, `scene_color`, and `shadow_map` resources declare
`TRANSFER_SRC`. Imported swapchain color/depth resources are augmented only by
the request-specific overlay after their capture-capable Vulkan usage was
validated at initialization.

That usage bit is a Vulkan copy-legality requirement, not evidence that the
change is free or that it disables depth compression. On the MoltenVK revision
pinned by a run, inspect
[`MVKPixelFormats::getMTLTextureUsage`](https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKPixelFormats.mm):
current upstream maps transfer-source use to Metal shader-read use and may also
require a pixel-format view for combined depth/stencil transfers. Apple treats
[`MTLTextureUsage`](https://developer.apple.com/documentation/metal/mtltextureusage)
as an optimization input. Neither fact supports a blanket compression claim;
the pinned MoltenVK/OS/GPU stack must be measured. Prefer `D32_SFLOAT` for the
first depth-capture path, or validate combined depth/stencil transfer and
conversion separately.

The selected present color also needs transfer-source capability. Offscreen
images are created with it. A windowed target may add
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` only when
`VkSurfaceCapabilitiesKHR.supportedUsageFlags` advertises it. If WSI does not,
preflight either routes final output through a graph-declared captureable
intermediate followed by the ordinary present composite, or reports
`final_color` unavailable for that target/profile. It never records an illegal
swapchain copy or silently substitutes `scene_color`.

**Open obligation.** Query format/image/surface capability and measure the cost
of adding transfer-source usage to color and depth resources on target devices.
A profiling run with capture-capable usage is a different configuration even
when no capture occurs. Do not assume either zero cost or disabled compression.
If the cost exceeds noise, use capture-only intermediate resources/paths and
keep authoritative profiling on the ordinary usage flags.

### 5.4 Channel classes and replay

| Channel | Source | Mechanism |
|---|---|---|
| `final_color` | selected present-target image | direct copy |
| `scene_color` | graph `scene_color` | editor mode only |
| `depth` | `swapchain_depth` / `scene_depth` | depth-aspect copy |
| `shadow_cascade_N` | `shadow_map` layer N | array layer slice |
| `picking_ids` | `picking_color`, R32_UINT | exact object IDs |
| `visibility_ids`, `visibility_primitives` | Metal `opaque_vbuffer`, RG32_UINT | direct component capture; provisional deferred path only |
| `gbuffer_diffuse`, `gbuffer_specular`, `gbuffer_normal` | Metal P8 G-buffer targets | direct capture; normal preview decodes signed RG16 |
| `deferred_emissive`, `resolve_barycentric_lod` | Metal P8 resolve targets | direct diagnostic capture |
| `normals` | `final_color` at `VKR_RENDER_MODE_NORMAL` | **re-render, not an attachment** |
| `unlit`, `lighting` | `final_color` at `UNLIT` / `LIGHTING` | re-render |
| `shadow_debug_cascades`, `_factor`, `_depth` | `shadow_debug_mode` 1–3 | re-render |

The first five rows can share one direct-capture replay; debug-mode rows require
one replay per distinct global. **No capture pass is inserted into a primary
timing repetition.** For each requested checkpoint/mode the harness starts from
the same manifest, seed, fixed delta, cache policy, and fixed warmup, replays to
the checkpoint, then captures. This avoids advancing primary temporal/retained
state or polluting timing samples merely to obtain color, depth,
normals/unlit/shadow-debug output. Capture/replay runs are never performance
evidence.

`picking_ids` requires the picking producer and compares exact integer IDs. A
missing producer is an unavailable required channel, not a transparent image.

### 5.5 Comparison

Every capture writes metadata before comparison: channel/capture version,
producer pass, source and canonical formats, value kind, color space, origin,
extent, row pitch, mip/layer, source frame/submit serial, data digest, and
artifact-relative paths. Extent, value kind, canonical format, capture version,
or color-space mismatch is `incompatible`, never a pixel failure.

Canonical payloads are deliberately small-dependency:

- display color/debug channels become top-left `RGBA8_SRGB` PNGs;
- depth/shadow become little-endian tightly packed `R32_SFLOAT` data plus an
  8-bit normalized PNG preview and metadata describing preview normalization;
- picking IDs become little-endian tightly packed `R32_UINT` data plus a
  deterministic palette preview.

Comparison uses canonical data, not previews. Float/color channels report mean
absolute error, maximum absolute error, failing value/pixel count, and failed
pixel ratio. Comparison converts every non-identity component to a `[0, 1]`
error domain: RGBA8 code values are divided by 255 and depth/shadow values are
their finite normalized device-depth values. A pixel fails if any component
exceeds `max_pixel_delta`; mean absolute error is over scalar components. This
makes one threshold meaningful across color and depth batches. Case/profile
thresholds include `max_pixel_delta`,
`max_mean_absolute_error`, and `max_failed_pixel_ratio`; dimensions and finite
values are checked first. Integer identity channels compare exactly.

`picking_ids` compares **exactly**. Tolerance matching on object identifiers is
meaningless — ID 41 is not "nearly" ID 42.

Vendor `stb_image_write.h` next to the existing `stb_image.h` and
`stb_truetype.h`, with `lib/src/vendor/stb_image_write_impl.c` following the
established `*_impl.c` convention. A small owned converter writes the raw
numeric payloads and previews; no general image library is required. PNG
previews render in pull-request diffs, while raw payload digests keep numeric
comparison exact.

## 6. Cases, camera scripting, and autotests

One manifest format serves all three subcommands:
`tools/cases/<suite>/<case>.case.json`. Schema:
[`harness-case-schema.json`](harness-case-schema.json).

```json
{
  "schema_version": 1,
  "id": "smoke.sponza.orbit",
  "suite": "smoke",
  "scene": "assets/scenes/sponza.scene.json",
  "seed": 1,
  "resolution": [1280, 720],
  "boot": "automation",
  "target": "offscreen",
  "present": "none",
  "cache": "isolated_warm",
  "fixed_delta": 0.016666667,
  "repetitions": 1,
  "repetition_timeout_ms": 60000,
  "asset_ready_timeout_ms": 30000,
  "frames": { "warmup": 120, "measure": 300 },
  "renderer": {
    "editor": false,
    "skybox": true,
    "shadow_preset": "default",
    "shadow_cascades": 4
  },
  "camera": {
    "mode": "orbit",
    "speed": "medium",
    "center": [0, 2, 0],
    "radius": 12,
    "height": 3,
    "revolutions": 1,
    "duration_seconds": 5,
    "vertical_fov_degrees": 70,
    "near_plane": 0.1,
    "far_plane": 500
  },
  "captures": [
    { "at_frame": 200, "channels": ["final_color", "depth", "normals", "shadow_cascade_0"] }
  ],
  "assertions": [
    { "metric": "draw.calls_issued", "equals": 281 },
    { "metric": "upload.fence_waits", "equals": 0 }
  ]
}
```

Camera modes: `static`; `keyframes` (linear or Catmull-Rom over
`{t, position, yaw, pitch}`); `orbit`; and `flythrough`, a keyframe path
traversed at constant arc-length speed. The `speed` preset has fixed multipliers
(`slow=0.5`, `medium=1`, `fast=2`): authored case time advances by
`fixed_delta * multiplier`, so slow doubles wall-frame count for the same path
and fast halves it. One path can exercise several temporal loads without being
re-authored. Every static/keyed pose includes yaw and pitch in degrees. Orbit
looks at `center` with world +Y as up and defines relative height, authored
duration, revolution count, and start angle. Key times are strictly increasing.
Lens values are part of the manifest because they change visibility and pixels.

Camera evaluation is versioned (`camera_script_version=1`) and included in the
workload fingerprint. Version 1 fixes yaw-wrap direction, endpoint behavior,
Catmull-Rom tension, flythrough arc-length lookup subdivision count, lookup
tie-breaking, and orbit handedness in one implementation table with CPU
fixtures. It may not depend on an adaptive tolerance or platform math-library
iteration count that can choose a different pose on another run.

`harness-case-schema.json` owns structural constraints. Runtime semantic
validation additionally checks that the suite matches the containing directory,
the ID begins with that suite, paths are repository-relative and stay below
their allowed root, the scene exists, capture frames are less than
`frames.measure`, camera key times are ordered, requested cascades/channels
exist, target/present combinations are valid, and every required metric is
registered. Schema `default` values are annotations; the parser applies and
records effective defaults explicitly.

### 6.1 Determinism rules

These are what separate a harness from a screenshot script. Each is a
requirement, not a default.

1. **Freeze until ready.** Boot, async asset preparation/finalization, and an
   isolated pipeline-cache prewarm happen while simulation time remains zero.
   The root scene and its required dependency closure must reach a successful
   terminal state; a fallback or partial-load policy is explicit in the case,
   never inferred. Timeout produces a failed/incomplete report, not a capture.
2. **Fixed delta drives simulation.** Camera advancement uses `fixed_delta`,
   never wall-clock. Wall-clock is still measured for timing metrics, but never
   fed back into scene state. After readiness/prewarm, the run executes exactly
   `warmup` frames followed by exactly `measure` frames. A contiguous simulation
   frame index starts at the first warmup frame; a separate zero-based measured
   frame index starts after warmup and is what `captures[].at_frame` addresses.
3. **Bypass `VkrCameraController` entirely.** It integrates input and is
   frame-rate dependent. Add `vkr_camera_set_pose()` and an explicit lens/extent
   setter. No harness camera reads window size or input state.
4. **Warmup is an exact phase and a gate.** During the configured warmup, require
   no new required pipeline creations and stable non-overlapping windows of the
   profile-selected metric for authoritative profiling. Existing profiles
   default to `cpu.render_submit`; profiles for an implementation where driver
   work moves between prepare and submit may explicitly select `frame.wall`.
   The execution profile owns metric, window size, and allowed drift. The
   harness never extends warmup to chase stability, because that would shift
   simulation and capture poses.
   Insufficient frames or unstable warmup sets `authoritative=false`, or makes
   the run `incomplete` when the selected profile requires stability.
5. **Control pacing and cache state.** The frame limiter is off. Windowed
   profiling verifies actual IMMEDIATE; offscreen uses `present=none`. An
   `isolated_cold` run gets a new per-run cache path, while `isolated_warm`
   prewarms that isolated cache. The harness never deletes or mutates the user's
   ordinary pipeline cache.
6. **Self-check.** Independent repetitions of the same case/build must produce
   bit-identical work-volume metrics: draw, batch, visibility, overflow, and
   capture-request counts. A difference invalidates timing evidence regardless
   of its average. Completion-availability gauges such as
   `visibility.hzb.history_valid` are excluded; actual candidate, visibility,
   HZB-rejection, command, overflow, and resolve counts remain part of the
   identity. Timing samples are never expected to be bit-identical.

When GPU timings are requested, the child freezes its pass catalog only after
eight consecutive completed frames expose the same pass-name topology and every
row has a valid GPU result. A scene's first packet may precede light/cascade
synchronization and therefore must not define the steady-state catalog.

### 6.2 Execution profiles and independent repetitions

Case manifests describe workload. `tools/profiles/<profile>.json` describes the
evidence environment and authority policy: dirty-tree policy,
OS/CPU/GPU/vendor/device/driver constraints, target/window mode, required actual
present mode, power/thermal policy, process priority, exclusive GPU-lane policy,
instrumentation flags, minimum isolated repetitions, warmup stability
metric/window/drift, required metrics/channels, and comparison-threshold
ownership.

P20 acceptance adds three clean-tree, five-repetition profiles:
`p20-acceptance-offscreen-gpu.json` for ordinary offscreen workloads,
`p20-acceptance-offscreen-gpu-work.json` for synthetic fixtures whose stable
candidate count is the warmup invariant, and
`p20-acceptance-windowed-gpu.json` for hidden-window IMMEDIATE presentation.
They require complete requested GPU timestamps, deterministic work, stable
warmup, and an exclusive GPU lane. They do not promote snapshot baselines.

Authoritative performance thresholds live with an accepted profile baseline,
not in a case that can weaken its own gate. Case-local timing assertions are
allowed for exploration but set `authoritative=false`. Deterministic
work-volume and correctness assertions remain case-owned.

`repetitions > 1` launches a fresh child process per repetition, each with its
own timeout, report, stdout/stderr, and isolated run directory. In-process frame
windows are samples, not independent repetitions. A profile can raise the
manifest repetition count but never lower it below its authoritative minimum.

### 6.3 Autotest is not a separate engine

`autotest` is the same runner with an `assertions` block.

- `profile` runs capture-free primary repetitions and reports metrics.
- `snapshot` runs direct/debug capture repetitions and compares images; timing
  from those repetitions is diagnostic only.
- `autotest` runs capture-free primary repetitions plus the required snapshot
  repetitions, then asserts over their separate evidence sets.

One shared runtime, parser, case catalog, artifact writer, and report envelope;
three verdict policies. The referenced [Nuri tools](https://github.com/Panbok/nuri/tree/master/tools),
[autotest skill](https://github.com/Panbok/nuri/tree/master/.codex/skills/nuri-autotests),
[benchmark skill](https://github.com/Panbok/nuri/tree/master/.codex/skills/nuri-benchmarks),
and [snapshot skill](https://github.com/Panbok/nuri/tree/master/.codex/skills/nuri-snapshots)
demonstrate the value of a shared runtime, profiles, fingerprints, and
independent repetitions. VKR keeps one C executable and does not import their
external CLI/JSON/image stack.

**Implemented Phase-5 boundary.** `profile` runs full or automation boot on
windowed-visible
or windowed-hidden cases. It freezes harness-owned scene/camera simulation until
the requested scene is ready, applies a fixed delta and scripted pose/lens,
isolates cold/warm cache state, verifies actual presentation configuration,
launches independent timed child processes, checks work-volume identity, and
aggregates metrics, per-pass timings, and bounded events. Automation resolves a
typed dependency closure before initialization and persists the renderer's
actual mask in samples, reports, and workload identity. `offscreen` targets
fail as unavailable until phase 6. `snapshot`, auxiliary debug replay,
`autotest`, comparison, and guarded profile-scoped baselines are implemented
for the windowed targets.

## 7. Reports and exit codes

Artifacts land in `build/_artifacts/<tool>/<run-id>/`. The run ID is generated,
not accepted as an unchecked path. Manifest IDs, suite names, artifact paths,
and baseline paths are relative, reject `..`/absolute/symlink escape, and resolve
under an explicit root. Reports and metadata are written to temporary siblings
and atomically renamed; a `complete` report is the final write. A killed process
leaves an incomplete run directory, never a plausible partial report.

```json
{
  "schema_version": 1,
  "kind": "vkr.harness.report",
  "tool": "profile",
  "tool_version": "1",
  "run_id": "20260801T120000.000Z-a1b2c3",
  "status": "pass",
  "exit_code": 0,
  "authoritative": false,
  "authority_reasons": ["profile.local_only"],
  "case": {
    "id": "smoke.sponza.orbit",
    "suite": "smoke",
    "manifest_sha256": "sha256:..."
  },
  "profile": {
    "id": "local-macos-mvk",
    "compatible": true,
    "incompatibility_reasons": []
  },
  "provenance": {
    "git_sha": "d1cbfc8",
    "dirty": false,
    "build_type": "Release",
    "compiler": "AppleClang ...",
    "os": "macOS ...",
    "cpu": "Apple M1 Pro",
    "gpu": "Apple M1 Pro",
    "gpu_vendor_id": 4203,
    "gpu_device_id": 0,
    "driver": "MoltenVK ...",
    "power_mode": "normal",
    "thermal_state_start": "nominal",
    "thermal_state_end": "nominal",
    "process_priority": 0
  },
  "comparison": {
    "environment_fingerprint": "sha256:...",
    "workload_fingerprint": "sha256:...",
    "policy_fingerprint": "sha256:..."
  },
  "effective_config": {
    "resolution": [1280, 720],
    "scene": "assets/scenes/sponza.scene.json",
    "target": "offscreen",
    "target_image_count": 3,
    "present_mode": "none",
    "boot_profile": "automation",
    "subsystem_mask": "0x...",
    "editor": false,
    "cascades": 4,
    "cache": "isolated_warm",
    "camera_script_version": 1,
    "gpu_timing": false,
    "metrics_compile_enabled": true,
    "events_enabled": true
  },
  "execution": {
    "requested_repetitions": 1,
    "completed_repetitions": 1,
    "warmup_frames": 120,
    "measured_frames": 300,
    "warmup_stability_metric": "cpu.render_submit",
    "warmup_stable": true,
    "gpu_lane_lock_acquired": true
  },
  "runs": [
    { "index": 0, "status": "pass", "report": "runs/0/report.json", "sha256": "sha256:..." }
  ],
  "auxiliary_runs": [],
  "aggregate": {
    "metrics": {
      "cpu.render_submit": {
        "unit": "ns", "sample_count": 300, "invalid_count": 0,
        "mean": 6100000, "p50": 6000000, "p95": 7400000,
        "min": 5200000, "max": 11800000, "stddev": 600000
      }
    },
    "passes": [
      {
        "name": "World.Opaque",
        "cpu_ms": { "sample_count": 300 },
        "gpu_ms": { "sample_count": 0, "unavailable_reason": "disabled" }
      }
    ]
  },
  "events": { "items": [], "dropped": 0, "subjects_truncated": 0 },
  "captures": [
    {
      "channel": "final_color", "capture_version": 1,
      "actual": "captures/final_color.png", "status": "pass",
      "max_abs_error": 0.0078431373, "failed_pixel_ratio": 0.0001
    }
  ],
  "assertions": [
    { "metric": "draw.calls_issued", "operator": "equals", "status": "pass", "actual": 281, "limit": 281 }
  ],
  "diagnostics": [
    { "code": "gpu_timing.disabled", "severity": "info", "message": "GPU timing was not requested" }
  ],
  "artifacts": [
    { "role": "capture.final_color", "path": "captures/final_color.png", "media_type": "image/png", "sha256": "sha256:...", "status": "complete" }
  ]
}
```

The example is normative in shape; phase 2 lands a machine-validated report
schema beside the case schema before any report becomes a baseline. Required
top-level fields are identity/version, status/authority, case/profile,
provenance, comparison fingerprints, effective configuration, execution,
primary child runs, auxiliary replay runs, aggregate metrics/passes, events,
captures, assertions, diagnostics, and artifact digests. Paths are relative to
the run root. Auxiliary runs carry their debug mode, checkpoint, status, report
digest, effective capture configuration/fingerprints, and artifacts; they can
fail a requested snapshot but never contribute timing samples to `aggregate`.
`authoritative=false` requires at least one stable code in `authority_reasons`;
an authoritative report requires that array to be empty.
Each child repetition report's digest is recorded by the aggregate parent. The
aggregate report's digest is emitted by the final stdout result or enclosing CI
metadata after the atomic rename; a report never embeds a self-referential
digest.

Fingerprints use sorted, length-prefixed name/value fields and SHA-256 so field
order and delimiters cannot collide. The workload fingerprint covers semantic
effective scene, camera, renderer, cache, target extent/image count, pacing,
fixed-delta, warmup/measurement, subsystem, and instrumentation settings; it
does not include a raw manifest digest, so editing a description does not change
the workload. The environment fingerprint covers actual build type/toolchain
and relevant flags, OS/CPU, GPU vendor/device/driver, target kind,
formats/color space, actual present mode, power mode, process priority, and GPU
lane policy. Start/end thermal state remains reported validity evidence rather
than equality identity. The policy fingerprint covers profile authority rules,
required metrics/channels, capture versions, assertions, comparison thresholds,
and statistic algorithms. A comparison may report raw observations across a
policy mismatch, but it cannot issue an authoritative verdict.

Fingerprint inputs are command-projected, not a hash of unused manifest text:
`profile` excludes capture requests/image thresholds, `snapshot` excludes
timing-assertion policy, and an `autotest` parent retains the distinct primary
and auxiliary child fingerprints instead of flattening them into one identity.

The manifest digest, source `git_sha`, dirty state, binary digest, timestamps,
run ID, and artifact paths are provenance, not comparison identity—otherwise
two revisions could never be compared. A dirty tree or forced mismatch sets
`authoritative=false`.

Comparison lists field-level incompatibilities and refuses an authoritative
verdict when any required fingerprint differs. Missing required samples,
`gpu_valid=false`, inexact device-memory totals, event drops for a completeness
gate, snapshot-publication drops, unstable warmup, insufficient independent
repetitions, or failed artifact digests make the report
`incomplete` when required, or set `authoritative=false` with a diagnostic when
the affected evidence is optional; they do not turn into zeros. Percentiles use
one documented algorithm (nearest-rank for v1) and `stddev` is population
standard deviation. Every statistic carries unit, sample count, and invalid
count.

`summary.csv` is long-form rather than one widening row:
`run_index,metric,unit,stat,value,sample_count,status`. Adding a metric adds rows,
not an `awk` column index. Human-readable logs go to stderr; stdout ends with one
small JSON result containing status, exit code, and report-relative path.

**Exit codes:**

| Code | Meaning |
|---|---|
| 0 | Completed pass/observation; inspect `authoritative` before using as evidence |
| 1 | Fail — assertion violated or image diff over threshold |
| 2 | Invalid usage or malformed manifest |
| 3 | Environment unavailable — no GPU, no display, no timestamp support |
| 4 | Missing or incompatible required baseline/profile |
| 5 | Internal error, timeout, cancellation, or incomplete artifact |

The report `status` is one of `pass`, `fail`, `invalid`, `unavailable`,
`missing_baseline`, `error`, `cancelled`, or `incomplete`.
Separating failure, unavailable evidence, missing baseline, and tool failure
prevents an agent from reporting a renderer regression when the instrument or
evidence was absent. A completed non-authoritative observation may exit 0 with
`status=pass`, but `authoritative=false` and its diagnostics prohibit presenting
it as a gate or a performance result.

## 8. Agent integration

- New skill `.codex/skills/vkr-harness/SKILL.md`, symlinked into
  `.claude/skills/` per the existing tracked-symlink convention.
- Phase 0 repairs current factual drift in
  `.codex/skills/vkr-performance/SKILL.md`; do not leave invalid instruments or
  pre-P2 status in live agent guidance while the harness is only proposed.
- Phase 2b rewrites that skill to use the harness after parity with the legacy
  benchmark is established. Authoritative profiles require multiple isolated
  repetitions, and timestamp-enabled profiles require complete per-pass GPU
  samples.
- `AGENTS.md`: one row in the skill table, and an updated build/test command
  block.
- **Baseline rule, stated in the skill:** an agent never runs
  `vkr_harness baseline accept` unless the user explicitly asks. Ordinary
  `profile`, `snapshot`, `autotest`, and `compare` never mutate baselines.
- Candidate artifacts go to `build/_artifacts/`. Accepted performance and image
  evidence is profile-scoped under `tools/baselines/` and remains a reviewed git
  change.

Git review governs the resulting repository change, but it does not prevent a
tool from overwriting evidence before review. Promotion is therefore two-step:

1. `baseline propose --from <complete-run>` verifies profile compatibility,
   relative paths, report/artifact digests, and writes a no-mutation plan listing
   exact sources, destinations, prior digests, and new digests.
2. `baseline accept --plan <plan> --confirm-sha256 <digest>` re-verifies every
   input and writes an immutable, content-addressed generation under
   `tools/baselines/<profile>/<case>/generations/<digest>/`. Only after every
   file and digest is durable does it atomically rename a small `current.json`
   pointer to that generation and record reason/actor/prior digest. Any change
   since proposal invalidates the confirmation. Failure before the pointer swap
   leaves the prior baseline active; an unreferenced generation is safe to
   inspect or remove in a later cleanup.

This is evidence integrity and time-of-check/time-of-use protection, not a
second product-scale approval service. Manual copying into baseline roots is
unsupported.

## 9. Phasing

| Phase | Deliverable | Touches |
|---|---|---|
| 0 | This document, ADR-014, ADR-015, and immediate factual repair of the live `vkr-performance` skill | `docs/`, `.codex/skills/vkr-performance/SKILL.md` |
| 1 | **Implemented 2026-08-01.** Explicit `VkrMetrics` owner, core registry, renderer adapter, catalog/validity, JSON writer; HUD reads snapshots; `--metrics-json` dump | `lib/src/core/`, `lib/src/renderer/`, `application.h`, `app/src/main.c` |
| 1b | **Implemented 2026-08-01.** Explicit logical GPU allocation owners; tracker live/peak/lifetime totals; live/peak gauges and allocated/created interval counters; no inferred font/mesh categories | resource descriptions, Vulkan allocation tracker, renderer metrics adapter |
| 2 | **Implemented 2026-08-01.** Shared harness runtime, strict case/profile parsers, case/profile/report schemas, three fingerprints, safe paths/atomic artifacts, camera scripting, isolated repetitions, `profile` | `tools/harness/`, `tools/cases/`, `tools/profiles/` |
| 2b | **Implemented 2026-08-02.** Established structured metric/pass parity, added reviewed CPU and GPU-timestamp performance profiles plus a deterministic Sponza case, rejected one-process authoritative profiles, made requested GPU timing completeness-gated, retired the grep/awk script/app accumulator, and migrated `vkr-performance` | tooling and skills |
| 3 | **Implemented 2026-08-02.** Dependency-resolved automation boot; actual effective masks in samples/reports/fingerprints; paired full/automation boot and residency profiles with identical work-volume gates | `renderer_frontend.c`, `application.h`, harness runtime |
| 4 | **Implemented 2026-08-02.** Capture batch API and fixed ring, request-specific declared exact-slice graph reads, capability-gated transfer sources, canonical converters and metadata, isolated direct-channel `snapshot` replays | backend, graph, `tools/harness/` |
| 5 | **Implemented 2026-08-02.** Logical auxiliary debug replays; canonical color/depth/ID comparison and diffs; primary-plus-snapshot `autotest`; profile-scoped immutable baseline generations; no-mutation proposals and digest-confirmed atomic promotion | `tools/harness/`, `tools/baselines/` |
| 6 | **Implemented 2026-08-02.** Target-neutral queries and graph imports; graph-owned terminal barriers; surface/swapchain/present-free ordinary-image targets; retained per-image state; explicit recreation; actual target provenance; windowed/offscreen work and capture equivalence | backend configuration/device selection, graph imports, frame path, harness runtime |

Phases 4 and 6 both touch GPU-completion and frame-failure invariants: phase 4
now associates/rolls back asynchronous readback slots, while phase 6 changes
acquire/submit/present synchronization. Phase 6 remains last so the windowed
harness can validate it. After each implementation phase, update this document's
front-matter, both indexes, the architecture specification's feature table and
verification record, and the relevant ADR status in the same change.

## 10. Verification

### CPU suite (`./build_test.sh`) — all GPU-free

- **metrics:** counter, gauge, and duration aggregation; concurrent-slot
  atomicity under contention; a worker crossing two frame boundaries is counted
  once without writing a recycled frame; validity/inexactness propagation;
  snapshot pin/release and non-blocking publication-drop behavior; event-ring
  publication, truncation, and exact overflow accounting.
- **GPU owners:** allocation/free attribution, colliding-handle deletion and
  reinsertion, live/peak/total owner aggregates, invalid-owner normalization to
  `unknown`, saturation/inexact behavior, and owner catalog names/units.
- **JSON/artifacts:** escaping, length-prefixed strings, nesting, non-finite
  rejection, interrupted-write behavior, atomic rename, digest verification,
  and absolute/`..`/symlink-escape rejection.
- **case manifest:** schema-valid example; defaults applied by parser; rejects an
  unknown/duplicate channel, missing/escaping scene path, bad target/present
  pair, unordered camera keys, capture beyond the measured window, and a
  negative frame count.
- **reports:** schema validation; fingerprint field order independence;
  provenance changes do not change comparison identity; every effective config
  change does; incompatible/missing/inexact evidence cannot pass;
  authoritative profiles reject fewer than two independent repetitions; a
  requested GPU timing series is incomplete unless every executed pass CPU
  sample has a matching GPU sample.
- **camera:** the same case produces an identical pose sequence across two runs.
- **capture state:** batch-slot state transitions, `BUSY`, release ownership,
  submit association, and cancel rollback.
- **image compare:** BGRA/origin conversion fixtures; depth-format to canonical
  float fixtures; identical inputs produce zero diff; a known delta produces the
  expected counts; `picking_ids` rejects one identifier difference exactly.

### GPU and integration

- **Validation layers** are required for phases 4 and 6. Cover color, depth, one
  array-layer capture, normal completion, ring pressure, packet/graph cancel,
  submit failure injection, and release. A green CPU suite proves none of the
  Vulkan transitions or completion lifetime.
- **Determinism:** run at least two isolated processes of one case. Work-volume
  metrics and direct capture digests must match; timing is reported with spread,
  not required to match.
- **Overhead:** the five balanced AB/BA process-pair minimum in §3.7 with full
  environment/workload/policy fingerprints.
- **Boot:** phase 3 reports `boot.*` and resident CPU/GPU bytes for `full`
  versus `automation`, with the effective subsystem masks.
- **Selected implementations:** run a focused validation-enabled lifecycle case
  on each claimed platform implementation. Phase 6 additionally covers at least
  two offscreen image counts and available two/three/four-image windowed
  configurations, resize/minimize on windowed, and target recreation on
  offscreen. The Vulkan 1.2 threading matrix was retired with V7.
- **Target equivalence:** the same case on windowed and offscreen targets has
  bit-identical work-volume metrics and compatible canonical captures. Timing is
  not compared across targets.
- **No regression in the existing app:** `./build_run.sh` still renders Sponza
  with its HUD, now sourced from metrics.

## 11. Open questions

- **Attachment `TRANSFER_SRC` cost/capability** (§5.3). Format/image and
  windowed-color support must be queried, and ordinary versus capture-capable
  images must be measured on the pinned driver stack. Neither zero cost nor
  disabled compression is assumed.
- **Report evolution.** Version 1 readers reject unknown major versions. Add a
  version only with a second real shape; do not build migration machinery in
  advance.
- **Cross-machine baselines.** Visual baselines are profile-scoped
  (`local-macos-mvk`) because GPU, driver, and OS all affect output. Whether a
  shared CI profile is achievable remains unanswered and depends on native
  Vulkan/cross-vendor evidence plus an explicitly pinned CI profile.
- **Surface-free native-Vulkan coverage.** VKR's surface-free instance/device
  selection, queue policy, formats, recreation, and validation behavior are
  verified on the local Apple M1 Pro/MoltenVK stack. Native Vulkan and
  cross-vendor coverage remain open; the local dirty-tree runs are correctness
  evidence, not an authoritative performance result.
