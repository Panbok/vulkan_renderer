---
status: proposed
updated: 2026-08-25
authority: adr
---

# ADR-035: Canonical MTSDF screen-pixel-range shading

## Status

Proposed. No production code implements this decision.

## Context

`text_alpha()` in `lib/src/renderer/shaders/vulkan/slang/text/default.slang`
antialiases an MTSDF glyph like this:

```hlsl
float distance = max(min(atlas.r, atlas.g), min(max(atlas.r, atlas.g), atlas.b)) - 0.5f;
return saturate(distance / max(fwidth(distance), 1e-6f) + 0.5f);
```

`lib/src/renderer/shaders/metal/msl/text/default.metal` carries the same form.

This is not the msdf-atlas-gen contract, and it fails in three separate ways.

`fwidth()` is applied to the median rather than to the UV. `median3` returns
whichever of R, G, B is the middle value, and which channel that is changes
across a corner — that switch is the entire mechanism by which MSDF preserves
sharp junctions. The median is therefore piecewise-smooth with discontinuities
exactly at corners, and its screen-space derivative spikes there. The reference
form takes `fwidth()` of the UV, which is smooth everywhere, and converts to a
distance-space rate through a known unit range.

The `.a` channel is never sampled. In an MTSDF atlas the alpha channel holds the
true single-channel signed distance. It is the channel that stays well-behaved
under minification, where the three color channels disagree at a rate the
sampler cannot resolve. It is the reason to pay four channels instead of three,
and the current shader pays for it and discards it. Without it there is no
graceful path below roughly two screen pixels of range, which is exactly the
regime a downsized window puts UI text into.

At high magnification `fwidth(distance)` tends to zero, the guarded divide
saturates, and the glyph edge becomes a hard step with no antialiasing at all.

The correct range is meanwhile computed on the CPU and thrown away.
`vkr_ui_system_prepare_text_draws()` and
`vkr_world_resources_prepare_text_draws()` both evaluate:

```c
screen_px_range = Clamp(font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f);
```

`vkr_vulkan_draws.c` writes it into `root->material_alpha.x`. No shader reads
`material_alpha`. The expression is also wrong: msdf-atlas-gen's contract
divides by the atlas pixels-per-em, not by `emSize`. With `Ubuntu-2d.json`'s
`distanceRange = 8` and `atlas.size = 64`, the correct value at 18 px is 2.25;
the shipped expression computes `8 × 18 / 1 = 144` and the clamp pins it to
`4.0` for every font at every size. The clamp is what conceals the error, and
`atlas.size` is parsed into `VkrMtsdfFontMetadata::size`, validated, and never
read.

`nuri` implements the reference form in
`assets/shaders/text_2d_mtsdf.{vert,frag}` and `text_3d_mtsdf.{vert,frag}`:
`unitRange = pxRange / atlasSize` computed once per vertex,
`max(0.5 * dot(unitRange, 1/fwidth(uv)), 1.0)` per fragment, and an explicit
blend toward the `.a` SDF below two pixels of range.

## Decision

Adopt the msdf-atlas-gen reference shading form in both backends, and make
`pxRange` a cooked constant rather than a CPU-computed product.

**Fragment form.** Both `text/default.slang` and `metal/msl/text/default.metal`
compute:

```hlsl
// unitRange = pxRange / atlasSize, computed once per vertex
float screenPxRange(float2 uv, float2 unitRange)
{
    float2 screenTexSize = 1.0f / max(fwidth(uv), 1e-6f);
    return max(0.5f * dot(unitRange, screenTexSize), 1.0f);
}

float range = screenPxRange(uv, unitRange);
float sdMsdf = median3(atlas.rgb) - 0.5f;
float sdSdf  = atlas.a - 0.5f;
float fall   = saturate(2.0f - range);
float sd     = lerp(sdMsdf, sdSdf, fall);
float alpha  = saturate(range * sd + 0.5f);
```

`unitRange` is a vertex output. Computing it per vertex keeps the atlas-extent
query out of the fragment shader, matching nuri's arrangement.

**Range source.** `pxRange` is the cooked `distanceRange` in atlas pixels,
delivered by [ADR-034](034-offline-cooked-font-artifacts.md), and `atlasSize` is
the bound atlas extent. Neither is a function of render size; the projection
derivative supplies that. `VkrPreparedTextDraw::screen_px_range` is repurposed
to carry the cooked `distanceRange` and both `prepare_text_draws` functions stop
computing a render-size product. The `[1, 4]` clamp is deleted. The clamp that
matters is `max(..., 1.0)` inside `screenPxRange`, which is what stops sub-pixel
text from vanishing.

