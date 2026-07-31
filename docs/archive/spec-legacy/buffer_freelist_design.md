---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Buffer Freelist Design Specification

## Overview

This document describes the design and implementation of dynamic sub-allocation for Vulkan buffers using a freelist-based memory allocator. The implementation provides efficient sub-allocation within GPU buffers, reducing the number of VkBuffer objects and improving memory utilization.

## Architecture

The system is organized in three distinct layers, each building upon the previous:

```
┌─────────────────────────────────┐
│      VulkanBuffer               │
│  (GPU buffer management)        │
│  - VkBuffer handle              │
│  - Sub-allocation API           │
└───────────┬─────────────────────┘
            │ uses
            ▼
┌─────────────────────────────────┐
│      VkrDMemory                 │
│  (Offset allocation tracking)   │
│  - alloc/free/resize API        │
│  - Virtual address space        │
└───────────┬─────────────────────┘
            │ uses
            ▼
┌─────────────────────────────────┐
│      VkrFreeList                │
│  (Free space tracking)          │
│  - Linked list of free blocks   │
│  - Node memory management       │
└─────────────────────────────────┘
```

### Layer 1: VkrFreeList

**Purpose**: Track free and allocated regions within an address space.

**Key Features**:
- Tracks offsets and sizes of free blocks
- Coalesces adjacent free blocks
- Minimal overhead (node storage only)
- Resize support with node memory reallocation

**API**:
```c
bool8_t vkr_freelist_create(void *memory, uint64_t memory_size,
                            uint64_t total_size, VkrFreeList *out_freelist);
bool8_t vkr_freelist_allocate(VkrFreeList *freelist, uint64_t size,
                              uint64_t *out_offset);
bool8_t vkr_freelist_free(VkrFreeList *freelist, uint64_t size, uint64_t offset);
bool8_t vkr_freelist_resize(VkrFreeList *freelist, uint64_t new_total_size,
                            void *new_node_memory, void **out_old_memory);
```

### Layer 2: VkrDMemory

**Purpose**: Provide a complete allocator interface using platform memory and freelist tracking.

**Key Features**:
- Manages platform memory (reserve/commit/decommit/release)
- Uses freelist internally for allocation tracking
- Returns pointers into managed memory space
- Resize support with data preservation

**API**:
```c
bool8_t vkr_dmemory_create(uint64_t total_size, VkrDMemory *out_dmemory);
void *vkr_dmemory_alloc(VkrDMemory *dmemory, uint64_t size);
bool8_t vkr_dmemory_free(VkrDMemory *dmemory, void *ptr, uint64_t size);
bool8_t vkr_dmemory_resize(VkrDMemory *dmemory, uint64_t new_total_size);
```

### Layer 3: VulkanBuffer

**Purpose**: Manage GPU buffers with sub-allocation support.

**Key Features**:
- Creates and manages VkBuffer and VkDeviceMemory
- Uses VkrDMemory for offset tracking (not real memory)
- Sub-allocation returns offsets into GPU buffer
- Resize support with GPU data copying

**API**:
```c
bool8_t vulkan_buffer_create(VulkanBackendState *state,
                             const VkrBufferDescription *desc,
                             struct s_BufferHandle *out_buffer);
bool8_t vulkan_buffer_allocate(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t size, uint64_t *out_offset);
bool8_t vulkan_buffer_free(VulkanBackendState *state, VulkanBuffer *buffer,
                            uint64_t size, uint64_t offset);
uint64_t vulkan_buffer_free_space(VulkanBuffer *buffer);
bool8_t vulkan_buffer_resize(VulkanBackendState *state, uint64_t new_size,
                             VulkanBuffer *buffer, VkQueue queue, VkCommandPool pool);
```

## Design Decisions

### 1. DMemory as Offset Tracker

**Decision**: Use VkrDMemory to track GPU buffer offsets, not actual CPU memory.

**Rationale**:
- VkrDMemory provides a clean allocator interface (alloc/free)
- Encapsulates freelist complexity
- Returns pointers that we convert to offsets
- Consistent API across CPU and GPU allocation tracking

