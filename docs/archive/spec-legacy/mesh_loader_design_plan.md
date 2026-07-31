---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Mesh Loader Design & Implementation Plan

## Context
- Loaders registered in `lib/src/renderer/renderer_frontend.c:678-683` currently cover textures, materials, and shader configs. `VkrMeshManager` (`lib/src/renderer/systems/vkr_mesh_manager.{h,c}`) only exposes manual creation via `VkrMeshDesc`.
- The resource system (`lib/src/renderer/systems/vkr_resource_system.{h,c}`) dispatches loads to registered loaders. We need a loader equivalent to `shader_loader.*`/`material_loader.*` that can parse `assets/models/*.obj` + companion `.mtl` files and emit usable mesh data.
- Reference implementation `stuff/mesh_loader.c` demonstrates how `.obj` meshes are parsed, triangulated, deduplicated, and prepared for runtime consumption (including `.ksm` binary caching). We should port the working behavior but mark binary caching as future work.
- Geometry utilities required for deduplication/tangent generation already exist in `lib/src/renderer/systems/vkr_geometry_system.c` (see `vkr_geometry_system_deduplicate_vertices` around lines 891-945 and `vkr_geometry_system_generate_tangents` around 784-873). Filesystem helpers live in `lib/src/filesystem/filesystem.c`, and string helpers exist in `lib/src/containers/str.c`.
- Materials are authored in custom `.mt` files (example: `assets/materials/default.world.mt`). The `lib/src/renderer/resources/loaders/material_loader.c` implementation expects this format. `.obj` files reference `.mtl` files; our loader must synthesize `.mt` files (or equivalent buffers) to feed the material loader so meshes render with the intended textures/colors.

## Objectives
1. **Refactor first:** Introduce `VkrSubMesh`, update `VkrMesh` to hold only transform/model + a submesh array, and migrate all existing code paths (mesh manager APIs, renderer frontend draw loop) to operate on the new structure before touching loader work.
2. Adapt `vkr_mesh_manager` to create/manage/destroy meshes composed of submeshes, including per-submesh resource ownership, pipeline refresh, and runtime setters.
3. Update `vkr_renderer_draw_frame` (and other mesh iterators) to loop over meshes → submeshes for rendering, ensuring per-mesh transforms and per-submesh resources are applied correctly.
4. After the refactor is stable, add a mesh loader that parses Wavefront `.obj` + `.mtl` files from `assets/models`, converts them into geometry/material resources, and instantiates meshes through the mesh manager.
5. Provide a declarative loader API (via the resource system) so higher-level code can call `vkr_mesh_manager_load(...)` with a path, letting the loader handle parsing, GPU uploads, material generation, and `VkrMesh` creation.
6. Reuse existing subsystems (`filesystem`, `str`, `Arena` scratch management, `VkrGeometrySystem`, `VkrMaterialSystem`, `VkrAllocator`, `VkrTransform`) without inventing bespoke parsing helpers whenever possible.
7. Support multiple submeshes (`o`/`g` blocks or `usemtl` changes) per `.obj`, associating per-submesh materials and transforms, so scenes like `falcon.obj` and `sponza.obj` render faithfully.
8. Defer binary mesh cache generation (similar to `.ksm` in the reference loader) to a future phase but outline where the hooks would live.

## Phase 1: Mesh/SubMesh Refactor (Pre-loader)

### Submesh-Oriented Mesh Representation
- Extend `lib/src/renderer/resources/vkr_resources.h`:
  - Introduce `typedef struct VkrSubMesh { ... }` that contains the fields currently on `VkrMesh` except for the transform/model (geometry/material handles, pipeline + instance state, shader override, ownership flags, `last_render_frame`, `pipeline_domain`, `model` etc.).
  - Replace `VkrMesh` fields with:
    ```c
    typedef struct VkrMesh {
      VkrTransform transform;
      Mat4 model;
      Array_VkrSubMesh submeshes;
    } VkrMesh;
    ```
    The mesh manager’s arena owns the submesh arrays.
- Keep transforms at the mesh level so every submesh references the same `model`
  matrix, ensuring grouped pieces move in sync (matches how OBJ subsets share a
  parent transform).
- Update descriptors:
  - `VkrMeshDesc` carries per-mesh transform/model plus an array of `VkrSubMeshDesc` entries (geometry/material handles or names, override strings, pipeline domain).
  - Resource loader output (`VkrMeshLoaderResult`) will convert each `.obj` subset into a `VkrSubMeshDesc`.
- Rendering code (`vkr_renderer_draw_frame`) iterates meshes -> submeshes; per-mesh transforms are applied once, while submeshes drive geometry/material binding.

