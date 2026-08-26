---
status: partial
updated: 2026-08-26
authority: design
---

# Bindless GPU-Address Renderer — Design Specification

**Document status:** Accepted in part. Stages 0–5 are implemented and validated
on Metal 4. The application and harness select the production Metal packet
renderer; shared loaders publish generation-safe assets; and the authored graph,
PBR/lighting/text paths, capture, metrics, retirement, and pipeline archives are
exercised by focused and Bistro cases. The bootstrap allocator race is fixed,
and Gate A once produced a fourteen-view Metal Bistro-plus-text baseline.
A later cross-backend audit found that generation preserved broken retained IBL,
sampler, transparency, and presentation behavior. Those defects are corrected,
the old Metal generation is historical, and the owner accepted corrected
replacement output on 2026-08-11. Stage 6 modern Vulkan is complete
through V7 on the RX 6700 XT implementation line, including keyed dynamic-memory pools,
authored-graph parity, owner-accepted corrected Bistro/text output, and the
Windows default selection. Local evidence artifacts, including the standalone
V0/V3 executables and fixture-only walking/diagnostic plumbing, were removed on
request after their observations were recorded. V7 removed the Vulkan 1.2 path
and its migration surface. The detailed
[bindless Vulkan backend specification](bindless-vulkan-backend-spec.md) and
ADRs 023–026 own its current status. The descriptor-buffer backend still cannot
run on the macOS development machine. Its post-extraction CPU, byte-identical
snapshot, and focused Metal-validation witnesses pass there, while the paired
Release profile still misses its predeclared submit bound. A fresh post-V7
native Windows rerun passes Debug/Release whole-graph execution plus focused
synchronization and GPU-assisted validation on the RX 6700 XT; it is correctness
evidence, not a performance result. Linux remains an unimplemented,
evidence-gated target, so this umbrella specification remains partial. Current local tooling
is Vulkan SDK 1.4.357.0, MoltenVK 1.4.1, and Slang
`2026.13.1-1-g84792eb15`; dated earlier observations remain historical
evidence. The completed evidence proves only the focused paths described in
§12 and the detailed Vulkan specification §12.

**Scope:** A native Metal 4 renderer for Apple Silicon, followed by a
capability-gated modern Vulkan renderer for Windows and Linux. Both use 64-bit
buffer addresses and bindless texture references, while lowering those concepts
to their APIs' different resource models.

**Non-goals:** Claiming that Metal
implements every primitive in Sebastian Aaltonen's proposal, choosing a heap
block size before measurement, or promising speed without a Release profile.

**Companions:** [ADR-020](adr/020-bindless-backend-seam.md),
[ADR-021](adr/021-metal-first-bindless-backend.md),
[ADR-022](adr/022-gpu-pointer-resource-model.md), and the
[renderer status specification](renderer-architecture-spec.md).

Stage 6 is designed in detail by the
[bindless Vulkan backend specification](bindless-vulkan-backend-spec.md), with
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md) owning the
capability contract, [ADR-024](adr/024-shared-bindless-gpu-cores.md) the shared
backend-neutral cores, [ADR-025](adr/025-selected-renderer-implementation-strategy.md)
the implementation seam, and [ADR-026](adr/026-vulkan-1-2-retirement.md) the
retirement sequence. ADRs 023–026 are accepted and implemented.

The status authority remains the renderer status specification. It records the
implemented Metal path separately from the Vulkan Windows path.

---

## 1. Conclusion

The direction is viable, but the original one-to-one mapping from the article
to Metal was too strong.

Metal 4 supplies the most important primitives needed for a pointer-oriented
renderer: shader-visible buffer addresses, argument-table buffer bindings,
per-resource GPU IDs, transient command allocators, queue residency sets, and
stage-scoped barriers without resource lists. It is therefore a good first
native backend for this project.

Metal 4 does **not** supply the article's implicit, process-wide global texture
heap. Direct texture IDs are opaque 64-bit `MTLResourceID` values. Metal 4 does,
however, expose bounded application-managed `MTLTextureViewPool` slots whose
resource IDs are contiguous from `baseResourceID`. This is a viable candidate
for compact material indices, but it is not an unbounded automatic descriptor
heap. `MTL4ArgumentTable` is a separate bounded root binding table—31 buffer
slots, 128 texture slots, and 16 sampler slots in the installed SDK.

The portable model must therefore be:

- buffers and structured data use 64-bit shader addresses;
- textures use a backend-native bindless reference;
- one canonical CPU resource/material record is lowered once into a
  backend-specific GPU record;
- logical handles, memory placement, shader addresses, and texture references
  are different types with different lifetimes;
- the graph remains the resource/subresource authority, while each backend
  lowers graph dependencies into its native barrier vocabulary;
- no backend selection, allocation, lookup, string construction, or lock is
  introduced into a per-draw/per-dispatch loop.

This preserves the article's useful objective—no per-material texture binding
or descriptor-set management in the draw loop—without inventing a Metal feature
that does not exist.

---

## 2. Evidence Boundary

The primary inspiration is Sebastian Aaltonen's
[No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api).
The article proposes an API for modern hardware; it is not a specification of
Metal, Vulkan, or Slang.

The following distinctions are mandatory when using it as design input:

| Article primitive | Adopted VKR requirement | Backend-specific reality |
|---|---|---|
| `gpuMalloc` returns a pointer | Buffer allocations expose a cached CPU mapping when available and a stable shader address | Texture placement is an allocation handle/heap offset, not a shader-dereferenceable pointer |
| One root pointer per stage | Root data is a small immutable GPU record reached through one address per active shader stage | Metal binds the address through an `MTL4ArgumentTable`; Vulkan may use a device address/push-address mechanism |
| Global 32-bit texture heap | No per-material texture bind in the draw loop | Metal may use explicit 64-bit IDs or indices into a bounded `MTLTextureViewPool`; modern Vulkan may use compact heap indices |
| Static constants may contain pointers | Static constants remain a pipeline-time optimization | Pointer-valued Metal function constants are **unverified** and are not required by the minimum design |
| Resource-list-free stage barriers | The graph emits logical dependencies with explicit stage and visibility intent | Metal encodes list-free barriers; Vulkan may lower to global or resource barriers depending on capabilities and measured cost |
| Raw-memory split barriers | Timeline completion is the sole reuse/retirement authority | Metal queue events cover submission timelines, but do not prove the article's stage-scoped raw-memory signal/wait primitive |
| Transient command buffers | Per-frame command storage resets only after completion | `MTL4CommandAllocator` maps directly; Vulkan uses resettable per-frame command pools |

The article's hardware claims and measurements remain the author's evidence.
Nothing in this document turns them into a VKR performance result.

---

## 3. Current VKR Boundary

The current renderer is deliberately not bindless:

- `VkrRendererBackendInterface` has 86 function-pointer entries. It exposes
  render-pass, render-target, descriptor-instance-state, vertex-binding, and
  descriptor-write telemetry concepts shaped by the Vulkan 1.2 implementation.
- Direct backend calls are concentrated: 141 in `renderer_frontend.c` and seven
  in `vkr_capture.c`. This bounds mechanical backend coupling, not semantic
  coupling.
- The systems layer currently has 214 textual `vkr_renderer_*` call sites, 100
  of which are error-string queries. A low call count is not proof that a
  subsystem's GPU data model is portable.
- The graph tracks per-(mip, layer) access, execution stage, and layout in
  `VkrRgSubresourceState`. `VkrImageAccessFlags` and `VkrGpuDependency`
  deliberately survive into backend lowering. Existing sampled/storage
  declarations retain the conservative all-graphics-plus-compute default;
  staged declarations can name a precise scope.
- `vkr_rg_compile_schedule()` and `tests/src/render_graph_barrier_test.c`
  provide a CPU seam for dependency planning. They cannot prove an encoded
  Metal or Vulkan barrier is correct.
- GPU allocation remains one `VkDeviceMemory` per buffer/image/readback
  resource. `VkrFreeList` is a reusable offset-range primitive, but it has no
  aligned allocation and has a maximum of 1,024 bookkeeping nodes.