**Implementation**:
```c
typedef struct VulkanBuffer {
  VkBuffer handle;             // GPU buffer
  VkDeviceMemory memory;       // GPU memory
  VkrDMemory offset_allocator; // Tracks which offsets are allocated (value, not pointer!)
  // ... other fields
} VulkanBuffer;
```

The `offset_allocator` is embedded directly in the structure (no dynamic allocation needed). It manages a virtual address space from 0 to buffer size. When we allocate, we get a pointer into this virtual space and convert it to an offset:

```c
void *ptr = vkr_dmemory_alloc(&buffer->offset_allocator, size);
uint64_t offset = (uintptr_t)ptr - (uintptr_t)buffer->offset_allocator.base_memory;
```

### 2. No Freelist Exposure at Buffer Level

**Decision**: Don't expose freelist directly in VulkanBuffer API.

**Rationale**:
- VkrDMemory already wraps freelist with a better API
- Reduces coupling between layers
- Simpler for buffer users (they just call allocate/free)
- Easier to change internal implementation later

### 3. Preserve-on-Resize

**Decision**: All layers preserve allocations at same offsets when resizing.

**Rationale**:
- Simplifies usage (no need to track offset changes)
- GPU buffers need data copied anyway
- Matches user expectations
- Only supports growth, not shrinking

**Resize Flow**:
1. **Freelist**: Copy active nodes to new memory, add new space as free block
2. **DMemory**: Reserve new memory, copy all data, resize freelist
3. **Buffer**: Create new VkBuffer, copy GPU data, resize dmemory

### 4. Explicit Node Memory Management

**Decision**: Freelist resize requires caller to provide new node memory.

**Rationale**:
- Freelist doesn't own memory, just uses it
- Caller controls allocation strategy (arena, malloc, dmemory)
- Clear ownership model (caller must free old memory)
- Enables different memory sources for different use cases

### 5. Embedded VkrDMemory (Not Pointer)

**Decision**: `VkrDMemory offset_allocator` is a value member, not a pointer.

**Rationale**:
- Simpler: no malloc/free needed
- Cleaner: VkrDMemory is part of VulkanBuffer's state
- Efficient: one less indirection for allocations
- Natural: offset_allocator's lifetime matches buffer's lifetime

### 6. Arena-free Freelists for Tests

**Decision**: Tests use malloc/free for freelist node memory.

**Rationale**:
- Tests need precise control over memory lifetimes
- Simpler to verify resize behavior
- Production code uses dmemory (which itself uses platform memory)

## Usage Examples

### Old Approach (One Allocation Per Buffer)

```c
// Create separate buffer for each object
VkrBufferDescription desc1 = {.size = 1024, .usage = ...};
VkrBufferDescription desc2 = {.size = 512, .usage = ...};

BufferHandle buf1 = vulkan_buffer_create(state, &desc1, NULL);
BufferHandle buf2 = vulkan_buffer_create(state, &desc2, NULL);

// Result: 2 VkBuffer objects, potential memory waste from alignment
```

### New Approach (Sub-Allocation)

```c
// Create one large buffer
VkrBufferDescription desc = {.size = 1024 * 1024, .usage = ...};
BufferHandle buf = vulkan_buffer_create(state, &desc, NULL);

// Sub-allocate within it
uint64_t offset1, offset2;
vulkan_buffer_allocate(state, &buf->buffer, 1024, &offset1);
vulkan_buffer_allocate(state, &buf->buffer, 512, &offset2);

// Use offsets for binding/rendering
vulkan_buffer_bind_vertex_buffer(state, cmd, 0, buf->buffer.handle, offset1);

// Free when done
vulkan_buffer_free(state, &buf->buffer, 1024, offset1);
vulkan_buffer_free(state, &buf->buffer, 512, offset2);

// Result: 1 VkBuffer object, efficient memory utilization
```

### Resize Example

```c
// Buffer is full
if (!vulkan_buffer_allocate(state, &buf->buffer, size, &offset)) {
  // Double the buffer size
  uint64_t new_size = buf->buffer.total_size * 2;

  if (vulkan_buffer_resize(state, new_size, &buf->buffer, queue, pool)) {
    // All existing allocations preserved at same offsets
    // Try allocation again
    vulkan_buffer_allocate(state, &buf->buffer, size, &offset);
  }
}
```

