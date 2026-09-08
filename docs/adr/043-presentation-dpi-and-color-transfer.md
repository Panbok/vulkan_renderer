---
status: implemented
updated: 2026-09-08
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
precede grading. Neutral grading bypasses the shader work. Each FXAA/sharpening tap
uses the same transform. Editor composition and diagnostic modes bypass grading.
Capture summary version 9 preserves the controls; versions 2–8 migrate to neutral
grading and ACES fitted to reproduce their historical presentation.

A native Metal Release emissive grid spans six colors at radiance 0.01–64.
AgX, ACES fitted and non-neutral grading match independent CPU references within
one 8-bit code value. Their captured pre-display HDR bytes are identical.
Metal API validation passes; GPU shader validation remains unresolved after a
MetalTools report-decoding crash. Native Vulkan execution is unavailable.

Direct mode tonemaps to the target. Editor mode tonemaps/composites the Scene
rectangle and draws native-resolution UI afterward. Output-space FXAA stays in
the final draw, with offsets expressed in output pixels. HDR/intermediate and
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

No image, graph pass or temporal history is added. Editor.Resolve applies the
filter once before overlays; Editor.Composite bypasses it. UI and diagnostic
render modes are excluded. The SDK's FSR sharpener stays disabled, avoiding two
sharpening stages. Native roots and the outstanding Metal validation gate are
recorded in ADR-044. Increased edge contrast can expose existing temporal
variation, so quality and cost require matched static and moving captures.


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
physical target to reduce Scene cost also degrades native UI. Blending authored
sRGB values directly treats encoded values as linear.

## Revisit when

HDR display output, another surface format or a new DPI/presentation platform is
authorised with explicit transfer and coordinate semantics.

## Implementation

[`vkr_color_transfer.c`](../../renderer/src/vkr_color_transfer.c),
[`vkr_platform_windows.c`](../../lib/src/platform/vkr_platform_windows.c),
[`vkr_window_windows.c`](../../runtime/src/platform/vkr_window_windows.c),
[`vkr_vulkan_target.c`](../../renderer/src/vulkan/vkr_vulkan_target.c), and
[`tonemap.metal`](../../renderer/src/shaders/metal/msl/post/tonemap.metal).
