---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../rendering/render-graph-design.md`](../rendering/render-graph-design.md). Retained for history; do not treat as current.
# View-Layer System Refactor (Deprecated)

The view/layer system has been removed. Render orchestration now uses the
render graph and the stateless packet API:

- Build a `VkrRenderPacket` per frame.
- Submit via `vkr_renderer_prepare_frame()` + `vkr_renderer_submit_packet()`.
- Pass executors (`pass.world`, `pass.shadow.cascade`, `pass.ui`, `pass.editor`,
  `pass.skybox`) consume packet payloads.
- Stateless resource owners back the passes (`vkr_world_resources`,
  `vkr_ui_system`, `vkr_skybox_system`).

For current architecture details, see:
- `docs/rendering/render-graph-design.md`
- `docs/rendering/stateless_renderer/stateless_renderer_spec.md`
