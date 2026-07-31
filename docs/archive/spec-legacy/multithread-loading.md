---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Multithreaded Mesh & Texture Loading – Detailed Plan (with code references)

## Reference (other engine)
- Uses the job system end-to-end:
  - Mesh: `mesh_load_job_start` does `resource_system_load` (disk/CPU), copies `mesh_load_params` into result_data; `mesh_load_job_success` runs on the job thread and creates GPU geometry (renderer not thread-safe noted, but still done there).
  - Texture: `texture_load_job_start` loads image via `resource_system_load` with `image_resource_params` (`flip_y`), inspects alpha, builds a temp texture; `texture_load_job_success` uploads via `renderer_texture_create`, swaps into the live texture, unloads the resource.
- STBI handling: `STBI_NO_STDIO`, custom filesystem; `stbi_set_flip_vertically_on_load_thread(flip)` per job, `stbi_load_from_memory` with caller-managed buffers. Thread-safe because flip setting is per-thread; no global flip.
  - See `stuff/texture_system.c:430-540`, `stuff/image_loader.c:1-120`.
- Mesh job references (other engine):
  - Job wiring and CPU/GPU work: `stuff/mesh.c:1-82` (`mesh_load_job_start`, `mesh_load_job_success`, `mesh_load_job_fail`, `mesh_load_from_resource`).
  - Mesh job success does GPU uploads (geometry creation) on the job thread and bumps generation.
- Texture job references (other engine):
  - Job wiring and GPU swap: `stuff/texture_system.c:430-536` (`texture_load_job_start`, `texture_load_job_success`, `texture_load_job_fail`, `load_texture`).
  - STBI usage and alpha detection happen in `texture_load_job_start` (`stuff/texture_system.c:484-521`).

## Goals & constraints for our engine
- Make mesh/texture loads asynchronous by default; APIs return handles immediately and jobs drive the work.
- Disk/CPU decode on worker threads; GPU uploads either on a dedicated GPU worker (if renderer can be called there) or marshalled to the main/render thread via a job of type GPU.
- Avoid duplicated loads; coalesce in-flight requests.
- Keep resource system loaders callable from worker threads (filesystem and STBI usage must be thread-safe).
- Permit callers to wait/poll on job handles if they need readiness.

## Why staging/apply split (kept, but purely via job system)
- We still “let the job system handle everything”, but we distinguish two job phases to respect renderer thread-safety:
  - **Stage (RESOURCE job)**: disk IO + decode into CPU buffers; never call renderer.
  - **Apply (GPU job)**: consume staged data and call renderer to create GPU objects; depends on the stage job handle.
- This split allows: clear ownership of temp buffers, deterministic unload of resource handles, and an easy toggle between “GPU worker” and “main-thread apply” by choosing job type/worker mask. The job system enforces ordering through dependencies; no external queues needed beyond job submission.
  - Job system dependency/core code to rely on: `lib/src/core/vkr_job_system.c` at `job_system_register_dependency_locked` and enqueue logic lines ~15-200; `vkr_job_add_dependency`/`vkr_job_mark_ready` ~456-520.
  - Worker masks/type masks defined in `lib/src/core/vkr_job_system.h` and configured in `vkr_job_system_config_default` (`lib/src/core/vkr_job_system.c` ~219-236).

## API changes (breaking single-threaded assumptions)
- **Texture system**
  - `vkr_texture_system_load_async(String8 name, VkrJobHandle *out_job, VkrTextureHandle *out_handle)`: returns a handle immediately, enqueues stage+apply jobs; handle becomes valid when apply completes. Current synchronous `load` becomes a thin wrapper that waits on `out_job`.
  - Add `vkr_texture_system_poll_ready(handle)` / `wait(handle)` helpers that inspect generation or call `vkr_job_wait`.
- **Mesh manager**
  - `vkr_mesh_manager_load_async(desc, VkrJobHandle *out_job, uint32_t *out_mesh_index)`: returns mesh index reserved; stage+apply jobs fill it. Existing `load` becomes a wait-wrapper.
