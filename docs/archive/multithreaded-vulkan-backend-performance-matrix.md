---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../../.codex/skills/vkr-performance/SKILL.md`](../../.codex/skills/vkr-performance/SKILL.md). Retained for history; do not treat as current.
# Multithreaded Vulkan Backend Performance Matrix

This document defines the repeatable performance-validation flow for the active
parallel-upload rollout path.

Render-record parallelization has been retired from the active implementation
track, so the benchmark matrix now compares serial vs upload-parallel only.

## Runner

- Script: `tools/benchmark_multithreaded_backend.sh`
- Output root:
  `build/_validation/multithreaded_backend/perf`
- Logs:
  `build/_validation/multithreaded_backend/perf/logs`
- Summary CSV:
  `build/_validation/multithreaded_backend/perf/summary.csv`

## Runtime Cases

- `serial`:
  `VKR_PARALLEL_UPLOAD=0`
- `upload_only`:
  `VKR_PARALLEL_UPLOAD=1`

## Benchmark Telemetry

The app emits environment-controlled benchmark logs:

- `BENCHMARK_SAMPLE ...` at the existing FPS update interval.
- `BENCHMARK_SUMMARY ...` on shutdown with:
  - `avg_frame_ms`
  - `min_frame_ms`
  - `max_frame_ms`
  - `avg_rg_cpu_ms`
  - sample counts

Environment controls used by the runner:

- `VKR_BENCHMARK_LOG=1`
- `VKR_BENCHMARK_LABEL=<case>`
- `VKR_RG_GPU_TIMING=1`
- `VKR_AUTOCLOSE_SECONDS=<seconds>`
- `VKR_PARALLEL_UPLOAD=<0|1>`

## Commands

Smoke:

```bash
VKR_BENCH_AUTOCLOSE_SECONDS=2 VKR_BENCH_MAX_WAIT_SECONDS=20 \
tools/benchmark_multithreaded_backend.sh --smoke
```

Full:

```bash
tools/benchmark_multithreaded_backend.sh
```

## Latest Run (2026-02-09)

- Full matrix: `PASS=2`, `FAIL=0` (serial + upload-only cases).
- Final numbers should be collected from local desktop runs using the same
  scene/profile for both cases.

## Gate Usage

For Phase D decision:

1. Run full matrix on the same machine profile used for baseline docs.
2. Compare `serial` vs `upload_only`.
3. Verify upload-path changes do not regress steady-state frame time.
4. Verify scene-load and hot-reload paths still show expected upload
   responsiveness improvements.
