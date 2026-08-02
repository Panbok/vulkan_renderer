---
status: investigation
updated: 2026-08-02
authority: investigation
---

# Renderer Harness Phase-3 Verification

## Conclusion

Phase 3 is implemented. `VKR_BOOT_PROFILE_AUTOMATION` is now a typed,
dependency-resolved initialization contract rather than a collection of harness
conditionals. The plan is built and closed before renderer subsystem startup,
revalidated at the frontend boundary, retained for symmetric teardown, and
reported as the mask that actually initialized — not the mask that was
requested. A zero-initialized ordinary `ApplicationConfig` still resolves to
full boot.

The exercised Release reports are non-authoritative observations: both the
local profile and dirty implementation worktree clear authority. They establish
functional integration and show that the retained skips have a boot or exact
GPU-residency benefit on this Apple M1 Pro/MoltenVK configuration. They are not
an authoritative speedup claim. The shipped `performance-windowed-boot.json`
profile requires a clean tree, five repetitions, stable warmup, and exclusive
GPU-lane ownership for a reviewed rerun.

## Initialization contract

`VkrSubsystemPlan` exposes requested, excluded, and effective masks over 19
stable initialization units. `vkr_renderer_subsystem_plan_build()` rejects
unknown bits, exclusions from full boot, and dependency closures that intersect
an explicit exclusion. `vkr_renderer_systems_initialize()` independently
rebuilds the plan and rejects a caller-provided effective mask that is not the
same closure before calling the backend or initializing a subsystem.

The enum is split by `VKR_RENDERER_SUBSYSTEM_MANDATORY` and
`VKR_RENDERER_SUBSYSTEM_OPTIONAL`, asserted at compile time against the enum
order. The mandatory set is exactly the automation base, so the fourteen units
that `vkr_renderer_systems_initialize()` does not gate cannot be excluded by any
plan, and the harness derives its exclusion set from the published optional mask
rather than a second list that could drift as units are added.

Automation retains the camera, pipeline registry, render graph, frame streams,
shader/resource/geometry/texture/material/mesh systems, fonts, lighting,
shadows, and world resources. Fonts remain conservative because a scene may
contain world text and the case manifest does not yet declare scene text as an
independent feature. Skybox and editor initialization follow case render
features. `picking_ids` capture requests are already mapped to picking for the
future snapshot tool, while UI, editor, gizmo, and picking are omitted from the
profile workload when nothing requests them.

Callers supply intent only. `ApplicationConfig.subsystem_plan` contributes its
profile and requested/excluded masks; the closure is always recomputed, so a
zero-initialized configuration resolves to full boot and a hand-assembled
`effective_mask` cannot bypass dependency resolution.

The focused Sponza automation plan therefore resolves to
`0x000000000000bfff`: the core closure plus skybox. Full resolves to
`0x000000000007ffff`. Optional shutdown paths already guard their initialized
state, so both masks use the same teardown function. Plan resolution occurs
only during boot; frame execution continues to use stable initialized-state and
packet feature gates.

## Reporting and comparison identity

Each child copies the renderer-reported effective mask into `samples.bin` before
shutdown. The parent rejects repetitions whose masks differ, adopts the first
actual mask, writes it as a canonical 16-digit hexadecimal string under
`effective_config.subsystem_mask`, and includes the same value in the workload
fingerprint. The hard-coded `"full"` report value is removed. One helper
produces that text for both the report and the fingerprint, so the schema's
`^0x[0-9a-f]{16}$` pattern has a single producer.

The reported mask is the closure **minus anything that did not initialize**.
Editor, gizmo, and picking initialization is non-fatal and picking is
additionally skipped on a zero-extent window, so a planned bit was never
evidence of a live subsystem; leaving one set would let two runs that rendered
different work compare as the same observation. The renderer narrows the mask
from the subsystems' own initialized state and warns when it does. The parent
still resolves the plan before launching any child, but only as preflight: that
value is a prediction, replaced by the first completed repetition's actual mask.

Scene boot duration is set when the requested scene and dependency closure first
reach `READY`, before measured frames begin. This makes `boot.scene` complete
for both profiles instead of leaving the registered row unavailable throughout
the report.

## Case and profile pairing

A profile that requires warmup stability compares the two halves of the last
`warmup_stability_window` warmup frames, so a window wider than the case's
warmup can never be answered and the run is unconditionally incomplete. That
pairing is now rejected before any repetition executes, as
`VKR_HARNESS_EXIT_MISSING_BASELINE` with a stable reason, alongside the existing
target and presentation-mode checks that the parent and child previously
duplicated and disagreed on.

Both focused boot cases originally declared 30 warmup frames against the
authoritative boot profile's 60-frame window — the exact command both skills
document. They now declare 120 warmup frames, matching the convention
`sponza_orbit.case.json` already uses with the other authoritative profiles, and
their orbit duration is extended from 1.5 s to 3.0 s at the same 0.2 rev/s rate
so the authored path still spans the whole run instead of clamping to its end
partway through warmup.

## Paired Release observation

Both focused cases use Sponza, 1280×720, hidden-window IMMEDIATE presentation,
isolated warm pipeline caches, the same camera path, four shadow cascades, skybox
enabled, editor disabled, 120 warmup frames, and 60 measured frames. The local
boot profile raised each case to five independent repetitions.

