---
status: implemented
updated: 2026-10-23
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
while retaining accounting. Host-visible and device-address buffers are
placed at 16-byte alignment or more, and creation refuses a device address
that is not, because shader loads through a device address declare their
type's alignment (VUID-RuntimeSpirv-PhysicalStorageBuffer64-06315) while
the driver can report 4 for a storage buffer. A `gpu_draw_instances` buffer
placed at a 4-byte boundary lost the device after 23,733 to 82,549 frames of
the 1440p Bistro orbit at `VKR_VULKAN_MEMORY_BUDGET_MB=6000` (6 of 8 runs,
2026-10-23). GPU-assisted validation named unaligned loads from it in
`VBuffer.Opaque`; a Radeon GPU Detective dump had placed the fault in
`Transmission.Compact.Fullscreen.0`, which also loads through device
addresses. With the alignment, 3 of 3 runs kept the device. Metal uses placement heaps and upload/readback
rings through the same range and submit contracts. There is no VMA, online
defragmentation or graph transient aliasing.

Vulkan's PUBLICATION class requires coherent memory for descriptor and material
rows that can be updated beside older GPU readers. It preserves native descriptor
strides and uses the capability failure policy in ADR-023. The white sentinel
stays in GENERAL for both sampled and storage descriptors; its initial copy
dependency includes fragment and compute consumers. White remains the unoccluded
AO/default value; this does not authorize passes to use it as a scratch output.

The Vulkan geometry owner retains a CPU row mirror with a publication generation.
Each completed frame slot reserves a fixed prefix in its upload buffer and copies
that mirror only when its generation differs. Publication, retirement and
megabuffer address changes update the mirror. Upload-buffer replacement and
non-scene prefix reuse invalidate the slot's generation. Old submissions retain
their independent table bytes and resource lifetime proofs. Unchanged frames
perform no table clear, capacity scan or row copy; a changed generation refreshes
the whole table rather than maintaining a second dirty journal.

The Vulkan FSR 3.1 bridge, a native exception to the descriptor-buffer path
with its own classic descriptors, was removed with FSR (ADR-052).

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

The default managed allocation ceiling is two thirds of the device's
`recommendedMaxWorkingSetSize`, or 5 GiB when the device reports none;
`VKR_METAL_MEMORY_BUDGET_MB` (positive integer MiB) overrides it at startup. The
remaining third covers allocations outside the cap and the rest of the system on
unified memory. A fixed 5 GiB default rejected a Bistro import whose 3.4 GiB of
referenced textures, inflated by paired normal/roughness variants
([ADR-012](012-texture-compression-pipeline.md)), plus render targets exceeded it;
a 16 GB M1 Pro reports 11.84 GiB, so its default is 7.9 GiB. The cap includes committed
heap backing, the existing upload/readback rings, material and SH buffers, and
ICBs. It covers spare heap capacity and resources awaiting completion. Allocation
and reconciliation against native sizes happen before publication. Metal offers
no ICB size query: provisional ICB creation is followed by an actual-size budget
check before residency; rejection releases the object. Thus this cap bounds
accepted managed allocations, not every transient native allocation. ICB budget
rejections retain typed out-of-memory status through frame-upload preparation
so bounded Scene recovery can run; other native creation failures remain terminal.

Device-reported allocation and residency footprints remain separate observations.
Driver/validation/compiler storage, opaque command and counter heaps, and
external drawables are outside the managed cap. Texture views do
not get charged a second time. The managed cap does not guarantee a matching
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
same heaps cannot change that classification. Under Metal's cap the allowance is
the greater of already charged asset capacity and the space left below the
existing 80% target.
The 90% pressure-entry and 75% exit thresholds remain unchanged. Reusing capacity
already charged to the managed cap does not require eviction to reach that target.
Native placement still enforces the cap, alignment and fragmentation constraints.
Streamed textures share those heaps with the other asset textures, a reload
places its new chain before it releases the old one, and a first load whose
placement the cap rejects retries every frame from its retained payload. So
under the cap the material system's budget is that allowance less the other
asset textures' live bytes (texture-owned live bytes less resident stream
bytes) and less a headroom, the larger of 1/16 of the charged capacity and the
largest asset texture the cap has rejected since one last placed
(`asset_texture_blocked_bytes`, reported as `pending_texture_upload_bytes`,
which also raises the projected usage into pressure)
(`vkr_render_assets_refresh_texture_residency_budget`). Lowering then frees
the room that loads and reloads need. Only the first rejection of a stall
logs. Without this, Bistro at `VKR_METAL_MEMORY_BUDGET_MB=2600` (2026-10-23,
M1 Pro, Release, `metal_bistro_texture_pressure`) stopped at
`scene.texture_ready_timeout` with 560 of 632 textures resident against a
1.797 GB allowance and logged 87,894 budget rejections; with it the run
passed with 632 resident, 144 lowering steps, scene textures ready in 6.1 s
and no rejection. Vulkan's allowance, which nothing caps, is unchanged.
Application publishes resource completions before sampling this allowance and
applying material textures. Active texture loads sample every frame; idle sampling
keeps its 60-frame cadence. Newly charged texture heaps must be included before
material application decides whether to evict against a finite allowance.

