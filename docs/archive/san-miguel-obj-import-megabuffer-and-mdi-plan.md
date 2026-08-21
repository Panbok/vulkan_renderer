---
status: superseded
updated: 2026-08-21
authority: design
---
# San Miguel OBJ: Import Dedup, Static Batching, Mega-Buffers, and Multi-Draw Indirect

> **Superseded.** The megabuffer and multi-draw-indirect parts shipped as
> P3 megabuffer publication plus GPU compaction and indirect submission under
> [ADR-028](../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md).
> Import dedup was never taken up. Historical only.


## Purpose

Make the **San Miguel** content load and render efficiently by addressing the two root causes in the current renderer:

1. **Asset import fragmentation**: the OBJ importer produces too many “subsets” (submeshes) for a single mesh file.
2. **Per-geometry buffers**: each subset becomes its own GPU vertex/index buffers, which prevents effective batching and makes multi-draw indirect mostly useless.

This document is written to be **LLM-consumable**: explicit file paths, concrete invariants, and phased implementation steps.

## Implementation Status (Current)

- Phase 1 done: import-time material bucketing drops subset count from ~1,772 to ~281.
- Phase 2 done: mega-buffer cache format (`.vkb` v3) stores one vertex/index buffer + submesh ranges per mesh.
- Phase 3 done: opaque world/shadow paths emit MDI batches; shaders index instances via
  `SV_InstanceID + SV_StartInstanceLocation` (Slang’s Vulkan base instance path).
- MDI requires `multiDrawIndirect` and `drawIndirectFirstInstance`; the Vulkan backend enables these when supported and falls back to single-draw indirect otherwise.

## Observed Results (San Miguel)

- World draw calls: 1,772 → 281 (Phase 1 material bucketing).
- Shadow draw calls: 1,772 → 281 (Phase 1 material bucketing).
- Shadow draw calls: 281 → 98 (Phase 3 MDI batching).
- World draw calls remain at 281 because the scene still binds per-material ranges.
  - Verified on San Miguel after MDI fix: shadows stay at 98.

## Current San Miguel Data (as committed)

- Scene file: `assets/scenes/san_miguel.scene.json` (version 2)
  - 1 mesh entity: `assets/models/san-miguel-low-poly.obj`
- Mesh file sizes:
  - `assets/models/san-miguel-low-poly.obj`: ~599 MB
  - `assets/models/san-miguel-low-poly.vkb` (mesh cache): ~512 MB
  - `assets/models/san-miguel-low-poly.mtl`: ~34 KB
- OBJ statistics (approx):
  - `v`: 3,738,829
  - `vn`: 4,517,249
  - `vt`: 844,670
  - `f`: 4,963,199
  - `usemtl` switches: 1,563
  - `g`/`o` lines: 2,726
  - materials defined in `.mtl`: 281
- Current `.vkb` subset count (v1 cache header): **1,722 subsets**
  - Exact geometry duplicates inside this cache are rare (4 duplicate pairs).

## Current Code Paths (What Happens Today)

### Scene → Mesh load

- Scene parsing: `lib/src/renderer/resources/loaders/scene_loader.c`
  - Reads entity `"mesh": { "path": ... }`, then calls `vkr_mesh_manager_load_batch()`.

### Mesh manager → Mesh loader → Geometry system

- Mesh manager: `lib/src/renderer/systems/vkr_mesh_manager.c`
  - Loads a mesh resource via `vkr_resource_system_load_batch(...)`.
  - For each mesh subset: generates a **stable geometry name** from `(mesh_path + subset_index)`, then:
    - `vkr_geometry_system_acquire_by_name(...)`, else
    - `vkr_geometry_system_create(...)`
  - Net effect: repeated loads of the *same mesh file* reuse geometry; identical geometry across *different* mesh files does not.

- OBJ mesh loader: `lib/src/renderer/resources/loaders/mesh_loader.c`
  - Parses `.obj` (or reads `.vkb`) into `VkrMeshLoaderSubset[]`.
  - `vkr_mesh_loader_parse_obj()` finalizes a subset on:
    - each `usemtl`
    - each `o`/`g`
  - `vkr_mesh_loader_finalize_subset()`:
    - expands faces into a vertex stream per subset
    - calls `vkr_geometry_system_deduplicate_vertices()` (subset-local only)
    - generates tangents per subset
    - stores `vertices/indices` in the loader result arena

