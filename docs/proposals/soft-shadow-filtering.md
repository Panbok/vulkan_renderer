---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Soft shadow filtering: far-cascade EVSM

## Current baseline

Directional shadows already harden at contact. The nearest two cascades run an
eight-sample PCSS blocker search and at most sixteen filter samples, sized by
the authored sun angular diameter (default 0.53 degrees). Farther cascades use
rotated Poisson comparison PCF with 16 taps on High and 9 on Balanced
([ADR-041](../adr/041-retained-cascaded-shadows.md)). The shared helpers are in
[`shadow_kernel.slangh`](../../renderer/src/shaders/shared/shadow_kernel.slangh).
The **Soft shadows** setting sets the sun diameter to zero, which leaves PCF only.

Point and spot shadows use Poisson PCF in `Shadow.LocalMask`, and the
full-filter lights harden at contact from an authored source radius
([ADR-019](../adr/019-bounded-forward-spatial-lighting.md)). That part of this
proposal has shipped; this document keeps the remaining far-cascade EVSM work.

Storage today:

| Map | Format | Size | Memory |
|---|---|---|---|
| Cascades (High) | `D32_SFLOAT` | 4 × 2048² per target image | 64 MiB per target image |
| Local atlas | D16 | 4096² per layer, at most 32 layers | 32 MiB per layer |

Measured shadow cost. All rows are local, non-authoritative runs from
[lighting efficiency](lighting-efficiency.md),
[meshlet cluster culling](meshlet-cluster-culling.md) and
[automatic mesh LOD](mesh-lod.md), each with its configuration recorded there:

| Work | M1 Pro, Metal, 1280x720 | RX 6700 XT, Vulkan, 1080p |
|---|---:|---:|
| `Shadow.LocalMask` | 5.77 ms (4.21 ms with TAA) | 2.82 ms |
| Local filter taps for the three full-filter lights | 2.63 ms (nine taps) | 1.10 ms (four taps) |
| Directional sampling in `Lighting.Deferred` | 0.59 ms with cloud shadow | 0.53 ms, PCSS blocker search 0.08 ms |
| `Shadow.Cascade.*` raster, averaged over all frames | 0.46 ms | not measured |
| One cascade refresh | 1.4–2.2 ms | not measured |

Local shadows dominate. Directional sampling and cascade raster together cost
about 1 ms per frame on M1 Pro.

## Goals and non-goals

- Evaluate exponential variance shadow maps (EVSM) as a filterable
  representation for distant cascades. EVSM is a quality change. The measured
  sampling cost it could replace is below 0.6 ms per frame.
- Keep retained cascades, cached local faces, transmission layers, contact
  shadows and both backends' shared semantics unchanged.
- Non-goal: EVSM for the local atlas. See [Declined scope](#declined-scope).
- Non-goal: ray-traced or denoised soft shadows.

## EVSM

### What EVSM changes

EVSM stores the warped depths `exp(c * z)` and `-exp(-c * z)` and their
squares. A separable blur and mips make that representation filterable. One
trilinear or anisotropic fetch then replaces a PCF kernel, and grazing-angle
aliasing on distant cascades becomes mip-filtered softness. Under ADR-041 the
blur runs only when a cascade redraws. The cost therefore moves from every
pixel to each refreshed texel.

EVSM has known costs:

- Light bleeding where casters overlap at different depths. Bistro's awnings,
  balconies and alpha-tested foliage are such cases.
- An exponent limit set by storage precision. With 32-bit storage the positive
  exponent is about 42 and the negative about 5. 16-bit storage limits it to
  about 5.5 and bleeds heavily.
- A conversion and blur pass for every redrawn cascade. The raster still
  writes D32 depth.
- Contact-hardening on EVSM needs a blocker estimate from moments and a
  variable-width fetch from mips or a summed-area table. Both are
  approximations of the PCSS that already ships.

### Memory

| Option | Per cascade | Four cascades per target image |
|---|---:|---:|
| EVSM4 `RGBA32F` at 2048² with mips | 85 MiB | 341 MiB |
| EVSM2 `RG32F` at 2048² with mips | 43 MiB | 171 MiB |
| EVSM4 `RGBA32F` at 1024² with mips, cascades 2–3 only | 21 MiB | 43 MiB (two cascades) |

ADR-041 keeps one cascade set per physical target image, so these figures
multiply by the image count. Bistro already reaches the managed GPU budget on
the 16 GB M1 Pro ([ADR-083](../adr/083-supported-hardware-matrix.md)), so
full-resolution EVSM for every cascade does not fit.

### Where EVSM fits

Use EVSM4 at 1024² for cascades 2 and 3 only, built from their D32 maps by a
downsample, warp and separable blur pass when they redraw. Cascades 0 and 1
keep depth PCSS. The cascade cross-fade blends PCF and EVSM visibility across
the boundary. Ship EVSM behind an opt-in setting until measured evidence
justifies a default.

Open choices:

1. **Representation.** The recommendation is EVSM4, which bleeds less than
   EVSM2 for twice the memory. A light-bleeding reduction bias is a second
   parameter. Change only one of the two in a capture sweep.
2. **Blur width.** The width should equal the current PCF footprint in cascade
   texels so that the cross-fade band does not show a seam.

### Declined scope

EVSM for the local atlas is not proposed:

- One `RG32F` atlas layer is 128 MiB, four times the D16 layer.
- Faces of 128 to 1024 texels need per-face blurs with gutters.
- Hardware filtering across cube-face borders breaks the current per-tap face
  remap.
- One moment fetch cannot keep the current per-tap combination of opaque
  depth and receiver-gated RGB transmission.
- The four-tap mask filter costs 1.10 ms on the RX 6700 XT, so a single fetch
  saves less than the blur and memory cost.

Revisit local EVSM only if a persistent static local cache
([local shadow architecture](local-shadow-architecture.md)) makes per-face blurs
rare and measured filter cost dominates again.

## Relation to SDSM

Occupied-depth SDSM ([ADR-033](../adr/033-occupied-depth-sdsm-feedback.md)) is
implemented and opt-in. SDSM changes cascade fits, and through them the texel
size and depth span. The PCSS and proposed EVSM conversions read those values
per cascade, so the two features are compatible. SDSM is not a performance
lever for current shadows:

- SDSM moves resolution. SDSM does not reduce the raster or sampling work per
  cascade.
- A changed fit invalidates the retained cascade (ADR-041). Retention keeps
  cascade raster at about 0.46 ms per frame against 1.4–2.2 ms for each
  refresh on M1 Pro.
- Cascade raster scales with submitted triangles
  ([automatic mesh LOD](mesh-lod.md)), so a smaller map at equal texel density
  saves little.

A possible exception is to use occupied depth to skip far cascades that the
view cannot reach, such as in an enclosed street. That idea needs its own
proposal and measurement.

## Phases

1. **Far-cascade EVSM, opt-in.** Add the build pass for cascades 2–3, sample
   it in `Lighting.Deferred`, and sweep the exponent and bleeding bias.
2. **EVSM default decision** from quality captures and matched timings.

## Acceptance evidence

- **Off identity.** Bistro captures stay byte-identical with EVSM disabled.
- **EVSM.** Bistro captures of distant cascades at grazing angles, with
  bleeding inspected at awnings and foliage. Memory is reported per target
  image. Timings cover refresh frames and steady frames separately.
- **Native validation.** One focused Metal API validation run. Vulkan
  validation runs on a Windows host. If that host is unavailable, the gate is
  recorded as unavailable.