- The macOS platform already exposes `vkr_window_get_metal_layer()`, and the
  Apple build already enables Objective-C. A native Metal implementation does
  not need a second window abstraction to acquire a drawable.

The important coupling is semantic. Materials currently imply descriptor
state, pipelines imply reflected descriptor/vertex layouts, and the graph
compiler derives Vulkan layouts. Those are the parts that need an explicit
backend lowering; they are not made portable by adding another vtable.

---

## 4. Metal 4 Capability Map

Observed in the installed macOS 26.5 SDK:

| Requirement | Metal mechanism | Evidence boundary |
|---|---|---|
| Runtime capability gate | `-[MTLDevice supportsFamily:MTLGPUFamilyMetal4]` | Must be queried; OS version and Apple Silicon branding are not sufficient checks |
| Buffer GPU address | `MTLBuffer.gpuAddress` / `MTLGPUAddress` | Shader-visible address for a buffer allocation |
| Root address binding | `-[MTL4ArgumentTable setAddress:atIndex:]` plus `setArgumentTable:atStages:` | Metal snapshots table resources when a draw/dispatch is encoded; Stage 0 verified the current Slang `ParameterBlock` slot lowering |
| Bindless texture reference | `MTLTexture.gpuResourceID` / `MTLResourceID`; `MTLTextureViewPool.baseResourceID` plus indexed slots | Direct IDs are opaque; a bounded texture-view pool provides an application-managed contiguous ID range. Stage 0 validated direct and pool-backed IDs in GPU root records, but not shader-side base-plus-index reconstruction |
| Argument table | `MTL4ArgumentTable` | Bounded table, not the proposed global texture heap |
| Stage barrier | `barrierAfterQueueStages:beforeStages:visibilityOptions:`, `barrierAfterStages:beforeQueueStages:visibilityOptions:`, and the encoder-scoped form | List-free, but visibility is Metal's `None`, `Device`, or `ResourceAlias`, not the article's hazard enum |
| Command storage | `MTL4CommandAllocator` | Reset only after all command buffers allocated from it complete |
| Submission timeline | `-[MTL4CommandQueue signalEvent:value:]` / `waitForEvent:value:` | Queue/submission ordering and frame pacing; not a stage-scoped in-command-buffer split barrier |
| Residency | `MTLResidencySet` attached to the command queue | Adds/removes are batched by `commit`; placement heaps can normally be made resident as long-lived allocations |
| Placement heaps | `MTLHeapTypePlacement`, size/alignment queries, resource creation at an offset | Supports explicit texture/buffer placement; the resulting texture still has no raw shader pointer |
| Pipeline compilation/cache | `MTL4Compiler`, pipeline objects, `MTL4Archive` | Real pipeline/archive behavior remains an implementation spike |
| Depth-stencil state | `MTLDepthStencilState` | Separate from the render pipeline |

Three prior claims are intentionally not carried forward:

1. `MTL4ArgumentTable` is not a global descriptor heap.
2. `MTLFunctionConstantValues` existing does not prove pointer-valued function
   constants work in the intended Slang/MSL ABI.
3. Queue event signal/wait is not the article's raw-memory, stage-scoped split
   barrier.

The minimum renderer does not need claims 2 or 3. Stage 0 proved claim 1's
replacement: one root buffer address plus direct and pool-backed
`MTLResourceID` values stored in GPU-visible root data render and survive Metal
API and shader validation.

---

## 5. Implementation Boundary

### 5.1 Do not freeze a speculative second low-level vtable

The original proposal specified approximately 25 operations and treated 30 as a
failure threshold. Those numbers were not derived from working call sites and
are not useful invariants. A smaller table can still be the wrong abstraction,
and a correct deep module can legitimately need more operations.

The decision is instead:

- Keep `VkrRendererBackendInterface` unchanged for the shipping Vulkan 1.2
  renderer.
- Select the renderer implementation once during initialization. No per-draw
  backend-type branch is allowed.
- Build the first Metal walking slice with backend-private typed modules for
  device/queue, memory, resources, pipelines, recording, submission, capture,
  and metrics.
- Keep shared only what is already demonstrated to be backend-neutral: public
  generation handles, packet validation and payload semantics, JSON graph
  authoring, dependency topology, scene/ECS/asset CPU data, and harness case
  semantics.
- Lower GPU-facing material, shader, render-target, barrier, and capture state
  inside the selected implementation.
- Extract a cross-backend low-level `VkrGpuInterface` only when Metal and modern
  Vulkan provide two concrete implementations of the same operation and
  lifetime contract. Until then, `vkr_gpu.h` may own shared typed records proven
  by multiple real callers, but not a speculative function-pointer table.

This is semantic compression: the eventual seam is derived from repeated use,
not from deleting names from the Vulkan vtable.

### 5.2 Where dispatch lives

At the Stage 6 design point, `renderer_frontend.c` owned both public
orchestration and the legacy backend wrappers. The planned Vulkan
implementation was forbidden from adding a backend test to each of those 141
backend call sites.

Initialization installs a coarse renderer-implementation strategy. Resource
creation, frame preparation, packet submission, capture, metrics, and shutdown
enter that selected strategy. Within Metal packet submission, draw recording is
direct to Metal-private modules or through pre-resolved operations; it never
checks `backend_type` per draw.

If sharing `renderer_frontend.c` produces branch duplication, split it into:

- shared public lifecycle/packet validation;
- the existing Vulkan 1.2 orchestration;
- bindless orchestration shared only after Metal and modern Vulkan prove it.

Duplicating a small amount of orchestration is preferable to making every hot
operation conditional or forcing Metal to reconstruct descriptor-set/render-
pass semantics.

---

## 6. Resource Identity, Placement, and Lifetime

Four concepts that were previously conflated must remain distinct.

| Concept | CPU meaning | GPU meaning | Reuse rule |
|---|---|---|---|
| Logical resource handle | `{id, generation}` validated by a registry | Never dereferenced by a shader | Generation changes immediately on logical destruction |
| Memory allocation/placement | Generation-safe allocation handle plus heap, offset, reserved size, requested size, alignment, class, owner | Backing storage; may or may not be addressable | Heap range returns only after last-use completion |
| Buffer address pair | Optional CPU mapping plus `uint64_t` GPU address and size | Pointer to bytes/structs | Address remains stable for the allocation lifetime; no compaction while live |
| Texture reference | Backend-private token produced from a live texture view | Metal `MTLResourceID` or Vulkan heap reference | Token and referenced texture remain alive through every submission that can read it |

An illustrative shared record is therefore not a universal `{cpu, gpu}`
allocation:

```c
typedef struct VkrGpuAllocationHandle {
  uint32_t id;
  uint32_t generation;
} VkrGpuAllocationHandle;

typedef struct VkrGpuAddressPair {
  void *cpu;      /* NULL when not host visible. */
  uint64_t gpu;   /* 0 when the allocation has no shader buffer address. */
  uint64_t size;
} VkrGpuAddressPair;
```

The real allocation record is private and also retains heap/offset/reserved
range, memory class, owner, and last-use retirement point. A texture placement
can have a valid allocation handle and no `VkrGpuAddressPair`.

### 6.1 Memory classes

| Class | Intended use | Metal starting point | Vulkan memory-property search |
|---|---|---|---|
| `UPLOAD` | Root data, frame streams, frequently updated material rows, staging, and the descriptor buffers | Shared storage, write-combined CPU cache mode | Prefer `HOST_VISIBLE\|HOST_COHERENT\|DEVICE_LOCAL`, then `HOST_VISIBLE\|HOST_COHERENT`, then compatible `HOST_VISIBLE` |
| `DEVICE` | Textures and persistent buffers that benefit from private or compressed storage | Private placement heaps | Prefer `DEVICE_LOCAL` without `HOST_VISIBLE`, then compatible `DEVICE_LOCAL`, then any compatible type with a named degraded-placement report |
| `READBACK` | Capture, picking, counters read by CPU | Shared storage, default CPU cache mode | Prefer `HOST_VISIBLE\|HOST_CACHED\|HOST_COHERENT`, then cached, coherent, and finally compatible `HOST_VISIBLE` |

