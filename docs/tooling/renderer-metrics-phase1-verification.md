---
status: investigation
updated: 2026-08-01
authority: investigation
---
# Renderer Metrics Phase 1 Verification

This record verifies phase 1 of
[renderer-harness-and-metrics-spec.md](renderer-harness-and-metrics-spec.md).
It is implementation evidence for the metrics registry and adapter only; it is
not evidence that the later harness, capture, automation-boot, or offscreen
phases ship.

## Configuration

- Source base: `f9a525a265a786e6d4ffffd7b05e37f3ffcb9498`, dirty with the
  phase-1 implementation under test.
- Build: Release, Apple Clang 21.0.0, C11, `-march=native`; the only A/B build
  difference was `VKR_METRICS_ENABLED=0` versus `1`.
- Binary SHA-256: off
  `b9d98acd84dd4e91b3cd7f1de4aa0db8e36916c5287b375665405bd4b0c0561d`;
  on `21b651dc5274457fedd841af32d1bc176211d3b277999d4b1b0664092ccb1921`.
- Host: MacBookPro18,3, Apple M1 Pro (6 performance + 2 efficiency CPU
  cores, 14 GPU cores), 16 GiB, macOS 26.5.2 build 25F84.
- Vulkan: Apple M1 Pro through MoltenVK driver 0.2.2019, Vulkan 1.2.296,
  Vulkan SDK 1.4.313.0.
- Power/thermal: AC power, 80% battery, no recorded thermal, performance, or
  CPU-power warning. Default process priority; no competing renderer process.
- Workload: sample application's default one-cube scene and fixed camera,
  800x600 logical / 1600x1200 framebuffer pixels, windowed target, three
  images and three frames in flight, frame limiter disabled. The current target
  selects `MAILBOX_KHR` when advertised and otherwise `FIFO_KHR`; MoltenVK
  advertised mailbox for this successful run. Pipeline cache was shared and
  warm. GPU pass timing and event subjects were off in both variants.

## Overhead gate

Five isolated pairs used alternating BA/AB order. Each process ran for six
seconds. `BENCHMARK_SAMPLE` publishes 250 ms interval means; the first interval
was discarded as warmup, leaving 21 intervals per process. Delta is
`(enabled / disabled - 1) * 100`, so positive is a regression.

| Block | Order | Disabled mean ms | Enabled mean ms | Paired delta |
|---:|:---:|---:|---:|---:|
| 1 | BA | 8.397619 | 8.323952 | -0.877233% |
| 2 | AB | 8.318190 | 8.338429 | +0.243299% |
| 3 | BA | 8.399048 | 8.339905 | -0.704161% |
| 4 | AB | 8.328429 | 8.352048 | +0.283595% |
| 5 | BA | 8.341857 | 8.380429 | +0.462384% |

Median paired delta is **+0.243299%**; observed range is -0.877233% to
+0.462384%. This clears the 1% gate. It is evidence that the enabled set did
not regress this workload, not evidence that metrics made rendering faster.

## Functional and correctness gates

- `./build_test.sh`: exit 0, including typed aggregation, contended concurrent
  counter/gauge/duration publication, cross-frame deltas, four-producer MPSC
  event publication, registry-generation rejection, pinned-snapshot
  publication drops, exact event overflow, transactional event peek/consume,
  JSON escaping/state, non-finite rejection, abort, and atomic commit tests.
- Release symbol inspection found no out-of-line metric writer symbols in
  either binary; the enabled writers are static-inline indexed operations and
  the disabled writers preprocess to no-ops.
- `./build.sh Debug`: exit 0. The two emitted warnings are pre-existing
  `vkr_dmemory.c` C23-label and `vkr_rg_debug.c` const-qualification warnings.
- Validation-layer metrics run: exit 0 with no validation messages. The final
  snapshot contained 155 catalog slots, eight non-truncated pass rows with GPU
  source frame/submit provenance, actual memory-type/heap rows, and no snapshot
  or event drops.
- Sponza load: `boot.scene` recorded 3.750 seconds in Release;
  `boot.scene` became valid. The report preserved 205 events covering all
  five asset classes plus shader load/reflection and pipeline creation, with
  nonzero source bytes for each asset class, zero event drops, and one
  explicitly reported truncated texture subject.