Vulkan runs the same policy from `VK_EXT_memory_budget`. Each submission samples
the usage and budget of every heap
(`vkr_vulkan_device_memory_budget`); device memory stats report them as heap
rows, and `texture_heap_mask` limits the policy to the device-local heaps, so
the host heap does not dilute pressure. Images owned by
`VKR_GPU_ALLOCATION_OWNER_TEXTURE` take blocks of their own pool
(`asset_textures` in `VkrVulkanMemoryPoolKey`). Release frees such a block when
it holds no live or retired placement, and the pool reports those blocks plus
dedicated asset images as the texture capacity. The separation is required:
other pool blocks are kept until shutdown, so evicting a texture placed in a
shared block would not lower the driver's usage, and a policy that counted
live texture bytes instead of capacity would then evict every texture. The
policy therefore runs only on stats with texture heap capacity. Without the
extension it does not run.

Nothing rejects a Vulkan allocation, so its texture capacity is not capped
(`texture_heap_capacity_capped` is false). With Metal's floor at charged
capacity, each new texture block would raise the allowance with it, and a
Bistro run at a 6000 MiB budget kept all 632 textures and 7.34 GB of
device-local usage. Vulkan's allowance is therefore the 80% target alone, and
the material system lowers and, at the floor, evicts textures to reach it.
Classifying non-asset storage by capacity keeps that target stable: a new or
freed texture block changes usage and capacity together. By the owner's
decision (2026-10-22) the backend adds no allocation cap.
`VKR_VULKAN_MEMORY_BUDGET_MB` lowers the reported device-local budget so a large
GPU can run the policy under pressure; nothing rejects an allocation against it,
and the driver can still oversubscribe video memory.

Under any texture budget the material system lowers mips before it evicts
textures. Each stream carries `extent_cap`, the budget's cap on its slot,
which lowers its requests' load limit (`max_extent`,
[ADR-012](012-texture-compression-pipeline.md)) below the material limit.
`vkr_material_system_evict_to_fit` projects the resident bytes once pending
cap changes have reloaded. While they exceed the budget it halves caps: the
oldest demand epoch first, and within an epoch every texture loses one mip,
largest first, before any loses two, down to
`VKR_MATERIAL_TEXTURE_RESIDENT_FLOOR` (128), so every loaded texture keeps its
mips at or below 128x128. Only when every texture is at that floor does it
evict whole textures, which then show neutral defaults. A cap reaches every
stream resident on a shared texture and the reloads that replace it, so the
slots keep sharing one texture. The limit's reload loads the smaller chain,
binds it and releases the larger one; until it binds the resident bytes exceed
the budget, and over the budget only reloads that shrink may start. A raise
scan, at most once per 16 epochs, doubles the caps of drawn textures, newest
demand and smallest first, while the projected bytes stay under 7/8 of the
budget, and clears a cap that reaches the source extent or the material limit.
Each reload allocates a new image. Both backends copy the levels the
replaced texture already holds on the GPU and read and upload only the
levels it adds ([ADR-012](012-texture-compression-pipeline.md)). Without a
measured need the order follows the material's draw epoch, not the mip a
view needs; the GPU demand below supplies that need. `asset.material.texture_stream.mip_lowered_total` and `mip_raised_total`
count the steps.