Vulkan additionally has `bufferImageGranularity`, which Metal does not. It is
handled by **segregation rather than padding**: allocator/device-memory blocks
are pooled by `(class, kind, memory_type_index, device_address_required)`, using
the buffer-versus-texture parameter the allocator core already carries, so
buffers and optimal-tiling images never share a block, every placement respects
the resource's exact `memoryTypeBits`, and address-bearing buffers never enter a
block allocated without the device-address flag. Metal's adapter collapses
those keys onto its placement heap. Same core, different adapter policy.
`VkMemoryDedicatedRequirements` is queried before placement; required and
initially preferred dedicated allocations bypass the range core but retain owner
and retirement accounting.
Where padding is unavoidable it is counted into the existing alignment-waste
row rather than hidden. Non-coherent upload ranges are flushed before submit,
and non-coherent readback ranges are invalidated after completion before CPU
access.

The exact heap count and block sizes are not decided here. They must come from
the observed Stage 2 allocation distribution, working-set budget, fragmentation,
and alignment waste. Growth occurs only on a cold allocation path; it never
occurs while encoding a draw.

### 6.2 Range allocator requirements

`VkrFreeList` may be reused only if the heap layer closes all of these gaps:

- aligned allocation must align the **chosen offset**, not merely round the
  requested size;
- the allocation record must retain the exact reserved start and size returned
  to `vkr_freelist_free()`;
- the 1,024-node bookkeeping ceiling must be configured/proven sufficient or
  replaced; exhaustion returns an explicit allocation error and metric;
- allocation failure must distinguish byte exhaustion, alignment/fragmentation,
  metadata exhaustion, and device allocation failure;
- double-free, overlap, integer overflow, and stale-generation use are rejected;
- no live allocation is moved unless every embedded GPU address and texture
  reference is rebuilt after proven completion.

One valid wrapper strategy is to reserve `size + alignment - 1`, align within
that reservation, and retain the original start/reserved size for free. It is
simple but exposes alignment waste and increases pressure. A tested aligned
free-range operation may be better. Stage 2 chooses from evidence; this design
does not perturb the frozen Vulkan backend in advance.

### 6.3 Retirement

Logical destroy invalidates the CPU handle immediately. The retirement record
keeps the native object, memory slice, texture token/material row, residency
membership if applicable, and last-use retirement point alive. In the initial
single-queue renderer that point is one monotonically increasing submit value.
If the backend cannot enqueue retirement, destruction fails without clearing
the shared texture record or advancing its generation; the owner may retry the
same handle. A successful destroy may still defer native release until the
recorded completion point.
Only after the completion domain reaches that point may the backend:

1. release the native object;
2. remove standalone residency allocations and commit that change, if the
   resource was not covered by a resident heap;
3. recycle a texture-reference/material-table slot;
4. return the heap range; and
5. recycle the logical registry slot.

Frame streams, upload rings, command allocators, readback slots, descriptor/
resource-reference rows, and indirect argument ranges obey the same rule. An
assumed frame lag is never completion proof.

Independent transfer or compute queues cannot reuse the scalar rule by taking
the maximum of unrelated queue serials: one queue may complete a larger value
while another still owns the resource. Before adding independent queues, the
backend must either join all relevant work into a truly ordered completion
domain or store a per-queue retirement vector and prove every component
complete.

### 6.4 CPU allocator relationship and metrics

GPU allocation remains outside the `VkrAllocator` vtable because CPU `free`
means reusable now, while GPU retirement means reusable after completion.
Accounting is shared through `vkr_allocator_report()` and existing
`VkrGpuAllocationOwner` categories.

Do not overload the current `memory.gpu.allocations.*` meaning. Today it counts
device allocations. A heap backend must publish separate device-heap and
logical-suballocation rows before recording a baseline:

- `memory.gpu.heaps.live|peak|created`;
- `memory.gpu.suballocations.live|peak|created`;
- live/peak requested bytes and reserved bytes;
- alignment waste;
- free bytes, largest free range, and metadata-capacity failures per class;
- owner totals over logical suballocations;
- driver-observed allocated size and recommended working-set budget, clearly
  labelled as driver values rather than allocator totals.

`MTLDevice.currentAllocatedSize` and `recommendedMaxWorkingSetSize` can feed
driver-level rows. `MTLHeap.usedSize`, `currentAllocatedSize`, and
`maxAvailableSizeWithAlignment:` can feed heap rows. None of those observations
by itself proves the absence of fragmentation or eviction pressure.

Stage 5 now publishes these as distinct rows. The legacy
`memory.gpu.allocations.*` rows retain native heap-allocation meaning, while
`memory.gpu.suballocations.*` owns placement counts, requested/reserved bytes,
alignment waste, free-range state, bounded failure classes, and separate
buffer/texture owner totals. Driver current allocation and recommended working
set remain explicitly labelled observations. The focused memory state machine,
Metal adapter, CPU suite, and production Bistro report all exercise the same
accounting model.

---

## 7. Texture and Material Lowering

The CPU `VkrMaterial`/texture-handle representation remains authoritative.
Backend GPU rows are derived once when a material or referenced texture changes.

### Metal 4

A material row contains scalar material fields and a backend-selected texture
reference representation. The conservative form stores explicit 64-bit resource
IDs for its texture slots, optionally in a second row to keep the hot material
prefix smaller. A compact form may store 32-bit indices into one or more bounded
`MTLTextureViewPool` ranges. A shader reaches the row from the root address and
constructs/samples textures through the Metal resource-ID mechanism proven by
Stage 0.

Stage 0 proves that pool slots receive contiguous IDs and that a pool-backed ID
stored in a GPU root record samples correctly. It does not yet prove the exact
shader arithmetic, ABI, pool growth policy, or retirement protocol for
`baseResourceID + index`. Stage 3 must select the representation from that
focused evidence and measurements rather than freezing explicit 64-bit rows by
assumption.

### Modern Vulkan

The same CPU material lowers to compact 32-bit indices into application-managed
descriptor-buffer heaps. The selected row is 64 bytes, holding
tint, four texture indices, four sampler indices, material identifier, and
flags, against Metal's 96 bytes carrying eight 64-bit resource identifiers. That
removes one third of the row bytes; any bandwidth or frame-time effect requires
measurement. It is the "denser natural
representation" ADR-022 anticipated for Vulkan and explicitly refused to force
onto Metal.

The two rows are **deliberately not unified.** Unifying would require either
Metal carrying compact texture-view-pool indices, which Stage 3 left unproven,
or Vulkan carrying 32 bytes of padding for nothing. The Vulkan GPU row therefore
differs from the Metal row; this is a backend lowering, not a second CPU
authority, and the ABI manifest simply gains a second record family.

Descriptor slots are owned by published texture/sampler resources, not by a
single material. A material row retains those publications; retiring the row
releases them only after completion. A slot returns to the heap only after its
last logical reference and last GPU use are both gone, which keeps shared
textures and canonical samplers valid across independent material replacement.

Contiguity and stride are guaranteed by the descriptor-buffer layout queries
rather than assumed: the per-binding base offset and the per-descriptor stride
are read from the device at initialization and never written as literals. See
the [bindless Vulkan backend specification](bindless-vulkan-backend-spec.md) §4.

### Publication and updates

GPU-visible rows are immutable after publication for as long as any pending
submission may read them. Hot reload or texture replacement allocates and fills
a new row, publishes the new logical mapping at a frame boundary, and retires
the old row against its last-use retirement point. A barrier can make writes
visible to later GPU work; it cannot make an in-place CPU overwrite safe for an
already pending submission.

Statically known samplers remain shader literals where Slang/MSL supports the
required state. Dynamic sampler state is a separate backend-native reference
with the same publication and retirement rule. The old MoltenVK sampler-alias
field remains required on the Vulkan 1.2 path until that path retires.

