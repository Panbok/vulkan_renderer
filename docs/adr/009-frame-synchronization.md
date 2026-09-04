---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-009: Separate submission and presentation completion

## Status

Accepted.

## Context

A completed graphics submission does not alone prove that presentation has
finished consuming its binary wait semaphore. Command slots, target images and
history resources therefore have different reuse domains.

## Decision

Use the shared monotonic submit ring for bounded frame slots and retirement.
Vulkan signals a timeline semaphore; Metal maps completion to native submission
completion and shared-event ordering. Reuse a slot only after its last submit
completes. Report a slot wait instead of removing that proof.

Vulkan window targets own render-complete semaphores per swapchain image.
When swapchain maintenance is supported, present fences prove presentation
completion. The base path uses a completed submit that consumed the reacquired
image's acquire semaphore. Reacquire return alone is insufficient. Successor
swapchain completion participates in collecting retired window targets.

Target recreation, shutdown and explicit lifecycle operations may wait idle.
Normal successful frames do not wait the whole device. Cancellation must retire
recorded-but-unsubmitted uses and resolve an acquired target through the native
implementation. Temporal/capture/retained-content owners additionally track the
actual producer and last consumer; command-slot index is not history identity.

## Consequences

Bounded overlap is safe across different target-image counts. Slot pressure can
still block the CPU, and presentation retirement needs its own proof.

## Alternatives considered

Frame-slot binary present semaphores conflate lifetimes. Unconditional device
idle removes overlap. Reacquisition without an acquire-wait completion fails to
prove the presentation wait finished.

## Revisit when

Another queue, presentation API or target model changes the completion proof.

## Implementation

[`vkr_gpu_submit_ring.c`](../../lib/src/renderer/vkr_gpu_submit_ring.c),
[`vkr_vulkan_wsi.c`](../../lib/src/renderer/vulkan/vkr_vulkan_wsi.c),
[`vkr_vulkan_target.c`](../../lib/src/renderer/vulkan/vkr_vulkan_target.c), and
[`vkr_metal_packet_frame.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_frame.inc).