## Memory Layout

### CPU Side (DMemory for Node Storage)

```
Platform Memory (reserved/committed)
┌────────────────────────────────────┐
│  Freelist Nodes                    │
│  ┌──────────────────────┐          │
│  │ Node 0: offset=512   │          │
│  │         size=512     │          │
│  │         next=NULL    │          │
│  └──────────────────────┘          │
│  ┌──────────────────────┐          │
│  │ Node 1: INVALID      │          │
│  └──────────────────────┘          │
│  ...                               │
└────────────────────────────────────┘
```

### GPU Side (Vulkan Buffer)

```
VkDeviceMemory
┌────────────────────────────────────┐
│  Offset 0:   [ALLOCATED - 512B]   │
│              Vertex data           │
├────────────────────────────────────┤
│  Offset 512: [FREE - 512B]         │
│              Available             │
└────────────────────────────────────┘
```

The CPU-side tracking (freelist/dmemory) mirrors the GPU-side allocation state.

## Performance Considerations

### Benefits

1. **Reduced VkBuffer Count**
   - Fewer Vulkan objects to manage
   - Less driver overhead
   - Improved batching opportunities

2. **Better Memory Utilization**
   - Pack multiple allocations in one buffer
   - Reduce waste from minimum allocation sizes
   - Share committed memory across allocations

3. **Efficient Resize**
   - Grow buffer without recreating all objects
   - Preserve existing data at same offsets
   - Add new space without disrupting active allocations

### Trade-offs

1. **Fragmentation**
   - Small allocations can fragment buffer
   - Freelist coalesces adjacent blocks
   - May need compaction for extreme cases (not yet implemented)

2. **Resize Cost**
   - Must copy entire GPU buffer contents
   - Requires command buffer submission
   - Consider pre-allocating larger buffers

3. **Memory Overhead**
   - Freelist nodes stored in CPU memory
   - ~32 bytes per node (capped at 1024 nodes)
   - Negligible compared to GPU memory savings

## Future Improvements

### 1. Buffer Pools

Create pools of commonly-sized buffers to reduce fragmentation:

```c
VulkanBufferPool *pool = vulkan_buffer_pool_create(state, 256 * 1024);
uint64_t offset = vulkan_buffer_pool_allocate(pool, 1024);
```

### 2. Defragmentation

Compact buffer to eliminate fragmentation:

```c
vulkan_buffer_defragment(state, buffer, queue, pool);
// Moves allocations to eliminate gaps, updates all offsets
```

### 3. Alignment Support

Respect alignment requirements for different buffer types:

```c
vulkan_buffer_allocate_aligned(state, buffer, size, alignment, &offset);
```

### 4. Sub-Allocation Statistics

Track usage patterns for optimization:

```c
VulkanBufferStats stats;
vulkan_buffer_get_stats(buffer, &stats);
// stats.fragmentation_ratio
// stats.allocation_count
// stats.average_allocation_size
```

## Testing

### Freelist Tests

- `test_freelist_resize_empty` - Resize with no allocations
- `test_freelist_resize_with_allocations` - Preserve allocations on resize
- `test_freelist_resize_and_allocate_new` - Allocate from grown space
- `test_freelist_resize_coalescing` - Merge new space with free tail
- `test_freelist_resize_node_copy` - Verify node data integrity

### DMemory Tests

- `test_dmemory_resize_empty` - Resize with no allocations
- `test_dmemory_resize_with_allocations` - Preserve data on resize
- `test_dmemory_resize_and_allocate` - Allocate from grown space
- `test_dmemory_resize_shrink_rejected` - Reject invalid shrink operations

## References

- Kohi Engine vulkan_backend.c - Original inspiration for buffer sub-allocation
- Vulkan Memory Allocator (VMA) - Advanced allocation strategies
- dlmalloc - Free list implementation techniques

## Revision History

- **Version 1.0** (2025-10-11): Initial design and implementation
  - Freelist resize with explicit node memory management
  - DMemory resize with data preservation
  - VulkanBuffer sub-allocation API
  - Comprehensive test coverage

