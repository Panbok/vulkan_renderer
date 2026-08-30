---
status: implemented
updated: 2026-08-30
authority: design
---

# Transmission shading correctness spec

Implemented correction for the transmissive-surface shading defects visible as
opaque or white glass in Bistro. The four-layer ordered peel remains the
production bound; T0 through T5 now ship, including the capture-only fifth-peel
diagnostic used to retain that bound.

Prior state and rationale:
[ADR-018](../architecture/adr/018-graph-declared-transmission-feedback.md),
[ADR-028](../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md), and
[deferred-visibility-buffer/SPEC.md](deferred-visibility-buffer/SPEC.md) §7 and
§11. The normative material model is Khronos
[`KHR_materials_transmission`](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_transmission/README.md).

## Decision summary

Keep the accepted ordered peel. Weighted or depth-weighted OIT still cannot
represent a per-layer refracted background sample, so this work does not reopen
ADR-028.

Correct the surface equation as one cross-backend change. Transmission replaces
the diffuse base layer. It does not fade specular reflection or emissive output.
The implementation must preserve RGB Fresnel, apply the resolved transmission
texture, tint transmitted radiance by base color, and keep metallic materials
non-transmitting.

Treat exit-point reprojection and roughness blur as later volume and frosted-
glass work. They are not credible causes of the current Bistro report because
the relevant Bistro materials author zero thickness and zero roughness.

Keep four layers until a focused fifth-layer diagnostic proves that the bound,
rather than the surface equation, causes an unacceptable image. Changing the
accepted bound requires an ADR-028 amendment and matched Release timing.

## Implementation result

- T0 adds the analytic and four Bistro cases, direct lobe replays, linear
  pre/post-transmission captures, and distinct transmission overflow,
  resolve-invalid, and per-layer coverage metrics.
- T1 uses one portable composition contract on Metal and Vulkan:
  `R + (1 - T)D + T(1 - M)(1 - F_rgb)B_tinted + E`.
- T2 publishes and samples transmission and thickness textures through
  `32/16`-byte Metal/Vulkan extensions at the same slot index as the restored
  `176/144`-byte base rows. Both segments share one allocation, publication,
  replacement, and completion-gated retirement operation.
- T3 reprojects a refracted exit point using resolved thickness, instance scale,
  a shared safe-`w` projection helper, each backend's viewport convention, and
  refracted Beer path length. Zero thickness samples the original pixel.
- T4 retains ordered full-resolution composition and samples a graph-declared,
  immutable six-level opaque-color pyramid for positive roughness. Roughness
  zero samples the ordered chain at LOD 0; any positive resolved material/ORM
  roughness selects the opaque pyramid at a continuous, IOR-adjusted LOD. The
  separate `0.04` BRDF floor does not affect this source choice. The rejected
  exact per-layer pyramid spike added 18 passes and `0.905292 ms` mean frame
  wall (`+27.45%`) in the matched local fixture.
- T5 adds an explicit cold-case fifth peel and coverage channel. The layered
  fixture measured 70,118 fifth-surface pixels; the four Bistro cameras measured
  30,766, 64,269, 83,849, and 40,425. Corrected four-layer images retain the
  scene background without a bound-specific unacceptable artifact, so ADR-028
  is unchanged and ordinary frames instantiate no diagnostic resources or
  passes.
- The owner-reported Bistro front-door camera exposed two post-T5 production
  defects. `LMBR_000009e_Glass` combined positive transmission with omitted
  metallic/roughness factors, which glTF otherwise defaults to `1/1`; the
  importer now lowers that common glass form to dielectric/smooth only when the
  factors and metallic-roughness texture are absent, while explicit `1/1`
  remains `1/1`. Metal's transmission ICB also had buffer inheritance disabled,
  so the fragment peel root bound on the parent encoder never reached indirect
  draws. Transmission now uses a dedicated inherited-buffer ICB and production
  encode kernel; ordinary opaque and shadow ICBs retain their existing contract.