On the desktop pipeline the GPU measures what each material needs
([ADR-044](044-shader-cross-backend-contract.md), texture demand). Each frame
one pixel of every 4x4 block, at a rotating phase, computes the texture extent
at which one texel of its material's UV space covers it under anisotropic
filtering of up to 16 samples and the material samplers' -0.5 LOD bias
(`texture_demand_kernel.slangh`), and the G-buffer resolve maxes it, as log2
plus one, into a word per material slot. The upload pass clears the frame
slot's words, the deferred readback copies them out, and the completed slot's
decode turns them into one `VkrTextureDemand` per live material, which
`vkr_renderer_take_texture_demand` hands to the runtime when no frame renders.
The material system keeps the larger of two 16-epoch windows per material, so
the rotating sample covers every pixel. A texture needs the mips its source
holds at or below the largest extent its materials measured. Within a demand
epoch the budget first lowers mips beyond that need, the largest surplus
first, and only then every texture by size; a raise stops at the need, and
without pressure nothing changes (owner decisions, 2026-10-22). Every loaded
world source touches its material's epoch before the GPU culls it
(`vkr_scene_emit_world_sources`), so the epoch alone does not tell the
camera's materials from the rest. While demand runs without a gap of
`VKR_MATERIAL_TEXTURE_UNMEASURED_EPOCHS` (64), a texture none of whose
materials a frame measured in that time is out of the camera's view or drawn
only by shadows, reflections or probes, and a raise skips it until demand
measures it again or stops. One idle as well, unmeasured for
`VKR_MATERIAL_TEXTURE_IDLE_EPOCHS` (600, about 10 s) of running demand, goes
toward the floor, largest first, after the measured surplus and before any
measured texture loses a mip it needs (owner decision, 2026-10-25). Once loading ends, resident
bytes within the budget run no shrink pass, so each raise scan
(`vkr_material_system_make_room_for_need`) also lowers idle textures, at most
`VKR_MATERIAL_TEXTURE_ROOM_STEPS` (8) halvings a scan, until the bytes that
bring measured ones to their need fit under the raise ceiling, 7/8 of the
budget. A texture is idle when no frame measured its materials for
`VKR_MATERIAL_TEXTURE_IDLE_EPOCHS` (600, about 10 s) of running demand, so a
texture the camera has not reached yet waits as long as one it left: a first
version that took the room from any texture unmeasured for 64 epochs
lowered and raised textures each time Bistro's 5-second orbit turned away and
back, about 300 reloads in 25 s, and took the orbit's wall frame at p95 from
14.6 to 17.2 ms on the RX 6700 XT and from 32.7 to 55.4 ms on the M1 Pro,
where creating and staging each reloaded texture in the submit costs about
10 ms of CPU (reverted in 69de844e). Lowering by size order until a texture
is idle, and raising only once the resident bytes, not the projected ones,
are under the ceiling, keep a texture the camera turns back to as it was.
Before the rule, Bistro's 1200 MiB pressure case on the M1 Pro held 278
unmeasured textures at full mips (744 MiB of 1196 MiB) while 26 measured
ones stayed below their need. On the RX 6700 XT (2026-10-25, Release, at
`VKR_TEXTURE_STREAM_BUDGET_MB=1200`):

- The pressure case with a 900-frame warm-up (a temporary case, past the idle
  window): slots below their need 31 to 0, measured textures 317 to 433 MiB,
  unmeasured full chains 706 to 401 MiB, resident bytes 1198 to 1043 MiB,
  nothing evicted, failed or stalled.
- `bistro_vulkan_high_orbit_1440` with `performance-windowed-gpu.json`, five
  runs before against two after: wall frame p50 13.22 to 13.27 ms against
  14.29 and 14.32 ms, p95 14.60 to 14.71 ms against 17.98 and 17.45 ms, at
  most 16.8 to 21.7 ms against 26.6 and 23.6 ms; slots below their need at
  most 102 to 107 against 52 and 40. Each harness child renders about 1,900
  frames of its 38-second load at the orbit's first pose, so the textures out
  of that view are idle and lowered by the time the orbit starts, and each is
  raised once when the camera first reaches it: about 150 lowerings, 140
  raises and 250 level copies in the 1,500 measured frames. Budgeted orbit
  runs are not authoritative: which mips are resident at a frame depends on
  load timing, so the work volume differs between repetitions.
- At the driver's budget nothing lowers: the same orbit took 15.07 and
  15.91 ms at p95 against 15.77 and 15.39 ms before. At
  `VKR_VULKAN_MEMORY_BUDGET_MB=6000` the texture allowance is 0 and the
  scene never reaches readiness, before this rule as after.
