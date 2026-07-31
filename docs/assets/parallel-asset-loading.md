---
status: partial
updated: 2026-07-31
authority: design
---
# Parallel Asset Loading System

This document describes the parallel mesh, material, and texture loading system implementation, including optimizations, threading considerations, and performance improvements.

## Overview

The asset loading system uses a fully parallel async-first architecture with the job system, coordinated through the resource system. This provides significant performance improvements for loading complex scenes like Sponza.

## Architecture

```
Main Thread                         Worker Threads
     │                                    │
     ├─ vkr_mesh_manager_load_batch ──────┤
     │         │                          │
     │   vkr_resource_system_load_batch ──┤
     │         │                          │
     │         └──► Submit mesh jobs ─────► Parse OBJ / Read cache
     │                   │                  (uses job context allocator)
     │              Wait for jobs ◄────────┘
     │                   │
     │              Material batch ───────► Parse .mt files
     │                   │                  (uses temp_alloc parameter)
     │              Wait for jobs ◄────────┘
     │                   │
     │              Texture batch ────────► Decode images (stb_image)
     │                   │                  (no GPU operations)
     │              Wait for jobs ◄────────┘
     │                   │
     │              GPU uploads (sequential, main thread)
     │                   │
     └───────────────────┘
```

## Key Design Principles

1. **GPU operations only on main thread** - Vulkan queue submissions and command buffer operations are not thread-safe
2. **Parallel file I/O and parsing** - Mesh parsing, material parsing, and image decoding run on worker threads
3. **Batch deduplication** - Duplicate assets are detected and loaded only once
4. **Job context allocator** - Each job uses the thread-local allocator from `VkrJobContext` for temporary allocations
5. **Resource system integration** - All batch loading goes through `vkr_resource_system_load_batch` for consistent API

## Component Changes

### Resource System (`vkr_resource_system.c`)

| Change | Before | After |
|--------|--------|-------|
| Batch loading | N/A | `vkr_resource_system_load_batch` function |
| Loader callbacks | `load`, `unload`, `can_load` | Added `batch_load` callback |
| Job system access | N/A | `vkr_resource_system_get_job_system()` for parallel loading |
| Fallback behavior | N/A | Sequential loading if `batch_load` is NULL |

### Mesh Loader (`mesh_loader.c`)

| Change | Before | After |
|--------|--------|-------|
| Resource callbacks | `load = NULL`, `unload = NULL` | Proper implementations registered |
| Memory allocation | Per-job arenas (`arena_create`) | Job context allocator (`ctx->allocator`) |
| Batch callback | N/A | `batch_load` registered with resource system |
| Result cleanup | `arena_destroy` per result | Resource system `unload` callback |

### Material Loader (`material_loader.c`)

| Change | Before | After |
|--------|--------|-------|
| Global state | `file_buffer_arena`, `file_buffer_alloc` | Removed (uses `temp_alloc` parameter) |
| Thread safety | Shared global arena | All allocations via function parameters |
| Batch callback | N/A | `batch_load` uses job system from resource system |
| Texture loading | Per-material | Batch all textures across all materials |

### Mesh Manager (`vkr_mesh_manager.c`)

| Change | Before | After |
|--------|--------|-------|
| `vkr_mesh_manager_load_batch` | Direct `vkr_mesh_loader_load_batch` call | Uses `vkr_resource_system_load_batch` |
| Result processing | `vkr_mesh_manager_process_batch_result` | `vkr_mesh_manager_process_resource_handle` |
| Result type | `VkrMeshBatchResult` | `VkrResourceHandleInfo` |

### Texture System (`vkr_texture_system.c`)

| Change | Before | After |
|--------|--------|-------|
| Loading API | Sequential | `vkr_texture_system_load_batch` |
| Image decoding | Main thread | Worker threads (parallel jobs) |
| GPU upload | Per-texture | Sequential after all decodes complete |
| Transparency check | Scan all pixels | Sample 64 evenly distributed pixels |
| Deduplication | None | System-wide + within-batch |

### Geometry System (`vkr_geometry_system.c`)

| Change | Before | After |
|--------|--------|-------|
| Vertex deduplication | O(n²) nested loops | O(n) hash-based with FNV-1a |
| Hash table size | N/A | 2x vertex count |
| Collision resolution | N/A | Linear probing |

### Job System (`vkr_job_system.c`)

| Change | Before | After |
|--------|--------|-------|
| Worker arena size | 256KB | 32MB reserve, 4MB commit |
| Slot exhaustion | Return error | Block and wait (condition variable) |
| Per-job logging | Every submission | Removed for performance |
| Max jobs | 1024 | 4096 |

## Memory Allocation Strategy

1. **Job execution**: Use `ctx->allocator` from `VkrJobContext` for all temporary allocations within jobs
2. **Parse results**: Allocate from temp_alloc, copy needed data to persistent allocator
3. **Persistent data**: Use context allocator (e.g., `VkrMeshLoaderContext.allocator`) for data that survives the load
4. **Batch coordination**: Use allocator scopes for grouping temporary allocations

## Performance Optimizations

### Deduplication

| Asset Type | Deduplication Method | Complexity |
|------------|---------------------|------------|
| Vertices | FNV-1a hash + linear probing | O(n) |
| Textures (system) | Hash table lookup | O(1) |
| Textures (batch) | Linear scan | O(n²) |
| Materials (batch) | Linear scan | O(n²) |

### Parallel Processing

