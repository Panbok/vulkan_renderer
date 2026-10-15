---
status: partial
updated: 2026-10-15
authority: adr
---

# ADR-037: SMAA 4x and T2x temporal antialiasing

## Status

Accepted (partial): SMAA T2x on the desktop pipeline (Vulkan) since
2026-10-13, replacing portable scene-linear TAA and FXAA (owner decision,
[Short-history rendering](../proposals/short-history-rendering.md),
section 2). Since 2026-10-15 FSR 3.1 native antialiasing is the default and
SMAA T2x the selectable fallback (`anti_aliasing_method` = `smaa_t2x`), also used
where the build or device lacks FSR
([ADR-052](052-vulkan-fsr31-upscaling.md)). SMAA 4x (S2x on a 2x multisampled visibility raster, plus T2x)
is an opt-in path since 2026-10-15 (owner decisions of 2026-10-14 and
2026-10-15, after the owner reported aliasing on thin geometry in motion; it
stays off by default because its measured gain did not repay its cost); it
replaces the former visibility-buffer MSAA proposal. Deformation and broader motion signals
remain open. The tiled pipeline keeps multisampling without temporal history
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 7).

## Context

Visibility raster alone does not suppress subpixel geometry and specular
shimmer. Portable TAA averaged up to 128 frames, and its long history showed
as smearing, ghosting and frozen noise; two of its static-accumulation inputs
were found missing (skinned poses and cloud wind). Short history needs a
morphological antialiaser for edges and at most one previous frame.

## Decision

The desktop pipeline runs SMAA T2x (Jimenez et al. 2012) from the reference
`SMAA.hlsl` at iryoku/smaa commit `71c806a8`, HIGH preset
([`post/smaa.slang`](../../renderer/src/shaders/vulkan/slang/post/smaa.slang),
[`vendor/smaa.md`](../../vendor/smaa.md)):

- **Jitter.** Two subsamples alternate every frame
  (`VKR_TEMPORAL_SEQUENCE_LENGTH` 2): the reference's (0.25, −0.25) and
  (−0.25, 0.25) with y up, which VKR's raster pixels, y down, write as
  (+0.25, +0.25) and (−0.25, −0.25), with area-texture rows (1, 1, 1, 0) and
  (2, 2, 2, 0) (`vkr_temporal_jitter_for_frame`,
  `vkr_temporal_smaa_subsample_indices`). Projection jitter translates clip xy
  in proportion to clip w. With SMAA 4x the jitter is the reference's
  (0.125, 0.125) and (−0.125, −0.125) with y up, (+0.125, −0.125) and
  (−0.125, +0.125) in raster pixels, and the rows are the reference's 4x table
  per frame and sample: (5, 3, 1, 3) and (4, 6, 2, 3), then (3, 5, 1, 4) and
  (6, 4, 2, 4). S2x without T2x takes (1, 1, 1, 0) and (2, 2, 2, 0).
- **SMAA 1x on each subsample.** After exposure, grading and the GT7
  transform write the display-linear target (ADR-043), three compute passes
  run edge detection (luma of the square-root-encoded colour), blending
  weights with diagonal and corner detection, and neighbourhood blending, which
  stores `sqrt(5·|v|)` of the antialiased motion in alpha. The area
  (160 × 560 R8G8) and search (64 × 16 R8) tables upload once at device
  creation as permanent sampled rows. Edge detection is predicated on the
  visibility image (since 2026-10-15): where a pixel's draw differs from its
  left or upper neighbour's, the luma threshold halves (0.1 to 0.05), so
  silhouettes of low luma contrast are found; elsewhere it stays. The
  reference's predication also doubles the threshold elsewhere, which does
  not help such silhouettes.
- **Resolve.** `Post.SMAA.Resolve` blends this frame's antialiased subsample
  with last frame's at `uv + motion`, weight
  `0.5 · saturate(1 − 30 · sqrt(|a²−b²| / 5)) · accept`, the reference's
  velocity weighting times the accept mask below. VKR's motion already points
  from the current to the previous position, so it is not negated as the
  reference's motion-blur velocity is. The antialiased subsample is a history
  image; the previous frame's instance is chosen as the temporal tuple is.
- **Accept mask.** `Temporal.Resolve` no longer blends colour. It hands the
  jittered raster colour (`scene_resolved_color`) to post-processing, keeps
  raw depth (R32F), identity and surface token as history, and writes an R8
  mask: zero without valid history, off screen, on invalid validity and on a
  blended overlay without identity; for a pixel moving half a pixel or more,
  also zero unless one of the 3 × 3 previous texels around its reprojected
  position has its identity, surface token and, outside blended overlays, a
  device depth within 2 % of the depth the surface had then. Reversed depth
  is close to inverse linear depth, so the relative device tolerance is a
  relative linear one; the former fixed 0.005 covered the whole depth range
  beyond about 15 m at a 0.1 m near plane. A still pixel's history is the
  other subsample of the same content, which may land on another leaf, wire
  or silhouette side, so the identity and depth checks apply only to moving
  pixels. Authored temporal reactivity no longer reduces the weight: two
  frames ghost for at most one, and a partial weight makes a still image
  alternate between its subsamples.
