---
status: partial
updated: 2026-09-01
authority: adr
---

# ADR-035: Canonical MTSDF screen-pixel range with evidence-gated fallback

## Status

**Accepted (partial).** Both production backends now receive a two-component
atlas `unit_range` and implement the same component-wise UV-derivative
reconstruction. UI color, world color, and picking use that shared coverage
contract. Loose transition atlases live outside generic texture inputs and are
validated as linear, uncompressed, one-mip RGBA8 with clamp-to-edge sampling;
stale `.vkt` sidecars are removed.

Production deliberately uses pure canonical RGB MSDF. The candidate alpha-SDF
blend is not accepted because matched pure-MSDF, pure-SDF, and blend captures
have not been compared on both native backends. Release shader compilation and
single-process native Metal API/GPU validation pass. Native Vulkan execution is
unavailable on the current macOS build, so the shader parity ledger remains
UNALIGNED and this ADR remains partial.

## Context

The Vulkan and Metal text shaders reconstruct MTSDF alpha from the median
distance divided by `fwidth(distance)`. The form is not the
[msdfgen reference contract](https://github.com/Chlumsky/msdfgen#using-a-multi-channel-distance-field).
The
reference converts the encoded atlas distance range into a projected
screen-pixel range from UV derivatives, then multiplies that range by the
decoded signed distance.

Applying `fwidth()` after `median3` also makes the derivative follow whichever
color channel currently supplies the median. That selection changes around
MSDF corners. UV derivatives provide the stable geometric input the reference
form expects.

The current CPU paths calculate a purported `screen_px_range`:

```c
Clamp(font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f)
```

They upload it, but neither text shader reads it. The expression is not the
canonical quantity either. `metrics.emSize` describes the metric coordinate
system; it is not the atlas resolution. For the checked-in Ubuntu artifact,
the expression computes `8 * 18 / 1 = 144` and the clamp returns `4`, while
`distanceRange * render_size / atlasPxPerEm` would be `8 * 18 / 64 = 2.25` for
an axis-aligned 18-pixel-em glyph. Perspective and rotation make a single CPU
render-size product invalid for world text in any case.

An MTSDF atlas also stores a true single-channel SDF in alpha. Upstream
documentation identifies alpha as useful for true-distance effects and warns
that antialiasing becomes unreliable when projected range is too small. The
specific blend used by `nuri`, `saturate(2 - range)`, is not an upstream
canonical rule. It is a VKR minification policy that must be compared against
pure MSDF and pure alpha-SDF output.

The checked-in loose atlas is not a valid input for evaluating either shader.
Its `.vkt` sidecar declares sRGB color data, lossy block compression, and a box
mip chain. A correct range can make damage in the stored field more visible.
Shader replacement therefore cannot land before, or separately from, clean
linear uncompressed atlas sampling.

## Decision

Adopt msdf-atlas-gen's derivative-based screen-pixel-range reconstruction in
both backends. Evaluate the alpha-channel minification blend as an explicit VKR
policy in the same implementation stage, but accept its threshold only through
the visual evidence gate.

### Range representation

For each atlas page, the cold load path computes:

```text
unitRange = (distanceRange / pageWidth, distanceRange / pageHeight)
```

`distanceRange` is the symmetric cooked range in atlas pixels. `unitRange` is a
two-component value because pages need not be square. It is constant for a
bound atlas page and does not depend on authored font size, transform, output
extent, or projection.

`VkrPreparedTextDraw` and both backend root ABIs gain this two-component
contract. The existing scalar `screen_px_range` is removed rather than
repurposed under a misleading name. UI and world preparation stop computing a
render-size product and delete the `[1, 4]` CPU clamp. This is a packet/root ABI
change on both Vulkan and Metal; it is not assumed to fit an unnamed spare
field.

### Fragment reconstruction

The MTSDF branch uses the reference form:

```hlsl
float screen_px_range(float2 uv, float2 unit_range)
{
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    float2 gradient_squared = max(dx * dx + dy * dy,
                                  float2(1e-12f, 1e-12f));
    float2 screen_tex_size = rsqrt(gradient_squared);
    return max(0.5f * dot(unit_range, screen_tex_size), 1.0f);
}

float range = screen_px_range(uv, unit_range);
float sd_msdf = median3(atlas.rgb) - 0.5f;
float alpha_msdf = saturate(range * sd_msdf + 0.5f);
```

Equivalent Slang and Metal code must produce the same coverage within the
cross-backend capture tolerance. Bitmap and system-raster branches continue to
use the sampled alpha channel directly.

### Alpha-channel fallback

The candidate VKR policy is:

```hlsl
float sd_sdf = atlas.a - 0.5f;
float sdf_weight = saturate(2.0f - range);
float sd = lerp(sd_msdf, sd_sdf, sdf_weight);
float alpha = saturate(range * sd + 0.5f);
```

It applies only to artifacts declared as MTSDF. The cooker records the field
kind and the loader rejects incompatible four-channel data. F0 compares this
blend at several thresholds against pure MSDF and pure alpha SDF. The accepted
threshold becomes a named shader constant or cooked policy version, not an
unexplained literal. If the comparison shows no advantage, the canonical MSDF
form lands without the blend.

### Visual, picking, and depth coverage

UI text remains blended and does not add an alpha discard. Picking derives
coverage from the same reconstructed alpha and retains its domain-specific
threshold.

World text that writes depth may discard below an explicit world-text alpha
threshold so transparent quad regions do not claim depth. That threshold and a
world/UI domain bit must have an explicit packet/root ABI representation and
matching Vulkan and Metal behavior. It is not silently packed into the current
dead range scalar.

### Atlas preconditions and landing boundary

The shader change lands atomically with all of the following:

- source atlas sampled as linear UNORM or float, without lossy block
  compression and without generated mips;
- stale font-atlas `.vkt` sidecars deleted and the font atlas directory removed
  from, or explicitly excluded by, generic texture packing;
- atlas dimensions and symmetric distance range validated at load time;
- correct MTSDF field-kind declaration and alpha-channel presence;
- two-component `unitRange` delivered through both backend ABIs.

The temporary request `?cs=linear&tc=data-mask` is defense in depth, not a
substitute for deleting stale sidecars. If `tc=data-mask` can still transcode to
a lossy block format on a device, it does not satisfy this decision.

## Consequences

Coverage responds to projected glyph size and perspective rather than to a
CPU guess. Expected outcomes are more stable corners and consistent edge width
over scale, but the evidence gates determine whether those outcomes are met.

The alpha fallback is no longer described as part of the upstream canonical
algorithm. VKR can keep, adjust, or reject it based on captures without
changing the accepted range reconstruction.

The packet/root contract becomes clearer: it carries an atlas-derived
`unitRange`, not a render-size-derived scalar. The cost is additional fragment
ALU and a backend ABI change. Any claim that the change is free, faster, or
slower requires a measured profile with text shown.

The two local resize snapshot cases are observational inputs, not accepted
goldens. F0 records their reports and captures. If a new or existing baseline
is proposed, it goes through the normal owner-acceptance workflow. The
offscreen text snapshot's final-color and picking-ID captures remain the
cross-backend coverage gate.

## Alternatives Considered

**Keep `fwidth(median)` and add alpha fallback.** This retains a shader that
cannot be compared directly with the generator's documented contract.
Rejected.

**Use a CPU scalar range.** This can approximate axis-aligned UI but does not
handle perspective or rotation in world text. Rejected.

**Always use alpha SDF.** This gives a simpler minification path but discards
the corner fidelity for which MSDF is used. Retained as an A/B reference, not
the production default.

**Use a per-glyph range attribute.** Current draws bind one atlas, so the value
is constant for the draw. Rejected until multi-font batching is a measured
need.

**Approximate projected texel size with `1 / fwidth(uv)`.** Upstream documents
this form as an approximation and `nuri` uses it. It can reduce shader work, but
the exact derivative magnitude is the initial correctness reference. Revisit
only with matched skewed, rotated, and perspective captures plus a measured GPU
benefit.

**Supersample an offscreen text target.** This adds a render target, pass, and
bandwidth, and resamples already reconstructed coverage. Rejected.

## Revisit When

- Text effects need true signed distance for outlines, glow, or shadows.
- A measured GPU profile puts text fragments on the critical path.
- Multi-page or multi-font batching changes the draw-level constant contract.
- The pinned msdf-atlas-gen version changes its recommended reconstruction.
- Subpixel antialiasing is requested and its output-transfer interaction has a
  separate accepted design.