| Phase | Parallel | Sequential | Reason |
|-------|----------|------------|--------|
| Mesh file read | ✓ | | File I/O |
| OBJ parsing | ✓ | | CPU-bound text parsing |
| .mt file parsing | ✓ | | CPU-bound text parsing |
| Image decoding | ✓ | | CPU-bound decompression |
| GPU buffer upload | | ✓ | Vulkan queue thread safety |
| GPU texture upload | | ✓ | Vulkan queue thread safety |
| Sampler creation | | ✓ | Vulkan object creation |

## Threading Issues Resolved

### Issue 1: GPU Operations from Worker Threads

**Problem:** Material loading during OBJ parsing triggered GPU texture uploads from worker threads, causing Vulkan validation errors:
```
vkQueueSubmit(): THREADING ERROR : object of type VkQueue is simultaneously used in current thread 0x16b6ab000 and thread 0x16b7c3000
```

**Solution:** Materials are collected during parsing and loaded after all parse jobs complete on the main thread.

### Issue 2: Shared Global Allocator

**Problem:** All parallel mesh loading jobs used a global `loader_allocator` backed by `loader_arena`. Arena allocations are not thread-safe.

**Solution:** Each job now uses the job context's thread-local allocator (`ctx->allocator`). Result data is allocated from the context's allocator which is set up during loader registration.

### Issue 3: Material Loader Global Arena

**Problem:** The material loader used global `file_buffer_arena` and `file_buffer_alloc` which are not thread-safe for parallel parsing.

**Solution:** Removed global arenas. All allocations now use the `temp_alloc` parameter passed to load functions.

### Issue 4: Sampler Limit Exhaustion

**Problem:** Vulkan validation error when creating more than 1024 samplers (hardware limit on Apple M1):
```
vkCreateSampler(): Number of currently valid sampler objects (1024) is not less than the maximum allowed (1024)
```

**Solution:** Implemented texture deduplication in `vkr_texture_system_load_batch` to prevent loading duplicate textures.

### Issue 5: Job Slot Exhaustion

**Problem:** Complex scenes submitted more jobs than available slots (1024), causing "No free slots" errors.

**Solution:**
1. Increased max jobs to 4096
2. Modified `vkr_job_submit` to block and wait using condition variable when slots exhausted

## API Reference

### Resource System Batch Loading

```c
// Generic batch loading through resource system
uint32_t vkr_resource_system_load_batch(
    VkrResourceType type,
    const String8 *paths,
    uint32_t count,
    VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors);
```

### Mesh Loading

```c
// Primary API - loads multiple meshes via resource system
uint32_t vkr_mesh_manager_load_batch(
    VkrMeshManager *manager,
    const VkrMeshLoadDesc *descs,
    uint32_t count,
    uint32_t *out_indices,
    VkrRendererError *out_errors);

// Single mesh convenience wrapper (uses batch internally)
bool8_t vkr_mesh_manager_load(
    VkrMeshManager *manager,
    const VkrMeshLoadDesc *desc,
    uint32_t *out_first_index,
    uint32_t *out_mesh_count,
    VkrRendererError *out_error);
```

### Material Loading

```c
// Single material loading through resource system
vkr_resource_system_load(
    VKR_RESOURCE_TYPE_MATERIAL,
    material_path,
    temp_alloc,
    &out_handle,   // VkrResourceHandleInfo
    &out_error);

// Batch material loading through resource system (uses job system internally)
vkr_resource_system_load_batch(
    VKR_RESOURCE_TYPE_MATERIAL,
    material_paths,
    count,
    temp_alloc,
    out_handles,    // VkrResourceHandleInfo array
    out_errors);
```

Note: The resource system's batch loading callback uses `vkr_resource_system_get_job_system()`
to access the job system for parallel loading. All batch loading APIs are internal.

### Texture Loading

```c
// Batch texture loading with parallel decoding
uint32_t vkr_texture_system_load_batch(
    VkrTextureSystem *system,
    const String8 *paths,
    uint32_t count,
    VkrTextureHandle *out_handles,
    VkrRendererError *out_errors);
```

## Future Improvements

| Improvement | Impact | Complexity |
|-------------|--------|------------|
| Hash-based material/texture deduplication | O(n²) → O(n) | Medium |
| Async GPU uploads with staging buffers | Better parallelism | High |
| Mesh instancing | Reduce GPU memory | Medium |
| Compressed texture formats (BC/ASTC) | Faster GPU uploads | Medium |
| Memory-mapped file I/O | Reduced copies | Low |

## Files Modified

| File | Changes |
|------|---------|
| `lib/src/renderer/systems/vkr_resource_system.h` | Added `batch_load` callback, job system param, `vkr_resource_system_get_job_system` |
| `lib/src/renderer/systems/vkr_resource_system.c` | Job system storage, `vkr_resource_system_load_batch`, getter |
| `lib/src/renderer/resources/loaders/mesh_loader.c` | Proper load/unload/batch_load callbacks, job context allocator, internal types |
| `lib/src/renderer/resources/loaders/mesh_loader.h` | Minimal public API (factory function only) |
| `lib/src/renderer/resources/loaders/material_loader.c` | Removed global arenas, batch_load uses resource system job system |
| `lib/src/renderer/resources/loaders/material_loader.h` | Factory function only, all batch APIs internal |
| `lib/src/renderer/systems/vkr_texture_system.c` | Batch loading, deduplication |
| `lib/src/renderer/systems/vkr_mesh_manager.c` | Uses resource system batch API |
| `lib/src/renderer/systems/vkr_geometry_system.c` | O(n) vertex deduplication |
| `lib/src/core/vkr_job_system.c` | Larger arenas, blocking submit |
