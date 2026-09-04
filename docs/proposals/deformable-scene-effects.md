---
status: proposed
updated: 2026-09-05
authority: proposal
---

# Deformable scene effects

VKR has no authored effect, tag, deformation, or particle system. The legacy
effects plan therefore does not describe a partially integrated feature.

## Current implementation baseline

Scene entities own mesh-instance association through
[the scene system](../../lib/src/renderer/systems/vkr_scene_system.h), while
[mesh instances](../../lib/src/renderer/systems/vkr_mesh_manager.h) reference
immutable published geometry. The packed-geometry contract states that routing
and deformation change only through the publisher in
[vkr_resources.h](../../lib/src/renderer/resources/vkr_resources.h).

Compute is already production infrastructure: the graph has typed compute
passes in [vkr_render_graph.h](../../lib/src/renderer/vkr_render_graph.h), and
the Vulkan graph maps GPU culling, deferred shading, temporal, post, and HZB
work to compute executors in
[vkr_vulkan_graph.c](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c). A new
effect must not treat compute support as a prerequisite.

## Proposed gap

Investigate one authored vertex-deformation pilot, such as a wind-driven cloth
asset, before adding an effect registry or a general tag vocabulary. The pilot
must define its data owner, source geometry, output buffer lifetime, and the
common deformed input used by visibility, shadow, deferred resolve, temporal
motion, and picking. It must preserve GPU completion before a deformed buffer
or its source geometry is reused.

## Unsettled decisions

- Whether the attachment belongs to a scene entity, a mesh instance, or an
  authored mesh range.
- Whether deformation writes bounded transient geometry, persistent
  per-instance state, or is evaluated in the raster/resolve shaders.
- How authored parameters and stable previous-frame deformation state enter the
  packet without widening unrelated draw rows.
- Which first asset exposes a renderer problem that justifies the added memory,
  synchronization, and shader surface.
