---
status: implemented
updated: 2026-10-06
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
Snapshot reports adopt the first verified child's actual render extent, as
profile reports do. Recomputing that extent from requested scale used obsolete
nearest rounding and could disagree with the captured source textures.
The camera script is versioned: warmup holds its initial pose and measured frame
zero starts the authored path. Version 5 starts warmup at the common zero of the
active raster jitter and 64-phase GTAO noise sequences, invalidating temporal
history at that boundary. Portable TAA uses eight raster phases; FSR uses the
renderer-owned period from the rounded render/output width ratio (17 for
1239-to-1858 pixels, giving a common 1088-frame period). Stopped scenes and non-temporal modes require only the GTAO
alignment. Aligning to 64 frames alone left FSR captures dependent on bootstrap
duration through their raster jitter phase.
Waiting for that phase remains bootstrap work; authored frame counts and GPU
completion rules do not change. The workload fingerprint includes this version,
so results from the previous replay behavior are incompatible.

Editor cases may set `renderer.editor_stop_frame` and `editor_resume_frame` to
exercise retained Scene presentation. Their zero-based indices include authored
warmup and exclude bootstrap; stop at zero also suppresses bootstrap Scene work.
Resume must follow stop. Captures during the stopped interval support only
`final_color`. These optional controls enter the workload fingerprint when set.
Capture-summary version 8 also stores `renderer.image_sharpness`, a finite
[0,1] control defaulting to zero. Nonzero values enter the workload fingerprint;
zero retains existing workload identities. The effective value is reported and
replayed. Readers migrate versions 2–6 with editor controls unset, and versions
2–7 with sharpness zero. Version 7 retains its explicit native case/config
layout; the new field occupies former alignment padding before the camera, so
case size alone cannot distinguish the formats. Version 11 records the requested
`renderer.display_output`; readers migrate versions 2–10 to SDR. AUTO output
enters the workload fingerprint; SDR preserves existing identities.

A case may set `capture_session: "single"`: one child then takes every capture
checkpoint in frame order instead of one child per checkpoint, and the
checkpoints must request identical channels at increasing frames. A checkpoint
may name `camera_mode` as a cubemap face of a cubemap case camera; the camera
shows that face while rendering toward the checkpoint. Both enter the case
digest. Capture-summary version 16 stores them; readers migrate versions 2–15
with per-checkpoint sessions and no face overrides. The
[reflection probe bake](077-asset-build-system.md) uses one session for its six
faces: on Bistro at 64² it took 14.2 s against 83.7 s for six sessions, with
five faces byte-identical and 18 binary16 components of `nx` one ULP apart.

`renderer.texture_max_load_dimension` (0, 1024 or 2048; default 0) selects the
texture load limit of [ADR-012](012-texture-compression-pipeline.md). Nonzero
values enter the workload fingerprint; zero preserves existing identities.
Capture-summary version 17 stores it in former padding before the camera;
readers migrate versions 2–16 with the limit at zero, which loaded every mip.

`profile` collects capture-free repetitions. `snapshot` runs replay children,
produces canonical captures with metadata and digests, and compares compatible
baselines. `autotest` keeps these two results separate. `compare` rechecks a
completed snapshot. Offscreen cases use ordinary images without a window or
swapchain; automation boot alone does not imply an offscreen target.

Color channels declared `RGBA16_FLOAT_LE` publish tight, top-left, little-endian
binary16 RGBA payloads separately from their PNG previews. Version 2 introduced
the actual float16 payload; earlier output mislabeled PNG-only files as float16,
so those baselines are incompatible. `ssr_reflection` version 4 contains
[ADR-055](055-screen-space-reflections.md)'s full-source-resolution receiver-shaded
RGB. Version 3 held half-resolution shaded RGB; earlier versions held incoming
radiance. Those history baselines are incompatible with the new extent and receiver. `ssr_raw` remains incoming radiance at version 2. Numeric comparison
decodes finite half values and never substitutes
a preview for radiance data. Portable temporal modes capture `scene_color` and
`hdr_pre_bloom` from `temporal_history_color`; the former is a display-converted
PNG and the latter raw reconstructed scene-linear HDR. SDR final-color PNG and
scalar/vector channels keep
their existing contracts. Extended-linear final color under ADR-061 uses
`RGBA16_FLOAT_LE` version 2 with `extended_srgb_linear` color space. Its
sidecar records producer `display_headroom` and `display_output_scale`; its
PNG preview divides native white, clamps to SDR, and applies the sRGB transfer
without a second display transform. Extended-linear comparison verifies both
metadata sidecar digests and requires equal valid headroom/output scale;
different display mappings are incompatible, not pixel regressions. Windows CLI summaries normalize report paths to forward
slashes so the publication line remains valid JSON.

