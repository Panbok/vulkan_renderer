---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../rendering/render-graph-design.md`](../rendering/render-graph-design.md). Retained for history; do not treat as current.
# Layer/Event Communication Refactor (Deprecated)

The legacy view/layer system and its messaging APIs are removed. All rendering
is now packet-driven and orchestrated via the render graph.

Current pathways:
- Application builds a `VkrRenderPacket` per frame.
- Pass executors consume packet payloads.
- Stateless resource owners manage persistent GPU state:
  `vkr_world_resources`, `vkr_ui_system`, `vkr_skybox_system`.
- Viewport mapping and picking use `vkr_editor_viewport` +
  `vkr_viewport` utilities.

See:
- `docs/rendering/render-graph-design.md`
- `docs/rendering/stateless_renderer/stateless_renderer_spec.md`