- `VKR_METRICS_ENABLED=OFF ./build_release.sh`: exit 0.
- `VKR_SKIP_BUILD=1 ./validate_pipeline_cache.sh`: exit 0. Both cold and warm
  runs emitted 23 pipeline events; persisted cache load reduced their summed
  creation time from 54.947 ms to 16.361 ms.

Generated JSON and raw logs remain under `build/metrics_validation/` in the
local validation workspace; they are not baselines and are not committed.

## Review pass, 2026-08-01

A defect review of the phase-1 implementation produced the following
corrections. The overhead gate above predates them and has not been re-run;
the changes are structural and correctness-oriented, not hot-path additions,
but the paired A/B remains the only authority on overhead and should be
repeated before any phase-1 number is presented as a performance result.

Correctness:

- Every `*_ms` metric carried nanoseconds. Names now carry no unit suffix and
  `VkrMetricUnit` has no millisecond member, so a name cannot contradict its
  slot. This had to land before any baseline keyed off those names.
- The catalog held 256 slots against a worst case of 258 (146 fixed plus
  2/type and 3/heap at the Vulkan maxima of 32 and 16). Capacity is now 384,
  device-dependent rows clamp to what fits, and exhaustion logs and degrades
  instead of failing `application_create`.
- `job.queue_depth` was incremented on enqueue but not decremented when a
  stale handle was discarded from the queue, biasing the gauge upward
  permanently. `job.workers_busy` had the same asymmetry at shutdown.
- `VKR_ASSERT_NO_UPLOAD_WAITS` read an unavailable metric as zero waits. It
  now distinguishes missing evidence from measured zero and warns instead of
  passing, and detects publication-serial gaps that would silently lose a
  frame's waits.
- Pipeline creation, shader load, and asset load each had a different policy
  for failed operations. `VkrMetricEvent` gained a status field; failures
  publish an event but never enter the duration aggregate.
- `vkr_resource_system_metric_source_bytes()` dereferenced `path.str` before
  its own null check.
- The last frame was never published: the loop exited via `break` after
  `begin_frame` without `end_frame`, so `--metrics-json` always dumped frame
  N-1 on the auto-close path every automated run takes.
- `config.pass_gpu_timings` was reported but gated nothing; a second copy in
  `Application` was the real switch. The registry config is now the single
  authority, so a report cannot claim timestamps were on for a run that took
  none.
- Event subjects are truncated at a byte budget and could sever a UTF-8
  sequence; truncation now backs off to a codepoint boundary and the JSON
  writer validates sequences and substitutes U+FFFD rather than emitting a
  document strict parsers reject.

Contract:

- `VkrMetricSample` carries `kind` and `scalar`, and typed readers
  (`vkr_metrics_frame_read_u64/_f64/_duration`) refuse a union member the
  producer did not write. Consumers previously read `value.u64` unchecked.
- Cumulative pull sources publish per-frame deltas as counters rather than
  running totals as `_total` gauges. A counter nobody incremented publishes a
  valid zero.
- `required_when_enabled` is load-bearing: `vkr_metrics_frame_missing_required()`
  reports it and the JSON dump carries the count.
  `frame.limiter_sleep` is deliberately not required — the limiter is off for
  profiling, so requiring it would mark every authoritative run incomplete.
- The pass table is single-buffered and not part of the published snapshot. It
  now records its own CPU frame index and the report states whether it matches
  the pinned snapshot.

Evidence for this pass:

- `./build_test.sh`: exit 0, including new coverage for availability marking
  and INEXACT propagation, typed-reader kind rejection, event status
  separation, `event_subjects=false`, UTF-8-boundary truncation, and JSON
  writer depth-limit, UTF-8, and sticky-failure behaviour. Test fixtures now
  arena-allocate `VkrMetrics` rather than placing ~700 KB on the stack.
- `./build.sh Debug`, `./build_release.sh`, and
  `VKR_METRICS_ENABLED=OFF ./build_release.sh`: exit 0. Only the pre-existing
  `vkr_rg_debug.c` const-qualification warning.
- Validation-layer run: exit 0, no validation messages, 156/384 slots,
  `missing_required_metrics=0`, zero snapshot or event drops, pass table
  matching the published snapshot.
- Determinism: two isolated Release processes produced identical values for
  all 82 work-volume metrics.
- `VKR_SKIP_BUILD=1 ./validate_pipeline_cache.sh`: exit 0; 23 pipeline events
  in both runs, summed creation time 48.436 ms cold versus 15.687 ms warm.
