---
status: partial
updated: 2026-08-10
authority: adr
---

# ADR-024: Shared Backend-Neutral GPU Cores Extracted from the Metal Backend

## Status

**Accepted (partial)** — V1 characterized all four candidates on macOS and
Windows. The production V3 and V4 Vulkan callers now share the extracted
`vkr_gpu_memory`, `vkr_gpu_submit_ring`, `vkr_gpu_abi`, and
`vkr_gpu_slot_table` cores with typed Metal adapters. Their RX 6700 XT callers
complete the V3 and V4 target slices, including keyed dynamic memory,
native window resize/reacquisition, loaded geometry consumption, writable
sampled/storage textures, asynchronous frame-batched initialization, canonical
shared samplers, dependent material-row republication, and material retirement.
The post-change RX 6700 XT V4 gate passes Release offscreen, three fresh Debug
validation lifecycles, windowed synchronization validation, GPU-assisted
validation, and logical-memory return to baseline. The Vulkan memory adapter
now uses the shared allocator core for keyed DEVICE, UPLOAD, and READBACK buffer
and image pools, including dynamic geometry, texture staging, and placement
images. The capture ring remains Metal-owned until its
V5 Vulkan caller exists. The complete
CPU suite passes on macOS and Windows; production Vulkan synchronization and
GPU-assisted validation pass. On macOS, clean post-extraction snapshot
`20260809T144750.194Z-00878c` is byte-identical to the pre-extraction final-color
and picking bytes, focused Metal API/GPU-validation snapshot
`20260809T144806.578Z-00852e` repeats those bytes without a diagnostic, and
the fresh-toolchain Release pair matches all fingerprints and work. Its wall
mean, p50, p95, and prepare p50 pass their predeclared upper bounds, but submit
p50 does not: authoritative candidate `20260809T154933.555Z-00bb51` is
`+172.421%` against pre-extraction baseline
`20260809T152943.486Z-00b06c`, above the `+5%` bound. An earlier authoritative
run of the identical candidate binary also misses at `+41.920%`, so the Release
extraction witness remains open even though total frame time does not regress.
This ADR remains partial for the failed Release submit bound and the V5
capture-ring extraction.

## Context

[ADR-020](020-bindless-backend-seam.md) deliberately refused to freeze a
speculative low-level GPU interface. Its rule was that a cross-backend seam is
extracted "only after Metal and modern Vulkan provide two concrete
implementations of the same operation, ownership, and completion contract," and
that shared typed records "still require multiple real callers before
extraction." That rule was correct and prevented a second one-implementation
vtable.

The Metal 4 backend has since been implemented, and four of its modules turned
out to be written as pure, API-neutral C rather than as Metal code:

| Module | Lines | What it owns |
|---|---|---|
| `vkr_metal_memory.c` | 537 | Fixed-capacity first-fit range allocator: generation handles, separate reserved and resource offsets, alignment-waste accounting, byte-exhaustion versus fragmentation classification, retirement records, per-class metrics, and the submit-ring slot state machine |
| `vkr_metal_material_table.c` | 276 | Slot table with generations and a retirement queue: publish, replace (publish-new-then-retire-old), retire, resolve, and collect, with acquire and release fences |
| `vkr_metal_capture_ring.c` | 204 | Request-owned capture slot state machine: reserve, submit, ready, acquired, released |
| `vkr_metal_packet_abi.c` | 334 | The durable host ABI manifest machinery: per-record expected size, alignment, and per-field offset, validated against `sizeof`, `_Alignof`, and `offsetof` |

Three facts make these credible extraction candidates, while not by themselves
satisfying the second-caller rule:

1. **None contains a Metal type.** Their only references to Metal are in
   comments. Their headers include the project's own `defines.h`, math headers,
   and renderer headers — no Metal framework header, and no platform guard.
2. **They already compile on Windows.** The Windows arm of the library's CMake
   source glob sweeps `src/*.c` recursively, so all four are already in the
   Windows build today.
3. **Their CPU tests already run on both platforms.** The test registry calls the
   memory, material, capture-ring, and ABI suites unconditionally.

Meanwhile the
[bindless Vulkan backend design](../bindless-vulkan-backend-spec.md) predicts it
will need
exactly these four behaviours, with identical contracts: the same suballocation
and retirement semantics over `VkDeviceMemory` instead of a Metal placement
heap; the same slot table for material rows *and* for descriptor-heap slots; the
same capture request lifecycle; and the same manifest validation against SPIR-V
reflection instead of Metal reflection.

