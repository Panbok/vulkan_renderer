---
status: superseded
updated: 2026-07-31
authority: spec
---

> **Archived.** Superseded by [`../architecture/renderer-architecture-spec.md`](../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Spec: Multithreaded Vulkan Backend for Asset Uploads and Command Execution

## Summary
This spec defines a Vulkan-backend-first multithreading design that keeps existing synchronous public renderer APIs while introducing backend batch/parallel internals for:
1. Parallel texture uploads.
2. Parallel geometry buffer uploads.

The design removes upload-path `vkQueueWaitIdle` stalls, avoids shared command-pool/threading hazards, and preserves current caller semantics.

## Implementation Status Note (2026-02-09)
- Active scope: parallel upload runtime and batch upload APIs (textures +
  geometry).
- Retired scope: parallel render-record/capture path for graphics pass command
  recording. It was removed from the active code path due correctness and
  performance tradeoffs.

## Current Bottlenecks to Address
1. Single-use command helper hard-stalls queues with `vkQueueWaitIdle` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_command.c:139`.
2. Buffer copy path waits fence and then still calls `vkQueueWaitIdle` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_buffer.c:463`.
3. Image upload helpers still serialize through `vkQueueWaitIdle` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:774`, `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:886`, `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:1096`, `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:1161`, `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:1397`, `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_image.c:1463`.
4. Batch texture decode is parallel, but GPU uploads are explicitly main-thread serialized in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/systems/vkr_texture_system.c:2860`.
5. Command recording is single-primary-buffer-only with an explicit TODO in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_backend.c:5182`.

## Architecture Specification

### 1. Backend Parallel Infrastructure
1. Add a backend-owned `VulkanParallelRuntime` to `VulkanBackendState` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_types.h`.
2. `VulkanParallelRuntime` contains per-job-worker Vulkan contexts keyed by `VkrJobContext.worker_index`; each context has one transfer command pool, one graphics-upload command pool, and one secondary-record command pool per frame-in-flight.
3. Add queue submission locks in backend state: one lock per queue role used concurrently by worker jobs and frame thread. Submission wrapper functions become mandatory for every `vkQueueSubmit` and `vkQueuePresentKHR`.
4. Keep `graphics_command_pool` frame-thread-owned for primary frame command buffers; workers never allocate from it.
5. Remove all upload-path `vkQueueWaitIdle`; synchronization uses per-task fences and semaphores only.
6. If no job system is attached, parallel runtime is disabled and backend falls back to existing sequential behavior.

### 2. Job-System Attachment
1. Add backend interface function `set_job_system(void *backend_state, VkrJobSystem *job_system)` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vkr_renderer.h`.
2. Call it from renderer systems init in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/renderer_frontend.c` before texture/material/mesh loading begins.
3. Runtime chooses `parallel_worker_count = min(job_system->worker_count, 8)`.

### 3. Upload Multithreading (Textures + Geometry)
1. Add backend batch APIs in `VkrRendererBackendInterface` and frontend wrappers:
   1. `texture_create_with_payload_batch(...)`.
   2. `buffer_create_batch(...)` with upload metadata for initial data copies.
2. Single-resource APIs remain synchronous and call batch API with `count=1` internally.
3. Texture batch integration:
   1. Keep current decode jobs in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/systems/vkr_texture_system.c`.
   2. Replace per-texture GPU upload loop with one backend batch call.
   3. Preserve dedup logic and handle registration logic unchanged.
4. Geometry batch integration:
   1. Add `vkr_geometry_system_create_batch(...)`.
   2. Mesh manager collects geometry configs for all subsets and submits one buffer batch.
   3. Geometry system reserves slots first, commits on full success, rolls back on failures.
5. Upload worker task contract:
   1. Task owns staging resources and upload command recording.
   2. Task submits transfer phase and optional graphics phase with semaphore chaining.
   3. Task signals completion fence; no queue-idle waits.
6. Queue ownership transfer rules for dedicated transfer families stay explicit with matched release/acquire barriers; no worker may skip release-acquire pair.

### 4. Parallel Command Recording/Execution for All Graphics Passes (Retired)
Status: retired from active scope.

This section is retained as historical design context and is not part of the
current default implementation track.

1. Replace immediate inline draw recording in backend with pass-scoped command capture mode.
2. Begin graphics render passes with `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS` when parallel recording is enabled.
3. During pass execution, backend API calls update a per-pass capture state and append self-contained `VulkanDrawOp` records instead of issuing `vkCmd*` directly.
4. `VulkanDrawOp` contains fully resolved state required to replay independently:
   1. Pipeline handle and layout.
   2. Descriptor set handles and dynamic offsets.
   3. Push constant bytes and ranges.
   4. Vertex/index buffer handles and offsets.
   5. Dynamic state snapshot (viewport/scissor/depth bias).
   6. Draw parameters.
5. End of pass:
   1. Partition draw ops into chunks.
   2. Submit one GPU job per chunk to record a secondary command buffer from chunk-local op data.
   3. Wait all chunk jobs.
   4. Execute secondaries in deterministic order on primary via `vkCmdExecuteCommands`.
6. Deterministic ordering policy:
   1. Opaque passes use state-sorted chunking.
   2. Transparent/UI/editor/picking preserve original submission order.
7. Descriptor/state preparation remains on main thread during capture; worker jobs only encode Vulkan commands from prepared immutable draw-op data.
8. Add per-pass thresholds to avoid overhead; under threshold uses single-thread secondary encode path.

### 5. Synchronization and Lifetime Rules
1. No worker path may read or write `state->temp_scope`; worker allocations use job `ctx->allocator` or explicit temporary host allocations freed inside job.
2. Resource wrapper pools remain main-thread-owned by batch-call orchestration; workers never allocate wrapper structs.
3. Deferred destruction remains submit-serial-based; destroy calls from batch failure paths enqueue destruction instead of forcing `wait_idle`.
4. Frame-thread queue submissions and worker submissions share lock discipline with strict lock order: transfer lock, then graphics-upload lock, then present lock.

### 6. Cleanup/Compression Requirements in Implementation
1. Remove redundant single-use helpers once batch paths replace them, or keep one canonical helper with explicit submit mode.
2. Consolidate upload command recording duplicated across buffer/image paths into local static helpers in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vulkan/vulkan_backend.c`.
3. Eliminate duplicate error-teardown branches with single cleanup labels in each multi-step creation path.

## Public API / Interface Changes
1. Add to `VkrRendererBackendInterface` in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vkr_renderer.h`:
   1. `set_job_system`.
   2. `texture_create_with_payload_batch`.
   3. `buffer_create_batch`.
2. Add frontend wrappers in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/renderer_frontend.c` for the above batch operations.
3. Add new upload and batch descriptor types in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/vkr_renderer.h`:
   1. `VkrBufferUploadPayload`.
   2. `VkrTextureBatchCreateRequest` or equivalent flat arrays contract.
4. Add geometry-system batch entrypoint in `/Users/Yaroslav_Panok/Dev/c_projects/vulkan_renderer/lib/src/renderer/systems/vkr_geometry_system.h`.

## Test Cases and Scenarios
1. Unit: batch texture upload success/failure fan-out; verifies per-item error mapping and cleanup symmetry.
2. Unit: batch buffer upload creates valid vertex/index buffers and no leaked staging resources.
3. Unit: no upload path invokes `vkQueueWaitIdle` (hook/assert wrapper in debug builds).
4. Integration: `vkr_texture_system_load_batch` with 100+ unique textures and duplicates; verify dedup, handles, and timing improvement.
5. Integration: mesh batch load with many submeshes; verify geometry creation correctness and material bindings.
6. Stress: concurrent scene reload loop (load/unload cycles) verifies no growth in created-vs-destroyed GPU resource counters.
7. Validation-layer run: no queue-family transfer errors and no thread-safety queue-submit errors.
8. Performance baseline:
    1. Cached Sponza+Falcon load target <= 0.8s on same machine profile used by current docs.
    2. Upload-parallel path does not regress steady-state frame CPU time versus serial.

## Rollout Plan
1. Phase A: introduce backend parallel runtime, queue locks, and upload wait-idle removal behind `VKR_VULKAN_PARALLEL_UPLOAD`.
2. Phase B: wire texture and geometry batch APIs to use parallel runtime.
3. Phase C (retired): pass command capture + secondary recording for all graphics passes.
4. Phase D: enable upload parallel by default after validation/perf gates pass; keep runtime env override for rollback.

## Assumptions and Defaults
1. Public renderer call semantics remain synchronous; returned handles are ready for immediate use.
2. Existing job system is available and is the execution substrate for backend GPU jobs; without it, behavior is sequential fallback.
3. Vulkan queue ownership transfers remain explicit and standards-compliant for mixed queue-family uploads.
4. Implementation must preserve current resource lifetime/accounting rules and deferred-destroy semantics.
