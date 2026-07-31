---
status: partial
updated: 2026-07-31
authority: spec
---
# ECS Hot Path Optimization Specification

## Overview

This document analyzes performance bottlenecks identified via Xcode Time Profiler in the ECS (Entity Component System) and scene update code, and documents the optimizations implemented to address them.

> **Measurement boundary.** Phases 1–4 ship, but the percentages under
> “Expected Performance Improvement” are estimates, not measured results. This
> document remains partial until the same workload is profiled before and after;
> do not cite those estimates as a performance result.

### Profiling Data Summary (Pre-Optimization)

From a 35-second profiling session:

| Function | Time | Percentage |
|----------|------|------------|
| `vkr_scene_update` | 432ms | 20.7% |
| `vkr_entity_get_component_mut` | 73ms | 13.0% (of scene_update) |
| `vkr_entity_is_alive` | 55ms | 9.8% (of scene_update) |

These three functions dominated CPU time in the rendering pipeline.

---

## Bottleneck Analysis

### 1. `vkr_entity_is_alive`

**Issues Identified:**
1. **Not inlined** - Function call overhead on every invocation
2. **Called excessively** - 2000-5000+ times per frame for 1000 entities
3. **Redundant** - Often followed by `get_component_mut` which re-validates

### 2. `vkr_entity_get_component_mut`

**Issues Identified:**
1. **Not inlined** - Hot path function with per-call overhead
2. **Multiple validations** - Both entity and type validated
3. **5-6 pointer indirections** per access

### 3. `vkr_scene_update`

**Issues Identified:**
1. **Redundant validation** - Entity validated twice (is_alive + get_component)
2. **Entity ID reconstruction** - Full ID rebuilt from index each iteration
3. **Per-entity iteration** - No chunk-based processing
4. **Cascading lookups** - `scene_mark_children_world_dirty()` did per-child lookups

---

## Implementation Status

### Phase 1: Inline Functions ✅ COMPLETED

**Effort:** Low | **Impact:** High (30-40% reduction) | **Risk:** Very Low

**Problem:** Non-inlined ECS functions caused function call overhead in hot paths.

**Solution:** Made all hot-path ECS functions `static INLINE` in `vkr_entity.h`.

**Files Modified:**
- `lib/src/core/vkr_entity.h` - Added inline implementations
- `lib/src/core/vkr_entity.c` - Removed legacy implementations

**Key Functions Added:**
```c
// Combined validation + access (eliminates redundant is_alive check)
static INLINE void *vkr_entity_get_component_if_alive(
    VkrWorld *world, VkrEntityId id, VkrComponentTypeId type) {
  // Single validation pass
  if (id.u64 == 0 || id.parts.world != world->world_id ||
      id.parts.index >= world->dir.capacity ||
      world->dir.generations[id.parts.index] != id.parts.generation) {
    return NULL;
  }
  VkrEntityRecord rec = world->dir.records[id.parts.index];
  if (!rec.chunk) return NULL;
  VkrArchetype *arch = rec.chunk->arch;
  uint16_t col_i = arch->type_to_col[type];
  if (col_i == VKR_ENTITY_TYPE_TO_COL_INVALID) return NULL;
  uint8_t *col = (uint8_t *)rec.chunk->columns[col_i];
  return col + (size_t)arch->sizes[col_i] * rec.slot;
}

// Unchecked access for pre-validated entities
static INLINE void *vkr_entity_get_component_unchecked(
    VkrWorld *world, VkrEntityId id, VkrComponentTypeId type);

// Fast entity ID construction from index
static INLINE VkrEntityId vkr_entity_id_from_index(
    const VkrWorld *world, uint32_t index);
```

**Impact:**
- Eliminated function call overhead for all ECS accessors
- Reduced redundant validation (combined is_alive + get_component)
- Direct archetype `type_to_col` lookup instead of function calls

---

### Phase 2: Topo Order Full Entity IDs ✅ COMPLETED

**Effort:** Low | **Impact:** Medium (10-15% reduction) | **Risk:** Low

**Problem:** Topo order stored only indices, requiring per-iteration ID reconstruction.

**Solution:** Store full `VkrEntityId` in topo order array.

