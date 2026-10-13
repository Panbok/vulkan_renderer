---
status: partial
updated: 2026-10-13
authority: adr
---

# ADR-037: SMAA T2x temporal antialiasing

## Status

Accepted (partial): SMAA T2x on the desktop pipeline (Vulkan) since
2026-10-13, replacing portable scene-linear TAA and FXAA (owner decision,
[Short-history rendering](../proposals/short-history-rendering.md),
section 2). Deformation and broader motion signals remain open. The tiled
pipeline keeps multisampling without temporal history
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
  in proportion to clip w.
- **SMAA 1x on each subsample.** After exposure, grading and the GT7
  transform write the display-linear target (ADR-043), three compute passes
  run edge detection (luma of the square-root-encoded colour), blending
  weights with diagonal and corner detection, and neighbourhood blending, which
  stores `sqrt(5·|v|)` of the antialiased motion in alpha. The area
  (160 × 560 R8G8) and search (64 × 16 R8) tables upload once at device
  creation as permanent sampled rows.
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
  sampling alike.

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
overlays keep history. MSAA for the visibility buffer remains a separate
proposal.

## Revisit when

New animation or motion producers, or moving-image fixtures, expose missing
signals or unacceptable rejection or ghosting; or the resolve's velocity weight
scale of 30, tuned by the reference at lower resolutions, rejects too much at
2560 × 1440.

## Implementation

[`vkr_temporal.c`](../../renderer/src/vkr_temporal.c),
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)
(`vkr_vk_prepare_temporal_transform`, `vkr_vk_prepare_temporal_resolve`,
`vkr_vk_prepare_smaa_*`),
[`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)
(`vk_temporal_resolve`), [`smaa.slang`](../../renderer/src/shaders/vulkan/slang/post/smaa.slang)
and [`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
