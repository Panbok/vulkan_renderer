---
status: implemented
updated: 2026-10-03
authority: adr
---

# ADR-043: Physical-pixel presentation, color transfer and image sharpness

## Status

Accepted.

## Context

Logical window dimensions, physical output pixels and internal Scene pixels
are distinct. Gamma encoding in shaders plus sRGB attachments applies transfer
twice; blending encoded UI colors gives a different result from linear blending.

## Decision

This decision owns SDR transfer and spatial output filtering. Optional
extended-linear display output is being integrated under
[ADR-061](061-extended-linear-display-output.md), including its headroom, native
unit conversion and output-format lifetime contract.

Window targets expose physical client pixels. Windows establishes Per-Monitor
V2 before window creation, handles monitor DPI changes and updates content scale.
The editor/UI convert logical sizing before layout under ADR-036. Scene output
and internal rendering use ADR-039's explicit mapping.

Keep HDR lighting and post effects linear through the selected display transform. Metal and
Vulkan final shaders emit linear RGB into sRGB window/offscreen attachments;
attachment conversion performs the output encode. Authored UI/text colors decode
from sRGB once on the CPU before blending. Texture color/data intent is resolved
at loading/publication, not repaired by extra material shader gamma powers.


AgX is the default display transform; ACES fitted remains selectable for comparison.
The compact shared AgX kernel uses Rec.2020 inset/outset matrices, a 16.5-stop
log range and a polynomial contrast curve. It follows Filament's compact transform,
not the complete Blender OCIO configuration. It returns linear display RGB so the
attachment still owns the single sRGB encoding step.

Frame-input version 33 exposes normalized temperature/tint in [-1,1], contrast in
[0.5,1.5], and saturation in [0,1.5]. Neutral values are 0,0,1,1. Runtime and harness
controls share these validated ranges. A frame-owned 64-byte block holds the CAT02
white-balance matrix and grading controls; CPU preparation occurs once per frame.
After exposure, apply white balance, luminance contrast about 0.18 and saturation,
then the display transform. Metering, bloom and scene-linear temporal histories
precede grading. Neutral grading bypasses the shader work. When FXAA or
sharpening filters the final draw, the display transform runs once per output
pixel before filtering (see below). Editor composition and diagnostic modes
bypass grading.
Capture summary version 9 preserves the controls; versions 2–8 migrate to neutral
grading and ACES fitted to reproduce their historical presentation.

A native Metal Release emissive grid spans six colors at radiance 0.01–64.
AgX, ACES fitted and non-neutral grading match independent CPU references within
one 8-bit code value. Their captured pre-display HDR bytes are identical.
Metal API validation passes; GPU shader validation remains unresolved after a
MetalTools report-decoding crash. Native Vulkan execution is unavailable.

Direct mode tonemaps to the target. Editor mode tonemaps/composites the Scene
rectangle and draws native-resolution UI afterward. Output-space FXAA stays in
the final draw, with offsets expressed in output pixels. It filters only frames
without temporal reconstruction; portable TAA, MetalFX and FSR frames omit it.
On the M1 Pro, `bistro_metal_production_040` (0.4 render scale, portable TAA)
ran at 17.54 ms per frame with FXAA and 16.27 ms without, and an orbit frame
changed by more than 8/255 in 2,044 of 921,600 pixels (non-authoritative
`local-windowed-gpu-single` runs, 2026-10-01). Frames with TAA disabled keep
FXAA. HDR/intermediate and
final-color captures are different contracts and must be compared accordingly.

The user-approved `VkrFrameGlobals.image_sharpness` control applies to FSR,
portable TAA, MetalFX and native Scene presentation. It accepts finite values in
[0,1]; zero bypasses sharpening, and the sample app starts at 0.25. The harness
default is zero so existing cases retain their settings. Frame-input version 32
makes the added control explicit.

