---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Mesh Resource Refactor Plan

## Current State & Pain Points
- `lib/src/renderer/renderer_frontend.c:757-901` builds the default scene by hand-crafting three `VkrMesh` structs. Each mesh owns its own cube geometry (`rf->cube_geometry_*`), material (`rf->world_material_*`), and pipeline handle (`rf->world_pipeline_*`).
- `VkrMesh` is just a POD defined in `lib/src/renderer/resources/vkr_resources.h:159-166`; there is no API to create/destroy meshes or to keep the backing systems in sync when multiple meshes should share handles.
- The geometry system already tracks geometries by name (`geometry_by_name` hash), but nothing exposes lookups, so higher-level code re-creates identical CPU/GPU buffers.
- Materials can be re-acquired by name through `vkr_material_system_acquire`, but the renderer never retains or reuses a handle when instantiating the same mesh definition more than once.
- Pipelines are created per-mesh even when the shader config is identical, and meshes store the resolved handle permanently. Runtime code (`vkr_renderer_draw_frame`) still calls `vkr_pipeline_registry_get_pipeline_for_material` and rebinds when needed, so the pre-created per-mesh pipelines are redundant bookkeeping.

## Goals
- Introduce a mesh creation/lifecycle API so mesh instances can share geometry, material, shader, and pipeline state safely.
- Allow meshes to be described declaratively (geometry/material names or handles, optional pipeline/shader override, transform) and have the renderer resolve/reuse the underlying resources.
- Ensure geometry/material/pipeline systems expose the queries and ref-count updates needed for sharing, and that we stop duplicating GPU buffers or pipelines for meshes that only differ by transform.
- Make renderer teardown/reload release the correct references (geometry/material handles, pipeline instance state) when meshes are removed.

## Architecture Overview
1. **Resource-level sharing**
   - Geometry system: expose public helpers to lookup/create geometries by name and to retain/release them (`vkr_geometry_system_acquire_by_name`, `vkr_geometry_system_get_by_handle`).
   - Material system: leverage existing acquire/release API when meshes are spawned or destroyed.
   - Pipeline registry: rely on `vkr_pipeline_registry_get_pipeline_for_material` (shader name + pipeline_id) to lazily pick a pipeline instead of storing bespoke instances per mesh; cache the resolved handle inside the mesh with a “dirty” flag.
2. **Mesh description & lifecycle**
   - Add `VkrMeshDesc` (descriptor with geometry/material identifiers, optional shader/pipeline override, parent transform/initial transform, instance count).
   - Provide `vkr_mesh_build_from_desc(RendererFrontend*, const VkrMeshDesc*, VkrMesh*)` which resolves handles, acquires references, and grabs a pipeline instance state from the registry once.
   - Provide `vkr_mesh_release(RendererFrontend*, VkrMesh*)` that releases geometry/material handles, frees the pipeline instance state, and resets local state.
3. **Renderer integration**
   - Replace the manual mesh array writes in `vkr_renderer_default_scene` with calls to `vkr_renderer_add_mesh(RendererFrontend*, const VkrMeshDesc*, uint32_t *out_index)`.
   - Drop duplicated members in `RendererFrontend` (`world_pipeline_2/3`, `world_material_2/3`, `cube_geometry_2/3`). Store a single shared cube geometry + materials loaded via the resource system, and instantiate multiple meshes referencing the same handles.
   - Extend `RendererFrontend` API with helpers to change a mesh’s material/geometry at runtime, marking the cached pipeline as dirty so `vkr_renderer_draw_frame` can reacquire the correct instance state.

## Implementation Plan

