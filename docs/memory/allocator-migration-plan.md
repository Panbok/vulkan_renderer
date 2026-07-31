---
status: partial
updated: 2026-07-31
authority: design
---
# Allocator Migration Plan

## Goals & Constraints
- Route all allocations through `VkrAllocator`, including temporary/scope allocations (replacement for direct `arena_alloc`, `scratch_create`, and `vkr_dmemory_alloc`).
- Preserve callee transparency from the temp-allocation proposal: callees call `vkr_allocator_alloc` normally; callers decide permanence by wrapping in scopes (already wired for arena/scratch in `vkr_arena_allocator.c`).
- Keep existing arena benefits (O(1) scratch reset) via allocator scopes; maintain statistics for temp vs. permanent usage (already implemented for arena).
- Avoid regressions in renderer/core systems; keep Vulkan buffer offset logic functional while bringing it under the abstract allocator.

## Current Landscape (lib/src snapshot)
- Direct `Arena`/`Scratch` in: containers (`array/vector/queue`), string utils, filesystem, logger, event system, job system, resource loaders (mesh/material/shader/texture), renderer systems (geometry, shader, view, pipeline registry, mesh manager, texture, material), renderer frontend, Vulkan backend/device/swapchain/shaders, plus temporary arenas in tests/tools.
- `VkrAllocator` already wraps arenas (`vkr_arena_allocator.c`) and implements scope tracking; scopes are unused at call sites and `Scratch` is still the primary temp mechanism.
- `VkrDMemory` is only consumed via `vkr_dmemory_allocator_create` in Vulkan buffers; migration simply means using the existing `VkrAllocator` wrapper instead of touching `vkr_dmemory` directly.
- Filesystem API signatures have been moved to `VkrAllocator*`; call sites in tests plus shader/material/mesh/texture loaders and Vulkan shader setup now pass allocators. Filesystem still casts to `Arena` internally for string helpers until the string module migrates.

## Phase 0 – Prerequisites & Decisions
- [ ] Confirm scope semantics are already satisfied: LIFO scopes, tag on `end_scope`, arena scopes map 1:1 to scratch reset (implemented in `vkr_arena_allocator.c`); no new scope design required.
- [ ] Decide on transitional helpers (outside allocator code): keep helpers/macros minimal; prefer direct call-site swaps (treat `VkrAllocator` like `malloc`/`free`, with scope wrappers only where scratch was used).
- [ ] Define ownership: every subsystem struct should own a `VkrAllocator` (or pointer) plus an optional temp allocator reference; avoid storing raw `Arena*` in new code.
- [ ] Tag mapping policy: enforce alignment between `VkrAllocatorMemoryTag` and `ArenaMemoryTag` at call sites (no allocator changes).

## Phase 1 – Migration Enablement (no allocator core changes)
- [ ] Convert low-level modules (filesystem, containers, strings) to accept `VkrAllocator*` directly instead of `Arena*`/scratch; use scope API in those modules where scratch was used, and update call sites even if it breaks higher layers (to surface remaining migrations).
  - [x] Filesystem signatures switched to `VkrAllocator*` and initial call sites updated (tests, shader/material/mesh/texture loaders, Vulkan shader loading).
  - [ ] Containers/string helpers still accept `Arena*`; migrate them so filesystem path helpers can drop internal casts.
- [ ] Identify all struct fields storing `Arena*`/`Scratch`/`VkrDMemory` and define replacement fields/pointers to `VkrAllocator` instances without modifying allocator internals.
- [ ] Document migration guidelines in-code (brief comments) and in `docs/` for contributors to prevent new direct arena/scratch usage.

## Phase 2 – Foundation Library Migration (Utilities & Shared Types)
- [ ] Containers (`array.h`, `vector.h`, `queue.h`, `vkr_hashtable.h`): switch APIs to accept `VkrAllocator*` instead of `Arena*`; update internal allocations to `vkr_allocator_alloc`. Provide shims/wrappers for legacy code during transition.
- [ ] String utilities (`containers/str.c/h`): accept `VkrAllocator*` (plus optional `VkrAllocatorScope` usage for temps); remove direct `arena_alloc` usage.
- [x] Filesystem (`filesystem.c/h`): migrate signatures to `VkrAllocator*` (not raw `Arena*`) for buffers/paths; replace scratch with allocator scopes. Follow-up: once string/container APIs move to allocators, remove remaining internal `Arena` casts.
- [ ] Event data buffer and hash/bitset helpers: convert to `VkrAllocator` ownership.
- [ ] Update any helper macros that embed `Arena*` (e.g., `string_substring`, formatting helpers) to allocator equivalents.

## Phase 3 – Core & Renderer Systems Migration
- Application/Core
  - [ ] `application.h`: store main allocator + temp allocator as `VkrAllocator`; stop exposing raw arenas.
  - [ ] Logger/event/job system/thread primitives: migrate to allocator + scopes (replace per-thread scratch arenas; enforce scope cleanup on thread exit).
  - [ ] ECS/entity/world: ensure `world->alloc` is the single source; remove direct casts to `Arena*`.
- Resource Loading
  - [ ] Shader/material/mesh/texture loaders: change contexts to carry `VkrAllocator*` (+ temp allocator); swap scratch to `vkr_allocator_begin_scope/end_scope`.
  - [ ] Update loader job payloads and builder structs to drop `Arena*` fields; adjust unit tests/fixtures.
- Renderer Frontend & Systems
- [ ] `renderer_frontend`: own persistent allocator and temp allocator as `VkrAllocator`; eliminate `scratch_arena` usage, replace with scopes at call sites (e.g., shadercfg parsing, render target regeneration).
- [ ] Systems: geometry, shader, material, texture, mesh manager, view, pipeline registry, camera – convert struct fields and init paths to `VkrAllocator`; wrap temp work in scopes.
- [ ] Renderer resource system and pipeline registry: update internal caches/tables to use allocator-based containers.
- Vulkan Backend
  - [ ] Backend/device/swapchain/shaders/pipeline/utils: replace `temp_arena`/`scratch` with temp scopes from a `VkrAllocator` owned by backend state.
  - [ ] Buffer handling: ensure `vkr_dmemory_allocator` is used via `VkrAllocator` consistently (no changes to allocator implementation).
  - [ ] Verify render target and descriptor set setup use allocator scopes instead of raw arenas for transient arrays.

## Phase 4 – Cleanup & Enforcement
- [ ] Deprecate or gate direct `arena_alloc`/`scratch_*` usage behind internal-only headers; remove from public-facing headers once call sites migrate.
- [ ] Remove redundant arena fields from structs; ensure single allocator source of truth per subsystem.
- [ ] Update docs/examples to show `VkrAllocator` + scopes; add a short “temp allocation” usage section.
- [ ] Add CI/static checks (grep or clang-tidy rule) to flag new `arena_alloc`/`scratch_create` in lib/src.
- [ ] Audit stats reporting: ensure allocator/global stats reflect new usage patterns; add debug logging hooks if needed.

## Validation & Rollout Checklist
- [ ] Smoke tests/build across all platforms; run renderer startup path to ensure allocators are initialized before use.
- [ ] Memory-leak/regression pass: confirm scopes release expected bytes; validate `total_allocated` vs. tag deltas.
- [ ] Vulkan buffer resize/growth paths: verify `vkr_dmemory` alloc/free counts tracked through allocator stats.
- [ ] Remove transitional shims once no external `Arena*` references remain in lib/src.