The child owns its large application record in the repetition arena, releases
application resources before publishing reports, and destroys the arena after
publication. This keeps nested fingerprint/report calls within the Windows
executable's default stack. The parent logs abnormal child exit codes alongside
the child artifact directory.

Scene readiness requires successful streamed textures. A terminal texture request
failure reports `scene.texture_load_failed` before testing pending streams, so
optional interactive fallback materials cannot silently become complete capture
or profile evidence. Pending work without a terminal failure retains the bounded
readiness timeout. Before authored warmup begins, a typed frame OOM may retry only
when Application has scheduled an uncommitted bounded Scene-reduction generation.
That failed frame contributes no samples and cannot start capture or measurement.
Other frame errors and errors during authored phases remain terminal. This lets
the harness observe the app's approved startup recovery without treating failed
frames as rendering evidence.

The child consumes pinned metrics snapshots, checks required sample validity,
collects completed GPU timings by source serial, and records bounded events.
Metal has both per-pass timestamp collection and submission feedback; unavailable
or unsupported results remain explicit. A Metal graphics pass's interval starts
after the vertex stage and ends after the fragment stage, so it includes the
vertex and tiling work Apple GPUs run before fragment shading. Until 2026-10-01
both stamps followed the fragment stage and graphics-pass rows omitted that
work: in Bistro each shadow cascade read about 0.4 ms against about 2 ms of real
cost, and the pass sum was 26.2 ms against 34.3 ms of whole-submission time.
With the correction the sum is 34.24 ms against 33.26 ms with the sun up and
24.86 ms against 24.44 ms at night. Compare graphics-pass rows only between
reports on the same side of that change.

The vertex stage can start while earlier encoders still run, because a
pass's consumer barrier blocks only the stages that read the producers. Since
2026-10-02 a graphics interval therefore starts no earlier than the end of the
previous timed pass in execution order, so overlap is counted once, in the
earlier pass
([`vkr_metal_packet_commands.inc`](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc)).
Before, on the Metal desktop implementation, `World.Blend.Fullscreen` with no
transparent draws read 7.4 ms in the Bistro street view, and the pass sum
exceeded the frame; after, it reads
0.05 ms. Compute intervals are unchanged.

Vulkan writes a pass's begin stamp at the top of the pipe before its barrier
and its end stamp at the bottom of the pipe, which waits for all earlier work.
A pass that does not depend on its predecessor therefore began timing while
that predecessor still ran. Since 2026-10-03 every Vulkan interval, compute or
graphics, starts no earlier than the end of the previous timed pass
([`vkr_vulkan_renderer.c`](../../renderer/src/vulkan/vkr_vulkan_renderer.c)).
Before, in the Vulkan Bistro street view at 1280x720 (RX 6700 XT,
`local_shadow_bistro_vulkan_street`), `AO.PrefilterDepth.0` read 1.94 ms
behind `Shadow.LocalMask` and `HZB.BuildMip.7` 1.62 ms behind
`Lighting.Deferred`, and the pass sum was 11.78 ms against a 5.95 ms frame;
after, they read 0.009 and 0.004 ms and the sum is 5.95 ms against 5.98 ms.
Compare Vulkan pass rows only between reports on the same side of that change.

The pass catalog freezes once the set of pass names from the requested scene
stays unchanged for eight completed frames. Retained shadow cascades and local
shadow faces leave passes out frame to frame, so a per-frame list may never
repeat; a pass that first appears later joins the catalog by name, with its
earlier frames omitted. Repetitions merge their catalogs by name, and a pass a
repetition never produced counts as omitted in its frames. Metal hands
completed results to the renderer in submission order, and a pass-timed frame
that also carries submission feedback waits at slot reuse for that feedback,
so enabling both timings no longer leaves a frame without GPU pass rows. In
three 600-frame Bistro repetitions with a moving sun and both timings, every
frame had complete pass coverage and 1800 of 1800 submission samples were
valid; before, most such runs lost one frame's pass timings. A bounded completion drain after the
measurement window does not turn missing results into valid zero durations.
Work-volume and stability checks govern timing authority independently of
execution success.