Sharpen post-tonemap linear RGB in the existing final draw, preserving alpha.
A four-neighbor cross average supplies the unsharp residual; clamp that residual
to the sampled color envelope to limit ringing. With FXAA enabled, reuse its
nine samples, sharpen its selected result and multiply strength by
`1 - subpixel_blend`. The low-contrast FXAA exit still sharpens when requested.
Without FXAA, a nonzero strength adds four samples; zero adds no samples or
filter arithmetic. Sampling offsets use output pixels and clamp at image edges.
This is bounded detail recovery, not FSR RCAS or a new antialiasing algorithm.

Sharpening adds no temporal history; its source is the display-linear image
described below. Editor.Resolve applies the filter once before overlays;
Editor.Composite bypasses it. UI and diagnostic render modes are excluded. The
SDK's FSR sharpener stays disabled, avoiding two sharpening stages. Native roots
and the outstanding Metal validation gate are recorded in ADR-044. Increased
edge contrast can expose existing temporal variation, so quality and cost
require matched static and moving captures.


## Display-linear preparation

Default scene rendering filters an output-resolution RGBA16F intermediate
whenever the final draw filters: FXAA on frames without temporal
reconstruction, or nonzero sharpening. `VKR_POST_TRANSFORM_CACHE=0` keeps the analytic reference
path, which transforms every filter sample; unset, empty and other values
select the intermediate. A preparation draw applies exposure, grading, AgX or
ACES, and the scene-relative extended-linear mapping once per output pixel. It
omits FXAA, sharpening and the physical display scale. The final draw filters
that image, then applies the physical output scale once. Editor.Resolve follows
the same split; Editor.Composite continues sampling its already converted Scene
image. Diagnostic render modes and frames without FXAA or sharpening keep the
single analytic draw.

The graph declares separate fullscreen and editor targets and activates only
the current target. Each image uses `PER_IMAGE` storage with final-target
extent, independent of the internal scene render scale. Native graph owners
retain image storage through its final sampled use and completion, including
resize and cancellation. The logical payload is eight bytes per output pixel
per realized instance: 21.09 MiB for three 1280×720 instances or 47.46 MiB for
three 1920×1080 instances, before allocator alignment and resize overlap.
No temporal history or shader-root fields are added.

This moves a nonlinear transform before fractional FXAA sampling and introduces
FP16 storage, so edges and saturated highlights differ from the analytic path.
It trades one full-image write and subsequent sampled reads for repeated
analytic arithmetic. The harness adds `renderer.post_transform_cache` to the
workload fingerprint and reports `effective_config.post_transform_cache_enabled`
whenever a case's frames use the intermediate; the analytic reference keeps its
earlier identity. Different transform paths are separate quality/cost
observations, not equivalent-work speedup evidence.

[The Bistro comparison case](../../tools/cases/local/post_transform_cache_bistro.case.json)
uses bright opaque, blended and transmitting surfaces with AgX, non-neutral
grading, FXAA and 0.4 sharpening at 640×360. On 2026-09-24, 13.91% of its
final pixels differ from the analytic path, 0.8% by more than 32 codes and at
most 201/255: the analytic path's bright rim around emissive edges becomes an
antialiased edge. Two runs of either path differ in at most 32 pixels. Its
[capture-free counterpart](../../tools/cases/local/post_transform_cache_bistro_cost.case.json)
retains that workload at 1280×720, where the
[2026-09-12 M1 Pro evaluation](../../assets/verification/renderer-features/renderer-features-perf.txt)
measured 1.381 ms analytic post against 0.128 ms preparation plus 0.326 ms
finish.

On 2026-09-24 the owner made the intermediate the default after a local M1 Pro
comparison of `bistro_metal_production_040`: 2560×1440 output from 1024×576,
TAA and FXAA, `local-windowed-gpu`, five children of 300 frames per path.
Post.Tonemap took 3.630 ms (SD 0.570) analytically, against 0.372 ms
preparation plus 1.260 ms finish; mean frame wall time fell from 22.850 ms to
20.813 ms. These runs are non-authoritative: the tree was dirty and warmup did
not stabilize. At that output the intermediate holds 84.4 MiB across three
images. Tracked baselines accepted before the change match
`VKR_POST_TRANSFORM_CACHE=0`; default runs report a fingerprint mismatch until
new generations are accepted. Native Vulkan execution, EDR scaling and
authoritative clean-tree timing remain open.


