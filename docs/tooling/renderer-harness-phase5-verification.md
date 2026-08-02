---
status: investigation
updated: 2026-08-02
authority: investigation
---

# Renderer Harness Phase-5 Verification

## Conclusion

Phase 5 is implemented for the existing windowed targets. The harness groups
logical forward-render debug channels into isolated replay configurations,
compares canonical color/depth/ID payloads with type-specific rules, writes
diff artifacts for image failures, composes primary profile and auxiliary
snapshot evidence without mixing their aggregates, and loads profile/case
baselines from immutable content-addressed generations.

Baseline mutation is guarded. `baseline propose` verifies a completed snapshot
and writes a review plan without changing accepted evidence. `baseline accept`
requires the exact plan digest, rechecks the previous accepted generation and
every source digest, copies an immutable generation, and publishes
`current.json` atomically only after the copy is complete. No baseline was
accepted into this working tree during implementation; the successful accept
path was exercised under an isolated temporary repository root.

At the time of this evidence, Phase 6 still owned the target-neutral present
seam and true offscreen execution; it is now recorded in
[the Phase-6 verification](renderer-harness-phase6-verification.md).
The evidence here is dirty-tree, local-profile correctness evidence, not a
performance claim or an accepted visual baseline.

## Implemented contract

The logical catalog includes `normals`, `unlit`, `lighting`,
`shadow_debug_cascades`, `shadow_debug_factor`, and `shadow_debug_depth` beside
the Phase-4 direct channels. Channels using the same checkpoint and render mode
share one replay. Each distinct shadow-debug mode gets its own replay. Every
auxiliary child report and aggregate reference carries environment, workload,
and policy fingerprints.

Canonical comparison is value-aware:

- RGBA8 uses normalized per-channel absolute error and reports mean/max error,
  failing value/pixel counts, and failing-pixel ratio;
- little-endian R32 float depth rejects non-finite input before applying the
  configured absolute thresholds;
- little-endian R32 unsigned identifiers require exact equality.

The source and baseline data digests are verified before decode. Extent,
encoding, value kind, color space, origin, mip, layer, and capture version must
be compatible. Comparison results and effective thresholds are persisted in
`captures[]`; image failures can publish canonical RGBA diff PNGs.

The aggregate `capture-summary.bin` is the comparison and promotion trust
boundary. Version 2 carries the tool verdict, case/profile manifests and
digests, effective fingerprints, provenance, captures, and artifact inventory.
`compare --run` copies verified source artifacts into its own atomic report
root. `snapshot` also compares automatically when a compatible accepted
baseline exists. A missing baseline remains a completed diagnostic snapshot
with `baseline.missing`; explicit compare and autotest require one and return
exit 4.

`autotest` launches a normal capture-free profile under `primary/` and a
snapshot under `snapshot/`. The top report has one primary `runs[]` reference
and one `auxiliary_runs[]` reference, both digest- and fingerprint-verified. It
does not copy nested metrics, passes, assertions, captures, or capture timings
into a misleading combined aggregate.

## Evidence

| Gate | Result |
|---|---|
| `./build_test.sh` | Exit 0; every registered suite passed, including replay grouping, RGBA/depth/ID algorithms, and the isolated guarded-promotion workflow |
| Guarded-promotion unit integration | Wrong confirmation rejected; correct confirmation created an immutable generation, published `current.json`, reloaded the accepted summary, verified the capture digest, and removed its temporary root |
| `./build.sh Debug` | Exit 0; all shaders, renderer targets, application, texture packer, and complete Phase-5 harness built with implicit declarations treated as errors |
| Six-channel debug replay | Exit 0/`pass`; six independent replay configurations completed and published six distinct canonical data digests |
| Vulkan validation scan | No `Validation Error`, `VALIDATION`, or `FATAL` diagnostics in the debug replay or combined autotest stderr logs |
| No-mutation proposal | Exit 0; verified the source report, aggregate summary, and capture inventory and emitted a content-addressed plan |
| Confirmation guard | A deliberately incorrect plan digest returned exit 5 and did not create `tools/baselines/local.windowed/smoke.sponza.snapshot_debug/current.json` |
| Missing-baseline compare | Exit 4/`missing_baseline`; the command still emitted a digest-addressed comparison report |
| Combined autotest | Primary profile passed, snapshot produced four captures, nested references carried non-empty fingerprints, top-level aggregates remained empty, and the expected missing baseline produced exit 4 |
| Backend matrix | `tools/validate_multithreaded_backend_matrix.sh` completed with `PASS=5`, `FAIL=0` |
| Report schema | `harness-report-schema.json` passed `jq empty`; the schema now covers compare reports, run-reference fingerprints, comparison thresholds, and comparison results |
| Formatting | `clang-format` applied to changed C headers/sources and `git diff --check` passed |