### Mesh Manager Adjustments
- Update `VkrMeshDesc` to accept a list/array of `VkrSubMeshDesc` entries. Each `VkrSubMeshDesc` includes handles/names, shader override string, pipeline domain, and ownership flags.
- `vkr_mesh_manager_add/create` should allocate the `Array_VkrSubMesh` from the manager arena, copy descriptors, and acquire per-submesh geometry/material handles with proper ref counts.
- Maintain helper APIs:
  - `uint32_t vkr_mesh_manager_submesh_count(const VkrMesh *mesh);`
  - `VkrSubMesh *vkr_mesh_manager_get_submesh(VkrMeshManager*, uint32_t mesh_idx, uint32_t submesh_idx);`
  - Per-submesh setters (`set_material`, `set_pipeline_domain`, `set_shader_override`).
- Update removal/release paths to iterate submeshes, releasing geometry/material references and freeing pipeline instance states individually.

### Renderer Frontend & Draw Loop
- Refactor `vkr_renderer_draw_frame` (and any other traversal such as debug views) to:
  1. Pull mesh model matrices once per mesh.
  2. Iterate each submesh to resolve pipelines/materials, bind the geometry, and issue draw calls.
- Ensure pipeline dirtiness, descriptor updates, and per-instance uniform buffer writes happen per submesh while still batching by render pass if possible.
- Update any telemetry (e.g., descriptor writes avoided, draw call counters) to account for submesh granularity.

### Refactor Validation Gate
- Only proceed to loader work once:
  - Mesh manager unit/integration tests confirm submesh acquire/release semantics.
  - Renderer default scene renders existing hard-coded meshes via the new mesh/submesh structure.
  - No regressions in existing tests (`tests/src/transform_test.c` etc.). Add targeted tests for submesh-level material swapping if practical.

## Phase 2: Mesh Loader Architecture

### Resource Loader Contract
- Add `lib/src/renderer/resources/loaders/mesh_loader.h/.c` that exposes `VkrResourceLoader vkr_mesh_loader_create();`.
- Introduce `VkrMeshLoaderContext` (stored in `loader.resource_system`) containing:
  - `VkrGeometrySystem* geometry_system`
  - `VkrMaterialSystem* material_system`
  - `VkrMeshManager* mesh_manager` (for referencing transforms/pipeline defaults)
  - Persistent `Arena* arena` for per-load allocations and an embedded `VkrAllocator` (backed by `arena_create(...)`) for temporary CPU buffers.
- Register the loader after mesh manager init inside `vkr_renderer_systems_initialize` (`lib/src/renderer/renderer_frontend.c`).
- Loader type: add new type `VKR_RESOURCE_TYPE_MESH` and call `vkr_mesh_manager_load(...)` via `vkr_resource_system_load(...)` with this tag.
- Add new resource type `mesh` to the `VkrResourceHandleInfo` union.
- `VkrResourceHandleInfo.as.mesh` should point to a `VkrMeshLoaderResult` struct containing parsed submesh descriptions (see below). Lifetime is owned by the loader context arena until `unload`.

### Mesh Loader Result Format
- Define:
  ```c
  typedef struct VkrMeshLoaderSubset {
    VkrGeometryConfig geometry_config;   // CPU-side vertices/indices/bounds
    String8 material_name;               // e.g., "assets/materials/falcon_Albedo.mt"
    VkrPipelineDomain pipeline_domain;   // default WORLD, override via hints
    String8 shader_override;             // optional
  } VkrMeshLoaderSubset;

  typedef struct VkrMeshLoaderResult {
    String8 source_path;
    VkrTransform root_transform;
    Array_VkrMeshLoaderSubset subsets;
  } VkrMeshLoaderResult;
  ```
- During `vkr_mesh_manager_load`, iterate subsets, hand each `geometry_config` to `vkr_geometry_system_create(...)` (auto_release=true so resource system can drop references once mesh releases), load materials via the material loader, and build `VkrSubMeshDesc` entries that populate the parent mesh’s `Array_VkrSubMesh`.

### OBJ Parsing Flow
1. Use `file_path_create` + `file_open` (`lib/src/filesystem/filesystem.c`) to read the `.obj`.
2. Reuse shader loader-style helpers (trim, comment stripping, CSV splitting) to implement:
   - `vkr_mesh_loader_parse_vertex_line(...)` for `v`, `vt`, `vn`.
   - `vkr_mesh_loader_parse_face_line(...)` that supports `f v/t/n v/t/n ...` with fan triangulation when faces have more than 3 vertices.
   - `vkr_mesh_loader_handle_object_or_group(...)` to finalize the previous submesh and reset accumulators.
