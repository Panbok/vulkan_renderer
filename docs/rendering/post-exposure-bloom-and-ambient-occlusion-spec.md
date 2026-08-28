---
status: partial
updated: 2026-08-28
authority: design
---
# Automatic exposure, bloom, and ambient occlusion

**Document status:** Implemented through automatic-exposure phases E0-E3, bloom
phases B0-B1, and GTAO phases G0-G2 on Metal and Vulkan. Metal runtime,
capture, timing, and validation evidence passes. Windows Vulkan Release smoke,
Bistro, cold/warm deferred, and focused Debug validation evidence now passes;
matched authoritative performance and final-color owner acceptance remain
open. The architecture specification remains the status authority.

**Windows Vulkan follow-up (2026-08-28).** The apparent black-ground GTAO
regression was a two-channel normal-map decode defect outside E, B, and G. One
shared shader
decoder now reconstructs positive tangent-space Z for Metal and Vulkan. The
harness also waits for renderer asset publications, so its smoke fixtures are
no longer empty on Windows. A later matched Metal run proves static exposure is
stable on both backends but leaves absolute pre-bloom HDR parity open. Exposure
adaptation now obeys its authored EV-per-second limits. See §7.4 and
[Windows Vulkan post-effect parity investigation](windows-vulkan-post-effect-parity-investigation.md).

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

GTAO ships as a packet-controlled, full-resolution ambient-visibility path on
Metal and Vulkan. Production initialization enables it; deterministic harness
cases default it off unless they explicitly author enable, radius, and power.
Its dedicated current-frame view-depth pyramid, raw visibility, edge data, and
denoised visibility are graph resources. Deferred lighting multiplies only
indirect diffuse by the final GTAO visibility. Material occlusion continues to
own the existing specular-occlusion approximation, and direct analytic lighting
is unchanged.

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

The adaptation equation uses a bounded frame delta and separate brighten and
darken rates. Each rate is a maximum EV change per second:

`adapted = previous + clamp(target - previous, -darken_rate * dt, brighten_rate * dt)`

This is a constant-rate bound, not an exponential response coefficient. Define
which direction each rate applies to. A skipped frame must not publish state
that the GPU never wrote.

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
for an empty histogram, advances valid history by at most the selected rate
times the bounded frame delta, and snaps an invalid history chain directly to
the measured target. The former exponential interpolation treated those values
as response coefficients and could move far more than 3 EV/s after a large
luminance change; it violated the public unit contract and caused the reported
bright pop when moving between dark and bright areas.

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

### 5.5 Implemented GTAO path

Packet version 22 adds `gtao_enabled`, `gtao_radius`, and `gtao_power`. The
frontend validates enabled controls at the cold packet boundary: radius must be
finite and within `[0.0001, 10000]`, and power must be finite and positive. It
derives one `VkrGtaoFrame` and preserves a zeroed packet as GTAO-disabled.
Production initialization enables the XeGTAO-derived defaults, while
`VKR_GTAO_DISABLED=1` is a cold forced bypass. Harness cases remain disabled by
default and must author all three controls when enabling GTAO.

`VkrGtaoConfig` fixes the first quality tier to five view-depth mips, three
horizon slices, three steps per slice, full-resolution evaluation, and one
edge-aware 3x3 denoise. Radius is `0.5`, power is `2.2`, radius multiplier is
`1.457`, falloff range is `0.615`, sample distribution power is `2.0`, depth
mip sampling offset is `3.3`, and denoise blur beta is `1.2`. Shared C and
shader helpers pin exact reconstruction for canonical jittered perspective and
orthographic projections, the depth-aware mip filter, edge packing, spatial
noise, falloff, horizon integration, and final visibility. Positive R16 view
distance clamps to the finite half-float ceiling `65504`.

The authored graph creates full-resolution `gtao_view_depth` as `R16_SFLOAT`
with a full image mip chain and uses at most its first five levels, plus
full-resolution `gtao_raw`, `gtao_edges`, and `gtao_visibility` as `R8_UNORM`.
`AO.PrefilterDepth` converts current normal-Z device depth to positive view
distance. Ascending
`AO.PrefilterDepth.${i}` repeats build each following AO-specific mip without
reading `hzb_history`. `AO.Evaluate` rotates the current G-buffer world normal
into view space and writes raw visibility and packed directional edges.
`AO.Denoise` writes the final single-channel visibility. Odd extents clamp
source coordinates at every mip and dispatch boundary.

