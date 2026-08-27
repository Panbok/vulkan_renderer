---
status: partial
updated: 2026-08-27
authority: design
---
# Automatic exposure, bloom, and ambient occlusion

**Document status:** Partial. Automatic-exposure phases E0-E3 and bloom phases
B0-B1 have production Metal and Vulkan implementations. Metal runtime,
capture, timing, and validation evidence passes for both features; native
Vulkan runtime validation remains pending. GTAO has no production
implementation. The architecture specification remains the status authority.

**Scope:** Three separate renderer features. They share HDR and deferred inputs,
but each adds graph resources and passes and each must land with its own
evidence.

## 1. Recommendation

Implement automatic exposure first, after the presentation transfer function is
fixed. It changes how every later image-quality result is judged and supplies
the display calibration for later post effects.

Implement bloom second. Implement GTAO last because it needs an AO-specific,
current-frame view-space depth pyramid and a deliberate indirect-lighting
contract. Do not reuse `hzb_history`.

## 2. Current behavior

`VkrFrameGlobals.exposure_mode` selects manual or automatic exposure.
Production initialization selects automatic mode and uses the finite, positive
`0.30` multiplier as its empty-histogram fallback. Explicit manual mode
preserves that multiplier and its byte-identical output. Automatic mode meters
and adapts entirely on the GPU. Fullscreen tonemapping and editor composition
consume the current resolve directly. Delayed completed readback supplies
diagnostics, metrics, and canonical post-tonemap capture metadata.

Bloom ships as a packet-controlled, scene-linear threshold/downsample/reverse-
upsample/combine chain on Metal and Vulkan. Production initialization enables
it; deterministic harness cases default it off unless explicitly authored.
Material occlusion is stored in `gbuffer_albedo.a` and affects indirect diffuse
plus the existing specular-occlusion approximation. Direct analytic lighting
is not occluded by that term.

## 3. Automatic exposure

### 3.1 Metering

Meter the final pre-bloom, pre-tonemap HDR scene after transmission and ordinary
blend. Use log luminance with a configured finite range. Ignore invalid values
and near-zero background values according to an explicit policy.

A raw arithmetic mean is too sensitive to a few hot texels and to letterbox or
background area. Resolve the histogram through configurable low and high
percentiles, then compute the weighted log-average of the retained bins. Record
the exact bin count, luminance range, percentiles, middle gray, minimum EV,
maximum EV, and adaptation rates in one cold configuration record.

Two graph passes are sufficient:

- `Post.Exposure.Histogram` clears and fills a bounded histogram; and
- `Post.Exposure.Resolve` reduces it and writes the target exposure.

Start with group-shared bins and one bounded global merge per workgroup. Do not
assume subgroup pre-reduction is faster. Histogram samples can land in different
bins, so the useful subgroup operation must be derived from measured contention,
not copied from a same-predicate ballot.

### 3.2 Packet contract and units

The existing `exposure` field is a linear multiplier. An EV bias is
logarithmic. One field cannot mean both without silently changing old callers.

Version the packet contract:

- `exposure_mode`: manual or automatic;
- `manual_exposure`: the current linear multiplier, preserving `0.30` and
  byte-identical manual output;
- `exposure_compensation_ev`: an additive EV bias used only in automatic
  mode; and
- optional metering controls in a cold renderer configuration or a bounded
  packet block if they genuinely vary per frame.

The automatic result is a GPU value. Do not read it back for the tonemap pass.
Metrics may publish a delayed completed value for observability.

### 3.3 Temporal state

Exposure adaptation is history, not ADR-029 retention. A single retained buffer
read and written by overlapping frames would either race or serialize the frame
path. Use a completion-safe history ring:

1. select the newest completed valid exposure as input;
2. write the current target and adapted exposure to the current frame slot;
3. publish it only after successful submission; and
4. invalidate the chain on resize, scene change, camera cut, mode change, or a
   discontinuity declared by the application.

The adaptation equation must use a bounded frame delta and separate brighten
and darken rates. Define which direction each rate applies to. A skipped frame
must not publish state that the GPU never wrote.

### 3.4 Determinism

Default regression and golden cases should keep manual exposure unless the case
explicitly tests adaptation. An automatic-exposure case must provide a fixed
time step, manual fallback, reset point, and frame count. "Warm up until it
looks converged" is not deterministic evidence.

