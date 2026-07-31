---
status: proposed
updated: 2026-07-31
authority: design
---
# Temporary Allocation Tracking Proposal

## Executive Summary

This document proposes extending the `VkrAllocator` abstract interface to support **scoped/temporary allocations** while maintaining:
1. **Transparency** - Functions that allocate don't need to know they're in a temp scope
2. **Abstraction** - Works with arena, heap, or any custom allocator
3. **Statistics** - Track temp vs permanent allocations without changing allocation call sites

---

## Critical Design Constraint: Callee Transparency

The current scratch pattern has an elegant property that **must be preserved**:

```c
void foo(Arena *arena) {
  // foo doesn't know or care if it's in a scratch scope
  void *data = arena_alloc(arena, size, tag);
  // ...
}

// CALLER controls whether this is temporary:

// Permanent allocation:
foo(myArena);

// Temporary allocation:
Scratch scratch = scratch_create(myArena);
foo(myArena);  // Same call! foo is UNAWARE of scratch
scratch_destroy(scratch, SOME_TAG);
```

**This transparency is the core feature.** Any solution must preserve it.

---

## Current Architecture Analysis

### Arena & Scratch System (`arena.h`, `arena.c`)

```c
typedef struct Scratch {
  Arena *arena;
  uint64_t pos;  // Position when scratch was created
} Scratch;

Scratch scratch_create(Arena *arena);
void scratch_destroy(Scratch scratch, ArenaMemoryTag tag);
```

**Key Characteristics:**
- Scratch is a **caller-side concept** - callees are unaware
- `scratch_create()` just saves position (marker)
- Regular `arena_alloc()` calls work normally
- `scratch_destroy()` resets to saved position (O(1) bulk free)

### VkrAllocator Interface (`vkr_allocator.h`)

```c
typedef struct VkrAllocator {
  VkrAllocatorType type;
  VkrAllocatorStatistics stats;
  void *ctx;

  void *(*alloc)(void *ctx, uint64_t size, VkrAllocatorMemoryTag tag);
  void (*free)(void *ctx, void *ptr, uint64_t old_size, VkrAllocatorMemoryTag tag);
  void *(*realloc)(void *ctx, void *ptr, uint64_t old_size, uint64_t new_size, VkrAllocatorMemoryTag tag);
} VkrAllocator;
```

**Gap:** No way to track which allocations are temporary vs permanent.

---

## Problem Statement

1. **Stats don't distinguish temp from permanent** - Can't see how much memory is used temporarily
2. **Scratch bypasses VkrAllocator** - Direct `scratch_create(arena)` isn't tracked at allocator level
3. **Non-portable** - Scratch is arena-specific; other allocators can't implement similar pattern

---

## Proposed Solution: Transparent Scope Tracking

### Core Principle

The allocator internally tracks "scope depth". When a scope is active, allocations are automatically attributed as temporary. **No changes to allocation call sites.**

```c
void foo(VkrAllocator *allocator) {
  // foo is UNAWARE of scopes - just allocates normally
  void *data = vkr_allocator_alloc(allocator, size, tag);
  // ...
}

// CALLER controls scope (just like scratch):

// Permanent:
foo(&allocator);

// Temporary:
VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
foo(&allocator);  // SAME CALL - foo doesn't change!
vkr_allocator_end_scope(&scope, tag);
```

### Design Goals

1. **Transparent** - `vkr_allocator_alloc()` API unchanged; callees don't know about scopes
2. **Internal Tracking** - Allocator maintains scope stack internally
3. **Zero-Cost for Arena** - Maps directly to scratch (just position save/restore)
4. **Statistics** - Track temp bytes, scope count, peak temp usage

---

## New Types

```c
/**
 * @brief Handle representing a temporary allocation scope.
 * Caller creates scope, calls functions that allocate, then destroys scope.
 * Functions being called don't need to know about the scope.
 */
typedef struct VkrAllocatorScope {
  VkrAllocator *allocator;
  void *scope_data;           // Allocator-specific (e.g., arena position)
  uint64_t bytes_at_start;    // Position/bytes when scope created
} VkrAllocatorScope;

/**
 * @brief Extended statistics with temp allocation tracking.
 */
typedef struct VkrAllocatorStatistics {
  // ... existing fields ...

  // Scope/temp tracking
  uint64_t total_scopes_created;
  uint64_t total_scopes_destroyed;
  uint64_t active_scope_depth;        // Current nesting depth
  uint64_t total_temp_bytes;          // Total bytes ever allocated in scopes
  uint64_t peak_temp_bytes;           // High-water mark
} VkrAllocatorStatistics;
```

### Extended Allocator Interface

```c
typedef struct VkrAllocator {
  // ... existing fields ...

  // Internal scope state (managed by allocator, not exposed)
  uint32_t scope_depth;               // How many scopes deep we are
  uint64_t scope_bytes_allocated;     // Bytes allocated in current scope stack

  // Optional callbacks for scope support
  VkrAllocatorScope (*begin_scope)(void *ctx);
  void (*end_scope)(void *ctx, VkrAllocatorScope *scope, VkrAllocatorMemoryTag tag);

  bool8_t supports_scopes;
} VkrAllocator;
```

