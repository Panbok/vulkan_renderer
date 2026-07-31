---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../architecture/renderer-architecture-spec.md`](../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Renderer Frontend Architecture Analysis and Refactoring Guide

**Document Version**: 1.0
**Date**: 2026-01-08
**Target Audience**: LLM assistants and developers working on vulkan_renderer

**Note:** The view/layer system has been removed. Render orchestration now uses the render graph and the stateless packet API (`vkr_renderer_prepare_frame` + `vkr_renderer_submit_packet`), with pass executors consuming packet payloads and stateless resource owners.

## Executive Summary

This document provides a comprehensive analysis of the current renderer frontend implementation (`renderer_frontend.c/h`, `vkr_renderer.h`) and its integration with the Vulkan backend. It identifies architectural issues, proposes refactoring strategies, and outlines a path toward multithreaded rendering and Forward+ lighting evolution.

---

## Table of Contents

1. [Current Architecture Overview](#1-current-architecture-overview)
2. [Identified Issues and Drawbacks](#2-identified-issues-and-drawbacks)
3. [Inconsistencies and Technical Debt](#3-inconsistencies-and-technical-debt)
4. [Refactoring Recommendations](#4-refactoring-recommendations)
5. [Multithreading Strategy](#5-multithreading-strategy)
6. [Forward to Forward+ Evolution](#6-forward-to-forward-evolution)
7. [Implementation Phases](#7-implementation-phases)
8. [API Changes Summary](#8-api-changes-summary)

---

## 1. Current Architecture Overview

### 1.1 Component Hierarchy

```
RendererFrontend (renderer_frontend.h)
├── Memory Management
│   ├── Arena *arena (6 MB persistent)
│   ├── Arena *scratch_arena (1 MB temporary)
│   ├── VkrAllocator allocator
│   └── VkrAllocator scratch_allocator
├── Backend Interface
│   ├── void *backend_state
│   ├── VkrRendererBackendType backend_type
│   └── VkrRendererBackendInterface backend
├── Subsystems
│   ├── VkrPipelineRegistry pipeline_registry
│   ├── VkrShaderSystem shader_system
│   ├── VkrGeometrySystem geometry_system
│   ├── VkrTextureSystem texture_system
│   ├── VkrMaterialSystem material_system
│   ├── VkrViewSystem view_system
│   ├── VkrFontSystem font_system
│   ├── VkrCameraSystem camera_system
│   └── VkrMeshManager mesh_manager
├── Resource Loaders
│   ├── VkrMeshLoaderContext mesh_loader
│   ├── VkrBitmapFontLoaderContext bitmap_font_loader
│   ├── VkrSystemFontLoaderContext system_font_loader
│   └── VkrMtsdfFontLoaderContext mtsdf_font_loader
└── Frame State
    ├── VkrPickingContext picking
    ├── VkrShaderStateObject draw_state
    ├── VkrGlobalMaterialState globals
    ├── bool32_t frame_active
    ├── uint64_t frame_number
    └── VkrAtomicUint64 pending_resize_mailbox
```

### 1.2 Data Flow

```
Application
    │
    ▼
vkr_renderer_prepare_frame()
    │
    ├─► Acquire swapchain image
    ├─► Wait for in-flight fence
    └─► Reset command buffer state

    │
    ▼
Build RenderPacket (per-frame state only)

    │
    ▼
vkr_renderer_submit_packet()
    │
    ├─► vkr_rg_begin_frame()
    ├─► Build graph from JSON
    ├─► vkr_rg_execute()
    │     └─► Pass executors consume packet payloads
    └─► vkr_renderer_end_frame()
    │
    └─► backend.end_frame()
        ├─► Submit command buffer
        ├─► Present swapchain image
        └─► Update readback ring
```

### 1.3 Backend Interface Contract

The `VkrRendererBackendInterface` (vkr_renderer.h:1265-1398) defines the abstraction layer:

```c
typedef struct VkrRendererBackendInterface {
  // Lifecycle
  bool32_t (*initialize)(void **out_backend_state, ...);
  void (*shutdown)(void *backend_state);
  void (*on_resize)(void *backend_state, uint32_t width, uint32_t height);

  // Frame management
  VkrRendererError (*begin_frame)(void *backend_state, float64_t delta_time);
  VkrRendererError (*end_frame)(void *backend_state, float64_t delta_time);

  // Resource creation/destruction
  VkrBackendResourceHandle (*buffer_create)(...);
  VkrBackendResourceHandle (*texture_create)(...);
  VkrBackendResourceHandle (*graphics_pipeline_create)(...);
  // ... and destroy counterparts

  // Rendering
  void (*draw)(void *backend_state, ...);
  void (*draw_indexed)(void *backend_state, ...);

  // State management
  VkrRendererError (*pipeline_update_state)(...);
  VkrRendererError (*instance_state_acquire)(...);
  VkrRendererError (*instance_state_release)(...);
} VkrRendererBackendInterface;
```

---

## 2. Identified Issues and Drawbacks

### 2.1 CRITICAL: Global Static State

**Location**: `renderer_frontend.c:21`

```c
static RendererFrontend *g_renderer_rt_refresh = NULL;
```

**Problem**: This global pointer is used for render target refresh callbacks but creates several issues:

1. **Not thread-safe**: No synchronization when accessed/modified
2. **Single-instance assumption**: Breaks if multiple renderer instances exist
3. **Hidden dependency**: Callback `renderer_frontend_on_target_refresh_required()` relies on this implicitly

**Impact**: Race conditions, undefined behavior with multiple renderers

**Severity**: CRITICAL for multithreading

---

### 2.2 CRITICAL: Non-Atomic Reference Counting

**Locations**:
- `vkr_pipeline_registry.h:36`: `uint32_t ref_count`
- `vkr_material_system.h:26`: `uint32_t ref_count`
- `vkr_texture_system.h` (similar pattern)

**Problem**: Reference counts are plain `uint32_t`, not atomic:

```c
typedef struct VkrPipelineEntry {
  uint32_t id;
  uint32_t ref_count;  // NOT ATOMIC
  bool8_t auto_release;
  const char *name;
  VkrPipelineDomain domain;
} VkrPipelineEntry;
```

**Impact**: Data races when incrementing/decrementing from multiple threads

**Severity**: CRITICAL for multithreading

---

### 2.3 HIGH: Monolithic Frontend Structure

**Location**: `renderer_frontend.h:25-97`

**Problem**: The `RendererFrontend` struct contains everything:

```c
struct s_RendererFrontend {
  // Memory (4 fields)
  Arena *arena;
  VkrAllocator allocator;
  Arena *scratch_arena;
  VkrAllocator scratch_allocator;

