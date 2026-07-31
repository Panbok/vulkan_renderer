---
status: proposed
updated: 2026-07-31
authority: design
---
# Resource Loading Analysis

## Overview
This document analyzes the performance of the resource loading system, specifically focusing on the bottlenecks observed during the loading of the Sponza and Falcon Car models.

**Current Performance (after texture caching):**
- Uncached (first run): ~6.2 seconds (writes cache files)
- Cached (subsequent runs): **~1.3 seconds** ✓

**Previous Performance (before texture caching):**
- Uncached: ~5-6 seconds
- Cached (mesh only): ~4.6 seconds

The texture cache implementation reduced cached load times by **~3.5x** (4.6s → 1.3s).

## Completed Optimizations

### ✓ Texture Cache (Phase 3 - COMPLETED)
**Location:** `lib/src/renderer/systems/vkr_texture_system.c`

Implemented binary cache for decoded texture data:
- **Format:** `.vkt` files containing raw RGBA pixel data + metadata header
- **Cache validation:** Uses source file modification time to auto-invalidate stale caches
- **71 cache files** created for Sponza + Falcon models

**Header format:**
```c
typedef struct VkrTextureCacheHeader {
  uint32_t magic;           // 'VKTH'
  uint32_t version;         // Cache format version
  uint64_t source_mtime;    // Source file modification time
  uint32_t width;
  uint32_t height;
  uint32_t channels;        // Always 4 (RGBA)
  uint32_t format;          // VkrTextureFormat enum
  uint8_t has_transparency;
  uint8_t padding[3];
  // Followed by: width * height * channels bytes of raw pixel data
} VkrTextureCacheHeader;
```

## Remaining Bottlenecks

### 1. Sequential GPU Texture Upload
**Location:** `lib/src/renderer/vulkan/vulkan_backend.c` - `renderer_vulkan_create_texture`

After decoding (now fast due to cache), textures are uploaded to GPU **one at a time** on the main thread. Each upload involves:
1. Staging buffer allocation
2. Data copy to staging buffer
3. Command buffer allocation from shared pool
4. Image creation and layout transitions
5. Queue submission and synchronization

**Current time breakdown (estimated for 67 textures):**
- Cache read: ~200-300ms
- GPU uploads: ~800-1000ms (sequential)

### 2. Vulkan Backend Not Thread-Safe
**Location:** `lib/src/renderer/vulkan/vulkan_backend.c`

The Vulkan backend cannot support parallel GPU operations because:

| Resource | Current State | Problem |
|----------|--------------|---------|
| Command Pools | 1 shared `graphics_command_pool` | Vulkan spec: pools are NOT thread-safe |
| Queue Submission | Single `graphics_queue` | All uploads serialize here |
| Staging Buffers | Allocated from `temp_scope` | Shared allocator state |
| Fences | Shared `in_flight_fences` | Cannot track per-upload completion |

Simply adding mutexes would serialize all parallel work, defeating the purpose.

## Future Optimization Plan

### Phase 1: Parallel Texture Upload via Transfer Queue
**Status:** NOT STARTED
**Complexity:** Medium-High (~400-500 lines)
**Expected Impact:** ~0.5-0.7s reduction (1.3s → 0.6-0.8s)

The device already has a **dedicated transfer queue** available:
```
Transfer queue: 0x612000059bd8 (family 2, dedicated: yes)
```

#### Required Changes

**1. Per-Thread Command Pool Infrastructure**
```c
// New structure for thread-local Vulkan resources
typedef struct VulkanTransferContext {
  VkCommandPool command_pool;      // One per worker thread
  VkCommandBuffer command_buffer;  // Reusable command buffer
  VkFence fence;                   // Per-upload completion tracking
  VkSemaphore semaphore;           // Signal when transfer complete
} VulkanTransferContext;
```

**Files to modify:**
- `lib/src/renderer/vulkan/vulkan_device.c` - Create per-thread pools
- `lib/src/renderer/vulkan/vulkan_backend.h` - Add transfer context array

**2. Transfer Queue Upload Path**
New function to upload via transfer queue instead of graphics queue:

```c
// New function signature
VkrBackendResourceHandle renderer_vulkan_create_texture_async(
    void *backend_state,
    const VkrTextureDescription *desc,
    const void *initial_data,
    VulkanTransferContext *transfer_ctx,  // Thread-local context
    VkSemaphore *out_semaphore);          // Caller waits on this
```

**Flow:**
1. Worker thread calls with its own `VulkanTransferContext`
2. Upload uses transfer queue (parallel with other uploads)
3. Returns semaphore that graphics queue waits on before first use

