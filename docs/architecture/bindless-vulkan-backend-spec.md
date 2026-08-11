---
status: partial
updated: 2026-08-11
authority: design
---

# Bindless Vulkan 1.4 Backend — Design Specification

**Document status:** Partial. V0–V4 are complete on the RX 6700 XT. The selected
production strategy owns the Vulkan 1.4
device/profile, exact offscreen and windowed prepare/submit/readback, a portable
completed acquire-wait-submit WSI path with optional present fences, reflected
production ABI, shared GPU cores, descriptor heaps, and the V4
geometry/texture/sampler/material
publisher. Prepared and writable texture initialization is now recorded into
the next frame command buffer without a CPU timeline wait; staging lifetime is
tied to that frame's completion value; publication flushes are batched;
samplers are canonical, shared, and reference-counted; and sampler replacement
republishes every dependent material row before retiring the old sampler. The
§5.2 pool now suballocates DEVICE buffers/images, UPLOAD buffers and staging,
and READBACK buffers by `(class, kind, memory_type_index,
device_address_required)`, honors dedicated requirements, persistently maps
host-visible blocks, and publishes logical plus physical memory metrics. The
post-change native offscreen, windowed, synchronization-validation,
GPU-assisted, and repeated-lifecycle gate passes. The Windows V5 implementation
now lowers the canonical authored graph to synchronization2 and dynamic
rendering, executes every required graphics/transfer/IBL category, publishes
completion-valid per-pass timestamps, and uses the shared asynchronous capture
ring. Analytical irradiance plus all nine prefilter mips and whole-graph
offscreen/windowed validation pass on the RX 6700 XT. A later visual audit found
and corrected canceled global-HDR baking, deferred logical unpublication that
blocked generation reuse, and rejection of Bistro's normalized local-probe
cubemap. The corrected Bistro and text output was visually accepted by the
owner on 2026-08-11. Local snapshots, reports, and bindless baseline generations
used during implementation were removed afterward at the owner's request; they
are not retained repository assets. Packet-native retained editor/gizmo
initialization satisfies the full-boot subsystem contract, and packet
submission publishes the required no-overflow instance metric. Windows V6 and
Gate B1 are complete: no-argument application and unpinned harness selection
use bindless Vulkan on Windows, while explicit `vulkan` retains Vulkan 1.2 as
the diagnostic fallback. The required post-extraction Metal witnesses keep the
cross-platform V5 gate open; V7 legacy retirement also remains open.
Section 12 records the exact target and macOS evidence plus the older 1.4.335
tooling limitation.

**Scope:** A bindless Vulkan 1.4 renderer for Windows, built on the semantic
model that the Metal 4 backend already implements, followed by the complete
removal of the Vulkan 1.2 backend. Linux is admitted as a separate,
evidence-gated stage whose platform work is named in §10.3 but not designed
here.

**Non-goals:** Changing shipping Vulkan 1.2 behaviour before its retirement
gates; making a mesh-shader, device-generated-command, or shader-object path
authoritative without measurement; designing the Linux window/input/filesystem
layer; claiming speed without matched Release evidence.

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

## 1. Prerequisite: runtime work requires native Vulkan 1.4 hardware

The local Vulkan runtime is MoltenVK. With SDK 1.4.357.0 it reports device
`apiVersion 1.4.334`, MoltenVK 1.4.1, on an Apple M1 Pro, but does not expose
`VK_EXT_descriptor_buffer`. The missing required extension is disqualifying
against the capability profile in §3; advertising Vulkan 1.4 alone is not
enough.

The consequence is stated first because it governs the whole staging plan:
**every runtime gate in this document requires Windows hardware.** A native
Windows machine became available for V0 and produced the evidence in §12. The
macOS/MoltenVK environment still cannot execute this backend.

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
| Shared-core characterization (V1) | The four candidate modules are pure C with no GPU API; their existing CPU tests and the shipping Metal snapshot pin behaviour before any extraction |
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

One `VkrVkCapabilityProfile`, assembled in two initialization phases, stored by
value, and `const` before the backend is exposed. Physical-device evaluation
fills extensions, features, limits, queues, formats, and recorded optionals. A
candidate logical-device attempt then proves device creation and the two concrete
layout-support queries; success retains the device for the backend, while failure
records the attempt, destroys a successfully created candidate when necessary,
and continues to the next viable physical device.
Nothing in the frame path re-queries the accepted profile. It carries three
sections:

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
| `shaderDrawParameters` | 1.1 | Packet vertex shaders consume `BaseInstance`/`BaseVertex` to preserve packet-local instance and indexed-draw semantics |
| `timelineSemaphore` | 1.2 | The ordinary GPU retirement domain (§6) |
| `descriptorIndexing` | 1.2 | Gates the minimum descriptor-indexing set |
| `runtimeDescriptorArray` | 1.2 | Unbounded `Texture2D g_textures[]` declaration |
| `shaderSampledImageArrayNonUniformIndexing` | 1.2 | Per-lane material indices |
| `shaderStorageImageArrayNonUniformIndexing` | 1.2 | Per-lane storage-image indices in IBL bake |
| `scalarBlockLayout` | 1.2 | Makes the SPIR-V layout of a mixed `uint64`/`vec4`/`mat4` record equal the C layout. Without it the ABI manifest fights std140/std430 padding |
| `hostQueryReset` | 1.2 | Reset timestamp pools without a command buffer, for per-pass timings |
| `dynamicRendering` | 1.3 | Removes `VkRenderPass`/`VkFramebuffer` and the twelve render-pass vtable entries |
| `synchronization2` | 1.3 | Barrier vocabulary that maps onto `VkrGpuDependency`; real `VK_PIPELINE_STAGE_2_NONE`; split transfer stages |
| `maintenance4` | 1.3 | `vkGetDeviceBufferMemoryRequirements` / `vkGetDeviceImageMemoryRequirements` — placement size and alignment **without creating a throwaway object**, the analogue of Metal's `heapBufferSizeAndAlignWithLength:` |
| `shaderDemoteToHelperInvocation` | 1.3 | Alpha-cutout packet shaders lower `discard` to `OpDemoteToHelperInvocation` |
| `maintenance5` | 1.4 | `VkBufferUsageFlags2CreateInfo`; `vkCmdBindIndexBuffer2` for size-bounded index binding, matching Metal's `indexBufferLength:` |
| `VK_EXT_descriptor_buffer` with `descriptorBuffer == VK_TRUE` | extension | The global texture heap (§4). No fallback |
| One queue family with graphics + compute + transfer (+ present when windowed) | — | Keeps the scalar submit value a valid total order (§6.5) |
| `VK_KHR_surface` + `VK_KHR_win32_surface` | instance extensions | Windowed Windows targets only; dropped for offscreen (§1.2) |
| `VK_KHR_swapchain` | device extension | Windowed targets only. Per-image render-complete semaphores plus reacquisition provide the presentation-completion proof (§6.4); a submit timeline is insufficient |

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
- `maxPerStageDescriptorSamplers` and `maxDescriptorSetSamplers` ≥ sampler
  heap capacity;
- `maxPerStageDescriptorStorageImages` and
  `maxDescriptorSetStorageImages` ≥ storage-image heap capacity;
- `maxPerStageResources` ≥ sampled-image plus storage-image descriptors visible
  to each stage at the configured capacities, plus fragment color attachments
  for the fragment stage; separate samplers do not count against this limit;
- `maxPushConstantsSize` ≥ the reserved root range (§7.1);
- `maxBoundDescriptorSets` ≥ 2;
- each descriptor-set layout size ≤ `maxResourceDescriptorBufferRange` /
  `maxSamplerDescriptorBufferRange`;
- combined heap bytes ≤ `descriptorBufferAddressSpaceSize`, and each buffer
  within its own `resourceDescriptorBufferAddressSpaceSize` /
  `samplerDescriptorBufferAddressSpaceSize`;
- `maxDescriptorBufferBindings` ≥ 2,
  `maxResourceDescriptorBufferBindings` ≥ 1, and
  `maxSamplerDescriptorBufferBindings` ≥ 1;
- both concrete descriptor-set layouts report supported through
  `vkGetDescriptorSetLayoutSupport` at their configured capacities.