The implemented Metal material row stores one native sampler reference beside
each base-color, normal, ORM, and emissive texture reference. Loader-authored
repeat, min/mag, mip, and anisotropy state is cached outside draw recording and
published with the immutable row; the draw loop performs no sampler lookup or
creation. A retained texture's sampler may change before material publication;
once a live material references it, changing the sampler requires material
republication and the backend rejects an isolated in-place update. Because
sampler objects remain alive with renderer-lifetime immutable rows, the cache
covers the complete current canonical key domain (15,872 states) rather than
the simultaneous texture count; texture churn therefore cannot consume a
smaller lifetime-only cache.

Configured capacity is explicit. Exhaustion is reported and measured; it never
silently substitutes another texture or grows storage in the draw loop.

---

## 8. Shader ABI

Shaders remain authored in Slang provisionally. On 2026-08-05, all 15 current
`.slang` sources completed `slangc -target metal -o /dev/null` with Slang
2025.7.1. This proves that the existing source set reaches the Metal code-
generation path. The Stage 0 executable then proved a focused 32-byte,
16-byte-aligned root record containing a typed texture field: both hand-written
MSL and Slang-generated MSL compiled into Metal 4 pipelines, consumed direct and
pool-backed resource IDs, and produced the expected readback under Metal API and
shader validation. Its runtime binding check also records the current Slang
`ParameterBlock` lowering at argument-table slot 1.

That result accepts Slang for the initial bindless path, but it does **not**
replace reflection/layout tests for every shipped record, prove pointer-valued
specialization constants, or establish general lifetime correctness. The
hand-written MSL remains the bounded control for the capability spike, not the
default long-term shader language.

Root ABI rules:

- one read-only, non-aliasing root record per active shader stage;
- 16-byte record alignment and explicit field widths;
- hot scalar/pointer fields first;
- output/storage pointers are separate from the const root record;
- raw pointer vertex fetch is used where it improves the backend model, but the
  indexed draw path remains available until mesh-shader evidence justifies a
  different geometry path;
- texture reference fields are backend-lowered and are not assumed to have one
  portable width.

`_Static_assert` alone cannot compare a C struct to shader layout. The ABI needs
one durable authority: either a shared schema/header that Slang and C both
consume, or compiler-reflection metadata checked against C `sizeof`,
`_Alignof`, and `offsetof` values. The generated/reflected check runs in the CPU
suite for every shipped bindless shader. Pipeline creation rejects an
incompatible root signature before any frame is encoded.

Stage 5 implements the reflection form of that authority. A durable host
manifest records expected size, alignment, and field offsets for every shipped
address-referenced root or nested record. The CPU suite rejects incomplete,
duplicate, or host-incompatible entries. Metal pipeline creation requests
buffer-type reflection and compares each declared root plus its pointer element
records before accepting the pipeline. This gate found and corrected a real
tonemap padding error, and it pins 16-byte root alignment rather than relying on
matching C/MSL declarations by inspection.

### 8.1 Production shader ownership and correspondence

Production shader source is backend-owned. Vulkan and Metal both use domain
directories under `lib/src/renderer/<backend>/shaders/`: `common`, `world`,
`shadow`, `picking`, `text`, `skybox`, `ibl`, and `post`. Vulkan additionally
owns `ui` and `viewport`, which have no distinct Metal pipeline module. CMake
maps Vulkan source modules explicitly to the existing
`assets/shaders/*.spv` names, where `.shadercfg` manifests continue to resolve
runtime assets. Metal compiles `library.slang` and deterministically aggregates
the ordered native MSL domain inputs into generated runtime libraries. The
production renderer and backend-pinned harness snapshots consume those exact
artifacts; no shader source or backend-specific renderer validator lives under
`tools/`.

The source trees align by render domain, not by an artificial one-file-per-file
rule. Vulkan 1.2 keeps separate descriptor-set pipeline modules. Metal compiles
one pointer-based packet library, with corresponding domains exposed as named
entry points:

| Render domain | Vulkan source modules | Metal source modules |
| --- | --- | --- |
| World geometry and materials | `world/default.slang`, `world/pbr.slang` | `world/default.slang`, `world/default.metal`, `world/lighting.metalh` |
| Shadows | `shadow/cutout.slang`, `shadow/opaque.slang` | `shadow/opaque.slang`, `shadow/sampling.metalh` |
| Geometry and text picking | `picking/world.slang`, `picking/text.slang` | `picking/world.slang`, `text/default.metal` |
| UI and text | `ui/default.slang`, `text/default.slang` | `text/default.metal` |
| Skybox | `skybox/default.slang` | `skybox/default.metal` |
| IBL bake | `ibl/*.slang` | matching `ibl/*.metal` modules plus `ibl/common.metalh` |
| Tonemap and display | `post/tonemap.slang`, `viewport/display.slang` | `post/tonemap.metal` |

The bindless Vulkan backend adds a third tree,
`lib/src/renderer/vulkan_bindless/shaders/`, with the same domain split and a
**separate SPIR-V output namespace**, so the fifteen legacy outputs and their
`.shadercfg` manifests are untouched throughout migration. The legacy column of
the table above is deleted at [ADR-026](adr/026-vulkan-1-2-retirement.md)
step 2. Its shaders may be pure Slang: the limitation that forced hand-written
MSL on Metal — Slang aborting on a structured-buffer element containing texture
resource fields — has no analogue there, because the Vulkan material row carries
32-bit indices rather than texture objects.

Backend-only helpers stay local. Vulkan's transform, instance, alpha-cutout,
cube, CSM, and tonemap headers express descriptor-era reuse, and the bindless
tree does not reuse them for that reason. Metal keeps a
hybrid Slang/native-MSL library because Slang 2025.7.1 cannot lower a
texture-resource-bearing `StructuredBuffer` row; the native resource model and
lighting helpers are split by the same domains instead of hidden in one packet
file. A shared cross-backend helper is extracted only when it preserves both
APIs' concrete contracts.

---

## 9. Render Graph and Synchronization

### 9.1 Canonical graph record

The graph keeps resource identity and subresource ranges even when a backend's
barrier command does not. Those facts remain necessary for dependency edges,
pass culling, lifetime/alias planning, capture, diagnostics, and validation.

The portable use record is conceptually:

```text
{ resource, mip/layer range, access kind, shader/engine stages }
```

Attachment, transfer, and present stages can be inferred from their access
kind. Sampled/storage use must carry a precise stage mask or opt into a named
conservative default. The present `VkrImageAccessFlags` vocabulary alone is not
precise enough for the article's stage-mask model.

The Stage 4 graph compiler produces logical dependencies and batches compatible
transitions. A backend lowerer then emits commands:

- Metal: `MTLStages` plus `MTL4VisibilityOptions`, with the correct encoder-
  scoped or queue-scoped method;
- Vulkan 1.2: the existing access/layout/subresource barrier;
- modern Vulkan: synchronization2 access/stage masks and either global or
  resource/image barriers as required by enabled features and measured cost.

This permits the shipping Vulkan graph and the Metal graph to coexist. Stage 4
retains layout fields and exact resource/subresource ranges for Vulkan 1.2 and
graph diagnostics even though Metal's encoded barrier carries no resource list.

### 9.2 Layouts and external states

Metal has no Vulkan image layouts. A modern Vulkan backend may use
`VK_KHR_unified_image_layouts` to keep most images in `GENERAL`, but that feature
is optional and has exceptions such as initialization from `UNDEFINED`, present,
some feedback-loop information, and video/external ownership. It is present in
the current Vulkan 1.4.357 headers but remains unselected because its target
driver parity/performance gate has not run; the bindless Vulkan backend keeps
the full layout state machine and would select a unified path behind one profile
boolean.

Layouts also reach the descriptor model, which Metal's resource identifiers do
not. When `descriptorBufferImageLayoutIgnored` is false a descriptor is baked
with a specific layout, so a texture that is storage-written in one pass and
sampled in another needs **two descriptor slots over the same image view** — one
baked `GENERAL` for the write, one baked shader-read-only for the sample. Every
IBL bake target has this shape. Unified image layouts would collapse the two
slots into one.

