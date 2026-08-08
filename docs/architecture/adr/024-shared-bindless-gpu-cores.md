---
status: proposed
updated: 2026-08-08
authority: adr
---

# ADR-024: Shared Backend-Neutral GPU Cores Extracted from the Metal Backend

## Status

**Proposed** — nothing is extracted yet. This ADR acts on
[ADR-020](020-bindless-backend-seam.md)'s third "Revisit When" trigger in
anticipation of a second bindless backend; the honest tension in that timing is
recorded under Consequences rather than glossed.

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

Three facts about these four make the extraction question concrete rather than
speculative:

1. **None contains a Metal type.** Their only references to Metal are in
   comments. Their headers include the project's own `defines.h`, math headers,
   and renderer headers — no Metal framework header, and no platform guard.
2. **They already compile on Windows.** The Windows arm of the library's CMake
   source glob sweeps `src/*.c` recursively, so all four are already in the
   Windows build today.
3. **Their CPU tests already run on both platforms.** The test registry calls the
   memory, material, capture-ring, and ABI suites unconditionally.

Meanwhile the
[bindless Vulkan backend design](../bindless-vulkan-backend-spec.md) needs
exactly these four behaviours, with identical contracts: the same suballocation
and retirement semantics over `VkDeviceMemory` instead of a Metal placement
heap; the same slot table for material rows *and* for descriptor-heap slots; the
same capture request lifecycle; and the same manifest validation against SPIR-V
reflection instead of Metal reflection.

Duplicating roughly 1,350 lines of tested state machines and then maintaining
two copies of the allocator, two retirement state machines, and two metrics
models is the alternative. That directly contradicts the reason the shared
metrics model exists.

## Decision

Extract exactly four modules into shared, backend-neutral C. Each backend keeps
only its device adapter and its own backend-specific record tables.

| Shared module | Extracted from | Backend keeps |
|---|---|---|
| `vkr_gpu_memory` | `vkr_metal_memory.c` | The device adapter: Metal's placement heap and residency set; Vulkan's `VkDeviceMemory` blocks, persistent mapping, and device-address capture |
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

In particular:

- Do **not** extract a low-level `VkrGpuInterface` function-pointer table.
  ADR-020 rejected that shape, and the bindless Vulkan design surfaces a concrete
  counter-example: Metal models three barrier forms — producer, consumer, and
  intra-encoder — and the intra-encoder form has no general Vulkan analogue. A
  speculative low-level seam would have gotten that wrong.
- Do **not** "improve" the allocator during extraction. The one genuine change is
  parameterizing the slot table by row size so it serves both material rows and
  descriptor-heap slots. Material publication stays a thin typed wrapper so the
  Metal call sites are textually unchanged.
- Do **not** move Vulkan-only concepts into shared code. Buffer-image granularity
  is handled by the Vulkan adapter allocating one core instance per
  class-and-kind pair, using the buffer-versus-texture parameter the core already
  carries. Metal's adapter collapses those onto its single heap. Same core,
  different adapter policy.

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

The extraction lands as stage V1 of the backend specification — **before any
Vulkan implementation code** — as a pure rename and relocation with no behaviour
change. Its required evidence is that the shipping Metal harness snapshot is
byte-identical before and after, that Metal API and GPU validation stay clean,
and that a Release Metal profile shows no frame-time regression. The CPU suite
runs on both platforms.

If a Vulkan need forces a core API change during stages V3 through V5, that
change must be re-validated against Metal with the same snapshot gate. A shared
core is not permission to change Metal's behaviour without Metal's evidence.

## Consequences

**Positive**

- One allocator, one retirement state machine, one capture lifecycle, and one
  manifest validator, each with one set of CPU tests and one metrics model.
- The Vulkan backend inherits behaviour that is already exercised by a shipping
  renderer, rather than reimplementing a state machine whose edge cases were
  already found once.
- The slot table serving both material rows and descriptor-heap slots means the
  descriptor heaps get generation safety and completion-gated recycling for free,
  which is exactly the property ADR-022 requires of texture references.
- Roughly 1,350 lines are not duplicated, and a defect fixed in the core is fixed
  for both backends.
- The extraction is verifiable without Vulkan hardware, which matters because the
  Vulkan backend cannot run on the development machine.

**Negative / risks**

- **This is technically ahead of ADR-020's stated rule**, which says extract when
  two concrete implementations exist, not when the second is planned. The risk
  accepted is that the shared core gets shaped by a hypothesis about Vulkan
  rather than by a working Vulkan implementation. The containment rule above is
  the mitigation: only modules that are already neutral move, and the second
  implementation must re-validate any change it forces.
- Parameterizing the slot table by row size introduces indirection where Metal
  previously had a concrete type. If that costs measurable time in the Metal
  Release profile, the correct response is to keep two copies rather than one
  slower one — performance is correctness.
- Shared code raises the blast radius of a defect. A regression in the core
  breaks both backends at once, where duplication would have broken one.
- The module names change, so every Metal call site and four test suites are
  touched by a change that alters no behaviour. That is a large, boring diff whose
  review value depends entirely on the snapshot witness rather than on reading it.
- Two ABI record tables plus one shared table is more structure than one table
  per backend. It is justified only because the three shared records are
  genuinely identical; if that stops being true, split them back.

## Alternatives Considered

- **Duplicate all four into Vulkan-private modules, extract later.** Safest
  against premature abstraction, and strictly what ADR-020's rule prescribes.
  Rejected because it creates roughly 1,350 lines of near-duplicate state machine
  with two places to fix every defect and two metrics models, and because the
  four modules are already demonstrably API-neutral rather than
  hypothetically so.
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

- The bindless Vulkan implementation forces a core API change that cannot be
  expressed as adapter policy. That is the signal the extraction was shaped by a
  hypothesis, and the change must be re-validated against Metal before it lands.
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