- Geometry system: `lib/src/renderer/systems/vkr_geometry_system.c`
  - `vkr_geometry_system_create()` creates **one vertex buffer + one index buffer per geometry**.
  - Drawing binds V/I with offset 0 and draws the full geometry (`index_count`).

### World render path (opaque)

- World view: `lib/src/renderer/passes/vkr_pass_world.c`
  - Collects visible submeshes, then sorts/batches opaque by:
    - `(pipeline_id, material_id, geometry_id)` via `lib/src/renderer/vkr_draw_batch.h`
  - Emits **one instanced draw per batch** using `vkr_geometry_system_render_instanced(...)`.
  - Optional indirect path exists, but only issues **one** `VkDrawIndexedIndirectCommand` per batch and does not change the fact that **each geometry binds different V/I buffers**.

### Shadow render path (CSM)

- Shadow view: `lib/src/renderer/passes/vkr_pass_shadow.c`
  - Per cascade, collects visible submeshes and batches by `(pipeline_id, material_id, geometry_id)`.
  - Already splits:
    - **opaque shadows**: `assets/shaders/shadow_opaque.shadercfg` (`use_instance=0`)
    - **alpha-tested shadows**: `assets/shaders/shadow.shadercfg` (`use_instance=1`, `diffuse_texture`, `alpha_cutoff`)
  - Still issues one draw per batch; if geometry is unique, batch size is 1.

## Why San Miguel Performs Poorly Today

### 1) Subset explosion caused by OBJ parsing rules

`mesh_loader.c` finalizes a new subset on `o/g` boundaries, not just material changes.

For San Miguel, this produces **1,722 subsets** even though there are only **281 materials**.

Result:
- hundreds/thousands of small geometries
- many expensive per-subset operations (tangent generation, GPU buffer creation)
- world pass draw count ~= subset count (since there is only 1 mesh instance)
- shadow pass draw count ~= subset count × cascade count

### 2) Per-subset vertex/index buffers prevent deeper batching

Because each `VkrGeometry` owns its own V/I buffers:
- draw batching can only help when the **same geometry_id repeats**
- San Miguel is mostly “unique geometry per submesh” → batches collapse to size 1
- multi-draw indirect cannot combine draws across different buffers

### 3) Previous shader instance indexing blocked true multi-draw indirect

Slang maps `SV_InstanceID` to `InstanceIndex - BaseInstance`. When using indirect
draws with `firstInstance`, this yields `0` for each draw unless you add the base
instance. The correct path for Slang+SPIR-V is:

- `uint instance_id : SV_InstanceID`
- `uint base_instance : SV_StartInstanceLocation`
- `instance_index = instance_id + base_instance`

This makes MDI instance indexing correct and fixes “only one instance renders”
failures in heavy instanced scenes (e.g. `assets/scenes/cars.scene.json`).

## Proposed Improvements (What to Implement)

### A) Geometry de-duplication by content hash (cross-file reuse)

Goal: if two subsets produce identical `(vertex bytes + index bytes)`, reuse an existing geometry handle even if they come from different mesh paths.

Best place to implement: `lib/src/renderer/systems/vkr_mesh_manager.c` inside the subset loop (before `vkr_geometry_system_create()`), because:
- it already has the final `VkrGeometryConfig` bytes
- it already performs `acquire_by_name` for stable reuse

Proposed key:
- `geo_<hash>` where `hash = blake2b(vertices||indices, 128-bit)` and include `vertex_size/index_size` in the hashed preimage.

Collision policy:
- Use 128-bit digest + optional `memcmp` verification on match (only when a hash key already exists).

Expected impact for San Miguel:
- low (current `.vkb` has only 4 duplicate pairs)
But this is still valuable for general scenes and future asset pipelines.

### B) Import-time static batching: **don’t split on `o/g`**

Goal: reduce San Miguel’s subsets from ~1,722 down toward the material count (~281), and reduce cache size and load time.

Minimal change:
- In `lib/src/renderer/resources/loaders/mesh_loader.c`:
  - stop calling `vkr_mesh_loader_finalize_subset()` on `o`/`g`
  - keep `builder.name` as metadata only (optional), but don’t flush geometry

Stronger change (recommended):
- Parse the OBJ into **N active builders**, keyed by `material_name` (and optionally `shader_override`, `pipeline_domain`).
- When a face is parsed, append it into the builder for the current material.
- Finalize all builders at end-of-file → exactly “one subset per material bucket”.