**Files Modified:**
- `lib/src/renderer/systems/vkr_scene_system.h`:
  ```c
  // Before
  uint32_t *topo_order;   // Entity indices only

  // After
  VkrEntityId *topo_order; // Full entity IDs with generation
  ```
- `lib/src/renderer/systems/vkr_scene_system.c`:
  - Updated allocation/deallocation to use `sizeof(VkrEntityId)`
  - Updated `scene_rebuild_topo_order()` to store full entity IDs
  - Updated `vkr_scene_update()` to use IDs directly

**Before:**
```c
for (uint32_t i = 0; i < scene->topo_count; i++) {
  uint32_t entity_idx = scene->topo_order[i];
  VkrEntityId entity = vkr_entity_id_from_index(world, entity_idx);
  // ...
}
```

**After:**
```c
for (uint32_t i = 0; i < scene->topo_count; i++) {
  VkrEntityId entity = scene->topo_order[i]; // Direct access
  // ...
}
```

**Impact:**
- Eliminated `vkr_entity_id_from_index()` call per entity
- Eliminated per-iteration generation lookup
- Trade-off: 8 bytes vs 4 bytes per entity (acceptable for reduced per-frame work)

---

### Phase 3: Two-Pass Transform Update ✅ COMPLETED

**Effort:** Medium | **Impact:** High (20-30% reduction) | **Risk:** Medium

**Problem:** Single-pass update mixed cache-friendly operations with topo-ordered operations.

**Solution:** Split into two passes:
1. **Pass 1 (chunk-based):** Update local matrices - cache-friendly, no dependencies
2. **Pass 2 (topo-ordered):** Propagate world matrices - requires parent-before-child order

**Files Modified:**
- `lib/src/renderer/systems/vkr_scene_system.c`:
  - Added `transform_local_update_cb()` chunk callback
  - Refactored `vkr_scene_update()` to two-pass approach

**Implementation:**
```c
void vkr_scene_update(VkrScene *scene, float64_t dt) {
  // Pass 1: Chunk-based local matrix update (cache-friendly)
  // Iterates chunks contiguously, direct column access via archetype
  vkr_entity_query_compiled_each_chunk(&scene->query_transforms,
                                       transform_local_update_cb, scene);

  // Pass 2: Topo-ordered world matrix propagation
  for (uint32_t i = 0; i < scene->topo_count; i++) {
    VkrEntityId entity = scene->topo_order[i];
    SceneTransform *transform = vkr_entity_get_component_if_alive(...);
    if (!transform || !(transform->flags & SCENE_TRANSFORM_DIRTY_WORLD))
      continue;
    // Compute world matrix...
  }
}
```

**Pass 1 Chunk Callback:**
```c
vkr_internal void transform_local_update_cb(const VkrArchetype *arch,
                                            VkrChunk *chunk, void *user) {
  // Direct column access - no per-entity lookup
  uint16_t transform_col = arch->type_to_col[scene->comp_transform];
  SceneTransform *transforms = (SceneTransform *)chunk->columns[transform_col];

  for (uint32_t i = 0; i < chunk->count; i++) {
    SceneTransform *t = &transforms[i];
    t->flags &= ~SCENE_TRANSFORM_WORLD_UPDATED; // Clear from previous frame

    if (t->flags & SCENE_TRANSFORM_DIRTY_LOCAL) {
      t->local = scene_compute_local_matrix(t->position, t->rotation, t->scale);
      t->flags &= ~SCENE_TRANSFORM_DIRTY_LOCAL;
      t->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }
}
```

**Impact:**
- Pass 1: Better cache utilization (contiguous memory access in chunks)
- Pass 1: No per-entity component lookup (direct column access)
- Pass 2: Early-out for clean entities
- Removed redundant local matrix computation from topo loop

---

### Phase 4: Deferred Dirty Propagation ✅ COMPLETED

**Effort:** Medium | **Impact:** Medium (10-20% reduction) | **Risk:** Medium

**Problem:** `scene_mark_children_world_dirty()` did expensive per-child lookups after each world matrix update.

**Solution:** Use `WORLD_UPDATED` flag to propagate dirty state inline during topo traversal.

**Files Modified:**
- `lib/src/renderer/systems/vkr_scene_system.h`:
  ```c
  #define SCENE_TRANSFORM_WORLD_UPDATED 0x08 // World matrix updated this frame
  ```
