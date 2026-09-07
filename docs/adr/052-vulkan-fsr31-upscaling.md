---
status: implemented
updated: 2026-09-07
authority: adr
---

# ADR-052: Vulkan FSR 3.1 temporal upscaling

## Status

Accepted.

## Context

Vulkan previously had only same-resolution portable TAA and rejected non-unit
spatial scale. MetalFX is a Metal-only scaler, so it cannot provide a Vulkan
upscaling path while retaining native output and UI resolution.

## Decision

Add `fsr31` as a Vulkan-only temporal upscaler. It accepts a fixed render scale
in `[1/3, 1]`, including Native AA at one, and forces temporal inputs. It has no
frame generation or dynamic-resolution control. Metal retains MetalFX; inactive
FSR graph declarations do not activate a Metal path.

FSR uses the portable jitter convention and a scale-dependent Halton phase count.
The preparation pass receives raw HDR, temporal validity, opaque depth, and
nearest transmission depth when present. It writes FSR depth, reactive and
composition inputs. The SDK also consumes normalized `previous_uv - current_uv`
motion and upscales to output-sized scene-linear HDR before exposure, bloom,
tonemap and UI.

Optical contrast between pre- and post-transmission HDR contributes only to the
composition mask. The reactive mask retains authored material reactivity and
missing-motion rejection. Feeding optical contrast into both masks rejected too
much history around moving glass; the user approved separating these signals.

When the whole rendered scene is unchanged, reuse portable TAA's eligible scene
signature and native radiance, publication and graph revision checks to suppress
optical composition contrast. Commit that proof with the previous successful FSR
submission; camera or scene changes restore composition masking. Authored
reactivity and missing-motion protection remain active in stationary scenes.

The user-approved static convergence pass follows the SDK dispatch. It averages
128 stationary FSR output samples at each canonical output pixel, then copies
the completed history exactly. Camera, geometry, lighting or publication changes
immediately preserve the current SDK RGB and clear the sample age. A nonzero
reactive mask in the corresponding jittered bilinear footprint also clears age,
so authored reactive and missing-motion surfaces continue updating. The SDK
still dispatches every frame and retains its private accumulation rules.

Reuse the graph HISTORY pool for `fsr31_output_color`: five RGBA16F output
images instead of three per-target images. At 1858x1057 this adds approximately
30 MiB and one output-resolution compute pass. Alpha stores the private sample
age; exposure and bloom consume RGB, and the Vulkan fullscreen opaque-alpha flag
prevents age from reaching the final target when bloom is disabled. Other modes
retain their existing fullscreen alpha behavior. The exact preceding submitted
FSR record identifies both transform and color history. A same-queue dependency
orders history reads, and submitted producer/consumer use protects physical reuse.
Resize and teardown release graph images only after their GPU uses complete.

Pin FidelityFX SDK `v1.1.4`, which supplies FSR 3.1.4. Keep its private
resources, descriptor sets and pipelines behind a C bridge; VKR continues to
own graph resources and descriptor-buffer binding. The graph recorder restores
descriptor buffers and graphics/compute offsets after SDK recording. Exact
previous-dispatch transform metadata supplies temporal state. A three-entry submitted-use ring bounds SDK
reuse; output resize or cancelled SDK recording recreates the context only after
GPU completion.

The bridge object uses the caller's allocator. SDK CPU scratch has one private,
precommitted arena for the context lifetime: about 21.6 MiB for this pinned SDK.
Destroying the context releases the arena after all submitted uses complete.

Use the SDK luma-history patch from `rgba8` to `rgba16f`. Select its FP32/default
subgroup path because VKR does not enable float16 or subgroup-size controls. This
is a compatibility choice, not a performance claim.

FSR is limited to finite perspective cameras in this slice. Orthographic,
infinite and reversed projections are unsupported. Shared presentation sharpening
uses `image_sharpness` after stabilization and tone mapping, under ADR-043.
SDK RCAS remains disabled; the shared control also supports TAA and native output.

## Consequences

The Vulkan graph has a private native upscaler exception while portable TAA
semantics remain unchanged. FSR has no Metal parity obligation. Windows Release
and Debug wrappers compile the bridge and production shaders. Bistro static,
camera-motion, Native AA and portable-TAA reference snapshots pass at 800x600;
the fixed two-thirds cases prove 533x400 internal rendering. The motion case also
checks automatic exposure and an exposure reset after reconstruction.

Native Vulkan static and editor-resize diagnostics pass with no validation
errors or warnings on an RX 6700 XT, driver 26.6.3. The editor case observes
400x300 and restored 320x240 window extents; SDK contexts follow the scene panes
122x79, 202x139 and 122x79. Captures were inspected without promoting a baseline.
These are execution and lifecycle checks, with no performance or comprehensive
temporal-quality claim. Native Metal execution was unavailable.

