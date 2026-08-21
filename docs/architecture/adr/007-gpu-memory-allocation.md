---
status: partial
updated: 2026-08-21
authority: adr
---
# ADR-007: Per-Resource Device-Memory Allocation

**Status:** Accepted (partial) — retained legacy policy. The selected Vulkan
1.4 implementation now uses the keyed block allocator specified by
[ADR-023](023-vulkan-1-4-bindless-capability-profile.md).

## Context

Vulkan separates resource objects (`VkBuffer`/`VkImage`) from
`VkDeviceMemory`. A renderer may allocate memory per resource or allocate large
blocks per memory type and suballocate ranges. The latter reduces allocation
count and driver overhead but adds pooling, alignment, fragmentation, and
lifetime machinery.

An offset allocator inside a shared `VkBuffer` is a separate level of the
problem: it can pack many logical ranges into that buffer without making
different Vulkan buffers/images share `VkDeviceMemory`.

## Decision

Keep direct device-memory ownership in the legacy Vulkan implementation:

- image creation allocates and binds its own `VkDeviceMemory`;
- buffer creation and resize allocate and bind their own memory;
- each readback buffer owns one allocation;
- no VMA or custom block allocator is present in that legacy path.

Legacy device allocation/free operations route through one tracked backend
wrapper. The Vulkan implementation instead owns keyed pooled-block and dedicated
allocation sites below its Vulkan-private strategy boundary.
Every `VulkanBuffer` also owns a `VkrDMemory` offset allocator over its virtual
byte range. Callers may use that to suballocate logical ranges when they
intentionally share the same buffer; it does not imply that all mesh geometry
currently uses a common mega-buffer.

Supporting behavior:

- a requested host-visible/device-local buffer memory type can fall back to
  host-visible memory when the combined type is unavailable, and resources
  retain the selected memory type's actual property flags;
- allocation sizes are reported to the project's GPU/Vulkan accounting tags;
- resource descriptions declare a fixed logical allocation owner, retained in
  the handle-keyed table so free removes the exact owner's bytes/count without
  classifying from names, usage, or memory types;
- the table exposes live/peak/total allocation counts and bytes globally and by
  logical owner plus per-memory-type distribution; saturation marks live totals
  and owner live/peak rows inexact;
- heap capacity is always reported and `VK_EXT_memory_budget` adds driver usage
  and budget when available;
- graph-resource statistics separately track graph-owned image/buffer sizes;
- Vulkan driver host allocations use project `VkAllocationCallbacks`, which is
  independent of this device-memory policy.

## Consequences

**Positive**

- Resource lifetime and ownership are simple and local.
- There is no device-memory allocator dependency or fragmentation policy to
  maintain.
- Shared buffers can still suballocate logical ranges where the loader/system
  is designed for it.

**Negative / risks**

- Allocation count scales with Vulkan resource count and is subject to the
  device's `maxMemoryAllocationCount`.
- Small resources pay alignment/granularity and driver-allocation overhead.
- There is no block reuse, defragmentation, eviction, or heap-budget policy.
- Telemetry reports usage but does not impose budgets, prevent exhaustion, or
  expose fragmentation.

A Debug Sponza capture on Apple M1 Pro/MoltenVK peaked at 206 live allocations
against `maxMemoryAllocationCount` ~1.07e9. Its DEVICE_LOCAL allocations
averaged 14.5 MB while host-visible allocations averaged 0.47 MB. This does not
justify a pool on that device; broader hardware and content measurements remain
necessary before choosing block sizes. See
[GPU device-memory baseline](../../performance/gpu-memory-baseline.md).

## Alternatives Considered

- **VMA.** Mature pooling/budget support, at the cost of an added implementation
  dependency and integration with project accounting. Still viable.
- **Custom blocks keyed by memory type.** Reuse the project's range allocator
  inside large device-memory blocks. Fits C11 but requires correct granularity,
  alignment, mapping, and dedicated-allocation handling.
- **Keep direct allocation indefinitely.** Acceptable only while measured
  allocation count and performance remain comfortably within target devices.

## Revisit When

- Pool when measured content/device limits or `vkAllocateMemory` time justify
  the complexity.
- Re-measure on discrete GPUs and any target exposing a materially lower
  allocation-count limit.
- Revisit with texture streaming, eviction, or graph transient aliasing.