The portable graph therefore does not make applications author Vulkan layouts.
The Vulkan lowerer still owns every layout or external-state transition required
by its enabled capability set. "Layout-free graph" does not mean "the Vulkan
backend may ignore layouts."

### 9.3 Barrier visibility and indirect work

Do not copy the article's hazard bitfield into the public seam before two
backends prove the vocabulary. Record semantic access and visibility instead:

- execution dependency between producer and consumer stages;
- device-memory visibility when writes feed later reads;
- resource-alias visibility when a heap range changes interpretation;
- descriptor/resource-heap reads and writes where the Vulkan extension exposes
  them;
- indirect-command consumption as a destination stage/access, not as an opaque
  Metal-specific flag.

Metal's list-free barrier is still a backend command. Vulkan is free to use a
global memory barrier only when the specification permits it and measurement
shows that it is not materially worse than resource-specific barriers on a
supported target.

### 9.4 Completion timeline

One totally ordered completion domain is the authority for command-allocator
reset, frame-stream reuse, memory/resource retirement, capture and readback slot
reuse, and residency removal. Stages 0–5 begin with one graphics queue, so a
monotonically increasing submit value is sufficient.

Metal queue events provide the submission timeline. Stage-local producer/
consumer barriers use Metal's barrier methods. The minimum interface does not
expose raw-memory `signal_after`/`wait_before` operations because neither VKR's
current workloads nor the Metal evidence requires them.

An independent transfer or compute queue requires an explicit join into that
timeline or a multi-queue retirement point. A numerically larger value from one
queue is not evidence that an unrelated queue has completed. The same rule
applies to the later Vulkan implementation.

Window-system presentation is outside that submit-completion domain. The
bindless Vulkan windowed target requires
`VK_KHR_swapchain_maintenance1` present fences so a retired swapchain's views,
render-complete semaphores, and handle are recycled only after the presentation
contract permits it. A timeline value or wait-idle inference is not substituted
for that WSI proof; the fence may signal before display, and offscreen targets
have no such obligation.

The CPU waits only when it would otherwise exceed the configured frames-in-
flight or reuse a still-owned bounded slot. Such waits are counted and must not
appear per draw or as an unreported allocation fallback.
`frame.command_slot_waits` publishes every such command-slot reuse wait. The
existing `upload.fence_waits` row is the upload-path subset, so the two counts
must not be added together.

---

## 10. Frontend and Feature Impact

The intended shared/changed boundary is a hypothesis until the Metal walking
slice proves it.

**Expected to remain shared:** public generation handles, render packet payload
semantics and validation, JSON graph/pass names, scene/ECS state, asset decode,
visibility decisions, camera data, harness cases, and CPU metrics registry.

**Backend-lowered:** shader/pipeline creation, GPU material rows, texture
references, vertex/index access, render-target realization, graph barrier
commands, present targets, capture/readback, residency, and device-memory
metrics.

**Legacy-only until Gate B:** descriptor pools/sets, instance descriptor state,
descriptor-write elision, sampler aliasing, classic Vulkan render passes and
framebuffers, and the 15 descriptor-set shader programs.

The mesh and material managers currently make no direct backend calls, but that
does not make their GPU data responsibilities unchanged. CPU asset identity can
remain; material-table publication and mesh-address lowering are new owners with
new lifetime rules.

The render graph's JSON authoring can remain stable, while its compiled backend
state becomes an explicit lowering. Pass executors remain shared only when they
express work through graph/payload semantics rather than legacy descriptor or
render-pass operations.

---

## 11. Modern Vulkan Capability Profile

**This section is now a summary.** The full profile, its rationale, and its
staged implementation live in the
[bindless Vulkan backend specification](bindless-vulkan-backend-spec.md), and
the decision itself in
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md).

"Vulkan 1.4" is a version floor, not the feature contract. The Windows backend
queries and records an immutable capability profile at startup and rejects any
device that does not satisfy it, with a precise per-device report.

Minimum required capabilities:

| Capability | Requirement |
|---|---|
| Buffer device address, 64-bit shader integers | Core Vulkan 1.2/1.0 features enabled; shader address ABI validated by reflection |
| Timeline semaphore | Core Vulkan 1.2 feature enabled for completion/retirement |
| Descriptor indexing minimum set | Runtime descriptor arrays plus non-uniform sampled-image and storage-image indexing; descriptor-buffer bindings already imply partially-bound/update-after-bind behavior without those feature bits |
| Scalar block layout | Makes the SPIR-V layout of a mixed address/vector/matrix record equal the C layout |
| Host query reset | Timestamp pool reset for per-pass timings |
| Dynamic rendering, synchronization2, maintenance4 | Core Vulkan 1.3 features enabled |
| Maintenance5 | Core Vulkan 1.4 feature enabled; size-bounded index binding |
| Bindless texture references | **`VK_EXT_descriptor_buffer` required, with no fallback** |
| Windowed presentation lifetime | Base swapchain plus per-image semaphore retention until a submit consuming the reacquired image's acquire semaphore completes; maintenance1 remains optional but supplies explicit per-image present fences when available |
| Shader language/toolchain | Slang→SPIR-V address and descriptor model validated by reflection, validation layers, and a rendered case |

`VK_EXT_descriptor_heap` is the standardized successor and closer to the
article's raw heap model. It was absent from the observed Vulkan 1.4.313 SDK and
is present in the current 1.4.357 headers, but it cannot be selected until target
drivers, validation, debugger, shader tooling, and rendered behavior are
demonstrated. It is retained as a forward migration trigger, not as the current
implementation; header presence is not executable target evidence.

Descriptor buffers do retain layout and mapping rules, and their descriptor
sizes are backend-defined — but that variance is contained rather than exposed.
**Backend-defined descriptor sizes never reach the shader:** the shader indexes
an unbounded array with a 32-bit integer, and the driver turns that index into a
layout-defined byte address. The application computes byte offsets only when
writing a descriptor, host-side, so a size that varies across drivers is
contained in the heap layout/writer module and changes zero bytes of shader ABI.

No fallback to traditional per-material descriptor sets is required or provided.
The frozen Vulkan 1.2 renderer remains the compatibility path until its
retirement gates in [ADR-026](adr/026-vulkan-1-2-retirement.md).

Optional capabilities — `VK_KHR_unified_image_layouts`, mesh shaders,
device-generated commands, shader objects, graphics pipeline libraries, and
host image copy — are recorded unconditionally at startup but consumed only
after a named measurement gate. The backend retains layout lowering whether or
not unified layouts are present.

**The backend cannot run on the current development machine.** With SDK
1.4.357.0 the Apple M1 Pro reports Vulkan 1.4.334 and MoltenVK 1.4.1 but still
does not expose required `VK_EXT_descriptor_buffer`, so every bindless Vulkan
runtime gate requires Windows hardware. This inverts the premise ADR-021 used
to choose Metal first.

Linux platform/window support does not currently exist and is separate work.
The Vulkan backend cannot be called a Windows/Linux implementation until the
platform target, build scripts, presentation/offscreen paths, and validation
matrix exist on each claimed platform.

---

## 12. Staged Implementation

Each stage is a vertical slice. A header that compiles without a caller is not a
stage.