3. Maintain arrays (allocated from loader context arena or per-load scratch):
   - `Array_Vec3 positions`, `Array_Vec3 normals`, `Array_Vec2 texcoords`.
   - `Array_uint32_t indices` per submesh.
   - `Array_VkrVertex3d vertices` for assembled data.
4. Each parsed face expands to `VkrVertex3d` entries using referenced attributes. Missing normals/uvs should fall back to zero vectors.
5. Track `mtllib` and `usemtl` state to connect faces to materials. Each time `usemtl` changes create/fetch a `VkrMeshLoaderSubset` representing that material chunk.
6. After finishing vertex assembly for a subset:
   - Call `vkr_geometry_system_generate_tangents(&geometry_system->allocator, verts, vertex_count, indices, index_count)` for per-vertex tangents.
   - Call `vkr_geometry_system_deduplicate_vertices(geometry_system, scratch.arena, verts, vertex_count, indices, index_count, &deduped, &deduped_count)` to reduce duplicates.
   - Compute bounds/center by iterating deduped vertices.
   - Populate `geometry_config` with deduped arrays (store copies in loader arena) and metadata (name derived from submesh label).

### Material Conversion
- Parse the referenced `.mtl` (path derived from `mtllib` directive relative to the `.obj` directory). Support at minimum:
  - `newmtl`, `Kd`, `Ks`, `Ns`, `Ke`, `map_Kd`, `map_Ks`, `map_bump` or `bump`.
  - Additional props can be ignored or logged.
- Convert each `newmtl` block to a `.mt` string:
  ```
  name=<derived_name>
  diffuse_texture=<resolved_path or empty>
  diffuse_color=r,g,b,1
  specular_texture=<resolved or empty>
  specular_color=r,g,b,1
  norm_texture=<resolved bump map>
  shininess=<Ns or default to 8>
  shader=shader.default.world
  pipeline=world
  ```
- File writing:
  - Output directory: e.g., `assets/materials/<obj_name>/`.
  - Use filesystem helpers to create directories if needed (if no helper exists, call `mkdir` via `system` or extend filesystem utilities).
  - Use `file_open` with `FILE_MODE_WRITE | FILE_MODE_TRUNCATE | FILE_MODE_BINARY` and `file_write_line`.
  - Return absolute/relative path (String8) to the new `.mt`.
- Material loading:
  - After writing, call `vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL, generated_path, temp_arena, &handle_info, out_error)`.
  - Associate resulting `VkrMaterialHandle` with the subset so `vkr_mesh_manager_create` can set `desc.material`.
  - Keep stable copy of generated path in loader arena for unload bookkeeping.

### Mesh Manager API Additions
- Add to `vkr_mesh_manager.h`:
  ```c
  typedef struct VkrMeshLoadDesc {
    String8 mesh_path;          // e.g., string8_lit("assets/models/falcon.obj")
    VkrTransform transform;     // applied to every submesh
    VkrPipelineDomain domain;   // override pipeline domain
    String8 shader_override;    // optional global override
  } VkrMeshLoadDesc;

  bool8_t vkr_mesh_manager_load(VkrMeshManager* manager,
                                const VkrMeshLoadDesc* desc,
                                uint32_t* out_first_index,
                                uint32_t* out_mesh_count,
                                VkrRendererError* out_error);
  ```
- Implementation outline:
  1. Acquire scratch arena from `manager->arena` via `scratch_create`.
  2. Call `vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, desc->mesh_path, scratch.arena, &info, out_error)`.
  3. Use `info.as.mesh` to get `VkrMeshLoaderResult*`.
  4. For each subset:
     - Create geometry via `vkr_geometry_system_create(manager->geometry_system, &subset->geometry_config, true_v, &geo_err);`
     - Ensure resulting handle stored on subset for reuse/unload.
     - Acquire/load materials referenced in subset (the loader may already return `VkrMaterialHandle`; if not, load them now).
     - Populate a `VkrSubMeshDesc` entry (geometry/material handles, shader override, pipeline domain).
  5. Once all subset descriptors are populated, create a `VkrMeshDesc` containing the global transform + array of `VkrSubMeshDesc`, then call `vkr_mesh_manager_create` once to allocate the mesh + submeshes. Track mesh index for callers.
  6. Call loader `unload` callback (resource system) once GPU state is created to free CPU arrays.

### Loader Unload Responsibilities
- Iterate every subset, release any geometry/material handles if mesh creation failed mid-way.
- Free per-load allocations by resetting arena markers or using `scratch_destroy`.
- Remove/generated `.mt` files optionally (or leave for reuse; document choice). Minimum viable approach: keep files so repeated loads skip regeneration.

