---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-006: CPU allocation by lifetime

## Status

Accepted.

## Context

Scratch, scene-lifetime state and independently removable records have different
release points. Arena allocation cannot reclaim individual reloadable keys.

## Decision

Expose arena, `VkrDMemory` and fixed-slot `VkrPool` through `VkrAllocator`.
Use arenas for a shared bulk lifetime, DMemory for individual release, and pools
for homogeneous churn. `VkrArenaPool` separately supplies synchronized chunks
for loader scratch; it is not another allocator-interface backend.

Capacity growth can invalidate borrowed array pointers. The owner must establish
capacity before publishing views or entering hot recording loops. Allocators
are not intrinsically thread-safe: `_ts` calls use caller-supplied synchronization.
Local tagged counters and atomic global counters report ownership. Bulk arena
destruction reconciles accounting with
`vkr_allocator_release_global_accounting()` when individual frees are skipped.

Vulkan uses null `VkAllocationCallbacks`; driver host allocation is outside these
CPU totals. Device-memory accounting belongs to ADR-024.

Allocator scopes already track temporary bytes and nesting without changing
callee allocation APIs. Scope support is explicit per allocator; arena scope
callbacks restore scratch lifetime and update accounting.

## Consequences

Lifetime and release remain explicit C contracts. Arena high-water retention is
expected; neither accounting nor allocator choice proves loader teardown is
balanced.

## Alternatives considered

Arena-only storage cannot reclaim independent records. General allocation for
every request loses bulk scope semantics. Encoding every lifetime in an API type
would require broader interfaces.

## Revisit when

Load/unload growth or allocator contention identifies a concrete ownership or
synchronization defect.

## Implementation

[`memory/`](../../lib/src/memory),
[`vkr_vulkan_device.c`](../../lib/src/renderer/vulkan/vkr_vulkan_device.c), and
[`vkr_scene_system.c`](../../lib/src/renderer/systems/vkr_scene_system.c).