  // Backend (4 fields)
  VkrWindow *window;
  EventManager *event_manager;
  void *backend_state;
  VkrRendererBackendInterface backend;

  // 8 subsystems as direct members
  VkrPipelineRegistry pipeline_registry;
  VkrShaderSystem shader_system;
  VkrGeometrySystem geometry_system;
  VkrTextureSystem texture_system;
  VkrMaterialSystem material_system;
  VkrViewSystem view_system;
  VkrFontSystem font_system;
  VkrCameraSystem camera_system;

  // Mesh manager + 4 loaders
  VkrMeshManager mesh_manager;
  VkrMeshLoaderContext mesh_loader;
  // ... more loaders

  // Frame state (12+ fields)
  VkrPickingContext picking;
  VkrShaderStateObject draw_state;
  VkrGlobalMaterialState globals;
  bool32_t frame_active;
  uint64_t frame_number;
  // ...
};
```

**Issues**:
1. **Tight coupling**: All systems directly embedded, hard to test in isolation
2. **Initialization order dependency**: Systems must init/shutdown in specific order
3. **Memory layout**: Large struct (likely >1KB), poor cache locality
4. **Single responsibility violation**: Frontend does too much

**Severity**: HIGH for maintainability

---

### 2.4 HIGH: Inconsistent Error Handling

**Problem**: Multiple error handling patterns coexist:

**Pattern 1**: Return bool + out_error parameter
```c
bool32_t vkr_renderer_initialize(..., VkrRendererError *out_error);
```

**Pattern 2**: Return error directly
```c
VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer);
```

**Pattern 3**: Return handle (NULL = error)
```c
VkrBufferHandle vkr_renderer_create_buffer(..., VkrRendererError *out_error);
```

**Pattern 4**: Void with assert
```c
void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer);
// Uses assert_log internally, no error return
```

**Impact**: Inconsistent API, caller confusion, missed error handling

**Severity**: HIGH

---

### 2.5 HIGH: Memory Accumulation on Scene Reload

**Problem**: Arena-based allocation with high-water mark semantics:

```c
// Scene 1 loads: arena grows to 50 MB
// Scene 1 unloads: arena NOT freed (high-water mark stays)
// Scene 2 loads: arena grows to 75 MB total
// Repeat: unbounded growth
```

**Affected Systems**:
- `VkrPipelineRegistry`: Uses `pipeline_arena`
- `VkrMaterialSystem`: Uses `arena`
- `VkrTextureSystem`: Uses `arena`

**Current Mitigation** (from CLAUDE.md):
- DMemory for per-item-free structures
- Pools for bounded resources
- Scope-based allocation

**Missing**:
- No automatic arena reset between scenes
- No explicit "unload all" API

**Severity**: HIGH for long-running applications

---

### 2.6 MEDIUM: Domain Render Pass State Machine Complexity

**Location**: `vulkan_types.h:387-408`

**Problem**: 9 pipeline domains with automatic transitions:

```c
typedef enum VkrPipelineDomain {
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  VKR_PIPELINE_DOMAIN_SHADOW = 2,
  VKR_PIPELINE_DOMAIN_POST = 3,
  VKR_PIPELINE_DOMAIN_COMPUTE = 4,
  VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT = 5,
  VKR_PIPELINE_DOMAIN_SKYBOX = 6,
  VKR_PIPELINE_DOMAIN_PICKING = 7,
  VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT = 8,
  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;
```

State tracked in backend:
```c
VkrPipelineDomain current_render_pass_domain;
bool8_t render_pass_active;
bool8_t swapchain_image_is_present_ready;
```

**Issues**:
1. Implicit state transitions based on pipeline domain changes
2. No clear documentation of valid transitions
3. Risk of invalid state combinations

**Severity**: MEDIUM

---

### 2.7 MEDIUM: Hash Table Key Lifetime Management

**Problem**: String keys stored as pointers require manual lifetime management:

```c
typedef struct VkrMaterialEntry {
  uint32_t id;
  uint32_t ref_count;
  bool8_t auto_release;
  const char *name;  // Points to allocated string
} VkrMaterialEntry;
```

**Requirement** (from CLAUDE.md):
> Free keys **after** calling hash_remove (removal probes compare against stored key pointer)

**Risk**: Use-after-free if key freed before removal, or memory leak if key never freed

**Severity**: MEDIUM

---

### 2.8 MEDIUM: Resize Event Mailbox Overwrite

**Location**: `renderer_frontend.c:49-52`, `renderer_frontend.c:918-926`

```c
// On resize event (can happen any time):
uint64_t packed = ((uint64_t)resize->width << 32) | (uint64_t)resize->height;
vkr_atomic_uint64_store(&rf->pending_resize_mailbox, packed,
                        VKR_MEMORY_ORDER_RELEASE);

// On begin_frame:
uint64_t packed = vkr_atomic_uint64_exchange(
    &renderer->pending_resize_mailbox, 0, VKR_MEMORY_ORDER_ACQ_REL);
```

**Problem**: If two resize events occur before `begin_frame`, the first is lost.

**Impact**: Potential visual artifacts during rapid resizing

**Severity**: MEDIUM (cosmetic issue)

---

### 2.9 LOW: Descriptor Set Frame Count Coupling

**Location**: `vulkan_types.h:23`

```c
#define BUFFERING_FRAMES 3
```

**Problem**: Many structures hard-code arrays of size 3:

```c
typedef struct VulkanShaderObjectInstanceState {
  VkDescriptorSet *descriptor_sets;  // length == frame_count
  uint32_t *global_descriptor_generations;  // length == frame_count
  // ...
};
```

**Impact**: Changing buffering frames requires recompilation and careful audit

**Severity**: LOW

---

### 2.10 LOW: Allocator Non-Thread-Safety

**Location**: `memory/vkr_allocator.h:114-116`

```c
// @note Thread Safety: Individual VkrAllocator instances are NOT thread-safe.
//       Each allocator should be used from a single thread, or callers must
//       provide external synchronization.
```

**Current Mitigation**: Job workers use scoped allocators

**Risk**: Frontend system modifications during job execution

**Severity**: LOW (currently single-threaded)

---

## 3. Inconsistencies and Technical Debt

### 3.1 API Naming Inconsistencies

| Pattern | Examples |
|---------|----------|
| `vkr_renderer_*` | `vkr_renderer_create_buffer`, `vkr_renderer_begin_frame` |
| `vkr_*_system_*` | `vkr_texture_system_init`, `vkr_material_system_acquire` |
| `vkr_rg_*` | `vkr_rg_execute` |
| `vkr_pipeline_registry_*` | `vkr_pipeline_registry_bind_pipeline` |

**Inconsistency**: Some functions take `VkrRendererFrontendHandle`, others take specific system pointers.

**Recommendation**: Standardize on passing system pointers when operating on specific systems.

---

### 3.2 Handle Type Inconsistencies

| Type | Definition |
|------|------------|
| `VkrBufferHandle` | `struct s_BufferResource *` (raw pointer) |
| `VkrPipelineOpaqueHandle` | `struct s_Pipeline *` (raw pointer) |
| `VkrTextureOpaqueHandle` | `struct s_TextureHandle *` (raw pointer) |
| `VkrPipelineHandle` | `{uint32_t id; uint32_t generation}` (ID + generation) |
| `VkrMaterialHandle` | `{uint32_t id; uint32_t generation}` (ID + generation) |
| `VkrTextureHandle` | `{uint32_t id; uint32_t generation}` (ID + generation) |

**Problem**: Two handle paradigms coexist:
1. **Opaque pointers**: Direct backend resource access, no validation
2. **ID + generation**: Safe handle pattern with validity checking

**Recommendation**: Migrate all handles to ID + generation pattern.

---

### 3.3 Missing Validation in Public APIs

**Example**: `vkr_renderer_destroy_buffer` (renderer_frontend.c:473-482)

```c
void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  // No validation that buffer belongs to this renderer
  // No validation that buffer isn't in use

  VkrBackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.buffer_destroy(renderer->backend_state, handle);
}
```

**Risk**: Destroying in-use resources causes GPU errors

**Recommendation**: Add resource tracking and validation

---

### 3.4 Subsystem Initialization Order Dependencies

Current order in `vkr_renderer_systems_initialize`:

```c
1. vkr_camera_registry_init
2. vkr_pipeline_registry_init
3. vkr_shader_system_initialize
4. vkr_resource_system_init
5. vkr_geometry_system_init
6. vkr_texture_system_init
7. vkr_material_system_init
8. vkr_mesh_manager_init
9. vkr_font_system_init
10. vkr_rg_begin_frame / vkr_rg_execute
11. Register built-in views (skybox, world, UI, editor)
12. vkr_picking_init
```

**Problem**: Order not documented, dependencies implicit

**Recommendation**: Create explicit dependency graph and validate at init time

---

### 3.5 Duplicated Constants

```c
// vulkan_types.h
#define VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT 4096