- `lib/src/renderer/systems/vkr_scene_system.c`:
  - Pass 1 clears `WORLD_UPDATED` from previous frame
  - Pass 2 propagates dirty state from parent inline

**Implementation:**
```c
// Pass 2: Deferred dirty propagation
for (uint32_t i = 0; i < scene->topo_count; i++) {
  VkrEntityId entity = scene->topo_order[i];
  SceneTransform *transform = vkr_entity_get_component_if_alive(...);
  if (!transform) continue;

  // Single parent lookup for both propagation and matrix computation
  SceneTransform *parent_transform = NULL;
  if (transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
    parent_transform = vkr_entity_get_component_if_alive(...);

    // Deferred dirty propagation: inherit from parent updated this frame
    if (parent_transform && (parent_transform->flags & SCENE_TRANSFORM_WORLD_UPDATED)) {
      transform->flags |= SCENE_TRANSFORM_DIRTY_WORLD;
    }
  }

  if (!(transform->flags & SCENE_TRANSFORM_DIRTY_WORLD))
    continue;

  // Compute world matrix...
  transform->flags &= ~SCENE_TRANSFORM_DIRTY_WORLD;
  transform->flags |= SCENE_TRANSFORM_WORLD_UPDATED; // Mark for child propagation

  scene_mark_render_dirty(scene, entity);
  // NOTE: scene_mark_children_world_dirty() call REMOVED
}
```

**Impact:**
- Eliminated `scene_mark_children_world_dirty()` from hot path
- No more per-child lookups (child index slot, entity validation, component lookup)
- Dirty state propagates O(1) per entity during natural topo traversal
- Single parent lookup serves both propagation check and matrix computation

---

## Phase 5: SoA Layout - Evaluation

**Effort:** High | **Impact:** Potentially High | **Risk:** High

### Proposed Change

Convert `SceneTransform` from Array-of-Structures (AoS) to Structure-of-Arrays (SoA):

```c
// Current (AoS) - 168 bytes per entity
typedef struct SceneTransform {
  Vec3 position;        // 12 bytes
  VkrQuat rotation;     // 16 bytes
  Vec3 scale;           // 12 bytes
  VkrEntityId parent;   // 8 bytes
  Mat4 local;           // 64 bytes
  Mat4 world;           // 64 bytes
  uint8_t flags;        // 1 byte (+padding)
} SceneTransform;

// Proposed (SoA) - separate arrays
typedef struct SceneTransformSoA {
  Vec3 *positions;      // Hot
  VkrQuat *rotations;   // Hot
  Vec3 *scales;         // Hot
  uint8_t *flags;       // Hot
  Mat4 *locals;         // Cold
  Mat4 *worlds;         // Cold
  VkrEntityId *parents; // Cold
} SceneTransformSoA;
```

### Theoretical Benefits

1. **Cache efficiency:** When iterating flags only (dirty checks), load 1 byte per entity instead of 168 bytes
2. **SIMD potential:** Contiguous position/rotation/scale arrays enable vectorization
3. **Reduced memory traffic:** Only touch arrays actually needed

### Why NOT Worth Implementing Now

#### 1. ECS Already Provides Chunk-Based SoA

The existing ECS archetype system stores components in SoA layout within chunks:
```c
// Archetype chunks already store components contiguously
chunk->columns[transform_col] // All SceneTransforms for this chunk are contiguous
```

Phase 3's chunk-based iteration already exploits this:
```c
SceneTransform *transforms = (SceneTransform *)chunk->columns[transform_col];
for (uint32_t i = 0; i < count; i++) {
  SceneTransform *t = &transforms[i]; // Sequential memory access
}
```

#### 2. Limited SIMD Opportunity

Local matrix computation (`scene_compute_local_matrix`) involves:
- `mat4_translate(position)` - builds 4x4 matrix
- `vkr_quat_to_mat4(rotation)` - quaternion to matrix conversion
- `mat4_scale(scale)` - builds 4x4 matrix
- Two `mat4_mul` calls

These operations are inherently per-entity and involve 4x4 matrix math. SIMD gains would require:
- Batch 4+ entities with identical operations
- Restructure matrix math for SIMD lanes
- Most time is in `mat4_mul`, not memory access