- **Resource system**
  - Ensure loaders are reentrant/thread-safe; expose a “worker-safe” entry (same signature) to be used from jobs. No global mutable state beyond the registered loader table.
  - Touch points:
    - Texture system logic currently synchronous: `lib/src/renderer/systems/vkr_texture_system.c` load paths ~840-980 (and cube map paths ~939+). Async wrappers and handle readiness must be added here.
    - Texture loader entrypoint: `lib/src/renderer/resources/loaders/texture_loader.c` lines ~1-120 calls `vkr_texture_system_load`; needs async-aware interface.
    - Mesh manager synchronous load: `lib/src/renderer/systems/vkr_mesh_manager.c` load path ~330-470 builds meshes synchronously; will be refactored into stage/apply jobs.
    - Mesh loader (CPU decode) already exists: `lib/src/renderer/resources/loaders/mesh_loader.c` (readers/builders around lines ~1-360); stage job should leverage this code on worker threads.

## Texture pipeline (job-driven)
1) **Stage job (`VKR_JOB_TYPE_RESOURCE`)**
   - Inputs: texture name, desired channels, flip_y flag.
   - Work:
     - Call resource loader (stb) on worker thread:
       - Define `STBI_NO_STDIO`; use our filesystem to read bytes.
       - Set `stbi_set_flip_vertically_on_load_thread(flip_y)` per job.
       - Use `stbi_load_from_memory` to RGBA.
     - Detect alpha < 255 to set transparency flag.
     - Fill `TextureStageResult` (pixels buffer, width/height/channels, flags).
   - Outputs: stage result, resource handle for unload.
   - Files/locations to adjust:
     - Stage job implementation will live alongside texture system or a new loader helper; reference current loader at `lib/src/renderer/resources/loaders/texture_loader.c:1-120`.
     - Renderer upload currently done inline in `vkr_texture_system_load_from_file` `lib/src/renderer/systems/vkr_texture_system.c:840-980`; will be moved to apply job.
2) **Apply job (`VKR_JOB_TYPE_GPU` or main thread) depends on stage**
   - Work:
     - Create GPU texture via renderer; on failure, set handle to default and mark load_failed.
     - Swap into texture slot (assign id/generation, refcount stays).
     - Free CPU pixels and unload the resource.
3) **In-flight coalescing**
   - Track name -> {handle, stage_job} in a small map to avoid duplicate loads; subsequent requests return the same handle/job. Clearing occurs after apply completes.

## Mesh pipeline (job-driven)
1) **Stage job (`VKR_JOB_TYPE_RESOURCE`)**
   - Inputs: mesh path, pipeline domain/shader overrides.
   - Work:
     - Call `vkr_resource_system_load` for mesh; obtain `VkrMeshLoaderResult`.
     - Copy subset metadata into `MeshStageResult`: geometry configs, material names/handles, pipeline domains, shader overrides. No renderer calls.
   - Outputs: staged subsets/material refs, resource handle for unload.
   - Files/locations to adjust:
     - CPU decode already in `lib/src/renderer/resources/loaders/mesh_loader.c` (parsing/cache logic around lines ~1-360). Stage job should run this on worker thread.
     - Current synchronous mesh creation path in `lib/src/renderer/systems/vkr_mesh_manager.c:330-470` builds geometries/materials inline; will move to apply job.
2) **Apply job (`VKR_JOB_TYPE_GPU` or main thread) depends on stage**
   - Work:
     - For each subset: create geometry via geometry system; resolve/acquire materials; determine pipeline domain/shader override.
     - Build submeshes and place into pre-reserved mesh slot; bump generation; update bounding info.
     - Release temp geometry handles (ownership transferred) and unload the resource.
     - On failure: destroy any created geometries/material refs and mark mesh entry failed.
3) **In-flight coalescing**
   - Track mesh path -> {mesh_index, stage_job}; reuse for duplicate requests; clear after apply.

## Thread-safety & renderer access
- If renderer is single-threaded: mark apply jobs as `VKR_JOB_TYPE_GPU` but bind GPU workers to the render/main thread only, or route apply work to a main-thread pump that executes pending apply jobs (still created via job system, but the worker mask restricts to the main-thread worker).
- If renderer is made thread-safe: enable a dedicated GPU worker in job system with `worker_type_mask` including `VKR_JOB_TYPE_GPU`.
  - Worker masks and job type masks come from `lib/src/core/vkr_job_system.h` and are consumed in the dequeue logic at `lib/src/core/vkr_job_system.c:96-128`.

