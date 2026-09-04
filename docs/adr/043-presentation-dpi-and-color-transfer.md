---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-043: Physical-pixel presentation with one sRGB transfer

## Status

Accepted.

## Context

Logical window dimensions, physical output pixels and internal Scene pixels
are distinct. Gamma encoding in shaders plus sRGB attachments applies transfer
twice; blending encoded UI colors gives a different result from linear blending.

## Decision

Window targets expose physical client pixels. Windows establishes Per-Monitor
V2 before window creation, handles monitor DPI changes and updates content scale.
The editor/UI convert logical sizing before layout under ADR-036. Scene output
and internal rendering use ADR-039's explicit mapping.

Keep HDR lighting and post effects linear through ACES-fitted tonemap. Metal and
Vulkan final shaders emit linear RGB into sRGB window/offscreen attachments;
attachment conversion performs the output encode. Authored UI/text colors decode
from sRGB once on the CPU before blending. Texture color/data intent is resolved
at loading/publication, not repaired by extra material shader gamma powers.

Direct mode tonemaps to the target. Editor mode tonemaps/composites the Scene
rectangle and draws native-resolution UI afterward. Output-space FXAA stays in
the final draw, with offsets expressed in output pixels. HDR/intermediate and
final-color captures are different contracts and must be compared accordingly.

## Consequences

Output transfer is shared while native surface formats differ. Correct source
transfer does not prove mixed-DPI interaction, translucent fixtures or final-color
baseline acceptance. Offscreen rendering cannot validate monitor transitions.

## Alternatives considered

Shader gamma plus sRGB attachment encoding is double conversion. Scaling the
physical target to reduce Scene cost also degrades native UI. Blending authored
sRGB values directly treats encoded values as linear.

## Revisit when

HDR display output, another surface format or a new DPI/presentation platform is
authorised with explicit transfer and coordinate semantics.

## Implementation

[`vkr_color_transfer.c`](../../lib/src/renderer/vkr_color_transfer.c),
[`vkr_platform_windows.c`](../../lib/src/platform/vkr_platform_windows.c),
[`vkr_window_windows.c`](../../lib/src/platform/vkr_window_windows.c),
[`vkr_vulkan_target.c`](../../lib/src/renderer/vulkan/vkr_vulkan_target.c), and
[`tonemap.metal`](../../lib/src/renderer/shaders/metal/msl/post/tonemap.metal).
