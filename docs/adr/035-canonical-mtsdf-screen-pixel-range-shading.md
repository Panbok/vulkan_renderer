---
status: implemented
updated: 2026-10-18
authority: adr
---
# ADR-035: Derivative-based MTSDF coverage

## Status

Accepted.

## Context

CPU-derived text ranges vary with projection and do not describe the MTSDF
reconstruction contract carried by cooked atlases.

## Decision

For MTSDF draws, the packet carries an atlas-derived two-component `unit_range`.
The Metal and Vulkan text shaders derive screen texel size from UV derivatives,
compute the screen pixel range, take the median RGB distance, and produce
coverage from that value. Bitmap text continues to use sampled alpha. UI
blends the resulting coverage; the picking variants discard below their shared
coverage threshold.

The cooked MTSDF loader accepts only a linear, single-page RGBA8 MTSDF atlas.
Its clamp-to-edge, linear, non-mipmapped sampling stays fixed when the sample's
scene-texture filtering controls change.
No alpha-SDF blend fallback is implemented.

World text and its picking variant draw with a depth bias toward the camera,
`VKR_WORLD_TEXT_DEPTH_BIAS_CONSTANT` (256 units of the float depth's last
mantissa bit) and `VKR_WORLD_TEXT_DEPTH_BIAS_SLOPE` (2), on both backends
(since 2026-10-18). A label placed on a surface then wins its depth test
instead of fighting the surface as the temporal jitter moves the samples: in
the Level Design Test scene six signs lie in their plates' planes, and the
freight elevator sign lost letters frame to frame. At 5 m the constant part
moves the text under 0.3 mm.

Bootstrap atlas range is selected for the smallest UI text at 1x display scale;
see [ADR-034](034-offline-cooked-font-artifacts.md). Clamping a too-small range in
the shader does not recover distance information missing from the cooked field.

## Consequences

Atlas range is a per-atlas input, not an authored text-size or output-size
guess. Both shader implementations and their packet lowering must keep the
same component ordering and MTSDF flag meaning. Font atlas compression or mip
generation would violate this contract unless the shader and artifact policy
are revised together.

## Alternatives considered

`fwidth(median)` and CPU scalar ranges are approximations that cannot preserve
the same projected reconstruction rule for UI and world text.

## Revisit when

An accepted visual comparison requires an alpha-SDF fallback, multi-page atlas
binding, or signed-distance effects such as outlines and glow.

## Code evidence

- [frame text draw](../../renderer/src/vkr_frame_input.h)
- [Metal text shader](../../renderer/src/shaders/metal/msl/text/default.metal)
- [Vulkan text shader](../../renderer/src/shaders/vulkan/slang/text/default.slang)
- [Metal world text draws](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc), [Vulkan world text draws](../../renderer/src/vulkan/vkr_vulkan_draws.c)
- [cooked-atlas validation](../../runtime/src/renderer/resources/loaders/cooked_font_loader.c)