## Implementation Steps
1. **VkrMesh/VkrSubMesh Refactor**
   - Define `VkrSubMesh`, slim `VkrMesh` down to transform/model + submesh array, and update descriptors/types in `vkr_resources.h`.
2. **Mesh Manager Updates**
   - Refactor `vkr_mesh_manager` APIs to build/manage arrays of submeshes per mesh, including per-submesh handle acquisition, release, and helpers (getters/setters, counts).
3. **Renderer Frontend & Draw Loop**
   - Update `vkr_renderer_draw_frame` and any supporting code to iterate meshes → submeshes, handle pipeline/material binding, and verify existing scenes still render.
4. **Scaffold Loader**
   - Create `mesh_loader.h/.c`.
   - Implement factory `vkr_mesh_loader_create` returning loader with callbacks.
   - Define context/result structs and helper enums (face types, etc.).
   - Add registration in `vkr_renderer_systems_initialize`.
5. **Parsing Utilities**
   - Port/adapt helper functions from shader loader (`vkr_trim_string8_scratch`, comment stripping) as static functions inside `mesh_loader.c`.
   - Implement token parsing for floats (`string8_to_float` already exists) and index triplets.
   - Add `vkr_mesh_loader_push_vertex(...)` to expand OBJ indices into `VkrVertex3d`.
6. **OBJ Reader**
   - Loop over file lines, handle directives (`v`, `vn`, `vt`, `f`, `o`, `g`, `mtllib`, `usemtl`).
   - Maintain current subset keyed by `usemtl` + group. When encountering new subset, finalize previous subset’s geometry config.
7. **Geometry Finalization**
   - After collecting vertices/indices for a subset, deduplicate + compute tangents + bounds, then allocate/stash `geometry_config` in loader arena.
   - Keep CPU copies around so `vkr_geometry_system_create` can read them after the loader returns to the mesh manager.
8. **Material Pipeline**
   - Parse `.mtl` lazily on first reference; cache parsed results in a hash map (material name -> generated `.mt` path + `VkrMaterialHandle`).
   - Generate `.mt` text (store in arena) and write to disk, or short-circuit by directly invoking material loader with a memory buffer (if we add `vkr_material_loader_load_from_buffer` later).
   - Load resulting `.mt` via resource system and store the handle on the subset.
9. **Mesh Loading API**
   - Add `VkrMeshLoadDesc` + `vkr_mesh_manager_load`.
   - Implement subset-to-submesh descriptor conversion, handle error unwinding, and expose helpers for renderer/frontend to load meshes like `falcon`/`sponza`.
10. **Resource System Integration**
    - Update `vkr_resource_system_unload` paths to recognize mesh loader results (free arrays, release temporary handles, possibly delete generated `.mt` files).
11. **Documentation & Logging**
    - Add logging around success/failure (paths, subset counts).
    - Document `.mt` generation path and future `.ksm` caching hook inside loader file comments.

## Testing & Validation
- **Unit-style**: Add a test harness or extend `tests/src/transform_test.c` analog to create `mesh_loader_test.c` (if feasible) that loads `assets/models/falcon.obj` via the new API and asserts returned mesh count > 0, vertices/indices non-zero, material handles valid.
- **Manual**: Update demo scene/render loop to call `vkr_mesh_manager_load` for `falcon` and `sponza`; verify geometry renders with expected materials.
- **Error paths**: Test missing `.mtl` or textures and ensure loader logs warnings but still creates meshes using fallback materials (e.g., default world).
- **Performance**: Confirm large OBJ (sponza) loads without arena exhaustion; adjust scratch sizes (e.g., `Arena` of 32–64 MB for loader) if necessary.

## Future Work (Binary Cache)
- Reference loader writes `.ksm` binary meshes to skip re-parsing. Mirror that pipeline:
  - After parsing + dedup, emit `.vkm` (or `.ksm`) containing vertex/index data + metadata (bounds/material names).
  - On subsequent loads, prioritize binary file to speed up startup.
  - Include checksum/version fields to validate compatibility.
- This should be designed as a pluggable stage in `vkr_mesh_loader_finalize_subset(...)`, but postponed per current requirements.

## Status
- **Phase 1 (Mesh/SubMesh refactor): Completed.** `VkrSubMesh`/`VkrMesh` now live
  in `lib/src/renderer/resources/vkr_resources.h`, `VkrMeshDesc` consumes
  submesh descriptor arrays, and the mesh manager allocates/manages per-submesh
  resources (`lib/src/renderer/systems/vkr_mesh_manager.{h,c}`). The renderer
  frontend iterates meshes → submeshes for draw submission while keeping the
  default cube hierarchy + UI logo rendering (`lib/src/renderer/renderer_frontend.c`).
  With this foundation in place we can proceed to Phase 2 (loader work).