Metal and Vulkan cold-create all four mandatory compute pipelines and validate
the same 192-byte parameter ABI. Metal binds graph mip views through GPU
resource IDs; Vulkan binds the corresponding sampled/storage indices and
device-address root. Both deferred-lighting variants receive a conditional
sampled input at binding 7. Disabled frames bind a cold-created 1x1 white Metal
texture or the Vulkan sampled-image sentinel, so the shader contains no feature
branch.
The sample occurs only after diagnostic and background early returns. The
result multiplies `material_ao * gtao_visibility` for indirect diffuse only;
direct punctual/directional terms and the material-only specular-occlusion
approximation remain unchanged.

Direct captures expose `gtao_view_depth`, `gbuffer_normal`, `gtao_raw`, and
`gtao_visibility`; `final_color` remains the post-tonemap result. R16 depth
canonicalizes to `R32_FLOAT_LE`; R8 visibility canonicalizes to grayscale
`RGBA8_UNORM_PNG`, including padded source-row handling. Bent normals,
multi-bounce compensation, temporal accumulation, GI, half-resolution
evaluation, and GTAO-driven specular occlusion remain outside G0-G2.

## 6. Phases

| Phase | Work | Gate |
| --- | --- | --- |
| E0 | Versioned manual/automatic exposure contract | **Implemented.** Manual mode is byte-identical |
| E1 | Histogram, percentile resolve, debug outputs | **Implemented.** CPU reference pins accepted samples, saturated bins, fractional percentile edges, resolved EV, adaptation, and empty-histogram hold; delayed state plus all 256 bins is observable |
| E2 | Completion-safe adaptation history | **Implemented.** Metal and Vulkan use completion-proven graph history, submit-only publication, and reset-chain invalidation; the fixed-step static case repeats exactly |
| E3 | Tonemap and capture integration | **Implemented.** Fullscreen and editor tonemap consume current GPU state, canonical capture records the completed multiplier, and manual output remains unchanged |
| B0 | Bloom resource and repeat topology | **Implemented.** Odd extents, bounded mip counts, reverse upsample ordering, and the read-after-write subresource barrier are pinned by CPU graph tests |
| B1 | Threshold, downsample, upsample, combine, debug captures | **Implemented.** Shared sanitizer/soft-knee references, separate chains, full-resolution combine, four direct HDR/bloom channels, Release execution, and focused Metal validation pass |
| G0 | Current view-space depth pyramid and normal conversion | **Implemented.** Shared reconstruction/mip references, odd-extent graph tests, R16 depth capture, and current G-buffer-normal capture pass; no HZB history is sampled |
| G1 | GTAO evaluation and spatial denoise | **Implemented.** Full-resolution 3-slice × 3-step evaluation and one edge-aware 3x3 denoise pass produce nondegenerate R8 captures; keyframed motion over 0.22-unit-thick geometry retains thin silhouettes |
| G2 | Indirect-diffuse integration | **Implemented.** Both deferred shaders consume a branchless white fallback and multiply only indirect diffuse; direct analytic and material-only specular-occlusion terms are unchanged |

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

### 7.3 GTAO implementation evidence

Local Apple M1 Pro evidence from 2026-08-27 is implementation evidence, not an
accepted baseline or a cross-device performance result:

- the GTAO defaults, normalization, mip derivation, bounded-radius derivation,
  jittered-perspective and orthographic projection round trips, R16 clamp,
  depth filter, noise/192-byte ABI, packet validation, graph repeat/80-pass
  capacity, capture conversion, harness fingerprint/replay, and summary v2-v4
  compatibility tests pass;
- `./build_test.sh` reaches and passes the GTAO, harness, render-graph, and
  Vulkan contract suites, then stops at the pre-existing cooked-mesh tangent
  assertion in `mesh_cooked_tests.c:166`;