| Stage | Deliverable | Required evidence |
|---|---|---|
| 0 — capability spike | Runtime Metal 4 gate; compiler, command allocator, argument table, root buffer address, one texture `MTLResourceID`, stage barrier, offscreen readback | Metal API/shader validation clean; Slang and a bounded hand-written MSL control both render the expected triangle; root/resource-ID ABI recorded |
| 1 — walking renderer | Windowed and offscreen clear plus indexed textured draw through the selected Metal implementation strategy | Resize/drawable lifecycle; deterministic offscreen color and exact ID capture; no per-draw allocation/lock/string/backend branch |
| 2 — memory/lifetime | Placement heaps, upload/readback rings, address pairs, generation handles, residency, submit-value retirement, allocation metrics | Focused aligned-range/stale-handle/retirement tests; repeated create→submit→destroy cycles return logical totals; Metal validation clean |
| 3 — bindless materials | Multiple materials lowered to explicit Metal texture-reference rows; immutable publication and retirement | Multi-material capture; texture replacement while frames are pending; no per-material texture slot binding in the draw loop; capacity/overflow metrics |
| 4 — graph lowering | Canonical access+stage records and separate Vulkan 1.2/Metal barrier lowerers | Existing graph CPU tests plus new stage/subresource cases; `./build_test.sh`; Vulkan validation for the unchanged legacy result; Metal validation and deterministic dependency cases |
| 5 — feature parity | Packet/graph execution for every required pass, capture/readback, windowed/offscreen, metrics, asset load/unload | Gate A checklist in ADR-021, including Bistro bootstrap/comparison policy below |
| 6 — modern Vulkan | Capability-gated Windows implementation, then any claimed Linux target, using the proven semantic model. **Expanded into the V0–V7 ladder in the [bindless Vulkan backend specification](bindless-vulkan-backend-spec.md) §11**, because a single row is not a vertical slice | Native validation layers, pipeline/shader checks, backend matrix, snapshots, load/unload, and authoritative performance profiles per target — all on Windows hardware, since the backend cannot run on macOS |

Stage 0 completed on 2026-08-06 in a standalone capability fixture that was
retired after the production explicit-ID path and packet validator subsumed its
selected invariants. Its recorded run passed on Apple M1 Pro with Metal API and
GPU shader validation enabled. It verified the runtime
Metal 4 gate, a contiguous two-slot texture-view pool, root GPU addresses,
producer/consumer stage barriers, and exact RGBA8 readback for four variants:
hand-written MSL and Slang-generated MSL, each with a direct and pool-backed
texture ID. This is capability evidence, not a production renderer call site.

Stage 1 completed on 2026-08-06 in a Metal-private walking slice. Its dedicated
module and shader fixture were retired after the packet renderer replaced it.
The selected
offscreen and `CAMetalLayer` target strategies share precreated pipelines,
indexed vertex/index/root buffers, argument tables, residency, two bounded
frame slots, render-pass descriptors, and readback storage. Metal API and GPU
validation passed offscreen 8x8→13x7 and hidden-window 64x48→96x72 runs. Each
target submitted four indexed textured frames, reused a frame slot, presented
window drawables, and captured exact 32-bit IDs; offscreen RGBA8 was also exact.
Source audit and runtime counters found no renderer-owned draw-path allocation,
lock, string construction, or backend branch. This remains a walking module,
not the application renderer or Gate A feature parity.

Stage 2 completed on 2026-08-06 in `vkr_metal_memory` and
`vkr_metal_memory_device`. Its now-retired focused fixture proved private
placement-heap buffers and textures, a three-allocation residency set, and
separate shared upload/readback address-pair rings over 64 GPU copy/readback
cycles under Metal API and GPU validation. All 128 resources retired, both
rings reused 62 slots, every byte matched, and logical/native live totals plus
the 4 MiB free range returned to their initial values. The maintained CPU tests
are now the deterministic authority for aligned over-reservations, generation
handles, stale handles, out-of-order retirement, exhaustion classification,
range-metadata exhaustion, and ring-busy behavior. Production Metal snapshots
exercise the integrated native allocation and retirement path.

Stage 3 completed on 2026-08-06 in `vkr_metal_material_table` and a focused GPU
fixture that was retired after production packet validation covered material
publication, replacement, exact IDs, and capture. The selected 96-byte Metal
row stores four texture and four sampler `MTLResourceID` payloads, tint,
material ID, and flags. A three-row fixed-capacity table published red/blue
rows, held the first render pending
behind a GPU event, atomically published a green replacement, immediately
invalidated the red generation, and retained the old row and placed texture
until submit completion. One indexed draw covered both materials without a per-
material texture bind. The pending capture was exactly red/blue and the next
capture exactly green/blue, including IDs; the intentional fourth publication
reported capacity exhaustion and all rows/textures later drained to zero under
Metal API and GPU validation.

Slang 2025.7.1 currently aborts with an internal compiler error when a
`StructuredBuffer` element contains texture resource fields. Stage 3 therefore
retains a Slang-generated vertex/root-address path and uses a bounded hand-
written MSL fragment as the resource-bearing control. The Metal compiler's
96-byte row `static_assert`, host offsets, pixels, and IDs validate this Metal
ABI, but do not prove portable Slang material-row lowering. The production path
therefore keeps the fallback as an explicit, domain-split Metal-only shader
boundary; it does not claim the failing Slang path works.

Stage 4 completed on 2026-08-06 in the shared graph compiler plus the separate
`vulkan_dependency` and `vkr_metal_dependency` lowerers. Image and buffer uses
now carry a canonical execution-stage scope; compiled barriers preserve source
and destination stages plus device/resource-alias visibility alongside the
existing resource, subresource, access, and Vulkan-layout facts. Existing APIs
select the former conservative Vulkan scopes, while explicit staged APIs permit
narrower dependencies. CPU tests pin the unchanged legacy masks and an exact
one-layer compute-write-to-fragment-read case. The Debug Vulkan 1.2 application
completed startup, three-image swapchain recreation, steady frames, and shutdown
with validation enabled and no diagnostics. The now-retired focused Metal
fixture lowered transfer and compute-to-fragment records, then passed 16
split-encoder and 16 intra-encoder exact readbacks under Metal API and GPU
validation. CPU barrier tests now pin deterministic lowering facts, while
production Metal snapshots exercise them through the application graph
executor delivered by Stage 5.

Stage 5 completed on 2026-08-06 in the Metal-private
`vkr_metal_packet_renderer`. Its former focused backend validator has been
retired; backend-pinned harness snapshots are the maintained end-to-end
authority. Coarse prepare/submit calls consume the real
`VkrRenderPacket` and `main.rendergraph.json`, schedule the shared graph, cache
and resize placement images, lower Stage 4 dependencies, acquire/present real
`CAMetalLayer` drawables, and return exact readback. The current asset-backed
vertical uses fixed-capacity generation-handled mesh, material, and cubemap
registries; private placement buffers/textures; immutable explicit-ID material
rows; Slang vertex pulling; and fourteen precreated Metal pipelines. Real pass
bodies now cover shadow cascades, skybox, opaque, transmission, alpha blend,
geometry picking, tonemap, editor/UI draw lists, transfer feedback, and a
production-size IBL convolution into a 64-square irradiance cube, every mip of
a 256-square nine-mip GGX prefilter cube, and a 128-square split-sum BRDF LUT.
The first source generation dispatches all 11 jobs; subsequent packets reuse
the bake until source replacement. The focused offscreen/windowed evidence runs
execute 172 authored pass instances and 494 dependency halves. A five-channel declared
capture batch returns aligned final-color, HDR scene-color, depth, shadow-layer,
and picking-ID layouts with exact producer names, shadow depth, and center ID.
Exact RGBA8 probes, constant-environment analytical checks across irradiance and
all prefilter mips, and bounded BRDF samples pass under Metal API and GPU
validation. Merged application-loader mesh results publish one shared vertex/
index allocation plus fixed-capacity submesh ranges, and the focused case
alternates two ranges without duplicating the buffers. Mesh, material, and
cubemap unload/reload immediately reject stale handles, retain
native objects through their last submit, and fully drain the 64 MiB heap.
The Objective-C implementation remains one translation unit with one private
renderer state owner, but graph/dependency handling, command/timing ownership,
setup/pipelines, resource publication/retirement, frame encoding, and public
lifecycle code live in named private implementation units. This is source
organization only: it adds no public vtable or hot-loop dispatch.