- Transmissive materials route exclusively through the deferred peel stream.
  The unreachable forward-shader transmission fallback and its frame flag are
  removed so there is one production surface equation.
- The exact 1600x1200 owner camera now preserves menu text and lighting through
  both panes. Layer coverage decreases `63,695 -> 2,033 -> 771 -> 652 -> 164`,
  every layer has a distinct visibility hash, and overflow/resolve-invalid
  counts remain zero. The two 80x120 pane regions have luma ranges `49..210`
  and `42..212` and normalized RGB entropy of at least `0.697`, replacing the
  nearly uniform pale fill with a bounded spatial-variation witness.

The final five-run Metal profiles measured `3.161408 ms` mean frame wall for
the frosted fixture, `0.639933 ms` summed transmission shading, and `0.050092
ms` for all five pyramid kernels. A focused serial Metal API/GPU validation run
passed with report
`sha256:1f8bdaa03463cb42d568e138114f506449d6675373bf76fef867e8fcee30fe40`.
The correctness implementation is complete. Against the single-observation T0
analytic checkpoint, the repeated T2 result raised frame-wall mean by `6.91%`
and summed production transmission shading by `15.94%`; the final T0-T5 result
was `26.30%` and `20.30%` higher respectively. The baseline is too narrow for a
speed claim, but both measured deltas exceeded the original provisional `5%`
limit. That miss drove a measured redesign which restores the base-row stride
and partitions compact pixels into thin factor-only and extended ranges without
adding a graph pass or dispatch. The thin compile-time path does not read the
extension, sample optional textures, or evaluate thickness/attenuation work.
Two final post-prune five-run profiles measured `0.273592-0.279767 ms` compact
and `0.808542-0.809458 ms` shade GPU means, with `4.003775-4.269375 ms`
frame-wall means. The original limit remains a historical redesign target; the
owner-approved current acceptance ceiling is defined below.

The follow-on production shader family removes cold diagnostic and per-layer
temporal decisions from the ordinary path. Metal and Vulkan now precreate a
diagnostic shade pipeline, a default-production temporal pipeline for layer 0,
and a default-production non-temporal pipeline for layers 1 through 3, for both
the compact and fullscreen launch shapes. Diagnostic replays retain the general
pipeline. The production variants compile with render-mode and shadow-debug
logic disabled; the non-temporal variants also compile without temporal motion,
validity, or transform work. Host selection uses normalized frame state and
layer only; it creates no pipeline and performs no cache lookup in the pass.

A historical five-run M1 Pro Release profile reported exactly 65,372 pixels in
every layer. That invariant is now known to be a Metal peel failure: the ICB
did not inherit the previous-depth root, so all four passes replayed layer 0.
Its `0.742233 ms` shader-family result and the later `0.711450/0.714017 ms`
results remain useful optimization history, but they are not correctness-valid
performance gates. The 464-byte roots remain shared: specialization removes
unused shader loads, while splitting the uploaded roots would save only 528
CPU-upload bytes per frame and has no measured GPU mechanism. A root split is
therefore not retained without evidence that it changes generated code or
timing.

The retained production family now also specializes the lighting lobe at the
resolved pixel value. When `T == 1`, both production shaders evaluate the
specular-only direct, punctual, and environment paths and omit diffuse BRDF, SH,
and ambient work that the composition equation multiplies by zero. Diagnostic
shaders always retain the full lobe path, and every `T < 1` pixel is unchanged.
This adds no compact stream, pass, dispatch, root field, material field, or
lifetime boundary.

After the peel correction, two matched five-run M1 Pro Release observations
measure summed shade means of `0.397800 ms` and `0.396558 ms`. Their paired
compact means are `0.274700/0.268167 ms`; frame-wall mean/p50/p95 is
`3.686141/3.640916/3.879791 ms` and `3.757358/3.765624/4.031708 ms`.
Every repetition retains seven draws, exact layer coverage
`65,372/65,371/0/0`, valid timestamps, and zero overflow or resolve-invalid
counts. The lower shade cost is a consequence of correct work elimination in
empty deeper peels, not a portable shader-speed claim.