**Files to modify:**
- `lib/src/renderer/vulkan/vulkan_backend.c` - Add async upload function
- `lib/src/renderer/vulkan/vulkan_image.c` - Transfer queue upload helpers

**3. Semaphore Synchronization**
Graphics queue must wait for transfer completion before using texture:

```c
// At first draw using the texture
VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
VkSubmitInfo submit = {
    .waitSemaphoreCount = 1,
    .pWaitSemaphores = &texture_upload_semaphore,
    .pWaitDstStageMask = &wait_stage,
    // ... rest of submit
};
```

**Files to modify:**
- `lib/src/renderer/vulkan/vulkan_backend.c` - Track pending semaphores
- Texture system - Pass semaphores up to renderer

**4. Integration with Job System**
Modify texture decode jobs to also perform GPU upload:

```c
// In vkr_texture_decode_job_run()
// After decoding from cache/stbi:
if (job->transfer_ctx) {
    // Upload on worker thread via transfer queue
    result->gpu_handle = renderer_vulkan_create_texture_async(
        job->backend, &desc, pixels, job->transfer_ctx, &result->semaphore);
}
```

**Files to modify:**
- `lib/src/renderer/systems/vkr_texture_system.c` - Pass transfer context to jobs
- `lib/src/core/vkr_job_system.c` - Initialize per-worker transfer contexts

### Phase 2: Batched Staging Buffer Uploads
**Status:** NOT STARTED
**Complexity:** Medium (~200 lines)
**Expected Impact:** Additional ~0.1-0.2s reduction

Instead of N staging buffers for N textures, use one large staging buffer:

1. Calculate total size needed for all textures
2. Allocate single staging buffer
3. Copy all pixel data at different offsets
4. Record all copy commands in one command buffer
5. Single queue submission

This reduces:
- Buffer allocation overhead (N → 1)
- Command buffer allocation overhead (N → 1)
- Queue submission overhead (N → 1)

### Phase 3: Pipeline Overlap
**Status:** NOT STARTED
**Complexity:** High (~400-450 lines)
**Expected Impact:** ~0.2-0.4s reduction (variable, depends on asset structure)

Trigger material/texture loading immediately when a mesh finishes parsing, rather than waiting for all meshes.

#### Current Architecture Problem

The mesh batch loader in `lib/src/renderer/resources/loaders/mesh_loader.c` uses a **strictly sequential two-phase approach**:

```
┌─────────────────────────────────────────────────────────────┐
│ Phase 1: Parse ALL meshes in parallel                       │
│ ┌─────────┐ ┌─────────┐ ┌─────────┐     ┌─────────┐        │
│ │ Mesh 1  │ │ Mesh 2  │ │ Mesh 3  │ ... │ Mesh N  │        │
│ └────┬────┘ └────┬────┘ └────┬────┘     └────┬────┘        │
│      └──────────┴──────────┴──────────────┘               │
│                        │                                   │
│                  WAIT FOR ALL ← blocking point             │
│                        ▼                                   │
├─────────────────────────────────────────────────────────────┤
│ Phase 2: Load ALL materials (main thread)                   │
│   - Collect all material paths from all meshes              │
│   - Batch load ALL materials → triggers texture loading     │
│   - GPU upload ALL textures sequentially                    │
└─────────────────────────────────────────────────────────────┘
```

The blocking point is in `vkr_mesh_loader_load_batch()` at lines 1418-1422:
```c
// Wait for all mesh load jobs to complete
for (uint32_t i = 0; i < count; i++) {
    if (job_submitted[i]) {
        vkr_job_wait(job_sys, job_handles[i]);
    }
}
```

#### Target Architecture

With pipeline overlap, mesh parsing and material loading would interleave:

```
┌────────────────────────────────────────────────────────────────────┐
│ Overlapped Loading                                                 │
│                                                                    │
│ Mesh 1 parse ──► Material load ──► Texture decode ──► GPU upload   │
│ Mesh 2 parse ──────► Material load ──► Texture decode ──► GPU ↓    │
│ Mesh 3 parse ────────────► Material load ──► Decode ──────► GPU ↓  │
│                                                                    │
│ Time ──────────────────────────────────────────────────────────►   │
│                                                                    │
│ Current:  [===ALL PARSE===][====WAIT====][===ALL MATERIALS===]     │
│ Overlap:  [===PARSE+MATERIALS INTERLEAVED===]                      │
└────────────────────────────────────────────────────────────────────┘
```

#### Core Challenges

**1. Main Thread GPU Constraint (Critical)**

The Vulkan backend is NOT thread-safe. GPU texture uploads via `renderer_vulkan_create_texture` MUST happen on the main thread. This severely limits overlap potential since the material loading pipeline eventually requires:
- Texture decoding (parallelizable ✓)
- GPU upload (main thread only ✗)

