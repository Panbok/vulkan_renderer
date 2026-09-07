---
status: implemented
updated: 2026-09-07
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

Host-visible Vulkan buffer placements satisfy both native memory requirements
and the 16-byte alignment of typed CPU vector records. Transfer-only staging
buffers can report a four-byte native requirement; using that alone permits
misaligned candidate-row stores in optimized builds. Buffer-relative upload
offsets retain their existing typed alignment, and resource retirement is unchanged.

Metal creates private placement heaps on demand instead of reserving one large
resident heap at startup. Each heap owns a disjoint span in the shared logical
allocator. Bounded range allocation cannot cross its heap span; native offsets
subtract the aligned span base. A live resource's native address never moves.
Creation first tries existing heaps in the same lifetime group, then requests a
64 MiB chunk or a larger heap for an oversized resource. Groups separate asset
textures, Scene graph images, Scene graph buffers, and persistent resources. This
lets completed Scene replacement release empty heaps without long-lived assets
holding their backing capacity. Grouping does not relocate live resources. Budget or span fragmentation can reduce the
chunk to the requested size. Heap metadata is preallocated; native allocation
occurs only at resource creation/preparation boundaries.
If Metal rounds a provisional chunk's actual charge beyond the remaining budget,
the adapter releases that unpublished heap and retries once with the resource's
required size. The retry must also pass the actual-charge check before publication.

The memory adapter owns each heap and its residency membership. Retired resources
continue holding their heap until the shared core observes their last submission
complete. Packet collection first drains retired alias views using that same
observed completion serial. Collection releases the native resource, and removes/releases its heap
when the final allocation is gone. An unpublished native allocation failure uses
immediate retirement; creation requires enough retirement and free-range metadata
to guarantee that rollback. No online compaction or live-resource migration occurs.
Live Scene image-allocation recovery makes one retry at the unchanged extent
after GPU completion and reclamation of superseded targets, as specified in
[ADR-046](046-editor-viewport-mapping-and-picking.md). Successful ordinary
replacement retains its asynchronous retirement policy.

The default managed allocation ceiling is 4 GiB, configurable at startup through
`VKR_METAL_MEMORY_BUDGET_MB` (positive integer MiB). The cap includes committed
heap backing, the existing upload/readback rings, material and SH buffers, and
ICBs. It covers spare heap capacity and resources awaiting completion. Allocation
and reconciliation against native sizes happen before publication. Metal offers
no ICB size query: provisional ICB creation is followed by an actual-size budget
check before residency; rejection releases the object. Thus this cap bounds
accepted managed allocations, not every transient native allocation. ICB budget
rejections retain typed out-of-memory status through frame-upload preparation
so bounded Scene recovery can run; other native creation failures remain terminal.

Device-reported allocation and residency footprints remain separate observations.
Driver/validation/compiler storage, opaque command and counter heaps, MetalFX
internals, and external drawables are outside the managed cap. Texture views do
not get charged a second time. A 4 GiB managed cap does not guarantee a 4 GiB
process footprint or that the host has enough available memory.

Metal transfer buffers start at 32 MiB upload and 64 KiB readback per frame slot.
Demand grows their backing geometrically up to the existing 384 MiB upload and
48 MiB readback slot ceilings. Growth occurs at upload/frame preparation, after
submitted users complete and picking, diagnostic and capture consumers copy their
results. An active unsubmitted frame allocator is not reset. Acquired slices
prevent replacement. The adapter releases old backing before allocating the new
buffer, checks nominal and actual charges against the managed cap, and leaves an
empty, non-acquirable ring on native failure. Later admission can recreate it.
Capacity persists until teardown; first use of larger transfers can wait and
allocate. Transfer capacity and largest request are reported separately.

Metal's C entrypoints bound temporary Objective-C objects with lexical
autorelease pools around frame preparation/submission, resource publication,
startup, result collection and teardown. The macOS event pump's pool ends before
rendering and cannot own these temporaries. Persistent native resources retain
their explicit ownership and GPU completion rules. An upload batch retains its
encoder across publication calls and releases it after `endEncoding`; destruction
cancels an unsubmitted batch and its never-issued use serials before draining
actual submissions. Pool drainage does not establish GPU completion.
This corrects a source-confirmed leak path for drawables, encoders and timing
objects. A bounded normal Release two-load Bistro editor run completed
Stop/resize/Resume and unload/reload in 66.08 s, ending with zero pending or
terminal texture failures; its peak process footprint was 5,644,323,240 bytes.
The final six-frame Metal API transport check passed without native API errors
(report SHA-256 `40a04fe6d1615e69ad1dda39d1fcc99a5059492a60892b49c99a3a2e1d4e10b6`).
These local dirty-run observations do not prove the cause of earlier panics or
long-session stability.

