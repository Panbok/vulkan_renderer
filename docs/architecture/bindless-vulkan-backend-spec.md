---
status: proposed
updated: 2026-08-08
authority: design
---

# Bindless Vulkan 1.4 Backend — Design Specification

**Document status:** Proposed. No production code, no call site, and no
executable evidence exists for this backend. Every capability observation below
was made against the locally installed Vulkan SDK 1.4.313 headers, the
`vk.xml` registry shipped with it, and the local MoltenVK runtime; those
observations are recorded in §12. Nothing here is a measurement, and nothing
here authorizes a status change in the
[renderer status specification](renderer-architecture-spec.md).

**Scope:** A bindless Vulkan 1.4 renderer for Windows, built on the semantic
model that the Metal 4 backend already implements, followed by the complete
removal of the Vulkan 1.2 backend. Linux is admitted as a separate,
evidence-gated stage whose platform work is named in §10.3 but not designed
here.

**Non-goals:** Changing shipping Vulkan 1.2 behaviour before its retirement
gates; making a mesh-shader, device-generated-command, or shader-object path
authoritative without measurement; designing the Linux window/input/filesystem
layer; claiming any performance result.

**Companions:** The
[bindless GPU-address design](bindless-gpu-pointer-renderer-spec.md) remains the
umbrella design and owns the portable model. This document is the Vulkan-specific
implementation design for that model's Stage 6.
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md) owns the capability
contract, [ADR-024](adr/024-shared-bindless-gpu-cores.md) the shared cores,
[ADR-025](adr/025-selected-renderer-implementation-strategy.md) the
implementation seam, and
[ADR-026](adr/026-vulkan-1-2-retirement.md) the retirement sequence.

---

## 1. Prerequisite: this backend cannot run on the development machine

The local Vulkan runtime is MoltenVK. It reports `apiVersion 1.2.296` on an
Apple M1 Pro and does not expose `VK_EXT_descriptor_buffer`. Both facts are
disqualifying against the capability profile in §3, and neither is likely to
change: MoltenVK has no descriptor-buffer implementation, and Metal argument
buffers are a poor structural match for one.

The consequence is stated first because it governs the whole staging plan:
**every runtime gate in this document requires Windows hardware.** If no Windows
machine or runner is available, stage V0 does not start.

This inverts the premise ADR-021 used to choose Metal first — that "the most
uncertain shader/resource ABI is tested on the machine used for daily
iteration." For this backend the most uncertain ABI is tested on a machine the
developer is not sitting at. That is a process cost, not a design flaw, but it
must be planned for rather than discovered.

### 1.1 What can be gated without Windows

This list is the ceiling on local iteration and is referenced by every stage in
§11.

| Gate | Why it works on macOS |
|---|---|
| Shared-core extraction (V1) | The four extracted modules are pure C with no GPU API; the shipping Metal snapshot is their correctness witness |
| Implementation-seam refactor (V2) | Metal and legacy Vulkan-via-MoltenVK are both selectable locally |
| Host ABI manifest check | `vkr_gpu_abi_validate_host()` compares `offsetof`/`sizeof`/`_Alignof` against a durable table; no device needed |
| Offline SPIR-V reflection | Runs against built `.spv` artifacts in the CPU suite; `slangc` targets SPIR-V on macOS |
| Barrier-lowering table tests | The lowering functions in §8 are pure functions of the compiled graph record |
| Compile-only builds | Only if the backend is `#if defined(PLATFORM_WINDOWS)`-guarded, mirroring Metal's `PLATFORM_APPLE` guards |

Everything else — device creation, descriptor publication, draw recording,
presentation, capture, pixels — needs Windows.

### 1.2 Headless is not optional

Because the development loop is remote, the offscreen present target is the
mechanism that makes evidence automatable at all. The platform seam in §10.1
classifies surface extensions so `VKR_PRESENT_TARGET_OFFSCREEN` drops
`VK_KHR_surface` and `VK_KHR_win32_surface` entirely, letting a Windows CI
runner with no display execute the whole functional matrix. This is the existing
[ADR-014](adr/014-offscreen-present-target.md) pattern applied for a new reason.

---

## 2. Relationship to the Metal backend

The Metal 4 backend is the reference implementation, not a template to copy
literally. What transfers is the semantic model; what does not transfer is
called out explicitly wherever it appears.

| Concern | Transfers unchanged | Diverges |
|---|---|---|
| Buffer access | 64-bit shader addresses for vertices, instances, materials, lights, cascades, probes, text | Index buffers still bind as objects (§5.6) |
| Material identity | One CPU `VkrMaterial` authority lowered once into an immutable GPU row | Row is 64 bytes of 32-bit indices, not 96 bytes of 64-bit IDs (§4.5) |
| Retirement | One totally ordered completion domain; `last_use_submit_value` per object; collect on completion | Timeline semaphore, plus a binary-semaphore exception for WSI (§6.4) |
| Graph dependencies | The same canonical `VkrGpuDependency` record, unchanged | Vulkan additionally consumes access masks and layouts the graph already carries (§8) |
| Root delivery | One address per stage reaching an immutable record | Push constant instead of an argument table (§7.1) |
| ABI validation | Durable host manifest plus reflection cross-check at pipeline creation | No shader-side static assertion exists (§7.4) |
| Barrier scoping | Producer and consumer halves around encoder boundaries | Metal's intra-encoder barrier has no Vulkan analogue (§8.5) |

---

## 3. Capability profile

The full decision, its alternatives, and its consequences live in
[ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md). This section
specifies the mechanism.

### 3.1 Shape

One `VkrVkCapabilityProfile`, filled once during physical-device selection,
stored by value, and `const` after initialization. Nothing in the frame path
re-queries it. It carries three sections:

- **enabled** — what the device was created with;
- **recorded** — what the device reports but no code consumes yet;
- **rejection** — why each rejected candidate device failed.

### 3.2 Required entries

Every entry is queried and enabled explicitly. A missing entry rejects the
device with a named report row.

