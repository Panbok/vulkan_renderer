---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Mesh & UI Follow-Up Plan

## Context
The initial mesh refactor introduced `vkr_mesh_manager`, per-mesh pipeline resolution, and sharing of geometry/material/pipeline resources. Remaining work items fall into three buckets: public APIs for engine consumers, data-driven mesh instancing, and a dedicated UI draw path that avoids the full 3D mesh overhead.

## Goals
1. Expose mesh lifecycle helpers on the renderer/frontend so game code can create/destroy/update meshes without touching internal arrays.
2. Introduce a resource- or scene-driven path for building `VkrMeshDesc` objects (from mesh/scene assets) to replace the hardcoded default scene.
3. Design a lightweight UI draw/item system that coexists with, but is not forced through, the 3D mesh pipeline.

## Work Streams

### 1. Renderer Mesh APIs
- Add `vkr_renderer_add_mesh`, `vkr_renderer_remove_mesh`, `vkr_renderer_set_mesh_material`, `vkr_renderer_get_mesh` wrappers that call into `vkr_mesh_manager`.
- Ensure these APIs return handles/IDs that higher-level systems can store instead of raw indices.
- Update application/default scene/demo code to call the renderer wrappers rather than the manager directly.
- Document thread-safety expectations (likely “main thread only” for now) and error reporting strategy.

### 2. Data-Driven Mesh Instancing
- Define a `VkrMeshResource` or scene block format that contains geometry/material references, initial transform, and optional shader override.
- Extend the resource system with a loader stub that can parse this data (could just be JSON or a simple text format initially).
- Add helper(s) to convert loaded data into `VkrMeshDesc` records and call the renderer mesh APIs.
- Plan for reference counting: geometry/material handles acquired through the loader must release when the mesh is destroyed.
- Optional stretch: integrate with scene graph/culling structures once basic loading works.

### 3. UI Draw System
- Define a `VkrUiDrawItem` structure (position, size, depth, texture, color) that is independent from `VkrMesh`.
- Decide on batching strategy (e.g., single dynamic vertex buffer per frame, sprite atlas support).
- Create a small UI renderer that:
  - Maintains its own pipeline/descriptor state (still using shader system/pipeline registry).
  - Builds vertex/index data each frame (or on demand) and issues one or a few draw calls.
- Bridge with existing UI material if helpful, but allow UI-specific shaders/resources.
- Ensure the UI renderer exposes simple APIs (e.g., `vkr_ui_draw_quad`) for future UI layers.

## Sequencing
1. Implement renderer-facing mesh APIs and refactor existing call sites (frontend/application) to use them.
2. Prototype a basic mesh resource loader and hook it into the resource system; integrate with demo scene to spawn meshes from data.
3. Design and implement the UI draw path once mesh resource loading is stable.

Each stream can be tackled independently, so we can schedule them based on priority. This plan should be revisited after completing each stream to capture new insights or requirements.