### 1. Strengthen Geometry/Material/Pipeline Systems for Reuse
1.1 **Geometry lookup helpers**
   - Add `VkrGeometryHandle vkr_geometry_system_acquire_by_name(VkrGeometrySystem*, String8 name, bool8_t auto_release, VkrRendererError *out_error)` using the internal `geometry_by_name` map (file: `lib/src/renderer/systems/vkr_geometry_system.c/.h`).
   - Add `VkrGeometry *vkr_geometry_system_get_by_handle(VkrGeometrySystem*, VkrGeometryHandle)` so higher-level code can query metadata (vertex size, bounds) when deciding compatibility with shader configs.
   - Update creation paths to set `auto_release` appropriately (shared meshes should set `auto_release=false` and release explicitly via the mesh lifecycle).

1.2 **Material ref-count convenience**
   - Verify `vkr_material_system_acquire` / `release` semantics are used by mesh creation/destruction. Add small inline helpers if needed (e.g., `vkr_material_system_try_get_by_name`) to avoid duplicating hash lookups inside the renderer (file: `lib/src/renderer/systems/vkr_material_system.h/.c`).

1.3 **Pipeline cache coordination**
   - Clarify that `vkr_pipeline_registry_get_pipeline_for_material` already aliases pipelines by shader name (`lib/src/renderer/systems/vkr_pipeline_registry.c:835-878`). Mesh creation should not pre-create dedicated handles; instead, store a cached `resolved_pipeline` plus a `bool pipeline_dirty` flag in `VkrMesh`.
   - Provide a helper `vkr_renderer_mesh_resolve_pipeline(RendererFrontend*, VkrMesh*)` that (re)acquires the instance state when the resolved pipeline changes and keeps `m->instance_state` synchronized.

### 2. Introduce Mesh Descriptor & Lifecycle API
2.1 **Extend `VkrMesh`/resource definitions**
   - In `lib/src/renderer/resources/vkr_resources.h`, add:
     - `typedef struct VkrMeshDesc { VkrGeometryHandle geometry; String8 geometry_name; VkrMaterialHandle material; String8 material_name; const char *shader_name_override; uint32_t pipeline_domain; VkrTransform transform; }`.
     - Additional runtime fields in `VkrMesh` (e.g., `VkrPipelineHandle resolved_pipeline; bool pipeline_dirty; bool owns_geometry/material;`) so the renderer knows what to release.

2.2 **Mesh lifecycle functions (Mesh Manager)**
   - Introduce a dedicated mesh manager subsystem under `lib/src/renderer/systems/vkr_mesh_manager.{h,c}` with the `vkr_mesh_manager_*` namespace.
   - Mesh manager state:
     - Owns an `Array_VkrMesh` and a `uint32_t mesh_count`, mirroring the previous renderer fields but encapsulated in the subsystem.
     - Keeps a freelist of vacant slots so removing meshes does not require shifting arrays (simple `Array_uint32_t free_indices` is sufficient).
     - Stores a pointer/reference to `RendererFrontend` so it can call into geometry/material/pipeline systems when acquiring or releasing resources.
   - API surface:
     - `bool8_t vkr_mesh_manager_init(vkr_mesh_manager* mgr, RendererFrontend* rf, uint32_t max_mesh_count)` initializes the arrays and freelist.
     - `bool8_t vkr_mesh_manager_shutdown(vkr_mesh_manager* mgr)` releases remaining meshes (calling the release helper below) and destroys buffers.
     - `bool8_t vkr_mesh_manager_add(vkr_mesh_manager* mgr, const VkrMeshDesc*, uint32_t* out_index)` resolves handles, increments ref counts, acquires pipeline instance state, and stores the mesh in the managed array.
     - `void vkr_mesh_manager_remove(vkr_mesh_manager* mgr, uint32_t index)` releases geometry/material handles, returns the slot to the freelist, and decrements `mesh_count`.
     - `bool8_t vkr_mesh_manager_set_material(vkr_mesh_manager* mgr, uint32_t index, VkrMaterialHandle)` to swap materials at runtime (handles acquire/release + pipeline dirty flag).
     - `VkrMesh* vkr_mesh_manager_get(vkr_mesh_manager* mgr, uint32_t index)` for the renderer draw loop.