- **Flags.** SMAA runs on desktop frames in the default render mode with the
  display-linear target (`VKR_SMAA_DISABLED` turns it off); T2x's jitter and
  resolve run with SMAA, and the motion and history views (render modes 7 and
  8) keep them. A perspective projection change no longer resets history;
  a change to or from an orthographic projection still does, because sky
  reprojection assumes an orthographic eye ray stays fixed.
- **Inputs moved off TAA.** Local shadows keep their four rotated filter taps
  and contact-march offset (owner decision, 2026-10-13), but both alternate
  between two phases in step with the jitter, which the resolve averages into
  about eight taps; a still image therefore stays stable, and penumbrae keep a
  fine per-pixel pattern. Nine fixed taps cost 1.28 ms more on the Bistro
  street at 2560 × 1440. DoF and motion blur read
  the jittered raster image and its depth with no offset. Vulkan material
  samplers take a −0.5 texture LOD bias for the jitter's blur, as the
  reference recommends; it applies to explicit-gradient and implicit
  sampling alike. The G-buffer resolve's normal and ORM fetches scale their
  gradients by √2 to cancel it (since 2026-10-15): the packer bakes each
  mip's normal variance into roughness, which only the mip matching the
  footprint holds. Removing the bias from every sampler instead raised the
  street view's still error by 9 % for 10 % less flicker away from edges.

### SMAA 4x

