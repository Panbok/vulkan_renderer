---
status: investigation
updated: 2026-08-01
authority: investigation
---

# Renderer Harness Phase-2 Verification

## Conclusion

Phase 2 is implemented for full-boot windowed profiling. The Release harness
loads the strict case and execution-profile formats, keeps scene/camera
simulation frozen until scene readiness, applies the fixed scripted camera,
uses isolated cache paths and child processes, and publishes an atomic parent
report only after child reports, raw samples, logs, digests, and `summary.csv`
are complete.

The exercised Sponza case completed two isolated hidden-window repetitions with
actual IMMEDIATE presentation and three swapchain images. Warmup was stable,
the deterministic work-volume comparison matched bit-for-bit, the case
assertion passed, and the aggregate contained 222 metric rows, eight named pass
rows, and the bounded event stream without event or snapshot-publication drops.
The local profile and dirty implementation tree correctly made the observation
non-authoritative.

This evidence does not claim Phase 2b benchmark parity or a performance win.
Capture, `snapshot`, `autotest`, automation boot, baselines, and offscreen
targets remain unavailable by design.

## Functional and integration evidence

| Gate | Result |
|---|---|
| `./build_test.sh` | Exit 0; all registered suites completed, including harness hashing/statistics, strict case/profile parsing, deterministic camera, fingerprint identity, report shape, and path-containment tests |
| `./build_release.sh` | Exit 0; shaders, renderer, application, harness core, and `vkr_harness` built in Release |
| `vkr_harness profile --case tools/cases/smoke/sponza_static.case.json --profile tools/profiles/local-windowed.json` | Exit 0; two independent child repetitions completed and the aggregate report passed |
| Draft-07 schema validation (`ajv-cli`) | The shipped smoke case, local execution profile, and generated aggregate report all validated against their committed schemas |
| Child and artifact integrity | Every child-report digest was recorded; each raw-sample digest was rechecked against its child report before aggregation; aggregate artifact digests matched the files under the run root |
| Determinism | All draw, visibility, overflow, and capture-request metric samples matched bit-for-bit across the two measured repetitions |
| Presentation/configuration | Apple M1 Pro through MoltenVK; hidden 800×600 window, actual IMMEDIATE, three target images, `bgra8_srgb`/`srgb_nonlinear` |
| Compile-disabled configuration | `VKR_METRICS_ENABLED=OFF ./build_release.sh` built both application and harness; invoking `profile` returned exit 3/`unavailable` without starting a child |

The integration run is a GPU smoke test of the existing render path and the new
window/present configuration seam. Phase 2 adds no command recording, resource
transition, or GPU-completion state machine, so the phase-4/phase-6 validation-
layer matrix is not claimed here.

## Report contract checked

- Generated run IDs are not accepted from the command line and run roots are
  created uniquely under `build/_artifacts/profile/`.
- Case, profile, scene, child-run, and artifact paths reject absolute paths,
  `.`/`..`, empty components, and resolved symlink/junction escape.
- Environment, workload, and policy fingerprints use sorted length-prefixed
  fields and SHA-256; provenance-only edits do not change comparison identity.
- Required invalid/missing samples, event/subject loss when requested,
  snapshot-publication drops, unstable required warmup, incomplete repetitions,
  work-volume mismatches, and artifact failures cannot produce a passing report.
- Percentiles use nearest-rank v1 and standard deviation is population standard
  deviation. `summary.csv` is long-form.
- Child stdout/stderr are isolated into per-run logs. Parent stdout ends with
  one small JSON result on every exit path, including preflight rejections that
  publish no report.
- Absent evidence is reported as absent rather than fabricated. A repetition
  with no verified report carries an empty digest, and a run where no child
  reached a presentation configuration reports `present_mode: "unknown"` with a
  zero image count instead of the requested values. One assertion verdict
  function serves both the exit code and the per-assertion `status`, so an
  assertion over missing or partially invalid samples reads `incomplete` rather
  than `pass`.
- Both the passing aggregate/child reports and a zero-completed-repetition
  failure report validate against
  [`harness-report-schema.json`](harness-report-schema.json).

## Remaining phases

- Phase 2b establishes parity with the legacy benchmark before changing the
  performance skill or retiring its grep/awk path.
- Phase 3 implements dependency-resolved automation boot.
- Phases 4 and 5 implement capture, snapshot/autotest, comparison, and guarded
  baselines.
- Phase 6 implements the target-neutral offscreen path.