The same need orders reloads, local and remote (ADR-012), and
`asset.material.texture_demand.below_need` counts resident texture slots
whose texture holds fewer mips than their material's measured need within
the material limit; Bistro's pressure case at
`VKR_VULKAN_MEMORY_BUDGET_MB=6000` ended with 44 and 38 of them
(2026-10-23), the driver's budget on the 1440p orbit with none.
`asset.material.texture_demand.materials` counts the materials the last
folded frame measured.

The tiled pipeline measures the same value only while the texture budget
binds (`VkrWorldPassPayload.texture_demand_requested`), in one frame of every
`VKR_TILED_TEXTURE_DEMAND_INTERVAL` (8) outside the overdraw view
(`VkrPreparedFrame.tiled_texture_demand`), and that frame measures every
pixel. Its opaque passes take shading variants with
`VKR_METAL_TILED_DEMAND_VARIANT`, whose forward entries pass the kernel the
UV differences across the pixel quad and the frame's texture LOD bias, and
write the material row and encoded demand into a memoryless R32Uint colour
target. A tile kernel after each opaque pass's draws
(`vkr_metal_tiled_demand_tile`) maxes every pixel's word into the frame's
readback slice, one word per material table row, which the CPU zeroes when
it takes the slice; the completed slot's decode returns them as on the
desktop pipeline. The variants, with the sky, resolve and tile pipelines of
those passes, build in the background on the first request in each sun
variant; until they are READY, and for Custom graphs and Standard kinds
whose demand variant is not, frames go without demand or draw their
fallback. The first lowering under new pressure therefore takes the epoch
and size order.

The tiled class cannot afford the desktop design (owner decision
2026-10-23, measured on the M1 Pro in Release with Bistro). A device atomic
in the forward fragment made `Tiled.Opaque` take 245 to 251 ms instead of
6.8 ms at 800x600, even where the atomic never ran; a plain store took
20.6 ms. Every frame writing the demand target and running the tile kernel
added 1.5 ms on `tiled_bistro_native` (2560x1440, 4x MSAA; 15.93 to
17.42 ms at the median, `performance-windowed-gpu`, authoritative,
alternating runs) and 1.0 ms at the production default
(`tiled_bistro_plaza_windowed_sun_contact`, MetalFX at 0.6; 8.34 to
9.35 ms in two local single-child pairs). The target itself cost about
0.95 ms whether R32Uint or R8Uint, and `dfdx`/`dfdy` about 0.3 ms more than
the quad differences, because the material samples then reused them. With demand frames only under pressure,
`tiled_bistro_native` without pressure matched its base (15.96 and
15.98 ms against 16.02 ms, the same workload fingerprint). Under
`VKR_TEXTURE_STREAM_BUDGET_MB=1200` after a 2400-frame warmup at the
production default, `Tiled.Opaque` went from 8.35 to 8.47 ms on average and
8.68 to 9.48 ms at p95, and `frame.wall` p95 from 17.29 to 18.05 ms
(`local-windowed-gpu-single`, one child each). The pressure case measured
51 materials, against 48 to 50 on the desktop pipeline, and one Metal API
validation run of it reported no error.

Evidence for measured demand (2026-10-22, RX 6700 XT, Release): the texture
pressure case measured 48 to 50 materials a frame. At
`VKR_VULKAN_MEMORY_BUDGET_MB=6000` it passed with all 632 textures resident,
none evicted, 1.103 GB against a 1.106 GB allowance after 635 lowering steps;
with the driver's budget it lowered nothing. On
`bistro_vulkan_high_orbit_1440` with `performance-windowed-gpu.json` and the
desktop pass groups, the `GBuffer.Resolve.Fullscreen` pass went from 1.287 to
1.307 ms at the median and 1.406 to 1.416 ms at p95, and the Surface line's
p95 stayed at 2.98 ms of its 3.0 ms target. The two runs came from one
session in the shared checkout with a browser open, so they bound the cost
but are not authoritative budget evidence. Image quality under pressure (2026-10-23, RX 6700 XT, Release, the texture
pressure camera captured on its 60th measured frame with `vkr_harness
snapshot`): at `VKR_VULKAN_MEMORY_BUDGET_MB=6000`, with 1.11 of Bistro's
3.16 GB resident after 612 lowering steps and 40 texture slots below their
measured need in every measured frame, G-buffer albedo differed from full
residency at 57.8 dB PSNR, 0.09% of pixels by more than 4 of 255 and none by
more than 16; specular 92.7 dB, normals 66.4 dB, final colour 53.2 dB. The
differences sit in fine facade and distant cobble detail.