### Current performance ceiling

The corrected current local summed transmission-shade GPU-mean ceiling is
`0.400 ms`. The gate is deliberately scoped to the Apple M1 Pro Metal result
that established it:

- Release configuration with Metal API and shader validation disabled;
- `tools/cases/smoke/transmission_analytic_snapshot.case.json` under
  `tools/profiles/local-offscreen-gpu-repeated.json`;
- five independent repetitions with valid GPU timestamps;
- seven draws and exact `65,372/65,371/0/0` compact pixels in layers 0..3; and
- zero overflow and resolve-invalid observations.

Both corrected five-run results pass: `0.397800 ms` and `0.396558 ms`. The
higher observation leaves `0.002200 ms` (`0.55%`) of margin. A matched result
above `0.400 ms`, changed work volume, or invalid diagnostics does not pass.
The former `0.715 ms` ceiling is superseded because it measured four copies of
the front layer; `0.691556 ms` remains only the earlier provisional redesign
target. Compact and frame-wall time remain observations rather than ceilings,
and this local Metal gate makes no Vulkan or other-device performance claim.

The retained shader passes the compact analytic snapshot
`sha256:299401b8a4a3c9733eaf3d2d2d8fe3691d2d8c79a907a86dafed14b99efe47e8`,
fullscreen rollback
`sha256:b6897500bce9ee0c95560f24600f4d8a9db15946caecba2ef8f1fc386e9f14a4`,
diagnostic state matrix
`sha256:6f1a1a87e141192e60c6806c52a4c128430244ab19e996d476768d6b9dfee385`,
and isolated-warm cache profile
`sha256:cf6800d452104eb827cadf04b7f0ecf38630815f81be3e8596e58f8701233b41`.
A strictly serial post-correction Metal API/GPU shader-validation snapshot is
clean apart from the two expected validation-enabled notices
(`sha256:f347ae9e7ecf84b2915fac729959f4449cb7974c45c634eb97933545408a2210`).

The follow-up native Vulkan pass found one host/Slang ABI defect before pipeline
creation: adding the transmission-material address and opaque-pyramid scalars
left an eight-byte shader padding field in front of a host-aligned `Mat4`. The
host inserted another eight bytes implicitly, so SPIR-V placed
`view_projection` at byte 104 while the host expected byte 112. The corrected
464-byte root makes all 16 padding bytes explicit on both sides. Startup
reflection now covers all six shade variants, all three compact variants, the
material extension, and the 32-byte coverage root.

On the RX 6700 XT with driver 26.6.3, the full CPU suite plus fresh Debug and
Release shader builds pass. Normal Release cold and warm launches both exit 0
against one explicit 603,572-byte pipeline cache. The analytic Release snapshot
passes all five replays with seven draws, exact `65,372/65,045/0/0` production
layer coverage, zero overflow or resolve-invalid counts, and report
`sha256:32eb77a2c0e53d151fafaa55ecaa0ad15e89cf1dda934db1d438f5c47e6549df`.
The Debug state-matrix synchronization-validation profile passes two serial
repetitions and all 18 assertions with layer minima
`4,486/2,664/746/721`, no VUID, validation error, or synchronization hazard,
and report
`sha256:393dee3d19cab3544df1d149ec37e12b329064786d7722bad625bd1704f64ee7`.
The source and native Vulkan gates now pass. Crossed image comparison remains
**UNALIGNED** because no accepted Metal generation covers this exact snapshot.

## Pre-implementation audit (historical)

The remaining defect descriptions and staged requirements record the audited
baseline that this implementation replaced. They are retained to explain the
tests and decisions; they are not current implementation status.

## Correction to prior documentation at the audited baseline

[shadow-transmission-transparency-improvements.md](shadow-transmission-transparency-improvements.md)
§2.1 says the shipped shader follows a reference formulation with exit-point
reprojection, roughness LOD, Beer-Lambert attenuation, and a thin-surface
fallback. The audited source supported only one item in that list.