// vkr_pipeline_registry.h
#define VKR_MAX_PIPELINE_COUNT 1024

// vkr_material_system.h (implicit)
// max_material_count = 1024 (from config)
```

**Problem**: Related limits scattered across files

**Recommendation**: Centralize configuration in a single header

---

## 4. Refactoring Recommendations

### 4.1 Introduce Renderer Context Pattern

**Current**:
```c
struct s_RendererFrontend {
  // Everything embedded directly
  VkrPipelineRegistry pipeline_registry;
  VkrTextureSystem texture_system;
  // ... 8+ systems
};
```

**Proposed**:
```c
// Forward declarations for systems
typedef struct VkrResourceContext VkrResourceContext;
typedef struct VkrRenderContext VkrRenderContext;

struct s_RendererFrontend {
  // Core state
  VkrAllocator *allocator;
  VkrWindow *window;
  EventManager *event_manager;

  // Backend abstraction
  VkrBackend *backend;

  // Separated contexts
  VkrResourceContext *resources;  // Textures, materials, meshes, shaders
  VkrRenderContext *render;       // Pipeline registry, view system, cameras
  VkrFrameContext *frame;         // Per-frame state, picking, globals

  // Thread safety
  VkrMutex mutex;
  VkrAtomicUint32 state;
};