Also retained in the diagnostic profile: `bufferImageGranularity`,
`nonCoherentAtomSize`, `minMemoryMapAlignment`,
`optimalBufferCopyOffsetAlignment`, `optimalBufferCopyRowPitchAlignment`,
`timestampComputeAndGraphics`, `timestampPeriod`, and the complete
`VkPhysicalDeviceDescriptorBufferPropertiesEXT`. Fields named in the floor above
still reject; the remaining recorded values are observation only.

### 3.4 Rejection report

Fixed-capacity and allocation-free, emitted **per candidate physical device** so
a two-GPU machine explains both:

```text
VkrVkCapabilityReportEntry { kind, name, required, present, detail }
  kind ∈ { API_VERSION, INSTANCE_EXTENSION, DEVICE_EXTENSION,
           FEATURE, LIMIT, QUEUE, FORMAT, DEVICE_CREATE, LAYOUT }
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
| `VK_KHR_unified_image_layouts` (present in 1.4.357 headers; target-driver gate unrun) | Deterministic readback parity with layout tracking disabled, plus a Release A/B on the same device. Layout lowering stays in the code either way |
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
The driver, not the application, turns that index into a layout-defined byte
address. The application computes byte offsets only when *writing* a descriptor,
host-side. `sampledImageDescriptorSize` varying across drivers is therefore
contained in the heap layout/writer module and changes zero bytes of shader ABI.

This is why `VK_EXT_descriptor_buffer` is a workable global texture heap despite
its backend-defined sizes, and it is why the umbrella spec's earlier framing —
that descriptor buffers "still retain descriptor layout/mapping rules and
backend-defined sizes," listed as a drawback — understates the containment. The
rule is stated here and repeated in ADR-023 because every other decision in this
section depends on it.

### 4.2 Two buffers, three bindings

The design uses two descriptor-buffer bindings because sampler and resource
descriptors have distinct usage bits, binding limits, range limits, and address
spaces. This is an application policy, not an assumption that one native buffer
could never carry both usage bits. The capability profile checks the total,
resource, and sampler binding limits independently; using two bindings keeps the
consumption explicit and bounded.

| Buffer | Usage bits | Set | Bindings |
|---|---|---|---|
| Resource | `VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT \| VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` | 0 | `SAMPLED_IMAGE[N_tex]`, `STORAGE_IMAGE[N_storage]` |
| Sampler | `VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT \| VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` | 1 | `SAMPLER[N_smp]` |

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
descriptors. This bounds descriptor memory by the configured capacities and
makes "global texture heap" the only descriptor concept in the renderer, which
is what keeps the seam narrow.

### 4.3 Layout derivation and binding cadence

At initialization, after the profile is captured:

1. Create both `VkDescriptorSetLayout`s with
   `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`. Each binding's
   `descriptorCount` is the configured finite heap capacity even though the
   shader declaration is runtime-sized.
2. `vkGetDescriptorSetLayoutSizeEXT` gives total bytes;
   `vkGetDescriptorSetLayoutBindingOffsetEXT` gives each binding's base offset.
3. Freeze one immutable record per binding —
   `{ base_offset, stride, capacity }` — with `stride` taken from
   `sampledImageDescriptorSize`, `samplerDescriptorSize`, or
   `storageImageDescriptorSize`. **Never a literal.**
4. Publication is one `vkGetDescriptorEXT` writing into
   `mapped_base + base_offset + (uint64_t)index * stride`. No API object is
   created; it is a blob write into persistently mapped host-visible memory.

Do **not** add `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT`,
`VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT`, or
`VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT`, and do not require their feature
bits merely for descriptor buffers. The
[descriptor-buffer binding contract](https://docs.vulkan.org/spec/latest/chapters/descriptorbuffers.html)
already provides the equivalent behavior without enabling those features, and a
descriptor-buffer layout is incompatible with
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT`. Runtime arrays and
non-uniform indexing remain explicit requirements because the shaders use them.
The heaps are fixed-capacity, so
`VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` is also unnecessary.

The two buffers are bound **once per command buffer** at recording start with
`vkCmdBindDescriptorBuffersEXT`. Descriptor offsets are then set once for every
bind point the command buffer actually uses — graphics and, when present,
compute — before its first draw or dispatch. All bindless pipelines must use
identical set-0/set-1 heap layouts, so pipeline-layout compatibility preserves
those offsets; an incompatible pipeline is rejected at creation rather than
causing a per-pass rebind. Never bind or set offsets per draw. Because the bound
offsets are always zero,
`descriptorBufferOffsetAlignment` is trivially satisfied at the command level;
the address supplied by `VkDescriptorBufferBindingInfoEXT` is aligned by placing
each descriptor buffer at
`max(descriptorBufferOffsetAlignment, memoryRequirements.alignment)`, asserted
once at initialization.

### 4.4 Capacity, sentinel, and the two-slot rule

**The heap never grows.** Growth would require a new descriptor buffer and set
layout, compatible pipeline-layout and pipeline rebuilds, and simultaneous old
and new heaps until every in-flight command using the old binding retires.
Published 32-bit indices could retain their numeric values, but the containing
layout/address contract could not change in place. Exhaustion is therefore an
explicit, metered failure, exactly as the Metal material table reports capacity
exhaustion rather than silently substituting or reallocating.

**Slot 0 is a sentinel.** Each heap's slot 0 holds a valid descriptor — a 1×1
opaque-white image, a flat-normal image, a default sampler. A material without a
normal map still resolves to a valid descriptor. Descriptor-buffer rules allow
undefined descriptors that are not dynamically accessed, but proving non-access
is harder than filling a sentinel. Keep the material `flags` bit for shader
branching, matching Metal, **and** fill the sentinel, so a flags defect is a
wrong pixel rather than a device loss.

Logical material capacity and material-row slot capacity are distinct. The
production publisher admits 8,192 material IDs; its GPU table reserves row zero
plus two generations of every logical material (16,385 rows). This guarantees
that a full-table sampler change can publish every successor row before any
predecessor retires. Configuration validation rejects smaller row tables rather
than deferring the mismatch to publication.

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
/* 64 bytes, 16-byte aligned. */
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
bytes instead of 96. That removes one third of the row bytes; any bandwidth or
frame-time effect still requires measurement.

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

The slot state machines are shared with Metal through
[ADR-024](adr/024-shared-bindless-gpu-cores.md), but their typed ownership is
different and remains explicit:

1. Texture and sampler publication acquire and fill descriptor-heap slots.
   Sampled/storage slot pairs belong to the published image resource; canonical
   sampler slots are deduplicated and reference-counted by the sampler publisher.
2. Material lowering resolves those already-published indices, retains the
   referenced resource publications, and fills a **new** row in a free slot of
   the material buffer, an `UPLOAD`-class suballocation reached by device
   address.
3. Publish the new row, then retire the old one against its
   `last_use_submit_value`. Replacing a resource publishes its new descriptor
   slot before rebuilding every affected material row.
4. Completion recycles a retired material row and releases its retained resource
   references. A descriptor slot is recycled only after no live row/publication
   references it **and** its own last use has completed; retiring one material
   must never recycle a texture or sampler shared by another.

Two Vulkan-specific notes:

- The immutability rule is **stricter** than under descriptor sets. A descriptor
  buffer is plain memory read by the GPU at execution time; there is no
  update-after-bind window and no driver-side copy. An in-place overwrite while a
  submitted command buffer can read it is a data race with no repair.
- `allowSamplerImageViewPostSubmitCreation` says whether the `VkSampler` or
  `VkImageView` *object* may be created after the descriptor is recorded. It does
  **not** relax the memory hazard above. This is an easy misread and is recorded
  so it is not made.
- Host writes to `HOST_COHERENT` descriptor memory need no flush. If the selected
  `UPLOAD` memory type is non-coherent, descriptor and material dirty ranges are
  atom-aligned and flushed once before queue submission (§5.4). No GPU barrier is
  needed for these host-only writes.
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

A pool per `(class, kind, memory_type_index, device_address_required)` key owns
one or more fixed-capacity allocator-core blocks, each backed by one
`VkDeviceMemory`. The exact resource requirements select the memory type first;
the key prevents a later image or buffer whose `memoryTypeBits` exclude an
existing block from being bound to incompatible memory. The addressability bit
also prevents an address-bearing buffer from entering a block allocated without
`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`. That allocation flag is used only for
buffer blocks whose resources expose device addresses, never for image blocks.
Block creation is a cold allocation/publication path, not frame or draw work;
block size and maximum blocks per pool remain configuration justified by
metrics rather than constants asserted here.