| Prior claim | Audited baseline implementation |
|---|---|
| Exit-point reprojection | Absent. Both backends apply `refracted.xy / max(abs(refracted.z), 0.25) * thickness * 0.02` as a screen-UV offset. |
| Roughness LOD | Absent. Vulkan samples LOD 0. Metal uses a sampler without mip filtering. The pre-transmission and feedback images have one mip. |
| Thin-surface fallback | Absent. No minimum-opacity veil exists. Khronos does not require one, so its absence is not a conformance defect. |
| Beer-Lambert attenuation | Present in both backends when thickness is positive and attenuation distance exceeds `1e-4`. |

That earlier document remains `partial` because it also records implemented
work. Its claim of shipped transmission parity is superseded by this source
audit.

## Audited baseline source contract

`main()` in `app/src/main.c` selects Metal by default outside Windows and Vulkan
on Windows. The two production shading functions are
`vkr_metal_packet_transmission_shade()` in
`lib/src/renderer/shaders/metal/msl/world/gpu_draws.metal` and
`vk_transmission_shade()` in
`lib/src/renderer/shaders/vulkan/slang/world/deferred.slang`.

`vkr_material_system_material_is_transmissive()` classifies a PBR material when
its constant transmission factor is positive or its transmission texture has
resolved. `vkr_packet_derive_material_constants()` packs the constant factor,
IOR, and constant thickness into `material_alpha`.

The deferred material rows publish only base-color, normal, ORM, and emissive
textures. Neither backend publishes or samples the per-material transmission or
thickness texture. This makes classification and shading disagree for a
texture-driven material and drops the texture modulation for a material that
also has a positive factor.

### Current equations

For this section:

- `D` is the lit diffuse term after the existing metallic and Fresnel rules.
- `R` is direct and environment specular reflection.
- `E` is emissive radiance.
- `B` is the sampled and volume-attenuated feedback radiance.
- `T`, `M`, and `F` are transmission, metallic, and RGB view Fresnel.

Metal computes a scalar weight and mixes the whole lit surface:

```text
w = T * (1 - M) * (1 - max(F.r, F.g, F.b))
C = (1 - w) * (D + R + E) + w * B
```

Vulkan uses the same scalar weight but replaces the lit surface with raw base
color:

```text
C = (1 - w) * base_color + w * B + E
```

A screen-space approximation of the Khronos lobe partition must instead keep
the terms separate:

```text
C = R + (1 - T) * D + T * (1 - M) * (1 - F) * B_tinted + E
```

`B_tinted` includes base-color tint and any volume attenuation. The exact
helper layout may differ, but these boundary cases are requirements:

- `T = 0` reproduces the ordinary opaque material equation.
- `T = 1, M = 0` removes diffuse reflection while preserving `R` and `E`.
- `M = 1` contributes no transmitted radiance.
- RGB `F` remains RGB. No maximum-channel scalar may replace it.
- Grazing Fresnel moves energy to `R`; it does not reveal diffuse or raw albedo.

### Bistro material facts

Eighteen Bistro materials author a positive transmission factor. Their factors
range from `0.66` to `0.95`; all use opaque alpha, zero thickness, and zero
attenuation distance. The owner-reported front-door panes are menu-sign mesh
236 with material 98, `LMBR_000009e_Glass`, not the building-glass rows 53/54.
Its source authors `transmissionFactor=0.9` but omits the metallic-roughness
block. Literal glTF defaults therefore produced metallic 1 and roughness 1,
which suppressed the transmitted lobe and selected the coarsest feedback mip.
The compatibility lowering described above treats this omitted-factor glass
form as metallic 0 and roughness 0; explicitly authored factors retain their
values. Fifteen positive-transmission base colors are white, two are pale blue,
and one is warm tinted.

The on-screen HUD does not print `transmission_draws`. Subtracting its opaque
and transparent counts from total world draws can suggest the transmission
count, but T0 must record the direct `draw.world.transmission_draws` and
`visibility.transmission.*` metrics instead.

