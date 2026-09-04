---
name: vkr-performance
description: Investigate frame cost or hitches, optimize renderer hot paths, inspect timings or GPU traces, and validate before/after performance claims.
---

# VKR performance

Performance is correctness. Preserve ownership, lifetime, GPU completion,
visible output, and required work while reducing measured cost.

## Measurement loop

1. Name the expensive operation, expected metric, frame budget, and invariant
   the change must preserve. Inspect current code and the capability/gap sections in
   `docs/ARCHITECTURE.md` before treating a known limitation as a new defect.
2. Run the smallest representative Release case. Use local observations to
   locate cost; use authoritative matched runs to support a speed claim.
3. Attribute the cost before changing code. CPU scopes locate host work;
   valid pass timestamps locate GPU work. Read [GPU-TRACE.md](GPU-TRACE.md)
   only when hardware counters or encoder attribution are needed.
4. Change one attributable cause. Preserve the measured workload or state
   the intentional change. Select correctness checks through `vkr-validation`.
5. Rerun the same configuration. Compare valid samples and spread, work-volume
   rows, and affected captures. Investigate an unexplained change before
   claiming improvement. Stop when the stated gate passes; do not add broad
   suites or speculative tests.

## Commands and authority

Build with `./build_release.sh` when needed. Use `vkr-harness` to run and inspect:

```sh
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_orbit.case.json \
  --profile tools/profiles/performance-windowed.json

./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_orbit.case.json \
  --profile tools/profiles/performance-windowed-gpu.json
```

Run with Metal and Vulkan validation variables unset. Debug, sanitizers,
validation, GPU traces, and capture replays are diagnostic configurations.
Their timings cannot establish normal Release performance.

Read the chosen profile JSON. The above profiles require five independent
children, clean source, stable warmup, actual immediate presentation, exclusive
GPU use, complete required metrics, and deterministic work volume. GPU timing
adds instrumentation and requires complete pass availability accounting;
`unsupported_timestamp_scope` is accounted for but supplies no duration.

A speed claim requires `status=pass`, `authoritative=true`, empty
`authority_reasons`, and matching environment/workload/policy fingerprints.
Match build/compiler, GPU/driver, resolution, assets, target/image count,
presentation, editor/features, cache policy, and instrumentation. Compare
GPU-timestamp-on with timestamp-on. FIFO can hide CPU savings behind refresh
pacing; inspect the relevant CPU work scope as well as `frame.wall`.

A dirty-tree or single-process observation may guide implementation. Label it
non-authoritative and do not convert it into a claimed speedup. If a required
environment or owner decision is missing, state that immediately; do not
silently weaken the gate or leave an unresolved architecture decision at the
end of a document.

## Instruments

Use the current metric catalog in `lib/src/renderer/vkr_renderer_metrics.c`
and actual report rows. Do not copy historical module/counter inventories into
new work.

| Need | Instrument and limit |
|---|---|
| CPU work or queue-slot waits | Harness CPU metrics; inspect scope definitions before interpreting duration |
| GPU pass duration | `vkr_rg_get_pass_timings()` in `vkr_render_graph.h`; use `gpu_ms` only when `gpu_valid`, for a completed buffered frame |
| Draw, visibility, overflow equivalence | Harness work-volume rows plus relevant captures; fewer calls alone does not prove equivalent work |
| Upload stall attribution | Harness upload metrics; `vkr_renderer_get_and_reset_upload_wait_stats()` consumes counters, so do not read it in competition with the metrics collector |
| Graph resource churn | `vkr_rg_get_resource_stats()`; a higher peak is a clue, not proof of recreation or a leak |
| CPU/GPU resident memory | Harness memory rows and `memory.gpu.live_totals_exact`; use `vkr-memory` to distinguish allocator accounting from live ownership |
| GPU limiter or occupancy | Instruments trace on Metal; counters suggest a cause and do not establish a speed claim |

For full/automation boot comparison, use
`tools/cases/performance/sponza_boot_full.case.json` and
`tools/cases/performance/sponza_boot_automation.case.json` with
`tools/profiles/performance-windowed-boot.json`. Boot and subsystem masks are
intentional workload-fingerprint differences. Require all other inputs and
deterministic work rows to match. Compare boot/residency metrics, record both
masks and memory exactness, and do not present steady-state timing across those
masks as an ordinary matched frame comparison.

## Reporting template

```text
Change and invariant: <operation changed; ownership/completion/output preserved>
Configuration: <Release, compiler, GPU/driver, scene, extent, target/image count,
                present mode, features, cache, instrumentation>
Evidence: <exact commands; report SHA-256; comparison fingerprints; authority>
Samples: <independent children, warmup/measurement windows, validity>
Before/after: <metric, values, spread, delta; relevant work and capture checks>
Coverage: <measured scope and any unavailable gate>
```

Transcribe results before removing this task's run directories and traces.
`vkr-harness` owns cleanup and pending-baseline retention rules.