The source asset boundary accepts the same finite 2:1 RGBA16F payload produced
by the shared HDR decoder. An additional compute pipeline uploads it, converts
it to a mipmapped cube, and retires the temporary source against the completed
submit value. The focused world fragment consumes irradiance, prefilter, and
BRDF references from its 512-byte root record. The version-10 packet additionally
carries backend-neutral directional-light and IBL controls plus immutable PBR
scalar material fields. Visible skybox selection and the lighting IBL source
remain distinct handles in the packet, but application submission selects the
same ready scene-authored environment cubemap for both. The scene loader is the
only owner of environment asset selection and preparation. World resources
retain the first successfully prepared scene environment as the fallback; the
six-face JPG loader remains available only for explicitly authored legacy
cubemap sources.
It also borrows the application lighting system's
bounded point-light table and conservative grid; Metal packs those records once
per frame into GPU-addressed upload storage. It also borrows up to 16 ready scene
reflection probes. Metal resolves their generation handles once per frame into
64-byte GPU records containing native irradiance/prefilter references, box
volumes, blend weights, and intensity controls. World shading normalizes
overlapping local influence, box-projects specular rays, and preserves the global
environment as the remaining fallback weight. Captures prove finite ordered IBL,
IBL-disabled directional light, and a grid-selected punctual light with distinct
analytically expected channel ordering. Cascade matrices are likewise packed
once per frame; world shading samples the graph-owned depth array, and a focused
occluder/receiver capture proves a shadowed directional result. Four distinct immutable material texture references supply base color,
normal, ORM, and emissive inputs. A tilted tangent-space normal capture and an
alpha-cutout capture that exposes the exact HDR skybox value prove the auxiliary
texture and cutoff paths. The transmission pass samples the graph-declared
pre-transmission scene copy, applies IOR/thickness attenuation, and is proven by
a green-selective absorption capture. A focused green-probe capture proves local
influence displaces the global environment at an enclosed receiver. The 512-byte
record and upload stride keep adjacent draw roots disjoint while retaining
Metal's 256-byte address alignment.

Packet version 10 also carries prepared retained-text draws. The UI and world
text resources now retain shaped CPU vertices/indices and a revision instead of
discarding geometry after a legacy Vulkan upload. Backend-neutral collectors
publish the logical atlas handle, model transform, font mode/range, and picking
identifier after queued text updates are applied. Metal copies the borrowed
geometry once into its bounded frame upload ring, resolves the atlas handle to
a native texture reference, and uses dedicated blended world/UI and integer
picking pipelines. The declared capture proves distinct exact UI and HDR world
text pixels plus both text picking identifiers. No glyph shaping, allocation,
texture lookup, or backend dispatch occurs inside a Metal text draw loop.
A backend-neutral harness fixture now prepares fixed system-font UI, bitmap UI,
MTSDF UI, and MTSDF world text through those production collectors. Two runs per
backend are deterministic, and the post-fix Vulkan and Metal final-color PNGs
are byte-identical. The fixture exposed three correctness defects: the legacy
Vulkan text pipeline inherited back-face culling, Vulkan squared destination
alpha by using `SRC_ALPHA` for its alpha blend factor, and Metal inverted the
shared Vulkan-oriented UI Y coordinate a second time. Picking-ID captures remain
deterministic within each backend but intentionally differ in coverage because
legacy Vulkan writes the full glyph quad and Metal discards outside atlas
coverage.

When the packet requests pass timestamps, the renderer writes start/end values
to a fixed-capacity Metal 4 counter heap around each authored graph pass. The
completed validation result reports one named, finite interval per executed
pass. A bounded publication adapter copies those intervals into the existing
application pass-metrics table with source frame/submit identity; truncation has
deterministic CPU coverage. Timing is opt-in instrumentation and no performance
claim is made from these Debug samples. Asynchronous submits retain the completed
interval array under its submit value after command completion, so a later
application poll can publish the correct older GPU identity without blocking the
original submit.

The same executable also proves fourteen-pipeline Metal 4 archive cold capture and
warm lookup. Metal GPU Validation on the installed macOS 26.5 stack rejects a
same-device `MTL4Archive` as an incompatible deployment target, so the gate is
split without weakening either half: archive behavior runs under Metal API
Validation, and all shader execution runs under API plus GPU Validation with
archive lookup disabled. No compilation-speed claim is made. The application
frontend now selects this same renderer with `--renderer metal`, publishes
loader-owned mesh, texture, cubemap, and material resources through a coarse
backend boundary. On Windows, Vulkan is the default and explicit
`--renderer vulkan` selects it directly; no diagnostic Vulkan 1.2 path remains.
The harness selects Metal with `VKR_HARNESS_RENDERER_BACKEND=metal`.
Scalar PBR, four material texture slots, alpha cutoff, directional and grid-
selected punctual lighting, directional shadow sampling, global IBL controls,
material transmission, and local reflection probes are GPU-proven. A fixed-capacity
Metal-private capture ring now gives declared batches
monotonic request identity, nonblocking submit, completion-value polling,
durable request-owned result bytes, explicit release, and abandoned-request
retirement. Production output uses an sRGB target, the shared ACES tonemap, and
the Metal clip-space conversion required by the Vulkan-oriented shared camera
matrices. The guarded Metal Bistro generation remains immutable, but its pixel
acceptance is historical after the retained-IBL, sampler, transparency, and
presentation corrections. The owner accepted corrected replacement output on
2026-08-11, completing Gate A and authorizing Metal selection.
Per-pass Metal timing
publishes in the shared pass-table format for synchronous evidence and is
submit-keyed for later polling after asynchronous completion.

Stage order can change only when dependencies permit it. Before V7, graph
lowering tests alone could not authorize deleting the legacy Vulkan lowering;
ADR-026 now records the explicit owner authorization and residual evidence risk
under which that retirement completed.

The historical Gate A image generation is the backend-pinned
`smoke.bistro.metal.text.snapshot` case: fourteen 1600x1200 Bistro views with
the deterministic system-font, bitmap, and MTSDF text fixture. Release source
run `20260807T091943.686Z-012428` passed all fourteen children and produced
generation
`sha256:3db4f4d2294e5fdbc3618e64c4b2baf03bf66051dee0c4ff452e341d20cae51d`.
Fresh snapshot `20260807T092542.337Z-0144b6` and explicit compare
`20260807T092754.437Z-015047` pass every row; maximum MAE is
`0.0000012208946078431343`, maximum normalized delta is
`0.35294117647058826`, and no pixel exceeds policy. Metal API, GPU, and shader
validation had already passed twice for the deterministic text fixture and
across the Stage 1-5 matrix. A later parity review invalidated those pixels as
current acceptance evidence; the immutable generation is retained only to
detect and explain the intended correction. The owner reviewed and accepted
corrected replacement output on 2026-08-11. Local replacement artifacts were
removed on request, so this is a recorded acceptance rather than a retained
pixel-baseline authority. The legacy Vulkan mate remains historical and does
not widen Metal's thresholds.

The earlier failed repetition, `20260806T182054.703Z-009e5d`, exposed a real
allocator race: the Metal packet graph and async resource path storage shared
the frontend's non-thread-safe arena. Random dependency indices and malformed
one-byte texture paths were two manifestations of the same ownership defect.
The Metal graph now uses the dedicated render-graph `VkrDMemory`; dependency,
cull, and topological-sort scratch uses the scoped graph-frame arena. A
500-frame CPU regression pins constant persistent/frame high-water use, and 12
fresh validation-enabled Bistro processes pass without the prior assertion or
path corruption.

---

## 13. Correctness and Baseline Policy

### 13.1 Metal baseline bootstrap

The existing Vulkan Bistro golden images are not silently relabelled as Metal
goldens, and existing tight same-backend tolerances are not widened.

The first Metal baseline requires:

1. identical case inputs, camera keys, resolution, scene-content manifest, and
   deterministic work-volume/ID semantics;
2. exact identifier captures;
3. an explicitly named cross-backend color comparison whose initial tolerances
   come from inspected diffs, not from a guessed threshold;
4. full-resolution visual review of every required view;
5. clean Metal API/shader validation logs;
6. two fresh Metal runs within the proposed same-backend tolerances; and
7. the repository's guarded baseline proposal/acceptance workflow, with report
   path and SHA-256 recorded.

