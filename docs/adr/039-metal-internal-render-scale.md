---
status: implemented
updated: 2026-10-16
authority: adr
---

# ADR-039: Separate internal Scene and physical output extents

## Status

Accepted.

## Context

Pixel-scaled Scene work can dominate a high-DPI frame. Reducing the physical
target would also reduce UI resolution and change capture/picking semantics.
The tiled pipeline's adaptive quality
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 12) also needs a
Scene scale that changes between frames.

## Decision

Accept a finite Metal Scene scale in `(0,1]`; zero selects unit scale for API
callers. `vkr_renderer_set_render_scale` changes it between frames within the
range `vkr_renderer_render_scale_range` reports. Only the Metal tiled pipeline
scales the Scene. Vulkan rejects non-unit scale and always renders the Scene at
unit scale; Vulkan FSR 3.1, which scaled it temporally, was removed
([ADR-052](052-vulkan-fsr31-upscaling.md)). The default render scale is 1.0 on
Vulkan and 0.6 on Metal, where MetalFX upscales the tiled Scene
([ADR-121](121-tiled-temporal-upscaling.md)). Keep native physical output and UI extent separate from
the Scene presentation extent: whole target in direct mode or the dock-owned
panel in editor mode.

Spatial mode rounds Scene output dimensions times scale to the nearest
internal pixel, with a minimum of one. Viewport-domain graph resources use
that internal extent. On Metal the MetalFX temporal scaler rebuilds the
internal HDR at the output extent (ADR-121); with the spatial upscaler, or a
frame outside MetalFX's eligibility, the tiled pipeline's tonemap pass samples
the internal HDR and upscales it spatially. UI composes afterward at native
resolution.

Picking maps physical target/panel coordinates to the internal extent using the
same authoritative viewport mapping as camera aspect, gizmos and composition.
A scale or extent change recreates the viewport-sized graph images and restarts
their histories; on the tiled pipeline these are the cloud and HZB histories.
MetalFX keeps its history across adaptive-quality steps by reading the
viewport from capacity-sized inputs (ADR-121). ADR-087 (decision 12) owns
dynamic scale changes. The harness includes scale and
both extents in workload identity and capture/report metadata.

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

[`vkr_renderer.c`](../../renderer/src/vkr_renderer.c),
[`vkr_viewport.c`](../../renderer/src/vkr_viewport.c),
[`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc), and
[`tonemap.metal`](../../renderer/src/shaders/metal/msl/post/tonemap.metal).
