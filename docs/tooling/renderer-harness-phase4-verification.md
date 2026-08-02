---
status: investigation
updated: 2026-08-02
authority: investigation
---

# Renderer Harness Phase-4 Verification

## Conclusion

Phase 4 is implemented. The renderer exposes a stable direct-capture catalog
and packet-carried batch request, the Vulkan backend owns a bounded asynchronous
capture ring separate from picking, and the render graph appends one
request-specific `Capture.Readback` pass with exact mip/layer reads. The
`vkr_harness snapshot` command replays each checkpoint in an independent child,
uses the case's pipeline-cache policy, canonicalizes results, verifies child
digests, and publishes one atomic diagnostic report.

This is capture evidence, not performance evidence. Snapshot reports are
non-authoritative by policy, and the implementation worktree was dirty. At the
Phase-4 boundary, comparison, auxiliary debug replays, accepted baselines, and
guarded promotion remained Phase 5; those now ship and are recorded in the
[Phase-5 verification](renderer-harness-phase5-verification.md). Phase 6 still
owns a true offscreen target.

## Capture contract

The fixed catalog ships `final_color`, `scene_color`, `depth`, four
`shadow_cascade_N` channels, and `picking_ids`. Preflight resolves names to IDs,
rejects duplicates, unknown channels, unsupported mip/layer requests, missing
subsystems, editor-only resources, and a missing picking producer before frame
mutation. Each result records the resolved producer resource, so `depth` is
reported as `swapchain_depth` in the ordinary fixture and `scene_depth` in the
editor fixture.

The backend reserves one persistently mapped host buffer per slot before graph
construction. States are explicit: idle, reserved, recorded, submitted, ready,
acquired, failed, and abandoned. A full ring returns `CAPTURE_BUSY` without a
wait. Successful submit associates frame/fence and serial ownership;
non-coherent memory is invalidated before publication. Result storage is
borrowed until explicit release. Cancel and submit-failure paths retain a failed
tombstone, while an early release of submitted work becomes abandoned and
cannot be reused until completion.

The image-copy primitive accepts checked mip, base layer, layer count, aspect,
row layout, and destination bounds. Capture initialization permits only the
implemented uncompressed color, `D16_UNORM`, `D32_SFLOAT`, and `R32_UINT`
paths. WSI color capture is enabled only when the surface advertises transfer
source usage; depth selection also checks transfer features and avoids combined
depth/stencil formats.

## Graph and canonical artifacts

The overlay imports the fixed staging buffer and declares one transfer write
plus one exact image-slice transfer read per item. The compiler carries that
slice through barrier generation. Imported swapchain resources are augmented
only for an active capture request; ordinary profile/application boots leave
capture disabled and allocate no ring.

Canonical outputs are:

- BGRA/RGBA color, normalized to top-left RGBA8 sRGB PNG;
- D16/D32 depth, normalized to tightly packed little-endian R32 float data plus
  an observed-range PNG preview;
- R32 picking identifiers, normalized to tightly packed little-endian uint data
  plus a deterministic palette preview.

Each capture report row and sidecar records its channel/capture version,
resolved producer, source/canonical encodings, value kind, color space, origin,
extent, source row pitch, mip/layer, source frame, submit serial, paths, and
SHA-256 digests. The aggregate report verifies every child artifact before
adopting it and carries the first child's actual GPU, driver, present mode,
target image count, color format, and color space.

## Evidence

