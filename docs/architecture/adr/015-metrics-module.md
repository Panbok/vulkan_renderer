---
status: implemented
updated: 2026-08-02
authority: adr
---
# ADR-015: Centralized Metrics Registry with Pre-Registered Slots

**Status:** Accepted

## Context

The renderer measures a great deal and exports almost none of it.

Each signal lives in its own structure with its own reader and its own lifetime
rule. `VkrRendererFrameMetrics` is an out-parameter of
`vkr_renderer_submit_packet()`. `VkrVisibilityStats` is written during payload
construction and stored on `Application`. `vkr_rg_get_pass_timings()` returns a
borrowed array valid until the next execute. `VkrDeviceMemoryStats` is a
non-resetting snapshot. `vkr_renderer_get_and_reset_upload_wait_stats()`
**resets** on read. `VkrAllocatorStatistics` is per-allocator with a separate
global set behind atomics. `VkrPipelineRegistry.stats` is a bare anonymous
struct. Pipeline creation is timed in `vulkan_pipeline_create_graphics()` and
then discarded into a `log_info`.

A consumer wanting a frame's worth of data must know seven access patterns, four
lifetime rules, and which single call is destructive. The only cross-process
transport is a formatted log line recovered with `grep` and `awk`. The existing
data also has validity that must survive consolidation: GPU timestamps can be
unsupported or not ready, heap budgets can be unavailable, and device-memory
live totals become inexact if their handle table saturates. Missing or inexact
data must not become a plausible zero.

Two properties of this codebase constrain any fix:

- **Hot-path cost is a correctness question.** `AGENTS.md` treats a per-draw heap
  allocation, lock, or string construction as a defect. A metrics API that hashes
  a name per sample, or takes a mutex, is not acceptable at draw granularity
  regardless of how convenient it reads.
- **Writers are on several threads.** Asset loaders and parallel upload run on
  job-system threads; draw submission and graph execution run on the render
  thread. A single synchronization policy would either be unsafe for the former
  or needlessly expensive for the latter.
- **Frame ownership and worker lifetime differ.** A job started in frame N can
  complete in frame N+1. Atomic increments make a value race-free; they do not
  make clearing or reusing a frame buffer safe while a worker still targets it.
- **Core cannot depend on renderer internals.** The storage module belongs in
  `core/`, but pulling `RendererFrontend`, render-graph, and backend structures
  from there would invert the repository's dependency direction.

## Decision

Introduce `lib/src/core/vkr_metrics.{h,c}`: an explicitly owned registry of
**pre-registered typed slots addressed by a 32-bit handle containing a
`uint16_t` slot index and `uint16_t` registry generation**. `Application` creates
it before renderer initialization and passes the pointer to subsystems that
publish metrics. There is no process-global registry; tests and tool runs can
therefore create and destroy isolated instances.