The reference's SMAA S2x runs SMAA 1x on each sample of a 2x multisampled
image and blends the second run over the first at 0.5; with T2x it forms
SMAA 4x. VKR multisamples only the camera's visibility raster and shades the
second sample only where it can differ
([`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json),
`world/deferred.slang`):

- **Raster and split.** With `msaa_enabled`, `VBuffer.Opaque.MSAA2` draws
  the opaque buckets into `opaque_vbuffer_ms` and `opaque_vbuffer_depth_ms`
  (2 samples, the standard positions, sample 0 at (+0.25, +0.25) and sample
  1 at (−0.25, −0.25) from the centre, y down). `VBuffer.Split` writes
  sample 0's identity into the single-sample `opaque_vbuffer` that every
  other consumer reads, sample 1's identity and depth into `_s1` images, an
  R8 edge mask, and an indirect-dispatch list of the pixels whose second
  sample needs shading (one wave-aggregated atomic per wave, after a
  one-thread reset dispatch). Sample 1 is listed when it names another
  triangle and does not continue sample 0's surface: another draw, or a
  depth more than 0.1 % off sample 0's surface extrapolated half a pixel,
  with the depth gradient taken from a neighbouring sample 0 on the same
  draw within 2 % depth. Tessellation edges stay unlisted; a railing a few
  centimetres before a wall in the wall's draw is listed. `VBuffer.SplitDepth` writes the single-sample
  depth through `SV_Depth`: the mean of both samples where they lie on one
  triangle or within 2 % of each other, which is the pixel centre's depth
  because the positions are symmetric and device depth is affine across a
  triangle; elsewhere sample 0's. Consumers reconstruct positions at pixel
  centres; sample 0's off-centre depth made local-light shadows show acne
  on grazing walls.
- **Second sample.** `GBuffer.Resolve.Sample1` and
  `Lighting.Deferred.Sample1` dispatch over the list only, resolve and light
  sample 1 at its raster position into its own G-buffer and
  `scene_color_s1` with the base lighting kernel (no clearcoat, sheen,
  anisotropy, subsurface or light-contribution counters), and keep the
  motion, validity and debug outputs sample 0's. Per-pixel lighting inputs
  (GTAO, the local shadow mask, the diffuse volume, baked lamps and their
  shadow) come from the nearest pixel whose sample 0 names sample 1's draw,
  else the pixel's own.
- **Compose.** `MSAA.Compose` runs after bloom: sample 1's display input is
  sample 0's post-processed colour plus, at listed pixels, sample 1's lit
  colour minus sample 0's. Bloom is additive, so the sum is sample 1
  bloomed by sample 0's halo. A pixel that a blended overlay (the visibility
  overlay bit) or the first transmission layer covers keeps sample 0's, the
  only sample those layers shade. Sample 1 gets no fog or overlay of its own.
- **Post.** `Post.DisplayLinear.Sample1` transforms that colour; the SMAA
  edge, weight and blend passes rerun on it with sample 1's rows, and the
  blend writes `0.5 · (sample 1 + destination)` into `smaa_output`, which
  T2x's resolve then reads as before.
- **Flags.** S2x is opt-in: it runs with SMAA when `VKR_MSAA_ENABLED` is
  set, unless the frame enables depth of field or motion blur. Those effects replace a pixel
  with its blurred surroundings, where a second sample's edge has no
  meaning, so their frames keep T2x alone with its ±0.25 jitter.

History selection is unchanged in kind: the immediately preceding submitted
frame, in flight or complete, or none, now anchored on the depth history. Its
same-queue barriers order the previous producer's writes before the readers,
and output reuse waits for completed last use. Reset publication invalidates
older depth and SMAA history, so an in-flight reset frame cannot leave
pre-reset producers eligible. Motion blur reads the same transform tuple with
or without T2x.

History identity is the temporal instance index and generation plus a stable
mesh-local submesh token in bits 1–17 of the temporal flags; bit 0 elects the
transform-history writer. Opaque and rigid transmission and blend paths
publish current-to-previous motion, and the G-buffer writes
translation-free sky rotation motion.

Removed with portable TAA: Halton jitter over eight phases, current-colour
reconstruction onto the unjittered grid, cubic and footprint-validated history
reconstruction, the neighbourhood clamp, the 0.9 and 0.99 retention, the
128-sample static accumulation with its CPU scene signature, and FXAA.

## Consequences

Edges come from SMAA on every frame and a two-sample supersample from T2x;
there is no long history to smear, ghost or freeze. A still camera converges
after two frames. Specular and subpixel shimmer that long history averaged now
shows, limited by the existing specular antialiasing. A pixel that disoccludes
or changes surface while moving takes the current subsample alone for a frame.

Evidence, Vulkan Release, RX 6700 XT, 2026-10-13, local and
non-authoritative, `local-offscreen.json`:
- `shader_fixes_bistro_move_stop` (800 × 600, eight frames after the camera
  stops): consecutive frames differ by more than 8/255 on 0 % of pixels
  (mean 0.004–0.011/255, from the moving clouds). The first accept mask, which
  checked identity and depth on still pixels as well, flickered on 2.3 % (2 × 2
  footprint) and 0.6 % (3 × 3) of pixels along silhouettes and foliage.
- `shader_fixes_bistro_fxaa_on` (moving camera): T2x is sharper than the
  former TAA on foliage and window frames, with no trail behind moving edges.
- SMAA 1x alone on `local_shadow_bistro_street_capture` changes 4.4 % of
  pixels, all on edges, and smooths railings, window frames and door edges.
- Timing, `profile` with `local-offscreen-gpu-repeated.json` on the street
  view of `local_shadow_bistro_street_capture` at 2560 × 1440 with T2x on,
  5 × 60 measured frames, p50: the antialiasing chain (`Temporal.Resolve`,
  `Post.DisplayLinear`, three SMAA passes, `Post.SMAA.Resolve` and the final
  draw) takes 1.41 ms (0.56 + 0.21 + 0.32 + 0.21 + 0.11) against portable
  TAA's 0.94 ms (`Temporal.Resolve` 0.72 and the final draw 0.23, same view,
  same day); `Shadow.LocalMask` 4.34 ms against 4.33 ms; frame wall 19.9 ms
  against 19.2–19.3 ms. The runs are local; warmup stability was not met, so
  they carry no authority.

SMAA 4x evidence, Vulkan Release, RX 6700 XT, 2026-10-15, local and
non-authoritative (dirty tree; warmup stability not met):
- Oracle: the Bistro street view of `local_shadow_bistro_street_capture`
  at 1280 × 720 against the same view rendered at 3840 × 2160 without
  antialiasing and box-filtered 3 × 3 in linear light, mean absolute 8-bit
  error on geometric edges (reference depth range above 2 % in a pixel)
  and on thin features (foreground coverage 5–70 % with background on both
  sides along an axis). A 2 × 2 reference favoured whichever pattern sits on
  its grid and was discarded.
- Still frame, geometric edges: no AA 16.39, SMAA 1x 11.88, S2x 11.68,
  T2x 10.24, SMAA 4x 10.26. Thin features: SMAA 1x 9.51, S2x 9.63, T2x
  8.62, SMAA 4x 8.97.
- Camera turning at 2°/s, four consecutive frames each against its own
  reference: geometric edges T2x 11.44, SMAA 4x 11.18; instability (per-pixel
  standard deviation of the signed error over the frames) 8.65 and 7.87;
  thin features 9.05 and 9.19.
- Timing, `profile` with `local-offscreen-gpu-repeated.json` at
  2560 × 1440, 5 × 60 frames, off then on within ten minutes, p50: frame
  wall 20.31 → 23.06 ms; pass sum 19.88 → 22.75 ms. Added passes:
  `VBuffer.Split` 0.76, `VBuffer.SplitDepth` 0.34, sample-1 lighting 0.58
  and resolve 0.10, its display-linear pass 0.25 and three SMAA passes
  0.41, `MSAA.Compose` 0.15; `VBuffer.Opaque.MSAA2` 0.38 replaces
  `VBuffer.Opaque` 0.37. Shading every triangle change instead cost 2.62 ms
  in sample-1 lighting alone, and full-screen sample-1 kernels with an
  edge-mask early-out 2.14 ms.
- Debug with synchronization validation on the street view: no messages.
  804 CPU tests pass.

Predication and the normal/ORM footprint, same oracle and day, T2x: still
geometric edges 10.28 and 10.27 (before 10.24), thin features 8.66 and 8.64
(8.62); moving geometric edges 11.46 and 11.41 (11.44), frame-to-frame
flicker (mean absolute second difference over three consecutive frames;
the reference's own motion gives 12.61 and 3.32) on edges 12.89 and 12.79
(13.02) and elsewhere 3.28 and 3.27 (3.40). Predication changes 1.8 % of the
pixels and costs 0.06 ms in `Post.SMAA.Edges` at 2560 × 1440 (0.074 to
0.136 ms p50, `local-offscreen-gpu-repeated.json`, frame wall 20.31 and
20.40 ms within its 0.4–0.5 ms spread). On the specular-only channel (`direct_specular` against a 2 × 2
no-filter reference) the footprint lowers flicker away from edges from 1.49
to 1.33. A geometric specular filter from the barycentric derivatives of the
vertex normals (Tokuyoshi and Kaplanyan 2019) raised silhouette flicker
there (9.49 to 9.99) and was not kept.

The tiled pipeline has not changed. A Bistro case with a skinned or
procedurally moving object has not been captured, so deformation remains an
open motion contract; the previous skinned positions that motion blur reads
take the in-flight predecessor without a barrier of their own.

## Alternatives considered

Filmic SMAA T2x (Call of Duty, 2016) adds an exponential history filter, which
brings back long history. Decima's FXAA with two-frame TAA publishes no
rejection details, and FXAA is softer than SMAA. Keeping the plan's overlay
rejection made every transparent surface alternate between subsamples in a
still image; the blend path does write rigid motion, so identity-matched
overlays keep history.

For SMAA 4x's second sample:

- **Shading every sample everywhere** (sample-rate shading of the full
  G-buffer and lighting) doubles the deferred cost; outside triangle edges
  both samples see the same surface.
- **Shading at every triangle change** gave the best quality (still frame
  10.16, moving 11.12 with instability 7.71) but cost 2.62 ms in sample-1
  lighting. **Shading only where the draws differ or depth jumps by 2 %**
  was cheap but worse (10.36): railings a few centimetres before a wall
  share its draw, and both SMAA runs then filter sample 0's image with
  different area rows. A plane test with the gradient from a neighbour on
  the same triangle matched every-triangle quality at 1.69 ms; dense meshes
  rarely give one. The chosen same-draw gradient costs 0.58 ms for 10.26.
- **Per-sample bloom, depth of field and motion blur** would duplicate the
  post chain; bloom is additive and composes exactly, and the blurs disable
  S2x instead.
- **The full-screen sample-1 kernels** with an edge-mask early-out cost
  2.15 ms at 2560 × 1440: nearly every wave keeps some edge lane. The list
  costs what the listed pixels cost.

## Revisit when

New animation or motion producers, or moving-image fixtures, expose missing
signals or unacceptable rejection or ghosting; or the resolve's velocity weight
scale of 30, tuned by the reference at lower resolutions, rejects too much at
2560 × 1440. For SMAA 4x: when its frame cost (above) outweighs its measured
gain on a target scene, or when sample 1 needs its own fog, overlays,
transmission or layered materials.

## Implementation

[`vkr_temporal.c`](../../renderer/src/vkr_temporal.c),
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)
(`vkr_vk_prepare_temporal_transform`, `vkr_vk_prepare_temporal_resolve`,
`vkr_vk_prepare_smaa_*`, `vkr_vk_prepare_vbuffer_split`,
`vkr_vk_prepare_msaa_compose`, the `sample_one` paths of
`vkr_vk_prepare_deferred_gbuffer` and `vkr_vk_prepare_deferred_lighting`),
[`vkr_vulkan_draws.c`](../../renderer/src/vulkan/vkr_vulkan_draws.c)
(`vkr_vk_prepare_vbuffer_split_depth`),
[`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)
(`vk_temporal_resolve`, `vk_vbuffer_split`, `vk_vbuffer_split_depth_fragment`,
`vk_msaa_compose`, `vk_msaa_side_pixel`), [`smaa.slang`](../../renderer/src/shaders/vulkan/slang/post/smaa.slang)
and [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
