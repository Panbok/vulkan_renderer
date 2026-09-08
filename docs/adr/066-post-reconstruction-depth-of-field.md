---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-066: Post-reconstruction depth of field

## Status

Accepted. Production compilation, compiled Vulkan contracts and the selected
Metal output/API checks pass. Native Vulkan execution remains unavailable.

## Context

The user approved optional depth of field with focus distance and f-stop,
a circular aperture capped at 16 output pixels of blur radius, 32 samples per
foreground/background layer, and separate storage after temporal reconstruction.
Transparent surfaces use the existing opaque depth. The approved image budget is
58.008 MiB for three sets at 1280×720, or 154.688 MiB for eight.

## Decision

[Frame input](../../renderer/src/vkr_frame_input.h) version 42 adds
`dof_enabled`, `dof_focus_distance` in metres, and `dof_f_stop`. Initialization
leaves the effect disabled, with focus distance 5 m and f/2.8. The existing
vertical field of view and a fixed 24 mm sensor height determine focal length;
there is no independent focal-length control. The input boundary rejects
nonfinite controls, nonpositive f-stop, focus at or inside the focal length,
and unsupported perspective depth ranges.

[CPU preparation](../../renderer/src/vkr_dof.c) lowers the thin-lens radius scale,
focus, near/far distances, output dimensions and mapping to current raster depth.
The 48-byte parameter record is embedded in native roots of 112 bytes on Metal
and 80 on Vulkan. Canonical reconstructed output coordinates account for the
current raster jitter and internal extent when sampling depth.

The [graph](../../assets/render_graphs/main.rendergraph.json) executes six compute
passes: CoC/depth, horizontal foreground dilation, vertical dilation, separate
near/far prefilter, separate near/far gather, and full-resolution composite.
The shared 32-position disk table avoids per-pixel trigonometry. Each half-resolution
gather samples 32 colors from each layer, plus CoC/depth metadata. Filtering uses
depth rejection and premultiplied foreground coverage. Sky uses the far-limit
blur radius. Half-resolution dimensions round down and retain a minimum of one.

Meter the original reconstructed HDR. Apply defocus before bloom and display
mapping, preserving UI sharpness. A separate full-resolution composite preserves
TAA and FSR history: later frames must never accumulate already-defocused color.
Disabled frames instantiate no DoF resources or passes. No DoF history is added.

Eight graph-owned transient, resizable, per-image resources use the reconstructed
Scene extent:

| Images | Format | Resolution | MiB per set at 1280×720 |
| --- | --- | --- | ---: |
| CoC and positive view depth | RG16F | Full | 3.515625 |
| Two foreground dilation images | RG16F | Half | 1.7578125 |
| Near/far prefilter and near/far blur | RGBA16F | Half | 7.03125 |
| Composite | RGBA16F | Full | 7.03125 |
| Total | | | 19.3359375 |

Existing graph allocation and GPU completion proofs govern reuse, replacement
and release. Native pipelines live with their renderer; upload roots live with
the frame submission. There is no new retained CPU or GPU owner.

## Consequences

Opaque foreground and background can defocus independently without mixing their
prefiltered colors. Already-composited transparency, refraction and fog use opaque
depth, so glass highlights and translucent foreground can blur at the wrong
distance. Screen-space filtering cannot reconstruct hidden background behind an
opaque foreground silhouette; some sharp foreground contribution can remain
where blurred foreground coverage is incomplete. This is a bounded image-space
approximation, not per-layer lens transport. No frame-time claim is made.

## Verification and limits

Release application compilation and all six production Metal/Slang entry points
pass. Compiled SPIR-V reflection validates 48-byte params and 80-byte Vulkan
roots. An independent thin-lens ray-cone oracle covers 16,000 cases with maximum
radius error 8.49e-6 output pixels; input checks reject nine invalid controls
and an unsupported projection.

`python3 tools/checks/check_dof.py .scratch/dof-native-runs.json` checks retained
native captures against the pre-change output. Disabled final and HDR payloads
are byte-identical. Measured near/focus/far CoC values are -15.426, -.291 and
9.766 pixels at depths .240, .490 and 1.590 m. The focused card retains 97.99%
of its green interior mean; foreground blur legitimately covers part of it.
The largest foreground/background horizontal edge steps fall to 77.7%/15.7%
of their respective unblurred values. These are local effect checks, not a
claim of exact lens transport. Foreground sharp remnants remain a known limit.

The first native image exposed a hard background silhouette caused by symmetric
depth rejection. Far-layer visibility now follows signed CoC and source-disc
coverage, allowing defocused positive-CoC surfaces to mix across depth changes.
Focused and foreground receivers retain their original far contribution policy.
Normalized HDR reads also handle lower-resolution spatial inputs correctly.

The same fixture passes 257×193 output, portable TAA, spatial scaling at 2/3,
and MetalFX temporal at .8 scale. A serial Metal API-validation resize capture
passes without errors, rendering the outbound 514×386 image and restoring
1026×770. Report SHA256 is
`c381d7c4476a08a5cab2d4f6cab6dc16f051ffc046f1dd75628ccd70062e6c14`.
The corrected focus capture SHA256 is
`51b0657f29ae667141c96be319757dc0130884df2105407b77f32c6a354db17f`.

Normal captures use `./build_release/tools/vkr_harness snapshot --case
 tools/cases/local/dof_focus_local.case.json --profile
 tools/profiles/local-brdf-display-validation.json` with graphics validation
variables unset. The resize uses `dof_resize_local.case.json`,
`local-metal-windowed-validation-serial.json`, and `MTL_DEBUG_LAYER=1` with
shader validation unset. The odd editor capture with motion blur and bloom passes (report SHA256
`5a3a6d9139b741b056b0fdf93fc59de287aef876bfa792cd490408eead88d03a`);
the dedicated Release editor build also passes with `env -u VKR_DISPLAY_OUTPUT -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS ./build_editor.sh Release`.
Its log is `.scratch/renderer-improvements-post-effects-editor.log`. Native Vulkan
execution and bilateral comparison remain unavailable on this Mac, so DoF
remains UNALIGNED under [ADR-044](044-shader-cross-backend-contract.md).

## Alternatives considered

Overwriting reconstruction output would contaminate temporal history. A single
mixed color prefilter loses foreground/background separation at silhouettes.
A per-layer lens model requires additional visibility, storage and transport.

## Revisit when

Measured output or frame cost requires a different radius, sample count,
resolution, aperture shape or transparency model. Such changes require a new
quality and resource budget.