```c
typedef uint32_t VkrMetricId; /**< generation:16 | slot:16 */
typedef struct VkrMetricsFrame VkrMetricsFrame;

typedef struct VkrMetricsSnapshotView {
  const VkrMetricsFrame *frame;
  uint32_t buffer_index;
  uint64_t publication_serial;
} VkrMetricsSnapshotView;

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

The contract, in order of importance:

1. **Registration is an initialization-time operation and never appears in a
   frame path.** Names are copied into registry-owned storage, descriptors are
   validated, duplicate names and exhaustion fail startup, and
   `vkr_metrics_seal()` makes the descriptor table immutable. The write path is
   an indexed operation — no
   hash, lookup, allocation, string construction, or kind check in a proven hot
   loop. Debug builds assert an ID's generation, kind, scalar, calling thread,
   and writer policy.
2. **The catalog is stable data.** Names use lowercase dotted segments, units
   are a `VkrMetricUnit` enum rather than arbitrary strings, and every descriptor
   records domain, kind, scalar type, writer policy, and whether absence makes a
   report incomplete. Renaming a metric is a report-schema change. Per-resource
   names do not become slots; cardinality stays bounded.
   Names carry no unit suffix: the descriptor's unit is the contract, durations
   are always nanoseconds, and there is no millisecond unit for a name to
   disagree with. Cumulative sources are published as per-frame counter deltas
   rather than running `_total` gauges, because averaging a running total
   describes nothing. Concurrent gauges are
   `u64` only; a floating atomic is not assumed lock-free. Concurrent ratios are
   derived from integer sum/count slots when the frame is finalized.
3. **Renderer-thread and concurrent storage are different on purpose.**
   Renderer-thread slots write plain per-frame accumulators. Concurrent slots
   hold cumulative atomics and never target a reusable frame buffer: counters
   use `fetch_add`, gauges use atomic exchange, and durations store cumulative
   integer nanosecond sum/count. At `vkr_metrics_end_frame()` the render thread
   snapshots those values and derives interval deltas. Cumulative minima and
   maxima cannot be differenced, so concurrent duration slots do not claim exact
   per-frame extrema; a producer that needs per-operation extrema emits bounded
   events for run-level aggregation. A job may cross a frame boundary without
   racing a clear.
4. **Publication has one owner and never overwrites a reader.** Only the render
   thread finalizes a `VkrMetricsFrame`. It publishes into one of three fixed
   snapshot buffers; readers pin the current buffer with an atomic reader count
   through `snapshot_acquire()` and must call `snapshot_release()`. The writer
   selects a non-current buffer whose reader count is zero, fills it, then
   publishes its index with release semantics. If both candidates are pinned it
   increments a publication-drop counter and leaves the prior frame current; it
   never waits. This is a data-race-free ownership contract, not the unsafe
   claim that double buffering or a sequence counter makes non-atomic payload
   reads safe in C.
5. **Synchronization is per slot, not global.** A slot registered with
   `VKR_METRIC_WRITER_RENDER_THREAD` is plain. A slot registered concurrent uses
   `core/vkr_atomic.h`. The registry rejects incompatible publication calls in
   Debug. A global always-atomic policy would put lock-prefixed instructions in
   draw submission to serve cold asset-loader metrics.
6. **Existing aggregates are pulled by their owning layer.**
   `lib/src/renderer/vkr_renderer_metrics.{h,c}` registers and samples render
   graph, frontend, pipeline, visibility, upload, and device-memory metrics.
   `core/vkr_metrics.c` never includes renderer headers. The renderer adapter is
   the **single caller** of
   `vkr_renderer_get_and_reset_upload_wait_stats()`; a second caller would steal
   samples.
   Device-memory rows include fixed logical-owner state copied from the Vulkan
   handle tracker. Live and peak values are gauges; the tracker's lifetime
   allocation totals are differenced into per-frame `bytes.allocated` and
   `allocations.created` counters. The owner is typed creation data and is never
   derived from a metric name, resource debug name, usage flags, or memory type.
7. **Allocator snapshots obey allocator ownership.** Global allocator totals
   may be pulled through `vkr_allocator_get_global_statistics()`, whose storage
   is atomic. `vkr_allocator_get_statistics()` returns non-atomic local fields
   and may only be sampled on the allocator's owner thread at a documented safe
   point. Worker-owned allocators publish their own snapshot or remain represented
   by global totals; the renderer collector never reads them concurrently.
8. **Availability is data.** Each frame sample carries `valid`, `inexact`, or
   `unavailable`, plus a stable reason code. GPU timing is associated with the
   completed source frame/submit serial rather than whichever CPU frame happens
   to collect it. `gpu_valid == false`, unavailable heap budgets, and
   `live_totals_exact == false` propagate into the report and can invalidate a
   required gate; none serialize as a valid zero.
9. **Unbounded-cardinality signals use a bounded MPSC event ring, not slots.**
   Asset loads and pipeline creations use 4096 preallocated records with a
   sequence-number publication protocol. Each record stores a registered source
   ID and a bounded inline subject copied on the cold path; no event retains a
   borrowed `String8` and no concurrent arena interning is required. Subject
   truncation is explicit, and `VKR_METRIC_EVENT_SUBJECT_MAX` is at most 255 so
   its length fits the record. Overflow increments `metrics.events_dropped`,
   which every report includes; a gate requiring complete events becomes
   incomplete after any drop.
10. **Cost is bounded and measured.** `VKR_METRICS_ENABLED=0` compiles writer
    calls away. GPU timestamps and event subjects are runtime-controlled and off
    for authoritative timing unless the comparison profile enables both sides.
    The always-on set ships only after an interleaved, same-configuration Release
    A/B with at least five balanced AB/BA process pairs shows no regression above
    the 1% budget; an inconclusive result leaves collection runtime-disabled
    rather than being reported as free.

The core registry provides storage and publication. Domain adapters own the
knowledge required to produce a metric, and the harness owns aggregation and
serialization. This keeps one representation per fact without making `core/`
depend on every producer.

## Consequences

**Positive**

- One immutable snapshot contract for consumers, replacing seven bespoke
  access/lifetime rules.
- Adding a metric is one `register` and one `add`, not an edit to a `snprintf`,
  a `grep`, and an `awk` field index across three files.
- Structured output becomes possible, which is what lets an automated workflow
  compare runs instead of eyeballing log lines.
- The destructive-read hazard in the upload-wait counters becomes a stated
  single-owner rule instead of an undocumented trap.
- The application HUD and the automation harness read the same data, so the
  overlay is a continuous check that collection works.
- Unsupported and inexact instruments remain distinguishable from zero.
- Worker jobs can cross frame boundaries without racing publication or clear.
- A slow diagnostics consumer cannot block the render thread or observe an
  overwritten snapshot.

**Negative / risks**

- Pre-registration trades ergonomics for cost. A metric cannot be created ad hoc
  at a call site; it must be declared first. This is deliberate and will
  occasionally be inconvenient.
- `VKR_METRICS_MAX_SLOTS` is a fixed bound, sized to hold the fixed set plus
  the worst-case device-dependent rows. Exhaustion logs loudly and drops the
  rows that do not fit; it does not fail startup. Instrumentation that can
  refuse to boot the renderer on an unusual memory layout is a worse defect
  than a report missing per-heap rows.
- Writer policy is a correctness obligation on whoever adds a slot. A job-thread
  write to a render-thread slot is a data race; registration ownership and a
  contended test are required.
- Concurrent frame deltas are assigned at the collection boundary, not the
  worker's start frame. Per-operation attribution belongs in the event stream.
- Concurrent duration slots provide exact sum/count deltas but not exact
  interval min/max; requiring extrema consumes bounded event capacity.
- The pull collector adds a fixed per-frame cost even when nothing changed.
  Bounded and measured, but not zero.
- Three publication buffers bound reader overlap. A consumer that pins two old
  buffers can cause a publication drop, which reports must surface.
- A 4096-entry event ring is intentionally lossy. Reports must treat drops and
  subject truncation honestly rather than promising a complete trace.

## Alternatives Considered

- **String-keyed hash map** (the conventional metrics-library shape). Ergonomic:
  `metrics_add("draw.calls", 1)` needs no registration. Rejected because it puts
  a hash and a lookup in the per-draw path, which this project classifies as a
  defect rather than a trade-off.
- **Leave the structures as they are and write a serializer.** Smaller change,
  no new module. Rejected because it preserves all seven access patterns and
  four lifetime rules, and every new signal still needs bespoke plumbing — it
  moves the problem into the serializer instead of solving it.
- **Compile-time slot enumeration** (an X-macro list of every metric). Removes
  the runtime bound and makes IDs constants. Rejected because subsystems must be
  able to register the metrics they own without editing a central header, and
  because per-pass metrics are dynamically sized by the graph.
- **Worker writes into `frames[frame_number & 1]`.** Appears lock-free, but a
  worker can retain the old index while the render thread publishes and clears
  it. Rejected because atomic slot writes do not make buffer reuse safe.
- **Concurrent name interning for events.** Preserves arbitrary subjects but
  requires shared allocation and synchronization on loader threads. Rejected in
  favor of fixed inline subjects with explicit truncation.
- **Tracy or another external profiler.** Mature UI, deep timeline analysis. Out
  of scope here: it does not solve structured reporting for automated
  comparison, it adds a substantial dependency, and its instrumentation
  perturbs the workload it measures — nuri classifies its own Tracy captures as
  explicitly non-authoritative for that reason. Not precluded later; the two
  answer different questions.
- **Always-atomic counters.** One rule, trivially safe. Rejected on the hot-path
  cost that motivates the per-slot flag.

## Implementation Record

Accepted on 2026-08-01. The registry has production application, renderer,
backend, resource-loader, shader, pipeline, job, and instance-stream writers;
the HUD, atomic JSON dump, and Phase-2 harness consume its snapshots. The
harness drains bounded events, samples the provenance-carrying pass table beside
the matching published frame, and refuses required invalid samples or snapshot
drops instead of manufacturing zeros. The required Release A/B,
environment, spread, compile-disabled build, CPU tests, Vulkan validation, and
pipeline-cache integration are recorded in
[the phase-1 verification](../../tooling/renderer-metrics-phase1-verification.md).
Harness integration evidence is in
[the phase-2 verification](../../tooling/renderer-harness-phase2-verification.md).
Phase 2b established parity with and removed the duplicate application
benchmark accumulator and grep/awk transport; reviewed CPU/GPU profiles and the
evidence are recorded in
[the phase-2b verification](../../tooling/renderer-harness-phase2b-verification.md).
Phase 3 wires scene-readiness boot duration and resident memory rows into paired
full/automation profiles. The harness persists the renderer-reported subsystem
mask in raw samples, reports, and workload fingerprints; implementation and
observational evidence are recorded in
[the phase-3 verification](../../tooling/renderer-harness-phase3-verification.md).

## Revisit When

Revisit `VKR_METRICS_MAX_SLOTS` if the degradation path is ever taken on real
hardware; the fixed bound is a simplification, not a principle. The worst case
is dominated by per-memory-type, per-heap, and per-cascade rows, which are
registered for the maxima rather than the configured counts.

Revisit the snapshot publication model if a consumer appears that needs
sub-frame granularity. A real timeline, rather than per-frame aggregates plus a
bounded cold-event ring, is a different data structure and warrants its own
decision rather than an extension of this one.