---

## Public API

```c
/**
 * @brief Begins a temporary allocation scope.
 * After this call, all allocations via vkr_allocator_alloc() are tracked as temporary.
 * Functions being called don't need any modification - they allocate normally.
 *
 * @param allocator The allocator to create scope on.
 * @return Scope handle for ending the scope later.
 */
VkrAllocatorScope vkr_allocator_begin_scope(VkrAllocator *allocator);

/**
 * @brief Ends a temporary allocation scope.
 * - For arena: resets to saved position (like scratch_destroy)
 * - For heap: frees tracked allocations
 * - Updates temp statistics
 *
 * @param scope The scope to end.
 * @param tag Memory tag for statistics.
 */
void vkr_allocator_end_scope(VkrAllocatorScope *scope, VkrAllocatorMemoryTag tag);

/**
 * @brief Checks if allocator currently has active scopes.
 */
bool8_t vkr_allocator_in_scope(const VkrAllocator *allocator);

/**
 * @brief Get current scope depth (0 = no active scope).
 */
uint32_t vkr_allocator_scope_depth(const VkrAllocator *allocator);
```

### Modified Allocation Tracking

The existing `_vkr_allocator_alloc()` is enhanced to track scope context:

```c
void *_vkr_allocator_alloc(VkrAllocator *allocator, uint64_t size,
                           VkrAllocatorMemoryTag tag, uint32_t line,
                           const char *file) {
  // ... existing stats updates ...

  // NEW: Track if this allocation is within a scope
  if (allocator->scope_depth > 0) {
    allocator->scope_bytes_allocated += size;
    allocator->stats.total_temp_bytes += size;

    // Update peak if needed
    if (allocator->scope_bytes_allocated > allocator->stats.peak_temp_bytes) {
      allocator->stats.peak_temp_bytes = allocator->scope_bytes_allocated;
    }
  }

  return allocator->alloc(allocator->ctx, size, tag);
}
```

---

## Implementation Details

### Arena Allocator Implementation

For arena, scope callbacks reuse `scratch_create`/`scratch_destroy` with zero memory overhead:

```c
vkr_internal VkrAllocatorScope arena_begin_scope_cb(void *ctx) {
  Arena *arena = (Arena *)ctx;
  Scratch scratch = scratch_create(arena);

  return (VkrAllocatorScope){
      .allocator = NULL,
      .scope_data = NULL,
      .bytes_at_start = scratch.pos,
  };
}

vkr_internal void arena_end_scope_cb(void *ctx, VkrAllocatorScope *scope,
                                     VkrAllocatorMemoryTag tag) {
  Arena *arena = (Arena *)ctx;
  Scratch scratch = {.arena = arena, .pos = scope->bytes_at_start};
  scratch_destroy(scratch, to_arena_tag(tag));
}
```

**Benefits:**
- Zero memory overhead - position stored in `bytes_at_start`, `scope_data` unused
- Reuses `scratch_create`/`scratch_destroy` for consistency
- Proper tag conversion via existing `to_arena_tag()` function

### Heap Allocator Implementation

For heap allocators that don't have native bulk-free:

```c
typedef struct HeapScopeData {
  void **allocations;      // Array of pointers to free
  uint32_t count;
  uint32_t capacity;
} HeapScopeData;

vkr_internal VkrAllocatorScope heap_begin_scope_cb(void *ctx) {
  HeapAllocator *heap = (HeapAllocator *)ctx;

  HeapScopeData *data = malloc(sizeof(HeapScopeData));
  data->allocations = NULL;
  data->count = 0;
  data->capacity = 0;

  // Push to scope stack
  heap->scope_stack[heap->scope_depth++] = data;

  return (VkrAllocatorScope){
    .scope_data = data,
    .bytes_at_start = heap->total_allocated,
  };
}

// heap_alloc_cb would track allocations when scope_depth > 0:
vkr_internal void *heap_alloc_cb(void *ctx, uint64_t size, VkrAllocatorMemoryTag tag) {
  HeapAllocator *heap = (HeapAllocator *)ctx;
  void *ptr = malloc(size);

  if (heap->scope_depth > 0) {
    HeapScopeData *scope = heap->scope_stack[heap->scope_depth - 1];
    // Add to tracked allocations for later bulk free
    array_push(scope->allocations, ptr);
  }

  return ptr;
}

vkr_internal void heap_end_scope_cb(void *ctx, VkrAllocatorScope *scope,
                                   VkrAllocatorMemoryTag tag) {
  (void)tag;  // Heap doesn't use arena tags
  HeapScopeData *data = (HeapScopeData *)scope->scope_data;

  // Free all allocations in this scope
  for (uint32_t i = 0; i < data->count; i++) {
    free(data->allocations[i]);
  }

  free(data->allocations);
  free(data);
}
```