| Entry | Core version of the feature bit | Why it is required |
|---|---|---|
| `apiVersion >= VK_API_VERSION_1_4` | — | Version floor for instance and device |
| `bufferDeviceAddress` | 1.2 | The entire model. Root records and every array are reached by address. Also required to bind a descriptor buffer, whose binding info takes a device address |
| `shaderInt64` | 1.0 | Address fields and address arithmetic in the root record |
| `timelineSemaphore` | 1.2 | The retirement domain (§6.4) |
| `descriptorIndexing` | 1.2 | Gates the minimum descriptor-indexing set |
| `runtimeDescriptorArray` | 1.2 | Unbounded `Texture2D g_textures[]` declaration |
| `descriptorBindingPartiallyBound` | 1.2 | Sparse occupancy of the heaps |
| `shaderSampledImageArrayNonUniformIndexing` | 1.2 | Per-lane material indices |
| `shaderStorageImageArrayNonUniformIndexing` | 1.2 | Per-lane storage-image indices in IBL bake |
| `scalarBlockLayout` | 1.2 | Makes the SPIR-V layout of a mixed `uint64`/`vec4`/`mat4` record equal the C layout. Without it the ABI manifest fights std140/std430 padding |
| `hostQueryReset` | 1.2 | Reset timestamp pools without a command buffer, for per-pass timings |
| `dynamicRendering` | 1.3 | Removes `VkRenderPass`/`VkFramebuffer` and the twelve render-pass vtable entries |
| `synchronization2` | 1.3 | Barrier vocabulary that maps onto `VkrGpuDependency`; real `VK_PIPELINE_STAGE_2_NONE`; split transfer stages |
| `maintenance4` | 1.3 | `vkGetDeviceBufferMemoryRequirements` / `vkGetDeviceImageMemoryRequirements` — placement size and alignment **without creating a throwaway object**, the analogue of Metal's `heapBufferSizeAndAlignWithLength:` |
| `maintenance5` | 1.4 | `VkBufferUsageFlags2CreateInfo`; `vkCmdBindIndexBuffer2` for size-bounded index binding, matching Metal's `indexBufferLength:` |
| `VK_EXT_descriptor_buffer` with `descriptorBuffer == VK_TRUE` | extension | The global texture heap (§4). No fallback |
| One queue family with graphics + compute + transfer (+ present when windowed) | — | Keeps the scalar submit value a valid total order (§6.5) |
| `VK_KHR_surface` + `VK_KHR_win32_surface` + `VK_KHR_swapchain` | — | Windowed targets only; dropped for offscreen (§1.2) |

**Verification boundary.** Several of these features are promoted into core
Vulkan and some are mandatory-to-support at some core version. The
authoritative per-version mandatory tables in the specification's Feature
Requirements section could not be retrieved in full when this document was
written, so **this design deliberately does not depend on any of them.** Every
required entry is queried and rejected against as though it were optional. That
is what the code must do regardless: the specification is explicit that a
supported feature must still be *enabled* at device creation or device creation
fails with `VK_ERROR_FEATURE_NOT_PRESENT`.

What was verified locally is recorded in §12. In particular, do **not** write
"Vulkan 1.4 makes everything except descriptor buffer mandatory" into this
document or ADR-023 without checking the Feature Requirements section directly.
The versions appendix does not list `descriptorIndexing`, its sub-features, or
`shaderInt64` among the features newly required in 1.4.

### 3.3 Limit floor

Rejected against, not merely recorded:

- `maxPerStageDescriptorSampledImages` and `maxDescriptorSetSampledImages` ≥
  configured sampled-image heap capacity;
- `maxPerStageDescriptorSamplers` ≥ sampler heap capacity;
- `maxPerStageDescriptorStorageImages` ≥ storage-image heap capacity;
- `maxPushConstantsSize` ≥ the reserved root range (§7.1);
- `maxBoundDescriptorSets` ≥ 2;
- each descriptor-set layout size ≤ `maxResourceDescriptorBufferRange` /
  `maxSamplerDescriptorBufferRange`;
- combined heap bytes ≤ `descriptorBufferAddressSpaceSize`, and each buffer
  within its own `resourceDescriptorBufferAddressSpaceSize` /
  `samplerDescriptorBufferAddressSpaceSize`;
- `maxDescriptorBufferBindings` ≥ 2.

Recorded but not rejected against: `bufferImageGranularity`,
`nonCoherentAtomSize`, `minMemoryMapAlignment`,
`optimalBufferCopyOffsetAlignment`, `optimalBufferCopyRowPitchAlignment`,
`timestampComputeAndGraphics`, `timestampPeriod`, and the whole of
`VkPhysicalDeviceDescriptorBufferPropertiesEXT`.

### 3.4 Rejection report

Fixed-capacity and allocation-free, emitted **per candidate physical device** so
a two-GPU machine explains both:

```text
VkrVkCapabilityReportEntry { kind, name, required, present, detail }
  kind ∈ { API_VERSION, INSTANCE_EXTENSION, DEVICE_EXTENSION,
           FEATURE, LIMIT, QUEUE, FORMAT }
```

Each device header records `deviceName` and, from
`VkPhysicalDeviceDriverProperties`, `driverID`, `driverName`, `driverInfo`, and
`conformanceVersion`. That group is not decoration: descriptor-buffer defects
are driver-version specific, and this is the only field set that makes a remote
Windows bug report actionable. Initialization terminates with
`VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED`. The report is exposed through the
harness so a failing runner emits a machine-readable capability row rather than
an opaque device error.

### 3.5 Optional capabilities

The rule: an optional capability is **recorded unconditionally at startup**, and
a code path may **consume** it only after a named harness case demonstrates the
improvement. Until then the field exists, is reported, and nothing reads it.
There is no hidden fork.

| Optional | Gate before any consuming code lands |
|---|---|
| `VK_KHR_unified_image_layouts` (absent from 1.4.313) | Deterministic readback parity with layout tracking disabled, plus a Release A/B on the same device. Layout lowering stays in the code either way |
| `VK_EXT_mesh_shader` | Indexed vertex pulling stays authoritative per [ADR-013](adr/013-draw-submission-strategy.md). A mesh path must measure better on identical case, resolution, and scene in Release |
| `VK_EXT_device_generated_commands` | Only after CPU draw-record time is measured as a bottleneck in a Release profile |
| `VK_EXT_shader_object` | Only if cold pipeline creation or state-permutation count is measured as a problem. It conflicts with pipeline caches, so `validate_pipeline_cache.sh`'s cold/warm evidence needs a replacement first |
| `VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_binary` | Same trigger, measured against the same cold/warm numbers |
| `hostImageCopy` (1.4 core feature, explicitly optional to support) | Removes the staging copy for texture upload. Gate on measured load time |
| `descriptorBufferCaptureReplay` | **Not a performance feature.** Enable in Debug and diagnostic configurations when present so RenderDoc and PIX can capture; never in Release, because it constrains driver allocation |

---

## 4. Descriptor buffers as the global texture heap

### 4.1 The property that makes this workable

**Backend-defined descriptor sizes never reach the shader.**

Slang declares `Texture2D g_textures[]` and indexes it with a 32-bit integer.
The driver, not the application, turns that index into `base + index * stride`.
The application computes byte offsets only when *writing* a descriptor,
host-side. `sampledImageDescriptorSize` varying across drivers therefore changes
exactly one line of host code and zero bytes of shader ABI.

This is why `VK_EXT_descriptor_buffer` is a workable global texture heap despite
its backend-defined sizes, and it is why the umbrella spec's earlier framing —
that descriptor buffers "still retain descriptor layout/mapping rules and
backend-defined sizes," listed as a drawback — understates the containment. The
rule is stated here and repeated in ADR-023 because every other decision in this
section depends on it.

### 4.2 Two buffers, three bindings

Two descriptor buffers, not one, because the specification segregates sampler
and resource descriptors into buffers with distinct usage bits and drivers
report their address-space sizes separately. Two, not three, so
`maxDescriptorBufferBindings` is never a constraint.

| Buffer | Usage bits | Set | Bindings |
|---|---|---|---|
| Resource | `RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT \| SHADER_DEVICE_ADDRESS` | 0 | `SAMPLED_IMAGE[N_tex]`, `STORAGE_IMAGE[N_storage]` |
| Sampler | `SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT \| SHADER_DEVICE_ADDRESS` | 1 | `SAMPLER[N_smp]` |

