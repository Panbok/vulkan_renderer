---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-033: Optional occupied-depth shadow fitting

## Status

Accepted.

## Context

Camera near/far splits can spend shadow resolution on empty depth. GPU depth
can guide fitting only if its frame, projection and completion are known.

## Decision

Offer graph-declared occupied-depth SDSM reduction on Metal and Vulkan as an
explicit opt-in. Reduce rendered camera depth into occupied range/count data,
copy it through bounded readback, and publish only after its submit completes.
Attach source frame, world/projection and extent metadata to each sample.

The CPU shadow system validates compatibility, smooths accepted ranges and
falls back to fixed splits when feedback is absent, stale, empty or invalid.
Consumption does not block for a fresh GPU result. Fixed splits remain the
production default; enabling feedback changes the workload and must be recorded.

This is separate from cascade retention: feedback may change cascade fits and
therefore invalidate reuse. It does not provide shadow-caster occlusion or a
second-phase camera visibility proof.

## Consequences

Completed delayed feedback can concentrate resolution but adds reduction and
cascade work. The original measured Metal choice retained fixed defaults;
source parity does not establish equal device cost.

## Alternatives considered

Synchronous readback stalls the frame. Unconditional SDSM adds work even when
quality or cost does not improve. Consuming stale depth without source metadata
can fit the wrong view.

## Revisit when

A representative quality/cost comparison justifies enabling it by default.

## Implementation

[`vkr_shadow_system.c`](../../lib/src/renderer/systems/vkr_shadow_system.c),
[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c),
[`vkr_vulkan_renderer.c`](../../lib/src/renderer/vulkan/vkr_vulkan_renderer.c), and
[`vkr_metal_packet_frame.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_frame.inc).