#### 3. Flags-Only Iteration Rare

The main hot path (Pass 2) needs:
- `flags` (dirty check)
- `parent` (propagation check)
- `position`, `rotation`, `scale` (if dirty)
- `local`, `world` (matrix computation)

When an entity IS dirty, we touch most fields anyway. SoA only helps when skipping many clean entities, but:
- Static scenes: No iteration needed (early exit if no dirty entities)
- Dynamic scenes: Most entities processed anyway

#### 4. Implementation Cost

Converting to SoA would require:
- New storage structure outside ECS or custom archetype handling
- API changes for component access
- Memory management for separate arrays
- Loss of ECS benefits (queries, archeytpe-based iteration)
- Potential cache thrashing if arrays not co-located

#### 5. Diminishing Returns

Phases 1-4 addressed the major bottlenecks:
- **Function call overhead** → Inlining (Phase 1)
- **Redundant validation** → Combined accessors (Phase 1)
- **ID reconstruction** → Full IDs in topo order (Phase 2)
- **Cache-unfriendly local updates** → Chunk iteration (Phase 3)
- **Per-child lookups** → Deferred propagation (Phase 4)

Remaining time in `vkr_scene_update` is likely:
- Actual matrix math (unavoidable)
- Parent lookups (reduced but necessary)
- Render dirty tracking (minimal)

### Recommendation

**Do NOT implement Phase 5.** The effort-to-benefit ratio is poor because:

1. ECS chunks already provide SoA-like benefits
2. Limited SIMD opportunity in matrix computation
3. High implementation cost and API disruption
4. Phases 1-4 already addressed the major bottlenecks

**Alternative optimizations if more performance needed:**
- Profile to identify actual remaining bottlenecks
- Consider SIMD matrix library (e.g., replace mat4_mul with intrinsics)
- Batch renderer sync (Phase 4.2 - not yet implemented)
- Reduce parent lookups via parent transform caching

---

## Summary

### Completed Optimizations

| Phase | Description | Key Change |
|-------|-------------|------------|
| 1 | Inline Functions | All ECS accessors now `static INLINE` |
| 2 | Full Entity IDs | Topo order stores `VkrEntityId` not indices |
| 3 | Two-Pass Update | Chunk-based local + topo-ordered world |
| 4 | Deferred Propagation | `WORLD_UPDATED` flag replaces child lookups |

### Files Modified

```
lib/src/core/vkr_entity.h
  - Added inline implementations of all ECS accessors
  - Added VKR_ENTITY_TYPE_TO_COL_INVALID sentinel
  - Added combined accessor functions

lib/src/core/vkr_entity.c
  - Removed vkr_entity_is_alive, vkr_entity_get_component_mut,
    vkr_entity_get_component, vkr_entity_has_component implementations

lib/src/renderer/systems/vkr_scene_system.h
  - Changed topo_order from uint32_t* to VkrEntityId*
  - Added SCENE_TRANSFORM_WORLD_UPDATED flag

lib/src/renderer/systems/vkr_scene_system.c
  - Added transform_local_update_cb() chunk callback
  - Refactored vkr_scene_update() to two-pass approach
  - Implemented deferred dirty propagation
  - Removed scene_mark_children_world_dirty() from hot path
```

### Unverified Performance Estimate

Based on the bottleneck analysis:
- **Phase 1:** 30-40% reduction (function call overhead eliminated)
- **Phase 2:** 10-15% reduction (ID reconstruction eliminated)
- **Phase 3:** 20-30% reduction (cache-friendly chunk iteration)
- **Phase 4:** 10-20% reduction (child lookup elimination)

**Combined estimate: 50-70% reduction in `vkr_scene_update` time. This has not
been verified and is not a result.**

### Next Steps

1. **Profile** with Xcode Time Profiler to measure actual improvement
2. **Validate** correctness with visual comparison
3. **Monitor** for any regressions in edge cases
4. **Consider** batch renderer sync (Phase 4.2) if profiling shows it's worthwhile

---

## References

- Xcode Time Profiler data (2026-01-23)
- `lib/src/core/vkr_entity.h` - Inline ECS functions
- `lib/src/renderer/systems/vkr_scene_system.c` - Scene update implementation
- ECS best practices: Data-Oriented Design concepts
