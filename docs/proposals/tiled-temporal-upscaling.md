---
status: proposed
updated: 2026-10-16
authority: proposal
---

# Tiled temporal upscaling: remaining work

MetalFX temporal upscaling is the tiled pipeline's default since 2026-10-16
([ADR-121](../adr/121-tiled-temporal-upscaling.md)), which owns the decision,
the implementation and the measurements that chose it. This proposal holds
the work it leaves open and the risks for features added later. Each item
needs its own Bistro evidence when it is taken up.

## Open work

- **Reactive mask for blended surfaces.** `Tiled.Blend` draws glass and other
  alpha-blended surfaces into the colour input without their own depth or
  motion, so MetalFX reprojects them with the opaque surface behind them.
  Reflections and refraction on glass move differently from that surface and
  can smear. Write a reactive mask for these pixels (`reactiveMaskTexture`,
  enabled with `reactiveMaskTextureEnabled`) so MetalFX favours the current
  frame there, or draw such surfaces after the upscale at output resolution.
  Measure Bistro's windows in motion first.
- **Timing on a base M1.** The hardware floor is the base M1
  ([ADR-083](../adr/083-supported-hardware-matrix.md)). MetalFX cost about
  4 ms at 1440p output on the M1 Pro; on a base M1 it is unmeasured and could
  cost about twice as much (estimate). If it misses the budget, a lower
  default scale or the spatial upscaler is the base M1's tier.
- **Custom material graphs.** Their samples take no texture mip bias, so
  they stay blurrier than standard materials under MetalFX.
- **HZB occlusion history.** The history requires an unchanged raster
  view-projection (`vkr_metal_packet_select_hzb_history` in
  [`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc)).
  Jitter changes the raster matrix every frame, so the history is never used.
  This costs only with a still camera. The fix would test the history
  against the unjittered matrix with a conservative bound for the jitter.
- **Frames outside MetalFX.** Orthographic views and the editor's inspection
  render modes keep the spatial tonemap. Under MetalFX they draw one sample
  per pixel, so their edges are not anti-aliased. Jitter-free SMAA or a
  temporary 4× sample count for those views would restore it.
- **Auto exposure and pre-exposure.** The scaler enables
  `autoExposureEnabled` and also receives the frame's pre-exposure. How the
  two interact was not checked separately; the image rounds use manual
  exposure.

## Risks for later features

- **Particles and blended effects.** The renderer has no particle system
  today. Particles write no depth or motion, so MetalFX would reproject them
  with the surface behind them: they ghost, smear or fade out. Mitigations:
  the reactive mask above, or compositing small bright effects after the
  upscale.
- **Lighting changes over static geometry.** Moving lights and shadows, a
  changed lamp and a time-of-day change alter shading without motion, and
  the history lags behind the change. Mitigations: a reactive mask from the
  lighting change, or a reset on large global changes. Measure with the
  tiled dynamic lights and a moving shadow caster.
- **Scrolling and animated textures.** None exist today. Their texels move
  without geometry motion and would smear. Such a feature must provide
  motion for the texture or write the reactive mask.
- **Disocclusion trails.** At a low internal scale, newly revealed pixels
  have few samples, and MetalFX can leave trails behind moving objects and
  at screen edges. The Bistro crowd fixture
  ([`bistro_tiled_crowd_local.scene.json`](../../assets/scenes/fixtures/bistro_tiled_crowd_local.scene.json))
  measures them; re-measure when the default scale or floor changes.
- **MetalFX is a black box.** Its history, rejection and sharpening are
  internal to the framework and cannot be inspected or changed. If a later
  effect cannot be made to work with it, a custom temporal upscaler is the
  fallback. It would own its history and rejection and would need its own
  cost measurement.
