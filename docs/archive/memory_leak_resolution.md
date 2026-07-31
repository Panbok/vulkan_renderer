---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../../.codex/skills/vkr-memory/SKILL.md`](../../.codex/skills/vkr-memory/SKILL.md). Retained for history; do not treat as current.
# Scene Load/Unload CPU Memory Growth (Allocator Stats)

## TL;DR

The growth seen in `Global allocator stats` during repeated scene load/unload was caused by a mix of:

1) **Real growth**: material/texture systems allocated hash keys (names/paths) from long-lived arenas and never freed them on unload.
2) **Accounting gap**: mesh loader results used short-lived arena pool chunks that were destroyed in bulk; global stats were not decremented because no per-allocation `vkr_allocator_free()` calls were made.
3) **Real growth**: Vulkan per-instance CPU tracking arrays were re-allocated on instance re-acquire even when instance ids were reused via a free list.
4) **Real growth**: Shader SPIR-V bytecode was never freed after `vkCreateShaderModule()`.
5) **Real growth**: Scene ECS allocations (entities, archetypes, chunks) used a shared arena that was never reclaimed on scene unload.

Fixes implemented in code:

- Use per-system `VkrDMemory` for **freeable strings** in `VkrMaterialSystem` and `VkrTextureSystem`, and explicitly free those strings on unload.
- Add `vkr_allocator_release_global_accounting()` and call it immediately before destroying arena-backed allocators that are freed in bulk (mesh loader result arenas).
- Remove an unnecessary persistent allocation in texture loading (`VkrTexture.file_path` was never read and created an extra copy).
- Fix Vulkan instance-state re-acquire to allocate CPU buffers once per instance id and reset generations on release.
- **Free shader SPIR-V bytecode** after `vkCreateShaderModule()` since Vulkan copies the data internally.
- **Add explicit pipeline struct free** in `vulkan_graphics_pipeline_destroy()`.
- **Per-scene arena isolation**: each `VkrSceneRuntime` now owns a dedicated arena that is destroyed with proper accounting on scene unload, reclaiming all ECS memory in bulk.

## Current Status

### Fixed (Confirmed Working)
- `STRING` growth: fixed via DMemory-backed strings in material/texture systems
- `FILE` growth: fixed - now shows **0 Bytes** after shader bytecode free fix
- `ARRAY` growth: fixed via mesh loader bulk-free accounting
- `STRUCT` growth from scene ECS: fixed via per-scene arena with proper destroy

### Still Growing (~0.75 MB/cycle)
- `RENDERER`: grows from 16.6 MB → 21.9 MB over 8 load/unload cycles
- `STRUCT`: minor growth (~0.5 KB/cycle) - likely from pipeline registry internals

The remaining growth is **not** from scene allocations (those now use isolated arenas). The leak source is in long-lived renderer subsystems that use arena-backed allocators.

## Symptom Snapshot (What We Observed)

After loading and unloading the same scene multiple times, these tags increased monotonically in the debug UI/log:

- `ARRAY`, `FILE`: ~16 MB per cycle (dominant growth)
- `STRING`: small per-cycle growth
- `RENDERER`, `STRUCT`: small per-cycle growth

GPU memory returned to baseline after unload, indicating Vulkan resource destruction was working.

## Important: What “Global Allocator Stats” Actually Measure

`vkr_allocator_print_global_statistics()` reports **net bytes tracked by the `VkrAllocator` API**:

- `vkr_allocator_alloc*()` increments tag counters.
- `vkr_allocator_free*()` decrements tag counters (requires correct `old_size`).
- **Bulk frees** like `arena_destroy()` or arena pool recycling do **not** automatically decrement global tag counters, unless the allocations were inside a `vkr_allocator_begin_scope()`/`vkr_allocator_end_scope()` region (scopes do adjust stats).

This means “allocator stats leak” can happen even when OS memory is stable, if memory is reclaimed by destroying/resetting an arena without informing the global counters.

## Root Causes

### Root Cause A (Real Growth): Arena-backed Hash Keys Never Freed

Material/texture systems kept their internal arenas for the lifetime of the renderer. During each load/unload cycle, loaders:

- allocated a new stable key string for the hash table (material name / texture path),
- removed the hash entry on unload,
- but did **not** free the key string memory.

Because the allocator was arena-backed, those strings accumulated and the arena’s high-water mark kept increasing.

### Root Cause B (Accounting Gap): Bulk Arena Destruction Not Reflected in Global Stats

Mesh loading uses per-mesh arena pool chunks. Those allocators are destroyed in bulk (via `arena_destroy()` + returning the chunk to the pool), but global `VkrAllocator` counters previously had no way to “subtract everything allocated by this allocator”.

Result: `FILE`/`ARRAY`/`STRUCT` counters appeared to grow across cycles even though the pool memory was reused and destroyed correctly.

### Root Cause C (Real Growth): Vulkan Instance State Re-acquire Reallocated CPU Buffers

In the Vulkan backend, `vulkan_shader_acquire_instance()` allocated per-instance CPU-side tracking arrays (`descriptor_sets` and per-descriptor `generations`) from an arena every time an instance id was acquired, even when reusing an id from the free list.

On release, only Vulkan descriptor sets were freed (`vkFreeDescriptorSets`); the CPU buffers were not freed (arena) and the pointers were overwritten on the next acquire.

Symptom: monotonically increasing `RENDERER` (and a smaller `STRUCT`) across load/unload cycles even when resources were otherwise correctly released.

### Root Cause D (Likely Remaining): Vulkan Pipeline/Shader CPU Allocations Live in a Long-Lived Arena

Even if Vulkan objects (pipelines, descriptor pools, shader modules, etc.) are destroyed correctly, CPU allocations made through a long-lived arena-backed allocator will not be reclaimed unless the allocator is:

- scope-reset, or
- explicitly adjusted via `vkr_allocator_release_global_accounting()` and then destroyed, or
- backed by a freeable allocator (pool/dmemory) and freed per object.

Two common culprits for `RENDERER` growth per load/unload cycle:

1) **Pipelines/shaders are recreated every cycle** and the backend allocates `struct s_GraphicsPipeline` from an arena (no free).
2) **SPIR-V bytecode buffers** are loaded using an arena allocator and never freed after `vkCreateShaderModule()`.

Specific edge case: `file_load_spirv_shader()` may allocate the final buffer under `VKR_ALLOCATOR_MEMORY_TAG_RENDERER` when it needs to realign non-4-byte-aligned file data.

## Fixes Implemented (Code-Level)

## What “Good” Looks Like After These Fixes

The allocator stats do not necessarily return exactly to the very first startup snapshot (long-lived arenas keep their high-water mark), but they should:

- Stop increasing monotonically on every load/unload cycle.
- Stabilize after the first cycle for a given “max scene complexity” (hash sizes, max in-flight loads, max instance count).

If any tag still grows on every cycle, treat it as either:

- **real growth** (something truly never freed / never reused), or
- **accounting gap** (bulk free without stats adjustment).

### 1) Bulk-free Accounting Helper

Added a helper to adjust global byte counters for allocators destroyed in bulk:

- `lib/src/memory/vkr_allocator.h`: `vkr_allocator_release_global_accounting()`
- `lib/src/memory/vkr_allocator.c`: implementation

Usage rule:

- Call `vkr_allocator_release_global_accounting(&allocator)` exactly once, immediately before destroying the allocator backing store (e.g. before `arena_destroy()`).

### 2) Mesh Loader: Track Allocator Per Result + Account Before Arena Destroy

Mesh loader results now store the allocator wrapper used for result allocations and call the accounting helper on unload and early cleanup paths:

- `lib/src/renderer/resources/loaders/mesh_loader.h`: `VkrMeshLoaderResult.allocator`
- `lib/src/renderer/resources/loaders/mesh_loader.c`: call `vkr_allocator_release_global_accounting()` before `arena_destroy()`

### 3) Material System: Freeable Strings via DMemory + Free on Unload

Material system now owns a small `VkrDMemory` arena for strings that must be freed individually:

- `lib/src/renderer/systems/vkr_material_system.h`: `string_memory`, `string_allocator`
- `lib/src/renderer/systems/vkr_material_system.c`: create/destroy `VkrDMemory`
- `lib/src/renderer/resources/loaders/material_loader.c`:
  - allocate stable material name and shader name via `system->string_allocator`
  - free those strings **after** removing the hash entry on unload
  - free shader name on early load failures (before the material is registered)

### 4) Texture System: Freeable Hash Keys via DMemory + Free on Unload

Texture system now owns `VkrDMemory` for stable keys and stores the key pointer in the value entry for later freeing:

- `lib/src/renderer/systems/vkr_texture_system.h`:
  - `VkrTextureEntry.name`
  - `VkrTextureSystem.string_memory`, `VkrTextureSystem.string_allocator`
- `lib/src/renderer/systems/vkr_texture_system.c`: allocate keys via `string_allocator`
- `lib/src/renderer/resources/loaders/texture_loader.c`: free `entry->name` after hash removal

### 5) Texture Load Cleanup: Stop Storing Unused `VkrTexture.file_path`

`VkrTexture.file_path` was assigned during load but never read anywhere. It also required creating a temporary null-terminated copy and then allocating another copy inside `file_path_create()`.

That assignment was removed to avoid persistent string growth.

### 6) Vulkan Backend: Allocate Instance CPU Buffers Once

- `lib/src/renderer/vulkan/vulkan_shaders.c`: allocate `descriptor_sets` and per-descriptor `generations` only on first use (`NULL`), then reuse on subsequent acquires of the same instance id.

### 7) Shader SPIR-V Bytecode: Free After Module Creation

**Location:** `lib/src/renderer/vulkan/vulkan_shaders.c`

The shader bytecode loaded by `file_load_spirv_shader()` is copied by Vulkan during `vkCreateShaderModule()`. The CPU buffer was never freed after module creation.

**Fix:** Added `vkr_allocator_free()` calls after successful (and failed) module creation:
- Single-file shaders: lines ~158-165
- Multi-file shaders: lines ~195-201

This fix is confirmed working - `FILE` tag now shows **0 Bytes** after scene unload.

### 8) Pipeline Structure: Free in Destroy Path

**Location:** `lib/src/renderer/vulkan/vulkan_pipeline.c:406-408`

`struct s_GraphicsPipeline` was allocated from `state->alloc` but never freed in `vulkan_graphics_pipeline_destroy()`.

**Fix:** Added explicit free at end of destroy function:
```c
vkr_allocator_free(&state->alloc, pipeline, sizeof(*pipeline),
                   VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
```

**Note:** This fix only helps if pipelines are actually destroyed. Currently pipelines are **cached** and not destroyed on scene unload, so this doesn't reduce per-cycle growth.

### 9) Per-Scene Arena for ECS Allocations

**Location:** `lib/src/renderer/systems/vkr_scene_system.c`

Previously, scene ECS allocations (entities, archetypes, chunks, components) used `rf->allocator` (the renderer frontend's arena). Since arena frees are no-ops, memory accumulated across load/unload cycles.

**Fix:** Each `VkrSceneRuntime` now owns a dedicated arena:

```c
struct VkrSceneRuntime {
  VkrScene scene;
  VkrSceneRenderBridge bridge;
  Arena *scene_arena;           // Per-scene arena
  VkrAllocator scene_allocator; // Wrapper with proper function pointers
  VkrAllocator *parent_alloc;   // For freeing runtime struct itself
};
```

**In `vkr_scene_handle_create`:**
1. Allocate runtime from parent allocator
2. Create 2MB arena for scene data
3. Initialize allocator via `vkr_allocator_arena()`
4. Pass `scene_allocator` to `vkr_scene_init` and `scene_render_bridge_init`

**In `vkr_scene_handle_destroy`:**
1. Shutdown scene and bridge (frees are no-ops but that's fine)
2. Call `vkr_allocator_release_global_accounting()` to adjust stats
3. Call `arena_destroy()` to reclaim all scene memory
4. Free runtime struct from parent allocator

This isolates scene memory completely - destroying the scene arena reclaims all ECS memory in bulk.

## Validation Checklist

1. Start the app and capture `Global allocator stats` baseline.
2. Load/unload the same scene repeatedly (e.g. `L` then `U` in the sample app).
3. Confirm:
   - `STRING` stops growing across cycles.
   - `FILE`/`ARRAY` no longer grow monotonically (mesh loader bulk-free accounting is applied).
4. Optional: confirm OS memory (RSS) stability with an external profiler; allocator stats are not a substitute for OS-level measurement.

## Remaining RENDERER Leak (~0.75 MB/cycle) - Current Investigation

After implementing shader bytecode free and pipeline structure free, RENDERER still grows ~0.75 MB per load/unload cycle. The fixes above are implemented and working for their specific cases, but the growth persists.

### Key Observation: Pipelines Are Cached

Pipelines in `VkrPipelineRegistry` are **cached by name** and reused across scene loads. They are only destroyed on renderer shutdown, not on scene unload. This means:

- The pipeline struct free (fix #8) only executes at shutdown
- If pipelines are truly cached, they shouldn't be recreated each cycle
- Yet RENDERER grows, so something IS being allocated each cycle

### Likely Remaining Causes

#### Theory 1: Pipeline Cache Miss → New Pipeline Created Each Cycle

**Hypothesis:** The pipeline cache lookup (`vkr_pipeline_registry_get_pipeline_for_material`) may fail to find cached pipelines, causing new pipeline creation each cycle.

**Possible reasons for cache miss:**
- Material name or shader name differs slightly between loads (string comparison fails)
- Hash collision or bucket overflow in the pipeline hash table
- Pipeline key includes something that changes per-load (e.g., a pointer or ID)

**Investigation:**
```c
// In vkr_pipeline_registry_get_pipeline_for_material():
// Add logging to see if cache hits or misses
log_debug("Pipeline lookup: name='%s', found=%s",
          material->shader_name, existing ? "HIT" : "MISS");
```

**Location:** `lib/src/renderer/systems/vkr_pipeline_registry.c:800-870`

#### Theory 2: Shader Object Allocations Per Pipeline Creation

**Hypothesis:** When a pipeline is created, `VulkanShaderObject` internal allocations (descriptor set layouts, UBO buffers, etc.) are made from `state->alloc` (arena) and never freed.

**Location:** `lib/src/renderer/vulkan/vulkan_shaders.c:220-350`

Allocations include:
- `global_descriptor_sets` array (line 283-286)
- `global_descriptor_generations` array (line 293-295)
- Global UBO buffers (line 331+)

These are allocated per shader object creation and only cleaned up in `vulkan_shader_object_destroy()`.

#### Theory 3: Instance State Pool Exhaustion

**Hypothesis:** The shader instance state free list isn't being used correctly, causing new instance IDs to be allocated each cycle instead of reusing released ones.

**Check:** Compare `VkrPipelineRegistry.stats.total_instance_acquired` vs `total_instance_released` after a full load+unload cycle. They should match.

**Location:** `lib/src/renderer/systems/vkr_pipeline_registry.c` (stats tracking)

#### Theory 4: Descriptor Pool Growth

**Hypothesis:** Vulkan descriptor pools for instance states may be growing or being recreated.

When instance descriptor sets are allocated (`vkAllocateDescriptorSets` in `vulkan_shader_acquire_instance`), if the pool is exhausted, a new pool might be created. CPU tracking for these pools would cause RENDERER growth.

**Location:** `lib/src/renderer/vulkan/vulkan_shaders.c:827-840`

### Recommended Next Steps

1. **Add pipeline cache hit/miss logging** to confirm whether pipelines are truly reused
2. **Compare instance acquire/release stats** after load+unload cycle
3. **Profile with Instruments/Heaptrack** to identify the exact allocation site
4. **Add RENDERER tag breakdown** to see which subsystem (pipeline_registry, shader_system, vulkan_backend) is growing

### Memory Growth Pattern

From test run (8 load/unload cycles):
```
Cycle 1: RENDERER = 16.62 MB
Cycle 2: RENDERER = 17.38 MB (+0.76 MB)
Cycle 3: RENDERER = 18.13 MB (+0.75 MB)
Cycle 4: RENDERER = 18.88 MB (+0.75 MB)
Cycle 5: RENDERER = 19.64 MB (+0.76 MB)
Cycle 6: RENDERER = 20.39 MB (+0.75 MB)
Cycle 7: RENDERER = 21.14 MB (+0.75 MB)
Cycle 8: RENDERER = 21.90 MB (+0.76 MB)
```

Consistent ~0.75 MB/cycle growth suggests a fixed-size allocation pattern (not proportional to scene complexity).

---

## If You Still See Monotonic Growth

### Investigation Checklist (Minimal, High-Signal)

1) **Instance acquire/release symmetry**
   - For every successful `vkr_pipeline_registry_acquire_instance_state()`, ensure there is a matching `vkr_pipeline_registry_release_instance_state()` on all unload/error paths.
   - Compare `VkrPipelineRegistry.stats.total_instance_acquired` vs `total_instance_released` after a full load+unload cycle; they should match.

2) **Pipelines recreated per cycle**
   - Compare `VkrPipelineRegistry.stats.total_pipelines_created` vs the number you expect to exist after the first load.
   - If pipelines are destroyed and recreated per cycle, `RENDERER` growth is expected with arena-backed allocations (even if Vulkan destruction is correct).

3) **Shader bytecode lifetime**
   - In `lib/src/renderer/vulkan/vulkan_shaders.c`, SPIR-V is loaded into CPU memory before creating `VkShaderModule`.
   - If that buffer is allocated from `state->alloc` (arena) and not freed after module creation, every shader creation increases `RENDERER`.

4) **Bulk-free accounting vs real memory**
   - If you destroy/reset an allocator in bulk (arena destroy/reset, pool reuse) without `vkr_allocator_end_scope()` or `vkr_allocator_release_global_accounting()`, global stats will drift even when OS memory is stable.

### Likely Fix Directions

- **Fix missing instance releases**: ensure scene unload releases every submesh/view instance state even in early-outs.
- **Stop recreating pipelines each cycle**: cache pipelines/shader objects across scenes; only destroy on renderer shutdown.
- **Make pipeline/shader allocations reclaimable**:
  - allocate pipeline structs from a `VkrPool` (fixed-size) or `VkrDMemory` and free on pipeline destruction, or
  - move “temporary” shader bytecode buffers to a scope allocator (`state->temp_scope`) and end the scope after `vkCreateShaderModule()`.

## Edge Cases to Keep in Mind

- **Hash table key lifetime**: free a key string only **after** removing the entry. Removal probes compare against the stored key pointer.
- **Default resources**: avoid freeing string literals or non-owned strings. The code uses `vkr_dmemory_owns_ptr()` checks before freeing.