- two isolated-cold 801x601 Release snapshots produce identical canonical
  payload digests for `gtao_view_depth`, `gbuffer_normal`, `gtao_raw`,
  `gtao_visibility`, and `final_color`. View depth is finite over
  `7.875-100`; raw visibility spans byte values `153-255` with 103 distinct
  values, and denoised visibility spans `159-255` with 97 distinct values.
  Report digests are
  `sha256:05e70839c580663195038c71cd548e84ccf90dc387e5031c2b990cce7f1ebe8d`
  and
  `sha256:33dc268a27416052697855567af24f3a94bc9559e4218c248485470642633a50`;
- a keyframed Release case captures distinct current depth, normals, raw AO,
  and denoised visibility at both ends of camera motion over 0.22-unit-thick
  geometry. Raw and denoised visibility span `8-255` at both checkpoints, and
  visual inspection retains the thin silhouettes without cross-edge blur.
  Report digest:
  `sha256:cc1301b87b1bcbb898639ff415aeb3d998e201b14734a33578864ab5426de956`;
- one Release Metal timestamp observation supplies 16 valid samples for each
  of the seven instantiated GTAO passes. The observed combined mean is
  `0.260182 ms`; evaluate is `0.144932 ms` mean / `0.395917 ms` p95 and
  denoise is `0.076188 ms` mean / `0.076958 ms` p95. This local dirty-tree
  process proves execution and attribution only. Report digest:
  `sha256:6ee1fdbd9d0b9f889b07fdf250e536e12de9672bfcc7bf49bb8d18a3e15d3e94`;
  and
- one strictly serial Debug hidden-window snapshot with Metal API and GPU shader
  validation enabled completes all five captures. Its log confirms both
  validation modes and contains no other diagnostic. Report digest:
  `sha256:5ddc90bab54a41d4519ded88afb8ea5eda71d548e8b2d34235325da0f0241478`.

The Release isolated-cold snapshots cover mandatory pipeline creation. Two
final isolated-warm attempts complete both startup children, but their
aggregates are rejected for unrelated nondeterministic work-volume rows and
are not counted as passed gates. Native Vulkan validation is recorded in
§7.4. An authoritative matched GTAO-on/off Release comparison and explicit
owner acceptance of any final-color baseline remain open. No cost or quality
ranking is inferred from the local observations.

### 7.4 Windows Vulkan closure

Local RX 6700 XT evidence from 2026-08-28. Windows 10 Pro 19045, driver
26.6.3, Clang 20.1.0, Release and Debug, based on tree `a517999` with this
change. These are local dirty-tree correctness diagnostics, not authoritative
performance or baseline evidence.

The harness readiness gate now includes the selected renderer's asset
publisher. Every smoke case also asserts at least one opaque draw and zero
publication omissions. The final Release reports are:

| Case | Report digest | Decisive evidence |
| --- | --- | --- |
| `smoke.auto_exposure.static` | `sha256:db0a996e7cb720f9065763cddb67fff9720d5f90f01ba6d857119f1031a734c9` | Non-empty histogram; multiplier and EV adaptation execute |
| `smoke.bloom.static` | `sha256:3af307a224ca770abe8a9efd2e81b8d6e1411a4acdbcf780299f606604a4c260` | Prefilter, result, combined HDR, and final color have distinct digests |
| `smoke.gtao.static` | `sha256:eb62020e3e91fc37a3e14b5291edc3af81dcd0faa0aeb8ade0f39296cbadd0d8` | Nonuniform view depth, raw AO, denoised visibility, and final color |
| `smoke.gtao.motion_thin` | `sha256:6c0902aa7155b1f1349388517e818d96bcff177652b7b53e7c4e69c318016ed6` | Both motion checkpoints populate the AO chain |

The exact owner-camera Bistro GTAO report
`sha256:8b0d223cdbc7f25e2f2c1ae5517af5c00727537f31bcaf3e532cace20f96e9e3`
shows lit ground, a positive-hemisphere G-buffer normal field, ordinary contact
occlusion, one or more opaque draws, and zero publication omissions. The full
automatic-exposure, bloom, and GTAO production case also passes its
non-vacuity assertions. That evidence did not prove temporal exposure
stability: its old assertions only required one accepted texel and a positive
multiplier.

