---
status: implemented
updated: 2026-07-31
authority: design
---
# ECS Refactor Plan (`vkr_entity`) — Correctness, Allocators, API UX, Performance

## Purpose

Refactor the ECS implementation in:

- `lib/src/core/vkr_entity.h`
- `lib/src/core/vkr_entity.c`

to be safe to use in production code (correctness), integrate the engine’s allocator stack (`lib/src/memory/*`), improve API ergonomics, and remove obvious perf pitfalls.

This document is **LLM-consumable**: explicit file paths, concrete changes, phased rollout, and tests.

---

## Current Status (Problems to Fix First)

### 1) Incorrect “alignment” math in chunk layout (data corruption risk)

In `lib/src/core/vkr_entity.c`, the chunk layout/capacity logic uses `vkr_ceil_div_u32()` to “align” offsets:

- `vkr_entity_compute_chunk_capacity()` uses it for `used` computation.
- `vkr_entity_archetype_create()` uses it for `ents_offset` and `col_offsets`.

`ceil_div` is not align-up. This can produce invalid offsets and overestimated capacity, causing overlapping columns or out-of-bounds writes into `chunk->data`.

### 2) Archetype registry stores values, but the ECS uses pointers (double-archetype bug)

`VkrHashTable(VkrArchetype)` stores `VkrArchetype` **by value** (copy). The ECS also allocates `VkrArchetype*` and stores pointers in `world->arch_list`.

This splits the system into two archetype instances per signature:

- inserts/creates can write chunks into the hash-table copy
- queries iterate `arch_list` pointers

Result: “lost” archetypes during queries, memory leaks on `vkr_entity_destroy_world()`, and inconsistent behavior.

### 3) `vkr_entity_is_alive()` does not check `world_id`

`lib/src/core/vkr_entity.c` checks only index/generation. An entity ID from another world can be considered alive if it matches those fields.

### 4) Allocation failure handling is not consistent

Some growth operations assign realloc results without NULL checks (directory grow), and some functions ignore failure returns.

If the ECS is to be used with non-arena allocators (where frees matter), partial-initialization cleanups should be well-defined.

---

## Goals / Non-goals

### Goals

1. **Correctness**: no chunk overlap, no archetype duplication, IDs validated.
2. **Allocator integration**: separate persistent and scratch allocations, optional pools where sizes are fixed.
3. **API UX**: reduce boilerplate (component lookup, create-with-components), easier queries.
4. **Performance**: remove avoidable O(archetypes) scans for per-frame systems, reduce per-mutation overhead.
5. **Test coverage**: add focused ECS tests under `tests/src/`.

### Non-goals (for this refactor)

- Multithreaded ECS mutations.
- Full “systems scheduler” (update graph, dependencies, etc.).
- Serialization format (that’s a separate module; ECS should only provide stable iteration and IDs).

---

## Proposed Architecture Changes

## A) World memory model (persistent + scratch + optional chunk pool)

### A.1 Add allocator roles to `VkrWorld`

Current `VkrWorld` has only `VkrAllocator *alloc`.

Refactor to:

```c
// lib/src/core/vkr_entity.h
typedef struct VkrWorld {
  VkrAllocator *alloc;         // persistent allocations (world lifetime)
  VkrAllocator *scratch_alloc; // scratch allocations (scopes recommended)
  uint16_t world_id;
  ...
} VkrWorld;
```

And update `VkrWorldCreateInfo` accordingly:

```c
typedef struct VkrWorldCreateInfo {
  VkrAllocator *alloc;
  VkrAllocator *scratch_alloc; // optional; defaults to alloc if NULL
  uint16_t world_id;
  ...
} VkrWorldCreateInfo;
```

### A.2 Use `VkrAllocatorScope` for temporary work

Where we currently allocate and then manually `free` (or where we allocate “tmp strings”), wrap operations in a scratch scope:

- `vkr_entity_archetype_get_or_create()`
- `vkr_entity_add_component()`
- `vkr_entity_remove_component()`
- any future “batch create”/“batch add/remove” API

