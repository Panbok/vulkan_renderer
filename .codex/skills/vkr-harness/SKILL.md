---
name: vkr-harness
description: Run and interpret VKR's structured renderer automation harness. Use when executing a renderer case, selecting a harness profile, reading a harness report or summary.csv, checking deterministic repetitions, or deciding whether harness evidence is authoritative.
---

# VKR Harness

Use `vkr_harness` for structured, repeatable renderer observations. Phase 2
supports the `profile` command with full boot and visible or hidden windowed
targets. Phase 2b adds authoritative CPU and GPU-timestamp performance profiles
and retires the old log-scraping benchmark. Capture, `snapshot`, `autotest`,
baseline comparison/promotion, automation boot, and true offscreen targets are
later phases and must not be claimed from this binary.

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

## Interpret evidence

Read these report fields before quoting a number:

- `status` and `exit_code` say whether execution completed and assertions held.
- `authoritative` and `authority_reasons` say whether the observation may be
  used as evidence. A passing local or dirty run is still non-authoritative.
- the three `comparison` fingerprints prove environment, workload, and policy
  compatibility.
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

No Phase 2 command mutates accepted evidence. Baseline proposal and acceptance
arrive in Phase 5. If those commands are added later, never accept or promote a
baseline unless the user explicitly requests that mutation.

Use `vkr-validation` for CPU and Vulkan correctness gates and `vkr-performance`
for repository performance claims.

| Profile | Timestamps | Use |
|---|---|---|
| `local-windowed.json` | off | Observational; two repetitions, drift not enforced |
| `local-windowed-gpu.json` | on | Observational per-pass GPU timing |
| `performance-windowed.json` | off | Authoritative CPU and work-volume evidence |
| `performance-windowed-gpu.json` | on | Authoritative evidence including complete per-pass GPU timing |

The authoritative profiles encode the repetition, warmup, exclusivity, and
completeness policy; a parser rejects any authoritative profile declaring fewer
than two independent repetitions. A dirty tree or any authority reason still
makes their output non-authoritative. A timestamp-on run is a different
comparison configuration from a timestamp-off run, so never compare the two.