**Separate `SAMPLED_IMAGE` and `SAMPLER`, never `COMBINED_IMAGE_SAMPLER`.**
Three reasons, the third of which is a correctness trap rather than a
preference:

1. The Metal row already separates four texture references from four sampler
   references, so the CPU lowering stays symmetric across backends.
2. The sampler domain is small and fully cacheable; the Metal path already
   covers the complete canonical key domain rather than the live texture count.
3. `combinedImageSamplerDescriptorSingleArray == VK_FALSE` on some drivers means
   a combined array is split into two non-contiguous sub-arrays, making index
   arithmetic driver-dependent. Avoiding combined descriptors avoids the
   variance entirely.

**Buffers are not descriptors.** Vertex, index, instance, material, light table,
light mask grid, shadow cascade, IBL probe, and text-vertex arrays are all
reached by device address, exactly as on Metal. Storage buffers are never
descriptors. This keeps the descriptor buffers to a few hundred kilobytes and
makes "global texture heap" the only descriptor concept in the renderer, which
is what keeps the seam narrow.

### 4.3 Layout derivation and binding cadence

At initialization, after the profile is captured:

1. Create both `VkDescriptorSetLayout`s with
   `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`.
2. `vkGetDescriptorSetLayoutSizeEXT` gives total bytes;
   `vkGetDescriptorSetLayoutBindingOffsetEXT` gives each binding's base offset.
3. Freeze one immutable record per binding —
   `{ base_offset, stride, capacity }` — with `stride` taken from
   `sampledImageDescriptorSize`, `samplerDescriptorSize`, or
   `storageImageDescriptorSize`. **Never a literal.**
4. Publication is one `vkGetDescriptorEXT` writing into
   `mapped_base + base_offset + (uint64_t)index * stride`. No API object is
   created; it is a blob write into persistently mapped host-visible memory.

Binding happens **once per command buffer** at recording start:
`vkCmdBindDescriptorBuffersEXT` for both buffers, then one
`vkCmdSetDescriptorBufferOffsetsEXT` for both sets at offset 0. Never per pass,
never per draw. Because the bound offsets are always zero,
`descriptorBufferOffsetAlignment` is trivially satisfied at the command level;
it is enforced instead on the descriptor buffers' own suballocation alignment as
`max(descriptorBufferOffsetAlignment, memoryRequirements.alignment)`, asserted
once at initialization.

### 4.4 Capacity, sentinel, and the two-slot rule

**The heap never grows.** Growth would invalidate every published 32-bit index
and every in-flight submission's descriptor-buffer address. Exhaustion is an
explicit, metered failure, exactly as the Metal material table reports capacity
exhaustion rather than silently substituting or reallocating.

**Slot 0 is a sentinel.** Each heap's slot 0 holds a valid descriptor — a 1×1
opaque-white image, a flat-normal image, a default sampler. A material without a
normal map still resolves to a valid descriptor. `descriptorBindingPartiallyBound`
makes never-accessed slots legal, but proving non-access is harder than filling a
sentinel. Keep the material `flags` bit for shader branching, matching Metal, **and**
fill the sentinel, so a flags defect is a wrong pixel rather than a device loss.

**The two-slot rule.** A texture that is storage-written in one pass and sampled
in another — every IBL bake target — occupies **one slot in the storage-image
heap and one in the sampled-image heap, over the same `VkImageView`.** This
falls directly out of descriptor buffers being plain memory: when
`descriptorBufferImageLayoutIgnored == VK_FALSE`, a descriptor is baked with a
specific `VkImageLayout` and the image must be in that layout when accessed —
`GENERAL` for the storage write, `SHADER_READ_ONLY_OPTIMAL` for the sample.

This is where a naive port breaks, and it is where
`VK_KHR_unified_image_layouts` would later collapse the two slots into one.

### 4.5 The material row

```c
/* 64 bytes, 16-byte aligned — one cache line. */
typedef struct VkrVkMaterialGpuRow {
  float32_t tint[4];           /* 16 @  0  — identical to Metal              */
  uint32_t  base_color_texture;/*  4 @ 16                                    */
  uint32_t  normal_texture;    /*  4 @ 20                                    */
  uint32_t  orm_texture;       /*  4 @ 24                                    */
  uint32_t  emissive_texture;  /*  4 @ 28                                    */
  uint32_t  base_color_sampler;/*  4 @ 32                                    */
  uint32_t  normal_sampler;    /*  4 @ 36                                    */
  uint32_t  orm_sampler;       /*  4 @ 40                                    */
  uint32_t  emissive_sampler;  /*  4 @ 44                                    */
  uint32_t  material_id;       /*  4 @ 48  — Metal semantics, Metal offset 80 */
  uint32_t  flags;             /*  4 @ 52  — Metal semantics, Metal offset 84 */
  uint32_t  reserved[2];       /*  8 @ 56                                    */
} VkrVkMaterialGpuRow;
```

`VkrMetalMaterialGpuRow` is 96 bytes because its eight resource-ID payloads are
64-bit. The Vulkan row narrows those eight fields to 32-bit heap indices: 64
bytes, one cache line, a third less material-row bandwidth.

**The rows are deliberately not unified.** This is precisely the "denser natural
representation" that [ADR-022](adr/022-gpu-pointer-resource-model.md)
anticipated for Vulkan and explicitly refused to force onto Metal — its
"standardize every backend on a 64-bit texture token" alternative is recorded as
rejected. Unifying would require either Metal carrying 32-bit texture-view-pool
indices, which Stage 3 left unproven and whose Slang lowering is broken anyway,
or Vulkan carrying 32 bytes of padding for nothing. `VkrMaterial` remains the
single CPU authority; the ABI manifest simply gains a second record family. That
is a lowering, not a second authority.

### 4.6 Publication, immutability, retirement

The protocol is identical to Metal because the code path *is* the shared slot
table from [ADR-024](adr/024-shared-bindless-gpu-cores.md):

1. Acquire descriptor-heap slots for newly referenced image views and samplers.
2. Fill a **new** material row in a free slot of the material buffer, an
   `UPLOAD`-class suballocation reached by device address.
3. Publish the new row, then retire the old one against its
   `last_use_submit_value`.
4. Collect on completion, returning both the row slot and the descriptor slots.

Two Vulkan-specific notes:

- The immutability rule is **stricter** than under descriptor sets. A descriptor
  buffer is plain memory read by the GPU at execution time; there is no
  update-after-bind window and no driver-side copy. An in-place overwrite while a
  submitted command buffer can read it is a data race with no repair.
- `allowSamplerImageViewPostSubmitCreation` says whether the `VkSampler` or
  `VkImageView` *object* may be created after the descriptor is recorded. It does
  **not** relax the memory hazard above. This is an easy misread and is recorded
  so it is not made.
- Host writes to `HOST_COHERENT` descriptor memory become visible at queue
  submission; no barrier is needed.
  `VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT` exists for GPU-written
  descriptors, which this design does not use. Recorded as unused.

### 4.7 Metrics

New rows in the bindless family alongside the existing
`memory.gpu.suballocations.*`, per
[ADR-015](adr/015-metrics-module.md)'s pre-registered-slot rule:

```text
memory.gpu.descriptors.sampled_image.{live,peak,capacity,published,retired,collected}
memory.gpu.descriptors.sampled_image.failures.capacity
memory.gpu.descriptors.sampler.*            (same shape)
memory.gpu.descriptors.storage_image.*      (same shape)
memory.gpu.materials.*                      (from the shared slot table)
```

The legacy `memory.gpu.allocations.*` device-allocation meaning is untouched.

---

## 5. Memory

### 5.1 No VMA

Rejected, with the reasons on the record: the extracted core is already a tested
range allocator with generation handles, separate reserved and resource offsets,
alignment-waste accounting, exhaustion classification, retirement records, and
per-class metrics; VMA is C++ while `vkr_require_declared_c_functions()` gates
this codebase to C11; [ADR-006](adr/006-cpu-memory-allocators.md) and
[ADR-007](adr/007-gpu-memory-allocation.md) own allocator policy; and the point
of the extraction is *one* allocator with *one* metrics model across both
backends, which adding VMA would undo.

Revisit trigger: the core's fragmentation and largest-free-range metrics show
churn that a binning allocator would fix.

### 5.2 Block topology

One shared allocator core instance per `(class, kind)` pair, each backed by one
`VkDeviceMemory` allocated with
`VkMemoryAllocateFlagsInfo{ VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT }`.

| Class | Memory property search, in order | Contents |
|---|---|---|
| `DEVICE` | `DEVICE_LOCAL` and not `HOST_VISIBLE` | Placement images; persistent vertex, index, and material buffers |
| `UPLOAD` | `HOST_VISIBLE\|HOST_COHERENT\|DEVICE_LOCAL` (resizable BAR), else `HOST_VISIBLE\|HOST_COHERENT` | Root records, frame streams, light tables, **both descriptor buffers**, staging |
| `READBACK` | `HOST_VISIBLE\|HOST_CACHED\|HOST_COHERENT`, else `HOST_VISIBLE\|HOST_CACHED` | Capture, picking, query results |

**`bufferImageGranularity` is handled by segregation, not padding.** One block
per `(class, kind)`, where `kind` is the buffer-versus-texture parameter the core
already carries, means buffers and optimal-tiling images never share a
`VkDeviceMemory` and the granularity hazard cannot occur. Metal's adapter
collapses `(class, kind)` onto its single placement heap; Vulkan's keeps them
separate. Same core, different adapter policy — which is what keeps a
Vulkan-only concept out of shared code. If padding is ever needed anyway, it
must be counted into the existing alignment-waste row rather than hidden.

### 5.3 Placement

`vkGetDeviceBufferMemoryRequirements` and `vkGetDeviceImageMemoryRequirements`
yield size and alignment from the create-info **without creating an object** —
the exact analogue of Metal's `heapBufferSizeAndAlignWithLength:`. The core then
returns a placement, and the adapter creates the object and binds it at
`placement.resource_offset`.

### 5.4 Addresses and mapping

- Device address: `vkGetBufferDeviceAddress` once at creation, cached in the
  allocation record beside the `VkBuffer`. Stable for the allocation's lifetime.
  Never recomputed per frame and never per draw.
- Mapping: each host-visible block is mapped **once, persistently, at block
  creation**. A suballocation's CPU pointer is
  `mapped_base + placement.resource_offset`. Never map per resource.
- Non-coherent memory: flush the written range rounded to `nonCoherentAtomSize`
  **once per ring slice at submit time**, not per write.

### 5.5 Rings

The shared submit ring is already API-neutral: slot state, retire value, and
busy classification. The Vulkan adapter backs it with one persistently mapped
`UPLOAD` buffer and one `READBACK` buffer. Acquiring a slice returns both the
offset and the `{cpu, gpu, size}` address pair, matching the Metal contract
including its ring-busy behaviour.

### 5.6 One divergence: index buffers

Vulkan has no device-address index binding. Metal takes an index buffer address;
Vulkan takes `vkCmdBindIndexBuffer2(cmd, VkBuffer, offset, size, indexType)`.
Mesh allocations therefore retain a `VkBuffer` handle *in addition to* the
device address, whereas Metal retains its buffer object plus its address. The
shared allocation record already carries an opaque native-object slot, so this is
absorbed entirely by the device adapter.

---

## 6. Lifetime and the completion domain

### 6.1 The mapping from Metal

| Metal | Vulkan |
|---|---|
| Shared event plus monotonic submit value | One `VkSemaphore` created with `VkSemaphoreTypeCreateInfo{TIMELINE, initialValue = 0}` |
| Queue signal at submit | `vkQueueSubmit2` signalling the submit value at `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` |
| Event `signaledValue` poll | `vkGetSemaphoreCounterValue`, non-blocking |
| Wait idle | `vkWaitSemaphores(timeline, submit_value, UINT64_MAX)`. **Never `vkDeviceWaitIdle` in steady state** |
| Transient command allocator reset after completion | One `VkCommandPool` per frame slot, `vkResetCommandPool` only after the timeline proves that slot complete. Created `TRANSIENT`, without `RESET_COMMAND_BUFFER` |

Every retirable object caches `last_use_submit_value`; collection releases it
once the timeline passes that value. The pre-submit rollback that already exists
on the Metal path carries over verbatim: if submission fails or a pass fails to
record, provisional stamps are rolled back and ring slices cancelled.

### 6.2 Frames in flight and waits

The CPU waits only when it would otherwise exceed the configured frames in
flight or reuse a still-owned bounded slot. Frame-slot reuse waits are counted
into `frame.command_slot_waits`; upload ring waits into `upload.fence_waits`.
Per the umbrella spec these are not summed — the second is a subset concept, not
an addend.

### 6.3 Retirement obligations

Command pools, frame streams, upload and readback ring slices, descriptor-heap
slots, material rows, image views, placement ranges, and swapchain image views
all obey the same rule. An assumed frame lag is never completion proof.

### 6.4 WSI is the one place the timeline is not enough

`vkAcquireNextImageKHR` and `vkQueuePresentKHR` require **binary** semaphores.
So a windowed submit signals two things — the timeline at value N for
retirement, and a per-swapchain-image binary render-complete semaphore for
present — and waits on a per-frame-slot binary acquire semaphore.
`VkSubmitInfo2` takes arrays of `VkSemaphoreSubmitInfo`, so this remains one
submit.

This is a genuine divergence from Metal, where drawable presentation needs no
semaphore, and it lands on the sizing table that
[ADR-009](adr/009-frame-synchronization.md) and
[ADR-014](adr/014-offscreen-present-target.md) record as a historical defect
family in this repository. The invariants are therefore stated explicitly:

- acquire semaphores are sized by **frames in flight** and indexed by frame slot;
- render-complete semaphores are sized by the **actual swapchain image count
  returned by `vkGetSwapchainImagesKHR`**, not by the requested minimum, and are
  indexed by acquired image;
- `VkFence` is **eliminated entirely** — the timeline replaces both the per-frame
  submit fence and the image-in-flight fence references;
- image-in-flight tracking survives as `uint64_t image_last_submit_value[]`, with
  a timeline wait after acquire. This is strictly simpler than the fence-reference
  scheme it replaces.

### 6.5 Independent queues

The design stays **single queue** through V6, so the scalar submit value remains
a valid total order, matching Metal. The profile records whether a dedicated
transfer or compute family exists but does not create one.