An evicted texture is destroyed at once, and the texture system can reuse its
id while material rows that sampled it still await retirement. Vulkan keeps
that texture's publication record in place until those rows retire, because
retired materials release texture references by record index. Publishing a
new generation into such a record returns `VKR_RENDERER_ERROR_RESOURCE_BUSY`,
and the texture load retries on a later frame.

A texture publication can name a copy source
(`VkrTexturePreparedLoad.copy_source`, only where the device reports
`supports_texture_level_copy`; both backends do, and the
`VKR_TEXTURE_LEVEL_COPY_DISABLED` diagnostic turns it off for comparisons).
Vulkan
accepts a live, initialized source of the same format whose smallest levels
match the new image's at the same extents, and returns
`VKR_RENDERER_ERROR_RESOURCE_BUSY` while the source still initializes. The
pending initialization holds a `copy_reference_count` on the source record,
which retirement waits for, so an unpublished source keeps its image. The
frame that records the initialization moves the source from
`SHADER_READ_ONLY_OPTIMAL` to `TRANSFER_SRC_OPTIMAL` after every earlier use
on the queue, copies the levels with `vkCmdCopyImage2`, and returns it before
the frame's passes. The commit after that submission drops the reference and
raises the source's last use to the submission; uploads of the larger levels
then follow through the bounded staging chunks. Texture images carry
`TRANSFER_SRC` usage where their format allows it.

Metal accepts a live one-layer 2D source in the same pixel format whose
copied levels sit at the new texture's extents and whose own upload has
completed on the GPU; one whose upload is still in flight returns
`VKR_RENDERER_ERROR_RESOURCE_BUSY`, so the load retries while the old
texture keeps drawing (owner decision, 2026-10-23;
`vkr_metal_packet_texture_copy_source`). The new texture's upload command
buffer copies the levels with its compute encoder's `copyFromTexture` before
the buffer copies of the larger levels, which fill other levels, so neither
waits for the other; a shrink submits the copy alone, and a batched upload
copies in the batch's encoder. The source's last use rises to that
submission, which its retirement waits for. Evidence (2026-10-23, M1 Pro,
Release, `metal_bistro_texture_pressure` at
`VKR_TEXTURE_STREAM_BUDGET_MB=1200`): 418 to 430 copying publications of
about 1,120 applied per run, all 632 textures resident, none evicted; a
base-colour capture with copies matched one without bit for bit; one Metal
API validation run reported no error.

Evidence (2026-10-22, RX 6700 XT 12 GB, Windows 10, Release,
`vkr_harness profile --case tools/cases/local/vulkan_bistro_texture_pressure.case.json
--profile tools/profiles/local-offscreen-gpu-single.json`): with the driver's
budget the run passed with 632 of 632 textures resident, pressure off,
`memory.gpu.heap_usage_valid` 1, 3.36 GB of texture capacity and 7.09 GB of
11.78 GB used on the main device-local heap, and no mip step. With
`VKR_VULKAN_MEMORY_BUDGET_MB=6000` and whole-texture eviction only, residency
settled at 1.105 GB against a 1.106 GB allowance with 218 of 632 textures, and
the run stopped at `scene.texture_ready_timeout` because demanded textures
stayed missing. With mip-level eviction the same budget passed: all 632
textures resident, none evicted, 1.125 GB against a 1.125 GB allowance after
300 lowering steps, and 5.07 GB used on the main device-local heap of a
6.04 GB budget (7.09 GB before). The same run with `VK_LAYER_KHRONOS_validation`
and synchronization validation passed with 310 steps and reported no message.
Texture block capacity was 1.41 to 1.74 GB for 1.1 GB of textures: blocks that
held larger chains stay while any placement in them lives. Frame time and
image quality under pressure are measured above.

Capacity-based retries use a finite high-water allowance for the current set of
texture streams. Returning from a lower allowance or toggling the unlimited
policy sentinel cannot trigger another retry. A committed Scene reduction still
provides its own generation-bounded retry; removing the final stream resets the
high-water mark. Explicit user texture budgets retain their logical residency
policy. Metal
allocation failures continue through the approved bounded Scene-relief path.