| Gate | Result |
|---|---|
| `./build_test.sh` | Exit 0; every registered suite passed, including capture catalog/converters, deterministic canonical digests, capture-ring state/ownership, and exact graph-slice barriers |
| `./build.sh Debug` | Exit 0; shaders, renderer, application, and parent/child snapshot harness built |
| Non-editor snapshot A | Exit 0/`pass`; final color, swapchain depth, shadow layer 0, and picking IDs published |
| Non-editor snapshot B | Exit 0/`pass`; all four canonical data and preview digests matched snapshot A exactly |
| Editor snapshot | Exit 0/`pass`; final color, `scene_color`, `scene_depth`, and picking IDs published with resolved producer names |
| Vulkan validation | Debug snapshot stderr contained no `VUID-`, `Validation Error`, validation-layer, or `ERROR` diagnostics |
| Profile regression | Exit 0/`pass`; the capture-free `profile` command is unaffected by the shared report tables |
| Backend matrix | `tools/validate_multithreaded_backend_matrix.sh` completed with `PASS=5`, `FAIL=0` |
| Report schema | `harness-report-schema.json` and generated reports passed JSON syntax; the optional Python `jsonschema` package was unavailable, so no separate Draft-07 validation command is claimed |

Final non-editor artifacts:

- A: `build/_artifacts/snapshot/20260802T134754.945Z-008917/report.json`,
  `sha256:afc4863fb5eaa1a11cb11b9a3dd4736e3c1f4da196e9dfeffb872d358a571cc8`
- B: `build/_artifacts/snapshot/20260802T134835.284Z-008d12/report.json`,
  `sha256:f77dac161bee1db39805ea7b8fa8f4d1e14ef0b97a181c984277a28f7e407c47`
- editor: `build/_artifacts/snapshot/20260802T134908.053Z-008c4a/report.json`,
  `sha256:d748894aa536161717ff9125d83b28b0c841f9d90376c1a4d814891e116415d3`

Report digests moved when the per-capture sidecar was rerouted through
`core/vkr_json_writer.h`; the canonical payload digests below are unchanged
from the first implementation pass, so the recorded pixels are the same bytes.

The two non-editor process runs produced these matching canonical data digests:

| Channel | SHA-256 |
|---|---|
| `final_color` | `sha256:7b22d79a6e1c996d47d84c623899309c3545a79e674a7d186a5cfb739f2bcf6c` |
| `depth` | `sha256:5488545b8495e39c847443e796e6f97ff3e4edb2dae4046382df2429b290cacc` |
| `shadow_cascade_0` | `sha256:7c92992c78f82059f8fa20629caf8c9b4cb5d9415815bd51fae2dc016897d646` |
| `picking_ids` | `sha256:1a0e49c31edc1ee7086a8ee10c4281f56a504b4ee13c05eeeb926820489a160c` |

Exact commands:

```sh
./build_test.sh
./build.sh Debug

build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_snapshot.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_snapshot_editor.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_static.case.json \
  --profile tools/profiles/local-windowed.json

tools/validate_multithreaded_backend_matrix.sh
```

## Report storage

`VkrHarnessReport` holds its capture and artifact tables in the owning arena
rather than inline, matching the metric, pass, and event tables beside them.
Each writer requests the capacity its command can reach: a `profile` parent
sizes four artifacts per repetition, a snapshot child sizes three per requested
channel, and a snapshot parent sizes the merged total. Publishing past that
capacity fails the run instead of truncating evidence. The struct is 104 KB, so
the parent, child, and snapshot writers can each keep one as an ordinary local.

## Residual evidence scope

The submit-failure rollback is wired and its pure state transition is covered,
but this run did not inject a native `vkQueueSubmit` failure. Validation covered
normal color/depth/array-layer/picking completion and release on the available
Apple M1 Pro/MoltenVK three-image configuration; it does not claim cross-vendor
or two/four-image coverage.

No capture-capable/no-copy AB/BA overhead comparison was run. Ordinary profiles
structurally leave capture disabled, while a future target-device study must
still quantify the resource-usage cost before any claim about transfer-source
usage or depth compression. At this Phase-4 checkpoint, baseline comparisons,
visual thresholds, debug-mode replays, and offscreen/windowed equivalence
remained later-phase work. Phase 5 now implements the first three;
offscreen/windowed equivalence remains Phase 6.
