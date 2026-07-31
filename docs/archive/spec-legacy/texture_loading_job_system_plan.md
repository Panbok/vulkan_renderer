---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Texture Loading Job System Plan

## Goals
- Parallelize texture loads by offloading file IO + image decode to the job system while keeping GPU uploads serialized/safe.
- Make the texture load API fully multithreaded by default (no single-threaded fast path); synchronous callers block only by waiting on async tickets.
- Avoid data races in `VkrTextureSystem`/resource system while multiple jobs decode textures.

## Current Flow (Single-Threaded)
- `texture_loader.c` → `vkr_texture_system_load` in `lib/src/renderer/systems/vkr_texture_system.c` calls `vkr_texture_system_load_from_file` (stbi), immediately creates the GPU texture, inserts into `texture_map`, and returns the handle. All on the caller thread.
- `material_loader.c`/`view_skybox` also call the synchronous path and then `acquire`, so the calling thread blocks on IO + decode + GPU upload.
- `vkr_resource_system` is a global singleton (not thread-safe) that simply dispatches to loaders; `vkr_texture_system` uses a shared arena and hash table with no mutexing.

## Constraints & Risks
- Job system (`lib/src/core/vkr_job_system.c`): workers default to `GENERAL|RESOURCE` type mask; no GPU worker is configured by default. Job callbacks run on the worker thread.
- `VkrTextureSystem` structures (arena, `texture_map`, `next_free_index`, `generation_counter`) are not thread-safe; concurrent mutation must be guarded or confined to a single thread.
- Renderer texture creation/destruction should stay on the render/main thread (assume Vulkan backend is not thread-safe).
- Use thread-local flip control: `stbi_set_flip_vertically_on_load_thread` avoids the global toggle when decoding on workers.
- Enforce `STBI_NO_STDIO` and feed `stbi_load_from_memory` using our filesystem API to remove any reliance on stdio and make IO predictable in worker jobs.

## Proposed Architecture (Two-Stage Jobs)
- Stage A (RESOURCE job): Read file via our filesystem into memory (arena/scratch), set flip with `stbi_set_flip_vertically_on_load_thread`, then decode via `stbi_load_from_memory`. Output: `TextureDecodeResult` (name key, width/height/channels/format, transparency flag, pixel pointer, size). No GPU interaction here.
- Stage B (GPU job, `JOB_TYPE_GPU`): Create the Vulkan texture and register it in `VkrTextureSystem` (`textures` array + `texture_map`, bump generation, refcount rules). Free the decoded pixel buffer after upload.
- In-flight tracking: maintain a `VkrTextureLoadTicket { state, handle, error, decode_job, gpu_job, refcount, name }` table keyed by texture name to dedupe requests and coordinate wait/poll.
- GPU execution: either (a) add a single GPU worker that runs on the main thread via a `vkr_job_system_pump(VkrJobTypeMask mask)` call each frame, or (b) spin up one GPU-only worker thread if/when backend is proven thread-safe. Default to (a) to keep Vulkan calls on the main thread.

## Implementation Steps
1) **Plumbing**: Extend `VkrTextureSystem` (and loader context) to hold a pointer to `VkrJobSystem` and a mutex guarding `texture_map`/free-slot bookkeeping. Add helpers for `job_type` masks (`RESOURCE`, `GPU`) in job descs. Compile with `STBI_NO_STDIO` (likely via `vkr_pch.h` or build flags).
2) **In-flight table**: Add a hashmap in `vkr_texture_system.c` for `VkrTextureLoadTicket` keyed by name. States: `PENDING_DECODE`, `DECODED`, `UPLOADING`, `READY`, `FAILED`. Track the final `VkrTextureHandle` + `VkrRendererError`.
3) **Decode job** (`RESOURCE`): Use filesystem API to read the file into memory, call `stbi_set_flip_vertically_on_load_thread`, then `stbi_load_from_memory`. Fill `TextureDecodeResult` and submit a dependent GPU job on success. On failure, mark ticket failed and signal any waiters.
4) **GPU job** (`GPU`): Use the decode result to call `vkr_renderer_create_texture` and register the texture (`find_free_slot`, map insert, generation). Guard with the texture-system mutex. Free pixel data after upload. On failure, mark ticket failed and clean up ticket.
5) **Job pumping**: Add `vkr_job_system_run_masked(VkrJobSystem*, Bitset8 mask)` (or similar) that lets the main thread drain one or more `GPU` jobs. Call from the render loop or a `vkr_texture_system_tick_uploads` helper.
6) **Public API**:
   - Replace the current synchronous `vkr_texture_system_load` implementation with an async-backed version: create/return an in-flight ticket and decode job; synchronous callers may optionally block by waiting on the ticket, but there is no single-threaded fast path.
   - Provide `vkr_texture_system_poll`/`wait` utilities and expose the ticket/job handles for integration points like material loading.
7) **Loader integration**: Update `texture_loader.c`, `material_loader.c` texture paths, and `view_skybox` cube-map path to use the async request + wait (or poll if appropriate). Ensure duplicate requests for the same name share the same ticket.
8) **Thread-safety & init**: Remove use of global `stbi_set_flip_vertically_on_load`; rely on the thread-local variant inside the decode job. Wrap `texture_map` mutations and `next_free_index` updates with the new mutex. Ensure ticket table mutations are also mutexed.
9) **Testing/validation**: Add a small harness or unit tests to validate: (a) multiple concurrent loads of distinct textures complete, (b) duplicate requests share work, (c) GPU jobs are only executed on the main-thread pump, (d) failures propagate to callers. Also validate the `STBI_NO_STDIO` path by forcing filesystem reads into memory and decoding via `stbi_load_from_memory`.

## Follow-Ups / Decisions to Lock
- If GPU calls are later proven safe on a worker thread, allow a configurable GPU worker instead of the main-thread pump.
- Add cancellation for tickets (set cancel flag, drop pending GPU job, free stbi buffer).
- Consider a staging allocator for decoded pixels to avoid heap churn once the basic flow is stable.
