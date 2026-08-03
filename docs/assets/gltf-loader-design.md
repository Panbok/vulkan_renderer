---
status: partial
updated: 2026-08-03
authority: design
---
# glTF Loader Design (Revised)

> **Current boundary.** glTF import emits native metallic-roughness PBR
> materials. Legacy `KHR_materials_pbrSpecularGlossiness` receives a deliberate
> diffuse-only compatibility conversion; its packed specular-glossiness texture
> is not converted to the incompatible metallic-roughness channel layout.

This spec replaces the previous draft and aligns with the current renderer architecture (`VkrResourceLoader` async callbacks, resource-system batching behavior, allocator/lifetime rules).

## Goals

1. Load static `.gltf` and `.glb` meshes through the existing mesh loading path.
2. Preserve current async resource behavior (`prepare_async` + `finalize_async`) for `VKR_RESOURCE_TYPE_MESH`.
3. Reuse existing `VkrMeshLoaderResult` so mesh manager integration stays unchanged.
4. Extract enough material/texture data for current runtime material model without introducing leaks or ownership ambiguity.

## Non-Goals (v1)

1. Skinning, animations, morph targets, cameras, lights.
2. Full glTF extension coverage.
3. Perfect one-to-one PBR parity with glTF in a single iteration.

## Corrections Applied From Previous Draft

1. **Resource loader contract updated**: mesh resources are async-by-default in `vkr_resource_system`; the spec now requires `prepare_async`, `finalize_async`, and `release_async_payload` (not only `load/unload/batch_load`).
2. **Batch routing issue fixed in design**: `vkr_resource_system_load_batch_sync` picks the first loader by `type` with `batch_load`, not by `can_load(path)`. A second mesh loader for glTF would be fragile.
3. **Vendor path corrected**: third-party headers are rooted under `vendor/` (for includes), while local impl translation units are under `lib/src/vendor/`.
4. **Embedded texture contradiction removed**: v1 requires external URI textures; embedded `data:` and GLB `buffer_view` images are explicitly deferred to v2.
5. **Arena pool snippet corrected**: use `context->arena_pool->chunk_size` and preserve release-order symmetry.

## Recommended Integration Strategy (Simplification)

Use **one** mesh loader (`VKR_RESOURCE_TYPE_MESH`) and add glTF parsing as a format backend inside the existing mesh loader flow.

### Why this is the right default

1. Avoids type-level batch dispatch ambiguity.
2. Keeps `vkr_mesh_manager` and resource tracking untouched.
3. Reuses proven ownership/unload logic in `vkr_mesh_loader_destroy_result`.

## High-Level Flow

```text
vkr_resource_system_load(VKR_RESOURCE_TYPE_MESH, path)
  -> mesh_loader.can_load(path) [obj/gltf/glb]
  -> mesh_loader.prepare_async() [CPU-only parse on worker]
       -> dispatch parser by extension:
            .obj  -> existing parser
            .gltf/.glb -> cgltf parser
       -> build VkrMeshLoaderResult in result arena
       -> enqueue material dependencies (resource handles)
  -> mesh_loader.finalize_async() [main thread pump]
       -> wait/resolve dependency handles
       -> publish READY mesh handle
  -> mesh_manager consumes unchanged VkrMeshLoaderResult
```

## File Layout

```text
lib/src/renderer/resources/loaders/
├── mesh_loader.c                 # existing orchestrator (extended)
├── mesh_loader.h
├── mesh_loader_gltf.c            # new: glTF parse/extract helpers
└── mesh_loader_gltf.h            # new: internal parser interface

vendor/
└── cgltf.h                       # vendored parser header

lib/src/vendor/
└── cgltf_impl.c                  # #define CGLTF_IMPLEMENTATION
```

## Parser Contract (Internal)

