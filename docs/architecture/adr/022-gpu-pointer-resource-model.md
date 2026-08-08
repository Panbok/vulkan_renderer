---
status: partial
updated: 2026-08-08
authority: adr
---

# ADR-022: GPU-Address Resources, Native Texture References, and Backend-Lowered Dependencies

## Status

**Accepted (partial)** — the Metal resource model is implemented. Stage 2
memory/lifetime, Stage 3 immutable explicit-ID material, and Stage 4
backend-lowered dependency subsystems are implemented and validated. A focused
Stage 5 packet/graph renderer now composes them for Metal offscreen/windowed submissions with
generation-safe mesh, material, and cubemap assets; all authored pass categories;
a production-size irradiance/prefilter/BRDF convolution and readback proof;
decoded RGBA16F equirectangular import and focused PBR consumption; declared
capture batches; shared directional/IBL controls and scalar PBR material
lowering with distinct base/normal/ORM/emissive references and alpha cutoff;
opt-in per-pass Metal 4 timestamps; and cold/warm pipeline archives.
The production application and harness can select this renderer, and shared
loaders publish production assets through the coarse backend boundary. The
Metal-private capture ring owns asynchronous request polling/release and
completion-safe result lifetime. The allocator race is fixed. The formerly
accepted backend-pinned Metal Bistro-plus-text baseline is now historical after
a cross-backend audit exposed retained IBL, sampler, transparency, and
presentation defects; corrected pixel acceptance remains open. Version-10
packets and the
Metal frame upload now include bounded local-reflection-probe records with
native texture references, normalized box influence, and global IBL fallback. They also carry
prepared retained UI/world text geometry whose atlas handles are resolved to
native texture references once per frame. Focused Metal stage validators have
been retired: CPU tests own deterministic resource/lifetime and lowering
invariants, while backend-pinned harness snapshots own integrated production
renderer and pixel evidence.

## Context

[ADR-020](020-bindless-backend-seam.md) establishes a parallel implementation
boundary and [ADR-021](021-metal-first-bindless-backend.md) chooses Metal 4 as
the first native backend. The remaining decision is the semantic resource model
that materials, shaders, allocators, the render graph, and a later Vulkan backend
must share.

The current Vulkan 1.2 model remains implemented and in force:

- [ADR-007](007-gpu-memory-allocation.md) allocates device memory per resource;
- [ADR-005](005-reflection-driven-pipelines.md) reflects descriptor, uniform,
  push-constant, and vertex layouts;
- [ADR-009](009-frame-synchronization.md) uses binary WSI semaphores and fences;
- the graph stores per-subresource access and Vulkan layout state and emits
  resource-specific barriers.

The first draft of this ADR proposed one universal `{cpu, gpu}` allocation, a
contiguous descriptor heap with `texture_base + n` on every backend, and a
resource-list-free hazard bitfield that removed layouts from the shared graph.
That model does not match Metal:

- a placed texture has heap storage and a resource ID, but no raw buffer address
  that a shader dereferences;
- direct Metal texture IDs are opaque 64-bit `MTLResourceID` tokens, while a
  bounded `MTLTextureViewPool` exposes application-managed contiguous slots;
- `MTL4ArgumentTable` is bounded and is not a global descriptor heap;
- Metal barriers use stage scopes plus `MTL4VisibilityOptions`, not the
  article's portable hazard bits; and
- the shipping Vulkan graph still needs layout lowering, while the graph's
  resource list remains necessary for dependencies, lifetimes, aliasing,
  capture, and diagnostics even if Metal's encoded barrier omits it.

## Decision

On the bindless path, adopt four distinct typed layers.

### 1. Logical resource identity

CPU-visible resources and allocations use generation handles. Logical destroy
invalidates a handle immediately. A handle is never stored as a shader pointer
or native texture reference.

Destruction is transactional: if the selected backend cannot enqueue
retirement, the call fails while preserving the logical record and generation
for retry. Once accepted, logical invalidation is immediate even though native
storage remains completion-gated.

### 2. Memory placement and buffer addressability

A private allocation record owns heap/class, offset, requested and reserved
size, alignment, owner, optional CPU mapping, optional shader buffer address,
and last-use retirement point.

- `UPLOAD`: host-mapped/write-combined frame/root/frequently updated material/
  staging data.
- `DEVICE`: private textures and persistent GPU data that benefits from private
  or compressed storage.
- `READBACK`: host-cached capture/picking/counter data.