## Source-proven defects and limits

### D1: transmission and thickness textures do not reach shading

The loader retains both texture slots, and classification can route a material
because its transmission texture resolved. The four-texture GPU material rows,
surface reconstruction, and transmission shaders do not carry either texture.
The shader then gates transmission on the constant factor alone.

The required values are the glTF products:

```text
T = transmission_factor * transmission_texture.r
thickness = thickness_factor * thickness_texture.g
```

An absent texture contributes one. A zero transmission factor contributes zero
regardless of texture contents and must not route a draw into transmission.

### D2: Metal mixes the wrong terms

Metal first computes the opaque result and then mixes all of it toward feedback.
This leaves too much diffuse at high transmission while also fading specular
reflection and emissive output. For `T=0.95`, `M=0`, and normal-incidence
`F=0.04`, its residual multiplier on the whole surface is `0.088`, not the
draft's former claim of an unattenuated diffuse lobe. The correct diffuse
multiplier is `0.05`; specular and emissive must not receive either multiplier.

This is a real equation defect, but source inspection alone does not establish
that it is the dominant cause of the Bistro symptom.

### D3: Vulkan substitutes reflectance for radiance

Vulkan's default transmission branch outputs raw `surface.base.rgb` for the
non-transmitted share. Base color is a reflectance ratio, not scene radiance.
The result ignores direct lighting, shadows, and IBL, and it cannot match Metal.

The same functions disagree on `N dot V`: Vulkan takes the absolute value while
Metal clamps the oriented dot product. T1 must use one shared convention and
cover front and back faces in the fixture. It must not select either expression
without that evidence.

### D4: Fresnel is collapsed to one channel and transmitted tint is absent

Both backends use `max(F)` as a scalar transmission mask. The renderer retains
colored dielectric F0, so this discards an authored RGB response. Both also
sample feedback without the base-color tint required by the transmission base
layer. This is visible on two pale-blue and one warm Bistro material even though
most Bistro glass is white.

### D5: volume refraction is incomplete

Both backends use a hardcoded screen-space coefficient instead of tracing the
refracted thickness vector to an exit point and reprojecting it. They also use
the constant thickness only. T3 must include thickness texture and instance
scale, then apply the backend's established viewport and projection convention.

Zero thickness means a thin surface with no average exit-point displacement.
The lack of distortion on current Bistro glass is therefore expected, not proof
of this defect's contribution to the white result.

### D6: rough transmission has no valid feedback pyramid

Every image that can feed a shallower peel has one mip. Adding mips only to
`hdr_pre_transmission` and `scene_pre_transmission` is insufficient because
layers 2 through 0 sample the composite produced by deeper layers through
`transmission_feedback`.

An exact ordered solution may need a new feedback pyramid after each shaded
layer. That cost scales with the peel bound. A cheaper solution that always
samples the opaque pyramid changes the accepted inter-layer feedback semantics
and needs an explicit quality decision.

### L1: four surfaces is an accepted bound, not silent draw overflow

One closed transmissive mesh normally consumes two peel surfaces, entry and
exit. The four-layer bound therefore covers two closed meshes along a pixel ray,
as ADR-028 states. Additional surfaces are clipped from the refractive chain;
the nearest four still composite over the opaque background.

Existing per-layer coverage metrics describe layers 0 through 3 but cannot say
whether a fifth surface was present. The existing transmission visibility
capture channels address the first array layer only. T5 needs a focused
exhaustion diagnostic before proposing a larger production bound.

## Staging

Each stage has its own gate. A later stage may be deferred without weakening an
earlier accepted correction.

### T0: fixtures and term isolation

Author the following cameras as harness cases or a deterministic camera script.
They are observations to encode in `tools/cases/`, not camera data to add to
`assets/scenes/bistro.scene.json`.