// Resource context groups related systems
typedef struct VkrResourceContext {
  VkrAllocator allocator;
  VkrTextureSystem textures;
  VkrMaterialSystem materials;
  VkrMeshManager meshes;
  VkrShaderSystem shaders;
  VkrGeometrySystem geometries;
  VkrFontSystem fonts;
} VkrResourceContext;
```

**Benefits**:
- Clear separation of concerns
- Easier to reason about ownership
- Systems can be tested in isolation
- Enables per-context locking for multithreading

---

### 4.2 Unified Handle System

**Proposed Handle Structure**:
```c
typedef struct VkrHandle {
  uint32_t index;       // Slot index
  uint32_t generation;  // Validity counter
  uint32_t type;        // Resource type enum
  uint32_t flags;       // Reserved for future use
} VkrHandle;

#define VKR_HANDLE_INVALID ((VkrHandle){0, 0, 0, 0})

// Type-safe wrappers
typedef VkrHandle VkrBufferHandle;
typedef VkrHandle VkrTextureHandle;
typedef VkrHandle VkrPipelineHandle;
typedef VkrHandle VkrMaterialHandle;

// Validation
static inline bool vkr_handle_is_valid(VkrHandle h) {
  return h.generation != 0;
}
```

**Handle Validation Function**:
```c
bool vkr_validate_handle(VkrResourceContext *ctx, VkrHandle handle) {
  if (handle.generation == 0) return false;

  switch (handle.type) {
    case VKR_RESOURCE_TYPE_BUFFER:
      return validate_buffer_handle(ctx, handle);
    case VKR_RESOURCE_TYPE_TEXTURE:
      return validate_texture_handle(ctx, handle);
    // ...
  }
  return false;
}
```

---

### 4.3 Atomic Reference Counting

**Proposed Change**:
```c
#include "core/vkr_atomic.h"

typedef struct VkrPipelineEntry {
  uint32_t id;
  VkrAtomicUint32 ref_count;  // Now atomic
  VkrAtomicUint8 auto_release;
  const char *name;
  VkrPipelineDomain domain;
} VkrPipelineEntry;

// Thread-safe reference management
static inline uint32_t vkr_entry_acquire(VkrPipelineEntry *entry) {
  return vkr_atomic_uint32_fetch_add(&entry->ref_count, 1,
                                     VKR_MEMORY_ORDER_ACQ_REL) + 1;
}

static inline uint32_t vkr_entry_release(VkrPipelineEntry *entry) {
  return vkr_atomic_uint32_fetch_sub(&entry->ref_count, 1,
                                     VKR_MEMORY_ORDER_ACQ_REL) - 1;
}
```

---

### 4.4 Eliminate Global State

**Current**:
```c
static RendererFrontend *g_renderer_rt_refresh = NULL;

vkr_internal void renderer_frontend_on_target_refresh_required(void) {
  if (g_renderer_rt_refresh) {
    renderer_frontend_regenerate_render_targets(g_renderer_rt_refresh);
  }
}
```

**Proposed**:
```c
// Callback with context
typedef void (*VkrRenderTargetRefreshCallback)(void *user_data);

typedef struct VkrBackendCallbacks {
  VkrRenderTargetRefreshCallback on_target_refresh;
  void *user_data;
} VkrBackendCallbacks;

// In backend config
typedef struct VkrRendererBackendConfig {
  const char *application_name;
  uint16_t renderpass_count;
  VkrRenderPassConfig *pass_configs;
  VkrBackendCallbacks callbacks;  // Context-aware callbacks
} VkrRendererBackendConfig;

// Usage
static void renderer_on_target_refresh(void *user_data) {
  RendererFrontend *rf = (RendererFrontend *)user_data;
  renderer_frontend_regenerate_render_targets(rf);
}

// In initialize:
VkrRendererBackendConfig config = {
  .callbacks = {
    .on_target_refresh = renderer_on_target_refresh,
    .user_data = renderer
  }
};
```

---

### 4.5 Standardized Error Handling

**Proposed Error Pattern**:
```c
typedef struct VkrResult {
  VkrRendererError error;
  union {
    VkrHandle handle;
    void *ptr;
    uint64_t value;
  };
} VkrResult;

