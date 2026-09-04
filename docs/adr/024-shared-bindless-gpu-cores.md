---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-024: Shared allocation, publication and completion cores

## Status

Accepted.

## Context

Metal and Vulkan need the same generation, range, retirement and bounded-request
rules while native resources, addressability and barriers differ.

## Decision

Share `vkr_gpu_memory`, `vkr_gpu_submit_ring`, `vkr_gpu_slot_table`,
`vkr_gpu_abi` and `vkr_capture_ring` with real callers in both implementations.
Cores describe ranges, generations and completion; backend adapters own native
heaps/buffers/images, mapping, residency, descriptors and queue operations.

Vulkan suballocates keyed DEVICE, UPLOAD, READBACK and STAGING blocks. Keys distinguish
resource kind, exact memory type and device-address requirements. Host-visible
blocks stay mapped; required dedicated allocations bypass the range allocator
while retaining accounting. Metal uses placement heaps and upload/readback
rings through the same range and submit contracts. There is no VMA, online
defragmentation or graph transient aliasing.

`VkrAssetPublisher` publishes immutable geometry, texture, sampler and material
generations. Replacement publishes new state before retiring old state against
last GPU use. Generation slots are not reusable merely because a CPU handle was
released. Per-frame candidate, instance and root storage belongs to completion-
protected slots and is sized before recording.

CPU workers prepare resources; render-thread finalization records publication
and uploads. Vulkan batches pending writes into submission and retires staging
at that submit value. Metal batches texture payloads into upload slices and
proves completion before consuming publication. Capture/picking readback uses
bounded requests; completed results remain owned until explicit release.

Metrics distinguish logical requested/reserved bytes from native allocation,
retired storage, capacity failures and owner classes. Vulkan driver host memory
uses null allocation callbacks and is outside VKR CPU allocator accounting.

## Consequences

The shared contract avoids two retirement implementations without hiding native
resource behavior. Ring pressure fails or waits at owning boundaries; it cannot
overwrite in-flight storage. Reusable pools retain capacity after logical release.

## Alternatives considered

Per-resource Vulkan device allocation was retired. A generic resource/command
RHI would add a second owner without unifying native policy. A universal memory
pool would erase memory-type and addressability constraints.

## Revisit when

Fragmentation, heap pressure or transfer cost demonstrates the need for a new
placement or upload policy with explicit last-use ownership.

## Implementation

[`vkr_gpu_memory.c`](../../lib/src/renderer/vkr_gpu_memory.c),
[`vkr_gpu_slot_table.c`](../../lib/src/renderer/vkr_gpu_slot_table.c),
[`vkr_asset_publisher.h`](../../lib/src/renderer/vkr_asset_publisher.h),
[`vkr_vulkan_memory.c`](../../lib/src/renderer/vulkan/vkr_vulkan_memory.c), and
[`vkr_metal_memory.c`](../../lib/src/renderer/metal/vkr_metal_memory.c).
This record incorporates the surviving lifetime rules from former ADR-007/008.