**2. Job Callback Threading Model**

The job system's `on_success` callback runs on a **worker thread**, not the main thread (see `vkr_job_system.c` line 230-232):
```c
// Run callback outside the lock to avoid blocking other workers.
if (callback) {
    callback(ctx, payload);
}
```

This means we cannot directly trigger GPU operations from mesh job completion callbacks.

**3. Material Deduplication During Streaming**

Currently, deduplication happens AFTER all meshes are parsed (lines 1499-1514). With streaming:
- Material A requested by Mesh 1 → start loading
- Mesh 3 completes, also needs Material A → must check if already loading/loaded
- Race conditions between "is it loaded?" check and load completion

#### Required New Structures

```c
// Thread-safe material request (submitted by completed mesh jobs)
typedef struct VkrMaterialRequest {
    String8 path;
    uint32_t mesh_index;
    uint32_t subset_index;
} VkrMaterialRequest;

// Thread-safe queue for material requests
typedef struct VkrMaterialRequestQueue {
    VkrMaterialRequest *requests;
    uint32_t capacity;
    uint32_t head;  // Read position (main thread)
    uint32_t tail;  // Write position (worker threads, atomic)
    VkrMutex *mutex;
} VkrMaterialRequestQueue;

// In-flight material tracking (prevents duplicate loads)
typedef enum VkrMaterialLoadStatus {
    MAT_STATUS_PENDING,
    MAT_STATUS_LOADING,
    MAT_STATUS_COMPLETE,
    MAT_STATUS_FAILED,
} VkrMaterialLoadStatus;

typedef struct VkrMaterialLoadState {
    VkrMaterialHandle handle;
    VkrRendererError error;
    VkrMaterialLoadStatus status;
} VkrMaterialLoadState;

// Hash table: material_path → VkrMaterialLoadState
```

#### Implementation Breakdown

| Component | Lines | Files | Description |
|-----------|-------|-------|-------------|
| Material Request Queue | ~80 | `mesh_loader.c` | Thread-safe SPSC queue for mesh→material requests |
| In-Flight Material Tracker | ~60 | `mesh_loader.c` | Hash table tracking `{path → status}` |
| Main Thread Material Pump | ~100 | `mesh_loader.c` | Poll for ready materials, trigger batch GPU uploads |
| Job Completion Callbacks | ~40 | `mesh_loader.c` | Use existing `on_success` to enqueue material requests |
| Result Association | ~60 | `mesh_loader.c` | Track which materials belong to which mesh subsets |
| Synchronization Logic | ~80 | `mesh_loader.c` | Handle partial completion, all-done signaling |

**Total: ~420 lines** (primarily in `mesh_loader.c`)

#### Modified Mesh Job Completion

```c
// New callback: enqueue materials when mesh parsing completes
vkr_internal void vkr_mesh_job_on_success(VkrJobContext *ctx, void *payload) {
    VkrMeshLoadJobPayload *job = (VkrMeshLoadJobPayload *)payload;

    // Queue material paths for main thread processing
    for (uint64_t i = 0; i < job->result->subsets.length; i++) {
        VkrMeshLoaderSubset *subset = &job->result->subsets.data[i];
        if (!subset->material_name.str || subset->material_name.length == 0) {
            continue;
        }

        VkrMaterialRequest req = {
            .path = subset->material_name,
            .mesh_index = job->mesh_index,
            .subset_index = (uint32_t)i,
        };
        // Thread-safe enqueue
        vkr_material_request_queue_push(job->request_queue, &req);
    }

    // Signal main thread that a mesh completed
    vkr_atomic_increment(&job->batch_state->meshes_completed);
}
```

#### Main Thread Polling Loop

Replace the simple "wait for all meshes → load all materials" with active polling:

