---
status: declined
updated: 2026-10-07
authority: adr
---

# ADR-032: Keep exact one-phase visibility gates

## Status

Declined: two-phase experiment; the one-phase production decision remains in force.

## Context

Previous depth is unsafe as a final occlusion proof under camera or occluder
motion. A predictor/confirmation design could recover reuse by confirming
predicted occlusion against current depth, but adds raster and reduction work.

## Decision

Keep ADR-028's one-phase path and exact history compatibility gates. Do not
ship the experimental second classify/raster/HZB phase. The prior bounded
experiment on the Metal desktop implementation, removed on 2026-10-06, deferred
zero candidates in its moving-camera workload, so there was
no deferred population to offset the extra work.

That observation justified declining the experiment for that workload; it is
not a claim that all scenes or devices produce zero candidates. No current
production predictor implementation is implied. The original 2026-08-24 Bistro
orbit observation on that implementation used two repetitions and 600 measured
frames; it was local, dirty-tree, and warmup-unstable. Its report digest is
`sha256:f54a508cbb1adac7233738b682cb6ac12c017b1bb263e595eaf8d0671e3b5f0e`. The
exact transient command was not retained, and the removed predictor cannot be
reproduced from the current binary.

A Vulkan upper bound (RX 6700 XT, driver 26.6.3, Release, local and
non-authoritative, one run each, 2026-10-07) used a temporary patch that built
HZB under TAA jitter and accepted history from any camera and jitter phase,
with no confirmation pass, so it could hide newly visible geometry. On the
moving Bistro orbit (`bistro_shadow_orbit`) the stale HZB rejected 1,726 of
2,317 opaque draws (p50), but the rejected draws are small: at 2560×1440
`VBuffer.Opaque` fell from 0.576 to 0.423 ms while the HZB build added about
0.14 ms, and the GPU pass sum went from 14.98 to 15.00 ms. At 1280×720 the
pass sum fell 0.07 ms. Reports
`sha256:f3abae63cd90c291b3d24d909967556e73ff24445187acdc1547ac875b1a7ae5`,
`sha256:bf76d0bb3b1e7d3c186818efba8fa7b4d4b2386db26c35fce33ee8d052c80fd5`,
`sha256:f8a32b6bc8a88f41020bb97b3a0402340ec6938b479d16f439fe826d1540aee5` and
`sha256:e75bfe557559956f8e82b22db80c1a61d9eebba5da46d6376bb86fbac232787f`.
A confirmation raster would add cost to that bound, so camera occlusion culling
at instance granularity does not pay for Bistro on the desktop pipeline.

## Consequences

Motion often disables historical occlusion, but visible geometry is preserved.
A future predictor must first demonstrate a nonzero useful deferred set and
must confirm final rejections using valid current-frame depth.

## Alternatives considered

Dropping history gates risks disocclusion holes. Always running a second phase
adds work without an established benefit.

## Revisit when

A materially different candidate population establishes both useful prediction
and a matched Release net benefit with unchanged visibility.

## Implementation

[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c)
and [`vkr_metal_packet_commands.inc`](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc)
retain the production history gates.