#define VKR_OK(handle_val) \
  ((VkrResult){.error = VKR_RENDERER_ERROR_NONE, .handle = (handle_val)})

#define VKR_ERR(err) \
  ((VkrResult){.error = (err), .handle = VKR_HANDLE_INVALID})

#define VKR_SUCCEEDED(result) ((result).error == VKR_RENDERER_ERROR_NONE)
#define VKR_FAILED(result) ((result).error != VKR_RENDERER_ERROR_NONE)

// Usage
VkrResult vkr_buffer_create(VkrResourceContext *ctx,
                            const VkrBufferDescription *desc,
                            const void *initial_data);

// Caller:
VkrResult result = vkr_buffer_create(ctx, &desc, data);
if (VKR_FAILED(result)) {
  log_error("Buffer creation failed: %s",
            vkr_renderer_get_error_string(result.error));
  return result.error;
}
VkrBufferHandle buffer = result.handle;
```

---

### 4.6 Resource Lifecycle Management

**Proposed Deferred Destruction Queue**:
```c
typedef struct VkrDeferredDestroy {
  VkrHandle handle;
  uint64_t frame_submitted;
} VkrDeferredDestroy;

typedef struct VkrDeferredDestroyQueue {
  VkrDeferredDestroy *entries;
  uint32_t count;
  uint32_t capacity;
  VkrMutex mutex;
} VkrDeferredDestroyQueue;

// Queue destruction (thread-safe)
void vkr_queue_destroy(VkrDeferredDestroyQueue *queue,
                       VkrHandle handle,
                       uint64_t current_frame) {
  vkr_mutex_lock(queue->mutex);
  queue->entries[queue->count++] = (VkrDeferredDestroy){
    .handle = handle,
    .frame_submitted = current_frame
  };
  vkr_mutex_unlock(queue->mutex);
}

// Process at end of frame (after GPU idle for N frames)
void vkr_process_deferred_destroys(VkrDeferredDestroyQueue *queue,
                                   uint64_t current_frame) {
  vkr_mutex_lock(queue->mutex);
  for (uint32_t i = 0; i < queue->count; ) {
    if (current_frame - queue->entries[i].frame_submitted >= BUFFERING_FRAMES) {
      // Safe to destroy
      vkr_destroy_resource_immediate(queue->entries[i].handle);
      // Remove from queue (swap with last)
      queue->entries[i] = queue->entries[--queue->count];
    } else {
      i++;
    }
  }
  vkr_mutex_unlock(queue->mutex);
}
```

---

### 4.7 Arena Reset Support

**Proposed Scene Lifecycle**:
```c
typedef struct VkrSceneContext {
  Arena *arena;           // Scene-scoped allocations
  VkrAllocator allocator;
  uint64_t load_frame;
} VkrSceneContext;

VkrResult vkr_scene_create(VkrResourceContext *ctx, VkrSceneContext *out_scene) {
  out_scene->arena = arena_create(MB(32));
  if (!out_scene->arena) {
    return VKR_ERR(VKR_RENDERER_ERROR_OUT_OF_MEMORY);
  }
  out_scene->allocator = (VkrAllocator){.ctx = out_scene->arena};
  vkr_allocator_arena(&out_scene->allocator);
  out_scene->load_frame = ctx->renderer->frame_number;
  return VKR_OK(VKR_HANDLE_INVALID);
}

void vkr_scene_destroy(VkrResourceContext *ctx, VkrSceneContext *scene) {
  // Release all scene resources
  vkr_scene_release_all_resources(ctx, scene);

  // Destroy scene arena (bulk free)
  arena_destroy(scene->arena);
  *scene = (VkrSceneContext){0};
}
```

---

## 5. Multithreading Strategy

### 5.1 Thread Model

```
┌─────────────────────────────────────────────────────────────────┐
│                        MAIN THREAD                              │
│  - Window events                                                │
│  - Input processing                                             │
│  - Game logic / scene updates                                   │
│  - Resource loading initiation                                  │
│  - Frame synchronization                                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       RENDER THREAD                             │
│  - Command buffer recording                                     │
│  - Pipeline binding                                             │
│  - Draw calls                                                   │
│  - Vulkan API calls (single-threaded requirement)               │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                     WORKER THREADS (N)                          │
│  - Asset decoding (textures, meshes)                            │
│  - Material compilation                                         │
│  - Scene graph updates                                          │
│  - Frustum culling                                              │
│  - LOD selection                                                │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Synchronization Points

```c
typedef enum VkrRendererState {
  VKR_RENDERER_STATE_IDLE = 0,
  VKR_RENDERER_STATE_UPDATING,
  VKR_RENDERER_STATE_RENDERING,
  VKR_RENDERER_STATE_PRESENTING,
} VkrRendererState;

typedef struct VkrFrameSync {
  VkrAtomicUint32 state;
  VkrCondVar update_complete;
  VkrCondVar render_complete;
  VkrMutex mutex;
} VkrFrameSync;

// Main thread signals update complete
void vkr_frame_signal_update_complete(VkrFrameSync *sync) {
  vkr_mutex_lock(sync->mutex);
  vkr_atomic_uint32_store(&sync->state, VKR_RENDERER_STATE_RENDERING,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_condvar_signal(&sync->update_complete);
  vkr_mutex_unlock(sync->mutex);
}

// Render thread waits for update
void vkr_frame_wait_for_update(VkrFrameSync *sync) {
  vkr_mutex_lock(sync->mutex);
  while (vkr_atomic_uint32_load(&sync->state, VKR_MEMORY_ORDER_ACQUIRE)
         != VKR_RENDERER_STATE_RENDERING) {
    vkr_condvar_wait(&sync->update_complete, sync->mutex);
  }
  vkr_mutex_unlock(sync->mutex);
}
```