```c
// New batch loading loop with pipeline overlap
vkr_internal uint32_t vkr_mesh_loader_load_batch_overlapped(
    VkrMeshLoaderContext *context,
    const String8 *mesh_paths,
    uint32_t count,
    VkrAllocator *temp_alloc,
    VkrMeshBatchResult *out_results)
{
    // ... initialization ...

    // Submit all mesh jobs with completion callback
    for (uint32_t i = 0; i < count; i++) {
        VkrJobDesc job_desc = {
            .run = vkr_mesh_load_job_run,
            .on_success = vkr_mesh_job_on_success,  // NEW: completion callback
            .payload = &payloads[i],
            // ...
        };
        vkr_job_submit(job_sys, &job_desc, &job_handles[i]);
    }

    // Active polling loop - process materials as meshes complete
    VkrHashTable_VkrMaterialLoadState in_flight_materials = {0};
    String8 pending_batch[MATERIAL_BATCH_SIZE];
    uint32_t pending_count = 0;

    while (meshes_remaining > 0 || materials_pending > 0) {
        // 1. Drain material request queue (from completed mesh jobs)
        VkrMaterialRequest req;
        while (vkr_material_request_queue_pop(&request_queue, &req)) {
            // Check if already loading/loaded
            VkrMaterialLoadState *state = vkr_hash_table_get(
                &in_flight_materials, req.path);

            if (state) {
                // Already tracked - just record this subset needs it
                vkr_record_material_dependency(req.mesh_index, req.subset_index,
                                               req.path);
            } else {
                // New material - add to pending batch
                pending_batch[pending_count++] = req.path;
                vkr_hash_table_insert(&in_flight_materials, req.path,
                    (VkrMaterialLoadState){.status = MAT_STATUS_PENDING});
                vkr_record_material_dependency(req.mesh_index, req.subset_index,
                                               req.path);
            }
        }

        // 2. Trigger batch load when threshold reached or all meshes done
        if (pending_count >= MATERIAL_BATCH_SIZE ||
            (meshes_remaining == 0 && pending_count > 0)) {

            // Batch load materials (includes texture decoding + GPU upload)
            VkrResourceHandleInfo handles[MATERIAL_BATCH_SIZE];
            VkrRendererError errors[MATERIAL_BATCH_SIZE];

            vkr_resource_system_load_batch(VKR_RESOURCE_TYPE_MATERIAL,
                pending_batch, pending_count, temp_alloc, handles, errors);

            // Update in-flight state and assign handles to subsets
            for (uint32_t i = 0; i < pending_count; i++) {
                VkrMaterialLoadState *state = vkr_hash_table_get(
                    &in_flight_materials, pending_batch[i]);
                state->status = MAT_STATUS_COMPLETE;
                state->handle = handles[i].as.material;
                state->error = errors[i];

                // Assign to all subsets that need this material
                vkr_assign_material_to_dependents(pending_batch[i],
                                                  state->handle, results);
            }
            pending_count = 0;
        }

        // 3. Update mesh completion count
        meshes_remaining = count - vkr_atomic_load(&batch_state.meshes_completed);

        // 4. Brief yield to avoid busy-waiting
        if (meshes_remaining > 0 && pending_count == 0) {
            vkr_thread_yield();
        }
    }

    // ... finalization ...
}
```

#### Files to Modify

| File | Changes |
|------|---------|
| `lib/src/renderer/resources/loaders/mesh_loader.c` | Main implementation (~350 lines) |
| `lib/src/renderer/resources/loaders/mesh_loader.h` | New structures and function declarations (~50 lines) |
| `lib/src/core/vkr_job_system.h` | Possibly add atomic helpers if not present (~20 lines) |

#### Complexity Assessment

**Why this is HIGH complexity:**

1. **Thread Synchronization**: Material request queue needs lock-free or mutex-protected operations between worker threads and main thread.

2. **State Machine**: Each material goes through `PENDING → LOADING → COMPLETE/FAILED` states with concurrent access.

3. **Deduplication Logic**: Must handle same material requested by multiple meshes at unpredictable times.

4. **Result Association**: Need to track `(mesh_index, subset_index) → material_path` mappings and resolve them when materials complete.

5. **Partial Completion**: Must handle scenarios where some materials fail while others succeed.

6. **Testing**: Race conditions are notoriously difficult to reproduce and test.

#### Recommendation

**Priority: LOW** - Implement Phase 1 (Transfer Queue) first.

Rationale:
- Current cached mesh parsing is fast (~200-300ms)
- GPU uploads are the true bottleneck (~800-1000ms)
- Phase 1 directly addresses GPU parallelism with better ROI
- After Phase 1, mesh parsing time becomes relatively more significant, making Pipeline Overlap more valuable

## Summary

| Optimization | Status | Impact | Complexity | Lines |
|-------------|--------|--------|------------|-------|
| Texture Cache | ✓ DONE | 4.6s → 1.3s | Low | ~200 |
| Parallel Upload (Transfer Queue) | TODO | 1.3s → ~0.7s | Medium-High | ~400-500 |
| Batched Staging | TODO | ~0.1-0.2s | Medium | ~200 |
| Pipeline Overlap | TODO | ~0.2-0.4s | High | ~400-450 |

**Recommended next step:** Phase 1 (Parallel Upload) provides the best remaining improvement with reasonable complexity. Pipeline Overlap should be implemented AFTER Phase 1, as its benefits increase once GPU upload parallelism is achieved.