Expose debug outputs for the histogram, clipped percentile range, target EV,
adapted EV, and reset reason.

### 3.5 Implemented automatic-exposure path

Packet version 20 replaces the single `VkrFrameGlobals.exposure` multiplier with
`exposure_mode`, `manual_exposure`, and `exposure_compensation_ev`, and adds a
renderer-owned `VkrExposureFrame exposure` block alongside the temporal block.
Making the old field name a struct is deliberate: an application still assigning
a float to `globals.exposure` fails to compile rather than silently changing
meaning.

`vkr_exposure.h`/`.c` own the contract. `vkr_exposure_prepare()` runs in
`vkr_renderer_prepare_packet()` and `vkr_exposure_commit()` runs only after a
successful submit on both the Vulkan and Metal paths, so a rejected or cancelled
frame cannot advance the chain. The frame carries the validated manual
multiplier, the compensation bias (zeroed in manual mode, since an EV bias
against a linear multiplier has no defined meaning), a frame delta clamped to
`[0, 0.25]` seconds, the reset mask, and `history_valid`.

Exposure does not re-derive discontinuities. `vkr_temporal_prepare()` already
computes first frame, frame gap, extent change, scene change, camera cut, and
explicit reset at the same boundary and does so independently of whether TAA is
enabled, so `VKR_EXPOSURE_RESET_TEMPORAL_MASK` consumes those bits directly and
adds one exposure-only `VKR_EXPOSURE_RESET_MODE_CHANGE`. Projection change is
excluded: a lens change is not a brightness change.
`vkr_renderer_invalidate_exposure_history()` lets the application declare a
lighting discontinuity without discarding pixel history.

Packet validation now rejects a non-finite or non-positive `manual_exposure`, a
non-finite `exposure_compensation_ev`, and an unsupported `exposure_mode`. That
made Metal's `exposure > 0 ? exposure : 1.0` substitution provably dead and it
was removed; every submit is validated before either backend sees the packet.

Each selected backend normalizes the shared `VkrExposureMeteringConfig` defaults
once during initialization. Production uses 256 bins over log2 luminance
`[-10, 10]`, rejects luminance below `1e-4`, retains percentiles `[0.5, 0.95]`,
maps to middle gray `0.18`, clamps target EV to `[-8, 8]`, and adapts at 3 EV/s
toward a brighter display and 1 EV/s toward a darker display. The shared
CPU/Slang kernel uses fractional percentile-bin weights, saturates
out-of-range valid luminance into the edge bins, holds the previous decision
for an empty histogram, and snaps an invalid history chain directly to the
measured target.

The authored graph runs `Post.Exposure.Histogram` and
`Post.Exposure.Resolve` after the mutually exclusive fullscreen/editor temporal
resolve. One histogram pass meters `temporal_history_color`, the final composed
HDR scene before bloom composition and tonemapping. It clears and accumulates
group-shared bins. Resolve then writes target EV, adapted EV, the linear
multiplier, accepted count, retained-bin range, average log luminance,
and reset reasons.

Both backends select the newest completion-proven record from a graph-owned
history ring, write the current frame's state to a separate instance, and
publish it only after successful queue submission. A reset invalidates the old
chain at submission, so an older completed record cannot cross a resize, scene
change, camera cut, mode change, or explicit discontinuity while the reset
frame is still in flight. Tonemapping consumes the current GPU state directly;
there is no CPU wait or readback dependency on the frame path.

Fixed delayed readback publishes `VkrExposureDebugSample`, including all 256
bins and the completed state with source frame and submit metadata. The metrics
registry exposes accepted texels, retained low/high bins, average log
luminance, target/adapted EV, multiplier, and reset reasons. Canonical
post-tonemap capture metadata is patched with the completed GPU multiplier, so
fullscreen, editor, and capture report the exposure actually applied rather
than the automatic fallback.

Harness cases explicitly author automatic mode, a positive fallback,
compensation, a fixed measured reset frame, fixed frame counts, and fixed
simulation time. They disable TAA explicitly so the fixture meters the
post-temporal passthrough without inheriting an asynchronously reached jitter
phase. The harness scene-activation path advances `scene_generation`, matching
the application boundary and preventing empty-scene adaptation from leaking
into a loaded-scene measurement. Manual mode remains the default for all other
cases.

