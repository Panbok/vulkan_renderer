---
status: proposed
updated: 2026-09-30
authority: proposal
---

# Graph-owned IBL baking

IBL source conversion, prefiltering, and SH projection ship, but their resource
accesses remain outside the authored graph. The architecture status records the
same gap: bake work is explicitly barriered rather than graph-declared.

## Current implementation baseline

The render graph describes typed compute dispatch in
[vkr_render_graph.h](../../renderer/src/vkr_render_graph.h). Both backends
queue bakes at publication and record them from the `IBL.Bake` executor with
backend-owned dispatches and barriers. Vulkan records them in
[vkr_vulkan_ibl.c](../../renderer/src/vulkan/vkr_vulkan_ibl.c), invoked from
[vkr_vulkan_graph.c](../../renderer/src/vulkan/vkr_vulkan_graph.c), and bounds
the queue in
[vkr_vulkan_publisher.c](../../renderer/src/vulkan/vkr_vulkan_publisher.c).
Metal queues them in
[vkr_metal_packet_resources.inc](../../renderer/src/metal/internal/vkr_metal_packet_resources.inc)
and records them in
[vkr_metal_packet_record.inc](../../renderer/src/metal/internal/vkr_metal_packet_record.inc).

## Proposed gap

Make one IBL bake's source, destination mips, accesses, and completion
publication visible to graph compilation without turning IBL into a per-frame
pass. The graph must retain the present queued-job model, preserve backend-owned
pipeline encoding, and reject or defer a job when its declared resource set
cannot be realized.

## Unsettled decisions

- Whether a queued bake is represented as a dynamic graph instance or as a
  bounded graph-owned auxiliary job list.
- How source and destination cube faces and mip transitions map onto the graph's
  resource and subresource model.
- Which layer owns job cancellation and completion-safe publication after a
  failed submit or target recreation.
- Whether both backends can share the declaration shape while keeping their IBL
  encoder implementation private.
