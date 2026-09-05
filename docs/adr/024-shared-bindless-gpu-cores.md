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

Vulkan frame slots separate directly read UPLOAD storage from copy-only candidate
STAGING storage. Direct storage starts at 16 MiB per slot and retains the 75 MiB
ceiling. Candidate staging grows on demand, bounded by the two candidate streams'
88 MiB maximum. Frame preflight reserves both before publishing pointers or GPU
addresses, after the slot's last submission completes. Capacity is retained until
slot teardown; no draw or dispatch grows either buffer. This avoids reserving
225 MiB of a discrete GPU's small mapped device-local heap at startup. A larger
first workload can incur an allocation hitch.

Dedicated Vulkan UPLOAD allocations still retry the next eligible ranked memory
type on device-memory exhaustion. Large direct payloads can exceed the preferred
heap even with the split. The buffer, mapping and completion owner stay unchanged;
fallback host-memory reads can cost PCIe bandwidth. Other allocation errors remain
fatal to creation.

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

`VkrRenderAssets` owns CPU asset systems, loaders, their asynchronous allocators
and load scratch. It borrows `VkrAssetPublisher` from the renderer; that publisher
outlives all assets. Application frame scratch is separate, and its arrays live
through `render_frame`. Loader staging scopes end only after their consumers have
finished reading them.

The application joins workers before asset teardown and proves GPU completion
before scene unload, then drains work caused by destruction. Registered loader
contexts survive subsystem release. Partial initialization uses the same ordering.
The renderer does not own scene resources or perform hidden scene teardown waits.

The resource pump receives explicit `VkrResourceSubmissionState` values from its
caller: last submitted serial, completed serial and whether a frame is active.
The resource owner still stamps active-frame publication with the next submit
and waits for completion before READY. This removes renderer callback queries
from resource-state progression without changing retirement semantics. The
application calls `vkr_render_assets_pump()` after successful frame acquisition.
Metal keeps that frame slot reserved while uploads acquire another slot only after
its previous submission completes. At least two native command slots are required;
uploads cannot reset the reserved frame slot.

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
[`vkr_render_assets.c`](../../lib/src/renderer/systems/vkr_render_assets.c),
[`vkr_vulkan_memory.c`](../../lib/src/renderer/vulkan/vkr_vulkan_memory.c), and
[`vkr_metal_memory.c`](../../lib/src/renderer/metal/vkr_metal_memory.c).
This record incorporates the surviving lifetime rules from former ADR-007/008.