---

## Usage Examples

### Example 1: Transparent Scoped Allocation

```c
// This function doesn't know about scopes - unchanged from current code
void process_mesh(VkrAllocator *alloc, MeshData *mesh) {
  // These allocations are temp or permanent depending on caller
  Vec3 *normals = vkr_allocator_alloc(alloc, mesh->vertex_count * sizeof(Vec3),
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  calculate_normals(mesh, normals);
  // ...
}

// Caller 1: Permanent allocation
process_mesh(&allocator, mesh);

// Caller 2: Temporary allocation
VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
process_mesh(&allocator, mesh);  // Same function, same call!
vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
// normals memory is now reclaimed
```

### Example 2: Nested Scopes

```c
VkrAllocatorScope outer = vkr_allocator_begin_scope(&allocator);
{
  void *a = vkr_allocator_alloc(&allocator, 1024, tag);

  VkrAllocatorScope inner = vkr_allocator_begin_scope(&allocator);
  {
    void *b = vkr_allocator_alloc(&allocator, 2048, tag);
  }
  vkr_allocator_end_scope(&inner, tag);  // 'b' reclaimed

  void *c = vkr_allocator_alloc(&allocator, 512, tag);
}
vkr_allocator_end_scope(&outer, tag);  // 'a' and 'c' reclaimed
```

### Example 3: Convenience Macro (Optional)

```c
#define VKR_TEMP_SCOPE(allocator)                                          \
  for (VkrAllocatorScope _scope = vkr_allocator_begin_scope(allocator),    \
       *_guard = &_scope;                                                  \
       _guard != NULL;                                                     \
       vkr_allocator_end_scope(&_scope, VKR_ALLOCATOR_MEMORY_TAG_UNKNOWN), \
       _guard = NULL)

// Usage:
VKR_TEMP_SCOPE(&allocator) {
  void *temp = vkr_allocator_alloc(&allocator, size, tag);
  process(temp);
}  // Automatically cleaned up
```

---

## Migration Path

### Phase 1: Core Implementation (Non-Breaking)

1. Add `scope_depth` and `scope_bytes_allocated` to `VkrAllocator`
2. Add `begin_scope`/`end_scope` optional callbacks
3. Modify `_vkr_allocator_alloc` to track scope-based allocations
4. Implement arena scope callbacks (wrapping scratch)

### Phase 2: Parallel Usage

Both patterns work simultaneously during migration:

```c
// Old style (still works):
Scratch scratch = scratch_create(arena);
foo(arena);
scratch_destroy(scratch, tag);

// New style (equivalent behavior):
VkrAllocatorScope scope = vkr_allocator_begin_scope(&allocator);
foo(&allocator);
vkr_allocator_end_scope(&scope, tag);
```

### Phase 3: Gradual Migration

Migrate call sites as convenient. No rush - both work.

---

## Comparison: Old Scratch vs New Scope

| Aspect | Scratch (Arena) | Scope (VkrAllocator) |
|--------|-----------------|----------------------|
| Caller control | ✓ | ✓ |
| Callee unaware | ✓ | ✓ |
| Zero overhead for arena | ✓ | ✓ |
| Works with other allocators | ✗ | ✓ |
| Statistics tracking | Partial | Full |
| Abstraction | Arena-specific | Abstract |
| Nesting | Implicit | Explicit |

---

## Statistics Output

```
=== VkrAllocator Statistics ===
Type: ARENA
Total Allocations: 15,432
Total Bytes: 128.5 MB

=== Scope Statistics ===
Total Scopes Created: 8,234
Active Scopes: 1
Total Temp Bytes: 45.2 MB
Peak Temp Bytes: 12.8 MB
Temp/Total Ratio: 35.2%

=== By Tag ===
ARRAY:      24.5 MB
STRING:      8.2 MB
TEXTURE:    64.0 MB
...
```

---

## Open Questions

1. **Thread safety:** Should scope depth be per-thread or per-allocator?
   - Arena is typically per-thread anyway
   - Could use thread-local for scope tracking

2. **Validation:** In debug builds, validate LIFO scope destruction?

3. **Arena bypass:** What if someone uses `scratch_create(arena)` directly while also using VkrAllocator scopes?
   - Could detect via position mismatch
   - Or just document "use one or the other"

---

## Conclusion

The key insight is that **scope tracking is internal to the allocator**. Functions don't need to change - they call `vkr_allocator_alloc()` normally. Only the caller decides temp vs permanent by wrapping calls in `begin_scope`/`end_scope`.

This preserves the elegance of the scratch pattern while adding:
- Statistics for temp allocation tracking
- Portability to non-arena allocators
- Explicit scope handles for debugging

**Recommended first step:** Implement arena scope callbacks and add scope tracking to `_vkr_allocator_alloc()`. This provides immediate value with minimal code changes.