This works best when `scratch_alloc` is backed by `arena` (`lib/src/memory/vkr_arena_allocator.h`) and supports scopes.

### A.3 Optional: use a pool for chunk data blocks

`VkrChunk.data` is always `VKR_ECS_CHUNK_SIZE` (currently `KB(16)` in `lib/src/core/vkr_entity.h`).

If you want efficient reuse in long-running sessions:

- Add `VkrPool chunk_pool` (or `VkrAllocator chunk_alloc`) to `VkrWorld`.
- Allocate `chunk->data` via pool, return it to pool when chunk is freed.

This avoids heap fragmentation and gives O(1) alloc/free for the biggest ECS allocation.

---

## B) Archetype registry: store pointers (single source of truth)

### B.1 Change archetype table type

Replace `VkrHashTable(VkrArchetype)` with a pointer table:

```c
// lib/src/core/vkr_entity.h
VkrHashTableConstructor(struct VkrArchetype*, VkrArchetypePtr);
VkrHashTable_VkrArchetypePtr arch_table;
```

Key remains `const char*` for now (lowest churn), value becomes `VkrArchetype*`.

### B.2 Make `world->arch_list` authoritative

- `arch_list` stores pointers to allocated archetypes.
- `arch_table` maps key → pointer in `arch_list`.
- All chunk lists live on those pointers.

### B.3 Destruction must destroy hash table storage

Currently `vkr_entity_destroy_world()` frees archetypes and chunks, but does not call `vkr_hash_table_destroy_*()`.

After the table refactor:

- call `vkr_hash_table_destroy_VkrArchetypePtr(&world->arch_table)` in `vkr_entity_destroy_world()`.

---

## C) Fix chunk layout computation (alignment correctness)

### C.1 Use align-up, not ceil-div

Use one of:

- `AlignPow2(offset, alignment)` from `lib/src/defines.h`
- or `vkr_align_up_u32(offset, alignment)` from `lib/src/math/vkr_math.h`

`vkr_ceil_div_u32()` should not appear in byte-offset alignment logic.

### C.2 Add debug validation for layout (development-only)

In debug builds, validate for each archetype:

- `ents_offset` and all `col_offsets[i]` are aligned to their respective alignment.
- column ranges do not overlap.
- the last column end ≤ `VKR_ECS_CHUNK_SIZE`.

This should be a small helper that runs when creating an archetype.

---

## D) ID validation and world safety

### D.1 Update `vkr_entity_is_alive()`

Check:

- `id.u64 != 0`
- `id.parts.world == world->world_id`
- `id.parts.index < world->dir.capacity`
- generation matches

### D.2 Centralize validation

Add an internal helper in `lib/src/core/vkr_entity.c`:

```c
vkr_internal INLINE bool8_t vkr_entity_validate_id(const VkrWorld *world,
                                                   VkrEntityId id);
```

Use it in:

- `vkr_entity_destroy_entity`
- `vkr_entity_add_component`
- `vkr_entity_remove_component`
- `vkr_entity_get_component(_mut)`
- `vkr_entity_has_component`

---

## API UX Improvements (Incremental)

### 1) Name → type lookup

Add a component name map in `VkrWorld`:

- `VkrHashTable_uint16_t component_name_to_id` (or a dedicated constructor)

New helpers:

- `VkrComponentTypeId vkr_entity_find_component(const VkrWorld *world, const char *name);`
- `VkrComponentTypeId vkr_entity_register_component_once(...)` (returns existing id if already registered)

### 2) Create entity with components in one step

Current flow requires creating in EMPTY then repeatedly moving via `add_component`.

Add:

```c
VkrEntityId vkr_entity_create_entity_with_components(
    VkrWorld *world,
    const VkrComponentTypeId *types,
    const void *const *init_data, // optional per component
    uint32_t count);
```

This should:

- sort types once
- get/create target archetype once
- insert entity directly into that archetype chunk

### 3) Query iteration helper that exposes typed columns

Keep the existing chunk query API, but add a helper for the common case:

```c
typedef struct VkrQueryIter {
  // precomputed list of matching archetypes/chunks
} VkrQueryIter;
```

