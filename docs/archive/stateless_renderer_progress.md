---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../rendering/stateless_renderer/stateless_renderer_spec.md`](../rendering/stateless_renderer/stateless_renderer_spec.md). Retained for history; do not treat as current.
# Stateless Renderer Implementation Progress

Spec: stateless_renderer_spec.md
Last updated: 2026-02-05

This document tracks implementation progress by phase. Each phase includes the
specific implementation details and file touches that landed in-tree.

## Phase 1: Packet Foundations + Render Graph Wiring
Status: completed

Implementation details (completed):
- Added `lib/src/renderer/vkr_render_packet.h` with full packet schema, payload
  structs, and `VKR_RENDER_PACKET_VERSION` for submit-time validation.
- Introduced `VkrMeshHandle` alias for mesh instance handles used by draw items.
- Added `VkrValidationError` for submit-time diagnostics.
- Wired render graph packet attachment via `vkr_rg_set_packet()` and payload
  accessors (`vkr_rg_pass_get_*_payload()`).
- Stored the packet pointer on `VkrRenderGraph` and cleared it on frame begin,
  frame end, and after `vkr_rg_execute()` to prevent retention.

Files touched:
- `lib/src/renderer/vkr_render_packet.h`
- `lib/src/renderer/vkr_render_graph.h`
- `lib/src/renderer/vkr_render_graph.c`
- `lib/src/renderer/vkr_render_graph_internal.h`
- `lib/src/renderer/vkr_rg_execute.c`

## Phase 2: Pass Executors Consume Packet Payloads
Status: completed

Implementation details (completed so far):
- Added shared packet draw helpers in `lib/src/renderer/passes/vkr_pass_packet_draw.c`
  for instance upload, mesh resolution, draw range selection, and pipeline
  resolution.
- Rewrote `pass.world`, `pass.ui`, and `pass.editor` executors to consume packet
  payloads directly and submit draws via cached resources (no view calls).
- UI/editor passes now derive orthographic projection from packet frame sizes.
- Implemented stateless `pass.shadow.cascade` to consume shadow payloads, upload
  instance data, bind shadow pipelines, and apply alpha cutoff + diffuse sampler
  for cutout draws.
- Frontend now initializes `VkrShadowSystem` directly and uses its config to
  size render graph shadow resources.
- Implemented stateless `pass.skybox` via `vkr_skybox_system_render_packet()`,
  using packet globals and payload cubemap while reusing skybox resources
  (payload material is not yet consumed).
- World pass now pulls shadow frame data from `VkrShadowSystem` and overrides
  the shadow map handle from the render graph, keeping shadow uniforms synced
  with packet-driven cascades.
- Implemented stateless `pass.picking` executor using the picking system's
  render target and readback path, driven by packet payloads or world draws.
- Added render-graph pass flag support (JSON `flags`) and marked picking as
  `NO_CULL` to keep the request-driven pass alive in the graph.
- Draw-item pipeline overrides are now honored in world, UI, and editor passes
  via `vkr_pass_packet_resolve_pipeline()`.

Implementation details (remaining):
- None.

## Phase 3: Frontend Cutover + App Packet Build
Status: completed

Implementation details (completed so far):
- Added stateless frame API entry points (`vkr_renderer_prepare_frame`,
  `vkr_renderer_submit_packet`) with packet validation, render-graph execution,
  and frame teardown.
- Added `VkrFrameSetup` to carry swapchain image/format/size from prepare to
  packet construction.
- Added submit-time validation for payloads, instance ranges, pipeline overrides,
  and picking bounds.
- Application loop now builds a minimal packet (world + shadow + picking + UI +
  skybox) and submits via the stateless APIs; view world update was removed.
- Added stateless text creation APIs (`vkr_renderer_create_ui_text`,
  `vkr_renderer_create_world_text`) and routed app UI text creation through the
  new API.
- Added per-frame scratch scope and queued UI/world text updates into the packet
  path to keep update strings alive until submit.
- Submit now syncs `rf->globals` from packet data and applies UI offscreen sizing
  based on packet viewport/editor flags.
- UI/world/picking passes now render persistent text via the view-backed text
  systems to keep text visible in the stateless path.
- Added packet support for non-instance meshes (generation==0 handles) and
  extended world/shadow/picking passes to resolve mesh submeshes and draw ranges.
- Packet build now includes mesh-manager meshes alongside instances so scene
  shapes render in stateless mode.
- Packet build now sorts transparent draws by camera distance to preserve
  back-to-front blending in the stateless path.
- Cutout classification now checks diffuse-texture transparency, avoiding
  transparent-pass rendering for fully opaque textures.
- Added editor viewport resources (`vkr_editor_viewport.*`) to own the
  viewport display shader/pipeline/material and a quad mesh for compositing.
- Editor pass now supports non-instance mesh handles and updates the viewport
  material diffuse texture from the render graph offscreen color target.
- Application builds editor payloads using the editor viewport resources and
  packet-provided viewport mapping, and resizes cameras to match target size.
- Application/editor picking now uses the shared viewport mapping utilities
  (`vkr_viewport.*`) instead of view-editor state.
- Editor view registration was removed from renderer initialization to avoid
  duplicate pipeline/material setup during stateless compositing.
- Removed view-system state from the renderer frontend and deleted the legacy
  view modules. Introduced stateless resource owners:
  `vkr_world_resources` (world pipelines + 3D text), `vkr_ui_system` (UI text),
  and `vkr_skybox_system` (skybox).
- Renderer init/shutdown now initializes these systems and no longer registers
  view layers. Resize/offscreen sizing routes through `vkr_ui_system`.
- World/UI/picking passes and the picking system now render text via the new
  stateless systems; skybox pass renders via `vkr_skybox_system`.
- Scene text3d creation/update/destruction now targets `vkr_world_resources`
  directly; shadow system invalidation was removed.

Implementation details (remaining):
- None.

## Phase 4: Debug Payload + Validation Hardening
Status: completed

Implementation details (completed):
- Wired `VkrGpuDebugPayload` into render-graph execution to gate GPU timing
  queries and optional pass timestamp capture (disabled by default unless
  the packet requests it).
- Hardened packet validation for text updates to reject non-zero counts with
  NULL update lists.

Files touched:
- `lib/src/renderer/vkr_rg_execute.c`
- `lib/src/renderer/renderer_frontend.c`

## Phase 5: Legacy Scene Text3D Cleanup
Status: completed

Implementation details (completed):
- Removed unused scene-local Text3D instance storage and legacy init/render/destroy
  helpers from the scene system, keeping world-resources text ownership as the
  only path.
- Dropped the unused `query_text3d` compiled query field.
- Updated scene text3d documentation/comments to refer to world-resources
  ownership instead of the removed view layer.

Files touched:
- `lib/src/renderer/systems/vkr_scene_system.h`
- `lib/src/renderer/systems/vkr_scene_system.c`