Buffer allocations may expose a cached `{cpu, gpu, size}` address pair. Texture
placement allocations do not pretend to have a shader-dereferenceable address;
the texture object and its native texture reference are separate.

The heap range allocator must align the selected offset, retain the exact
reserved range for free, expose alignment waste and fragmentation, reject stale
or overlapping frees, and report metadata-capacity exhaustion. `VkrFreeList`
may be reused only after these requirements are met; rounding request size alone
is not aligned allocation.

The Vulkan realization selects a compatible type from each resource's exact
`memoryTypeBits`. `UPLOAD` prefers host-visible coherent device-local memory,
then host-visible coherent memory, then non-coherent host-visible memory;
`DEVICE` prefers device-local memory that is not host visible, then any
compatible device-local type, and finally any compatible type with a named
degraded-placement report; `READBACK` prefers host-visible
cached coherent memory, then cached, coherent, and finally any compatible
host-visible type. Vulkan additionally has `bufferImageGranularity`, which Metal
does not; it is handled by **segregating buffers from optimal-tiling images and
keying blocks by memory type and addressability** —
`(class, kind, memory_type_index, device_address_required)` — rather than by
padding. `VkMemoryDedicatedRequirements` is queried before placement and a
required or initially preferred dedicated allocation bypasses the range core
while retaining the same owner and retirement accounting. Host-visible blocks
are mapped once persistently at creation. Non-coherent upload ranges are flushed
once per ring slice or publication batch before submit, and non-coherent
readback ranges are invalidated after completion and before CPU access.

GPU allocation remains outside the `VkrAllocator` vtable. It shares accounting
through `vkr_allocator_report()` and `VkrGpuAllocationOwner`, while device-heap
counts and logical-suballocation counts are published as distinct metrics.
The shared catalog now keeps native heap allocation rows separate from
placement rows for requested/reserved/live/retired/peak bytes, alignment waste,
free-range state, bounded failure classes, and buffer/texture ownership; driver
current allocation and recommended budget are explicitly labelled observations.

### 3. Backend-native bindless texture references

The CPU material/texture-handle record is the sole authoritative material
representation. Each backend lowers it once into a concrete GPU row:

- Metal stores either an explicit 64-bit `MTLResourceID` for every referenced
  texture slot (or an address to a row containing those IDs), or a compact index
  into a bounded `MTLTextureViewPool` after Stage 3 proves the shader arithmetic,
  ABI, growth, and retirement rules.
- Modern Vulkan stores compact 32-bit indices into application-managed
  descriptor-buffer heaps, per
  [ADR-023](023-vulkan-1-4-bindless-capability-profile.md). Per-binding base
  offsets and per-descriptor strides are read from the device at initialization
  and never written as literals, so **backend-defined descriptor sizes never
  reach the shader**: the shader indexes an unbounded array with an integer and
  the driver performs the layout-defined lookup. Two descriptor buffers are
  used, one for resources and one for samplers. Sampled images and samplers are
  kept as separate descriptors rather than combined ones, because
  `combinedImageSamplerDescriptorSingleArray` may be false and would then make
  index arithmetic driver-dependent. The resulting row is 64 bytes against
  Metal's 96 — the denser representation this ADR anticipated, and the reason
  its "standardize every backend on a 64-bit texture token" alternative was
  rejected.

  One Vulkan-only consequence: when `descriptorBufferImageLayoutIgnored` is
  false a descriptor is baked with an image layout, so a texture that is
  storage-written in one pass and sampled in another needs **two heap slots over
  the same image view**, one per layout. Every IBL bake target has this shape.
  Descriptor immutability is also stricter than under descriptor sets — a
  descriptor buffer is plain memory read at execution time, with no
  update-after-bind window and no driver-side copy, so an in-place overwrite
  while a submission can read it is an unrepairable race.

GPU-visible texture-reference/material rows are immutable while any pending
submission can read them. Updates allocate and publish a new row at a frame
boundary and retire the old row by retirement point. A barrier does not make an
in-place CPU overwrite safe for already-submitted work.

Descriptor slots belong to published texture/sampler resources, not to one
material row. Material publication retains those resources; row collection
releases them. A shared descriptor slot is reusable only after its last logical
reference is gone and its last GPU use is complete, so retiring one material
cannot invalidate another material that uses the same texture or sampler.

Static samplers are shader literals where supported. Dynamic sampler references
are backend-native values with the same publication/retirement rule.

### 4. Graph-declared dependencies with backend lowering