```c
typedef struct VkrMeshParseInput {
  String8 source_path;
  VkrAllocator *scratch_alloc;    // temporary, per-job
  VkrAllocator *result_alloc;     // persistent, loader result arena
} VkrMeshParseInput;

typedef struct VkrMeshParseOutput {
  bool8_t has_mesh_buffer;
  VkrMeshLoaderBuffer mesh_buffer;
  Array_VkrMeshLoaderSubmeshRange submeshes;
  Array_VkrMeshLoaderSubset subsets;
  String8 source_path;
} VkrMeshParseOutput;
```

`mesh_loader` remains owner of dependency loading, ref-counting, and unload behavior. Parser code only produces CPU-side mesh/material references.

## glTF Extraction Rules (v1)

### Meshes

1. Accept only triangle primitives (`mode == TRIANGLES`); other modes are skipped with warning.
2. Require `POSITION`; fail primitive if missing.
3. `NORMAL`, `TANGENT`, `TEXCOORD_0`, `COLOR_0` are optional:
   - Missing normals/tangents: generate via existing geometry helpers.
   - Missing texcoord: `(0,0)`.
   - Missing color: `(1,1,1,1)`.
4. Indices:
   - Accept u16/u32 and convert to engine index format.
   - If absent, synthesize linear indices.

### Node Transforms

To match current mesh asset shape (single transformed mesh payload), v1 flattens node transforms into vertex data during parse. Hierarchy is not preserved in runtime structures yet.

### Coordinate System

Position, normal, tangent, winding, and handedness data remain in glTF space.
Texture coordinates are the deliberate exception: glTF defines `(0,0)` at the
upper-left of an image, while VKR's ordinary 2D source and `.vkt` paths store
vertically flipped image rows for a bottom-left origin. The importer lowers
`TEXCOORD_0` once as `(u, 1-v)` before caching vertices. Missing texture
coordinates retain the engine's zero default. Any future spatial coordinate
conversion must remain explicit and tested against tangent handedness and normal
transforms.

## Material Mapping

The importer writes generated `.mt` files for the current PBR material path:

| glTF field | Runtime mapping |
|------------|-----------------|
| `pbrMetallicRoughness.baseColorFactor` | `base_color` |
| `pbrMetallicRoughness.baseColorTexture` | base-color slot (`cs=srgb&tc=color_srgb`) |
| `metallicFactor` / `roughnessFactor` | `metallic` / `roughness` |
| `metallicRoughnessTexture` | metallic-roughness slot (`tc=data_mask`) |
| `normalTexture` | normal slot (`tc=normal_rg`) |
| `occlusionTexture` | occlusion slot (`tc=data_mask`) |
| `emissiveFactor` / `emissiveTexture` | emissive factor/slot (`cs=srgb&tc=color_srgb`) |
| `alphaMode=MASK` | set `alpha_cutoff` (default 0.5 if missing) |
| `alphaMode=BLEND` | preserve base alpha for the transparent path |
| `KHR_materials_pbrSpecularGlossiness.diffuseFactor` | `base_color` |
| `KHR_materials_pbrSpecularGlossiness.diffuseTexture` | base-color slot (`cs=srgb&tc=color_srgb`) |
| `KHR_materials_pbrSpecularGlossiness.glossinessFactor` | `roughness = 1 - glossiness`, with `metallic = 0` |

Notes:

1. The RGB specular plus alpha glossiness texture cannot be rebound as the
   current G-roughness/B-metallic texture without an offline conversion or a
   separate shader workflow, so it remains unsupported.
2. The compatibility conversion prefers the specular-glossiness extension over
   an empty fallback `pbrMetallicRoughness` object, matching Bistro-style assets.

## Texture Source Policy

### v1 Required

1. External image URIs (`image.uri` that is not `data:`), resolved relative to
   the glTF file directory, `assets/`, or `assets/textures/` while preserving
   nested URI components. A basename-only `assets/textures/` lookup is retained
   as a final compatibility fallback.
2. Source paths and sidecar `<source>.vkt` files use the same candidate order.
3. Colorspace intent appended via existing texture request query pattern (`?cs=...` / `tc=...` as needed).

### v1 Explicitly Unsupported (returns clear error/warn)

