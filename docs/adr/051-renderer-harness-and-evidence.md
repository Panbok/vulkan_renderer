---
status: implemented
updated: 2026-09-05
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
zero starts the authored path.

`profile` collects capture-free repetitions. `snapshot` runs replay children,
produces canonical captures with metadata and digests, and compares compatible
baselines. `autotest` keeps these two results separate. `compare` rechecks a
completed snapshot. Offscreen cases use ordinary images without a window or
swapchain; automation boot alone does not imply an offscreen target.

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
