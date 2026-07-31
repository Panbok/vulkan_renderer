---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Job System Design Plan

## Goals
- Run mesh/texture/shader/material loaders off the main thread now; later extend to Vulkan backend work.
- Support priorities (`LOW`, `NORMAL`, `HIGH`) to balance latency-sensitive tasks.
- Support job type masks (bitset) so workers can be limited to `GENERAL`, `RESOURCE_LOADING`, `GPU_RESOURCES` or combinations.
- Allow chaining/dependencies plus per-job `on_success`/`on_failure` callbacks.
- Build on existing primitives: `VkrThread`, `VkrMutex`, `VkrCondVar`, circular-buffer queue, and `VkrAllocator`/`Arena`.

## Current Foundations – Assessment
- Threads (`lib/src/core/vkr_threads.h`, platform impls):
  - Provide create/join/detach/cancel, mutex, condvar, sleep, and thread id. Good enough for a worker pool.
  - No thread naming/affinity/tls helpers; `active`/`joined` flags are not atomic but acceptable under pool-owned discipline.
  - Uses `VkrAllocator` only for the control block; caller manages any per-thread memory.
- Queue (`lib/src/containers/queue.h`):
  - Fixed-capacity circular buffer; not thread-safe by itself; no blocking semantics.
  - Works if wrapped in a mutex/condvar; capacity must be sized up-front.
- Allocators (`lib/src/memory/arena.c`, `vkr_allocator.c`):
  - Arena is not thread-safe; best used per worker to avoid locks.
  - `vkr_allocator_arena` lets us wrap an arena in `VkrAllocator` for thread/mutex allocations.

## Architecture
- Job handle
  - `{ id, generation }` for safety; pooled and recycled.
- Job descriptor
  - `priority`, `Bitset8 type_mask`, `bool (*run)(JobContext*, void *payload)`.
  - `void *payload` + `uint32 payload_size` (copied into a job arena to avoid lifetime issues).
  - `on_success`/`on_failure` callbacks (run on worker thread after `run` returns).
  - Optional `continuation` handle for chaining; or enqueue from the callback.
  - `remaining_dependencies` counter for dependency/unlock semantics.
- Worker
  - Persistent `VkrThread`, `Bitset8 type_mask` (what it is allowed to run), `name` string (for logging), and two arenas: one persistent (job metadata, small payload copies) and one scratch per job.
  - Uses `VkrAllocator` wrapping the worker arena for any thread-side allocations.
- Queues and scheduling
  - Three ready queues (one per priority). Each is a `Queue<JobHandle>` guarded by `VkrMutex` + `VkrCondVar`.
  - Submission pushes to the queue matching `priority` when `remaining_dependencies == 0`.
  - Workers wait on a shared condvar; pop the highest non-empty queue that has a job whose `type_mask` intersects the worker’s `type_mask`.
  - Aging/fairness: after N consecutive high-priority pops, allow one normal/low if present to prevent starvation.
  - Backpressure: if a queue is full, submission blocks or returns a failure code; capacity is set during init based on expected loader fan-out.
- Dependencies & chaining
  - `job_add_dependency(child, parent)` increments `child.remaining_dependencies`; each parent completion decrements; when it hits zero the child is enqueued at its declared priority.
  - `continuation` is just another job submitted in `on_success`; failure can skip or enqueue an alternate path.
- Memory
  - Job metadata allocated from a central job-system allocator (arena-backed) and recycled via a freelist.
  - Payloads copied into the job-system arena to decouple from caller lifetimes; large buffers should be referenced by handle instead of copied.
  - Worker-local scratch arenas cleared per job to avoid allocator contention inside loaders.
- API sketch
  - `bool job_system_init(JobSystem*, JobSystemConfig* cfg);`
  - `JobHandle job_submit(const JobDesc*);`
  - `bool job_add_dependency(JobHandle job, JobHandle dependency);`
  - `void job_wait(JobHandle);` and `void job_counter_wait(JobCounter*);` for fences.
  - `void job_system_shutdown(JobSystem*);`

## Type & Priority Mapping
- Job types (bitset via `Bitset8` from `containers/bitset.h`): `JOB_TYPE_GENERAL = 1<<0`, `JOB_TYPE_RESOURCE = 1<<1`, `JOB_TYPE_GPU = 1<<2`.
- Set bits with `bitset8_set(&mask, JOB_TYPE_RESOURCE);` etc. For multi-bit defaults, either call `bitset8_set` per bit or provide a small helper that ORs into `mask.set` after validating inputs.
- Worker assignment (recommended defaults):
  - General workers: `type_mask` sets `GENERAL` and `RESOURCE`.
  - Resource-dedicated worker (optional): `type_mask` sets `RESOURCE` to isolate I/O-heavy work.
  - GPU worker (optional/main thread): `type_mask` sets `GPU` for Vulkan-backend tasks that must stay serialized or run in a special thread.

## Execution Flow
1) `job_system_init`: create job arena, freelists, ready queues, mutex/condvar, and N workers (N = cores-1 or config). Each worker builds its arenas/allocator.
2) `job_submit`: reserve a job entry, copy payload, set deps. If deps==0 enqueue to priority queue and signal condvar.
3) Worker loop: wait on condvar; pick highest priority compatible job; run `run(ctx, payload)`. On return, mark success/failure, invoke callbacks, decrement child dependency counters and enqueue newly-ready jobs, recycle finished job entry.
4) Shutdown: set `running=false`, broadcast condvar, join workers, destroy queues/arenas.

## Integration Notes
- Loaders: wrap mesh/texture/material/shader loads as `JOB_TYPE_RESOURCE` with `NORMAL` priority; chain material after texture by adding dependencies or by continuation callback.
- Vulkan backend: start with `GPU` jobs executed by a dedicated worker (could be the render thread) to respect backend constraints.
- Event system precedent: mirrors the mutex+condvar pattern; reuse similar error handling/logging style.

## Implementation Steps
1) Define public API/types (`job_system.h`) with priorities, type masks, job descriptors/handles, and counter/fence helpers.
2) Build the job-system core (`job_system.c`): queues with mutex/condvar, worker threads, job pool/freelist, submission/dependency logic, callbacks.
3) Add per-worker arena creation/reset hooks; expose `JobContext` with access to worker scratch arena and thread id for loaders.
4) Integrate with resource loaders: add job submission helpers for texture/material/mesh/shader loads; chain material loading to texture completion.
5) Add GPU job path: optional dedicated worker configuration and guard to ensure only GPU-typed workers execute those jobs.
6) Add tests/smoke harness (or debug asserts) for priority ordering, dependency resolution, callback firing, and shutdown without leaks.
