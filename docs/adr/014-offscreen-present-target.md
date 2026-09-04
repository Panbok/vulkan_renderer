---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-014: Window and offscreen targets share frame submission

## Status

Accepted.

## Context

Automated rendering needs the production frame path without requiring a visible
window. A fake swapchain would preserve unnecessary WSI assumptions.

## Decision

Select window or offscreen presentation at renderer initialization. Each native
implementation owns acquisition/advancement, image count, formats, recreation,
submission completion and final presentation behavior. The frontend consumes
capabilities and frame setup through `VkrRendererImpl`.

Offscreen targets own ordinary images and advance through a bounded target ring.
They use the same packet, authored graph and capture request path as windowed
targets. Capture is asynchronous and explicitly released; capacity pressure
returns busy rather than overwriting an owned result.

Physical output extent is separate from internal Scene extent and, in the
editor, the Scene panel rectangle. ADR-039/040 own Metal reconstruction and
ADR-043 owns output transfer.

## Consequences

Harness output exercises production passes while bypassing window presentation.
Offscreen execution does not validate window resize, DPI, compositor or WSI
synchronization.

## Alternatives considered

Hidden-window-only tests depend on a desktop compositor. A separate headless
renderer would duplicate rendering behavior.

## Revisit when

A new target kind requires a different ownership or completion contract.

## Implementation

[`vkr_renderer.h`](../../lib/src/renderer/vkr_renderer.h),
[`vkr_renderer_impl.h`](../../lib/src/renderer/vkr_renderer_impl.h),
[`vkr_vulkan_target.c`](../../lib/src/renderer/vulkan/vkr_vulkan_target.c), and
[`vkr_capture_ring.c`](../../lib/src/renderer/vkr_capture_ring.c).