The static mask correction was checked at 533x400 to 800x600 after 160 warmup
frames. Across four independently replayed settled checkpoints, pixels spanning
more than 8/255 in any color channel fell from 8.10% to 0.85% in the window region
and from 8.98% to 0.76% around a lantern. Portable TAA's settled control was
unchanged in those regions. Source jitter phases differed between replay sets;
these are bounded observations, not an exhaustive temporal-quality score.
The existing camera-motion case also passed. That mask-only revision retained
some FSR edge variation, motivating the subsequent static convergence pass.
The updated prepare root compiled and passed native reflection in Release and
Debug. The settled Debug case passed Khronos synchronization validation with no
warnings or errors on the same RX 6700 XT.

The static convergence check uses the supplied 1858x1057 Bistro camera with
1239x705 rendering. Four matched replay checkpoints (2336 through 2339) show no
pixels spanning more than 8/255 in the balcony railing, lantern, window-frame or
terrace-railing regions. Previously those fractions were 2.18%, 1.34%, 0.10% and
0.11%. The lantern and terrace regions are pixel-identical after convergence;
the balcony and window regions retain maximum differences of 4/255 and 2/255.
All four raw shadow cascades were already identical, confirming that shadow
caching was not the remaining source. These are local observations at this view.

A separate no-bloom capture holds a settled camera, moves it in one frame, then
holds the new view. The first changed frame replaces the old image; after renewed
convergence, the final two captures differ by at most 4/255. All final alpha
values are 255. Both cases pass their scene, publication and render-extent checks.
The corresponding Debug camera-change profile and editor resize round trip pass
Khronos synchronization validation with no API diagnostics and empty stderr.
The resize run recreates outputs at 122x79, 202x139 and 122x79. Both runs log a
startup geometry-publication warning; the measured reset case has zero omitted
candidates. Metal execution remains unavailable; this mode retains its authorized
Vulkan-only exception.

## Motion-quality limitation

A matched 1858x1057 Bistro camera rotation at two degrees/second compared native
TAA, FSR Native AA and two-thirds FSR Quality with FXAA disabled. Native FSR
retains more distinct thin detail than moving TAA in the inspected crops, but
native resolution alone does not eliminate the observed variation.

Retaining optical contrast only in the composition mask reduced
motion-compensated mean channel range from 2.414 to 1.810 in the window region
and from 5.025 to 4.135 around a lantern. Authored reactivity and missing-motion
protection remained active. An isolated opaque railing was unaffected by optical
mask removal. These scores include interpolation, shading and contrast changes;
they do not establish an absolute quality ranking or a ghosting-safe new policy.
The approved production change retains this separation. A matched one-world-unit/s
Bistro translation at the same resolution passed all capture assertions with no
obvious new trails in the inspected window and lantern crops. A temporary
analytic-fixture diagnostic moved a glass pane at 1.1 world units/s, rendering
533x400 to 800x600. Across four checkpoints, both policies placed its trailing
half-contrast edge at the same pixels, within one pixel of the independently
projected silhouette. Background residual beyond the one-pixel edge footprint
stayed below 0.87/255 for both. This is bounded ghosting coverage; arbitrary
refraction and motion remain outside that observation.

The unchanged static-reset case also passes: its final stationary pair has mean
difference 0.000184/255 and maximum 3/255, and the first changed camera frame
updates immediately. The temporary object-motion hook was removed; the retained
Bistro translation case runs on the production harness.
Final Release and Debug builds pass. The production translation case passes all
seven assertions under Khronos synchronization validation, with no API validation
warnings or errors. One bootstrap geometry-publication warning remains; measured
publication omissions are zero. The prepare root and static shader are unchanged.

Actual GPU motion readback matched an independent pure-rotation projection within
0.003 render pixels for opaque/transmission pixels. Every render pixel had valid
motion; no blended-overlay draws were present. This rules out a large motion
sign/scale error in that case, not every camera or object-motion path. The runtime
still enables post-FSR FXAA by default, while the harness defaults it off for FSR (explicit case values override this);
comparisons must control that difference. No performance claim follows.

Further matched rotation probes identify opaque specular shading as a contributor:
isolated railing range falls from 7.953 to 5.822/255 when that response is removed,
despite stronger edge contrast. GTAO removal changes it only to 7.736, and removing
shadow sampling increases it to 8.574. Unlit geometry still varies. These probes
were reverted; they guide filtering work rather than authorize removing lighting.

