---
name: vkr-memory
description: VKR allocator selection, scope discipline, ownership, and lifetime rules. Use when allocating or freeing anything, choosing between Arena/VkrDMemory/VkrPool, storing hash-table keys, handling scene load/unload or hot-reload paths, interpreting allocator statistics, or diagnosing memory growth, leaks, and use-after-free.
---

# VKR Memory and Lifetime

This codebase uses arenas heavily. **Arenas keep a high-water mark and do not
support per-allocation frees**, so anything created and destroyed repeatedly —
scene load/unload, editor toggles, hot reload — must be designed with a
lifetime-aware allocator or it will grow without bound.

`VkrAllocator` (`lib/src/memory/vkr_allocator.h`) is the common interface over
three backends. Choosing the wrong one is the single most common source of
growth in this project.

## Allocator choice by lifetime

| Backend | Lifetime model | Use for |
|---|---|---|
| `Arena` | Bump allocation, bulk reset or destroy | Data sharing one lifetime; scratch; long-lived caches with intentional growth |
| `VkrDMemory` | Reserved/committed virtual memory with individual free | Registries, hash keys, reloadable objects, anything removed before shutdown |
| `VkrPool` | Fixed-size slots | Homogeneous objects with churn |

`VkrArenaPool` is a separate thread-safe pool of fixed-size chunks used to build
asset-loader scratch arenas. It is **not** a fourth `VkrAllocator` backend.

Decision rule:

- **Temporary / scratch** → allocate inside a scope
  (`vkr_allocator_begin_scope()` / `vkr_allocator_end_scope()`) and always end
  it, including on error paths.
- **Needs per-item free** → `VkrDMemory` or `VkrPool`, freed on remove or
  unload. If an item can be removed before shutdown, it must not come from an
  arena.
- **Long-lived cache** → arena-backed is fine *only* when the growth is
  intentional (caching up to the maximum content ever used) and documented at
  the system level.

The interface also provides aligned operations, tagged local/global accounting,
and `_ts` wrappers that synchronize through a caller-supplied mutex. **Allocator
objects are not intrinsically thread-safe**; only the global counters are
atomic.

## Allocator statistics are not OS memory

`vkr_allocator_print_global_statistics()` counts bytes reported *through*
`VkrAllocator`. A bulk free — `arena_destroy()`, an arena reset, a
`VkrArenaPool` chunk return — does **not** decrement the global tag counters,
because no per-allocation `vkr_allocator_free()` ever happened.

So apparent growth in the global stats may be an accounting artifact rather than
a leak. Before diagnosing a leak, confirm which it is.

To keep global stats accurate when freeing in bulk, do one of:

- end the scope that covered the allocations, or
- call `vkr_allocator_release_global_accounting(&allocator)` **immediately
  before** destroying the allocator's backing store.

The scene runtime already does this before destroying its scene arena. Follow
that pattern.

## Hash-table key ownership

This is the most common real leak pattern in the codebase.

- Keys stored in a table must point to memory that stays stable for the entire
  lifetime of the entry. A key pointing into an arena that is later reset is a
  use-after-free waiting for the next probe.
- If entries are removed on unload, allocate keys from a freeable allocator
  (`VkrDMemory` or a pool) and free them when removing.
- **Free the key after calling the table's remove function.** Removal probes
  compare against the stored key pointer; freeing first can corrupt the probe.
- Only free keys you own. Defaults may use string literals — guard with
  `vkr_dmemory_owns_ptr()` before freeing.

## Borrowed views

`String8` is length-prefixed and **not** null-terminated internally. A `String8`,
`Array(T)`/`Vector(T)` view, or raw pointer into arena memory dies when that
arena is reset or destroyed — even though every value was valid when it was
created.

Before publishing a view that outlives the current call, use one proven policy:
exact pre-reservation before any view is formed, fixed-capacity storage, or an
owning arena whose scope dominates every consumer. Container growth reallocates,
so a pointer taken before a push may dangle after it.

`VkrRenderPacket` payload arrays are caller-owned and must stay alive until
`vkr_renderer_submit_packet()` returns.

## Acquire/release symmetry

Every successful acquire has a matching release on **all** paths, including
early error exits and partially-completed scene loads. Prefer one cleanup path
(`goto cleanup;`) or an explicit cleanup helper in multi-step loaders over
duplicated teardown at each return.

This applies to renderer handles (`TextureHandle`, `BufferHandle`, pipelines,
render targets), scopes, and pool slots alike.

## Vulkan-specific guidance

- Do not allocate per-scene CPU-side Vulkan objects — pipelines, shaders,
  tracking arrays — from long-lived arenas if those objects are destroyed and
  recreated per scene. Use a pool or `VkrDMemory`, or cache the pipelines across
  scenes.
- Treat SPIR-V, glTF, and KTX2 file byte buffers as temporary: allocate them
  from a scope (or free explicitly) after `vkCreateShaderModule()` or the
  corresponding upload completes.
- GPU device memory is separate and is **not** managed by `VkrAllocator`. There
  is no VMA and no block allocator: `vkAllocateMemory` is called per image, per
  buffer create/resize, and per readback buffer. A `VulkanBuffer` owns a
  `VkrDMemory` offset allocator for ranges *inside* that buffer, which permits
  suballocation where callers deliberately share a buffer — it does not make
  arbitrary buffers or images share device memory. See ADR-007.
- Vulkan host allocations (`VkAllocationCallbacks`) use a dedicated `VkrDMemory`
  reservation plus a small refcounted command-scope arena, so driver host
  allocations appear in the project's statistics. This is unrelated to device
  memory.

## Pre-merge checklist

- Does any data outlive the current scope? If yes, it must not point into a
  scratch arena or a temp scope.
- Is every `arena_destroy()` or bulk reset paired with
  `vkr_allocator_release_global_accounting()` where global stats must stay
  accurate?
- Do "created" counters match "destroyed/released" counters after a full
  load → unload cycle — pipelines, instance states, materials, textures, meshes?
- Are hash keys freed after removal, and only when owned?
- Does every acquire have a release on the error path, not just the happy path?
- Run a repeated load/unload cycle and confirm the tag totals return to their
  starting values, or that any residual is explained.

Repeated-load measurement and handle counters are the required validation. The
policies above reduce reload growth; they do not by themselves prove a path is
leak-free.