Design-for-later without implementing: the shared core's retirement record
carries a `queue_index` field from day one, defaulted to zero, so adding a
second queue is a data change rather than a state-machine rewrite. A negative
test is required: the core must **never** compute a maximum over unrelated queue
serials. ADR-022 explicitly rejects that, and a shared core is exactly where
someone would accidentally implement it.

---

## 7. Shader ABI

### 7.1 Root delivery

Metal writes a 64-bit address into a bounded argument table per draw. **Vulkan
uses a push constant holding the root record's device address**, visible to
vertex and fragment stages (and compute for compute pipelines). Per draw that is
one `vkCmdPushConstants` of eight bytes — the cheapest per-draw state change
Vulkan offers, with no descriptor rebind, no offset change, no allocation, no
lock, and no string construction. It is the exact semantic analogue of the
argument-table address write.

Reserve a 16-byte push-constant range in the pipeline layout (address plus a
material index and flags) but populate only the address at first. The reserved
range is recorded in the ABI manifest, so widening it later is a manifest change
with a reflection gate rather than silent ABI drift.

Rejected alternatives, recorded so they are not relitigated:

- per-draw `vkCmdSetDescriptorBufferOffsetsEXT` — heavier, and it ties draw data
  to descriptor memory when the whole design keeps draw data on device addresses;
- one large root array indexed by draw index or first instance — viable later
  with multi-draw indirect or device-generated commands, but it changes the
  batching contract. Kept as a revisit trigger, not a starting point.

### 7.2 The draw root

`VkrVkPacketDrawRoot` mirrors `VkrMetalPacketDrawRoot` field for field in
*semantics*, with two changes:

- address fields stay `uint64_t` — vertices, instances, materials, point-light
  data, point-light masks, shadow cascades, IBL probes;
- texture-reference fields narrow to `uint32_t` heap indices, and each gains a
  paired `uint32_t` sampler index — irradiance, prefilter, BRDF LUT, shadow map,
  transmission source. Five 64-bit fields become ten 32-bit fields: the same
  bytes, five more usable slots.

**Stride is not portable.** Metal's 512-byte root stride at 256-byte alignment
exists because Metal requires 256-byte address alignment for a bound buffer
address. Vulkan has no such requirement for a record reached by device address;
the only constraint is the record's own alignment. Vulkan therefore uses
cache-line granularity so adjacent draw roots never share a line. The 512 and
256 figures are Metal facts and must not be copied as portable ones.

The byte-identical vertex-prefix trick — where the vertex stage consumes only a
prefix of the fragment root — carries over unchanged, with its own manifest
record.

### 7.3 Slang declarations

```slang
[[vk::binding(0, 0)]] Texture2D            g_textures[];
[[vk::binding(1, 0)]] RWTexture2D<float4>  g_storage_images[];
[[vk::binding(0, 1)]] SamplerState         g_samplers[];

[[vk::push_constant]] ConstantBuffer<VkrVkDrawRootAddress> g_root;

g_textures[NonUniformResourceIndex(row.base_color_texture)]
    .Sample(g_samplers[NonUniformResourceIndex(row.base_color_sampler)], uv);
```

Root and array records use Slang pointer types lowering to SPIR-V
`PhysicalStorageBuffer`, with `scalarBlockLayout` so the SPIR-V struct layout
matches the C layout.

### 7.4 Validation, in three layers minus one

**Layer 1 — durable host manifest.** The manifest machinery extracts to a shared
module per ADR-024: a record and field table with expected size, alignment, and
per-field offset, validated against `sizeof`, `_Alignof`, and `offsetof`. Each
backend keeps its own record table; the genuinely shared vertex, instance, and
text-vertex records become one shared table both consume. This runs on macOS.

**Layer 2 — shader-side static assertion: deliberately absent.** Metal has MSL
`static_assert` over its struct sizes. Slang has no portable compile-time
`sizeof` over a struct. Rather than fake parity, this design records that layer 2
does not exist on Vulkan and that layer 3 is correspondingly mandatory and
stronger — SPIR-V reflection exposes exact per-member offset, size, and padded
size, where the MSL assertion only gave total size.

**Layer 3 — SPIR-V reflection cross-check, online and offline.** The existing
SPIRV-Reflect wrapper under `lib/src/renderer/vulkan/` is the starting point and
is one of the modules ADR-026 relocates rather than deletes. At pipeline
creation the check resolves the push-constant block and asserts size and offset;
resolves descriptor bindings and asserts set and binding numbers, descriptor
types, and runtime-sized arrays; **recurses into `PhysicalStorageBuffer` pointer
element types** and compares each struct's size and member offsets to the
manifest — the direct analogue of Metal's pointer-element recursion; and rejects
the pipeline before any frame is encoded.

**Named risk with a named fallback.** SPIRV-Reflect enumerates descriptor
bindings and push constants; blocks reached through `PhysicalStorageBuffer`
forward pointers may appear in neither list. Metal got this for free from its
reflection API. If SPIRV-Reflect does not expose them, the fallback is a small
purpose-built SPIR-V walker over `OpTypeStruct` and `OpMemberDecorate Offset`
for the named blocks. **This is a V0 gate item**, because it is the single
highest-risk piece of the validation story.

The same reflection check also runs **offline in the CPU suite against built
`.spv` artifacts**. For a backend that cannot execute on the development
machine, this is the most valuable locally executable gate that exists.

### 7.5 The Slang risk, correctly framed

**Slang needs to know nothing about `VK_EXT_descriptor_buffer.`** Descriptor
buffer is entirely a host- and driver-side change in how the *same* SPIR-V
descriptor bindings are backed. The SPIR-V emitted for a bindless texture array
is identical whether the application uses descriptor pools or descriptor
buffers. "Slang descriptor-buffer support is unproven" is therefore not a real
risk; there is nothing to support.

What is genuinely unproven and must be spiked in V0:

1. Slang's `PhysicalStorageBuffer` code generation for a large mixed
   `uint64`/`vec4`/`mat4` record under `scalarBlockLayout`, matching C offsets
   exactly;
2. whether SPIRV-Reflect exposes those blocks at all (§7.4).

The Metal blocker — Slang 2025.7.1 aborting on a structured-buffer element
containing texture resource fields, which forced hand-written MSL — **has no
analogue here**, because the Vulkan row carries `uint32` indices rather than
texture objects. This is a real advantage of the Vulkan lowering: the bindless
Vulkan path may be able to be pure Slang where Metal could not.

Revisit trigger: if Slang's device-address code generation cannot be validated,
the fallback is authoring bindless shaders in GLSL with buffer-reference
extensions. That forks the shader language across backends and must be an
**owner decision**, never a silent fallback.

### 7.6 Shader source ownership

A new tree under `lib/src/renderer/vulkan_bindless/shaders/` with the same domain
split the umbrella spec defines — `common`, `world`, `shadow`, `picking`,
`text`, `skybox`, `ibl`, `post` — compiled by a new CMake rule into a **separate
output namespace** so the fifteen existing Vulkan outputs and their `.shadercfg`
manifests are untouched throughout migration.