The fixed-step production trace exposed a Vulkan descriptor-index mismatch.
`Post.Exposure.Histogram` declared its source as `STORAGE_READ`, the Vulkan
executor supplied a storage-image index, and the shader used that number in
the sampled-image array. The two arrays have independent indices, so history
rotation intermittently selected unrelated sampled images. Metal binds the
texture object directly and was unaffected. Before the fix, the static camera
ranged from 35 to 480,000 accepted texels, target EV -8 to 0.1454, and
multiplier 0.00390625 to 1.1060. Report digest:
`sha256:aeea1298b6baa9fcffe88d57734a75d29d705410c1cdd3de9f1e892091ad0d36`.

The graph now declares the history source as `SAMPLED`, and Vulkan supplies
the matching sampled-image index. The identical two-repetition Release trace
holds exactly 479,910 accepted texels across all 120 measured frames; target EV
varies by less than `4.77e-7` and multiplier by less than `1.20e-7`. Report
digest:
`sha256:7f5d3524b7a02e5c4248e260795074b4b432edac8f501f1a16a2cabe85789f59`.
The main-graph CPU test pins sampled access, and the production case now bounds
accepted texels, target EV, and multiplier over the entire static trace.

Two independent focused Debug synchronization-validation children pass with no
Vulkan diagnostic and with zero deferred resolve errors or indirect overflows;
report digests
`sha256:0c34cf08fec72b3e660989de10c441fe28016719afbe8b43ba0cb86496838d97`
and
`sha256:fc037520645a577fca23378df4df10d95ddbdc0de4d22914176ebe9d455f9303`.
The stock two-child Windows diagnostic remains incomplete because its second
process stops during Vulkan loader startup after third-party Galaxy overlay
layer discovery, before renderer creation. An isolated Debug Bistro profile
with implicit overlay layers disabled passes one complete repetition with zero
Vulkan validation messages; report digest
`sha256:b0cf4ee52a50ebbd5dc8114f7d662d4042a574863a32db63d273d40c116b5bf1`.
Full causation, fixes, and remaining parity work are in
[Windows Vulkan post-effect parity investigation](windows-vulkan-post-effect-parity-investigation.md).

Owner review after that closure found an unresolved absolute-exposure mismatch
at darker Bistro cameras. A native M1 Pro run of
`local.mac.bistro.exposure_parity`, report digest
`sha256:d9b3ed168b50250d9600aab4e88772c89c3a3b24895c936ac6ca29c1aecdd5f6`,
finally supplies the exact-camera witness. Across 60 static samples Metal holds
average log luminance `-3.309451`, target/adapted EV `+0.835520`, and multiplier
`1.784500`. The matched RX 6700 XT run after the adaptation correction, report
digest `sha256:e32f6f67c453133c503e4fb2c2276a8d0a77028c7c166926719c915c4ef242f7`,
holds `-4.139783`, `+1.665852`, and `3.173010`. Both kernels are stable; Vulkan's
pre-bloom input is about `0.830 EV` darker, so the shared resolve correctly adds
about `0.830 EV` more exposure. Bloom is downstream of metering and remains
excluded as the source.

The comparison also removed a Vulkan-only diffuse-IBL term that multiplied
environment diffuse by `1 + directional_visibility * directional_intensity`.
Metal has no such term, and tying environment light to the camera-visible sun
shadow made surface brightness view dependent. Removing it changes this static
camera by only about `0.003 EV`, so it restores structural lighting parity but
does not explain the absolute HDR gap.

The remaining split requires matched GTAO-off Metal and Vulkan runs. The Vulkan
control resolves `-3.450421` average log luminance and `+0.976490 EV`; without a
Metal GTAO-off control, that number cannot distinguish an AO-output difference
from an upstream lighting difference. The symmetric
`local.mac.bistro.exposure_parity.gtao_off` and
`local.win.bistro.mac_reference.gtao_off` cases now provide that gate. The
GTAO-on pair also captures view depth, raw/final visibility, and exact G-buffer
normals. No backend-only exposure compensation or shared metering retune is
justified until the matched split is complete.

## 8. Primary references

- [Intel XeGTAO integration and depth-pyramid notes](https://github.com/GameTechDev/XeGTAO)
- [Jimenez et al., Practical real-time strategies for accurate indirect occlusion](https://research.activision.com/publications/archives/atvi-tr-16-01practical-realtime-strategies-for-accurate-indirect-occlusion)
- [AMD temporal super-resolution exposure and mask contract](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
