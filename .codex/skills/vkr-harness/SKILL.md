---
name: vkr-harness
description: Run renderer profiles, snapshots, comparisons, autotests, and baseline proposals; inspect reports and retire run artifacts.
---

# VKR harness

## Select and run the observation

1. State the invariant or measurement the run must establish. Select the
   smallest case that exercises it and a profile with the required evidence
   policy. Inspect the actual JSON files; do not infer settings from names.
2. Build with `./build_release.sh` when the Release binary or assets need
   updating. Use Debug only for a concrete diagnostic reproduction.
3. Match case target/present settings to the profile. Cases own workload,
   camera, warmup, cache policy, and captures. Profiles own instrumentation,
   minimum repetitions, environment, stability, and authority requirements.
4. Run once, inspect the report and child errors, fix the relevant failure,
   then rerun the affected gate. Broaden coverage only when a changed invariant
   or unexplained result requires it. Do not weaken an acceptance profile.

Small Release execution check, with no timing authority:

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  ./build_release/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_offscreen.case.json \
  --profile tools/profiles/local-offscreen.json
```

For ordinary snapshots and baselines, use Release with validation variables
unset. For a focused diagnostic run, use `vkr-validation`. For timing claims,
use `vkr-performance`. Native backend evidence remains backend-specific;
`vkr-shaders` owns bilateral shader and ABI acceptance.

## Choose the command

| Command | Use and output |
|---|---|
| `profile --case <case.json> --profile <profile.json>` | Isolated capture-free repetitions, aggregate metrics, child reports, `summary.csv` |
| `snapshot --case <case.json> --profile <profile.json>` | Replay captures and comparison results; replay timings are auxiliary |
| `autotest --case <case.json> --profile <profile.json>` | Separate nested primary-profile and snapshot reports |
| `compare --run <snapshot-run-directory>` | Recheck completed snapshot digests and compare to its accepted baseline |
| `baseline propose --from <snapshot-run-directory> --actor '<actor>' --reason '<reason>'` | Verify snapshot and write a reviewable promotion proposal |

All commands use `./build_release/tools/vkr_harness`. Windows multi-config
builds may place the executable under `build_release/tools/Release/`.
The CLI with no command prints usage and exits `2`; it has no `--help` command.
The final successful publication line names the report and its SHA-256 digest.
Read stderr when invalid usage or an early failure produces no report.

Concrete capture check:

```sh
./build_release/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_offscreen_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json
```

Snapshots run isolated replay children. Channels with the same checkpoint and
render mode share a replay. Inspect `captures[]` for producer, encoding,
source frame/submit serial, paths, digests, thresholds, and comparison verdict;
inspect `auxiliary_runs[]` for child reports and effective configurations.
Unavailable captures return `3`; never substitute a different channel.
Capture names and case fields come from
the runtime parser in `tools/harness/vkr_harness_manifest.c` and working
examples under `tools/cases/`. Report fields are emitted by
`tools/harness/vkr_harness_report.c`; ADR-051 records the evidence contract.

## Interpret the report

Check these fields before accepting an observation:

| Field | Required interpretation |
|---|---|
| `status`, `exit_code` | Execution and assertion verdict; a report file alone does not prove success |
| `authoritative`, `authority_reasons` | Authority policy result; a passing local or dirty run cannot support an authoritative timing claim |
| `comparison` | Environment, workload, and policy fingerprints; compare only compatible runs |
| `effective_config` | Realized boot, subsystem mask, target image count, extent, formats, present mode, and feature settings |
| `provenance` | Binary/build, GPU, and driver identity; `effective_config.renderer_backend=external` alone does not distinguish Metal from Vulkan |
| `execution` | Independent repetition count and warmup stability |
| `aggregate.metrics`, `aggregate.passes` | Valid samples, nearest-rank percentiles, population standard deviation |
| `events`, artifact digests | Required-metric invalidity, overflow, publication drops, and artifact completeness |

Logical window size can differ from drawable pixels. Offscreen targets use
ordinary images and report `present_mode=none`; infer neither pixels nor WSI
color space from the manifest.

Require deterministic work-volume rows to match across repetitions. GPU pass
coverage is complete only when each row is timed, skipped, or explicitly
`unsupported_timestamp_scope`. Unsupported timestamps stay invalid and cannot
support a duration claim. A shorter pass list or missing samples is not a win.

| Exit | Meaning |
|---|---|
| `0` | Completed observation or pass |
| `1` | Assertion or comparison threshold failure |
| `2` | Invalid usage or manifest |
| `3` | Unavailable environment or required capability |
| `4` | Incompatible case/profile or missing/incompatible baseline |
| `5` | Timeout, cancellation, internal failure, or incomplete evidence |

If a profile requires warmup stability, the case warmup must cover its stability
window. Pairing failure is exit `4`; use sufficient warmup, not a weaker gate.
Profile JSON files under `tools/profiles/` define the current policy. Useful
starting points are `local-offscreen.json`, `local-windowed.json`,
`local-windowed-gpu.json`, and the `performance-*` profiles. A single-process
local profile can reproduce a defect but cannot establish a speed claim.

## Baselines and cross-machine comparison

Ordinary commands do not mutate `tools/baselines/`. Baseline acceptance requires
user authorization to promote the reviewed result. A request to investigate or
fix pixels does not authorize replacing accepted evidence.

Review the proposal's `plan.json` and `entries.ndjson`. When promotion is
already authorized, run `baseline accept --plan <plan.json>
--confirm-sha256 <exact-proposal-digest>`. Acceptance re-verifies prior/source
digests, writes an immutable generation, and atomically updates `current.json`.
Do not copy capture files into the baseline tree manually.

For cross-machine shader parity:

1. Use the same backend-neutral case and profile on both machines. Leave
   `renderer.backend` unpinned in both case manifests.
2. Publish the first machine's captures through authorized baseline acceptance.
   The tracked generation retains canonical captures, metadata, reports, and
   previews. Commit and push it when repository publication is authorized.
3. Pull that generation on the second machine. Use `snapshot ...
   --cross-backend` or `compare --run <second-run> --cross-backend`.
4. Require identical workload/policy fingerprints and distinct environments.
   This mode permits the environment difference; ordinary baseline matching
   does not. Record comparison values and limits before deleting local output.

## Retire only this task's output

Every run writes regenerable files under `build/_artifacts/`. Record the exact
command, configuration, report digest, verdict, relevant values/spread, and
coverage limits in the task note or requested deliverable. Then remove only
this task's completed run directories and traces. Preserve other tasks' output.

Before deleting a snapshot needed on another machine, publish its accepted
baseline generation. A pending `baseline propose` still depends on the source
snapshot and its digests: accept or abandon that proposal before removing its
source. Accepted tracked generations are self-contained and survive deletion
of local run directories.