Run artifacts live under `build/_artifacts/`. Ordinary runs do not change
`tools/baselines/`. `baseline propose` writes a digest-addressed review plan;
`baseline accept` verifies its confirmation digest, source artifacts, and prior
generation before publishing an immutable generation and atomically replacing
`current.json`. Cross-backend comparison is explicit and still requires
matching workload and policy fingerprints. The workload fingerprint records a
tiled run's graphics pipeline class (`renderer.graphics_pipeline`), so a Metal
run and a Vulkan run of one case never match
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7).

The workload fingerprint's `case.scene_content` digests the scene's dependency
closure by path and host-neutral identity
([`vkr_harness_scene_manifest.c`](../../tools/harness/vkr_harness_scene_manifest.c)),
so two hosts holding the same sources produce one digest. Text dependencies
hash without the CR of CRLF line endings. A cooked `.vkt` hashes its image
shape and KTX2 metadata, which records `vkr.source_hash` and the packing intent,
without the writer and the block-encoder fields of `vkr.pack_settings`
(`encoding`, `astc_rg`, and the retired `uastc`, `basis_rg`): ASTC on Apple and
BC on x86-64 are the same workload (ADR-012). A cooked `.vkb` hashes its source
fingerprint and codec settings (`vkr_mesh_cooked_read_identity`), and the glTF
fingerprint itself ignores CR line endings (`vkr_cgltf_source_fingerprint`).
Any other file, or a `.vkt` without a source hash, hashes its bytes. Outside a
managed workspace the child applies the scene's editor overrides,
`<scene>.editor.json`, after the scene resolves, as the application runtime
does, and fails the repetition when they do not load; the closure includes that
file when it exists, so overrides change the digest. `VKR_SHADOW_EVSM=1`
enables far-cascade EVSM (ADR-041) in every child, like `VKR_SSR_QUALITY`, and
adds `renderer.shadow_evsm` to the workload fingerprint only when set. A change of
block encoder on one host therefore shows in the pixel comparison rather than in
the fingerprint. Introducing this identity on 2026-10-03 changed every workload
fingerprint once, so each accepted generation needs one re-acceptance on the
host that produced it.

## Metal crash diagnostics

`VKR_METAL_DIAGNOSTICS_DIR` opts into a separate CPU observation log. The renderer
creates a new directory exclusively; existing directories are rejected without
overwriting prior evidence. One renderer-owned sink writes two rotating 4 MiB
JSONL segments, with bounded 4 KiB records. The memory adapter borrows the sink
until adapter destruction. No worker or submission-feedback callback writes it.
With the option unset, diagnostic code does not open files, format records, or
query extra completion counters.

Records cover queue submissions, completion waits, command-slot reuse, planned
passes, Scene running/stopped state, and GPU resource creation/retirement/collection.
`seq` orders both segments. `submitted` is the renderer's serial counter, which
is assigned before commit; a begin record does not prove submission succeeded.
`completed` is an observed completion value. Memory events use zero for counters
the memory adapter does not know; their explicit `retire_after` field is the
retirement deadline. Planned passes and CPU commit returns do not prove GPU
execution. Text truncation is marked, and rotation removes older history.

Submission, wait, idle and shutdown boundaries request `fsync` for both segments.
This does not guarantee that the last record survives a kernel panic. An I/O
failure reports once and disables diagnostics without changing renderer work.
The added I/O perturbs scheduling, so these logs cannot support performance claims.
[`inspect_metal_diagnostics.py`](../../tools/inspect_metal_diagnostics.py) reads
both segments in sequence order and reports incomplete records without launching
a renderer.

The empty-stopped and tiny-node transport diagnostic cases are prepared for
separate, bounded native runs; CPU manifest checks do not establish GPU stability.

## Consequences

FSR cases default to FXAA disabled. An explicitly authored
`renderer.fxaa_enabled` overrides that default, so post-upscaler filtering can
be compared with the runtime. Capture channels select their replay render mode:
use `unlit` for unlit output; `final_color` selects the normal shaded path.

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