Do **not** reuse the legacy `vulkan/shaders/common/*.slangh` headers. The
umbrella spec already records that the transform, instance, alpha-cutout, cube,
cascade, and tonemap headers encode descriptor-era reuse. They are deleted with
the legacy path at ADR-026 step 1.

---

## 8. Graph and barrier lowering

A new lowerer consumes the **same** compiled records the graph already produces —
the image barrier with its access pair, layout pair, canonical dependency, and
subresource range; the buffer barrier with its access pair and dependency; and
the canonical `VkrGpuDependency` itself. **Zero graph changes are required.**
That is the payoff of the umbrella spec's Stage 4 and should be read as a
validation of ADR-022's fourth decision.

### 8.1 Stages

| Canonical stage | Synchronization2 | Versus the legacy lowerer |
|---|---|---|
| `NONE` | `VK_PIPELINE_STAGE_2_NONE` | Legacy substituted top/bottom of pipe — a real correctness improvement |
| `ALL_GRAPHICS` | `ALL_GRAPHICS` | Same |
| `DRAW_INDIRECT` | `DRAW_INDIRECT` | Same |
| `VERTEX_INPUT` | `INDEX_INPUT \| VERTEX_ATTRIBUTE_INPUT` | Split, therefore narrower |
| `VERTEX_SHADER`, `FRAGMENT_SHADER` | Direct equivalents | Same |
| `EARLY_DEPTH`, `LATE_DEPTH` | `EARLY_FRAGMENT_TESTS`, `LATE_FRAGMENT_TESTS` | Same |
| `COLOR_OUTPUT` | `COLOR_ATTACHMENT_OUTPUT` | Same |
| `COMPUTE_SHADER` | `COMPUTE_SHADER` | Same |
| `TRANSFER` | `COPY \| BLIT \| RESOLVE \| CLEAR` | Split, therefore narrower |
| `HOST`, `TOP`, `BOTTOM` | Direct equivalents | Same |

Access lowering mirrors the legacy access mapping with `VkAccessFlags2` bits.

### 8.2 Visibility

- Device visibility present emits non-zero source and destination access masks,
  making it a memory dependency; absent emits an execution-only dependency with
  both masks zero. Synchronization2 makes this distinction expressible and
  testable, and it is the exact counterpart of Metal's visibility options.
- Resource-alias visibility maps to Vulkan's aliasing rule — the newly
  interpreted image transitions from `UNDEFINED` plus a full memory barrier.
  **The graph does not currently alias placement ranges.** Until it does, the
  lowerer must **reject** this flag with a named error rather than silently
  ignoring it. Silent ignoring is exactly the anti-pattern ADR-022 warns about.

### 8.3 Emission

- **Images:** one `VkImageMemoryBarrier2` per compiled image barrier, carrying
  stages, accesses, old and new layout, image, and subresource range. The
  canonical subresource range maps directly through the existing resolve helper.
- **Buffers:** collapse all of a pass's buffer barriers into **one
  `VkMemoryBarrier2`**. With a single queue family there is no ownership
  transfer, so per-buffer barriers provide no benefit on any known desktop
  driver, and the graph retains resource identity for diagnostics regardless.
  The umbrella spec's rule — a global barrier only where the specification
  permits it and measurement shows it is not materially worse — is satisfied by
  construction for buffers without queue transfer. The measurement caveat would
  apply if images were also collapsed, which they are not.
- **One `vkCmdPipelineBarrier2` per pass boundary**, batching everything. The
  graph already batches into per-pass pre-image and pre-buffer barrier lists plus
  the terminal list. Barrier generation must not allocate in steady state; that
  invariant is enforced by the graph's reused subresource-state arrays and
  carries over unchanged.

### 8.4 Dynamic rendering

Each graph pass becomes `vkCmdBeginRendering` with color and depth attachment
infos carrying image view, layout, load and store ops, and clear value, then
`vkCmdEndRendering`.

Consequences:

- the graph's render-pass handle, render-target array, render-target cache
  entries, and render-pass hash map become **dead for this backend**. They
  survive only for legacy Vulkan 1.2 during migration and are deleted at
  ADR-026 step 5.
- Pipelines take `VkPipelineRenderingCreateInfo` with color and depth attachment
  formats instead of a render pass, so pipeline creation depends on
  graph-declared formats rather than a cached render-pass object. Render-pass
  signature compatibility checking disappears with it.
- **Barriers must be emitted outside the rendering scope.**

### 8.5 One asymmetry worth recording

Metal models three barrier forms — producer, consumer, and intra-encoder. **The
Vulkan lowerer needs only producer and consumer; the intra-encoder form has no
general Vulkan analogue.** This is precisely the kind of difference a
speculative shared low-level GPU vtable would have gotten wrong, and it is the
concrete evidence for ADR-025's decision not to build one.

### 8.6 Layouts

The graph already tracks layout per subresource and produces source and
destination layouts. The lowerer maps them to the core layout set, including the
separate depth and stencil layouts available since Vulkan 1.2.

Descriptor-buffer interaction is the two-slot rule of §4.4: sampled-image slots
are baked with `SHADER_READ_ONLY_OPTIMAL`, storage-image slots with `GENERAL`.

With `VK_KHR_unified_image_layouts` — absent from 1.4.313, therefore currently
unselectable — internal transitions collapse to `GENERAL`, and the two-slot rule
collapses to one slot. Exceptions survive: initialization from `UNDEFINED`,
present, and external or video ownership. The lowerer keeps the full layout
state machine either way and selects the unified path behind one profile
boolean; the graph is unchanged in both cases.

### 8.7 The three lowerers

| | Legacy Vulkan 1.2 | Metal 4 | Bindless Vulkan 1.4 |
|---|---|---|---|
| Vocabulary | `VkPipelineStageFlags` / `VkAccessFlags` | Metal stages and visibility options | `VkPipelineStageFlags2` / `VkAccessFlags2` |
| Resource identity in the command | image plus range | discarded | image plus range |
| Layouts | yes | none | yes, or `GENERAL` under unified layouts |
| `NONE` stage | substitutes top/bottom | all stages | real stage-none |
| Transfer granularity | one transfer bit | blit stage | split copy/blit/resolve/clear |
| Buffer barriers | per-buffer | none | one merged memory barrier per pass |
| Encoder scoping | render-pass restrictions | producer, consumer, intra | producer, consumer |
| Emission | per pass | per pass | one barrier command per pass |

### 8.8 The locally executable gate

Split mask computation from command recording: lowering a compiled image barrier
into a `VkImageMemoryBarrier2` must be a **pure function**, testable without a
device. The existing render-graph barrier test then gains a bindless lowering
table pinning the synchronization2 masks for the same canonical records it
already pins for the legacy lowerer. This runs on macOS and is a real gate for a
backend that cannot execute there.

---

## 9. The implementation seam

The decision, the branch-site taxonomy, and the concrete shape live in
[ADR-025](adr/025-selected-renderer-implementation-strategy.md). What matters
for this document is the contract this backend must satisfy and the hot-path
guarantee it must preserve.

The backend exposes a narrow surface modelled on the Metal packet renderer's:
coarse lifecycle, `prepare_frame`, `submit_packet`, wait and completion values,
drain, destroy, an asset-publisher table, capture poll and release, pass-timing
polls, memory metrics, and wait counters. Everything else — device, queues,
memory, descriptor heaps, pipelines, recording, presentation, residency — is
private.