## High-DPI rendering switch

On 2026-10-03 the owner added a switch for the window's pixel density, so that
Scene cost can be compared with and without Retina pixels. The machine-local
Graphics setting `high_dpi` (Display group, default on) selects the drawable's
pixels per point:

| `high_dpi` | macOS drawable | Content scale | Result |
|---|---|---|---|
| on | backing scale, 2 on a Retina display | backing scale | Physical pixels, as before the switch |
| off | 1 pixel per point | 1 | A quarter of the pixels at 2x; the compositor scales the image up, so the Scene and the UI both lose detail |

The window is created at the saved density. `vkr_window_set_high_dpi` changes
it on the window's thread: it publishes the content scale and pixel size,
sizes the `CAMetalLayer` drawable and dispatches a resize, so the swapchain and
the UI layout follow without a restart. Every macOS drawable-size path goes
through one helper, so live resizes, display changes and mouse coordinates
use the selected scale. Both backends present through this layer. Windows
uses Per-Monitor V2 and always renders physical pixels:
`vkr_window_high_dpi_switchable` is false there, the setting shows as
disabled and loads as on. The editor Cmd bar reads and assigns
`gfx.high_dpi` (ADR-075). Harness children do not read Graphics settings:
windowed cases keep the backing scale, and offscreen cases set their pixels
with `resolution`.

The reason is cost. Scene passes scale with output pixels, so a Retina editor
window costs far more than the 1280x720 output that most Bistro cases use.
The following were measured on the M1 Pro, Metal Release, at the Bistro street
view of `tools/cases/local/local_shadow_cache_bistro_metal_street.case.json`
with `shadow_preset` high, `shadow_map_size` 2048 and TAA on, each a single
`local-offscreen-gpu-single` run of 120 measured frames
(`VKR_LOCAL_SHADOW_FADE_DISTANCE=0.001` for the rows without local shadows).
They are local observations, not matched speed claims:

| Output | Render scale | Local shadows | `frame.wall` p50 | Report SHA-256 |
|---|---|---|---|---|
| 1280x720 | 0.7 | all 72 lamps | 8.52 ms | `84559b8f534917454a8235198afc88c930484062eb55fe62351f6552ab6b09dd` |
| 1280x720 | 0.7 | none | 6.64 ms | `39c13a0528ba8e2959a871078bc5511cf7942e74622d8a7942fefddc7a2fde95` |
| 1280x720 | 1.0 | all 72 lamps | 14.33 ms | `6c4de5d971fc1da440c384f67935088e3e3b331aaf9916e16b9322fcc96a5566` |
| 3024x1890 | 0.7 | all 72 lamps | 38.31 ms | `23cb8025288ba9641c5bfc4a42de0c9cbd5df4ccf2b9fe99bd14fb51d7a9e987` |
| 3024x1890 | 0.7 | none | 28.61 ms | `45398084ca64bd1c3c4e4244016178925152d862d7a9daaf7b8d9635567b739b` |

The frames are GPU-bound; `Lighting.Deferred` and `Shadow.LocalMask` grow
from 1.67 and 1.48 ms at 1280x720 to 9.77 and 7.81 ms at 3024x1890.

In the editor's default window on the same host, Bistro with the High preset,
0.7 render scale, MetalFX without dynamic resolution and vsync off rendered
the Scene at 1359x749 in 17.7 ms with `high_dpi` on and at 680x375 in 7.3 ms
with it off (median of the last 120 frame intervals, `stats.frame_ms`).
Switching back restored 1359x749 and 17.8 ms without a restart, with no
errors in the log:

