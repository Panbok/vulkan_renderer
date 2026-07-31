---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-007: Per-Resource Device-Memory Allocation

**Status:** Accepted (partial) — simple current policy with an acknowledged
scalability limit.

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

Keep direct device-memory ownership for now:

- image creation allocates and binds its own `VkDeviceMemory`;
- buffer creation and resize allocate and bind their own memory;
- each readback buffer owns one allocation;
- no VMA or custom block allocator is present.

There are four direct `vkAllocateMemory` sites in the backend. Every
`VulkanBuffer` also owns a `VkrDMemory` offset allocator over its virtual byte
range. Callers may use that to suballocate logical ranges when they intentionally
share the same buffer; it does not imply that all mesh geometry currently uses
a common mega-buffer.

Supporting behavior:

- a requested host-visible/device-local buffer memory type can fall back to
  host-visible memory when the combined type is unavailable;
- allocation sizes are reported to the project's GPU/Vulkan accounting tags;
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
- Existing accounting reports requested allocation sizes but does not expose
  heap budgets, fragmentation, or proximity to the device allocation-count
  limit.

No captured telemetry currently proves that Sponza or San Miguel approaches a
specific limit. Allocation count, heap usage, and load-time cost must be
measured before making that claim or selecting pool block sizes.

## Alternatives Considered

- **VMA.** Mature pooling/budget support, at the cost of an added implementation
  dependency and integration with project accounting. Still viable.
- **Custom blocks keyed by memory type.** Reuse the project's range allocator
  inside large device-memory blocks. Fits C11 but requires correct granularity,
  alignment, mapping, and dedicated-allocation handling.
- **Keep direct allocation indefinitely.** Acceptable only while measured
  allocation count and performance remain comfortably within target devices.

## Revisit When

- Add allocation-count and `VK_EXT_memory_budget` telemetry first.
- Pool when measured content/device limits or `vkAllocateMemory` time justify
  the complexity.
- Revisit with texture streaming, eviction, or graph transient aliasing.
