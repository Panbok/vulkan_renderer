---
status: partial
updated: 2026-08-05
authority: design
---

# Shadow, Transmission, and Transparency Improvements

Implementation record and residual design proposal. Most of the independently
shippable work described here now exists. Prerequisite reading:
[bistro-baseline-shading-investigation.md](bistro-baseline-shading-investigation.md),
which records the original diagnosis and the later owner-driven corrections.

## Implementation status

| Item | Status | Implementation authority |
|---|---|---|
| PCF light-space origin | Implemented | `vkr_shadow_light_space_origin_from_view()` and CPU basis tests |
| Caster-relevant cascade Z fit | Implemented | `vkr_shadow_fit_relevant_caster_z()` clips scene bounds against each cascade XY rectangle |
| PCF uniform-region early-out | Proposed | The 16-tap kernel is intentionally unchanged; no qualified Release evidence yet justifies a kernel change |
| SDSM depth-range fitting | Proposed | No depth pyramid/readback path exists; exact Bistro shadow-factor captures exclude it as the reported moving-wall cause |
| glTF transmission and volume import | Implemented | Material/import tests plus [ADR-018](../architecture/adr/018-graph-declared-transmission-feedback.md) |
| Graph-declared transmission feedback/refraction | Implemented, initial | Separate opaque, feedback-copy, transmission, and ordinary-blend stages; one immutable feedback copy per frame |
| Stable transparent ordering and opaque alpha | Implemented | Packed depth/tie-break keys and explicit opaque output alpha |
| PDF/solid-angle prefilter source LOD | Implemented | `ibl/specular_prefilter.slang` uses `SampleLevel`; CPU math tests pin the LOD |

The 2026-08-05 owner audits also found issues outside the proposal's original
attribution. The first moving exterior-wall band was analytic diffuse, caused
by orienting two-sided normals with camera-vector `faceforward`; fragment
shading now uses `SV_IsFrontFace`. A later curved-wall report was a different
phenomenon: depth-unprojected shadow-factor captures place its screen-moving
edge at the same world-space wall coordinates, proving it is a stationary sun
shadow rather than a CSM artifact. The café leak was an outdoor environment
reused as an indoor probe source; Bistro now authors an indoor diffuse cubemap
with specular IBL disabled. PBR's 17 sampled images now use 13 sampler
descriptors by sharing only semantically identical IBL sampler state, and each
IBL bake owns an immutable, GPU-deferred descriptor state.

