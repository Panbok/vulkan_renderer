---
name: vkr-memory
description: Select VKR allocators and verify CPU/GPU ownership, borrowed views, retirement, and load/unload memory behavior. Use for allocation changes or memory diagnosis.
---

# VKR memory and lifetime

Choose allocation from the release event and access pattern before choosing an
API. First consider bounded stack values or reuse of existing owned capacity.
Allocate only when storage or lifetime requires it. State who owns the bytes,
who borrows them, when the last consumer finishes, and how storage returns for
reuse.

## Allocator selection

`lib/src/memory/vkr_allocator.h` exposes these CPU backends:

| Lifetime and shape | Allocator | Release rule |
|---|---|---|
| Scratch or objects destroyed together | `Arena` | End the scope or destroy the owning arena |
| Variable-size objects removed independently | `VkrDMemory` | Free each removed object with its original size/alignment contract |
| Fixed-size objects with churn | `VkrPool` | Return each slot; size and alignment must fit the pool configuration |

A scene arena is valid for scene-owned objects discarded together. An object
removed independently while its arena remains live needs freeable storage.
An arena cache needs an explicit bound and reclamation policy; intentional
high-water retention is not permission for unbounded reload growth.

`VkrArenaPool` supplies synchronized chunks for asset-loader arenas. It is not a
fourth `VkrAllocator` backend. Allocator instances are not inherently thread-safe;
prefer task-owned scratch and partitioned ownership. `_ts` operations use the
caller's mutex and do not belong in per-draw work.

Reserve and commit hot-path capacity before recording. A cheap-looking arena
push can still commit pages or allocate another block. Select layout and batch
lifetime to avoid repeated allocation, copies, and pointer chasing.

## Scope and accounting contract

Use `vkr_allocator_begin_scope()` only on an allocator that supports scopes and
check its returned scope at the cold boundary. The arena adapter implements
scopes; the pool adapter does not, and the DMemory adapter provides no scope
callbacks. Pair a valid scope with `vkr_allocator_end_scope(&scope, tag)` on every
exit, in reverse nesting order. Use explicit frees for temporary DMemory/pool
allocations. Do not let a scope rewind another task's live allocations.

Allocator tag totals measure bytes reported through `VkrAllocator`, not process
resident memory. Raw arena reset/destroy or chunk return bypasses that accounting.
End the tracked scope for reusable scratch. Before final bulk backing-store
destruction, call `vkr_allocator_release_global_accounting(&allocator)` once if
individual frees did not reconcile it. That call marks accounting released; it
is not a reset operation for a still-live allocator.

## Keys, views, and cleanup

- Hash keys must remain stable for the entry's lifetime. Remove an entry before
  freeing its owned key because removal still compares the stored key. Prefer
  explicit ownership; use `vkr_dmemory_owns_ptr()` only for an existing mixed
  literal/owned-key policy tied to that allocator.
- `String8` is a length-prefixed, non-null-terminated view. Strings, array views,
  and pointers into scratch expire at scope end. Container growth can invalidate
  earlier pointers even while the allocator remains alive. Reserve before
  publishing views, use stable storage, or store handles/offsets as appropriate.
- Every successful acquisition needs a release on failure and cancellation as
  well as success. Use one cleanup path for partial loads when it makes the
  acquisition order explicit. Release only acquired resources, in dependency
  order.
- `VkrRenderPacket` arrays remain caller-owned until submission returns. That
  CPU borrowing boundary does not retire GPU resources referenced by the packet.

## GPU memory and completion

`VkrAllocator` manages CPU bytes. Selected backends own device allocations and
use shared GPU memory, submit, slot, and capture cores. Vulkan uses keyed
DEVICE/UPLOAD/READBACK pools plus dedicated paths; there is no VMA. Current
Vulkan allocation calls pass null host-allocation callbacks, so allocator tag
totals do not include all driver host memory.

Invalidate logical handles on destruction. Reuse device memory, descriptors,
material slots, staging ranges, and readback storage only after all recorded
uses are cancelled or submitted and submitted uses complete. A frame count is
not completion proof. Keep asynchronously borrowed upload bytes until the API
or staging owner has finished consuming them. Synchronously consumed file bytes,
such as SPIR-V passed to shader-module creation, can be released after that call.

## Evidence

For lifecycle changes, run a focused repeated load/unload or create/destroy case
through `vkr-harness`. Compare live bytes and live handle/slot counts after GPU
retirement drains. Separate those values from committed capacity and OS memory;
state any bounded cache retention. Inspect partial-failure cleanup where the
change affects it. Choose additional checks through `vkr-validation` only when
they detect a named failure the lifecycle run cannot expose.
