---
status: declined
updated: 2026-09-05
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
ship the experimental second classify/raster/HZB phase. The prior bounded Metal
experiment deferred zero candidates in its moving-camera workload, so there was
no deferred population to offset the extra work.

That observation justified declining the experiment for that workload; it is
not a claim that all scenes or devices produce zero candidates. No current
production predictor implementation is implied. The original 2026-08-24 Metal
Bistro orbit observation used two repetitions and 600 measured frames; it was
local, dirty-tree, and warmup-unstable. Its report digest is
`sha256:f54a508cbb1adac7233738b682cb6ac12c017b1bb263e595eaf8d0671e3b5f0e`.
The exact transient command was not retained, and the removed predictor cannot
be reproduced from the current binary.

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

[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c)
and [`vkr_metal_packet_commands.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_commands.inc)
retain the production history gates.