The prediction is strong enough to define an extraction boundary, but a design
document is not a second implementation. The sequencing below permits a short
Vulkan-private walking implementation where needed, then compresses only after
the second call site demonstrates the common contract.

## Decision

Extract at most these four modules into shared, backend-neutral C, one at a time
when a real bindless Vulkan caller for that module lands. Each backend keeps only
its device adapter and its own backend-specific record tables. If the Vulkan
caller forces a different ownership or completion contract, leave that module
private rather than weakening either backend's invariants to make the table come
true.

| Shared module | Extracted from | Backend keeps |
|---|---|---|
| `vkr_gpu_memory` and `vkr_gpu_submit_ring` | `vkr_metal_memory.c` | The device adapter: Metal's placement heap and residency set; Vulkan's `VkDeviceMemory` blocks, persistent mapping, and device-address capture |
| `vkr_gpu_slot_table` | `vkr_metal_material_table.c`, parameterized by row size | The storage adapter: Metal's shared write-combined buffer; Vulkan's upload-class suballocation. Also the row type itself |
| `vkr_gpu_capture_ring` | `vkr_metal_capture_ring.c` | The blit or copy that fills a slot, and the readback storage |
| `vkr_gpu_abi` | The manifest machinery in `vkr_metal_packet_abi.c` | Its own record table and its own reflection cross-check |

Three ABI records are genuinely shared by both backends — the vertex, instance,
and text-vertex records — and become one shared table both consume. Every other
record stays with its backend: the Metal material row and draw root remain
Metal's, and the Vulkan material row and draw root are Vulkan's. The material
rows differ deliberately (64 bytes of 32-bit indices versus 96 bytes of 64-bit
resource identifiers) and are **not** unified; ADR-022 already rejected
standardizing every backend on a 64-bit texture token.

### The containment rule

**Extract only what is already API-neutral and already compiles on Windows.
Extract nothing else.**

API neutrality is necessary but not sufficient: the extracting change must
contain two production or representative integration callers. Do not create a
shared forwarding wrapper whose only caller remains Metal.

In particular:

- Do **not** extract a low-level `VkrGpuInterface` function-pointer table.
  ADR-020 rejected that shape, and the bindless Vulkan design surfaces a concrete
  counter-example: Metal models three barrier forms — producer, consumer, and
  intra-encoder — and the intra-encoder form has no general Vulkan analogue. A
  speculative low-level seam would have gotten that wrong.
- Do **not** "improve" the allocator during extraction. The one genuine change is
  parameterizing the slot table by row size so it serves both material rows and
  descriptor-heap slots. Material publication stays a thin typed wrapper so the
  Metal call sites are textually unchanged. The generic table owns slot state,
  not resource sharing: Vulkan's typed publisher retains and reference-counts
  shared image/sampler publications so retiring one material cannot free a slot
  another material still references.
- Do **not** move Vulkan-only concepts into shared code. Buffer-image granularity
  is handled by the Vulkan adapter pooling blocks by
  `(class, kind, memory_type_index, device_address_required)`, using the
  buffer-versus-texture parameter the core already carries and the exact Vulkan
  memory requirements. The final bit prevents address-bearing buffers from
  entering memory allocated without the device-address flag. Metal's adapter
  collapses those keys onto its placement heap. Same core, different adapter
  policy.

### No VMA

The Vulkan adapter does not use the Vulkan Memory Allocator. Reasons, on the
record so this is not relitigated: the extracted core is already a tested
allocator with the accounting model the project's metrics depend on; VMA is C++
while the build gates this codebase to C11 through its declared-C-functions
check; [ADR-006](006-cpu-memory-allocators.md) and
[ADR-007](007-gpu-memory-allocation.md) own allocator policy; and the entire
point of this extraction is one allocator with one metrics model across both
backends, which adding a second allocator would undo.

### Sequencing and its witness

Stage V1 of the backend specification characterizes the four contracts and pins
their CPU tests; it moves no production module. Extraction then follows the
first real Vulkan use:

- memory, submit-ring, and ABI machinery with the V3 walking renderer;
- the slot table with V4 material/descriptor publication;
- the capture ring with V5 asynchronous capture.

Within each vertical slice, write the representative Vulkan use first, confirm
the ownership/completion contract matches, and then extract the observed
intersection. The required witness for every extraction is a byte-identical
shipping Metal snapshot, clean Metal API/GPU validation, the CPU suite on both
platforms, and a same-configuration Release Metal profile showing no material
regression under a tolerance declared before the paired runs. The Vulkan slice
supplies its own Windows runtime evidence.

