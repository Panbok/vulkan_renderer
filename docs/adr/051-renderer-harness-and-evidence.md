---
status: implemented
updated: 2026-09-07
authority: adr
---
# ADR-051: Isolated harness runs and reviewed capture baselines

## Status

Accepted. Consolidates the completed metrics/harness implementation series.
Current executable behavior is defined by the CLI, manifest parsers, and report
writers linked below; historical phase reports are available in Git history.

## Context

Performance runs, visual diagnostics, and accepted reference images have
different evidence requirements. Capturing or replaying diagnostic passes can
change the workload. A successful process alone proves neither comparable
measurements nor matching pixels.

## Decision

Cases own the scene, deterministic camera, target, cache policy, fixed delta,
warmup, measured frames, and captures. Profiles own repetitions,
instrumentation, environment constraints, stability, and authority requirements.
The parent launches isolated children and records effective configuration and
build/device provenance alongside workload, policy, and environment fingerprints.
The camera script is versioned: warmup holds its initial pose and measured frame
zero starts the authored path. Version 4 starts warmup at the common zero of the
eight-phase raster jitter and 64-phase GTAO noise sequences, invalidating temporal
history at that boundary. Aligning raster jitter alone left captures dependent
on bootstrap duration through ambient-occlusion noise.
Waiting for that phase remains bootstrap work; authored frame counts and GPU
completion rules do not change. The workload fingerprint includes this version,
so results from the previous replay behavior are incompatible.

Editor cases may set `renderer.editor_stop_frame` and `editor_resume_frame` to
exercise retained Scene presentation. Their zero-based indices include authored
warmup and exclude bootstrap; stop at zero also suppresses bootstrap Scene work.
Resume must follow stop. Captures during the stopped interval support only
`final_color`. These optional controls enter the workload fingerprint when set.
Capture-summary version 7 stores them; readers migrate versions 2–6 with both
controls unset so older captures preserve their original behavior.

`profile` collects capture-free repetitions. `snapshot` runs replay children,
produces canonical captures with metadata and digests, and compares compatible
baselines. `autotest` keeps these two results separate. `compare` rechecks a
completed snapshot. Offscreen cases use ordinary images without a window or
swapchain; automation boot alone does not imply an offscreen target.

The child owns its large application record in the repetition arena, releases
application resources before publishing reports, and destroys the arena after
publication. This keeps nested fingerprint/report calls within the Windows
executable's default stack. The parent logs abnormal child exit codes alongside
the child artifact directory.

Scene readiness requires successful streamed textures. A terminal texture request
failure reports `scene.texture_load_failed` before testing pending streams, so
optional interactive fallback materials cannot silently become complete capture
or profile evidence. Pending work without a terminal failure retains the bounded
readiness timeout. Before authored warmup begins, a typed frame OOM may retry only
when Application has scheduled an uncommitted bounded Scene-reduction generation.
That failed frame contributes no samples and cannot start capture or measurement.
Other frame errors and errors during authored phases remain terminal. This lets
the harness observe the app's approved startup recovery without treating failed
frames as rendering evidence.

The child consumes pinned metrics snapshots, checks required sample validity,
collects completed GPU timings by source serial, and records bounded events.
Metal has both per-pass timestamp collection and submission feedback; unavailable
or unsupported results remain explicit. A bounded completion drain after the
measurement window does not turn missing results into valid zero durations.
Work-volume and stability checks govern timing authority independently of
execution success.

Run artifacts live under `build/_artifacts/`. Ordinary runs do not change
`tools/baselines/`. `baseline propose` writes a digest-addressed review plan;
`baseline accept` verifies its confirmation digest, source artifacts, and prior
generation before publishing an immutable generation and atomically replacing
`current.json`. Cross-backend comparison is explicit and still requires matching
workload and policy fingerprints.

## Metal crash diagnostics

`VKR_METAL_DIAGNOSTICS_DIR` opts into a separate CPU observation log. The renderer
creates a new directory exclusively; existing directories are rejected without
overwriting prior evidence. One renderer-owned sink writes two rotating 4 MiB
JSONL segments, with bounded 4 KiB records. The memory adapter borrows the sink
until adapter destruction. No worker or submission-feedback callback writes it.
With the option unset, diagnostic code does not open files, format records, or
query extra completion counters.

Records cover queue submissions, completion waits, command-slot reuse, planned
passes, Scene running/stopped state, and GPU resource creation/retirement/collection.
`seq` orders both segments. `submitted` is the renderer's serial counter, which
is assigned before commit; a begin record does not prove submission succeeded.
`completed` is an observed completion value. Memory events use zero for counters
the memory adapter does not know; their explicit `retire_after` field is the
retirement deadline. Planned passes and CPU commit returns do not prove GPU
execution. Text truncation is marked, and rotation removes older history.

Submission, wait, idle and shutdown boundaries request `fsync` for both segments.
This does not guarantee that the last record survives a kernel panic. An I/O
failure reports once and disables diagnostics without changing renderer work.
The added I/O perturbs scheduling, so these logs cannot support performance claims.
[`inspect_metal_diagnostics.py`](../../tools/inspect_metal_diagnostics.py) reads
both segments in sequence order and reports incomplete records without launching
a renderer.

The empty-stopped and tiny-node transport diagnostic cases are prepared for
separate, bounded native runs; CPU manifest checks do not establish GPU stability.

## Consequences

A report can pass execution yet be non-authoritative for performance. Capture
replay timing cannot substitute for the primary profile. Accepted generations
are self-contained; pending promotion still depends on its source run. Manifest
parsers and checked-in cases/profiles are the maintained input contract, avoiding
independent descriptive schemas that diverge from runtime validation.

## Alternatives considered

Log scraping discards provenance and validity. A single run mixing capture and
profiling measures a different workload. Copying images directly into a baseline
bypasses review and artifact consistency checks.

## Revisit when

A new channel needs different canonical encoding, a report contract changes, or
a benchmark cannot establish comparability with the current fingerprints.

## Implementation and operation

- [CLI](../../tools/harness/vkr_harness_main.c),
  [manifest validation](../../tools/harness/vkr_harness_manifest.c),
  [cases](../../tools/cases/), [profiles](../../tools/profiles/).
- [Child collection](../../tools/harness/vkr_harness_child.c),
  [report writer](../../tools/harness/vkr_harness_report.c),
  [capture catalog](../../tools/harness/vkr_harness_capture.c).
- [Baseline publication](../../tools/harness/vkr_harness_baseline.c),
  [comparison](../../tools/harness/vkr_harness_compare.c).
- [Commands and artifact handling](../../.codex/skills/vkr-harness/SKILL.md),
  [performance evidence](../../.codex/skills/vkr-performance/SKILL.md).