Final six-channel debug replay:

- report: `build/_artifacts/snapshot/20260802T162433.067Z-0138ee/report.json`
- report SHA-256:
  `sha256:ab9c9e67a74d50f0e20733bf202dac0847579c0397d3b55be2df53ca2d814b33`

| Logical channel | Canonical data SHA-256 |
|---|---|
| `normals` | `sha256:e03a384087e4ffc433b67123207c2d4fc0affc27650dec879781e9f77a76150c` |
| `unlit` | `sha256:4bcfd1c599e9553158ce2f83361edfb64393daf7a792498338acf88c19aa7c2c` |
| `lighting` | `sha256:47cc60d24843f5374e8e29628a67e2b712dcc6f141e1d7d23a2eec2ba1b1f774` |
| `shadow_debug_cascades` | `sha256:26f822840f788f47ba64cb381f0d1f32c55212c204dec26031c0b7b487e72c43` |
| `shadow_debug_factor` | `sha256:e047314dbe23013fd758f026b7dd7f1cfea3efe7434b4114f0a7a301b522a18d` |
| `shadow_debug_depth` | `sha256:0eb8afba794d6e28335dcf9316c0220dcd92f9702ab2a48d9dc9a13ca5dbfa9a` |

The first debug replay revealed that the PBR shader declared but did not consume
`shadow_debug_mode`, so all three logical shadow outputs matched ordinary
lighting. The implementation added the cascade, factor, and receiver/map depth
visualizations to `pbr.world.slang`; the final replay above proves the three
paths now produce distinct canonical outputs.

Guarded workflow artifacts:

- proposal:
  `build/_artifacts/baseline/20260802T162302.226Z-013485/plan.json`
- proposal SHA-256:
  `sha256:399ec7a7a16c077c9d0f3d3773ee393ff2f4e1307a2ab87fffb05c36a5905956`
- proposed generation:
  `sha256:bdf210e5138d3c7546fb544919c28dcefc54b12f1575eda31d684735e83c7f13`
- missing-baseline compare report:
  `build/_artifacts/compare/20260802T162312.063Z-01349a/report.json`
- compare report SHA-256:
  `sha256:5a7f3fed3dfaf56fb6dacfdbd8d1e8196a2a02975c5d30cfa1437ceb9d673b87`
- autotest report:
  `build/_artifacts/autotest/20260802T163348.557Z-014557/report.json`
- autotest report SHA-256:
  `sha256:422018ec3ada6baa5768e68d62f420bef670c4bba898855e1eab887f766f81b6`

Exact runtime commands:

```sh
./build_test.sh
./build.sh Debug

build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_snapshot_debug.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness baseline propose \
  --from build/_artifacts/snapshot/20260802T161836.828Z-012c23 \
  --actor codex \
  --reason "P5 guarded-promotion verification candidate"

build/tools/vkr_harness baseline accept \
  --plan build/_artifacts/baseline/20260802T162302.226Z-013485/plan.json \
  --confirm-sha256 \
  sha256:0000000000000000000000000000000000000000000000000000000000000000

build/tools/vkr_harness compare \
  --run build/_artifacts/snapshot/20260802T161836.828Z-012c23

build/tools/vkr_harness autotest \
  --case tools/cases/smoke/sponza_snapshot.case.json \
  --profile tools/profiles/local-windowed.json

tools/validate_multithreaded_backend_matrix.sh
```

## Residual evidence scope

No real accepted baseline exists for these local fixtures, so this verification
does not claim a project-baseline pass/fail image result. Pure comparison tests
cover pass, threshold failure, non-finite depth rejection, and exact identifier
failure. The isolated promotion test covers successful acceptance without
altering repository evidence. A reviewed real promotion remains an explicit
user action.

Validation covered Apple M1 Pro/MoltenVK with the existing hidden window and
three-image configuration. It does not establish cross-vendor, different
swapchain-image-count, or true headless behavior. Offscreen/windowed target
equivalence is now recorded in
[the Phase-6 verification](renderer-harness-phase6-verification.md).
