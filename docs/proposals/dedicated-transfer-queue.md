---
status: proposed
updated: 2026-09-05
authority: proposal
---
# Dedicated transfer queue

## Current baseline

Vulkan resource publication uses bounded staging allocations and records upload
work in the active frame command path. The graph has transfer passes for packet
uploads, and staging retirement follows submitted work. Metal has its own
publication path. No selected renderer creates or synchronizes a dedicated
transfer queue family.

## Proposed change

Evaluate a Vulkan transfer-queue path only if an observed load or frame cost
identifies the active graphics queue as the limiting resource. The path would
own a transfer command ring, staging retirement values, ownership transitions,
and a dependency consumed by graphics before any resource is published for
drawing.

## Decision boundaries

- Decide whether this is a Vulkan-only capability or requires a portable upload
  scheduling boundary with a Metal counterpart.
- Define queue-family ownership transfers and semaphore/timeline ordering for
  images and buffers.
- Preserve current bounded upload bytes, cancellation, and completion-gated
  resource publication.

## Evidence needed

Compare matched Release workloads with the existing publication path and report
queue utilization, upload latency, frame time, staging retention, and visual
output. Test a device with a distinct transfer family and one without it. No
throughput benefit is assumed before those results exist.

## Code baseline

- [Vulkan publication](../../lib/src/renderer/vulkan/vkr_vulkan_publisher.c)
- [Vulkan transfer graph passes](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c)
- [resource finalization](../../lib/src/renderer/systems/vkr_resource_system.c)
- [Metal publication](../../lib/src/renderer/metal/vkr_metal_packet_renderer.m)