**Implementation status:** complete for V4. The keyed Vulkan adapter uses one
`vkr_gpu_memory` core per lazily-created block. Dynamic geometry, texture
staging, placement images, frame readback, descriptor/material buffers, and the
frame upload buffer all use the class policy below. Buffer/image segregation
and the device-address key enforce the binding invariants; dedicated-required
and dedicated-preferred resources remain outside the free-range core but enter
the same logical and physical accounting. Block bytes and count, peaks,
capacity failures, logical requested/reserved bytes, ownership classes,
retirement, fragmentation, and largest-free-range values lower into the
pre-registered renderer metric family.

| Class | Memory property search, in order | Contents |
|---|---|---|
| `DEVICE` | Prefer `DEVICE_LOCAL` without `HOST_VISIBLE`, then any compatible `DEVICE_LOCAL`, then any compatible type with a named degraded-placement report | Placement images; persistent vertex and index buffers |
| `UPLOAD` | Prefer `HOST_VISIBLE\|HOST_COHERENT\|DEVICE_LOCAL` (resizable BAR), then `HOST_VISIBLE\|HOST_COHERENT`, then compatible `HOST_VISIBLE` | Root records, frame streams, material/light tables, **both descriptor buffers**, staging |
| `READBACK` | Prefer `HOST_VISIBLE\|HOST_CACHED\|HOST_COHERENT`, then `HOST_VISIBLE\|HOST_CACHED`, then `HOST_VISIBLE\|HOST_COHERENT`, then compatible `HOST_VISIBLE` | Capture, picking, query results |

**`bufferImageGranularity` is handled by segregation, not padding.** Including
`kind` in the key means buffers and optimal-tiling images never share a
`VkDeviceMemory`, so the granularity hazard cannot occur. Metal's adapter
collapses class and kind onto its placement heap; Vulkan also keys by memory
type. Same core, different adapter policy — which is what keeps Vulkan-only
concepts out of shared code. If padding is ever needed anyway, it must be
counted into the existing alignment-waste row rather than hidden.

### 5.3 Placement

`vkGetDeviceBufferMemoryRequirements` and `vkGetDeviceImageMemoryRequirements`
yield size, alignment, and `memoryTypeBits` from the create-info **without
creating an object** — the analogue of Metal's
`heapBufferSizeAndAlignWithLength:`. Chain `VkMemoryDedicatedRequirements` into
the query. `requiresDedicatedAllocation` takes a dedicated path; the initial
policy also honors `prefersDedicatedAllocation`, and changing that hint policy
requires a same-device measurement. Dedicated resources chain the created
buffer or image through `VkMemoryDedicatedAllocateInfo`, use the same owner and
retirement accounting, and never enter the free-range core. Otherwise the
selected memory type chooses the pool, one compatible block returns a placement,
and the adapter creates and binds the object at `placement.resource_offset`.

### 5.4 Addresses and mapping

- Device address: `vkGetBufferDeviceAddress` once at creation, cached in the
  allocation record beside the `VkBuffer`. Stable for the allocation's lifetime.
  Never recomputed per frame and never per draw.
- Mapping: each host-visible block is mapped **once, persistently, at block
  creation**. A suballocation's CPU pointer is
  `mapped_base + placement.resource_offset`. Never map per resource.
- Non-coherent upload memory: flush the union of written ranges, rounded to
  `nonCoherentAtomSize`, **once per ring slice or descriptor/material batch at
  submit time**, not per write. Rounding is relative to the containing
  `VkDeviceMemory` allocation and never crosses its mapped range.
- Non-coherent readback memory: after the completion value proves device writes
  are done, invalidate the atom-aligned result range before the CPU reads it.

The atom-range calculation is a pure overflow-checked helper with CPU cases for
unaligned starts/ends, exact atoms, allocation-end clamping, and disjoint writes.
Hardware evidence exercises a non-coherent type when the target exposes one;
the helper tests remain mandatory when it does not.

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
slots, material rows, ordinary image views, and placement ranges all obey the
submit-timeline rule. Swapchain presentation resources use the additional proof
in §6.4. An assumed frame lag is never completion proof.

### 6.4 WSI is the one place the timeline is not enough

`vkAcquireNextImageKHR` and `vkQueuePresentKHR` require **binary** semaphores,
and completion of an unrelated submit timeline value does not prove that
presentation resources may be safely recycled or destroyed. A windowed submit
signals the timeline at value N for ordinary GPU retirement and a
per-swapchain-image binary render-complete semaphore for present, while waiting
on a per-frame-slot binary acquire semaphore. `VkSubmitInfo2` carries those
semaphores. Reacquiring an image is only the first half of the portable proof:
the backend records the reacquisition, consumes that acquire semaphore in a
successful queue submit, and waits for that submit's timeline value to complete
before treating the prior presentation as finished. Reacquire return alone is
not sufficient.

When a windowed target exposes both the
`VK_KHR_swapchain_maintenance1` extension and feature, the device enables it and
chains a per-image `VkSwapchainPresentFenceInfoKHR` into presentation. The
present fence is then the explicit completion proof before image reuse, retired
swapchain collection, or shutdown destruction. The completed
acquire-wait-submit algorithm remains the portable fallback, so maintenance1 is
not part of the required profile floor.