Any later core API change must be re-validated against Metal with the same
witness. A shared core is not permission to change Metal's behaviour without
Metal evidence.

## Consequences

**Positive**

- One allocator, one retirement state machine, one capture lifecycle, and one
  manifest validator, each with one set of CPU tests and one metrics model.
- Once each second caller exists, the Vulkan backend shares behaviour already
  exercised by a shipping renderer rather than retaining a duplicate state
  machine whose edge cases were already found once.
- The slot table serving both material rows and descriptor-heap slots means the
  descriptor heaps get generation safety and completion-gated recycling for free,
  which is exactly the property ADR-022 requires of texture references.
- Long-lived duplication of roughly 1,350 lines is avoided, and a defect fixed
  in a proven shared core is fixed for both backends.
- The shared-core invariants and Metal regression witnesses remain locally
  verifiable; the second caller's integrated behavior still requires Windows
  Vulkan hardware.

**Negative / risks**

- Extraction is spread across V3–V5 instead of one mechanically convenient V1
  rename. That produces smaller, behavior-bearing diffs but temporarily leaves
  Metal-owned names and may require a short Vulkan-private walking
  implementation before compression.
- Parameterizing the slot table by row size introduces indirection where Metal
  previously had a concrete type. If that costs measurable time in the Metal
  Release profile, the correct response is to keep two copies rather than one
  slower one — performance is correctness.
- Shared code raises the blast radius of a defect. A regression in the core
  breaks both backends at once, where duplication would have broken one.
- Module names change only when a second caller lands, so every extraction mixes
  a behavior-bearing Vulkan slice with Metal renames. The bounded per-module
  sequencing and dual-backend witnesses are what keep that diff reviewable.
- Two ABI record tables plus one shared table is more structure than one table
  per backend. It is justified only because the three shared records are
  genuinely identical; if that stops being true, split them back.

## Alternatives Considered

- **Extract all four before any Vulkan caller exists.** Mechanically simple and
  locally testable, but rejected because API-neutral syntax is not evidence of a
  shared ownership/completion contract. It would violate ADR-020 and the project
  N+1 rule.
- **Keep full Vulkan-private copies indefinitely.** Safest against a shared-core
  regression, but rejected once matching real callers exist because it creates
  roughly 1,350 lines of duplicate state machines, fixes, and metrics. A short
  private walking use before extraction is allowed; permanent duplication is
  not the target.
- **Extract a full low-level `VkrGpuInterface` function-pointer table now.**
  Rejected. ADR-020 rejected this shape explicitly, and the Metal-versus-Vulkan
  barrier asymmetry is concrete evidence that a low-level seam designed before the
  second implementation would encode the wrong contract.
- **Extract more than four modules — for example the packet renderer's frame
  orchestration.** Rejected under the containment rule. Those modules are not
  API-neutral today, and their shape is exactly what the second implementation
  exists to test. The umbrella specification's contingency to share bindless
  orchestration applies only after both backends prove it.
- **Adopt VMA for the Vulkan adapter and leave Metal on its own allocator.**
  Rejected above. It reintroduces the two-allocator, two-metrics situation the
  extraction exists to prevent, and it conflicts with the C11 constraint.
- **Unify the Metal and Vulkan material rows so the slot table needs no row-size
  parameter.** Rejected because it requires either Metal carrying unproven
  compact texture-pool indices or Vulkan carrying 32 bytes of padding for
  nothing. ADR-022 already recorded the rejection of standardizing on a 64-bit
  token.

## Revisit When

- A bindless Vulkan caller does not match a candidate's ownership or completion
  contract. Leave that candidate private or split the contract; do not extract
  it merely because this ADR listed it.
- The Metal Release profile shows measurable regression from row-size
  parameterization or any other extraction-induced indirection.
- The allocator core's fragmentation, largest-free-range, or range-metadata
  metrics show churn that a binning or buddy allocator would fix, at which point
  the allocator's data structure is revisited for both backends at once.
- A third backend appears and needs a fifth shared module, or shows that one of
  these four was not as neutral as it looked.
- The three shared ABI records stop being identical across backends.
- ADR-020's low-level `VkrGpuInterface` question is reopened because the two
  bindless backends turn out to share more operation-level contracts than the
  coarse strategy in [ADR-025](025-selected-renderer-implementation-strategy.md)
  captures.