Metal ICBs are also demand-created. Each acquired command slot retains separately
sized main and transmission command buffers. Preflight rounds candidate capacity
up to a power of two within the existing 262,144 candidate limit, after proving
that slot's previous submissions complete. Since 2026-10-18 an ICB group's
views share one command pool: the prefix kernel, one thread per group, places
each view's ranges after the previous view's, the camera first, and a
retained cascade whose pass does not run takes none. The pool holds what the
frames' views needed, read back with the compaction state at completion: once
a frame needs two thirds of it, it grows to twice that need in steps of 32,768
commands, never below four views of every candidate and never above every
candidate in every view, which cannot overflow. A view the pool cannot hold
whole reports the rest as overflow, and its shadow layer (the local atlas
layer, or the cascade's static and per-slot layers) loses its retained
content so it draws again; a cut camera view loses those draws for its frame.
The encode pass resets the pool once instead of each view's range. Before,
each view took every candidate's command: on the M1 Pro an ICB command takes
about 700 bytes whatever its bindings, so the Level Design Test's 31 views
held 682 MiB per slot, and each new high in the frame's view count released
and reallocated it on the render thread, 11 to 214 ms with its residency
commit, the 90 to 250 ms play stalls of rec-20261018-120657. Graph draw tables follow the demand-capacity contract in
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

The shared pool (2026-10-18, Release, M1 Pro, headless editor, `rec.drive`
of rec-20261018-120657 in the Level Design Test, local and non-authoritative):
each slot's ICB grew to 88 MiB during load instead of 682 MiB, the largest
growth taking 16.5 ms; the drive had no frame over 50 ms against 100 to
250 ms stalls before, GPU memory stayed at 4,341 MiB instead of rising from
4,951 to 5,301 MiB, and `Cull.Encode` fell from 1.14 to 0.45 ms while
`Cull.Prefix` rose from 0.135 to 0.155 ms. Fixed Edit-mode captures of four
views, lit and shadow factor, differ from the per-view build within the
run-to-run noise of one build (38 to 51 dB PSNR against 40 to 50 dB), and
picking selects the same entities. A build whose floor held one view cut
draws in the first frames, invalidated their local layers and grew the pool
to 98,304 commands; its settled captures match within the same noise. Metal
API validation was not run; the desktop pipeline (Vulkan) is unchanged.
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
Metal uploads rotate through their own command slots and a separate publication
transfer ring, allocated at the first upload, one slot per ring slot: an upload
waits only for the upload one ring's length earlier, never for a frame. Before
2026-10-04 uploads shared the two frame slots, so each streamed tile or texture
batch waited for the previous frame's GPU work; gliding over a streamed Bistro
terrain spent about a quarter of the render thread there. A publication run's
geometry buffers share one upload batch. A finished batch holding textures
still waits on the CPU until they are on the GPU: replacing that wait with a
queue event wait let a new terrain draw its layer weights as colour for
seconds before its textures showed. At least two frame command slots are
required.

Metrics distinguish logical requested/reserved bytes from native allocation,
retired storage, capacity failures and owner classes. Vulkan driver host memory
uses null allocation callbacks and is outside VKR CPU allocator accounting.

## Consequences

The shared contract avoids two retirement implementations without hiding native
resource behavior. Ring pressure fails or waits at owning boundaries; it cannot
overwrite in-flight storage. Vulkan pools and transfer rings retain capacity after logical release; an empty
Metal placement heap releases its backing after completed retirement.

In 90-second Release editor runs of that Bistro import on the 16 GB M1 Pro, the
fixed cap logged `managed=5363142704 cap=5368709120` exhaustion. The derived cap
rendered it without budget errors and exited 0, while free system memory fell
from 68% to about 20%. The cap bounds accepted allocations, not host pressure.

## Alternatives considered

Per-resource Vulkan device allocation was retired. A generic resource/command
RHI would add a second owner without unifying native policy. A universal memory
pool would erase memory-type and addressability constraints.

## Revisit when

Fragmentation, heap pressure or transfer cost demonstrates the need for a new
placement or upload policy with explicit last-use ownership.

## Implementation

[`vkr_gpu_memory.c`](../../renderer/src/vkr_gpu_memory.c),
[`vkr_gpu_slot_table.c`](../../renderer/src/vkr_gpu_slot_table.c),
[`vkr_asset_publisher.h`](../../renderer/src/vkr_asset_publisher.h),
[`vkr_render_assets.c`](../../runtime/src/renderer/systems/vkr_render_assets.c),
[`vkr_vulkan_memory.c`](../../renderer/src/vulkan/vkr_vulkan_memory.c), and
[`vkr_metal_memory.c`](../../renderer/src/metal/vkr_metal_memory.c).
This record incorporates the surviving lifetime rules from former ADR-007/008.