The graph continues to declare resource identity, subresource range, access,
and pass order. Its canonical use record also carries the shader/engine stage
scope needed to form a stage dependency. The current sampled/storage access
flags are not precise enough on their own and may use an explicit conservative
default until declarations are made exact.

The Stage 4 compiler produces logical dependencies; a backend lowerer emits:

- Metal stage/visibility barriers with no encoded resource list;
- existing Vulkan 1.2 access/layout/subresource barriers; or
- modern Vulkan synchronization2 global/resource/image barriers according to
  enabled features, specification requirements, and measured cost.

The implemented legacy declarations select compatibility defaults that lower
to the exact former Vulkan 1.2 stage/access masks. Explicit staged declarations
can narrow those scopes. Metal lowers the same canonical record to
`MTLStages`/`MTL4VisibilityOptions` and selects producer, consumer, or
intra-encoder barrier encoding at the recording site. Resource identity and
subresource ranges remain in the graph record even though Metal omits them from
the barrier command.

Applications do not author Vulkan layouts in the portable graph. The Vulkan
lowerer still owns layouts and external/present/initialization transitions when
required. `VK_KHR_unified_image_layouts`, when present, simplifies most internal
states but does not eliminate all exceptions or guarantee that a global barrier
is optimal on every device.

The modern Vulkan lowerer emits `VkImageMemoryBarrier2` per compiled image
barrier and `VkBufferMemoryBarrier2` per compiled buffer barrier, preserving
resource identity and range while batching both arrays into one barrier command
per pass boundary. A later measured change may replace a pass's buffer array
with one `VkMemoryBarrier2`; single-queue ownership makes that legal, but does
not prove its cost immaterial. Synchronization2 also lets the
canonical visibility flag be expressed exactly: device visibility becomes a
memory dependency with non-zero access masks, its absence an execution-only
dependency. Resource-alias visibility is **rejected with a named error** until
the graph actually aliases placement ranges, rather than silently ignored.

One asymmetry is worth recording as evidence against a premature low-level seam:
Metal models producer, consumer, and intra-encoder barrier forms, and the
intra-encoder form has no general Vulkan analogue.

One totally ordered completion domain is the only authority for command-
allocator reset, frame/upload/readback slot reuse, native object release,
texture-reference/material-row recycling, heap-range reuse, and residency
removal. The initial one-queue path represents it with a monotonically
increasing submit value — a Metal shared event, or a Vulkan timeline semaphore.
An independent transfer or compute queue must either join that domain or extend
the retirement point with every relevant queue; the maximum of unrelated queue
serials is not completion proof.

Vulkan has one exception the timeline cannot absorb: presentation completion.
Swapchain acquire and present require **binary** semaphores, while the submit
timeline does not prove presentation resources may be safely recycled or
destroyed.
Windowed bindless Vulkan therefore also requires
`VK_KHR_swapchain_maintenance1` present fences. A submit signals the timeline,
for ordinary retirement, and a per-image binary render-complete semaphore, for
present. Acquire semaphores are sized by frames in flight; render-complete
semaphores and present fences by the *actual* swapchain image count rather than
the requested minimum. Submit fences disappear, image-in-flight tracking becomes
a per-image last-submit value, and swapchain views/semaphores/handles retire only
after their present fences prove safe WSI resource release. Present fences are
not presentation-timing evidence and may signal before an image is displayed.

**Scope:** This ADR does not supersede ADR-005, ADR-007, ADR-008, or ADR-009 for
the Vulkan 1.2 renderer.

## Consequences

**Positive**

- No per-material texture descriptor set or texture-slot bind is needed in the
  bindless draw loop.
- Metal uses its real direct-ID or bounded texture-view-pool model instead of
  emulating an implicit process-wide heap.
- Vulkan can retain a denser index representation without forcing that ABI onto
  Metal.
- CPU material identity remains one authority and is lowered once per backend.
- Logical handle safety, memory placement, buffer addressability, and texture
  tokens have explicit, non-overlapping contracts.
- The graph preserves enough information for dependency analysis, lifetime/
  alias planning, capture, diagnostics, and both backend lowerers.
- Submit-value retirement applies one proof rule to every reusable GPU-visible
  object or range.

**Negative / risks**

- Metal and modern Vulkan GPU material rows are different ABIs and need separate
  shader-reflection/layout validation.
- Explicit 64-bit Metal texture IDs consume more material bandwidth/storage
  than a 32-bit pool index; the fallback cost and compact-pool behavior are both
  unmeasured.
