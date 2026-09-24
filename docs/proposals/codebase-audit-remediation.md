---
status: proposed
updated: 2026-09-24
authority: proposal
---
# Codebase audit remediation: remaining work

The 2026-09-23 audit plan was implemented on the local branch
`audit-remediation` (baseline `abeedc19`). Each resolved item's commit
records its change and verification, so `git log abeedc19..audit-remediation`
is the implementation record. This document keeps only what is still open:
- decisions the audit could not make;
- work deferred with evidence;
- checks that pass but no gate runs;
- checks this macOS host cannot run.

The [repository contract](../../AGENTS.md) applies to every item.

## Decisions for the owner

### Scene loader publication paths

`runtime/src/renderer/resources/loaders/scene_loader.c` publishes a scene
through two paths:
- the synchronous `scene_load_json_owned`, used by
  `vkr_scene_load_from_file`/`_from_json`, by the resource loader's `load`,
  and by every CPU scene-loader test;
- the staged `vkr_scene_loader_prepare_async` /
  `vkr_scene_loader_finalize_async` path the application uses.

Both consume the same parsed `Scene*Import` records, so splitting parsing
from publication would not remove the duplication. The paths already
disagree:

- The async path rejects entities with `gltf_light_source` or light range
  overrides. The synchronous path accepts them and ignores the fields.
- The synchronous path passes a shape's `material_path` and may block on the
  material. The async path clears it and waits for its own request.
- A mesh without source nodes gets an identity instance transform
  synchronously, but the entity's TRS asynchronously.
- When `vkr_scene_track_instance` fails, the async non-node attach does not
  destroy the instance. `scene_loader_attach_source_mesh` does.

Recommendation: route the synchronous load through prepare/finalize with an
unlimited budget and a test pump for dependencies, so CPU tests exercise the
production path, then delete `scene_load_json_owned`. This changes the
synchronous path's behavior in the cases above, so it needs a decision.

### Instance-buffer metrics

`renderer/src/vkr_renderer_metrics.c` registers `instance_buffer.occupancy`
and `instance_buffer.capacity` but never writes them, and it always publishes
`instance_buffer.overflows` as 0. Six cases assert that the overflow count
stays at 0. Three profiles, including
`tools/profiles/performance-windowed-gpu.json`, list it in
`required_metrics`, and `vkr_harness_metric_is_current_frame_work` counts it
as frame-work evidence. Neither backend overflows the instance buffer: the
Metal upload plan and `vkr_vk_upload_instances` fail the frame instead. The
assertion therefore cannot fail.

Recommendation: retire the three rows and their gates. If occupancy
telemetry is wanted, publish the frame's instance count and the backend
limit instead. Either change edits authoritative profiles.

### Metal packet renderer translation unit (plan D11)

Not done. A whole rebuild of `vkr_metal_packet_renderer.m` measured
1.8-2.1 s in Debug and 2.3-2.9 s in Release. The single translation unit is
documented intent in `vkr_metal_packet_renderer.m`. Objective-C sources do
not get ThinLTO, so a split would export about 128 cross-part helpers that
could no longer be inlined. Revisit if incremental build time becomes a
measured problem.

### Tracked Bistro text baselines

Every Bistro text snapshot reports `baseline.load_failed`. The tracked
baselines are version-2 capture summaries that are 112 bytes smaller than
the frozen V2 layout: renderer fields and `provenance.world_renderer` were
added without a version bump. Re-accepting the baselines needs baseline
publication authority. Until then, snapshots are compared by hand against
the local references `20260923T195327.504Z-00f563` and
`20260923T200423.055Z-01114d`, whose noise floor is max 21 per channel and
mean absolute error about 1e-4.

### `vec2_equal` epsilon boundary

`vec2_equal` treats a difference of exactly epsilon as unequal, while
`vec3_equal` and `vec4_equal` treat it as equal. Mesh deduplication calls it
with `VKR_FLOAT_EPSILON`, one float ULP at 1.0, so aligning the boundary
could change cooked vertex counts. Decide this together with a re-cook check.