### 5.3 Command Buffer Parallel Recording

**Proposed Secondary Command Buffer Pattern**:
```c
typedef struct VkrRenderBatch {
  VkrPipelineHandle pipeline;
  VkrHandle *objects;      // Array of drawable handles
  uint32_t object_count;
  Mat4 view_projection;
} VkrRenderBatch;

typedef struct VkrParallelRenderContext {
  VkrRenderBatch *batches;
  uint32_t batch_count;
  VkCommandBuffer *secondary_buffers;  // One per worker
  VkrAtomicUint32 next_batch;
} VkrParallelRenderContext;

// Worker function
void vkr_record_batch_worker(void *arg) {
  VkrParallelRenderContext *ctx = (VkrParallelRenderContext *)arg;
  uint32_t worker_id = vkr_get_worker_id();
  VkCommandBuffer cmd = ctx->secondary_buffers[worker_id];

  while (true) {
    uint32_t batch_idx = vkr_atomic_uint32_fetch_add(
        &ctx->next_batch, 1, VKR_MEMORY_ORDER_RELAXED);
    if (batch_idx >= ctx->batch_count) break;

    VkrRenderBatch *batch = &ctx->batches[batch_idx];
    vkr_record_batch_commands(cmd, batch);
  }
}
```

### 5.4 Thread-Safe Resource Access

**Read-Write Lock Pattern**:
```c
typedef struct VkrResourceLock {
  VkrRWLock lock;
  VkrAtomicUint32 pending_destroys;
} VkrResourceLock;

// Read access (multiple readers allowed)
void *vkr_resource_read_lock(VkrResourceContext *ctx, VkrHandle handle) {
  vkr_rwlock_read_lock(&ctx->lock);
  void *ptr = vkr_resolve_handle(ctx, handle);
  if (!ptr) {
    vkr_rwlock_read_unlock(&ctx->lock);
    return NULL;
  }
  return ptr;  // Caller must unlock
}

void vkr_resource_read_unlock(VkrResourceContext *ctx) {
  vkr_rwlock_read_unlock(&ctx->lock);
}

// Write access (exclusive)
void vkr_resource_write_lock(VkrResourceContext *ctx) {
  vkr_rwlock_write_lock(&ctx->lock);
}

void vkr_resource_write_unlock(VkrResourceContext *ctx) {
  vkr_rwlock_write_unlock(&ctx->lock);
}
```

---

## 6. Forward to Forward+ Evolution

### 6.1 Current Forward Rendering Path

```
For each object:
    Bind pipeline
    Update global uniforms (view, projection)
    Update instance uniforms (model, material)
    Draw
```

### 6.2 Forward+ Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    DEPTH PRE-PASS (Optional)                    │
│  - Renders depth only                                           │
│  - Enables early-Z for main pass                                │
│  - Required for light culling accuracy                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                     LIGHT CULLING (Compute)                     │
│  - Divide screen into tiles (e.g., 16x16 pixels)                │
│  - For each tile, determine affecting lights                    │
│  - Output: per-tile light index list                            │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      FORWARD+ MAIN PASS                         │
│  - Sample light list for current pixel's tile                   │
│  - Accumulate lighting from relevant lights only                │
│  - Transparent objects handled separately                       │
└─────────────────────────────────────────────────────────────────┘
```

### 6.3 Required Infrastructure Changes

**1. Compute Pipeline Support**:
```c
typedef struct VkrComputePipelineDescription {
  String8 shader_path;
  String8 entry_point;
  uint64_t push_constant_size;
  uint32_t local_size_x;
  uint32_t local_size_y;
  uint32_t local_size_z;
} VkrComputePipelineDescription;

VkrResult vkr_compute_pipeline_create(
    VkrResourceContext *ctx,
    const VkrComputePipelineDescription *desc);
```

**2. Storage Buffer Support**:
```c
typedef enum VkrBufferUsageBits {
  // Existing
  VKR_BUFFER_USAGE_VERTEX_BUFFER = 1 << 0,
  VKR_BUFFER_USAGE_INDEX_BUFFER = 1 << 1,
  VKR_BUFFER_USAGE_UNIFORM = 1 << 3,
  // New for Forward+
  VKR_BUFFER_USAGE_STORAGE = 1 << 4,       // SSBO
  VKR_BUFFER_USAGE_INDIRECT = 1 << 7,      // Indirect draw
} VkrBufferUsageBits;
```

**3. Light Culling System**:
```c
typedef struct VkrLightCullingConfig {
  uint32_t tile_size;           // Typically 16
  uint32_t max_lights_per_tile;  // Typically 256
  bool8_t use_depth_bounds;     // Tighter culling
} VkrLightCullingConfig;