Metal no longer maps permanent placement exhaustion to upload-busy retries.
Native allocation failure terminates the asset request and releases its prepared
payload. Required mesh failure terminates scene loading; optional streamed texture
failures keep the established fallback material and log the failure. The paneled
editor and standalone Metal app retain only memory-failed stream paths for bounded Scene-resolution
recovery in [ADR-046](046-editor-viewport-mapping-and-picking.md); failed upload
payloads are still released. Vulkan's
transient upload admission remains unchanged. CPU tests cover bounded placement,
transactional metadata failure, generation and completion-gated reclamation;
native allocation/lifecycle evidence is recorded separately from those tests.
The harness treats any terminal streamed-texture failure as incomplete scene
evidence and reports `scene.texture_load_failed` before checking pending streams.
Readiness also requires zero currently demanded missing texture assignments; a
historical applied count does not prove current residency. Shared-texture eviction
uses the newest demand epoch among all material users. Demanded evictions requeue
once after a real budget increase or committed Scene memory-relief generation,
without requiring a usage gap or retrying unchanged pressure each frame.

Asynchronous texture requests retain their published texture until the request
is released, including the interval waiting for GPU completion. Material
application acquires its own resident reference before releasing the request.
Request-reference cleanup is separate from final zero-reference destruction.
This prevents eviction from invalidating a shared handle between publication and
READY; READY-only material pinning did not cover that interval.

Metal reports the charged capacity of asset-texture heaps separately from live
texture bytes. The automatic policy subtracts that capacity from managed usage
to identify non-asset storage; publication, eviction and retirement inside the
same heaps cannot change that classification. Its allowance is the greater of
already charged asset capacity and the space left below the existing 80% target.
The 90% pressure-entry and 75% exit thresholds remain unchanged. Reusing capacity
already charged to the managed cap does not require eviction to reach that target.
Native placement still enforces the cap, alignment and fragmentation constraints.
Application publishes resource completions before sampling this allowance and
applying material textures. Active texture loads sample every frame; idle sampling
keeps its 60-frame cadence. Newly charged texture heaps must be included before
material application decides whether to evict against a finite allowance.

Capacity-based retries use a finite high-water allowance for the current set of
texture streams. Returning from a lower allowance or toggling the unlimited
policy sentinel cannot trigger another retry. A committed Scene reduction still
provides its own generation-bounded retry; removing the final stream resets the
high-water mark. Explicit user texture budgets and backends without dedicated
texture-capacity accounting retain their logical residency policy. Metal
allocation failures continue through the approved bounded Scene-relief path.


Metal ICBs are also demand-created. Each acquired command slot retains separately
sized main and transmission command buffers. Preflight rounds candidate capacity
up to a power of two within the existing 262,144 candidate limit, after proving
that slot's previous submissions complete. View command offsets use actual
candidate count. Graph draw tables follow the demand-capacity contract in
[ADR-002](002-render-graph.md), preserving the existing maximum bucket limits.
Growth removes and releases the completed old ICB before charging its
replacement. Failure leaves an empty slot for normal frame cancellation/retry;
capacity otherwise remains until teardown. First use and growth may allocate.

A four-shadow-view/transmission fixture passes Metal API validation, including
growth of populated slot buffers. Its matched Release captures are byte-identical
before and after ICB demand growth. Bistro passes complete texture readiness and
stop/resize/resume at 320×240 logical resolution with one shadow cascade under
the unchanged 4 GiB cap. Subsequent draw-table demand sizing and image-allocation
recovery support full 1528×1074 editor output with all textures resident in the
bounded two-load check recorded in
[ADR-046](046-editor-viewport-mapping-and-picking.md#verification-and-limits).
These are diagnostic checks, not panic-causality or long-session stability proof.
The command-capacity API report SHA-256 is
`819dfb4ee766c027d61ee4144e01bd3e2112aa607304276e0476b419db8022a5`; the
matching Release captures have SHA-256
`82510845bfe55d00ca57c4948579a0ebe367e8dd210f8f42fa48b9a2b49a7c28`.

Both backends allocate reusable geometry offsets through `VkrGeometryRanges`.
One shared memory core owns aligned vertex/decode spans and another owns index
spans; each published geometry holds a pair of generation handles. The owner
reserves both before native growth/upload and rolls back the first if the second
fails. Metadata accommodates one live and one retired generation per geometry
slot; further churn fails admission until completion frees capacity.

Destroying geometry retires its spans through the last recorded/submitted use.
Pending Vulkan initialization writes are discarded before retirement; submitted
uploads and Metal growth/upload work retain their submission proof. Collection
returns offsets only after observed GPU completion. Persistent default geometry
keeps its own live spans. Physical backing retains its high-water capacity until
renderer destruction; range reuse does not compact buffers or release native
heap charges. Existing growth preserves offsets, orders source upload writes
before preservation reads, and orders preservation writes before uploads into
reused holes. Metrics separate live, retired and reusable range bytes from
physical retained capacity.

Metal windowed queues register the `CAMetalLayer` residency set after assigning
the layer's device. The layer maintains drawable replacements in that set and
outlives the queue. Borrowed drawable textures are not covered by the engine's
placement-heap residency set. Drawable references are released after presentation.

Publication failure paths retire resources at their last submitted use and collect
only through the actual completion event value. A queued upload is not a
completion proof, including when subsequent sampler creation or bookkeeping fails.

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

Material slot allocation keeps a high-water cursor for never-reserved slots;
released slots return only through the free-ID stack. Unload never rewinds that
cursor, so subsequent loader and colored-material creation cannot overwrite a
live slot through two competing reuse paths. Failed name allocation returns its
reservation to the same stack.

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
overwrite in-flight storage. Vulkan pools and transfer rings retain capacity after logical release; an empty
Metal placement heap releases its backing after completed retirement.

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
