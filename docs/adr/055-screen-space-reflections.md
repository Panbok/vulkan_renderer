---
status: declined
updated: 2026-10-12
authority: adr
---

# ADR-055: Opaque screen-space reflections

## Status

Declined (2026-10-12). Screen-space reflections (SSR) shipped on the desktop
pipeline, which Vulkan implements, and were removed on 2026-10-12 by the
owner's decision in
[Short-history rendering](../proposals/short-history-rendering.md), section 1.
The tiled pipeline never had SSR
([ADR-087](087-gpu-class-graphics-pipelines.md)). This record keeps the
reasons, so the design is not tried again without addressing them.

## Context

Saved scene probes supply stable specular light outside the current view.
SSR was added to supply visible local geometry and motion on opaque surfaces
with perceptual roughness at most 0.6, and to keep the probes where the
screen could not establish a hit.

## Decision (removed)

Each covered pixel traced its own ray through a current-frame depth pyramid
at full source resolution, with at most 48 hierarchy decisions (24 in the
opt-in `balanced` tier). Coated pixels traced the coat lobe and uncoated
pixels the base lobe ([ADR-062](062-layered-clearcoat.md)). Rays sampled the
GGX lobe and were reused across a neighbourhood. History stored incoming
radiance and was reprojected through the reflected hit, not the receiver,
with identity, plane and depth tests, a short no-hit hold and a cubic
history fetch. `SSR.Composite` added the result to the deferred HDR image,
and the TAA resolve held a separate settling history mode for 128 matching
frames ([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)).

## Why it was removed

The reflections stayed noisy and produced temporal artifacts after repeated
fixes between 2026-09 and 2026-10-12: trails behind moving reflected
objects, blinking sparse hits on rough walls, discs on glossy receivers and
holes where rays ran out of decisions behind occluders. Each fix added
history rules (settling caps, reflected-hit reprojection, no-hit holds,
rough zero samples) and GPU cost: in local, non-authoritative Release
profiles of 2026-10-12 on an RX 6700 XT, `SSR.Temporal` alone took about
1.5 ms at 1280×720, against about 0.7 ms before the 2026-10-11 rework. The owner
decided that the renderer must not hide stochastic shading behind long
temporal history.

## Consequences

On the desktop pipeline, reflections come only from the global environment
and box-projected reflection probes
([ADR-019](019-bounded-forward-spatial-lighting.md),
[ADR-103](103-probe-and-volume-authoring.md)), scaled by baked sky light
([ADR-054](054-baked-diffuse-volumes.md)). The coat lobe reads the same
probes as the base lobe. Visible local geometry, moving objects and
surfaces that a probe does not cover no longer reflect. Planar reflections
for water are proposed work ([Planar reflections](../proposals/planar-reflections.md)).

The SSR sources, graph passes, capture channels `ssr_depth`, `ssr_raw` and
`ssr_reflection`, the `post.screen_history.state` metric, the
`VKR_SSR_DISABLED` and `VKR_SSR_QUALITY` variables and the SSR cases,
fixtures, checks and verification files were removed with the feature. The
graphics setting `screen_space_reflections` is a retired key that still
loads and has no effect. Git keeps the old record and its evidence.

## Removal evidence

Windows, RX 6700 XT, Vulkan desktop pipeline, 2026-10-12, local and
non-authoritative (dirty tree). The removal of SSR, SSGI and FSR 3.1
([ADR-060](060-screen-space-diffuse-indirect-lighting.md),
[ADR-052](052-vulkan-fsr31-upscaling.md)) was checked as follows:

- `build_test.bat` (Debug): every CPU suite passed, including the main
  graph's pass counts (378 at every feature maximum, 394 with every cascade
  composed, 372 at 1280×720).
- Vulkan validation (Debug, `VK_LAYER_KHRONOS_validation` with
  synchronization checks): `vkr_harness profile --case
  tools/cases/local/p21_vulkan_bistro_windowed_validation.case.json
  --profile tools/profiles/local-windowed-single.json` passed with no
  validation message, TAA on
  (`sha256:77f840870fb2997da1cc0d1a21ddb4faf1f3dff43c650731635a6ade6cc06db1`).
- Release `vkr_harness snapshot` with `local-offscreen.json` of two Bistro
  views without TAA, the street camera of
  `decals_bistro_street_baseline_capture` at 1920×1080 and the indoor camera
  of `local_shadow_bistro_indoor_capture` at 1280×720, capturing
  `final_color`, `depth`, `gbuffer_normal`, `gbuffer_diffuse` and
  `gbuffer_specular`. Against commit 9c6c2e1f with SSR and SSGI off, every
  channel of both views is byte-identical. Against 9c6c2e1f with SSR and
  SSGI on, depth and the G-buffer are identical and `final_color` differs
  by more than 2 of 255 on 23.7 % (street) and 33.4 % (indoor) of pixels,
  by more than 16 on 1.4 % and 7.5 %, at most 104 and 190: the light the
  two effects added.

## Alternatives considered

Shorter history only shortened the misplaced trails, and receiver-motion
reprojection followed the receiver instead of the reflected feature. Probe
audit and normalization, per-room probes and planar water reflections give
stable results without per-pixel history; the owner chose them.

## Revisit when

Probes cannot supply a required reflection of local or moving geometry, and
a candidate design produces a stable result from the current frame and at
most one previous frame, measured on Bistro for flicker and frame cost.