typedef struct VkrLightCullingSystem {
  VkrComputePipelineHandle cull_pipeline;
  VkrBufferHandle light_buffer;       // All lights
  VkrBufferHandle tile_light_indices; // Per-tile light lists
  VkrBufferHandle tile_light_counts;  // Lights per tile
  VkrTextureHandle depth_texture;     // For depth bounds
  uint32_t tile_count_x;
  uint32_t tile_count_y;
} VkrLightCullingSystem;
```

**4. Light Data Structure**:
```c
typedef struct VkrLight {
  Vec3 position;
  float radius;
  Vec3 color;
  float intensity;
  Vec3 direction;  // For spot lights
  float cone_angle;
  uint32_t type;   // Point, spot, directional
  uint32_t flags;
  uint32_t _pad[2];
} VkrLight;  // 64 bytes, aligned

typedef struct VkrLightGrid {
  uint32_t tile_size;
  uint32_t tiles_x;
  uint32_t tiles_y;
  uint32_t max_lights_per_tile;
  // Followed by tile data
} VkrLightGrid;
```

### 6.4 Shader Changes for Forward+

**Light Culling Compute Shader** (pseudocode):
```glsl
// light_cull.comp
layout(local_size_x = 16, local_size_y = 16) in;

shared uint s_minDepth;
shared uint s_maxDepth;
shared uint s_lightCount;
shared uint s_lightIndices[MAX_LIGHTS_PER_TILE];

void main() {
    // 1. Compute tile depth bounds from depth buffer
    // 2. Create tile frustum
    // 3. For each light, test against frustum
    // 4. Add passing lights to tile list
    // 5. Write to output buffers
}
```

**Forward+ Fragment Shader** (pseudocode):
```glsl
// forward_plus.frag
layout(set = 2, binding = 0) readonly buffer LightBuffer {
    Light lights[];
};
layout(set = 2, binding = 1) readonly buffer TileLightIndices {
    uint lightIndices[];
};