**Hot-path guarantee.** After initialization the frame path executes exactly two
indirect calls per frame. Inside `submit_packet` the backend is direct-typed.
There is no per-pass indirect call, no per-draw indirect call, no backend-type
test, no allocation, no lock, and no string construction. A source audit proving
this is required evidence in every stage from V3 onward, mirroring what the
Metal walking slice did.

---

## 10. Presentation and platform

### 10.1 The platform seam

The existing Vulkan platform seam is five functions parameterized on the legacy
backend state type. Rather than duplicating it, the bindless path parameterizes
it on primitives instead — instance, window handle, allocation callbacks, and an
out surface, plus the instance and device extension lists and the
"is this a surface extension" classifier.

One Windows implementation then serves **both** backends during migration and
serves Linux later without a third copy. This is a genuine compression, and it
is why ADR-026 records the legacy platform files as superseded at V3 rather than
deleted at the end.

### 10.2 Swapchain

- **Present mode:** FIFO by default, mailbox when available. Present mode
  dictates frame pacing and therefore every performance number, so it is
  recorded in the capability profile **and** in harness reports.
- **Image count:** clamped from the surface capabilities, and the arrays are
  sized by the **actual count returned by `vkGetSwapchainImagesKHR`**, not by
  the requested minimum. ADR-014 records the defect family this avoids.
- **Format:** prefer an sRGB BGRA surface format with nonlinear sRGB color
  space, matching what the Metal path reports for window targets; offscreen
  targets use an sRGB RGBA format.
- **Usage:** color attachment plus transfer destination. Capture reads the
  offscreen scene-color copy, never the swapchain.
- **Offscreen target:** a `DEVICE`-class placement image with color attachment,
  transfer source, and sampled usage — identical in role to Metal's.

### 10.3 Resize, and what Linux would add

Out-of-date or suboptimal results from acquire or present mark the target dirty.
Recreation happens at the next `prepare_frame` boundary after a timeline wait —
full idle of *our own* work, never a device-wide idle. The old swapchain is
passed as the retire handle; old image views retire against the last submit
value through the shared retirement core. A zero extent returns the existing
frame-skipped error, matching the Metal branch. Graph-owned placement images
recreate through the same cache path Metal already uses.

**Linux, named but not designed.** It requires five new files under
`lib/src/platform/` — window, platform, filesystem, threads, and gamepad — none
of which has any current analogue; a third arm in every `if(APPLE)/elseif(WIN32)`
CMake block, starting with the source glob and compile definitions; a Linux
Vulkan platform implementation for xcb and Wayland surfaces chosen at runtime
from the session type, both declared optional and classified by the surface
classifier; and the entire Windows evidence matrix repeated on Linux hardware.
The POSIX build and validation scripts already work. Explicit non-goals for that
stage: fractional scaling, present timing, and gamepad hotplug.

---

## 11. Staged implementation

Each stage is a vertical slice. A header that compiles without a caller is not a
stage. **Every runtime gate requires Windows hardware (§1).**

| Stage | Deliverable | Required evidence | Runs on |
|---|---|---|---|
| **V0 — capability and toolchain spike** | Standalone Windows executable: enumerate devices, build and print the capability profile and rejection report; create a 1.4 device with the full floor; create both descriptor buffers; publish one sampled image and one sampler; allocate one `UPLOAD` buffer and capture its device address; compile a Slang→SPIR-V pair with a push-constant root address, a `PhysicalStorageBuffer` vertex array, and one non-uniform texture sample; one indexed textured offscreen draw; exact readback; timeline signal and wait | Validation layers clean, including synchronization and GPU-assisted; exact RGBA8; profile printed; **SPIRV-Reflect `PhysicalStorageBuffer` recursion proven or the fallback walker written**; Slang scalar-layout offsets match `offsetof` | Windows, plus macOS for the offline reflection half |
| **V1 — shared-core extraction** | The four extractions of ADR-024; Metal keeps its device adapters and its own ABI record table; tests renamed | CPU suite green; **the shipping Metal harness snapshot byte-identical before and after**; Metal API and GPU validation clean; a Release Metal profile showing no frame-time regression | macOS |
| **V2 — implementation seam** | ADR-025's capability struct and coarse strategy, three implementations (Metal, legacy-Vulkan adaptor, bindless stub that fails initialization); all branch sites replaced; the neutral submit result replaces the untyped timing pointer | CPU suite green; Metal snapshot byte-identical; legacy Vulkan Debug startup, resize, and shutdown validation-clean on both platforms; source audit proving no backend-type test outside the factory; Release Metal profile flat | macOS and Windows |
| **V3 — walking bindless renderer** | Device, queue, timeline, command pools; Vulkan adapters for the shared memory core; descriptor heaps; offscreen and windowed targets; one indexed textured draw through the real prepare and submit path; resize | Validation clean windowed and offscreen; deterministic offscreen readback and exact identifier capture; resize across two extents; source audit and runtime counters proving no per-draw allocation, lock, string, or dispatch; wait counters publishing | Windows |
| **V4 — memory, materials, descriptor heaps** | Material row publication through the shared slot table; texture, sampler, and storage-image publish, replace, and retire; sentinel slot; two-slot rule; capacity reporting; the full bindless memory metric family; asset publisher wired to the shared loaders | Multi-material capture exact; texture replacement while frames are pending; capacity-exhaustion metric fires; repeated create, submit, and destroy returns every logical total to its initial value; validation clean | Windows |
| **V5 — graph, sync2, dynamic rendering, pass parity** | The bindless dependency lowerer; dynamic rendering for every authored pass; shadow cascades, skybox, opaque, transmission, blend, picking, tonemap, editor and UI, text, the full IBL bake, capture overlay, per-pass timestamps | The same declared five-channel capture batch as Metal returning exact final color, HDR scene color, depth, shadow layer, and picking identifiers; analytical IBL checks across irradiance and every prefilter mip; **synchronization validation clean across the whole authored graph**; deterministic repetitions; the CPU barrier-lowering table test | Windows, plus macOS for the CPU half |
| **V6 — feature parity and Windows baseline** | Application and harness backend selection; pipeline cache cold and warm; asset load and unload; metrics parity | ADR-021's Gate-B functional checklist on Windows; a Windows Bistro-plus-text baseline bootstrapped under the umbrella spec's seven-step policy, with report path and digest recorded; pipeline-cache and backend-matrix scripts; an **authoritative Release performance profile against legacy Windows Vulkan 1.2 on identical cases** | Windows |
| **V7 — legacy retirement** | Per [ADR-026](adr/026-vulkan-1-2-retirement.md) | Per ADR-026's gates B1 and B2 | Windows and macOS |

Optional capabilities are deliberately outside this ladder. Each is its own
measured change after V6, per §3.5.

**Locally executable coverage** — the complete set of gates that can run without
Windows hardware, and therefore the ceiling on local iteration: all of V1 and
V2; the host ABI manifest gate; the offline SPIR-V reflection check against
built artifacts; the pure barrier-lowering table tests; and compile-only builds.
Everything else needs Windows.

---

## 12. Reproducible observations

These commands reproduce the document-level observations only. They are
capability and environment facts, not measurements.

