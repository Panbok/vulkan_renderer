---
name: vkr-harness
description: Run and interpret VKR's structured renderer automation harness. Use when executing a renderer case, selecting a harness profile, reading a harness report or summary.csv, checking deterministic repetitions, or deciding whether harness evidence is authoritative.
---

# VKR Harness

Use `vkr_harness` for structured, repeatable renderer observations. Phases 2-5
support the `profile` command with full or dependency-resolved automation boot
and visible or hidden windowed targets. Phase 2b adds authoritative CPU and
GPU-timestamp performance profiles and retires the old log-scraping benchmark;
Phase 3 adds paired boot/residency profiles and actual effective subsystem
masks. Phase 4 adds direct-channel `snapshot` replays with canonical color,
depth, shadow-layer, and picking-ID artifacts. Phase 5 adds forward-render
debug replays, canonical comparison and diff artifacts, separated `autotest`
orchestration, and profile-scoped guarded baselines. True offscreen targets are
Phase 6 and must not be claimed from this binary.

## Run a profile

```sh
./build_release.sh
./build_release/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_static.case.json \
  --profile tools/profiles/local-windowed.json
```

The final stdout line is one JSON object containing `status`, `exit_code`, the
repository-relative aggregate report path, and its SHA-256 digest. Child logs,
samples, per-repetition reports, the aggregate `report.json`, and long-form
`summary.csv` live under `build/_artifacts/profile/<run-id>/`.

Cases own the workload. Profiles own environment constraints, instrumentation,
minimum independent repetitions, stability policy, and authority policy. Do
not weaken a profile to make a run pass. Add a separate local profile for an
investigative environment.

## Run a snapshot

```sh
./build.sh Debug
build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_snapshot.case.json \
  --profile tools/profiles/local-windowed.json
```

Snapshot starts one isolated replay child per required replay configuration and preserves
the case's fixed delta, seed, warmup, camera, and cache policy. Its output lives
under `build/_artifacts/snapshot/<run-id>/`. Read `captures[]` for the resolved
producer, canonical encoding, source frame/submit serial, paths, digests,
thresholds, and comparison result; read `auxiliary_runs[]` to audit each child
report and its effective fingerprints. Replay-child timings are auxiliary and
never primary performance evidence.

Shipped direct channels are `final_color`, editor-only `scene_color`, `depth`,
`shadow_cascade_0` through `_3`, and `picking_ids`. Logical auxiliary channels
are `normals`, `unlit`, `lighting`, `shadow_debug_cascades`,
`shadow_debug_factor`, and `shadow_debug_depth`. Channels sharing one render
mode and checkpoint share a replay; distinct debug modes do not. Final-color
availability is target/surface capability dependent; unavailable means exit
`3`, never a silent substitute.

## Run combined autotest and comparison

```sh
build/tools/vkr_harness autotest \
  --case tools/cases/smoke/sponza_snapshot.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness compare \
  --run build/_artifacts/snapshot/<run-id>
```

`autotest` runs capture-free primary repetitions and snapshot replays as
separate nested reports. Its top-level report references both; it never merges
capture-replay timings into primary metrics or image verdicts into timing
assertions. `compare` verifies the source summary and artifact digests before
reading the accepted baseline. Missing or fingerprint-incompatible baselines
return exit `4`; threshold failures return exit `1`; internal/incomplete
comparison returns exit `5`.

## Interpret evidence

Read these report fields before quoting a number:

- `status` and `exit_code` say whether execution completed and assertions held.
- `authoritative` and `authority_reasons` say whether the observation may be
  used as evidence. A passing local or dirty run is still non-authoritative.
- the three `comparison` fingerprints prove environment, workload, and policy
  compatibility.
- `effective_config.boot_profile` and `subsystem_mask` identify the actual boot
  closure. The mask is a canonical 16-digit hexadecimal string, not a label.
- `execution` proves isolated repetition count and warmup stability.
- `aggregate.metrics` and `aggregate.passes` contain nearest-rank percentiles
  and population standard deviation; unavailable GPU samples retain an
  unavailable reason.