Optional v1: just “compiled query” = cached archetype list.

This avoids scanning all archetypes every frame for common systems.

---

## Performance Improvements (Low Risk)

### 1) O(1) type → column index per archetype

Right now `vkr_entity_arch_find_col()` is binary search on `types[]`.

Since `VKR_ECS_MAX_COMPONENTS` is 256, add a small lookup table per archetype:

```c
// lib/src/core/vkr_entity.h
uint16_t type_to_col[VKR_ECS_MAX_COMPONENTS]; // 0xFFFF = not present
```

Then:

- `get_component` becomes O(1) without searching.
- moving entities can copy shared components without per-component searches.

### 2) Avoid temporary archetype-key heap churn

Current `vkr_entity_archetype_get_or_create()` allocates a temporary key string.

Options:

- Keep string keys but build them into scratch scope memory (no per-call frees).
- Follow-up (bigger change): change hashtable to accept binary keys (signature hash + collision check).

Recommendation for refactor v1: scratch-scope key building.

---

## Edge Cases to Handle Explicitly

- **Entity ID from other world**: must be rejected everywhere.
- **Generation wrap**: current code skips `0` (good); keep this invariant.
- **Chunk full**: chunk acquire must always return non-full chunk or create a new one; if allocation fails, propagate failure (don’t assert-only).
- **Allocator mismatch**: if `scratch_alloc` is NULL or doesn’t support scopes, fall back to stack small buffers + persistent alloc + manual free.
- **Component type misuse**: reject type IDs >= `world->comp_count` (not just < 0xFFFF).

---

## Testing Plan (`tests/src/`)

Add a new test file (example name):

- `tests/src/vkr_entity_tests.c`

Minimum tests:

1. **Create/destroy world**: no crashes, no leaks in basic instrumentation.
2. **Create many entities**: validates directory growth and chunk allocation.
3. **Add/remove components**:
   - add component A, set values, remove A, re-add A, ensure values reset/initialized correctly.
4. **Query correctness**:
   - create entities in different archetypes; query include/exclude returns expected chunks/entities.
5. **Archetype registry sanity**:
   - ensure a single archetype instance per signature by comparing pointers returned from repeated get-or-create.
6. **Layout validation** (debug-only):
   - for a synthetic archetype with mixed alignments, ensure computed offsets do not overlap and fit in `VKR_ECS_CHUNK_SIZE`.

---

## Implementation Phases (Recommended Order)

### Phase 0 — Safety net

- Add `tests/src/vkr_entity_tests.c`.
- Keep tests minimal but enough to catch the known archetype/layout bugs.

### Phase 1 — Correctness fixes

- Fix layout alignment (replace `vkr_ceil_div_u32` with align-up).
- Refactor archetype hash table to store `VkrArchetype*`.
- Add world-id validation to `vkr_entity_is_alive` and key entry points.
- Add missing `vkr_hash_table_destroy_*` call for the archetype table.

### Phase 2 — Allocator integration

- Extend `VkrWorldCreateInfo` with `scratch_alloc`.
- Introduce scratch scopes in places that do temporary allocations.
- Optional: add chunk pool for `chunk->data` if desired for long sessions.

### Phase 3 — API UX improvements

- Component name → id map.
- `vkr_entity_create_entity_with_components`.
- Convenience query iterator (compiled query) for per-frame systems.

### Phase 4 — Performance improvements

- Per-archetype `type_to_col[]` for O(1) access and faster moves.
- Reduce temporary key churn further (optional binary-key follow-up).

---

## File Changes Summary

**Update**

- `lib/src/core/vkr_entity.h`
  - add scratch allocator + archetype pointer table typedef
  - add optional per-archetype `type_to_col[]`
  - extend create-info and public APIs
- `lib/src/core/vkr_entity.c`
  - fix chunk layout alignment math
  - refactor archetype registry to store pointers
  - add centralized id validation helper
  - integrate scratch scopes for temporary allocations
- `tests/src/vkr_entity_tests.c` (new)

**Optional**

- `lib/src/memory/*` not modified; ECS only consumes existing allocators/scopes/pools.