Only after acceptance does the Metal golden become the same-backend regression
authority. Cross-backend comparison remains a separately labelled diagnostic;
legitimate rasterization/filtering/precision differences must not weaken either
backend's own regression threshold.

The same seven-step policy applies unchanged to the first bindless Vulkan
baseline, with two additions. **Windows hardware is a prerequisite** — the
backend cannot run on the development machine at all, so the baseline cannot be
bootstrapped locally. And after the legacy Vulkan 1.2 path retires there is no
second renderer on Windows, so cross-backend comparison there becomes
cross-*platform* comparison against Metal on macOS, which confounds driver, API,
and platform differences at once. That comparison remains a diagnostic and never
becomes either backend's regression authority.

### 13.2 What CPU tests do and do not prove

CPU tests own graph dependency construction, range allocation, generation
handles, retirement state machines, ABI metadata, and deterministic material
lowering. They do not prove encoded GPU barriers, residency, resource IDs,
drawable ownership, or pixels.

Anything that encodes Metal or Vulkan work also requires that API's validation
and a rendered/readback case. `./build_test_batch.sh` is reserved for a test that
failed once and then passed; it is not a routine stage gate.

### 13.3 Performance

No stage is faster by definition. Any comparison requires Release builds,
matching logical workload/resolution/editor/cascade/present conditions, stable
warmup, multiple independent repetitions, complete metrics, and retained
lifetime/correctness evidence. The API/driver/backend necessarily differ, so a
Metal-versus-Vulkan result is an end-to-end comparison, not proof that one
isolated design choice caused the delta.

Use the authoritative harness profiles and reporting shape owned by
`vkr-performance`. Record the report path/digest, environment/workload/policy
fingerprints, variance, and what was not measured.

---

## 14. Principal Risks and Revisit Triggers

- **A later Slang/Metal ABI diverges from its host record.** Reject pipeline
  creation through generated/reflected layout checks; do not hide the mismatch
  behind runtime casts.
- **Compact texture-view-pool indices fail their Stage 3 shader, growth, or
  lifetime gates.** Fall back to explicit 64-bit IDs and measure their material
  bandwidth/register cost before revisiting the platform choice.
- **The shared frontend accumulates backend branches.** Split implementation
  orchestration and keep dispatch outside hot loops.
- **Heap fragmentation or metadata capacity causes churn.** Revisit heap sizes,
  aligned-range structure, and placement policy using requested/reserved/largest-
  range metrics.
- **GPU addresses become relocatable.** Add an explicit relocation/rebuild
  protocol gated by completion; never move live address-bearing data silently.
- **Resource-list-free barriers over-synchronize or hide bugs.** Preserve graph
  resource diagnostics, add per-backend validation cases, and allow a Vulkan
  resource-specific lowering.
- **The macOS 26 floor is unacceptable.** Revisit Metal 3 or add another tested
  implementation under a new decision; do not create an untested hidden
  fallback.
- **Gate A takes too long.** This contingency originally kept MoltenVK supported
  until replacement output was accepted. ADR-026 records the later explicit
  authorization that retired it; future fallback policy requires a new decision
  rather than a hidden legacy path.
- **Modern Vulkan capability support is too narrow.** Revisit the required
  descriptor model or target matrix from real Windows/Linux device data.
- **The bindless Vulkan backend has no local development loop.** MoltenVK
  reports Vulkan 1.2 without descriptor buffers, so every runtime defect costs a
  remote Windows round trip. Maximize locally executable coverage, keep the
  offscreen target headless so a display-less runner executes the whole
  functional matrix, and establish remote build and harness invocation early.
- **Graphics-debugger capture is degraded on descriptor-buffer applications**
  without `descriptorBufferCaptureReplay`. Losing RenderDoc or PIX on the only
  platform that can run the backend compounds the previous risk. Enable the
  feature in diagnostic configurations when present; never in Release.
- **`VK_EXT_descriptor_heap` later supersedes descriptor buffers.** One migration
  is plausible. Keep index-to-descriptor translation in a single module to
  contain it, while still rerunning pipeline-layout, reflection, capture, and
  shader ABI gates rather than assuming a one-file swap.
- **The shared-core extraction destabilizes the shipping Metal path.** Land it as
  one module at a time only when its real Vulkan caller lands, with the Metal
  snapshot as its correctness witness and a Release Metal profile as its
  performance witness.
- **Retiring Vulkan 1.2 leaves no portable diagnostic path.** After the final
  gate a Windows machine without descriptor buffers has no renderer at all. This
  is the intended end state and is recorded in
  [ADR-026](adr/026-vulkan-1-2-retirement.md) rather than discovered.
- **Independent queues are justified.** Replace the single scalar retirement
  point with a proven queue join or per-queue vector before enabling their
  resources to retire independently.

---

## 15. Reproducible Observations

These commands reproduce the document-level observations only:

```sh
# Installed Metal SDK symbols and limits.
metal_sdk=$(xcrun --show-sdk-path)
metal_headers="$metal_sdk/System/Library/Frameworks/Metal.framework/Headers"
rg -n 'maxBufferBindCount|maxTextureBindCount|maxSamplerStateBindCount|setAddress:' \
  "$metal_headers/MTL4ArgumentTable.h"
rg -n 'barrierAfter.*Stages|MTL4VisibilityOption' \
  "$metal_headers/MTL4CommandEncoder.h"
rg -n 'gpuResourceID' "$metal_headers/MTLTexture.h"

# Current shader sources reach Slang's Metal target.
find lib/src/renderer/vulkan/shaders -name '*.slang' -exec sh -c '
  for shader_file do
    slangc -target metal -I lib/src/renderer/vulkan/shaders \
      "$shader_file" -o /dev/null || echo "FAIL $shader_file"
  done
' sh {} +

# Current backend and graph facts.
awk '/typedef struct VkrRendererBackendInterface/{in_interface=1} \
     in_interface{print} \
     /} VkrRendererBackendInterface;/{in_interface=0}' \
  lib/src/renderer/vkr_renderer.h | rg -c '\(\*'
rg -n 'rf->backend\.|renderer->backend\.' \
  lib/src/renderer/renderer_frontend.c lib/src/renderer/vkr_capture.c
rg -n 'VkrRgSubresourceState|VkrRgImageBarrier' \
  lib/src/renderer/vkr_render_graph_internal.h lib/src/renderer/vkr_rg_compile.c

# Installed Vulkan runtime/SDK observation; not a future target matrix.
vulkaninfo --summary

# Which bindless-relevant extensions exist in the installed SDK headers.
sdk="$VULKAN_SDK/include/vulkan/vulkan_core.h"
for e in VK_EXT_descriptor_buffer VK_EXT_descriptor_heap \
         VK_KHR_unified_image_layouts; do
  grep -q "define $e 1" "$sdk" && echo "PRESENT $e" || echo "ABSENT  $e"
done
```

Observed on 2026-08-08 with SDK 1.4.313: `VK_EXT_descriptor_buffer` is present;
`VK_EXT_descriptor_heap` and `VK_KHR_unified_image_layouts` are absent. That
dated local runtime was MoltenVK on an Apple M1 Pro reporting `apiVersion
1.2.296` without descriptor buffers. On 2026-08-09, SDK 1.4.357 headers contain
all three extensions and the device reports Vulkan 1.4.334 / MoltenVK 1.4.1,
but it still does not expose `VK_EXT_descriptor_buffer`, so it still cannot run
the bindless Vulkan backend. The
[bindless Vulkan backend specification](bindless-vulkan-backend-spec.md) §12
carries the full observation set and records what was *not* verifiable — notably
the specification's per-version mandatory-support tables, which no claim in
either document depends on.

Primary Vulkan references for the future capability profile:

- [VK_KHR_unified_image_layouts proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_unified_image_layouts.html)
- [VK_EXT_descriptor_buffer proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html)
- [VK_EXT_descriptor_heap guide](https://docs.vulkan.org/guide/latest/descriptor_heap.html)