## 4. Bloom

### 4.1 Graph shape

Bloom reads the pre-tonemap HDR result after all scene composition and before
automatic exposure is applied to tonemapping. The histogram should meter the
pre-bloom image so bloom cannot brighten itself through the exposure feedback
loop.

Use dedicated graph resources:

1. threshold and downsample HDR into a bloom mip chain;
2. downsample through the chain;
3. upsample into a separate accumulation chain or ping-pong image; and
4. combine bloom with the original HDR input for tonemapping.

Do not read and write the same subresource in one pass. The graph supports mip
views and repeat expansion, but HZB only proves a one-way reduction chain. Bloom
upsampling reads mip `i+1` while writing mip `i`, so add a focused compiler
test for ordering and subresource barriers before assuming no graph work is
needed.

### 4.2 Filter and controls

The first implementation needs explicit controls for:

- scene-linear threshold;
- soft-knee width;
- bloom intensity;
- maximum mip count or minimum extent; and
- optional firefly suppression.

Do not freeze a 13-tap kernel in the design before comparing a smaller filter on
the target GPUs. Keep the first and last mip bounded for odd extents. Clamp or
reject non-finite HDR input so one invalid texel does not contaminate a mip
chain.

Because the threshold is scene-linear, automatic exposure changes the displayed
strength but not which scene values enter bloom. If art direction later wants
exposure-relative bloom, that is a separate stated mode.

### 4.3 Editor and capture

Fullscreen and editor branches need the same semantic position. Do not duplicate
the bloom algorithm into two shader paths. Both should consume the selected
branch's composed HDR resource and feed the branch's tonemap/composite.

Capture bloom-only, pre-bloom HDR, combined HDR, and final color independently.
Those views make threshold, chain seams, and exposure interaction diagnosable.

### 4.4 Implemented bloom path

Packet version 21 adds `bloom_enabled`, scene-linear `bloom_threshold`,
`bloom_knee`, and `bloom_intensity`. The frontend validates enabled controls as
finite and non-negative, lowers them into `VkrBloomFrame`, and preserves a
zeroed packet as bloom-disabled. Production initialization enables the defaults
`1.0`, `0.5`, and `0.05`; `VKR_BLOOM_DISABLED=1` is a persistent cold override.
Harness cases remain disabled by default and require all three controls when
enabling bloom.

`VkrBloomConfig` bounds the cold topology to at most eight mips, a minimum mip
extent, a finite R16 ceiling, and a selected 13-tap or 4-tap downsample filter.
The production default uses six mips, minimum extent eight, clamp 32, and the
13-tap filter. `vkr_bloom_mip_count()` derives the frame chain from the current
viewport; an extent too small for two levels disables the graph slice rather
than dispatching a one-level no-op.

The authored graph creates half-resolution `bloom_chain` and `bloom_accum`
pyramids plus full-resolution `bloom_combined`. `Post.Bloom.Prefilter` applies
the shared sanitizer, 13-tap Karis reduction, and soft knee. Ascending
downsample repeats reduce `bloom_chain`; a reverse repeat emits upsample passes
deepest-first into the separate accumulation chain. The deepest executor binds
the defined downsample mip instead of the unwritten accumulation mip. No pass
reads and writes the same image subresource. `Post.Bloom.Combine` adds the
resolved bloom to the original HDR source before exposure, and fullscreen and
editor tonemap select that single combined resource.

Metal and Vulkan use the same shared threshold, sanitizer, Karis weight, and
GPU parameter ABI. Backend-native tap code supplies the same 13-tap/4-tap
downsample and 9-tap upsample arithmetic. Both backends instantiate all
pipelines at initialization and select the alternate downsample filter from
the cold config, not as a runtime fallback.

Direct captures expose `hdr_pre_bloom`, `bloom_prefilter`, `bloom_result`, and
`hdr_combined`; `final_color` remains the post-tonemap output. Histogram
metering still reads `temporal_history_color`, so bloom cannot feed back into
automatic exposure.

## 5. GTAO

### 5.1 Name and boundary

