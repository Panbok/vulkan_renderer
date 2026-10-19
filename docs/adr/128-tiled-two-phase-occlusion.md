---
status: implemented
updated: 2026-10-20
authority: adr
---

# ADR-128: Tiled two-phase camera occlusion

## Status

Accepted. The tiled pipeline's one-sample path (MetalFX) culls the camera in
two phases since 2026-10-20. The spatial upscaler's four-sample path, a frozen
culling camera and the overdraw view keep
[ADR-032](032-two-phase-confirmed-visibility.md)'s exact one-phase gates. The
desktop pipeline does not change.

## Context

[ADR-032](032-two-phase-confirmed-visibility.md) keeps exact history gates: a
pyramid is used only for a bit-identical camera, raster grid and world, and
never in a frame with skinning. MetalFX jitters every frame, so the tiled
pipeline built and read no pyramid at all, and a moving camera culled nothing
by occlusion. ADR-032 asks for a useful deferred set and a measured net benefit
before a second phase ships.

A level-design recording (`rec-20261019-231623`, Level Design Test, indoor
rooms) spent 4.3 ms of about 10 ms of pass time in `Tiled.Opaque` and drew up
to 22,000 commands per frame. The pass cost followed covered pixels and the
draw count. A stale-pyramid probe (any history, no confirmation; local,
non-authoritative) bounded the gain: on that level at 1146×668 `Tiled.Opaque`
fell from 4.18 to 2.32 ms mean, on the Bistro orbit at MetalFX 0.5 from 5.23
to 4.60 ms p50. On a tile-based GPU hidden-surface removal already skips most
hidden fragments, so the saving is the vertex and tiling work of hidden draws.

## Decision

1. **When.** The Metal backend sets `tiled_two_phase_occlusion` when HZB
   culling is on, the opaque pass draws one sample, the frame renders a scene,
   no culling camera is frozen and the view is not the overdraw view
   ([`vkr_metal_packet_prepare_frame_state`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)).
   Shared preparation leaves it false. A two-phase frame always builds its
   pyramid.
2. **Early phase.** `Cull.Classify` tests the camera's candidates against the
   newest submitted pyramid of the same extent, whatever camera, jitter or world
   built it. A hidden candidate takes the classification
   `VKR_METAL_GPU_DRAW_DEFERRED` instead of zero; encode skips it. When that
   pyramid's command buffer was not known complete at preparation, the classify
   waits for the queue's earlier dispatches, as `Clouds.Trace` does. The newest
   submitted pyramid makes the deferred set depend on the camera path alone,
   so fixed-step runs keep a deterministic work volume.
3. **Early pass.** `Tiled.Opaque` draws the camera view's ranges into cleared
   colour, sun light and depth and stores them; it leaves the clear sky and the
   resolve to the late pass. `HZB.BuildBase` and `HZB.BuildMip.*` then build
   this frame's pyramid from the early depth. It is also the next frame's
   history.
4. **Late view.** A late occlusion view follows every other culling view
   ([`vkr_render_graph_late_view`](../../renderer/src/vkr_render_graph.h)). It
   takes the camera's frustum, flags and LOD policy and its own Custom
   compaction table with the camera's ready masks. `Cull.Late` runs three
   dispatches over one root: the classify kernel tests only deferred candidates
   against this frame's pyramid with the frame's jittered view-projection;
   `vkr_metal_packet_gpu_draw_prefix_late` places the late view's ranges after
   the early commands in its ICB group's pool; the encode kernel fills them.
5. **Late pass.** `Tiled.Opaque.Late` loads the early pass's images, draws the
   late view's ranges, then the clear sky and the resolve.
6. **Deformed draws.** Occlusion never rejects a candidate whose instance has a
   deformation address, since its bounds need not hold its pose. A skinned mesh
   therefore no longer disables occlusion for the whole two-phase frame.
7. **Other consumers.** `Picking.Tiled` draws both views' ranges. The motion
   view tests no pyramid in two phases, so object motion covers every dynamic
   draw the late view may add. The camera draw counts add the late view's;
   `visibility.hzb.rejected` reports the late view's final rejections.

## Consequences