| Case | Position | Yaw | Pitch | Subject |
|---|---|---|---|---|
| `exterior-door` | `-14.26, 2.29, 10.26` | `-23.42` | `-7.74` | Door pane |
| `exterior-windows` | `-14.76, 2.21, 4.59` | `29.16` | `-6.58` | Windows over a lit interior |
| `interior-glassware` | `-8.16, 1.42, 10.03` | `25.77` | `-2.35` | Curved, multi-surface glassware |
| `bar-glassware` | `-5.03, 2.24, 9.73` | `-20.34` | `-7.77` | Dense overlap near the peel bound |

Reuse `assets/scenes/fixtures/layered_glass.scene.json` for controlled layered
coverage. Add a small analytic fixture with constant background, white and
colored dielectric materials, `T` values of 0, 0.5, and 1, metallic 0 and 1,
front and back faces, an emissive sample, and texture-only transmission and
thickness samples.

The gate records:

- final color, `unlit`, `lighting`, transmission visibility IDs and primitives;
- direct transmission draw, visible, compact-overflow, and per-layer coverage
  metrics, plus a transmission resolve-invalid metric added at T0 because the
  current published resolve-invalid metric covers opaque G-buffer work only;
- diffuse-only and specular-only replays, added as named harness channels for
  render modes 4 and 5;
- a linear pre-transmission source and post-transmission composite capture for
  the analytic fixture;
- matched Release per-pass timing before any shader, row, or graph change.

Run each backend on a native supported profile. Cross-backend diffs are
diagnostic until a threshold is chosen before capture and encoded in the case.
Integer visibility channels compare exactly. The repository has no generic
"backend parity" tolerance to inherit.

T0 closes only when the report identifies which term dominates the four Bistro
pixels. A source-level defect may proceed independently, but it must not be
reported as the symptom's measured cause without this evidence.

### T1: lobe partition and backend parity

Implement the target equation in both production shaders as one vertical slice.
Remove Vulkan's unlit-albedo branch. Compute diffuse, specular reflection,
transmitted feedback, and emissive separately, then combine them once. Preserve
RGB Fresnel and base-color tint. Reconcile the oriented `N dot V` convention.

Do not implement this as the draft's former diffuse-only stage. Vulkan has no
lit diffuse value in its current transmission branch, so diffuse suppression
and backend parity are coupled.

Gate:

- the analytic fixture satisfies every boundary case under the specified
  equation;
- `T=0` matches the ordinary opaque equation for the same material and lights;
- the four Bistro cases retain visible background where coverage exists and no
  channel contains non-finite values;
- focused Metal and Vulkan validation pass separately;
- matched Release transmission pass and frame timings remain within the budget
  chosen before the change. Report the delta even when no speed claim is made.

### T2: material texture completion

Publish and sample transmission and thickness textures on both backends. Align
classification with `factor * texture`: factor zero stays out of the
transmission list, while a positive factor with a texture uses the texture's
red channel. Thickness uses the green channel.

Choose the GPU representation before editing. Appending texture and sampler
handles to the common 176-byte Metal and 144-byte Vulkan material rows is the
smallest interface change, but it changes row stride for opaque shading. A
transmission-only table avoids that stride change but adds publication and
retirement state. Either choice must keep immutable publication and
submit-completion retirement intact; no per-pixel registry lookup is allowed.

Gate:

- loader, classification, publication, replacement, and retirement tests cover
  factor-only, texture-only, factor-times-texture, and missing-texture cases;
- the analytic texture fixture matches expected `T` and thickness values on
  both backends;
- ABI asserts and production shader reflection pass;
- focused cold/warm cache and native validation pass;
- if the common material row changes, matched Release opaque and transmission
  timing covers both Bistro and a material-diverse fixture before acceptance.

### T3: exit-point reprojection

Replace the `0.02` offset. Build the refracted thickness vector from IOR,
resolved thickness, and instance scale, add it to world position, and project
the exit point through the current backend view-projection convention. Clamp or
reject only at the established sampling boundary. Do not add per-pixel recovery
or logging.

Use the dedicated volume fixture. Do not change Bistro material authoring to
make this stage visible.

Gate:

