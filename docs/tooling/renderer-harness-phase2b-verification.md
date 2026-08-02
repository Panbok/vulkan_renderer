---
status: investigation
updated: 2026-08-02
authority: investigation
---

# Renderer Harness Phase-2b Verification

## Conclusion

Phase 2b is implemented. The structured harness now owns the repository's
performance workflow through a deterministic Sponza case and reviewed CPU-only
and GPU-timestamp execution profiles. The old application benchmark accumulator,
`BENCHMARK_SAMPLE`/`BENCHMARK_SUMMARY` output, eight-column grep/awk CSV, and
`tools/benchmark_multithreaded_backend.sh` have been removed.

Parity is semantic rather than byte-for-byte: every legacy timing and work
observation has a registered metric or per-pass report row, with exact measured
frame counts, independent repetitions, percentiles, population standard
deviation, validity, fingerprints, and artifact digests added. The legacy
`upload_only` row was not retained: it set `VKR_PARALLEL_UPLOAD=1` without the
separate unsafe opt-in required to execute worker uploads, and its steady-state
window contained no measured upload work. Backend-mode correctness remains in
`tools/validate_multithreaded_backend_matrix.sh`; a future upload-performance
case must contain and measure actual upload work.

The exercised local runs are integration observations, not performance results:
the worktree was dirty and the local profiles are explicitly non-authoritative.
The CPU observation additionally failed its permissive local warmup-drift
policy. The shipped authoritative profiles correctly refuse a dirty worktree.
No renderer speedup or regression is claimed here.

## Legacy-to-harness parity

| Legacy observation/control | Structured replacement |
|---|---|
| `samples` | `aggregate.metrics["frame.wall"].sample_count` plus exact `execution.measured_frames` and independent repetition counts |
| `avg_frame_ms`, `min_frame_ms`, `max_frame_ms` | Per-frame `frame.wall` nanosecond mean/min/max plus p50, p95, total, and standard deviation |
| `rg_cpu_samples`, `avg_rg_cpu_ms` | Named `aggregate.passes[].cpu_ms` rows with sample/invalid counts and full statistics; no pass sum is hidden in one formatted scalar |
| `BENCHMARK_SAMPLE` draw, batch, MDI, and visibility fields | Stable `draw.*`, `visibility.*`, `instance_buffer.*`, and shadow-cascade catalog rows sampled every measured frame |
| `VKR_RG_GPU_TIMING=1` (not consumed by the old CSV) | Dedicated timestamp-on profiles; every executed pass CPU sample must have a matching GPU sample or the report is incomplete |
| app auto-close and shell watchdog | Exact warmup/measure frame counts plus per-child `repetition_timeout_ms` |
| build-type shell selection | Explicit `./build_release.sh`; the report records actual build/compiler and binary digest |
| label/upload CSV columns | Case/profile identity and environment/workload/policy fingerprints; the false upload comparison is removed |
| stdout error grep | Child exit status, isolated stderr/stdout, atomic child/parent reports, raw-sample verification, and artifact digests; Vulkan correctness remains a separate validation-layer gate |
| widening eight-column CSV | Long-form `run_index,metric,unit,stat,value,sample_count,status`; new metrics add rows without parser changes |

## Functional and integration evidence

| Gate | Result |
|---|---|
| Baseline `./build_test.sh` | Built cleanly, then exited 139 during `test_event_subscription` |
| Baseline `./build_test_batch.sh` | 46/50 passed; runs 2, 14, 17, and 23 crashed at the same event-subscription point, confirming a pre-existing intermittent outside Phase-2b paths |
| Final `./build_test.sh` | Exit 0; all suites passed, including authoritative-profile minimum-repetition and GPU-pass completeness fixtures. Re-run after the review below: exit 0 |
| `./build_release.sh` | Exit 0; Release application and harness built after removing the duplicate app benchmark path. Re-run after the review below: exit 0, no compiler diagnostics |
| `clang-format --dry-run --Werror` | Clean over `tools/harness/` and `tests/src/harness_test.c` |
| Draft-07 schema validation | Python `jsonschema` 4.25.1 accepted both case fixtures and all four execution profiles; the strict runtime parser independently loaded the performance case/profiles |
| Draft-07 revalidation after review | Python `jsonschema` 4.26.0 accepted both case fixtures, all four execution profiles, and both aggregate reports below |
| Short local CPU profile | Exit 0; report `build/_artifacts/profile/20260802T074436.905Z-005aee/report.json`, digest `sha256:d293d17a2172dc9cf0fc471b837bb23fd45294b9bd600a137cba8c4486d6d05e`; the smoke case is unchanged by the review below |
| Performance-case local CPU profile | Exit 0; report `build/_artifacts/profile/20260802T082823.976Z-008ada/report.json`, digest `sha256:7b1c355d02dc9f09c513b630e014a740f5aafe524da404b282dbb2f889163def` |
| Performance-case local GPU-timestamp profile | Exit 0; report `build/_artifacts/profile/20260802T082928.064Z-008a8e/report.json`, digest `sha256:0bf15afea3dd9ddeef85210122c0482c77befaa7f3f7d6145d81f9b7bb48d676` |
| Authoritative dirty-tree preflight | Exit 3/`unavailable`, no child or report; `performance-windowed.json` requires clean provenance |
| Authoritative single-repetition preflight | Exit 2/`invalid` with `profile.authoritative_repetitions` on stderr, no child or report |