**Minification fallback.** The `saturate(2.0 - range)` blend toward the `.a`
channel is part of the contract, not an option. Two screen pixels of range is
the documented threshold below which MSDF color-channel artifacts dominate.

**World text.** The 3D fragment path discards below
`alphaDiscardThreshold` so text participates correctly in depth, as nuri's
`text_3d_mtsdf.frag` does. The threshold travels in the existing packet root
alongside `pxRange`.

**Bitmap and system fonts** keep the `material_flags == 0` branch and continue
to return `atlas.a` directly. This decision is about the MTSDF branch only.

**Atlas sampling preconditions.** The form assumes the atlas is sampled as
linear UNORM or float, uncompressed, with no mip chain. ADR-034 guarantees that
for cooked fonts. Until then, the MTSDF loader must request
`?cs=linear&tc=data-mask` so a `.vkt` sidecar's class cannot override it, and
`vkr_vkt_packer` must skip font atlases. Landing this shader against the current
UASTC/sRGB atlas would make output worse, not better, because a correct range
computation amplifies a corrupted field instead of blurring past it.

## Consequences

Antialiasing quality becomes a function of the actual projected glyph size on
screen, which is the property that makes SDF text resolution-independent. The
same font renders correctly at 12 px in a downsized window and at 128 px in a
world label without a per-size asset or a per-size tuning constant.

Corner notching from `fwidth(median)` disappears. This is directly observable on
`M`, `W`, and `4` and is the spec's corner-fidelity evidence gate.

Small text stops falling apart, because the `.a` blend takes over below two
pixels of range. This is the half of the reported scaling symptom that lives in
the shader; the other half is metric quantization and UI scale, covered by
ADR-034 and [ADR-036](036-dpi-derived-ui-text-scale.md).

Dead data leaves the packet root. `screen_px_range` stops being a computed value
nobody reads and becomes a cooked constant the shader consumes.

Existing text goldens change. Every snapshot with visible text needs a
replacement baseline and owner acceptance. The current goldens encode the
defect, so this is expected rather than a regression, but it is not free — the
spec's evidence gates enumerate the runs.

Fragment cost rises slightly: one extra texture channel is already fetched, and
the added work is a `dot`, a `lerp`, and a `saturate`. The removed work is a
divide. This is not expected to be measurable, and the spec does not claim it is
free without a measurement.

The `text_picking_fragment` entry point shares `text_alpha()` and inherits the
change, so picking coverage moves with visual coverage. That is correct, and it
means picking captures are part of the evidence set.

## Alternatives Considered

**Keep `fwidth(distance)` and add the `.a` fallback only.** Cheaper, and it
would fix the small-text case. Rejected because it leaves the corner-derivative
discontinuity, leaves magnification unantialiased, and keeps a form that cannot
be reasoned about against msdf-atlas-gen's documentation. Half-fixing a
contract is worse than either endpoint.

**Use the CPU-computed `screen_px_range` as a uniform scalar with no
derivative.** This is what the existing dead field was presumably intended for,
and it works for axis-aligned screen-space UI text where the projected size is
known on the CPU. Rejected because it cannot handle world text at all — a 3D
label's projected size varies per fragment with perspective and rotation — and
because it would require the UI and world paths to use different shading models
for the same font.

**Supply the range through a per-glyph vertex attribute.** More flexible; lets
different fonts batch together. Rejected as premature: text draws already batch
per atlas, and a push-constant scalar plus a vertex-computed `unitRange` costs
nothing. Revisit if multi-font batching becomes a measured need.

**Render text through a separate high-resolution offscreen target and
downsample.** Sidesteps the shading question entirely and gives excellent
quality. Rejected: it costs a target, a pass, and bandwidth proportional to
supersample factor, for a problem that a correct four-line fragment shader
solves.

## Revisit When

- Text needs outlines, glow, or drop shadows. nuri's `MtsdfParams` carries
  `outlineWidth` and `glow`; adding them means a second distance threshold and a
  second color, which changes the fragment contract.
- Subpixel (RGB-stripe) antialiasing is requested for desktop UI text. That
  needs three range evaluations at horizontal offsets and a different blend
  equation, and it interacts with the output transfer function that
  [presentation-dpi-and-transfer-function-spec.md](../../rendering/presentation-dpi-and-transfer-function-spec.md)
  governs.
- A measured profile puts the text fragment path on the critical path. The
  per-vertex `unitRange` and the `.a` blend are the first things to reconsider.
- msdf-atlas-gen changes its recommended shading form at a pin bump.