FXAA lowers that railing range to 6.956 with visible softening. At 80% scale it is
6.974, but windows and lanterns do not improve uniformly. Three independent local
Release profiles per scale observe summed GPU pass means of 5.531 ms at two-thirds
versus 7.252 ms at 80%, on the same RX 6700 XT. These dirty-tree measurements are
non-authoritative; submission timing is unavailable and pass sums are not elapsed
GPU-frame time. The retained `fsr31_bistro_cost66` and `fsr31_bistro_cost80` cases
run with `profile` and `tools/profiles/local-offscreen-gpu-single.json`.

Material resolve currently uses internal-resolution gradients with zero mip bias.
A temporary -1.585 base/ORM/emissive bias makes road texture more distinct while
raising both spatial detail and motion variation; it leaves railing variation
essentially unchanged. Normal-map and cutout coverage sampling were excluded.
No global mip bias was adopted. Material-aware mip tuning and thin-geometry
filtering remain quality work; sharpening cannot substitute for either.

The shared specular-AA roughness conversion is corrected under ADR-044: variance
broadens `alpha^2`, where `alpha` is squared perceptual roughness. The existing
samples, derivative coefficient and variance cap remain unchanged. Matched
two-thirds-scale FSR rotation captures observe mean channel ranges of
7.953 -> 7.931/255 on the isolated rail, 4.305 -> 4.238 on the balcony and
4.135 -> 4.080 around the lantern. Windows change from 1.810 to 1.834. These are
small, mixed changes; they do not establish a solution for moving thin edges.
TAA captures likewise show small mixed changes, and the settled supplied view
retains its detail. A tested triangle-normal fallback for pixels without matching
right/down neighbors did not materially improve the target and was removed.

Three local Release children of `fsr31_bistro_cost66` observe summed GPU pass
means of 5.526 ms with the correction, versus the prior 5.531 ms. This shows no
material added cost in this observation, not a speedup or elapsed GPU-frame time.
Motion report SHA-256 is
`dd3ac817b6c252e85cae637947ea9983c2c5073528c6294ce6569602d1eced41`; the retained math-only shader hashes identify the captured implementation.
Final Release and Debug builds pass; all 92 inventoried Release shader hashes
match the captured correction. The Debug static-reset profile passes all seven
assertions under Khronos synchronization validation with no API warnings or
errors (report `c4d72cc27ef394dc7a70f50b90264cec30bcf477a8e1e3515065a57cf13d593f`).
Its known bootstrap geometry-publication warning remains outside measured frames;
measured omissions are zero. Native Metal checks remain unavailable.

## Alternatives considered

Using MetalFX on Vulkan is unavailable. Folding SDK descriptors into VKR's
descriptor-buffer model would expose SDK ownership and layout policy. Frame
generation and dynamic resolution are outside this accepted slice.

## Revisit when

A supported camera class changes, or quality and performance evidence justifies
frame generation or dynamic resolution.

## Verification commands

Windows, 2026-09-07, ordinary Release for captures and Debug for native diagnostics:

```powershell
.\build_release.bat
.\build.bat Debug
foreach ($case in 'static', 'motion', 'native_aa', 'taa_reference') {
  .\build_release\tools\vkr_harness.exe snapshot --case "tools/cases/local/fsr31_bistro_$case.case.json" --profile tools/profiles/local-offscreen-gpu-single.json
}
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_bistro_static.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_editor_bistro_resize.case.json --profile tools/profiles/local-metal-windowed-validation-serial.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_stability_fsr.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_stability_taa.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_bistro_stability_fsr.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_user_camera_fsr.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_static_reset.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_motion_translation.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_bistro_motion_translation.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_debug\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_bistro_static_reset.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_motion_quality_quality.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/fsr31_bistro_motion_quality_taa.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe snapshot --case tools/cases/local/specular_aa_bistro_static.case.json --profile tools/profiles/local-offscreen-gpu-single.json
.\build_release\tools\vkr_harness.exe profile --case tools/cases/local/fsr31_bistro_cost66.case.json --profile tools/profiles/local-offscreen-gpu-single.json
```

The serial windowed profile is backend-neutral despite its historical filename.

## Implementation

[`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json),
[`fsr31.slang`](../../renderer/src/shaders/vulkan/slang/post/fsr31.slang),
[`vkr_vulkan_fsr.c`](../../renderer/src/vulkan/vkr_vulkan_fsr.c),
[`vkr_vulkan_fsr_sdk.cpp`](../../renderer/src/vulkan/vkr_vulkan_fsr_sdk.cpp),
[`vkr_fsr.cmake`](../../cmake/vkr_fsr.cmake), and
[`fidelityfx-sdk-1.1.4.md`](../../vendor/fidelityfx-sdk-1.1.4.md).
