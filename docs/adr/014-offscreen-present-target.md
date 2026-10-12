---
status: implemented
updated: 2026-10-11
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
submission completion and final presentation behavior. The renderer consumes
capabilities and frame setup through `VkrRendererImpl`.

A Vulkan target created without a requested image count, as the editor's and
the app's windows are, requests the renderer's present-target count
(`caps.present_target_image_count`, three by default) at creation, the count
a window resize recreates it with (2026-10-11,
`vkr_renderer_backend_initialize` in
[`vkr_renderer.c`](../../renderer/src/vkr_renderer.c)). Before, the first
swapchain took the surface's minimum, often two images, until the first
resize; with two images under FIFO, a frame whose recording plus GPU time
misses one refresh waits for the next, which can halve the rate. The surface's
minimum and maximum still clamp the request
([`vkr_vulkan_target.c`](../../renderer/src/vulkan/vkr_vulkan_target.c)).
Vsync selects FIFO and its absence IMMEDIATE, which falls back to FIFO where
the surface lacks it. Metal keeps its fixed three-image capability and
ignores requested counts.

Offscreen targets own ordinary images and advance through a bounded target ring.
They use the same packet, authored graph and capture request path as windowed
targets. Capture is asynchronous and explicitly released; capacity pressure
returns busy rather than overwriting an owned result.

Physical output extent is separate from internal Scene extent and, in the
editor, the Scene panel rectangle. ADR-039 owns the Metal render scale,
ADR-087 decision 12 the Metal dynamic resolution, and ADR-043 output transfer.

## Consequences

Harness output exercises production passes while bypassing window presentation.
`vkr_editor --headless` (ADR-075) uses the same target for scripted editor runs.
Offscreen execution does not validate window resize, DPI, compositor or WSI
synchronization. The device information's target image count reports the
requested count, so a Vulkan window created without one reports zero, not
the swapchain's actual count. No native Vulkan run has checked the windowed
three-image request yet.

## Alternatives considered

Hidden-window-only tests depend on a desktop compositor. A separate headless
renderer would duplicate rendering behavior.

## Revisit when

A new target kind requires a different ownership or completion contract.

## Implementation

[`vkr_renderer.h`](../../renderer/src/vkr_renderer.h),
[`vkr_renderer_impl.h`](../../renderer/src/vkr_renderer_impl.h),
[`vkr_vulkan_target.c`](../../renderer/src/vulkan/vkr_vulkan_target.c), and
[`vkr_capture_ring.c`](../../renderer/src/vkr_capture_ring.c).