## Checks that pass but no gate runs

- `python3 tools/checks/report_long_functions.py --max-lines 300` exits 0:
  6,753 functions measured, none in production over 300 lines, and 158 over
  150. The largest remaining production functions are 293-300 lines.
  `0e87af81` fixed the scanner, which had skipped every
  `API_AVAILABLE(macos(26.0))` definition. Wiring the check into
  `build_test.sh` would make 300 lines a hard limit; that is a policy choice.
- A `VKR_METRICS_ENABLED=0` build of every target compiles with warnings as
  errors, and its tester passes (`c5e95fb0`). No wrapper builds this
  configuration, so it can break again silently. Configure it with
  `VULKAN_SDK` set; see the header note below.

## Deferred with evidence

### Allocation exceptions kept on the C heap (plan D9)

These stay malloc-owned:
- Texture upload payloads: `VkrTexturePreparedLoad`, decode results and
  transcode cache records. Both publishers copy them before publication
  returns, decode workers allocate them in parallel, and `VkrDMemory` never
  decommits freed pages.
- The window platform state and the Windows UTF-8 argv shim.

The Windows window-creation failure paths repeat `free(state)`. Converting
them to one cleanup path needs a Windows build.

### Render-graph JSON read and write builders

`vkr_rg_json_add_pass_reads` and `vkr_rg_json_add_pass_writes` in
`renderer/src/vkr_rg_json.c` differ only in the use list they read and three
pass-builder calls. The repository contract disfavors a flag-driven merge, and
a table of builder functions would put indirect calls into the per-frame
graph build. They stay as they are unless that path is measured.

### Performance evidence

The authoritative `performance-windowed` profiles refuse a dirty worktree.
This tree carries uncommitted user edits and the configure-patched KTX
submodule, so every timing in these commits is a local, non-authoritative
observation (`local-windowed`, `bistro_shadow_orbit`).

The local Bistro orbit is GPU-bound at about 40.4 ms
(`frame.command_slot_waits` is 1 per frame). `Lighting.Deferred.Fullscreen`
takes 17.4 ms of the 40 ms GPU time, a larger target than any CPU item here;
see [renderer features and performance](renderer-features-perf/renderer-features-perf.md).

The Metal upload and submission splits each had four alternating A/B runs
per side:

| Split | `cpu.render_submit` p50 median | Untouched `cpu.graph_build` |
| --- | --- | --- |
| Upload | 1.94 -> 2.13 ms | 0.276 -> 0.296 ms |
| Submission | 2.04 -> 2.12 ms | 0.281 -> 0.299 ms |

Normalized by `cpu.graph_build`, the upload split is within 2% and the
submission split is not slower. Rerun C4, C6 and both splits on a clean
checkout before quoting any of them as a change.

## Unavailable on this host

- Native Vulkan execution and validation, and bilateral Metal/Vulkan
  comparison, for every renderer change. Vulkan compiles on macOS with
  warnings as errors, and the non-Apple branches were syntax-checked. The
  Vulkan IBL commit fix (`0bffcb18`) and the debug-messenger fix (`3680d390`)
  were verified by compilation and reading only.
- Windows builds, including the MSVC `__forceinline` removal and the Windows
  window code.
- A fresh configure without `VULKAN_SDK` finds the Homebrew Vulkan headers
  (1.4.328) before the SDK (1.4.357), and `vkr_vulkan_wsi.c` then fails on
  `VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`. The existing build directories
  cache the SDK path.
- `renderer/src/vkr_sheen_lut_data.inc` was generated on Windows. The macOS
  `vkr_sheen_cooker` output differs from it on 600 lines both before and
  after the D7 split, so the split was checked byte-identical against macOS
  output instead.
- Failure paths without a fixture: the mesh-manager merged-geometry release
  (`e96d585c`) and the Metal pipeline-creation library release (`44ff8c75`)
  were checked by reading.

## Declined

F3 (routing test fixtures through a test allocator): no named failure.