Invariants to preserve:
- each subset still produces valid tangents and bounds
- materials still generate `.mt` files via `vkr_mesh_loader_resolve_material()`

### C) Replace per-subset buffers with a “mesh mega-buffer” representation

Goal: allocate **one vertex buffer and one index buffer per mesh asset**, plus a list of submesh ranges.

This is the enabling step for:
- fewer GPU buffers
- faster loading (one upload instead of N uploads)
- MDI (batching draws that share buffers)
- shadow “opaque caster” fast path

Proposed data model (conceptual):

- **MeshAssetBuffers**:
  - `vertex_buffer`
  - `index_buffer`
  - `vertex_stride`, `index_type`

- **SubmeshRange**:
  - `material_handle` (or material index/name to resolve)
  - `first_index`, `index_count`
  - `vertex_offset` (optional, typically 0 if indices are global)
  - local bounds

This requires either:
1) Extending `VkrSubMesh` to carry draw-range information, OR
2) Creating a new mesh asset type used by the mesh manager + views.

### D) Multi-draw indirect (MDI) for mega-buffer batches

Goal: render many objects that share `(pipeline, material, vertex format)` with:

1. bind pipeline once
2. bind material once (world) / no material (shadow opaque)
3. bind one shared V/I buffer pair once
4. issue one `vkCmdDrawIndexedIndirect(..., drawCount=N)`

Implemented (world + shadow opaque):

- CPU indirect command stream uses `first_index/index_count/vertex_offset/first_instance`.
- Batch key ignores `range_id` in MDI mode so ranges sharing the same geometry
  and material are grouped together.
- Shaders use `SV_InstanceID` only; Vulkan `firstInstance` is applied via
  `gl_InstanceIndex`, so per-command instance addressing works without
  `SV_BaseInstance`.
- MDI requires `multiDrawIndirect` and `drawIndirectFirstInstance`. The Vulkan
  backend enables both when supported and falls back to single-draw indirect
  otherwise.

### E) Shadows: “opaque caster” fast path for static scenes

Even without MDI, shadows can be improved dramatically for San Miguel:

- For a fully static mesh asset:
  - build a **shadow-only merged index stream** for opaque casters
  - render it with one draw call per cascade using `shader.shadow.opaque`

Alpha-tested casters:
- keep separate, grouped by `(diffuse alpha texture)` to minimize descriptor binds

This is compatible with the current shadow pipeline split (already implemented).

## Phased Implementation Plan (Minimize Risk)

### Phase 1 (done): Import batching fix (subset count reduction)

- OBJ parsing buckets faces by `material_name`; `o/g` no longer flush subsets.
- `.vkb` cache version bumped and regenerated.
- Result: San Miguel subsets drop near material count and world/shadow draw calls follow.

### Phase 2 (done): Mesh mega-buffer asset format

- Cache format (v3) stores one merged vertex/index payload plus submesh ranges.
- Mesh manager builds one geometry handle per mesh and uses per-submesh ranges.
- Views render by `first_index/index_count/vertex_offset` ranges.

### Phase 3 (done): MDI batches (world + shadow opaque)

- Shaders use `SV_InstanceID` only; indirect draws set `firstInstance` per command.
- Draw batching uses `range_id = 0` for MDI so batches group by shared buffers.
- Opaque world/shadow batches emit one `vkCmdDrawIndexedIndirect` per batch.

### Phase 4: Scene-level static batching

Moved to dedicated spec: `docs/assets/static-scene-batching-spec.md`

## Notes / Guardrails

- Memory lifetime: avoid arena-backed growth for per-scene batch caches that are recreated on reload; prefer pool/dmemory for reclaimable CPU-side structures.
- Keep opaque shadow path material-free (already done) and ensure alpha-tested path binds only the diffuse alpha texture.
- MDI requires `multiDrawIndirect` + `drawIndirectFirstInstance`; the backend enables both when supported and falls back to single-draw indirect if needed.
- Use metrics already present in world/shadow passes to validate wins (draws collected, batches created, shadow draws per cascade).

## Related Docs

- `docs/instanced-rendering/SPEC.md` (current instancing + indirect support status)
- `docs/assets/resource_loading_analysis.md` (GPU upload bottlenecks; batching uploads complements mega-buffers)
- `docs/rendering/csm-implementation-analysis.md` (current CSM/shadow pipeline behavior)