## STBI specifics to document & match
- **How the other engine does it:** `stuff/image_loader.c:1-120` defines `STBI_NO_STDIO`, uses the engine filesystem to read bytes, sets `stbi_set_flip_vertically_on_load_thread(typed_params->flip_y)` per job/thread, then calls `stbi_load_from_memory` to decode; uses per-job buffers (no global state), and frees raw bytes after decode. Flip is per-thread, so thread-safe.
- **What we should do:**
  - Mirror `STBI_NO_STDIO` and always read files via our filesystem into a temporary buffer (stage job).
  - Call `stbi_set_flip_vertically_on_load_thread(flip)` inside the stage job before decode to avoid global flip state; do NOT call the global `stbi_set_flip_vertically_on_load`.
  - Decode with `stbi_load_from_memory` to RGBA; free the raw file buffer after decode. Free STBI pixel buffer after GPU upload in the apply job.
  - For cube maps, disable vertical flip, and load faces in order `_r,_l,_u,_d,_f,_b` (see `stuff/texture_system.c:430-540` for the reference’s face handling; our cube map path is in `lib/src/renderer/systems/vkr_texture_system.c` around the cube load functions ~939+).
  - Update our loader at `lib/src/renderer/resources/loaders/texture_loader.c:1-120` to follow the per-thread flip and memory ownership pattern.

## Migration steps (with file anchors)
1) Add stage/apply result structs for textures and meshes; add in-flight maps for coalescing.
   - Touch: texture system (`lib/src/renderer/systems/vkr_texture_system.c`), mesh manager (`lib/src/renderer/systems/vkr_mesh_manager.c`), job system API usage (`lib/src/core/vkr_job_system.h/.c`).
2) Extend job system usage helpers: submit stage job, then submit apply job with dependency; expose returned handles.
   - Use `vkr_job_submit`/`vkr_job_add_dependency`/`vkr_job_mark_ready` (`lib/src/core/vkr_job_system.c:415-520`).
3) Update texture/mesh public APIs to async-first; adapt existing sync callers to wait.
   - Texture loader entrypoint `lib/src/renderer/resources/loaders/texture_loader.c` and texture system load APIs `lib/src/renderer/systems/vkr_texture_system.c`.
   - Mesh manager load path `lib/src/renderer/systems/vkr_mesh_manager.c:330-470`.
4) Adjust resource loaders/STBI usage for per-thread flip and filesystem-backed loads.
   - STBI usage in our loader `lib/src/renderer/resources/loaders/texture_loader.c` and ref in `stuff/image_loader.c`.
5) Implement renderer access policy (main-thread apply or GPU worker mask).
   - Configure worker masks in job system init and in job submissions.
6) Add tests: job chaining (stage->apply), duplicate request coalescing, generation bump after apply, and STBI flip correctness (e.g., known flipped image).
   - Add to `tests/src` alongside existing suites; leverage job system tests and new async APIs.

## Readiness checklist (what to inspect/modify before coding)
- Job system:
  - Ensure worker type masks include GPU-capable worker or main-thread binding (`vkr_job_system_config_default` lines ~219-236).
  - Dependency/ready logic already supports stage→apply (`lib/src/core/vkr_job_system.c` lines ~61-199 and ~415-520).
- Resource system:
  - Confirm thread safety of loader table and filesystem; adjust if needed in `lib/src/renderer/systems/vkr_resource_system.c`.
- Texture path:
  - STBI per-thread flip and filesystem read in `lib/src/renderer/resources/loaders/texture_loader.c:1-120`.
  - Async load/apply and in-flight map in `lib/src/renderer/systems/vkr_texture_system.c` (current sync load ~840-980, cube map ~939+).
- Mesh path:
  - Stage on worker using existing parsing in `lib/src/renderer/resources/loaders/mesh_loader.c` (~1-360 and beyond).
  - Apply/build submeshes in `lib/src/renderer/systems/vkr_mesh_manager.c:330-470` (currently synchronous).
- Other engine references for parity:
  - Mesh job flow `stuff/mesh.c:1-82`.
  - Texture job flow and STBI usage `stuff/texture_system.c:430-536`, `stuff/image_loader.c:1-120`.
- Tests:
  - Add async load tests under `tests/src` to verify stage→apply, coalescing, and readiness semantics using `vkr_job_wait`.