2.3 **Instance state management**
   - Inside `vkr_mesh_manager_add`, after resolving the initial pipeline, call `vkr_pipeline_registry_acquire_instance_state` once and store the handle on the mesh.
   - Provide `void vkr_mesh_manager_refresh_pipeline(vkr_mesh_manager* mgr, uint32_t index)` that releases and reacquires the instance state when `pipeline_dirty` is true (used by renderer loop).

### 3. Integrate with Renderer Frontend
3.1 **Renderer struct cleanup**
   - Remove duplicate per-mesh handles from `RendererFrontend` (`renderer_frontend.h`). Keep only shared defaults (`default_cube_geometry`, `default_world_material`, `ui_geometry`, `ui_material`).
   - Track new mesh management helpers (e.g., free-list for `rf->meshes` or an `Array_uint32_t free_mesh_indices` for removal).

3.2 **Default scene rewrite**
   - In `vkr_renderer_default_scene`:
     - Create one cube geometry (`rf->default_cube_geometry`) and one shared world material, plus any alternate materials you want to demonstrate.
     - Use `vkr_renderer_add_mesh` three times with descriptors that reuse the same geometry handle but vary transforms (and optionally materials). No more per-mesh pipeline creation.
     - Ensure UI quad uses the same API (one geometry, one material, one pipeline) instead of bespoke fields when possible.

3.3 **Draw loop adjustments**
   - Update `vkr_renderer_draw_frame` to work with new `VkrMesh` fields:
     - Early-out if `geometry` or `material` handles are invalid (the mesh manager should prevent this, but guard anyway).
     - Call `vkr_renderer_mesh_resolve_pipeline` before binding; the helper handles pipeline transitions and instance-state reacquisition.
     - Since meshes now share pipelines, track stats per pipeline domain rather than per mesh.

3.4 **Shutdown / removal**
   - Update `vkr_renderer_destroy` to iterate meshes and call `vkr_renderer_remove_mesh` so geometry/material handles, pipeline instance states, and reference counts all return to their systems cleanly.

### 4. Resource Loading & Future Mesh Assets
4.1 **Resource system hookup**
   - Plan for a mesh/geometry loader that outputs `VkrMeshDesc` or directly registers geometries (`.geo`, `.obj`, etc.) so `vkr_renderer_add_mesh` can be driven from data rather than hardcoded.
   - Extend the resource system (if needed) with a `VKR_RESOURCE_TYPE_GEOMETRY` loader so that mesh descriptors can request named geometry via `vkr_resource_system_load` and share the handle automatically.

4.2 **Editor/runtime workflows**
   - Document how to spawn many mesh instances of the same asset: load geometry + material once via the resource system, then call `vkr_renderer_add_mesh` repeatedly with different transforms.
   - Provide hooks to stream/unload mesh instances (evicting geometry when the last reference releases).

### 5. Validation & Testing
5.1 **Unit/regression steps**
   - Add assertions in mesh creation/destruction to verify reference counts (geometry/material) never drop below zero.
   - Instrument `vkr_geometry_system` and `vkr_pipeline_registry` logs to confirm the default scene only creates one cube geometry and one world pipeline even when three meshes are alive.
   - Exercise `app/src/main.c` to rotate the shared cube meshes and ensure transforms update while the underlying handles stay identical.
5.2 **Manual runtime checks**
   - Confirm GPU memory usage drops (single vertex/index buffer for the cube) and that hot-reloading a material updates all meshes sharing that handle.
   - Toggle material assignment at runtime (using the new setter) to verify pipeline dirtying works and descriptor sets rebind only when necessary.

Delivering the above will give GPT-5.1-Codex a concrete checklist: add the missing system APIs, implement the mesh lifecycle helpers, refactor the renderer to call them, and verify sharing works end-to-end.
