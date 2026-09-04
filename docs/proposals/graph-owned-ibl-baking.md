---
status: proposed
updated: 2026-09-05
authority: proposal
---

# Graph-owned IBL baking

IBL source conversion, prefiltering, and SH projection ship, but their resource
accesses remain outside the authored graph. The architecture status records the
same gap: bake work is explicitly barriered rather than graph-declared.

## Current implementation baseline

The render graph describes typed compute dispatch in
[vkr_render_graph.h](../../lib/src/renderer/vkr_render_graph.h). Vulkan
currently records IBL dispatches and image barriers directly in
[vkr_vulkan_ibl.c](../../lib/src/renderer/vulkan/vkr_vulkan_ibl.c), while its
graph executor invokes that pending-work recorder from
[vkr_vulkan_graph.c](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c).
Publication already bounds pending bake jobs and carries their ownership in
[vkr_vulkan_publisher.c](../../lib/src/renderer/vulkan/vkr_vulkan_publisher.c).

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