| Observation | Full | Automation | Automation delta |
|---|---:|---:|---:|
| Effective subsystem mask | `0x000000000007ffff` | `0x000000000000bfff` | UI/editor/gizmo/picking omitted |
| `boot.systems` mean | 510,427,441.0 ns | 487,327,941.4 ns | -23,099,499.6 ns (-4.53%) |
| `boot.scene` mean | 3,188,091,282.8 ns | 3,213,201,508.0 ns | +25,110,225.2 ns (+0.79%) |
| `memory.gpu.bytes.live` mean | 2,371,080,080 bytes | 2,369,586,024 bytes | -1,494,056 bytes |
| `memory.gpu.live_totals_exact` | 1 | 1 | exact in both |
| `memory.cpu.bytes.live` | 0 | 0 | no observed CPU-residency benefit |
| `draw.world.calls_issued` | 35 | 35 | identical |
| `draw.world.commands_issued` | 35 | 35 | identical |
| `visibility.objects_tested` | 35 | 35 | identical |
| `visibility.distinct_geometries` | 5 | 5 | identical |

`boot.scene` moves in both directions across reruns and is dominated by asset
I/O; only `boot.systems` and the exact GPU-residency reduction are consistent
signals here, and neither is authoritative from this tree.

The first paired attempt exposed one extra full-boot world draw: the disabled
editor viewport's private composite plane was registered as an ordinary visible
mesh. Initialization was therefore changing case work. The editor pass already
submits that plane explicitly, so initialization now marks its mesh invisible
to scene enumeration. The final pair above is from the corrected binary and has
identical work-volume rows.

Artifacts:

- full: `build/_artifacts/profile/20260802T100845.372Z-00ff7e/report.json`,
  `sha256:9190204f7f1fb2d68abf1afda26581e791ad8108e587bd4c9ebfe45afb761f2c`
- automation: `build/_artifacts/profile/20260802T100657.021Z-00fc00/report.json`,
  `sha256:5010623751e14c978fd7ef850bba2e072e496402ab4fbe4b574a23bd65b5999a`

The boot profile and subsystem mask intentionally make the workload
fingerprints distinct. These reports are paired Phase-3 boot/residency evidence,
not interchangeable frame-performance baselines. Their case fields are
otherwise matched and deterministic work rows are compared directly.

## Gates

| Gate | Result |
|---|---|
| Baseline `./build_test.sh` | Exit 0 before Phase-3 edits |
| Focused tests | Dependency closure, impossible exclusion, optional-mask partition, NULL-error acceptance, harness plan mapping, both metric-family spellings, canonical mask text, case/profile pairing rejection, mask-sensitive workload fingerprint, and canonical report mask passed inside the CPU suite |
| Final `./build_test.sh` | Exit 0; every registered suite passed |
| `./build_release.sh` | Exit 0; application and complete parent/child harness built |
| Manifests/report shape | Every new file passed JSON syntax; case/profile files passed the strict runtime parser; generated reports and the report-shape test use the schema's canonical `^0x[0-9a-f]{16}$` mask. The optional local Python Draft-07 package was unavailable, so no separate `jsonschema` command is claimed |
| Full focused profile | Exit 0/`pass`; five repetitions and every required boot/residency/work row complete |
| Automation focused profile | Exit 0/`pass`; five repetitions and every required boot/residency/work row complete |
| Pairing rejection | `smoke/sponza_static.case.json` (warmup 30) against `performance-windowed.json` (window 60) exits 4/`missing_baseline` before launching a repetition; both boot cases now clear the same gate against `performance-windowed-boot.json` |
| Vulkan validation layers | Exit 0/`pass`; two automation repetitions under `VK_LAYER_KHRONOS_validation`, mask `0x000000000000bfff`, no `VUID-`, `Validation Error`, or validation-layer diagnostics in the run tree or captured stderr |

Validation artifact:
`build/_artifacts/profile/20260802T101302.747Z-01019f/report.json`,
`sha256:dd8e558a104c617cb68aa2eff1410132fdb6b2d898852058800afb27db5a7193`.

The warmup **drift** half of the authoritative stability gate does not pass on
this machine even for the pre-existing `sponza_orbit.case.json` /
`performance-windowed.json` pairing at 120 warmup frames and a 60-frame window,
so `execution.warmup_unstable` here is environmental and pre-existing rather
than a Phase-3 effect. The drift and pipeline-creation policy is deliberately
left unchanged; a clean-tree reviewer run must confirm it separately.

Exact commands:

```sh
./build_test.sh
./build_release.sh

build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_boot_full.case.json \
  --profile tools/profiles/local-windowed-boot.json

build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_boot_automation.case.json \
  --profile tools/profiles/local-windowed-boot.json

# Expected to exit 4 before any repetition runs.
build_release/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_static.case.json \
  --profile tools/profiles/performance-windowed.json

VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sponza_boot_automation.case.json \
  --profile tools/profiles/local-windowed.json
```

## Residual scope

Phase 3 remains windowed; boot profile and target are orthogonal. Capture,
`snapshot`, `autotest`, accepted baseline workflows, and true offscreen targets
remain Phases 4-6. No CPU-resident-byte reduction was observable because the
current global allocator gauge reports zero for both runs; the retained skip is
justified by independently measured `boot.systems` and exact GPU-residency
reductions. A clean authoritative boot comparison remains a reviewer-operated
follow-up rather than a claim manufactured from this dirty implementation tree.