- zero thickness samples the same pixel;
- positive thickness produces a displacement with the expected direction as
  view angle, IOR, thickness texture, and non-uniform instance scale vary;
- Metal and Vulkan preserve their established viewport-Y convention;
- Beer-Lambert attenuation uses the refracted path length;
- native validation and matched Release timing pass for the focused volume and
  Bistro cases.

### T4: roughness-driven feedback sampling

Start with a graph and cost spike. Measure two explicit choices:

1. regenerate the feedback mip chain after each deeper-layer composite;
2. sample one immutable opaque-color pyramid for every layer and retain the
   ordered full-resolution chain only for composition.

Choice 1 preserves the current per-layer feedback meaning and may be too
expensive. Choice 2 is cheaper but changes what rough refraction sees. Record an
owner quality decision before implementation. Do not silently ship choice 2 as
equivalent.

The chosen graph declares every mip level, generation pass, access, and
transition for fullscreen and editor branches. Map IOR-adjusted roughness to a
bounded LOD without pipeline creation or descriptor allocation during a frame.

Gate: a frosted-glass fixture shows stable blur at several roughness and IOR
values, ordered-layer captures match the accepted choice, native validation
passes, and matched Release results report every added mip pass plus frame wall.
Stop if the accepted budget is missed.

### T5: peel-bound evidence and decision

Add a diagnostic fifth peel or equivalent post-layer-3 coverage count in a
focused case. It must be graph-declared and disabled outside the diagnostic.
Record fifth-surface coverage at the layered fixture and all four Bistro
cameras.

If the coverage is material and the image is unacceptable, compare candidate
bounds with the same Release configuration. Report graph pass count, shaded
pixels per layer, per-pass GPU time, frame wall, graph image bytes, and every
overflow counter. Any production bound change amends ADR-028. If evidence keeps
four, retain the current clipping rule and document the result without a code
change.

## Out of scope

- OIT accumulation for transmission or ordinary alpha blend.
- A minimum-opacity thin-surface veil. It is a renderer policy, not a Khronos
  conformance requirement.
- Per-draw feedback refresh. The accepted graph composites one surface per
  pixel per peel layer.
- Broad Bistro material retuning. The focused omitted-factor compatibility rule
  for positive-transmission glass is part of the owner-reported correction.
- Changes to TAA, GTAO, exposure, or ordinary blend.

## Owner decisions

1. Numeric captures use maximum delta `2/255`, mean absolute error `0.1/255`,
   and failed-pixel ratio `0.001`; integer visibility remains exact. The local
   Release regression budget was 5% for matched frame wall and summed
   transmission-pass GPU time.
2. T2 uses one same-slot extension segment rather than expanding the common
   immutable row. The split adds no publication or lifetime domain. A two-range
   compact partition removes extension, optional-texture, and volume work from
   the thin factor-only shader path without adding a graph pass or dispatch.
3. T4 accepts the immutable opaque pyramid. The exact ordered candidate missed
   the frame budget and visibly accumulated deeper-layer tint into shallower
   rough blur.
4. T5 retains four production layers. The fifth-peel diagnostic remains
   graph-declared and case-only; ADR-028 needs no amendment.
5. Default shading uses precreated production temporal/non-temporal shader
   variants. Diagnostic capture state selects the general shader. Keep the
   proven common roots until a focused generated-code or timing comparison
   demonstrates a benefit from splitting them.
6. The former `0.715 ms` ceiling is invalid because Metal replayed the front
   surface into every peel. The corrected M1 Pro Metal ceiling is `0.400 ms`
   summed transmission-shade GPU mean under the exact five-run analytic profile
   and `65,372/65,371/0/0` layer work-volume constraint. No frame-wall, Vulkan,
   or portable-device ceiling follows from this decision.
7. A positive-transmission glTF material with omitted metallic/roughness
   factors and no metallic-roughness texture imports as dielectric/smooth.
   Explicit factors always win. Metal transmission draws use an inherited-
   buffer ICB so the parent-bound draw and peel roots reach every indirect draw.
