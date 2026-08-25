---
status: proposed
updated: 2026-08-25
authority: design
---
# Automatic exposure, bloom, and ambient occlusion

**Document status:** Proposed. No production code.

**Scope:** Three separate renderer features. They share HDR and deferred inputs,
but each adds graph resources and passes and each must land with its own
evidence.

## 1. Recommendation

Implement automatic exposure first, after the presentation transfer function is
fixed. It changes how every later image-quality result is judged and supplies an
exposure input to temporal reconstruction.

Implement bloom second. Implement GTAO last because it needs an AO-specific,
current-frame view-space depth pyramid and a deliberate indirect-lighting
contract. Do not reuse `hzb_history`.

## 2. Current behavior

`VkrFrameGlobals.exposure` is a finite, non-negative manual multiplier with a
default of `0.30`. Fullscreen tonemapping, editor composition, and the
canonical post-tonemap capture apply it before the ACES-fitted curve.

No automatic exposure, bloom, or screen-space AO ships. Material occlusion is
stored in `gbuffer_albedo.a` and affects indirect diffuse plus the existing
specular-occlusion approximation. Direct analytic lighting is not occluded by
that term.

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
time step, initial exposure, reset point, and frame count. "Warm up until it
looks converged" is not deterministic evidence.

Expose debug outputs for the histogram, clipped percentile range, target EV,
adapted EV, and reset reason.

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
| E0 | Versioned manual/automatic exposure contract | Manual mode is byte-identical |
| E1 | Histogram, percentile resolve, debug outputs | Synthetic luminance patterns produce exact expected bins and EV |
| E2 | Completion-safe adaptation history | Fixed-step case is deterministic across repetitions and resets |
| E3 | Tonemap and temporal-consumer exposure input | Fullscreen/editor/capture semantics agree |
| B0 | Bloom resource and repeat topology | Odd extents and reverse upsample dependencies compile with correct barriers |
| B1 | Threshold, downsample, upsample, combine, debug captures | No seams, firefly propagation, or read/write overlap |
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

## 8. Primary references

- [Intel XeGTAO integration and depth-pyramid notes](https://github.com/GameTechDev/XeGTAO)
- [Jimenez et al., Practical real-time strategies for accurate indirect occlusion](https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion)
- [AMD temporal super-resolution exposure and mask contract](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