- A placement heap adds alignment, fragmentation, growth, residency, metadata-
  capacity, and retirement failure modes absent from per-resource allocation.
- Stable GPU addresses constrain compaction; relocating live data requires an
  explicit rebuild protocol after completion.
- Canonical stage scopes enlarge the graph declaration/schema and can be overly
  conservative until pass accesses are precise.
- Resource-list-free Metal commands reduce direct API diagnostic context, so
  graph-level resource diagnostics and deterministic cases become more
  important.
- Two GPU memory/metrics policies coexist until Vulkan 1.2 retires.

## Alternatives Considered

- **Use raw GPU addresses as logical handles.** Rejected because addresses do
  not carry generations, do not identify non-addressable texture placement, and
  can be reused only under GPU completion rules invisible to ordinary handle
  validation.
- **Use one `{cpu, gpu}` allocation for buffers and textures.** Rejected because
  a Metal texture placed in a heap has no raw shader buffer address and may have
  no CPU mapping.
- **Require one unbounded 32-bit contiguous texture heap on every backend.**
  Rejected because Metal 4 texture-view pools are explicitly bounded and have
  backend-specific creation, growth, publication, and retirement rules.
- **Require a portable opaque descriptor blob in GPU-visible material rows.**
  Rejected because a C/shader ABI needs a concrete size, alignment, and access
  rule. Hiding backend-defined bytes in a public struct makes the contract less,
  not more, portable.
- **Standardize every backend on a 64-bit texture token.** Simpler ABI, but
  rejected before measurement because it would discard Vulkan's potentially
  denser natural representation merely to match Metal. Backend lowering already
  solves the difference outside the hot loop.
- **Overwrite material/texture rows in place and emit a barrier.** Rejected
  because a new barrier cannot repair a CPU race with an older pending
  submission. Allocate/publish/retire is the ownership-safe path.
- **Remove resource/subresource identity from the graph because Metal barriers
  do not carry it.** Rejected because the graph needs identity for scheduling,
  culling, aliasing, lifetime, capture, and diagnostics.
- **Remove layouts globally from `vkr_rg_compile_schedule()`.** Rejected while
  the Vulkan 1.2 lowerer ships. Layout-free authoring and Metal lowering do not
  erase Vulkan's backend responsibility.
- **Copy the article's hazard bitfield verbatim.** Rejected because Metal and
  Vulkan expose different visibility/access vocabularies and there is not yet a
  second concrete lowering from which to compress a portable enum.
- **Put GPU allocation in `VkrAllocator`.** Rejected because its `free` means
  immediately reusable, while GPU memory and tokens retire only after a submit
  value completes. Shared accounting is sufficient.
- **Retire multi-queue resources against the largest observed queue serial.**
  Rejected because an unrelated queue can complete a larger serial while a
  smaller-valued owner is still executing. Use a proven join or a per-queue
  retirement point.

## Revisit When

- A later Slang GPU record cannot express or validate the selected Metal texture
  reference representation against its host layout.
- Stage 3 cannot safely use compact texture-view-pool indices, or explicit
  64-bit fallback rows are measured as a material bandwidth/register bottleneck.
- The range allocator's alignment waste, fragmentation, or 1,024-node ancestry
  requires a different data structure or heap policy.
- Texture streaming or memory pressure requires eviction/relocation rather than
  stable allocation-lifetime addresses.
- ~~The modern Vulkan implementation establishes whether
  `VK_EXT_descriptor_heap` or `VK_EXT_descriptor_buffer` is the supported
  descriptor contract~~ — **resolved to `VK_EXT_descriptor_buffer` by
  [ADR-023](023-vulkan-1-4-bindless-capability-profile.md)**, since
  `VK_EXT_descriptor_heap` is absent from the installed SDK. The forward trigger
  is now the reverse: revisit when `VK_EXT_descriptor_heap` reaches SDK,
  drivers, validation, debugger, and shader tooling. Because indices stay
  indices, the heap module should contain most of the change, but the migration
  still reruns pipeline-layout, reflection, capture, and shader ABI gates. The
  second barrier lowering is still owed by the implementation.
- `VK_KHR_unified_image_layouts` target coverage and performance justify a
  simpler Vulkan layout state machine.
- A real workload requires stage-scoped split barriers beyond the minimum
  submission timeline and stage barriers.
- Independent compute or transfer queues are introduced; define and test their
  join or per-queue retirement representation first.