```sh
./build_release/editor/vkr_editor --scene assets/scenes/bistro.scene.json \
  --exec 'wait.scene; gfx.dynamic = false; gfx.vsync = false;
          gfx.preset = "high"; gfx.render_scale = 0.7; wait 20;
          stats.render_width; stats.frame_ms; gfx.high_dpi = false; wait 8;
          stats.render_width; stats.frame_ms; gfx.high_dpi = true; wait 8;
          stats.render_width; stats.frame_ms; quit discard'
```

The run isolated `HOME`, `VKR_EDITOR_LAYOUT_PATH` and
`VKR_GRAPHICS_SETTINGS_PATH` under `.scratch/`. Vulkan on macOS, image
quality with the switch off and Windows behavior were not checked.

## Consequences

Output transfer is shared while native surface formats differ. Correct source
transfer does not prove mixed-DPI interaction, translucent fixtures or final-color
baseline acceptance. Offscreen rendering cannot validate monitor transitions.

## Sharpness validation

Windows Release, RX 6700 XT, Bistro at the supplied camera, output 1858x1057:
FSR, portable TAA and native captures pass at strength 0.25; strength 1 and
FXAA on/off paths also pass. Inspected stationary windows and road detail gain
about 3.3% and 4.7% in local gradient magnitude. This measures contrast, not
recovered scene resolution. Moving FSR range rises roughly 9–11% in the same
regions, so sharpening does not replace temporal antialiasing. The static pair
remains stable (mean RGB difference 0.000092/255).

In three local Release children per setting, with FXAA off, presentation-pass
mean cost changes from 0.1075 to 0.1610 ms. Summed GPU pass means change from
5.5180 to 5.6286 ms, with all 61 pass rows and draw counts preserved. These
are dirty-tree observations, not authoritative timings or elapsed GPU-frame
time. Native editor captures retain all 5,185 colored UI pixels outside the
Scene rectangle exactly while changing Scene pixels.

Release and Debug wrappers pass. The enabled editor/text case passes three
assertions under Khronos synchronization validation with no API warnings or
errors and empty stderr; the known bootstrap publication warning remains outside
measured frames. Native Metal compilation and execution are unavailable here.

```powershell
.\build_release.bat
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/sharpness_bistro_fsr_motion.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/sharpness_bistro_editor_ui.case.json --profile tools/profiles/local-metal-windowed-validation-serial.json
.\build_release\tools\vkr_harness.exe profile --case tools/cases/local/sharpness_bistro_cost.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build.bat Debug
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/sharpness_bistro_editor_ui.case.json --profile tools/profiles/local-metal-windowed-validation-serial.json
```

Motion, cost and native diagnostic report SHA-256 values are respectively
`a40d7b15f06611e32cf3ff62f17736bf1ef643b5e487826084f0c1e1e2ae297b`,
`4f592a8777d478e142b0ab5b3a159f596ff8895af8de0c8870b6988349d8537a` and
`9ef330905efa7bebcbb0e91a59def4e251e1c8ff26933400f4790c53e68a4857`.

## Alternatives considered

Shader gamma plus sRGB attachment encoding is double conversion. Scaling the
physical target to reduce Scene cost also degrades native UI; the high-DPI
switch accepts that loss only when the user turns it off. Blending authored
sRGB values directly treats encoded values as linear.

## Revisit when

HDR display output, another surface format or a new DPI/presentation platform is
authorised with explicit transfer and coordinate semantics.

## Implementation

[`vkr_color_transfer.c`](../../renderer/src/vkr_color_transfer.c),
[`vkr_platform_windows.c`](../../lib/src/platform/vkr_platform_windows.c),
[`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c),
[`vkr_window_macos.m`](../../runtime/src/platform/vkr_window_macos.m),
[`vkr_graphics_settings.c`](../../runtime/src/vkr_graphics_settings.c),
[`vkr_vulkan_target.c`](../../renderer/src/vulkan/vkr_vulkan_target.c), and
[`tonemap.metal`](../../renderer/src/shaders/metal/msl/post/tonemap.metal).