Both local runs completed two isolated repetitions, each with 120 warmup and
300 measured frames. Each aggregate contains 222 metric rows, eight pass rows,
zero event or snapshot-publication drops, and 600 valid samples for
`frame.wall`, `cpu.render_submit`, and `draw.world.calls_issued`. Work volume
matched: `draw.world.calls_issued` was exactly 36 on every measured frame.

For observational context only, the CPU run's `frame.wall` mean/p50/p95 were
36.919/37.025/41.927 ms and its `cpu.render_submit` mean/p50/p95 were
1.545/1.623/2.628 ms. These numbers are not a result because authority was
cleared by the local profile, dirty provenance, and unstable warmup. They are
also not comparable with the pre-review observations recorded during
implementation: the orbit case was corrected afterwards (see below), which
changes the workload fingerprint.

The GPU profile produced 600 CPU and 600 GPU samples with zero GPU-invalid
samples for each of all eight named passes. Negative CPU fixtures prove the
exact per-sample predicate rejects both an entirely missing row and a CPU-valid
row without a GPU-valid sample. A second fixture proves an authoritative profile
with one independent repetition is rejected during parsing, and now asserts the
specific rejection code rather than only the refusal.

## Post-implementation review

A review of the Phase-2b implementation applied five corrections. They are
recorded here because one of them changes the shipped performance workload.

- **The performance case froze its camera inside the measured window.** The
  orbit authored a 5 s single revolution while the run advances 420 frames of
  authored time at a fixed 1/60 s delta. `vkr_harness_camera_evaluate()` clamps
  a completed orbit, so 120 of the 300 measured frames — 40% of the window that
  every future authoritative claim rests on — rendered a frozen pose. The case
  now authors 7 s and 1.4 revolutions: 1.2 degrees per frame, no clamped frame,
  and the 300 measured frames cover exactly one revolution. The observations
  above are from the corrected case.
- **Requested GPU completeness is now answered over the aggregated window.**
  The predicate reads the concatenated per-pass flags that
  `vkr_harness_aggregate_runs()` already builds, instead of re-deriving each
  repetition's warmup offset in a second walk, and it no longer restates
  `passes.gpu_timing_incomplete` on a run that completed no repetitions.
- **A name-mismatched pass row can no longer inherit `culled`/`disabled`.**
  Those bits now require the source row to name the catalog row, so a reordered
  pass table cannot present another pass's skip as this pass's legitimate
  absence and satisfy the completeness gate.
- **Execution-policy rejections name their rule.** The authoritative-repetition,
  repetition-range, stability-window, and drift-ratio checks previously returned
  a bare failure, so the runner printed an empty `code: message` pair and exited
  2 with no reason.
- The completeness predicate itself was restated as one positive rule.

## Scope and residual risk

Phase 2b changes policy, fixtures, reporting completeness, and the demo
application's retired logging path. It does not record new Vulkan commands,
change resources or synchronization, or claim a speed difference, so no new
validation-layer or before/after performance gate applies. Release harness runs
exercise the existing Vulkan path but are not a substitute for validation
layers.

The unrelated event-system intermittent remains: the clean baseline reproduced
four crashes in 50 processes. This phase neither touches nor claims to repair
that subsystem. Capture, automation boot, accepted baselines/comparison, and
offscreen targets remain phases 3-6.