GTAO is a screen-space approximation designed to track a ground-truth AO
reference. It is not ground truth and cannot see off-screen or hidden occluders.
Call the feature GTAO, not "ground-truth AO" in user-facing quality claims.

Keep it to ambient visibility. Bent normals, multi-bounce compensation,
temporal accumulation, and GI are later decisions.

### 5.2 Inputs

Use:

- current-frame depth converted to view-space distance;
- current-frame view-space normals derived from `gbuffer_normal`;
- projection constants and output extent; and
- a dedicated AO depth mip chain with an AO-specific depth filter.

Do not sample `hzb_history`. It is a completed previous-frame history image
for occlusion culling, uses normal-Z maximum reduction, and can come from a
different camera frame. GTAO references use a current-frame view-space depth
pyramid with a depth-aware weighted filter. Reusing HZB would create temporal
lag, edge halos, and the wrong mip semantics.

If MSAA ships, define a representative current-frame depth and edge policy for
AO. A minimum or maximum depth resolve alone cannot represent two surfaces in
one pixel. The safe first choice is one AO value for the representative opaque
surface with discontinuity-aware filtering, plus no claim that AO is
per-sample.

### 5.3 Passes

A practical first topology is:

1. `AO.PrefilterDepth`: convert current depth to view space and build the
   dedicated mip chain;
2. `AO.Evaluate`: horizon search using view-space normal and depth;
3. `AO.Denoise`: one or more edge-aware spatial passes; and
4. `Lighting.Deferred`: sample the final visibility.

Use one single-channel AO output and keep intermediate edge data separate.
Half-resolution AO may be evaluated later, but full resolution is the
correctness reference.

### 5.4 Lighting integration

Apply GTAO to indirect diffuse first:

`indirect_diffuse_visibility = material_ao * gtao_visibility`

Do not attenuate direct punctual or directional lighting. Do not feed the
combined value into the existing specular-occlusion approximation without a
separate visual comparison. Material cavity occlusion and screen-space horizon
visibility carry different information, and multiplying both into glossy IBL
can over-darken contact regions.

If specular occlusion needs GTAO, add it as a later isolated mode with a visible
debug channel and a reference comparison.

## 6. Phases

| Phase | Work | Gate |
| --- | --- | --- |
| E0 | Versioned manual/automatic exposure contract | **Implemented.** Manual mode is byte-identical |
| E1 | Histogram, percentile resolve, debug outputs | **Implemented.** CPU reference pins accepted samples, saturated bins, fractional percentile edges, resolved EV, adaptation, and empty-histogram hold; delayed state plus all 256 bins is observable |
| E2 | Completion-safe adaptation history | **Implemented.** Metal and Vulkan use completion-proven graph history, submit-only publication, and reset-chain invalidation; the fixed-step static case repeats exactly |
| E3 | Tonemap and capture integration | **Implemented.** Fullscreen and editor tonemap consume current GPU state, canonical capture records the completed multiplier, and manual output remains unchanged |
| B0 | Bloom resource and repeat topology | **Implemented.** Odd extents, bounded mip counts, reverse upsample ordering, and the read-after-write subresource barrier are pinned by CPU graph tests |
| B1 | Threshold, downsample, upsample, combine, debug captures | **Implemented.** Shared sanitizer/soft-knee references, separate chains, full-resolution combine, four direct HDR/bloom channels, Release execution, and focused Metal validation pass |
| G0 | Current view-space depth pyramid and normal conversion | Depth/normal debug captures agree with reconstructed geometry |
| G1 | GTAO evaluation and spatial denoise | Full-resolution reference passes motion and thin-occluder clips |
| G2 | Indirect-diffuse integration | No direct-light attenuation or unintended specular double-darkening |

Run and report E, B, and G as separate matched Release comparisons. A combined
profile cannot attribute cost.

## 7. Evidence

Each feature needs:

- focused CPU or shader-contract tests;
- `./build_test.sh`;
- focused Vulkan validation and Metal API validation;
- deterministic captures for every new debug channel;
- matched Release per-pass timings at fixed internal and output extents; and
- explicit owner review before final-color golden replacement.

Do not describe any cost or quality ranking as a VKR result until those profiles
exist.

### 7.1 Automatic-exposure implementation evidence