void main() {
    // Get tile index from gl_FragCoord
    uvec2 tile = uvec2(gl_FragCoord.xy) / TILE_SIZE;
    uint tileIndex = tile.y * tilesX + tile.x;

    // Read light count for this tile
    uint lightCount = tileLightCounts[tileIndex];
    uint lightOffset = tileIndex * MAX_LIGHTS_PER_TILE;

    // Accumulate lighting
    vec3 lighting = ambientColor;
    for (uint i = 0; i < lightCount; i++) {
        uint lightIdx = lightIndices[lightOffset + i];
        Light light = lights[lightIdx];
        lighting += calculateLight(light, ...);
    }

    outColor = vec4(albedo * lighting, alpha);
}
```

### 6.5 Integration with Current Architecture

**New Pipeline Domains**:
```c
typedef enum VkrPipelineDomain {
  // Existing
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  // ...

  // New for Forward+
  VKR_PIPELINE_DOMAIN_DEPTH_PREPASS = 10,
  VKR_PIPELINE_DOMAIN_LIGHT_CULL = 11,  // Compute
  VKR_PIPELINE_DOMAIN_FORWARD_PLUS = 12,

  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;
```

**Frame Graph Integration**:
```c
// Declarative render pass ordering
typedef struct VkrFrameGraphNode {
  String8 name;
  VkrPipelineDomain domain;
  VkrFrameGraphNodeType type;  // GRAPHICS, COMPUTE, TRANSFER
  uint32_t dependency_count;
  uint32_t *dependencies;  // Node indices
  VkrFrameGraphCallback execute;
} VkrFrameGraphNode;

// Example Forward+ frame graph
VkrFrameGraphNode nodes[] = {
  {.name = "DepthPrepass", .domain = VKR_PIPELINE_DOMAIN_DEPTH_PREPASS},
  {.name = "LightCull", .domain = VKR_PIPELINE_DOMAIN_LIGHT_CULL,
   .dependencies = {0}},  // Depends on depth
  {.name = "ForwardPlus", .domain = VKR_PIPELINE_DOMAIN_FORWARD_PLUS,
   .dependencies = {1}},  // Depends on light cull
  {.name = "UI", .domain = VKR_PIPELINE_DOMAIN_UI,
   .dependencies = {2}},
};
```

---

## 7. Implementation Phases

### Phase 1: Foundation (No Breaking Changes)

**Goals**:
- Fix critical issues without API changes
- Improve code quality and documentation

**Tasks**:
1. Add atomic reference counting (internal change)
2. Replace global state with context-aware callbacks
3. Document subsystem initialization order
4. Add validation in debug builds
5. Create centralized configuration header

**Duration**: 2-3 weeks

---

### Phase 2: Handle System Unification

**Goals**:
- Unified handle system with validation
- Deferred destruction queue

**Tasks**:
1. Define `VkrHandle` structure
2. Create handle validation macros
3. Implement deferred destroy queue
4. Migrate buffer/texture handles (internal, API-compatible wrappers)
5. Add resource usage tracking

**Duration**: 3-4 weeks

---

### Phase 3: Context Separation

**Goals**:
- Separate monolithic frontend into contexts
- Enable per-context locking

**Tasks**:
1. Create `VkrResourceContext` structure
2. Create `VkrRenderContext` structure
3. Create `VkrFrameContext` structure
4. Migrate subsystems to appropriate contexts
5. Update public API (optional: maintain compatibility layer)

**Duration**: 4-6 weeks

---

### Phase 4: Error Handling Standardization

**Goals**:
- Consistent error handling across all APIs
- Better error messages and debugging

**Tasks**:
1. Define `VkrResult` structure
2. Add error context (file, line, message)
3. Migrate high-traffic APIs first
4. Add error callback system

**Duration**: 2-3 weeks

---

### Phase 5: Multithreading Support

**Goals**:
- Safe concurrent resource access
- Parallel command buffer recording

**Tasks**:
1. Add RW locks to resource contexts
2. Implement frame synchronization
3. Create secondary command buffer pool
4. Implement parallel batch recording
5. Add render thread separation

**Duration**: 6-8 weeks

---

### Phase 6: Forward+ Integration

**Goals**:
- Compute pipeline support
- Light culling system
- Forward+ rendering path

**Tasks**:
1. Implement compute pipeline creation
2. Add storage buffer support
3. Create light culling system
4. Implement Forward+ shaders
5. Add frame graph for pass ordering

**Duration**: 8-10 weeks

---

## 8. API Changes Summary

### 8.1 New Types

```c
// Unified handle
typedef struct VkrHandle { uint32_t index, generation, type, flags; } VkrHandle;

// Result type
typedef struct VkrResult { VkrRendererError error; union { ... }; } VkrResult;

// Contexts
typedef struct VkrResourceContext VkrResourceContext;
typedef struct VkrRenderContext VkrRenderContext;
typedef struct VkrFrameContext VkrFrameContext;

// Forward+ types
typedef struct VkrLight VkrLight;
typedef struct VkrLightGrid VkrLightGrid;
typedef struct VkrLightCullingSystem VkrLightCullingSystem;
```

### 8.2 New Functions

```c
// Handle validation
bool vkr_handle_is_valid(VkrHandle h);
bool vkr_validate_handle(VkrResourceContext *ctx, VkrHandle h);

// Context access
VkrResourceContext *vkr_renderer_get_resources(VkrRendererFrontendHandle r);
VkrRenderContext *vkr_renderer_get_render(VkrRendererFrontendHandle r);
VkrFrameContext *vkr_renderer_get_frame(VkrRendererFrontendHandle r);

// Deferred destruction
void vkr_queue_destroy(VkrResourceContext *ctx, VkrHandle h);

// Compute pipelines
VkrResult vkr_compute_pipeline_create(VkrResourceContext *ctx, ...);
void vkr_dispatch(VkrRenderContext *ctx, uint32_t x, uint32_t y, uint32_t z);

// Light culling
VkrResult vkr_light_culling_system_init(VkrLightCullingSystem *sys, ...);
void vkr_light_culling_execute(VkrLightCullingSystem *sys, ...);
```

### 8.3 Deprecated Functions (Phase 3+)

```c
// Old: Direct subsystem access through renderer
VkrTextureHandle vkr_texture_system_load(...);

// New: Through resource context
VkrResult vkr_texture_load(VkrResourceContext *ctx, ...);
```

---

## Appendix A: File Locations Reference

| File | Description |
|------|-------------|
| `lib/src/renderer/renderer_frontend.h` | Frontend structure definition |
| `lib/src/renderer/renderer_frontend.c` | Frontend implementation |
| `lib/src/renderer/vkr_renderer.h` | Public types and backend interface |
| `lib/src/renderer/vulkan/vulkan_types.h` | Vulkan backend types |
| `lib/src/renderer/vulkan/vulkan_backend.c` | Vulkan backend implementation |
| `lib/src/renderer/systems/vkr_pipeline_registry.h` | Pipeline management |
| `lib/src/renderer/vkr_render_graph.h` | Removed (render graph owns orchestration) |
| `lib/src/renderer/systems/vkr_material_system.h` | Material management |
| `lib/src/renderer/systems/vkr_texture_system.h` | Texture management |
| `lib/src/memory/vkr_allocator.h` | Allocator abstraction |
| `lib/src/core/vkr_atomic.h` | Atomic operations |
| `lib/src/core/vkr_threads.h` | Threading primitives |

---

## Appendix B: LLM Usage Instructions

When using this document for code generation or analysis:

1. **Reference specific sections** by number (e.g., "See Section 4.2 for handle refactoring")

2. **Issue priorities**:
   - CRITICAL: Must fix before multithreading
   - HIGH: Should fix for maintainability
   - MEDIUM: Nice to have improvements
   - LOW: Future consideration

3. **Phase dependencies**:
   - Phase 1 has no dependencies
   - Phase 2 depends on Phase 1
   - Phase 3 depends on Phase 2
   - Phase 4 can run parallel to Phase 3
   - Phase 5 depends on Phases 3-4
   - Phase 6 depends on Phase 5

4. **Code generation hints**:
   - Follow existing naming conventions (`vkr_` prefix)
   - Use `VkrAllocator` for memory management
   - Handle generation counters start at 1 (0 = invalid)
   - All public functions should validate inputs with `assert_log`

5. **Testing recommendations**:
   - Unit test handle validation logic
   - Integration test resource lifecycle
   - Stress test concurrent access patterns
   - Benchmark parallel command recording