Informed by a review of [Panbok/nuri](https://github.com/Panbok/nuri)
(`shadow_renderer.cpp`, `transmission_renderer.cpp`, `transparent_renderer.cpp`,
`material_lighting.sp`, `envmap_prefilter.comp`). nuri is a different
architecture — bindless descriptors, buffer-reference push constants, a
`RenderGraphBuilder` that renderers append passes to. The *techniques* port; the
*structure* mostly should not. Where nuri is worse than what we already have,
this document says so.

---

## Part 1 — Shadows

### 1.1 Where our cascade resolution actually goes

Measured against `VKR_SHADOW_CONFIG_BALANCED` with `cascade_count = 4` (the
config the Bistro snapshot case uses), camera near `0.1`, far `500`:

`max_shadow_distance = 120.0f` clamps the shadowed range to `[0.1, 120]`, so the
far 380 units of the view frustum receive **no shadows at all**, with
`shadow_distance_fade_range = 10.0f` fading them out over `[110, 120]`. That
alone is the reported "poor draw distance" — it is a config value, not an
algorithmic limit.

With `cascade_split_lambda = 0.75`, `vkr_shadow_compute_cascade_splits()` yields:

| Cascade | View-depth range | Slice bounding-sphere radius | World units / texel @ 2048 |
|---|---|---|---|
| 0 | 0.1 – 7.96 | ≈ 10.1 | ≈ 0.011 |
| 1 | 7.96 – 17.6 | ≈ 24 | ≈ 0.026 |
| 2 | 17.6 – 37.8 | ≈ 52 | ≈ 0.057 |
| 3 | 37.8 – 120 | ≈ 146 | ≈ **0.160** |

Cascade 3 covers 68% of the shadowed range at 16 cm per texel — coarser than a
chair leg. Three multipliers stack to produce that:

1. **`use_constant_cascade_size = true_v`** fits each cascade to the slice's
   **bounding sphere** (`extent = radius * 2`) rather than its light-space AABB.
   This is what makes the fit rotation-invariant and kills shimmer, but it costs
   roughly 1.3–1.7× linear depending on light angle. It is a real and
   deliberate trade.
2. **`cascade_guard_band_texels = 128.0f`** expands the extent by
   `2 × 128 / 2048 = 12.5%` linear.
3. **Splits are computed for a range nobody is looking at.** This is the big
   one, and §1.2 addresses it.

### 1.2 SDSM remains the single largest available shadow-resolution experiment

This section remains a proposal. It was not implemented as part of the Bistro
shading correction because exact `shadow_factor`, direct-diffuse, cascade, and
scene-depth captures excluded a cascade-fit defect. The first wall issue was a
two-sided normal flip; the later curved-wall edge unprojects to a fixed world
coordinate and is a valid cast shadow. Building SDSM would add a depth-pyramid
and previous-frame dependency without addressing either observation.

Our splits are derived analytically from `near_clip`, `far_clip` and a fixed
lambda. They ignore what is actually on screen. In the Bistro **interior**
(frame 1), the visible depth range is roughly `[0.5, 30]` — so cascade 3
(37.8–120) is entirely wasted and cascade 2 (17.6–37.8) mostly is. Half the
shadow atlas renders geometry no pixel samples.

Sample-distribution shadow maps fix exactly this: reduce the previous frame's
depth buffer to a min/max, linearize it, and fit the splits to the range that is
actually visible.

Refitting the same 4 cascades to `[0.5, 30]` instead of `[0.1, 120]`:

| Cascade | Range | World units / texel |
|---|---|---|
| 0 | 0.5 – 2.21 | ≈ 0.003 |
| 1 | 2.21 – 5.06 | ≈ 0.007 |
| 2 | 5.06 – 11.0 | ≈ 0.016 |
| 3 | 11.0 – 30 | ≈ **0.040** |

**A 4× improvement in the far cascade** with no extra memory and no extra draw
calls — the same geometry rendered into a tighter frustum. In enclosed scenes
the gain is larger; in a wide-open exterior it degrades gracefully to the fixed
split distribution.

**What nuri does well here**, and what is worth copying:

- The raw min/max is **temporally smoothed** with an exponential blend
  (`settings.sdsmTemporalBlend`), not consumed raw. Raw per-frame min/max makes
  cascades snap violently whenever a near object enters or leaves view.
- The readback result is **validated before use** — finite, within `[0,1]`
  device-depth range, `max >= min`, and linearization checked again — with a
  typed status (`Active` / `Stale` / `Invalid` / `Unavailable` / `FallbackFixed`)
  surfaced to debug data. A GPU readback that silently returns garbage would
  otherwise produce inexplicable shadow behaviour.
- There is an explicit **staleness bound** (`kSdsmMaxCachedSourceFrameLag = 2`)
  and a clean fall back to fixed splits when exceeded. It never blocks the frame
  waiting for a readback.
- `minMaxSplitDepths[cascadeCount] = fixedSplitRange.farDepth` — the **last
  split is pinned to the fixed far plane** even when SDSM tightens the interior
  splits, so a newly-appearing distant caster is still covered rather than
  falling off a cliff.

**What is not worth copying:**

- `shadow_sdsm_prev_frame_minmax.comp` dispatches a compute shader with
  `local_size = (1,1,1)` that does a single `texelFetch` at `(0,0)` and writes
  two floats. It is not a reduction — the reduction already happened in
  `depth_minmax_pyramid.frag`. A one-thread dispatch plus its barriers to move
  8 bytes is more expensive than `vkCmdCopyImageToBuffer` on the 1×1 mip.
- `updateShadowFrameData()` is a single ~640-line function inside a 4660-line
  file. The algorithm is sound; the packaging conflicts with our compression
  rules in `PRINCIPLES.md`.

**Fit into our renderer.** We have no depth pyramid — `grep` for `hiz`,
`depth_pyramid`, `occlusion_cull` over `lib/src/` returns nothing — so the
reduction chain has to be built. We *do* have the harder half already: the async
pixel readback API in `vkr_renderer.h` (`VkrReadbackStatus`, used by picking) is
exactly the non-blocking, fenced, ring-buffered mechanism SDSM needs.

Proposed shape:

1. New render-graph pass `Depth.MinMaxPyramid` after `World.Fullscreen`, writing
   a small mip chain down to 1×1. Declared as a normal graph resource so
   ADR-002 access tracking covers the barriers.
2. Reuse the readback ring to fetch the 1×1 result N frames later.
3. `vkr_shadow_system_update()` gains an optional
   `const VkrShadowDepthRange *` parameter. When absent or stale, behaviour is
   byte-identical to today — this keeps the change additive and testable.
4. Temporal blend + validation + staleness bound as above, with the status
   exposed through the existing metrics registry so the harness can assert on it.

A depth pyramid is also the prerequisite for GPU occlusion culling later, so the
cost is shared.

### 1.3 PCF: corrected origin ships; the early-out remains proposed

`sample_shadow_pcf()` in `common/csm.slangh` unconditionally takes **16
`SampleCmpLevelZero` taps** for every shadowed fragment. In practice the large
majority of pixels are fully lit or fully shadowed, and all 16 taps agree.

nuri's `tryResolveUniformPoissonPcf()` probes 9 taps in a fixed ring first; if
they all agree (`sum <= 0.0` or `sum >= 9.0`) it returns immediately and skips
the full kernel. Only genuine penumbra pixels pay for the wide filter. This is a
plausible candidate for a measured VKR experiment, not an assumed win.

**But nuri's kernel rotation is worse than ours and should not be copied.**
`shadowPoissonRotation(cascadeIndex)` returns one of only **four** fixed
orientations selected by `cascadeIndex & 3u`. Every pixel within a cascade
therefore shares an identical kernel orientation, so the Poisson pattern does
not dissolve into noise — it becomes a static, structured artifact locked to the
cascade. Our `shadow_hash12(cell)` derives a per-texel-cell angle, which is the
correct approach.

The old version had a real defect, recorded as §6a of the investigation: the
`light_space_origin.x` sign is inverted relative to the basis the shader
reconstructs, because `mat4_look_at` computes `s = cross(f, up)` which works out
to `-right`. Fix that first, then add the early-out — otherwise the early-out
will be measured against an already-flickering baseline.

The origin correction and per-cell hash rotation ship. A future measured kernel
experiment would:

1. retain the corrected origin from `vkr_shadow_compute_cascade_matrix()`;
2. add a 9-tap agreement probe before the 16-tap loop; and
3. retain the per-cell hash rotation.

Expected: most fragments drop from 16 taps to 9, penumbra fragments go to 25.
Given the fragment shader is the hot path, this needs
`vkr-performance` treatment — before/after `sponza_orbit` in Release, not an
assertion that it is faster.

### 1.4 Cascade fit and lookup corrections

- **Implemented: caster-relevant per-cascade Z extension.** The old path
  extended every cascade to the full scene AABB whenever scene bounds were
  enabled. The current path transforms the bounds and accepts only caster
  portions that intersect the final cascade XY rectangle, so near cascades no
  longer inherit an unrelated full-scene depth range.
- **Implemented: redundant shadow lookup work removed.** The old shader
  computed `mul(g_ubo.view, ...)` and `select_cascade()` twice in adjacent
  blocks, then repeated covering-cascade searches in the blend band. The
  current lookup carries the selected depth/cascade result through one path.

### 1.5 On the "poor draw distance" specifically

Three independent causes, in order of size:

1. `max_shadow_distance = 120.0f` against a 500-unit far plane. Raising it
   without SDSM just spreads the same 2048² over more world — it trades near
   quality for far coverage. **Raise it together with §1.2, not before.**
2. Splits fitted to a range nobody looks at (§1.2).
3. Bounding-sphere fit plus a 128-texel guard band (§1.1).

nuri's `applyDirectionalShadowFitHysteresis()` is the interesting middle path:
it keeps a tighter AABB-style fit but suppresses per-frame breathing with
hysteresis on the fit itself, rather than paying the full bounding-sphere
penalty on every frame. That is worth evaluating as an alternative to
`use_constant_cascade_size` once SDSM lands — but only with measured shimmer
comparisons, not by inspection.

---

## Part 2 — Transmission

Implemented: the importer retains `KHR_materials_transmission`, IOR, volume
thickness, attenuation color, and attenuation distance independently from glTF
alpha mode. Transmissive materials enter a dedicated graph stage instead of
being rewritten to ordinary alpha blend.

### 2.1 nuri's model is the glTF reference model, and it is the right target

`transmission.frag` implements the glTF Sample Viewer formulation:

- `getVolumeTransmissionRay()` — `refract()` through the surface using IOR and
  `KHR_materials_volume` thickness, scaled by model scale.
- `resolveTransmissionUv()` — reprojects the refracted **exit point** through
  view-projection to screen space, rather than offsetting UVs by a fudge factor.
  This is what makes thick glass bend correctly instead of just smearing.
- `applyVolumeAttenuation()` — Beer-Lambert
  `exp(-(-log(attenuationColor) / attenuationDistance) * distance)`. Correct
  coloured absorption for tinted glass.
- `transmissionFramebufferLod()` — maps roughness (adjusted by IOR via
  `applyIorToRoughness`) to a mip of the feedback texture, so frosted glass
  blurs what is behind it.
- A thin-surface fallback with `kTransmissionThinSurfaceMinAlpha = 0.35` for
  materials with no volume extension.

The shipped shader follows this model: exit-point reprojection, roughness LOD,
Beer-Lambert attenuation, and a thin-surface fallback are confined to the
dedicated transmission pass.

### 2.2 The feedback-copy strategy, and its cost

Transmissive draws need to sample the already-composited scene behind them.
nuri copies the frame color into a feedback texture and picks a refresh mode:

- `BeforeEachDraw` when the candidate count is `<= kMaxExactTransparentFeedbackDraws`
  (64). Each glass pane sees everything drawn before it, including other glass.
- `OnceBeforeFirstDraw` otherwise — one shared copy for the whole frame, with an
  explicit `NURI_LOG_WARNING` naming the counts and the budget.

**The warning is the part I would copy verbatim in spirit.** It refuses to
silently degrade — it states what was dropped and why. That matches our "no
silent caps" principle.

**The budget of 64 is too high.** `BeforeEachDraw` at 64 candidates means up to
64 full-resolution color copies plus their barriers in a single frame. At
1600×1200 that is roughly 490 MB of copy traffic per frame before any shading.
nuri gates on *candidate* count rather than *visible* count, so the mode is
chosen before culling narrows it. I would set the exact-refresh budget to
single digits and measure, rather than inheriting 64.

Also worth noting: a comment in `transmission.frag` records that this pass runs
**post-TAA**, so transmissive surfaces are excluded from temporal
antialiasing and will alias more than the rest of the frame. That is an
acknowledged trade in nuri, not an oversight, but it is a consequence to accept
knowingly.

### 2.3 Implementation in our renderer

The implementation uses the existing architecture:

- `VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT` with depth-test-on / depth-write-off
  and `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`.
- Back-to-front sorting in `vkr_draw_batcher_finalize()`.
- A JSON-authored render graph where a new pass and a new resource are a
  declarative addition (ADR-003), with access tracking handling barriers
  (ADR-002).

Shipped staging:

- **Stage 1 — importer.** `VkrMaterial` and its local uniform block retain the
  transmission/volume fields. Transmission classification is independent of
  `OPAQUE`, `MASK`, and `BLEND` routing.
- **Stage 2 — feedback resource.** `World.TransmissionFeedback` is an immutable
  graph-declared copy of opaque HDR scene color, created once per frame.
- **Stage 3 — refraction.** The dedicated transmission draw stage activates the
  refraction branch in `world/pbr.slang`; ordinary opaque fragments do not take
  that branch because they run through a different pass/list contract.
- **Stage 4 — per-draw refresh remains unimplemented.** Deep transmissive
  compositing still sees the once-per-frame opaque feedback rather than prior
  transmissive draws. This is an explicit initial limitation in ADR-018.

---

## Part 3 — Transparency

### 3.1 What we already have is structurally fine

The original investigation found no defect in the blend state.
`vkr_draw_batcher_finalize()` sorts transparent commands back-to-front before
batching, and the domain state is correct. The reason Bistro looks wrong is that
almost nothing reaches the transparent list — `draw.world.transparent_draws`
averaged 1.4 against 143.2 opaque because the historical importer dropped
transmission and the source asset marked its glass `OPAQUE`. Those counts are
historical; transmission now has its own draw list.

Transmission now fixes that dominant symptom without redefining the source
alpha mode.

### 3.2 What nuri does better

- **Implemented: stable tie-break in the sort.** `sortTransparentDraws()` compares
  `sortDepth` and falls back to `stableOrder` when depths are equal. Our
  submission now packs view depth with a stable visibility slot, and the batch
  comparator falls back to the complete draw key when distances match.
- **Transparency and transmission are separate renderers** that contribute into
  a shared transparent stage (`buildTransparentStageContribution`), rather than
  one pass with mode branches. That separation is worth keeping when we add
  transmission — it maps naturally onto our pass executors.

### 3.3 What we should not copy

nuri's renderers each own scene caches, pipeline state, shader handles, ring
buffers, and frame-transaction bookkeeping (`beginFrameTransaction`,
`onFrameSubmitted`, `onFrameAbandoned`, `restorePendingFrameState`). That is a
consequence of renderers being independent objects that append to a graph
builder. Our architecture already centralizes this: the frontend owns
subsystems, the graph is JSON-authored, and packet submission is the contract
(ADR-003, ADR-004). Importing nuri's per-renderer transaction machinery would
duplicate what `renderer_frontend.c` already guarantees.

### 3.4 Implemented opaque-alpha correction

`world/pbr.slang` forces output alpha to one for `OPAQUE` materials. This keeps
the HDR feedback attachment valid for transmission consumers.

---

## Adjacent finding: the prefilter mip selection

This correction ships and is retained here because it came from the same
review.

The old `ibl/specular_prefilter.slang` used `source_cubemap.Sample(...)` inside the
importance-sampling loop. Implicit derivatives are meaningless for an
importance-sampled direction, so every one of the 256 taps reads source mip 0,
leaving the high-roughness mips under-converged and firefly-prone.

nuri's `envmap_prefilter.comp` computes the mip from the sample PDF:

```glsl
float computeLod(float pdf) {
  return 0.5 * log2(6.0 * width * height / (sampleCount * max(pdf, 1.0e-6)));
}
```

…and clamps to `lod = 0` when `roughness < 0.0001`. The shipped shader uses this
solid-angle/PDF calculation with `SampleLevel`; `ibl_math_tests` covers its
boundary behavior.

---

## Suggested sequencing

| Order | Item | Status | Why here |
|---|---|---|---|
| 1 | Stable tie-break in transparent sort (§3.2) | Implemented | Independent, removes a future red herring |
| 2 | `light_space_origin.x` sign (§1.3) | Implemented | Required before PCF measurement |
| 3 | Prefilter PDF mip (adjacent) | Implemented | Explicit source LOD |
| 4 | Transmission importer and independent routing (§2.3 stage 1) | Implemented | Restores authored glass semantics |
| 5 | Caster-bounded cascade Z (§1.4) | Implemented | Avoids full-scene depth ranges per cascade |
| 6 | PCF early-out (§1.3) | Proposed | Needs `vkr-performance` evidence |
| 7 | Depth min/max pyramid + SDSM (§1.2) | Proposed | Needs a new graph pass and ADR |
| 8 | Transmission feedback + refraction (§2.3 stages 2–3) | Implemented, initial | One immutable feedback copy per frame |
| 9 | Per-draw transmission feedback (§2.3 stage 4) | Proposed | Needs an explicit bandwidth budget |

SDSM remains ADR-worthy because it adds a frame-latency dependency between the
depth buffer and shadow fitting. Transmission feedback is owned by ADR-018.

## Open questions

- Should SDSM's depth reduction share a pyramid with future GPU occlusion
  culling? If yes, the pyramid's format and mip policy should be decided once,
  now, rather than fitted to shadows and refitted later.
- Is `use_constant_cascade_size` still the right stabilization strategy, or
  should we evaluate nuri's fit hysteresis? Requires a shimmer comparison
  harness case, which does not exist yet.
- What is our actual per-frame budget for transmission feedback copies at
  1600×1200? This determines whether §2.3 stage 4 is worth building at all.