Present-result classification is explicit. `VK_SUCCESS`, `VK_SUBOPTIMAL_KHR`,
and presentation-engine rejection results for which the specification says the
queue operations were still enqueued — including `VK_ERROR_OUT_OF_DATE_KHR`,
`VK_ERROR_SURFACE_LOST_KHR`,
`VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT`, and
`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT` when their extensions are enabled —
leave the image in the presented state until reacquisition. Out-of-host/device
memory results do not prove enqueue and leave the image acquired. Because the
target has no swapchain-maintenance release operation, cancellation records a
minimal transition/present submission instead of abandoning the acquired image.
`VK_ERROR_DEVICE_LOST` enters device-loss teardown. Any other result fails the
target without recycling or destroying present-owned resources under an
unproven condition. One pure classifier and one reacquisition state machine own
these distinctions and have table tests over every handled transition. The
[Khronos queue-present result contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)
is the authority for which rejection results still enqueue work. The
[Khronos swapchain-semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
is the authority for tying present-wait semaphores to acquired images, and the
[Khronos swapchain-recreation sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html)
is the authority for using reacquisition to retire old swapchains. This is
resource-lifetime proof, not presentation-timing evidence.

This is a genuine divergence from Metal, where drawable presentation needs no
semaphore, and it lands on the sizing table that
[ADR-009](adr/009-frame-synchronization.md) and
[ADR-014](adr/014-offscreen-present-target.md) record as a historical defect
family in this repository. The invariants are therefore stated explicitly:

- acquire semaphores are sized by **frames in flight** and indexed by frame slot;
- render-complete semaphores are sized by the **actual swapchain image count
  returned by `vkGetSwapchainImagesKHR`**, not by the requested minimum, and are
  indexed by acquired image;
- submit fences are eliminated — the timeline replaces the per-frame submit
  fence and the image-in-flight fence references — while an optional present
  fence or a completed acquire-wait submit remains the separate WSI
  resource-lifetime proof and never drives ordinary GPU retirement;
- image-in-flight tracking survives as `uint64_t image_last_submit_value[]`, with
  a timeline wait after acquire;
- presentation state is sized by actual swapchain image count. A completed
  submit that waited on the reacquired image's acquire semaphore permits reuse
  of that image's render-complete semaphore. After resize, the same proof from
  the successor swapchain permits collection of predecessor swapchains retired
  before it. When maintenance1 is enabled, the corresponding present fences
  provide those proofs directly. The retired list is bounded and reports
  exhaustion instead of unsafe destruction. An acquired image cancelled before
  the normal draw records a minimal transition/present submission so it is not
  abandoned.

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
one `vkCmdPushConstants` of eight bytes — one bounded per-draw command, with no
descriptor rebind, no offset change, no allocation, no lock, and no string
construction. It is the exact semantic analogue of the argument-table address
write; its cost is measured rather than inferred.

Reserve a 16-byte push-constant range in the pipeline layout (address plus a
material index and flags). V3 populated only the address; V4 also populates the
material index while flags remain reserved. The range is recorded in the ABI
manifest, so widening it later is a manifest change with a reflection gate
rather than silent ABI drift.

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
the constraints are the record's manifest alignment and the buffer-address
rules proven by V0. Vulkan therefore uses the smallest stride that satisfies the
manifest and device requirements. A guessed CPU or GPU cache-line size is not a
portable alignment rule; any extra padding requires measurement. The 512 and
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

The bindless source lives under
`lib/src/renderer/vulkan/bindless/shaders/`. `packet.slang` owns the shared
packet ABI and the `world`, `shadow`, `picking`, `text`, `skybox`, `ibl`, and
`post` entry points in one compilation unit; API-neutral helpers such as
tonemapping remain includes. CMake compiles each entry point into a **separate
output namespace** so the fifteen existing Vulkan outputs and their `.shadercfg`
manifests remain untouched throughout migration. Split a domain into its own
source only when it owns an independent ABI or the shared compilation unit
creates measured build-time or maintenance cost.

Do **not** reuse the legacy `vulkan/shaders/common/*.slangh` headers. The
umbrella spec already records that the transform, instance, alpha-cutout, cube,
cascade, and tonemap headers encode descriptor-era reuse. They are deleted with
the legacy path at ADR-026 step 2.

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
- **Buffers:** emit one `VkBufferMemoryBarrier2` per compiled buffer barrier,
  preserving resource identity and the graph's current whole-buffer range.
  Batch them into the pass's single barrier command. A future change may collapse
  them into a global `VkMemoryBarrier2` only after a same-device Release A/B
  shows no material regression and synchronization validation proves equivalent
  behavior. Single-queue ownership makes a global barrier legal; it does not
  make its cost free by construction.
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
  ADR-026 step 6.
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
concrete evidence for ADR-020 and ADR-024's decision not to build one.

### 8.6 Layouts

The graph already tracks layout per subresource and produces source and
destination layouts. The lowerer maps them to the core layout set, including the
separate depth and stencil layouts available since Vulkan 1.2.

Descriptor-buffer interaction is the two-slot rule of §4.4: sampled-image slots
are baked with `SHADER_READ_ONLY_OPTIMAL`, storage-image slots with `GENERAL`.

With `VK_KHR_unified_image_layouts` — present in the current 1.4.357 headers but
still unselected because its target-driver parity/performance gate has not run —
internal transitions collapse to `GENERAL`, and the two-slot rule collapses to
one slot. Exceptions survive: initialization from `UNDEFINED`, present, and
external or video ownership. The lowerer keeps the full layout state machine
either way and selects the unified path behind one profile boolean; the graph
is unchanged in both cases.

### 8.7 The three lowerers

| | Legacy Vulkan 1.2 | Metal 4 | Bindless Vulkan 1.4 |
|---|---|---|---|
| Vocabulary | `VkPipelineStageFlags` / `VkAccessFlags` | Metal stages and visibility options | `VkPipelineStageFlags2` / `VkAccessFlags2` |
| Resource identity in the command | image plus range | discarded | image plus range |
| Layouts | yes | none | yes, or `GENERAL` under unified layouts |
| `NONE` stage | substitutes top/bottom | all stages | real stage-none |
| Transfer granularity | one transfer bit | blit stage | split copy/blit/resolve/clear |
| Buffer barriers | per-buffer | none | per-buffer, batched into one command per pass |
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

**Hot-path guarantee.** After initialization a normal successful frame executes
two indirect strategy calls — `prepare_frame` and `submit_packet`. Exceptional
lifecycle, resize, and explicitly requested capture/poll operations may add
bounded coarse calls, but none is dispatched per pass or per draw. Inside
`submit_packet` the backend is direct-typed. There is no per-pass indirect call,
no per-draw indirect call, no backend-behavior type test, no allocation, no
lock, and no string construction. A source audit proving this is required
evidence in every stage from V3 onward, mirroring what the Metal walking slice
did.

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
is why ADR-026 records the legacy platform files as superseded at V3 so their
later deletion removes already-replaced code.

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
Recreation happens at the next `prepare_frame` boundary after the submit
timeline proves the renderer's GPU work complete. The old swapchain is passed as
`oldSwapchain`, but its render-complete semaphores and handle are retained. They
are destroyed only after a successor acquire semaphore has been consumed by a
completed submit, or after an enabled maintenance1 present fence signals. That
makes every predecessor retired before the proof safe to collect. This is WSI
resource-lifetime proof, not ordinary submit retirement or presentation-timing
evidence, and resize requires no queue/device-wide idle. A
zero extent returns the existing frame-skipped error,
matching the Metal branch. Graph-owned placement images recreate through the
same cache path Metal already uses.

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

Every "flat" or "no material regression" profile below uses a tolerance declared
before the paired runs, with the authoritative harness provenance and repetition
policy. The wording is not permission to choose a threshold after seeing data.

| Stage | Deliverable | Required evidence | Runs on |
|---|---|---|---|
| **V0 — capability and toolchain spike** | Standalone Windows executable: enumerate devices, build and print the capability profile and rejection report; create a 1.4 device with the full floor; create both descriptor buffers; publish one sampled image and one sampler; allocate one `UPLOAD` buffer and capture its device address; compile a Slang→SPIR-V pair with a push-constant root address, a `PhysicalStorageBuffer` vertex array, and one non-uniform texture sample; one indexed textured offscreen draw; exact readback; timeline signal and wait | Validation layers clean, including synchronization and GPU-assisted; exact RGBA8; profile printed; **SPIRV-Reflect `PhysicalStorageBuffer` recursion proven or the fallback walker written**; Slang scalar-layout offsets match `offsetof` | Windows, plus macOS for the offline reflection half |
| **V1 — shared-core characterization** | Pin the four ADR-024 candidate contracts with API-neutral tests and record the exact Metal call sites; move no production module before a second caller exists | CPU suite green on both platforms; the candidate modules remain Metal-owned; no new forwarding layer or speculative shared API | macOS and Windows |
| **V2 — implementation seam** | ADR-025's capability struct and coarse strategy, three implementations (Metal, legacy-Vulkan adaptor, bindless stub that fails initialization); all behavior branches replaced; the neutral submit result replaces the untyped timing pointer | CPU suite green; Metal snapshot byte-identical; legacy Vulkan Debug startup, resize, and shutdown validation-clean on both platforms; source audit proving no renderer-behavior backend-type test after factory selection; Release Metal profile meets its predeclared no-regression tolerance | macOS and Windows |
| **V3 — walking bindless renderer** | Device, queue, timeline, command pools; offscreen and windowed targets with per-image present semaphores and reacquisition completion proof; descriptor heaps; one indexed textured draw through the real prepare and submit path; resize; extract the memory, submit-ring, and ABI cores only when their Vulkan call sites land | Validation clean windowed and offscreen; deterministic offscreen readback and exact identifier capture; resize across two extents with retired-present proof; present-result and reacquisition table tests; source audit and runtime counters proving no per-draw allocation, lock, string, or dispatch; wait counters publishing; Metal snapshot/API validation after each extraction | Windows, plus macOS for Metal extraction witnesses |
| **V4 — memory, materials, descriptor heaps** | Material row publication; texture, sampler, and storage-image publish, replace, and retire; sentinel slot; two-slot rule; capacity reporting; asset publisher wired to the shared loaders; extract the slot table with this second caller; finish the §5.2 dynamic memory pool and full bindless memory metric family | Multi-material capture exact; two materials share one texture/sampler, one retires while work is pending, and the survivor remains exact; texture replacement while frames are pending; non-coherent atom-range CPU cases and target execution when available; capacity-exhaustion metric fires; repeated create, submit, and destroy returns every logical total to its initial value; validation clean; Metal snapshot/API validation after extraction | Windows and macOS |
| **V5 — graph, sync2, dynamic rendering, pass parity** | The bindless dependency lowerer; dynamic rendering for every authored pass; shadow cascades, skybox, opaque, transmission, blend, picking, tonemap, editor and UI, text, the full IBL bake, asynchronous capture overlay, per-pass timestamps; extract the capture ring with this second caller | The same declared five-channel capture batch as Metal returning exact final color, HDR scene color, depth, shadow layer, and picking identifiers; analytical IBL checks across irradiance and every prefilter mip; **synchronization validation clean across the whole authored graph**; deterministic repetitions; the CPU barrier-lowering table test; Metal snapshot/API validation after extraction | Windows, plus macOS for the CPU and Metal halves |
| **V6 — feature parity and Windows selection** | Application and harness backend selection; pipeline cache cold and warm; asset load and unload; metrics parity; a Windows-capable implementation matrix | ADR-021's Gate-B functional checklist on Windows; native validation and lifecycle correctness; and owner-accepted Bistro/text output. Local snapshots, reports, and bindless baseline generations are disposable implementation evidence, not retained product assets | Windows |
| **V7 — legacy retirement** | Per [ADR-026](adr/026-vulkan-1-2-retirement.md) | Per ADR-026's gates B1 and B2 | Windows and macOS |

**V0 implementation status (2026-08-08):** the standalone executable, shader,
build wrappers, recursive reflection check, descriptor-buffer publication,
indexed offscreen draw, exact readback, and timeline wait are implemented.
Release execution and synchronization validation pass on the Windows device
recorded in §12. Validation layer 1.4.357 executes the descriptor-buffer
GPU-assisted path and the V0 gate passes with zero actionable validation
warnings or errors. Its three `WARNING-Setting-Limit-Adjusted` setup notices
remain visible and are counted separately. Layer 1.4.335 still returns
`GPU_ASSISTED_UNAVAILABLE`, which remains evidence of an unavailable gate rather
than a pass. V0 is complete as a standalone spike; it does not implement or
accept the production backend.

**V1 implementation status (2026-08-09):** characterization is complete on both
required platforms without moving or renaming a production module. The
four candidates remain Metal-owned and have these exact integration witnesses:

| Candidate | Production integration callers | Contract test |
|---|---|---|
| Memory and submit ring | `vkr_metal_memory_device.m`; packet resource, graph, frame, command, lifecycle, and setup units consume only the typed device adapter | `tests/src/metal_memory_test.c` pins aligned placement/accounting, generation invalidation, completion-ordered collection, exhaustion classification, bounded-ring reuse, address-pair slicing, and wait-counter reset |
| Material slot table | `vkr_metal_material_table_device.m`; packet setup, resource, frame, and lifecycle units consume the typed device adapter | `tests/src/metal_material_test.c` pins publish-new-before-retire-old replacement, stale-handle rejection, completion-gated collection, and transactional capacity failure |
| Capture ring | packet setup, command, graph, frame, and lifecycle units call `vkr_metal_capture_ring_*` directly | `tests/src/metal_capture_ring_test.c` pins reserve/submit/poll/release ownership, abandoned pending retirement, capacity, and retained failure state |
| Host ABI manifest | packet setup validates the manifest before renderer publication | `tests/src/metal_packet_abi_test.c` pins every record/field name, expected range, unique shader member, host size/alignment/offset validation, and invalid-record rejection |

The macOS and Windows CPU suites pass this characterization; the four contract
test groups compile and register unconditionally on Windows. Per ADR-024, this
characterization creates no shared forwarding API and authorizes no extraction
before the corresponding V3–V5 Vulkan caller exists.

**V2 implementation status (2026-08-09):** the implementation seam is
implemented. `VkrRendererImpl` owns one immutable capability record, opaque
state, asset publisher, and coarse operation table. The factory selects real
Metal and legacy-Vulkan strategies and recognizes a bindless Vulkan strategy
whose initialization is deliberately unsupported until V3. Property/value
sites consume capabilities; resource ownership and lifecycle enter the selected
strategy; and the normal frame path makes exactly two indirect calls, prepare
and submit. Legacy-only orchestration still calls
`VkrRendererBackendInterface` directly inside the legacy strategy, preserving
the retained adaptor without widening it.

`VkrRendererImplSubmitResult` now carries the shared capture snapshot, memory
metrics, material metrics, draw counters, and pass timings. The former untyped
Metal result pointer and casts are absent. A renderer-source audit finds
backend-type behavior only in `vkr_renderer_impl_select()`; the legacy Vulkan
backend retains one initialization invariant assertion. Factory tests cover the
two real strategies, platform rejection, and the bindless stub, and the complete
macOS CPU suite plus Debug and Release wrapper builds pass.

For the macOS visual witness, clean pre-change text report
`20260808T195724.431Z-010759` and current report
`20260808T195733.500Z-0106f8` both pass with matching workload identity. Their
final-color bytes are identical at
`sha256:019ba7752b653ea77dc8fce8e4125b042f67794d1978aa12044ed4c5b44ad3a6`
and their picking-identifier bytes are identical at
`sha256:ed47dbf1b5e6ade6820370e0313b257c3067d667dd277a1d0033c4d204a26388`.
Debug legacy-Vulkan report `20260808T204053.961Z-01220d`
(`sha256:cc53632c1810dce60fa97e4195b9d677588e1eb517234fa93b47f29621917a58`)
passes both requested children and aggregate reporting. Each child performs an
explicit hidden native-window resize from 640×480 logical points to 400×300 and
back, proves that the renderer observed both resulting pixel extents, records
three swapchain recreations on the Retina host, creates and destroys the debug
messenger, and emits no stderr, VUID, validation warning, error, or fatal log.

Native Windows report `20260809T084713.791Z-003636`
(`sha256:0e4964fa31bb7e9dd575e868986fda82a3d50b61989ef6d38aa9ced969a0d5f7`)
completes the other platform half. The complete CPU suite passes. Both Debug
legacy-Vulkan children resize the hidden tracked text fixture from 320×240 to
400×300 and back, record the renderer-observed round trip and swapchain
recreation, create and destroy the debug messenger, and contain no VUID,
validation warning, error, fatal, or stderr output. The fixture is deliberately
self-contained and does not depend on an untracked Sponza source scene.
Post-V3/V4 combined-tree report `20260809T113259.325Z-003da8`
(`sha256:fbb9ec7686f3cd0fc6ba999c962e5566802366a9bed695bd3d7ff78bf488e12b`)
repeats the complete Windows CPU/build and two-child legacy resize/lifecycle
gate, proving that the new selected strategy and shared cores did not regress
the retained adaptor.

The predeclared Release Metal tolerance requires identical
environment/workload/policy identities and work volume, no more than +5% in
`frame.wall` mean or p50, +10% in `frame.wall` p95, and +5% in
`cpu.render_prepare` and `cpu.render_submit` p50. Clean baseline report
`20260808T202226.320Z-01173e`
(`sha256:9805f7d84179337df4681c39c546f1e365f6b68a8a7e8a6bd78f4131fc8cfb73`)
and clean candidate report `20260808T201602.292Z-01150f`
(`sha256:e01eab23669cd886452ce92593e9bc95c6282681d9a67b504b5610fd636ac65b`)
are authoritative, have empty authority reasons, share all three fingerprints,
and each contain 1,500 valid samples at exactly 322 calls per frame. Candidate
deltas are `frame.wall` mean +0.890%, p50 +0.419%, p95 -2.182%; prepare p50
+0.231%; and submit p50 -2.199%. Every threshold passes. Therefore V2 is
complete on both required platforms. ADR-025 is Accepted because the production
V3 bindless strategy now passes its target gates.

**V3 implementation status (complete for RX 6700 XT, 2026-08-09):** the selected
production bindless strategy creates the ADR-023 Vulkan 1.4 device and owns
the queue, timeline semaphore, bounded command-slot ring, fixed descriptor
buffers, per-memory-type allocation blocks, offscreen targets, window/swapchain
state, and retired-present collection. Its real frontend `prepare_frame` /
`submit_packet` path renders and reads back exact RGBA `{37,91,173,255}` and
identifier `0xffad5b25` at 4×4 and 7×5. Production SPIR-V is reflected before
pipeline creation: stages, entry points, push constants, recursive
`PhysicalStorageBuffer` records, vertex/material layouts, and the two runtime
descriptor arrays are checked against the host ABI. The runtime reports zero
command-slot waits for the deterministic sequence.

`renderer/vulkan/bindless/vkr_bindless_vulkan_wsi.*` classifies every handled
present result and pins the per-image reacquisition state machine. The RX 6700
XT exposes `VK_EXT_descriptor_buffer`, core surface/Win32-surface/swapchain, and
neither maintenance1 extension. The production window path therefore retains
per-image render-complete semaphores until the submit that waits on a reacquired
image's acquire semaphore completes. A hidden native window renders once at 320×240, recreates at
400×300, then renders eight frames. The run produces five reacquisition proofs,
retires and collects exactly one old swapchain, leaves zero retired swapchains
live, reports zero command-slot waits, and is synchronization-validation clean.
No queue/device idle occurs in prepare, submit, or resize; `vkDeviceWaitIdle` is
confined to final windowed shutdown after the timeline wait.

The memory, submit-ring, and shared vertex/instance/text ABI intersections moved
to `vkr_gpu_memory`, `vkr_gpu_submit_ring`, and `vkr_gpu_abi` only after this
production Vulkan caller existed. Metal retains typed adapters. The complete
CPU suite passes on both required platforms. The current macOS post-extraction
snapshot is byte-identical to the pre-extraction witness, the focused Metal
API/GPU-validation replay is clean, and the matched authoritative Release pair
passes its frame-wall and prepare bounds. Submit p50 remains over the
predeclared `+5%` bound, so the cross-platform extraction gate is not closed.
This does not change the completed RX 6700 XT V3 status or constitute a
Vulkan-versus-Metal performance comparison.

**V4 implementation status (complete for RX 6700 XT, 2026-08-10):**
`vkr_gpu_slot_table` is now shared by the Metal material wrapper and production
Vulkan sampled-image, sampler, storage-image, and material tables. Slot zero is
the permanent sentinel. Publication is fixed-capacity and generation checked;
replacement publishes the new row before retiring the old row; retirement and
native-object destruction wait for timeline completion. `slots_retired` is the
current pending gauge and `slots_retirements` is the cumulative counter, pinned
by an API-neutral CPU contract test.

The bindless `VkrAssetPublisher` publishes and completion-retires geometry and
loaded-mesh buffers, prepared and writable 2D/cubemap textures, sampled-image,
storage-image, and sampler descriptors, and material rows through the real
geometry, texture, mesh, and material systems. Samplers use one canonical key
over the effective filter, address, and mip-level state; equivalent
textures share one native sampler and descriptor slot under reference counting.
Replacement acquires the successor first, republishes all dependent material
rows, then completion-retires the old row and sampler. An identical update is a
no-op. Writable targets obey the two-slot sampled/storage rule and publish their
sampled descriptor with `GENERAL` layout.
Two materials share one texture in the runtime fixture; one material and the
logical texture retire while submitted work is pending; the survivor remains
exact; a new texture/material row replaces the old dependency; and collection
returns every fixture-owned live and pending total to its pre-test baseline.
The production defaults remain live (`sampled=5`, `storage=1`, `sampler=5`,
`material=2` including sentinels), while six fixture material retirements are
collected and none remain pending. Three fresh create/submit/destroy process
repetitions return the same logical totals and clean validation result.
Non-coherent dirty-range helpers, slot capacity failure, and non-mutating
multi-table retirement preflight are covered by CPU tests. The exact runtime
fixture publishes a loaded mesh and makes the walking
draw consume its vertex device address, native index buffer, count, and type. It
also publishes a writable storage+sampled texture, performs one sampler
replacement and one identical no-op update, and verifies the latter creates no
slot publication. Full IBL convolution remains V5 graph/pass work; V4 supplies
the writable cubemap targets and descriptor publication it needs. Prepared
texture uploads and writable-image initialization are bounded by the
sampled-image publication capacity and recorded at the start of the next frame
command buffer. Publication is immediately visible to material construction;
the initialization commands precede the draw in that submission, staging
buffers retire at its timeline value, and cancellation preserves the pending
batch for retry. Descriptor and material dirty intervals accumulate between
submissions and flush once per backing buffer immediately before queue submit.
Geometry vertex/index publication now stages through bounded UPLOAD placements
into DEVICE placements. Prepared textures use the same UPLOAD pool, images use
segregated DEVICE image pools, and frame result buffers use READBACK pools.
Maintenance4 requirements and dedicated hints are queried before native object
creation. Host-visible blocks are persistently mapped and address-bearing
buffer blocks alone carry `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`.
The runtime fixture now requires zero upload waits, shared canonical sampler
ownership, dependent-row republication, rejection of stale enabled texture
handles, the shared material-flag ABI, and logical memory totals returning to
their settled pre-test baseline. The Windows gate records seven physical
allocations, 45 MiB allocated, 31 live logical allocations before fixture
settling, zero retired allocations, and zero pool-capacity failures.

**V5 Windows implementation status (complete; cross-platform extraction
evidence open, 2026-08-10):** the Vulkan-private
`vkr_bindless_vulkan_dependency` module purely lowers canonical graph stage,
access, and visibility records into synchronization2 masks. It preserves real
stage-none, splits vertex input and transfer stages, emits zero access masks for
execution-only dependencies, and rejects unsupported resource-alias visibility
with a named result. The CPU barrier table pins these cases beside the unchanged
legacy lowerer.

The selected renderer realizes authored graph resources, emits the compiled
barrier batches once, opens dynamic-rendering scopes, records transfer copies,
and executes shadow, skybox, opaque, transmission, blend, picking, tonemap,
editor, UI/text, and compute IBL work through reflected production SPIR-V. The
full PBR path consumes packet lights, cascaded shadows, material textures,
diffuse/specular IBL, transmission, and alpha modes. The IBL runtime gate bakes
a constant analytical environment and checks irradiance plus all nine prefilter
mips within two binary16 ULP before proving memory return to baseline.

The Bistro visual audit found two publication-contract defects beyond that
analytical fixture. The decoded HDR delivery texture was unpublished after its
bake was queued, and bindless unpublication canceled the queued work before a
frame could submit it. A first correction retained the input but deferred its
logical unpublication, which prevented the texture system from publishing a
new generation in the same numeric slot. Logical unpublication now removes the
old generation from the active ID-indexed table immediately and moves its
physical image, descriptors, and sampler ownership to completion-gated retired
storage. Pending upload and IBL work retain full `{id, generation}` handles and
resolve either active or retired publications, so the old generation survives
until the successful recording submit owns the work without blocking immediate
slot reuse. Direct cubemap bakes also accept sampled normalized/sRGB sources
while retaining RGBA16F requirements for writable outputs and equirectangular
conversion. The V5 self-test now republishes the same ID at generation 2 after
unpublishing the queued generation 1 input and before any frame submission; the
analytical irradiance/prefilter result, validation counters, and logical memory
return all pass. Local dirty Debug child report
`20260810T211514.281Z-003c74/captures/0` and Release snapshot
`20260810T212716.021Z-0029d4` (digest
`sha256:cf8814484ab03c6f88f81f96ca869bcc5f09c53a5e6adf547e61499d7c3dddeb`)
load Bistro and produce captures without the publication, cubemap, probe, or
validation errors; neither is baseline-acceptance evidence.

The former Metal capture ring is now `vkr_capture_ring`, with typed Metal and
Vulkan callers. Bindless capture records bounded completion-gated copies for
final color, HDR scene color, depth, a selected shadow layer, and picking IDs;
per-pass timestamp rows become valid only after their submit value completes.
Independent Bistro snapshot reports `20260810T104724.326Z-0023ef` and
`20260810T140451.471Z-003c9a` have identical exact channel digests. Debug
offscreen report `20260810T140413.925Z-004038` and hidden-window whole-graph
report `20260810T150804.316Z-003609` pass with empty stderr. The dedicated gate
passes ordinary and GPU-assisted validation with zero actionable warnings or
errors; the windowed path records five reacquisition proofs, one retired and
collected swapchain, and no live retirement. The complete CPU suite passes.
The shared-core CPU contract is covered on Windows, but the required macOS
post-extraction Metal snapshot/API-validation witnesses are unavailable in this
workspace; no Windows result is substituted for them.

**V6 implementation status (Windows complete, 2026-08-11):** the
application, harness schema/resolver/child, and focused tests accept the
explicit `vulkan-bindless` selector, and no-argument application or unpinned
harness selection now chooses bindless Vulkan on Windows. Explicit `vulkan`
continues to select the retained legacy backend. The backend honors requested present mode,
loads and atomically saves the driver pipeline cache, completion-retires staged
assets and replacements, and publishes graph, descriptor, material, allocation,
draw, and timing metrics. The Windows implementation matrix covers legacy text,
bindless offscreen and windowed whole-graph text, lifecycle/IBL validation, and
cold/warm pipeline cache. Final pipeline report
`20260810T171129.662Z-0009cf` passes its cold-save and warm-load children.

Implementation-time Bistro/text snapshots were visually reviewed and accepted
by the owner after the IBL and lifecycle corrections. The local snapshot,
profile, proposal, and bindless baseline artifacts were subsequently removed
at the owner's request. Performance optimization and performance gating are
outside this implementation stage; no legacy shader change made solely for the
comparison remains in the tree.

Gate B1 therefore closes on the accepted visual/correctness result and the
implemented functional, lifecycle, cache, metrics, and validation behavior.
The Windows default flip is implemented. Gate B1 does not authorize legacy
deletion; explicit `vulkan` remains available through V7/B2.

Optional capabilities are deliberately outside this ladder. Each is its own
measured change after V6, per §3.5.

**Platform coverage boundary:** Windows evidence executes the RX 6700 XT
offscreen/windowed Vulkan, CPU, reflection, synchronization-validation, and
GPU-assisted gates. macOS still cannot execute the descriptor-buffer backend,
and no native Windows result is inferred from its evidence. V3 and V4 are target
complete. The shared capture ring has production Metal and V5 Vulkan callers;
ADR-024 remains partial only because its post-capture-extraction Metal snapshot
and API/GPU-validation witnesses have not been rerun on macOS.

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

# Enumerate the active runtime.
vulkaninfo --summary

# Build and run the standalone V0 spike on Windows.
build_bindless_vulkan_v0.bat Release
build_bindless_vulkan_v0_Release\tools\vkr_bindless_vulkan_v0.exe
build_bindless_vulkan_v0.bat Debug
build_bindless_vulkan_v0_Debug\tools\vkr_bindless_vulkan_v0.exe --validation
build_bindless_vulkan_v0_Debug\tools\vkr_bindless_vulkan_v0.exe --gpu-assisted

# Reproduce the completed V1/V2 Windows CPU and legacy-runtime gate.
powershell -ExecutionPolicy Bypass -File tools\validate_v1_v2_windows.ps1

# Build and run the selected production V3/V4 slices on Windows.
powershell -ExecutionPolicy Bypass -File tools\validate_v3_v4_windows.ps1

# Individual diagnostic invocations used by that gate.
build_bindless_vulkan_v3.bat Debug
build_bindless_vulkan_v3_Debug\tools\vkr_bindless_vulkan_v3.exe --validation
build_bindless_vulkan_v3_Debug\tools\vkr_bindless_vulkan_v3.exe --validation --windowed
build_bindless_vulkan_v3_Debug\tools\vkr_bindless_vulkan_v3.exe --gpu-assisted

# The branch ladder ADR-025 removes.
grep -rn "VKR_RENDERER_BACKEND_TYPE_METAL" lib/ app/ tools/ tests/ | wc -l

# The shared capture core and remaining Metal-owned candidates carry no Metal types.
grep -n "MTL\|@interface\|id<" \
  lib/src/renderer/metal/vkr_metal_memory.c \
  lib/src/renderer/metal/vkr_metal_material_table.c \
  lib/src/renderer/vkr_capture_ring.c \
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
- The metal-backend branch ladder is 46 sites across 14 files, 43 of the sites
  under `lib/`.
- The four extraction candidates contain no Metal type references outside
  comments.

Current macOS tooling observation on 2026-08-09 with SDK 1.4.357.0:

- `VK_EXT_descriptor_buffer`, `VK_EXT_descriptor_heap`, and
  `VK_KHR_unified_image_layouts` are present in the headers. Header presence
  does not alter the accepted profile or enable an optional path without its
  target-driver evidence gate.
- The Apple M1 Pro device reports Vulkan 1.4.334 and MoltenVK 1.4.1 but still
  does not enumerate `VK_EXT_descriptor_buffer`, so the bindless Vulkan backend
  remains unavailable on macOS for the required-extension reason.
- `slangc` reports `2026.13.1-1-g84792eb15`; the Release wrapper and complete
  CPU suite compile against Vulkan headers/libraries 1.4.357.

Initial Windows evidence on 2026-08-08 with SDK 1.4.335.0 and Slang 2025.23.2:

- The AMD Radeon RX 6700 XT reports Vulkan 1.4.315, AMD proprietary driver
  26.6.3, and conformance 1.4.0.0. The complete V0 common/offscreen profile is
  viable on queue family 0.
- The driver reports 32-byte sampled/storage-image descriptors, 16-byte sampler
  descriptors, and 16-byte descriptor-buffer offset alignment. Layout support
  succeeds for both finite V0 layouts.
- The Release path completes one indexed draw, signals and waits for timeline
  value 1, and reads back an exact 4x4 `R8G8B8A8_UNORM` image of
  `(255, 0, 255, 255)`. Recursive SPIR-V reflection matches every host
  `offsetof` used by the push/root/vertex ABI.
- Synchronization validation completes with zero Vulkan validation warnings and
  errors. Loader policy warnings for disabled implicit overlay layers are
  reported separately and are not counted as validation messages.
- GPU-assisted validation is **unavailable with this toolchain**. Khronos
  validation layer 1.4.335 reports that it does not currently support
  descriptor buffers and disables all shader instrumentation checks. The V0
  executable returns `GPU_ASSISTED_UNAVAILABLE` rather than misreporting this as
  a clean GPU-assisted run.
- The V0 offscreen profile is viable. Its then-required window preflight is
  rejected because this
  runtime lacks `VK_KHR_surface_maintenance1` and
  `VK_KHR_swapchain_maintenance1`; WSI is not an executed V0 deliverable. The
  later accepted V3 profile supersedes that experimental requirement with the
  base-swapchain reacquisition algorithm.

Superseding GPU-assisted evidence on 2026-08-08 with SDK and validation layer
1.4.357:

- The dedicated Debug wrapper fresh-configures against the selected SDK; its
  CMake cache resolves both Vulkan headers and `vulkan-1.lib` from
  `C:/VulkanSDK/1.4.357.0`.
- Khronos validation layer 1.4.357 keeps GPU-assisted shader instrumentation
  active for the descriptor-buffer path. The indexed draw, timeline completion,
  and exact 4x4 `(255, 0, 255, 255)` readback all pass.
- The layer emits three `WARNING-Setting-Limit-Adjusted` messages while reserving
  GPU-AV limits and enabling required device features. The executable prints and
  counts those as setup notices rather than actionable diagnostics; the result
  is `setup-notices=3 warnings=0 errors=0` and `V0 RESULT PASS`. Any other
  validation warning or error still fails the gate.
- Loading validation layer 1.4.335 against the same executable still produces
  the explicit descriptor-buffer instrumentation self-disable and returns
  `GPU_ASSISTED_UNAVAILABLE`, proving the unavailable path was not weakened.

Pre-integration production V3/V4 target evidence on 2026-08-09 with the RX 6700 XT,
SDK/validation layer 1.4.357, and driver Vulkan 1.4.315:

- The offscreen validation run passes exact RGBA `{37,91,173,255}` and identifier
  `0xffad5b25` readback at both walking extents. The V4 fixture publishes and
  consumes a loaded mesh, publishes a writable sampled/storage image, replaces
  and deduplicates its sampler, and completes six exact publication draws.
- The hidden native-window validation run renders one 320×240 frame, recreates
  at 400×300, and renders eight more frames. It records five reacquisition
  proofs, exactly one retired/collected swapchain, zero live retired swapchains,
  zero command-slot waits, and `setup-notices=0 warnings=0 errors=0`.
- GPU-assisted validation remains enabled for the descriptor-buffer V4 path and
  reports its three expected setup notices with zero warnings/errors.
- The complete CPU suite passes, including present-result/reacquisition,
  non-coherent range, shared memory/ring/ABI/slot, and capacity-exhaustion
  contracts. Three new whole-process validation repetitions return identical
  live/pending/retirement totals.
- These are local dirty-tree correctness witnesses. Their native Windows scope
  is unchanged by the separate macOS extraction evidence below.

Post-change V4 completion evidence on 2026-08-10 with the same RX 6700 XT,
SDK/validation layer 1.4.357, and driver Vulkan 1.4.315 is under
`.scratch/evidence/v3-v4-windows/20260810T084205.184Z`:

- Release offscreen and three fresh Debug validation processes pass the exact
  six-draw publication fixture with `upload-waits=0`, `staging-retirement=1`,
  `memory-baseline=1`, eight balanced retirement/collection events, and
  identical live/pending/retirement totals.
- The memory witness reports `physical=7`, `bytes=47185920`, `live=31`,
  `reserved=2233112`, `retired=0`, and `capacity-failures=0` before settling the
  shared-loader initialization batch.
- Windowed validation records five reacquisition proofs, one retired and
  collected swapchain, no live retired swapchain, and no warning/error. The
  GPU-assisted process reports its three expected setup notices with zero
  warning/error. The complete CPU suite passes the pool-key, placement-rank,
  block-size, non-coherent range, allocator-core, slot-table, and WSI contracts.

This supersedes the pre-integration witness for V4 acceptance. It is functional
and memory-accounting evidence, not a frame-time claim.

Post-extraction macOS evidence on 2026-08-09 with SDK 1.4.357.0,
Slang `2026.13.1-1-g84792eb15`, Apple M1 Pro, and Metal 4:

- The complete CPU suite and Release wrapper pass, including the shared
  memory, submit-ring, ABI, and slot-table cores plus their Metal adapters.
- Clean Release snapshot `20260809T144750.194Z-00878c`
  (`sha256:1d498ecdfa982b04fa750439bd558991703cf8a9fb9f48f4e3f0e0c3b7d993ba`)
  matches the pre-extraction environment, workload, and policy fingerprints.
  Final color remains byte-identical at
  `sha256:019ba7752b653ea77dc8fce8e4125b042f67794d1978aa12044ed4c5b44ad3a6`
  and picking identifiers at
  `sha256:ed47dbf1b5e6ade6820370e0313b257c3067d667dd277a1d0033c4d204a26388`.
- Focused Release snapshot `20260809T144806.578Z-00852e`
  (`sha256:003155a6e5833aef057a4ca5744b516feb9e88eb52997e1995fbdf21c60a2e3f`)
  repeats those exact bytes with Metal API and GPU validation enabled. Its
  stderr contains only the two validation-enabled notices and no diagnostic.
- The compiler update changes generated Metal source, so the historical
  old-Slang report is not a causal extraction baseline. The clean
  fresh-toolchain pair uses pre-extraction baseline
  `20260809T152943.486Z-00b06c`
  (`sha256:f06afcd405f5b5826907fef381890b934547e0726b6f2baecbd69a971cd07710`)
  and authoritative candidate `20260809T154933.555Z-00bb51`
  (`sha256:b01763fc83ca0e1038951b4ab824ec0f91e560b62ce97efad3a704586b5000a9`).
  All three fingerprints, 1,500 valid samples, and exactly 322 calls per frame
  match. Candidate deltas are `frame.wall` mean -6.823%, p50 -3.082%, p95
  -8.881%, and prepare p50 -36.130%, all within their upper bounds. Submit p50
  is +172.421%, above the predeclared +5% bound. Earlier authoritative candidate
  `20260809T145648.867Z-008dd2`, built from the identical binary, also misses
  submit p50 at +41.920%. These historical measurements are not an open
  implementation gate under the owner-selected V5/V6 scope; no cross-backend
  speed claim is made.

Current V4-integration macOS evidence on the same toolchain:

- The Release wrapper, standalone V3/V4 target compile, and complete CPU suite
  pass. The standalone executable then rejects initialization for the expected
  missing `VK_EXT_descriptor_buffer` capability.
- Metal snapshot `20260809T192826.053Z-00f844`
  (`sha256:f739f41a712a0ee68837313434fc51b2982000b6b1ce5ea43562a8d754581eda`)
  preserves the exact final-color and picking hashes above on the newly linked
  binary. Focused API/GPU-validation snapshot
  `20260809T192835.966Z-00fbb9`
  (`sha256:5e1b4a20f9e85e3161050e84ea0926f08b06df927651a00321ac215bc8d9e576`)
  repeats those bytes and emits only the two validation-enabled notices.
- The authoritative performance profile refuses the dirty implementation tree
  by policy. Non-authoritative dirty-tree diagnostic profile
  `20260809T192443.606Z-00f53e`
  (`sha256:c60e96634c21f2ad420d50fa2829906b901b014e611246c18425b4150c855ada`)
  preserves 322 calls/frame, zero upload waits, and a 3.626 ms wall p50 while
  repeating the candidate prepare/submit phase split. Its unstable warmup and
  dirty provenance prevent it from closing or replacing the clean-tree gate.

**Not verified:** the specification's per-version mandatory-support tables. See
the verification boundary in §3.2 before writing any claim that depends on them.

---

## 13. Risks and revisit triggers

### 13.1 Split development loop

The macOS environment still cannot execute this backend, but a native Windows
machine is now available for V0. The remaining process risk is keeping the
offline/macOS and runtime/Windows halves reproducible. Mitigations in priority
order: retain the standalone offscreen target and wrapper; maximize
platform-neutral coverage (§11); and keep runtime reports self-contained so a
failure does not depend on an interactive debugger session.

Revisit trigger: MoltenVK enumerating `VK_EXT_descriptor_buffer`; Vulkan 1.4 is
already reported locally and is insufficient by itself.

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

The first native run exposed a tooling-version dependency: Khronos validation
layer 1.4.335 disables GPU-assisted shader instrumentation when descriptor
buffers are enabled, while 1.4.357 executes this V0 path successfully. Keep the
layer version in every report, keep setup adjustments visible, and continue to
treat an explicit instrumentation self-disable as
`GPU_ASSISTED_UNAVAILABLE` rather than clean GPU-assisted evidence.

Revisit: if `VK_EXT_descriptor_heap` reaches the target SDK, drivers,
validation, debugger, and shader tooling, it is the standardized successor and
closer to the article's model. Keeping index-to-descriptor translation in one
heap module contains the expected change, but acceptance must still revalidate
pipeline layouts, shader declarations, capture tooling, and the ABI; an
unimplemented extension is not evidence that the migration is literally one
file.

### 13.3 Shader toolchain

Reframed in §7.5. The two real unknowns are `PhysicalStorageBuffer` code
generation under scalar block layout and SPIRV-Reflect's visibility of those
blocks, both V0 gates with named fallbacks.

### 13.4 The shared-core extraction destabilizing the shipping Metal path

Mitigated structurally: V1 characterizes the existing contracts without moving
production code. Each core is extracted only in the later vertical slice that
adds its second real caller, with the shipping Metal snapshot as its correctness
witness. This preserves ADR-020's evidence rule instead of treating a design
document as an implementation. Performance optimization is outside the current
owner-selected V5/V6 scope.

Additional guard: **do not improve the allocator during extraction.** The one
genuine change is parameterizing the slot table by row size so it serves both
material rows and descriptor slots; keep material publication as a thin typed
wrapper so the Metal call sites are textually unchanged. If parameterization
costs measurable indirection in the Metal profile, keep two copies rather than
one slower one — performance is correctness.

### 13.5 Retirement sequencing

**No file under the legacy Vulkan tree is deleted until Gate A2 is complete, the
bindless Vulkan backend has passed V6 on Windows, and it has been the default
Windows renderer for the predeclared observation contract.** ADR-026 owns the
split gate that enforces this.

### 13.6 Accepted costs of the chosen decisions

- Requiring descriptor buffers with no fallback narrows the target matrix and,
  more importantly, the tooling matrix. Defensible for a project with one
  supported Windows machine, but recorded as a deliberate acceptance.
- Requiring descriptor buffers while `VK_EXT_descriptor_heap` is the intended
  successor creates a likely future migration. Contain descriptor-buffer
  specifics in the heap module, while still treating the later shader/layout/
  tooling gates as real work.
- Waiting to extract each shared core until its Vulkan caller exists temporarily
  leaves Metal-owned names and may require a short private walking
  implementation. That is the accepted cost of preserving the project's
  multiple-real-caller rule.
- Retaining per-image present semaphores until a submit that consumes the
  reacquired image's acquire semaphore completes avoids requiring
  swapchain-maintenance extensions, but delays collection until that proof is
  available. Swapchain maintenance remains optional and supplies explicit
  present fences when supported rather than changing the profile floor.
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