- `events`, required-metric invalid counts, snapshot-publication drops, and
  artifact digests determine completeness.

Work-volume rows must be bit-identical across repetitions. Timing is expected
to vary and is reported with spread. Never call one process a performance
result, and never compare reports whose required fingerprints differ.

Exit codes are: `0` completed observation/pass, `1` assertion failure, `2`
invalid usage or manifest, `3` unavailable environment, `4` missing or
incompatible profile/baseline, and `5` timeout, cancellation, internal error,
or incomplete evidence.

## Baseline safety

Ordinary `profile`, `snapshot`, `autotest`, and `compare` commands are
read-only with respect to `tools/baselines/`. A safe proposal verifies a
completed snapshot without mutation:

```sh
build/tools/vkr_harness baseline propose \
  --from build/_artifacts/snapshot/<run-id> \
  --actor '<actor>' --reason '<reason>'
```

Review `plan.json` and `entries.ndjson`. Never run `baseline accept` unless the
user explicitly requests baseline promotion. Acceptance requires the exact
proposal digest, re-verifies the prior generation and every source digest,
writes an immutable content-addressed generation, then atomically replaces
`current.json`. Manual copying into `tools/baselines/` is unsupported.

Use `vkr-validation` for CPU and Vulkan correctness gates and `vkr-performance`
for repository performance claims.

| Profile | Timestamps | Use |
|---|---|---|
| `local-windowed.json` | off | Observational; two repetitions, drift not enforced |
| `local-windowed-gpu.json` | on | Observational per-pass GPU timing |
| `performance-windowed.json` | off | Authoritative CPU and work-volume evidence |
| `performance-windowed-gpu.json` | on | Authoritative evidence including complete per-pass GPU timing |
| `local-windowed-boot.json` | off | Observational five-repetition boot, residency, and work equivalence |
| `performance-windowed-boot.json` | off | Authoritative clean-tree boot/residency evidence |

The authoritative profiles encode the repetition, warmup, exclusivity, and
completeness policy; a parser rejects any authoritative profile declaring fewer
than two independent repetitions. A dirty tree or any authority reason still
makes their output non-authoritative. A timestamp-on run is a different
comparison configuration from a timestamp-off run, so never compare the two.

A case whose `frames.warmup` is smaller than the profile's
`warmup_stability_window` cannot answer that profile's stability gate. The
pairing is rejected up front with exit `4`; raise the case's warmup rather than
narrowing the profile's window.

| Case | Boot | Frames | Use |
|---|---|---|---|
| `smoke/sponza_static.case.json` | full | 30/60 | Fast functional smoke |
| `performance/sponza_orbit.case.json` | full | 120/300 | Steady-state frame evidence |
| `performance/sponza_orbit_automation.case.json` | automation | 120/300 | The same workload under automation boot; use it to observe automation steady state on its own, never as the other orbit case's comparand |
| `performance/sponza_boot_full.case.json` | full | 120/60 | Paired boot/residency evidence |
| `performance/sponza_boot_automation.case.json` | automation | 120/60 | Paired boot/residency evidence |
| `smoke/sponza_snapshot.case.json` | automation | 2/3 | Direct final/depth/shadow/picking snapshot fixture |
| `smoke/sponza_snapshot_editor.case.json` | automation | 2/3 | Editor scene-color/depth/picking snapshot fixture |
| `smoke/sponza_snapshot_debug.case.json` | automation | 2/3 | Normals/unlit/lighting and three shadow-debug replay fixture |

## Compare full and automation boot

Use the paired focused cases; do not change scene, camera, rendered features,
frame window, or profile between commands:

```sh
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_boot_full.case.json \
  --profile tools/profiles/performance-windowed-boot.json

./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_boot_automation.case.json \
  --profile tools/profiles/performance-windowed-boot.json
```

Boot profile and subsystem mask intentionally make the workload fingerprints
different. Treat this as a declared boot/residency comparison, not an ordinary
frame-performance baseline comparison. Require every deterministic draw,
command, visibility, and overflow row to be identical before interpreting
`boot.*` or resident memory. Report both masks and
`memory.gpu.live_totals_exact`; do not infer savings from subsystem count alone.
