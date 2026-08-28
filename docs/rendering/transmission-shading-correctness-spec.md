---
status: proposed
updated: 2026-08-28
authority: design
---

# Transmission shading correctness spec

Implementation plan for the transmissive-surface shading defects visible as
opaque or white glass in Bistro. The existing four-layer peel, graph-owned
feedback chain, and transmission draw classification ship today. The corrective
work in T0 through T5 does not.

This source audit proves several material-input and shading-equation defects. It
does not prove which one dominates the reported pixels. T0 must isolate the
terms before an implementation claims a root cause or a fix.

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

## Correction to prior documentation

[shadow-transmission-transparency-improvements.md](shadow-transmission-transparency-improvements.md)
§2.1 says the shipped shader follows a reference formulation with exit-point
reprojection, roughness LOD, Beer-Lambert attenuation, and a thin-surface
fallback. Current code supports only one item in that list.

| Prior claim | Current implementation |
|---|---|
| Exit-point reprojection | Absent. Both backends apply `refracted.xy / max(abs(refracted.z), 0.25) * thickness * 0.02` as a screen-UV offset. |
| Roughness LOD | Absent. Vulkan samples LOD 0. Metal uses a sampler without mip filtering. The pre-transmission and feedback images have one mip. |
| Thin-surface fallback | Absent. No minimum-opacity veil exists. Khronos does not require one, so its absence is not a conformance defect. |
| Beer-Lambert attenuation | Present in both backends when thickness is positive and attenuation distance exceeds `1e-4`. |

That earlier document remains `partial` because it also records implemented
work. Its claim of shipped transmission parity is superseded by this source
audit.

## Current source contract

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

Eighteen of 254 files under `assets/materials/bistro/` author a positive
transmission factor. Their factors range from `0.66` to `0.95`. All 18 use
`alpha_mode=opaque`, zero thickness, and zero attenuation distance. Seventeen
are non-metallic with authored roughness zero. One material authors
`metallic=1`, `roughness=1`, and `transmission_factor=0.9`; Khronos correctly
ignores transmission for that row. Fifteen base colors are white, two are pale
blue, and one is warm tinted.

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
- Bistro material authoring changes. T2 through T4 use dedicated fixtures.
- Changes to TAA, GTAO, exposure, or ordinary blend.

## Owner decisions

1. Before T1, set numeric HDR comparison thresholds and a Release regression
   budget for the analytic and Bistro cases.
2. At T2, choose common-row expansion or a transmission-only material table
   from measured opaque-path cost and lifetime complexity.
3. At T4, choose exact per-layer pyramids, the documented opaque-pyramid
   approximation, or defer frosted transmission.
4. At T5, amend ADR-028 only if fifth-layer evidence justifies a larger bound.