- A draw is dropped only when this frame's early depth hides it, so camera
  motion, jitter and occluder changes cannot remove visible geometry. Sub-pixel
  coverage between depth samples stays as conservative as the one-phase test.
- Two-phase frames pay one load of the opaque pass's colour, sun light and
  depth, the pyramid build and the late cull: on the Bistro orbit at 1280×720
  `Tiled.Opaque.Late` measured 0.14 ms, `Cull.Late` 0.04 ms and the pyramid
  0.13 ms.
- The classify can wait for the previous frame's dispatches, which reduces
  overlap between frames. The measured frame cost still fell.
- Per-pass timestamps attribute this frame structure differently: on the
  Bistro orbit the `Shadow.Local.*` rows rose about 0.1 ms each while total
  GPU frame cost fell. Use `gpu.submission` for frame cost.
- Late Custom draws whose pipelines are still compiling are not added to the
  late-Custom diagnostic count, which reads only the camera view's table.

## Alternatives considered

- **Stale pyramid without confirmation.** Cheaper, but it hides geometry a
  camera turn reveals for at least a frame. Used only as the upper-bound probe.
- **Newest completed pyramid.** Avoids the queue wait, but the deferred set
  then depends on GPU timing; the Bistro profile reported
  `determinism.work_volume_mismatch`.
- **Splitting the four-sample pass.** It would store every sample between the
  passes instead of keeping them in tile memory. Not measured.

## Revisit when

A scene's late phase defers so much that the late pass approaches the early
one, or the frame structure changes so that the queue wait costs more than the
saved vertex work. Revisit the four-sample path if it becomes a default.

## Evidence

Local, dirty-tree, non-authoritative; Apple M1 Pro, Release, validation
variables unset, 2026-10-20. Off is the same binary with `VKR_HZB_DISABLED=1`,
which under MetalFX builds and reads no pyramid, as before this change.

- CPU: `vulkan_renderer_tester` passed all 111 suites;
  `test_tiled_graph_topology` builds and compiles a two-phase frame and checks
  `Tiled.Opaque` < `HZB.Build*` < `Cull.Late` < `Tiled.Opaque.Late` <
  `Clouds.Trace`.
- Bistro image: `vkr_harness snapshot` of a local MetalFX orbit case at frame
  90 (`local-windowed`). On against off: PSNR 74.48 dB, largest difference
  12/255. Off against off: 74.15 dB, 13/255. Reports
  `sha256:1030ef0b14a258f7aca22b7825bf9f188dccbd6f22d2a6595e576656150083d3`
  (on) and
  `sha256:1d9620cd29a318274339e8d2e5d34a6fb76f4ffe2abdd27e2b078e61d23a0906`
  (off).
- Bistro cost: the same orbit at 1280×720 (MetalFX 0.5) with
  `local-windowed-gpu-submission-single`, three alternating pairs.
  `gpu.submission` p50 off 11.35, 11.05, 11.00 ms; on 10.81, 10.88,
  10.63 ms. Draw commands 2,483 → 505.
- Level Design Test: headless editor drives of `rec-20261019-231623`
  (`rec.drive` with `follow`) at 1146×668, MetalFX, dynamic resolution off,
  two runs each. `gpu.submission` mean 9.77, 9.74 → 8.30, 8.33 ms; p50 9.87,
  9.85 → 7.97, 7.97 ms; p95 12.58, 12.46 → 12.39, 12.39 ms.
  `Tiled.Opaque` mean 4.29, 4.27 → 2.26, 2.27 ms. Draw commands p50
  10,096 → 430. Seven captures after seeks, which jump the camera: on against
  off 39.0–42.8 dB, off against off 39.1–47.2 dB, and no missing objects in the
  difference images.
- Unavailable: authoritative clean-tree profiles; Vulkan, which this change
  does not run.

## Implementation

[`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal),
[`vkr_metal_packet_frame.inc`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc),
[`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc),
[`vkr_metal_packet_record.inc`](../../renderer/src/metal/internal/vkr_metal_packet_record.inc),
[`tiled.rendergraph.json`](../../assets/render_graphs/tiled.rendergraph.json).