Local Apple M1 Pro Release evidence from 2026-08-27 is implementation evidence,
not an accepted baseline or a cross-device performance result:

- the focused CPU reference and exposure contract suites pass; the full CPU
  command later stops at the pre-existing cooked-mesh tangent assertion in
  `mesh_cooked_tests.c:166`;
- two repetitions of `smoke.auto_exposure.static`, with temporal jitter disabled
  to isolate the post-temporal input, produced identical exposure samples:
  307,200 accepted texels, retained bins 56/76, average log luminance
  `-5.0189886093`, target and adapted EV `2.5450575352`, and multiplier
  `5.8363142014`;
- both exposure passes supplied 32 valid GPU timing samples. The histogram
  observed `0.0253750 ms` p50 and `0.0297917 ms` p95, with one `0.4130833 ms`
  outlier; resolve observed `0.0106667 ms` p50. These rows prove execution and
  do not support a performance conclusion;
- fullscreen and editor snapshots passed and recorded completed multipliers
  `5.8363142` and `4.8328896` respectively rather than the `0.30` fallback; and
- one focused editor run with Metal API and shader validation enabled passed
  without a validation diagnostic. Validation timings are diagnostic only.

The repeated profile report digest was
`sha256:22630412a22a661c78d9e43c07044d4a530b19cc0f0fd316a02d3fff78ca07a8`.
The fullscreen, editor, and validation snapshot report digests were
`sha256:ab7ae0ad4dc59101864220aaff9a612e4a9b60fb43705475a3a8aa5737cf2632`,
`sha256:83c54e5c3c455859b8d17cdd5734419acf97abdeb351acf1c4979dd4332afb24`,
and `sha256:2e9de72a1c443896cee239d094104e0ccd1200bc25a3e64dee8f40a991db7a09`.

The Release build compiles both backend implementations and shared shader/ABI
tests pass. MoltenVK cannot execute VKR's descriptor-buffer Vulkan path, so a
focused native Vulkan validation run remains an evidence gap.

### 7.2 Bloom implementation evidence

Local Apple M1 Pro evidence from 2026-08-27 is implementation evidence, not an
accepted baseline or a cross-device performance result:

- the bloom CPU reference, packet validation, odd-extent mip derivation, reverse
  repeat ordering, and subresource barrier tests pass;
- `./build_test.sh` reaches and passes the bloom and render-graph suites, then
  stops at the pre-existing cooked-mesh tangent assertion in
  `mesh_cooked_tests.c:166`;
- one 801x601 Release offscreen snapshot resolves all five requested channels
  from frame 58 / submit 119. `bloom_prefilter` and `bloom_result` are
  non-degenerate, while `hdr_pre_bloom`, `hdr_combined`, and `final_color`
  preserve the expected semantic order. The report digest is
  `sha256:333b97da86b2e226c7a9528ff579888e80c70cb2034d4655a51acaea07151659`;
- one Release Metal timestamp observation supplies 16 valid samples with zero
  invalid samples for every bloom pass. Prefilter observed `0.033125 ms` p50,
  combine `0.027167 ms` p50, downsample levels `0.005083-0.010792 ms` p50, and
  upsample levels `0.005042-0.013667 ms` p50. This single dirty-tree process
  proves execution and attribution only; report digest
  `sha256:a2e11c78190cb2f8eaeef192485af399494ea10071eb74401837a238af9c33d8`;
  and
- one strictly serial Debug Metal API/GPU shader-validation profile completes
  16 measured frames with valid samples for all 12 bloom passes and no
  diagnostic beyond the two validation-enabled notices. Report digest
  `sha256:b4f4db4d6e4ab6380d8ed8ad465fd02c2d377d402ce2a1cd539379b7afe8ef1e`.

Native Vulkan validation, an authoritative matched bloom-on/off Release
comparison, and explicit owner acceptance of any final-color baseline remain
open. No cost or quality ranking is inferred from the local observations.

## 8. Primary references

- [Intel XeGTAO integration and depth-pyramid notes](https://github.com/GameTechDev/XeGTAO)
- [Jimenez et al., Practical real-time strategies for accurate indirect occlusion](https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion)
- [AMD temporal super-resolution exposure and mask contract](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
