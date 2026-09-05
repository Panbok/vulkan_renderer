---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-039: Separate internal Scene and physical output extents

## Status

Accepted.

## Context

Pixel-scaled Scene work can dominate a high-DPI frame. Reducing the physical
target would also reduce UI resolution and change capture/picking semantics.

## Decision

Accept a finite Metal Scene scale in `(0,1]` at initialization; zero selects
unit scale for API callers. Vulkan rejects non-unit scale. Keep native physical
output and UI extent separate from the Scene presentation extent: whole target
in direct mode or the dock-owned panel in editor mode.

Round Scene output dimensions times scale to the internal render extent, with a
minimum of one pixel. Viewport-domain graph resources use that internal extent.
Spatial reconstruction samples internal HDR during final tonemap/composition;
FXAA offsets use output pixels and UI composes afterward at native resolution.
Portable TAA stays same-resolution within the internal Scene domain.

Picking maps physical target/panel coordinates to the internal extent using the
same authoritative viewport mapping as camera aspect, gizmos and composition.
Extent/scale transitions invalidate temporal history. Dynamic scale changes are
owned separately by ADR-040. The harness includes scale and both extents in
workload identity and capture/report metadata.

## Consequences

Scaling changes Scene quality and workload; it is not equivalent-work optimization.
Paneled mode has three extents: physical target, Scene panel output and internal
Scene render. Native UI detail remains independent.

## Alternatives considered

Scaling the drawable conflates Scene quality with DPI/UI. Calling spatial linear
sampling temporal reconstruction would misstate the implementation.

## Revisit when

Another backend gains an explicitly accepted scaling mode or quality/cost
measurements justify a different spatial filter.

## Implementation

[`vkr_renderer.c`](../../lib/src/renderer/vkr_renderer.c),
[`vkr_viewport.c`](../../lib/src/renderer/vkr_viewport.c),
[`vkr_metal_packet_graph.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_graph.inc), and
[`tonemap.metal`](../../lib/src/renderer/shaders/metal/msl/post/tonemap.metal).