```sh
# Installed SDK: which extensions exist in the headers.
sdk="$VULKAN_SDK/include/vulkan/vulkan_core.h"
for e in VK_EXT_descriptor_buffer VK_EXT_descriptor_heap \
         VK_KHR_unified_image_layouts VK_EXT_mesh_shader \
         VK_EXT_device_generated_commands VK_EXT_shader_object; do
  grep -q "define $e 1" "$sdk" && echo "PRESENT $e" || echo "ABSENT  $e"
done

# Vulkan 1.4 core feature membership, and the descriptor-buffer contract.
awk '/typedef struct VkPhysicalDeviceVulkan14Features/,/Vulkan14Features;/' "$sdk"
awk '/typedef struct VkPhysicalDeviceDescriptorBufferFeaturesEXT/,/FeaturesEXT;/' "$sdk"
awk '/typedef struct VkPhysicalDeviceDescriptorBufferPropertiesEXT/,/PropertiesEXT;/' "$sdk"

# Extension dependency, from the registry shipped with the SDK.
grep -o '<extension name="VK_EXT_descriptor_buffer"[^>]*>' \
  "$VULKAN_SDK/share/vulkan/registry/vk.xml"

# The local runtime, which cannot run this backend.
vulkaninfo --summary

# The branch ladder ADR-025 removes.
grep -rn "VKR_RENDERER_BACKEND_TYPE_METAL" lib/ app/ tools/ tests/ | wc -l

# The four extraction candidates carry no Metal types.
grep -n "MTL\|@interface\|id<" \
  lib/src/renderer/metal/vkr_metal_memory.c \
  lib/src/renderer/metal/vkr_metal_material_table.c \
  lib/src/renderer/metal/vkr_metal_capture_ring.c \
  lib/src/renderer/metal/vkr_metal_packet_abi.c
```

Observed on 2026-08-08 with SDK 1.4.313:

- `VK_EXT_descriptor_buffer` is present in the headers;
  `VK_EXT_descriptor_heap` and `VK_KHR_unified_image_layouts` are **absent**.
- `VkPhysicalDeviceVulkan14Features` contains `maintenance5`, `maintenance6`,
  `dynamicRenderingLocalRead`, `indexTypeUint8`, `hostImageCopy`,
  `pushDescriptor`, `shaderFloatControls2`, `shaderExpectAssume`,
  `shaderSubgroupRotate`, the line-rasterization features, the
  vertex-attribute-divisor features, `globalPriorityQuery`,
  `pipelineProtectedAccess`, and `pipelineRobustness`.
- `VK_EXT_descriptor_buffer` declares its dependency as satisfied by core
  Vulkan 1.3 or by the buffer-device-address, descriptor-indexing, and
  synchronization2 extension set. Dependency satisfaction is **not** feature
  support: the descriptor-indexing feature bits must still be queried.
- The local runtime is MoltenVK on an Apple M1 Pro reporting
  `apiVersion 1.2.296`, without `VK_EXT_descriptor_buffer`.
- `slangc` reports 2025.7.1.
- The metal-backend branch ladder is 46 sites, 44 of them under `lib/`.
- The four extraction candidates contain no Metal type references outside
  comments.

**Not verified:** the specification's per-version mandatory-support tables. See
the verification boundary in §3.2 before writing any claim that depends on them.

---

## 13. Risks and revisit triggers

### 13.1 No local development loop

The highest-impact process risk. Every runtime defect costs a Windows round
trip. Mitigations in priority order: maximize locally executable coverage
(§11); keep the offscreen target headless so a display-less Windows runner
executes the whole functional matrix; and establish remote build and harness
invocation on Windows early, because a long round trip changes this design's
cost model materially.

Revisit trigger: MoltenVK reporting Vulkan 1.4 with `VK_EXT_descriptor_buffer`.
Unlikely, since MoltenVK has no descriptor-buffer implementation.

### 13.2 Descriptor-buffer maturity and tooling

Deployment is narrower than descriptor sets. The *variance* is contained by
design — sizes are never hardcoded, combined descriptors are never used, and
stride never reaches the shader.

The residual risk that is **not** contained: graphics-debugger capture of a
descriptor-buffer application is degraded without `descriptorBufferCaptureReplay`.
Losing RenderDoc or PIX on the only platform that can run the backend is severe,
and it is a sharper practical cost than device coverage. Mitigation: enable the
feature in Debug and diagnostic configurations when present, and record its
absence as a named profile limitation.

Revisit: if `VK_EXT_descriptor_heap` reaches SDK, drivers, validation, and
shader tooling, it is strictly closer to the article's model, and **the only code
that changes is the heap layer** — the shader ABI does not, because indices stay
indices. Structuring the heap module as the single owner of index-to-descriptor
translation is what keeps that swap to one file.

### 13.3 Shader toolchain

Reframed in §7.5. The two real unknowns are `PhysicalStorageBuffer` code
generation under scalar block layout and SPIRV-Reflect's visibility of those
blocks, both V0 gates with named fallbacks.

### 13.4 The shared-core extraction destabilizing the shipping Metal path

Mitigated structurally: V1 is a pure rename and relocation with no behaviour
change, landed *before* any Vulkan implementation code, with the shipping Metal
snapshot as its correctness witness and a Release Metal profile as its
performance witness.

Additional guard: **do not improve the allocator during extraction.** The one
genuine change is parameterizing the slot table by row size so it serves both
material rows and descriptor slots; keep material publication as a thin typed
wrapper so the Metal call sites are textually unchanged. If parameterization
costs measurable indirection in the Metal profile, keep two copies rather than
one slower one — performance is correctness.

### 13.5 Retirement sequencing

**No file under the legacy Vulkan tree is deleted until the bindless Vulkan
backend has passed V6 on Windows and has been the default Windows renderer for a
defined observation period.** ADR-026 owns the split gate that enforces this.

### 13.6 Accepted costs of the chosen decisions

- Requiring descriptor buffers with no fallback narrows the target matrix and,
  more importantly, the tooling matrix. Defensible for a project with one
  supported Windows machine, but recorded as a deliberate acceptance.
- Requiring descriptor buffers while `VK_EXT_descriptor_heap` is the better
  long-term fit means one migration is already scheduled. Contain it in one
  module; do not let descriptor-buffer specifics leak into the material table,
  the ABI manifest, or the shaders.
- Extracting the shared cores now is technically ahead of ADR-020's stated rule,
  which says extract when two concrete implementations exist rather than when the
  second is planned. The mitigation is the containment rule in ADR-024 — extract
  only what is already API-neutral and already compiles on Windows — plus
  re-validating any forced core API change against Metal.
- Eliminating the legacy path entirely removes the only renderer that runs on
  non-descriptor-buffer hardware and on MoltenVK. After the final gate the
  project has Metal on macOS and bindless Vulkan on Windows and **no portable
  diagnostic path at all**. That is the intended end state; it is recorded here
  so it is not discovered later.

### 13.7 Carried forward from the umbrella spec

GPU addresses becoming relocatable, which requires an explicit completion-gated
rebuild protocol and no compaction while live; heap fragmentation and
range-metadata capacity; independent queues (§6.5); and conservative graph stage
scopes until pass declarations are made exact.
