---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-023: One explicit Vulkan capability floor

## Status

Accepted.

## Context

A Vulkan version number does not prove that a device supports the resource model
and synchronization required by the selected renderer.

## Decision

Select Vulkan 1.4 on Windows only after querying, reporting and enabling the
required feature/limit profile in `vkr_vulkan_device.c`. Require buffer device
address, shader 64-bit integers/draw parameters, independent blending, timeline
semaphores, descriptor indexing and runtime arrays, scalar layout, host query
reset, dynamic rendering, synchronization2, maintenance features and shader
demotion used by production. Require a graphics/compute/transfer queue family.
Windowed targets additionally require surface and swapchain support.

`VK_EXT_descriptor_buffer` is mandatory. Resource and sampler descriptors use
two separately checked descriptor-buffer bindings; sampled images and samplers
are separate descriptors. Native descriptor sizes remain inside host layout and
writer code. Shaders index arrays with logical indices and never interpret
native descriptor bytes. Unsupported devices fail initialization with capability
diagnostics; there is no legacy descriptor-set renderer fallback.

Swapchain maintenance is optional and supplies present fences when available.
The baseline completion proof is described in ADR-009. Memory pooling and slot
publication follow ADR-024. The removed Vulkan 1.2 backend is not a supported
compatibility path; Linux is not enabled by current implementation selection.

## Consequences

The implementation can assume its initialized capability contract. Vulkan 1.4
support alone, including a MoltenVK version report, does not establish usability.
Target-device coverage must be measured separately from source presence.

## Alternatives considered

Per-material descriptor sets would restore a different resource model. An
unimplemented descriptor-heap extension or another renderer is not a fallback.
Making swapchain maintenance mandatory would reject otherwise usable targets.

## Revisit when

A concrete supported-device requirement changes the floor, or a replacement
binding mechanism has SDK, driver, shader and native validation evidence.

## Implementation

[`vkr_vulkan_device.c`](../../lib/src/renderer/vulkan/vkr_vulkan_device.c),
[`vkr_vulkan_resources.c`](../../lib/src/renderer/vulkan/vkr_vulkan_resources.c), and
[`vkr_renderer_impl.c`](../../lib/src/renderer/vkr_renderer_impl.c).
