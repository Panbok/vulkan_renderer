---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Multithreading and GPU-Driven Rendering Design Specification

## Overview

This document specifies two advanced performance paths: multi-threaded command buffer recording using secondary command buffers, and GPU-driven rendering via indirect draw buffers built by a compute culling step.

Related: [Render Flow](./render_flow_and_state_updates_design.md), [Render Batching & State Caching](./render_batching_and_state_caching_design.md), [Scene Management](./scene_management_and_culling_design.md).

## Architecture

```
Multithreaded Recording
  Worker N: record secondary command buffer for batch i
  Main thread: begin primary → execute secondary buffers → end

GPU-Driven
  CPU builds instance data buffer
  Compute shader: cull → write vkCmdDrawIndexedIndirect-compatible buffer
  Graphics: vkCmdDrawIndexedIndirect with count from GPU
```

## API

```c
// Multithreaded recording (shape)
typedef struct RenderWorkerCtx { /* thread-local allocators, cmd pool */ } RenderWorkerCtx;
bool8_t renderer_workers_initialize(RendererFrontend *r, uint32_t worker_count, RenderWorkerCtx *out);
void    renderer_workers_shutdown(RendererFrontend *r, RenderWorkerCtx *workers);

bool8_t renderer_worker_begin(RenderWorkerCtx *wctx);
bool8_t renderer_worker_record_batch(RenderWorkerCtx *wctx, const RenderBatch *batch);
bool8_t renderer_worker_end(RenderWorkerCtx *wctx);
bool8_t renderer_execute_workers(RendererFrontend *r, RenderWorkerCtx *workers, uint32_t count);

// GPU-driven culling (shape)
typedef struct IndirectArgs { /* matches VkDrawIndexedIndirectCommand */ } IndirectArgs;
bool8_t renderer_build_indirect_buffers(RendererFrontend *r, const Scene *scene);
bool8_t renderer_cull_and_compact(RendererFrontend *r, const Camera *camera);
bool8_t renderer_draw_indirect(RendererFrontend *r, const PipelineHandle pipeline);
```

## Design Decisions

1) Secondary command buffers per worker
- Rationale: Vulkan allows parallel recording; primary submits in order.
- Implementation: One command pool per thread; reuse across frames.

2) Batch granularity
- Rationale: Avoid contention; balance between too many small and too few large secondary buffers.
- Implementation: Batch by material or geometry layout; target ~0.1–1.0ms per batch.

3) Compute-based frustum culling
- Rationale: Offload per-instance tests; scales with large instance counts.
- Implementation: Compute writes compacted indirect args and count; graphics consumes it.

4) Synchronization
- Rationale: Ensure correctness between compute and graphics.
- Implementation: Pipeline barriers between cull (compute) and draw (graphics).

5) Fallback path
- Rationale: Maintain portability and simplicity for small scenes.
- Implementation: Single-threaded recording and direct draw retained.

## Usage Examples

```c
// Multithreaded recording
renderer_workers_initialize(renderer, num_threads, workers);
for (t in workers) { renderer_worker_begin(&workers[t]); }
for (b in batches) { renderer_worker_record_batch(&workers[assign(b)], &b); }
for (t in workers) { renderer_worker_end(&workers[t]); }
renderer_execute_workers(renderer, workers, num_threads);

// GPU-driven
renderer_build_indirect_buffers(renderer, scene);
renderer_cull_and_compact(renderer, camera);
renderer_draw_indirect(renderer, pipeline);
```

## Performance Considerations

- Avoid CPU-side synchronization points; thread-local arenas and cmd pools.
- Batch to minimize command buffer overhead and state changes.
- Use persistent mapped buffers for indirect args where applicable.

## Testing

- `test_secondary_cmd_record_execute` – verify successful multi-thread record/execute.
- `test_compute_cull_pipeline_barriers` – correct memory visibility between stages.
- `test_indirect_draw_matches_direct` – draw output parity for small scenes.

## Revision History

- Version 1.0 (2025-10-11): Initial specification for multithreading and GPU-driven paths.