1. `data:` URI images.
2. GLB `image.buffer_view` embedded images.

### v2 Planned

Add decode-from-memory texture path (or texture-system API extension) to support both unsupported cases without temporary files.

## Async and Threading Contract

1. `prepare_async`:
   - Worker thread only.
   - CPU parse + dependency request creation.
   - No Vulkan calls, no renderer mutation.
2. `finalize_async`:
   - Called by resource-system pump on main/render thread.
   - Resolve dependency handles and publish final `VkrResourceHandleInfo`.
3. `release_async_payload`:
   - Must be safe for cancel/failure paths.
   - Must unload any dependency request handles that were queued but not transferred.

## Memory and Lifetime Rules

1. Parse scratch allocations use allocator scope and are ended in same function path.
2. Persistent parse output lives in result arena from `VkrArenaPool`.
3. Before destroying result arena, call `vkr_allocator_release_global_accounting(&result->allocator)`.
4. After arena destroy, return `pool_chunk` via `vkr_arena_pool_release`.
5. Material handle ref-count symmetry must match mesh loader unload/cancel paths.

## Error Model

1. Per-path failures in batch do not fail entire batch.
2. Unsupported glTF features in v1 produce structured warnings and safe defaults when possible.
3. Hard failures only for malformed core mesh data (invalid JSON/bin, missing required POSITION, out-of-memory, invalid accessor bounds).

## Batch Loading Behavior

Because glTF rides the existing mesh loader, mixed `.obj` + `.gltf` + `.glb` batches are valid with current resource-system API:

1. Single batch callback for `VKR_RESOURCE_TYPE_MESH`.
2. Internal parser dispatch by file extension per item.
3. Existing dedupe and job orchestration remain in one path.

## Testing Matrix

### Functional

1. `Box.gltf`: basic geometry + baseColor texture.
2. `DamagedHelmet.glb`: metallic-roughness workflow, tangent space.
3. Scene with multiple nodes sharing one mesh (transform flattening correctness).
4. Mixed batch load (`obj + gltf + glb`) through `vkr_resource_system_load_batch_sync`.

### Robustness

1. Malformed accessor bounds.
2. Missing optional attributes.
3. Unsupported texture source modes (`data:`, `buffer_view`) produce deterministic error.

### Lifetime/Leak

1. Repeated load/unload loop (>=100 cycles) without monotonic growth in created-live resources.
2. Verify material handle refs return to baseline after unload.

## Implementation Phases

### Phase 1 (core, mergeable)

1. Add cgltf vendor + implementation TU.
2. Add glTF parser backend inside mesh loader.
3. Support external URI textures only.
4. Preserve async mesh loader contract and unload symmetry.

### Phase 2 (feature completion)

1. Embedded image support (`data:` + GLB `buffer_view`) via decode-from-memory path.
2. Better PBR bridge or native PBR material path.
3. Optional hierarchy export if runtime scene format is extended.

## Alternative (Not Recommended Unless Required)

A separate `gltf_loader.c` registered as another `VKR_RESOURCE_TYPE_MESH` loader is only safe if resource-system batch dispatch is redesigned to route each path by `can_load`. Without that change, type-only batch selection can pick the wrong loader.

## Files Expected To Change

1. `lib/src/renderer/resources/loaders/mesh_loader.c`
2. `lib/src/renderer/resources/loaders/mesh_loader.h`
3. `lib/src/renderer/resources/loaders/mesh_loader_gltf.c` (new)
4. `lib/src/renderer/resources/loaders/mesh_loader_gltf.h` (new)
5. `vendor/cgltf.h` (new vendored header)
6. `lib/src/vendor/cgltf_impl.c` (new implementation TU)

## References

1. [glTF 2.0 Specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
2. [cgltf](https://github.com/jkuhlmann/cgltf)
3. [Khronos glTF Sample Models](https://github.com/KhronosGroup/glTF-Sample-Models)
4. Existing code paths: `mesh_loader.c`, `material_loader.c`, `vkr_resource_system.c`, `vkr_texture_system.c`
