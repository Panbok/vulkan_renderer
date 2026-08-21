---
status: investigation
updated: 2026-08-21
authority: investigation
---

# Bindless Renderer Audit — Metal 4 and Vulkan 1.4

**Document status:** Source audit of the post-V7 tree at commit `e028164`,
performed 2026-08-12 against `lib/src/renderer/` (both selected
implementations), the four shared GPU cores, and
`lib/src/renderer/vulkan/bindless/shaders/packet.slang`.
**Recommendations 1–11 are implemented in source as of 2026-08-12; see §7.**
The recommendation-10 ABI now passes native Metal reflection, focused API/GPU
validation, and exact final-color/picking capture on Apple M1 Pro (§7.7).

**Post-audit source note (2026-08-21):** The sole Vulkan implementation now
lives directly under `lib/src/renderer/vulkan/` and uses `vkr_vulkan_*` /
`VkrVulkan*` source identifiers. Old paths, symbols, and line citations below
remain unchanged because they record the audited `e028164` tree rather than the
current source layout.

**Authority:** This is an *audit* document. Per `AGENTS.md`, code remains the
implementation authority, `renderer-architecture-spec.md` the status authority,
and the ADRs the rationale authority. Nothing here changes a status claim; every
finding names the file and line it was read from so it can be confirmed or
refuted directly.

**Scope:** (1) how bindless each backend actually is, (2) correctness risks,
(3) hot-path and memory issues, (4) alignment with the engine's own primitives
(`defines.h`, `containers/`, `core/vkr_threads.h`, `core/vkr_atomic.h`,
`memory/`), and (5) refactoring opportunities.

**Explicit non-goal:** every performance statement below is a *static* claim
about bytes written, allocations made, or algorithmic complexity. Per the
guiding principle in `AGENTS.md`, none of it is a measured result, and none of
it should be quoted as one. Where a change is proposed, the required
measurement is named with it.

---

## 1. Verdict: are we fully bindless?

**Short answer: resource binding and the indexed resource-data model are fully
bindless on both backends; draw submission remains CPU-driven.**

The design's own definition — textures reached by index or resource ID,
everything else reached by GPU address, no per-draw descriptor rebind — is met.
Per-draw CPU work now writes a 32-byte record containing the frame-root address,
vertex address, material index, and first instance. Frame/pass data is written
once per non-empty pass, and immutable material parameters live in the published
GPU material row. The remaining per-draw command call is a submission-strategy
question covered by ADR-013, not a resource-binding gap.

| Axis | Metal 4 | Bindless Vulkan 1.4 | Assessment |
|---|---|---|---|
| Texture access | `MTLResourceID` in the material row + residency sets | 32-bit index into a `VK_EXT_descriptor_buffer` heap, `NonUniformResourceIndex` | **Fully bindless.** No per-draw descriptor or argument-buffer binding on either side |
| Sampler access | `MTLResourceID`, canonical and deduplicated | Index into the separate sampler heap, canonical, reference-counted | **Fully bindless** |
| Buffers (vertex, instance, material, light, cascade, probe, text) | 64-bit GPU addresses | 64-bit device addresses via `PhysicalStorageBuffer` | **Fully bindless.** Zero storage-buffer descriptors exist |
| Index buffers | `indexBuffer:` takes an address | `vkCmdBindIndexBuffer2` per draw (`vkr_bindless_vulkan_renderer.c:2346`) | **Not bindless on Vulkan, unavoidably.** Correctly recorded as the one divergence in the design spec §5.6 |
| Material identity and parameters | 176-byte immutable row, table-indexed | 144-byte immutable row, table-indexed | **Bindless.** Populated at publication, not per draw |
| Frame/pass constants | One 432-byte GPU-addressed frame root per non-empty indexed packet pass | One 432-byte device-addressed frame root per non-empty indexed packet pass | **Bindless data path.** Draw roots reference the frame root |
| Draw submission | One `drawIndexedPrimitives` per `VkrDrawItem` | One `vkCmdDrawIndexed` per `VkrDrawItem` | **CPU-driven.** No indirect, no MDI, no GPU culling. ADR-013 already records this as measured-future work |
| Pipelines | Fixed enum, bound per pass | Fixed enum, bound per pass (`:2293`) | Not a bindless concern; noted for completeness |

### 1.1 Historical finding and implemented resolution

At the audited `e028164` tree, `VkrBindlessVkPacketDrawRoot` was **exactly 512
bytes**, and `vkr_bindless_vk_fill_packet_root()` wrote all 512 of them **for
every draw**. Metal did the same thing in `vkr_metal_packet_frame.inc`.

Of those 512 bytes, by field:

- **~12 bytes are genuinely per-draw**: `vertices`, `material_index`,
  `first_instance`.
- **~84 bytes are per-material**: `material_emissive`,
  `material_dielectric_specular`, `material_surface`, `material_alpha`,
  `material_attenuation_color`, `alpha_mode`. These are already resident in
  `VkrBindlessVkPublishedMaterial::pbr` and could live in the GPU material row.
- **~416 bytes are per-frame or per-pass constants**: `view_projection`, `view`,
  `view_position`, `ambient_color`, `ibl_controls`, both directional-light
  vectors, the entire point-light grid block, `shadow_cascades`,
  `shadow_cascade_count`, `shadow_bias`, `ibl_probes`, `ibl_probe_count`, the
  five IBL/shadow/transmission texture-sampler pairs, `materials`,
  `render_mode`, `prefilter_mip_count`.

So roughly **80% of per-draw upload bandwidth is a re-copy of data that did not
change**. The material row exists and is correct, but the renderer routes most
material state around it.

This was not a wrong-pixel defect. It was the reason the original audit answered
"the binding model is; the data model isn't yet."

**Implemented shape:**

1. Indexed world, shadow, picking, editor, and UI passes allocate one 432-byte
   frame root per non-empty pass and one 48-byte table-driven draw root per draw.
   Text, skybox, and
   fullscreen entry points retain the legacy 512-byte utility record because
   they do not traverse world material state.
2. The five per-material `Vec4`s and alpha mode are immutable fields of the
   published material row. The resulting rows are 144 bytes on Vulkan and 176
   bytes on Metal.
3. Host assertions plus recursive SPIR-V reflection pin the Vulkan draw, frame,
   and material layouts. The Metal manifest pins the same three nested record
   families and is checked through host tests plus runtime pipeline reflection.

This changes the world-draw root payload from 512 bytes per draw to 32 bytes per
draw plus 432 bytes per non-empty pass. That is a static byte-count result, not a
measured speedup. The local Release observation in §7.6 is non-authoritative and
supports no faster/slower claim.

The source implementation and both selected-backend reflection/capture gates
now pass. Section 7.7 records the native Metal closure evidence.

---

## 2. Correctness findings

Ordered by severity. Each names its read site.

### 2.1 Metal and Vulkan compute specular IBL differently — HIGH

`packet.slang:677` computes the split-sum BRDF term analytically:

```
float2 brdf = packet_brdf_approximation(no_v, roughness);
```

`metal/shaders/world/default.metal:145-146` samples a baked LUT:

```
float2 brdf = root->brdf_lut.sample(environment_sampler, float2(no_v, roughness)).rg;
```

Both then feed the identical `(fresnel * brdf.x + f90 * brdf.y)` specular term.
Metal bakes the LUT (`metal/shaders/ibl/brdf_lut.metal`) and publishes it
(`vkr_metal_packet_frame.inc:121`). Vulkan has **no BRDF pipeline at all** —
`VkrBindlessVkIblPipeline` (`:119-123`) contains only `IRRADIANCE` and
`PREFILTER`.

Consequences:

- The two backends produce **different specular IBL for the same scene**. The
  magnitude is unmeasured and may be small, but it is not zero and it is not
  documented anywhere.
- The bindless-vulkan spec's claim that the Metal snapshot is the cross-backend
  reference is weakened: an exact-bytes comparison across backends cannot
  succeed while this divergence exists.
- `VkrBindlessVkPacketDrawRoot::brdf_texture` and `::brdf_sampler`
  (`:190-191`) are **declared in both the C struct and the Slang struct, and
  never written** — they hold the MemZero'd sentinel index forever. They pass
  the reflection cross-check (the offsets match) while carrying no meaning. This
  is exactly the kind of silently-dead ABI field the manifest is supposed to
  prevent.

**Action:** decide explicitly which is authoritative. Either (a) implement the
Vulkan BRDF bake and consume the two root fields, or (b) switch Metal to the
analytic approximation and delete `brdf_lut` from both roots and both shaders.
Option (b) is smaller and removes an IBL target, an image, two descriptor slots,
and a compute pipeline from Metal. Either way, record the decision — right now
the divergence is undocumented and only discoverable by reading both shaders.

### 2.2 Buffer barriers are designed but not implemented — MEDIUM

The design spec §8.3 specifies per-buffer `VkBufferMemoryBarrier2` emission
batched into the pass barrier command, and the spec's status header claims V5 is
complete. The code does not do this:

- `vkr_bindless_vk_record_graph_pass_barriers()` (`:1854-1859`) **rejects the
  frame** with `log_error` if any pass declares a buffer barrier.
- `vkr_bindless_vk_validate_graph()` (`:861`) rejects the same at load.
- `renderer->graph_buffer_barriers` is allocated (`:4675`), zeroed (`:4697`),
  and freed (`:8407`) — and **never written or read**. It is a dead allocation
  sized, incorrectly, by `max_graph_images` (`:4666`).

This is defensible as a deliberate "no caller yet" position, but the spec
should say so rather than describing the emission as shipped. The dead
allocation should go, and if the rejection is intentional it belongs behind a
named error code rather than a bare `false_v` plus a log line.

### 2.3 Geometry and texture records are sized by the descriptor-heap capacity — MEDIUM

`vkr_bindless_vk_create_descriptor_slot_tables()` (`:3801-3824`) sizes
**five unrelated arrays** by `config.sampled_image_capacity`:

```c
renderer->published_geometries_size = sampled_image_capacity * sizeof(...);
renderer->retired_geometries_size   = sampled_image_capacity * sizeof(...);
renderer->published_textures_size   = sampled_image_capacity * sizeof(...);
renderer->retired_textures_size     = sampled_image_capacity * sizeof(...);
renderer->pending_texture_initializations_size = sampled_image_capacity * ...;
```

and `vkr_bindless_vk_resolve_geometry()` (`:2140-2141`) enforces it as the
geometry-ID bound:

```c
if (!renderer || handle.id == 0u || handle.id > renderer->config.sampled_image_capacity)
```

There is **no `geometry_capacity` field** in `VkrBindlessVulkanRendererConfig`
(`vkr_bindless_vulkan_renderer.h:19-50`). Two consequences:

1. **Semantic.** A scene with more geometries than sampled images silently fails
   geometry resolution and the affected draws are rejected. The failure is
   reported as an invalid draw, not as a capacity limit.
2. **Memory.** At the shipping `sampled_image_capacity = 16384`
   (`renderer_frontend.c:610`), `VkrBindlessVkPublishedTexture` is roughly 460
   bytes (it embeds `VkrBindlessVkImage` plus `storage_views[16]` and
   `storage_slots[16]`), so `published_textures` + `retired_textures` alone
   reserve on the order of **15 MiB**, with geometry and pending-initialization
   arrays adding several more. Almost all of it is untouched in any real scene.

`graph_buffer_barriers` (§2.2) is the same class of mistake with a different
array.

**Action:** give geometry its own capacity knob; size each array by its own
capacity; and consider replacing the dense ID-indexed arrays with
`lib/src/containers/vkr_freelist.h`, which already exists and is tested
(`tests/src/freelist_test.c`).

### 2.4 The shared slot table uses raw C11 fences that prove nothing — MEDIUM

`vkr_gpu_slot_table.c:3` includes `<stdatomic.h>` and issues bare fences:

- `:137` `atomic_thread_fence(memory_order_release)` after the row `MemCopy`,
  before the plain store to `slots[i].state`.
- `:229` `atomic_thread_fence(memory_order_acquire)` in `resolve`.

Neither fence is paired with an atomic load or store. `VkrGpuSlot::state` and
`::generation` are plain `uint32_t`/enum, and every metrics counter is a plain
`uint64_t`. If two threads ever touch this table, the fences do not make it
safe — the accesses they order are themselves data races, and the metrics
counters are unsynchronized read-modify-write. If only one thread touches it,
which the current code guarantees (publication runs from
`vkr_resource_system_pump()` inside `prepare_frame`,
`renderer_frontend.c:1977-1982`), the fences are dead code that reads as a
thread-safety guarantee it does not provide.

This is also the audit's single clearest engine-primitive violation: the project
ships `lib/src/core/vkr_atomic.h` with `VkrAtomicUint32`/`VkrAtomicUint64` and
an explicit `VkrMemoryOrder`, and this file bypasses it for the raw C11 API.

**Action:** pick one. Either delete both fences and document the
single-threaded invariant in the header, or make `state`, `generation`, and the
metrics counters real `VkrAtomic*` types via `vkr_atomic.h` and add a contract
test that exercises concurrent publish/resolve. Deleting is almost certainly
right; the table is called only from the render thread.

### 2.5 Enum-value and struct-pointer casts across the shared-core seam — MEDIUM

`vkr_metal_material_table.c` translates between the Metal-typed API and the
shared core with unchecked casts:

- `(VkrMetalMaterialStatus)vkr_gpu_slot_table_publish(...)` (`:47`) and six
  more. `VkrGpuSlotStatus` has five enumerators;
  `VkrMetalMaterialStatus` has six, with `NATIVE_ALLOCATION_FAILED` appended.
  The first five happen to agree today. **Nothing pins that** — no
  `_Static_assert`, no test. Reordering either enum silently mistranslates every
  error code, including turning `CAPACITY_EXHAUSTED` into `STALE_HANDLE`.
- `(VkrGpuSlotHandle *)out_handle` (`:48`) and `(VkrGpuSlotTable **)out_table`
  (`:40`) cast between distinct struct types. Layouts are identical, so it works
  everywhere it is compiled, but it is a strict-aliasing violation the compiler
  is permitted to exploit.

**Action:** translate statuses explicitly and convert output handles through
local values. Layout assertions can catch ABI drift, but they do not make
pointer access through two distinct struct types legal under C's aliasing
rules.

### 2.6 Unconditional per-frame 1×1 readback copy — LOW

`vkr_bindless_vk_record_draw()` (`:5832-5904`) records a
`vkCmdCopyImageToBuffer2` of a single pixel plus a `vkCmdPipelineBarrier2` to
`HOST` **every frame**, whether or not picking is pending. When picking is not
pending it copies pixel (0,0) of the color target into `slot->readback` and
nobody reads it. It is cheap, but it is unconditional GPU work and an
unconditional host-visibility barrier on the critical path.

**Action:** gate on `slot->picking_readback_pending` unless a caller depends on
the always-present pixel (the exact-readback gates in the spec do, so check
`vkr_bindless_vulkan_renderer_get_pixel_readback_result` before removing).

### 2.7 Frame-upload exhaustion fails the frame silently — LOW

`vkr_bindless_vk_frame_upload_allocate()` (`:1921-1939`) returns `NULL` on
exhaustion; `vkr_bindless_vk_record_packet_draws()` (`:2335-2336`) turns that
into `return false_v`, which fails the entire frame with **no log, no metric,
and no way to distinguish it from a malformed draw**. With a 16 MiB per-slot
budget (`:85`) and a 512-byte root per draw, the ceiling is ~32k roots per
frame before instance, text, cascade, and probe data — reachable, and the
failure mode is opaque.

**Action:** count exhaustion into a pre-registered metric per ADR-015 and log it
once per frame at `warn`.

### 2.8 `create()` leaves a non-NULL out-pointer on most failure paths — LOW

`vkr_bindless_vulkan_renderer_create()` sets `*out_renderer = renderer` at
`:4655` and then has **fifteen** subsequent `return false_v` paths that leave it
set, while the **first four** failure paths free the renderer and leave it
`NULL`. The frontend handles this correctly (`renderer_frontend.c:654-657` calls
`destroy` on the possibly-non-NULL pointer), so this is not a live bug — but it
is an undocumented two-mode contract on a public function with no doc comment,
and the first four paths triplicate the same teardown sequence
(`:4612`, `:4620`, `:4632`, `:4651`).

**Action:** document the contract in the header, and collapse the early paths
onto one `goto cleanup;` per the `AGENTS.md` error-handling rule.

---

## 3. Algorithmic issues in the shared GPU cores

These are the three files with production callers on both backends, so a
regression here is a two-platform regression.

### 3.1 Linear free-slot scan on every publication

`vkr_gpu_slot_table.c:106-112` and `vkr_gpu_memory.c:123-129` both do:

```c
for (uint32_t i = 0; i < max; ++i)
  if (slots[i].state == FREE) return i;
```

At the shipping capacities — `sampled_image_capacity = 16384`,
`material_slot_capacity = 16385` (`renderer_frontend.c:610-614`) — publishing
*n* resources costs O(n²) slot inspections. Scene load publishes thousands of
textures and materials back-to-back, so this is a real load-time cost, and it
degrades as the table fills, which is exactly when it is scanned longest.

`lib/src/containers/vkr_freelist.h` already exists, is tested, and solves this.
A free-index stack embedded in the existing storage block would be a ~20-line
change with no API impact and no per-frame cost.

### 3.2 Quadratic retirement compaction

`vkr_gpu_slot_table.c:252-253` and `vkr_gpu_memory.c:391-392` both shift the
entire retirement tail down by one **for each collected record**:

```c
for (uint32_t j = i + 1u; j < retirement_count; ++j)
  retirements[j - 1u] = retirements[j];
retirement_count--;
```

Collecting *k* of *n* retirements costs O(k·n). Retirement records are unordered
by contract (they are matched by `submit_value`, not position), so a swap-remove
with the tail element is O(1) and behaviourally identical. Alternatively a
single compacting pass over the array is O(n) total.

Scene reload and texture-replacement storms are the paths that hit this.

### 3.3 Peaks are summed across pools

`vkr_gpu_memory_metrics_accumulate()` (`:437-443`, `:461-462`) adds
`peak_requested_bytes`, `peak_reserved_bytes`, and `peak_allocations` across
pools. The sum of per-pool peaks is an upper bound on, not equal to, the global
peak — pools rarely peak simultaneously. `largest_free_range` is correctly
handled with `Max` at `:431`. The reported `memory.gpu.*.peak` rows are
therefore conservative by an unknown margin.

**Action:** either document that these rows are "sum of per-pool peaks" or track
a real global peak at the aggregation site. Do not leave the ambiguity in a
metric that gates capacity decisions.

### 3.4 Contract tests are named for the wrong owner

`vkr_gpu_memory`, `vkr_gpu_slot_table`, `vkr_gpu_submit_ring`, `vkr_gpu_abi`,
and `vkr_capture_ring` are shared cores with two production callers each.
Their contract tests are still named `metal_capture_ring_test.c`,
`metal_material_test.c`, `metal_memory_test.c`, `metal_packet_abi_test.c` —
`metal_capture_ring_test.c` includes only `renderer/vkr_capture_ring.h` and
contains nothing Metal. `bindless_vulkan_test.c` covers the same cores from the
other side.

The V1 characterization plan justified the naming when the modules were
Metal-owned. They are not any more. Renaming to `gpu_slot_table_test.c`,
`gpu_capture_ring_test.c`, etc., and keeping backend-specific adapter tests
separate, would make it obvious which failures are shared-core regressions.

---

## 4. Alignment with engine primitives

### 4.1 `defines.h` — `vkr_internal` is not used in either backend

The codebase uses `vkr_internal` in **1,152 places** across `lib/src`. The two
renderer backends do not:

| File | plain `static` at file scope | `vkr_internal` |
|---|---|---|
| `vulkan/bindless/vkr_bindless_vulkan_renderer.c` | 169 | 0 |
| `vulkan/bindless/vkr_bindless_vulkan_device.c` | 16 | 0 |
| `vulkan/bindless/vkr_bindless_vulkan_memory.c` | 3 | 0 |
| `metal/internal/*.inc` (6 files) | 95 | 0 |
| `metal/*.m`, `metal/*.c` | 21 | 1 |
| `renderer_frontend.c` | 94 | (mixed; uses both) |
| `vulkan/bindless/vkr_bindless_vulkan_dependency.c` | 0 | 3 |
| `vkr_gpu_memory.c` / `vkr_gpu_slot_table.c` / `vkr_gpu_abi.c` / `vkr_capture_ring.c` | 19 | 0 |

`vkr_bindless_vulkan_dependency.c` is the only file in either backend that
follows the convention. This is purely cosmetic — `vkr_internal` *is* `static` —
but it is a 300+ site inconsistency in the largest and newest subsystem, and it
makes `rg vkr_internal` an unreliable way to find internal linkage.

Everything else in `defines.h` is used correctly: `MemCopy`/`MemZero` (83 uses
in the bindless renderer, zero raw `memcpy`/`memset`), `ArrayCount`, `Min`/`Max`,
`AlignPow2`, `VKR_SIMD_ALIGN`, `true_v`/`false_v`, `bool8_t`. That part is
genuinely clean.

One local exception worth fixing: `vkr_bindless_vk_align_up()` (`:1917-1919`)
reimplements `AlignPow2` from `defines.h:14` verbatim.

### 4.2 `containers/` — the backends use none of them

Neither backend includes `array.h`, `vector.h`, `str.h`, `bitset.h`,
`vkr_freelist.h`, `vkr_hashtable.h`, or `queue.h`. The frontend, systems, and
loaders use them extensively; the render graph uses `Vector(T)`; the two
backends are container-free.

Some of that is correct — fixed-capacity, allocation-free frame paths should not
use growable containers. But three places are paying for the absence:

1. **Fifteen hand-rolled `{pointer, size}` pairs.**
   `struct VkrBindlessVulkanRenderer` (`:503-604`) carries ten explicit
   `*_size` fields beside their pointers, all allocated in one 45-line block
   (`:3839-3883`), null-checked in one 12-line condition (`:3884-3895`),
   zeroed in one 13-line block (`:3896-3908`), and freed one-by-one in
   `destroy`. `Array(T)` from `containers/array.h` gives `.data`, `.count`,
   `.capacity` and removes the parallel size fields entirely. This is a
   mechanical ~120-line reduction with no behavioural change.
2. **Linear free-slot scans** (§3.1) where `vkr_freelist.h` is the existing
   answer.
3. **Fixed `char` buffers for paths.** `pipeline_cache_path[1024]` (`:585`)
   plus `strlen`/`snprintf`/`fopen` (`:629`, `:642`, `:710`, `:713`) while the
   file already includes `filesystem/filesystem.h` and the project has
   length-prefixed `String8`. The pipeline-cache read/write path is the only
   raw stdio file I/O in either backend.

`vkr_bindless_vulkan_device.c` uses `snprintf` into fixed `char` report buffers
(11 sites) — that one is defensible: the capability report is deliberately
fixed-capacity and allocation-free per design spec §3.4.

### 4.3 `core/vkr_threads.h` — correct where used, absent from the backends

The frontend and systems use `vkr_mutex_*` consistently (74 sites across
`renderer_frontend.c`, `vkr_texture_system.c`, `vkr_material_system.c`,
`vkr_resource_system.c`). No raw `pthread_*` anywhere in the renderer. Good.

The backends contain **no synchronization at all**, which is correct — they are
render-thread-only, and publication is pumped from inside `prepare_frame`
(`renderer_frontend.c:1977-1982`). That invariant is load-bearing and is stated
only in a comment at that one site. It should be stated in
`vkr_asset_publisher.h` and in both backend headers, because the asset-publisher
function pointers are reachable from systems that *do* have async paths and it
is not obvious from the signatures that they must not be called from a worker.

`core/vkr_atomic.h` exists and is well-formed; only `vkr_gpu_slot_table.c`
bypasses it (§2.4).

### 4.4 Allocators — the two backends disagree

| | Bindless Vulkan | Metal |
|---|---|---|
| Renderer struct | `vkr_allocator_alloc` | `calloc` (`vkr_metal_packet_setup.inc:774`) |
| Sub-arrays | `vkr_allocator_alloc` ×15 | `calloc` ×11 (`setup.inc:800-818`) |
| Core storage | `vkr_allocator_alloc` | `malloc` (`vkr_metal_memory_device.m:95-97`) |
| Per-frame graph memory | `Arena` + `VkrAllocator` | `Arena` + `VkrAllocator` |
| Vertex/index conversion | staged through pooled UPLOAD | `calloc` per mesh (`resources.inc:341,357`) |

Metal has a `VkrAllocator *allocator` field (`vkr_metal_packet_renderer.h:34`)
and uses it only for the graph frame arena. **45 raw `calloc`/`malloc`/`free`
sites** in `metal/` bypass the engine allocator, so those bytes are invisible to
`VkrAllocator` tagging, leak accounting, and the memory metrics that ADR-006
and ADR-015 exist to provide. `vkr_metal_packet_resources.inc:341,357` calls
`calloc` **per loaded mesh** in the resource path.

This is the single largest engine-primitive divergence in the audit, and it is
on the platform that is used for daily iteration.

**Action:** route Metal's allocations through the `VkrAllocator` it already
holds, tagged `VKR_ALLOCATOR_MEMORY_TAG_RENDERER`, matching what the Vulkan side
already does. The per-mesh `calloc` in the resource path should use the
publication staging path instead.

---

## 5. Structure and maintainability

### 5.1 `vkr_bindless_vulkan_renderer.c` is 8,431 lines

It is the largest file in the repository by a factor of 2.3 and contains
**196 functions** spanning: device wiring, memory adapters, image and buffer
creation, graph realization, barrier recording, dynamic rendering, packet draw
recording, text recording, IBL dispatch, capture planning, swapchain creation
and retirement, frame slots, descriptor tables, SPIR-V reflection, pipeline
creation and caching, timeline management, and the entire twelve-entry asset
publisher.

Metal solved the same problem by splitting into six `.inc` files included into
one translation unit — which preserves single-TU inlining but defeats every
tool that works on files (jump-to-definition, per-file blame, incremental
compile). Neither shape is good.

The natural seams are already visible in the function-name prefixes and would
each be a real translation unit with a narrow header:

| Proposed unit | Current functions | Approx. lines |
|---|---|---|
| `..._resources.c` | `create_buffer`, `create_image*`, `destroy_*`, `release/retire_allocation`, `flush`/`invalidate`/`mark_dirty` | ~900 |
| `..._graph.c` | `realize_graph_images`, `record_graph*`, `graph_attachment`, `graph_image_usage`, executors | ~1,300 |
| `..._draws.c` | `fill_packet_root`, `record_packet_draws`, `record_text_draws`, `record_packet_skybox`, `record_packet_fullscreen`, `record_graphics_body` | ~900 |
| `..._ibl.c` | `record_ibl_*`, `queue_ibl_bake`, `cmd_ibl_image_barrier` | ~400 |
| `..._capture.c` | `plan_capture`, `record_capture`, `capture_source`, `capture_align` | ~350 |
| `..._target.c` | window/swapchain create, destroy, retire, collect, present-mode and format choice | ~700 |
| `..._publisher.c` | the twelve `asset_*` entry points plus texture/sampler/material publication | ~2,000 |
| `..._pipelines.c` | shader modules, packet/IBL pipelines, reflection ABI validation, pipeline cache | ~800 |
| `..._renderer.c` (remaining) | create/destroy, prepare_frame, submit_packet, poll, metrics accessors | ~1,000 |

The blocker is `struct VkrBindlessVulkanRenderer` (102 fields), which every unit
touches. The honest sequencing is: move the struct into a private
`vkr_bindless_vulkan_internal.h`, split the leaf units first (`_capture`,
`_ibl`, `_target`), and only then attempt `_publisher` and `_graph`.

**This must not be done as a single change.** Per ADR-020's evidence rule and
the design spec's own extraction discipline, each split needs the Windows
offscreen/windowed validation gate and an exact-capture witness before the next
one starts.

### 5.2 The two backends duplicate ~400 lines of root-filling logic

`vkr_bindless_vk_fill_packet_root()` (`:2176-2279`, 104 lines) and the
`*root = (VkrMetalPacketDrawRoot){...}` initializer in
`vkr_metal_packet_frame.inc:100-215` (116 lines) compute **the same values from
the same `VkrRenderPacket`**, field for field, including the same
`packet->lighting ? ... : vec4_zero()` ternaries, the same
`1.0f / viewport_width` reciprocals, and the same `shadow_bias = 0.0001f`
literal in two places.

They differ only in where the result lands: 32-bit heap indices versus 64-bit
resource IDs, and the field order.

A shared `VkrPacketDrawConstants` struct in backend-neutral code, computed once
per pass and lowered by each backend into its own root layout, would remove the
duplication and — more importantly — make it structurally impossible for the two
backends to drift the way §2.1 shows they already have.

This is also the prerequisite for the §1.1 frame/draw root split: once the
frame-constant computation is shared and hoisted out of the draw loop, both
backends get the bandwidth reduction from one change.

### 5.3 Smaller items

- **`goto cleanup` is essentially unused.** One `goto` in 8,431 lines of
  bindless Vulkan, zero in all of `metal/`, against an `AGENTS.md` rule that
  prefers it. The cost is visible in `create()` (§2.8) and in
  `create_frame_slots` (`:3655-3674`), which returns `false_v` mid-loop leaving
  earlier slots' pools and buffers to be cleaned up by the caller's `destroy`.
  That works because `destroy` is null-tolerant, but it is implicit.
- **Magic sentinel `0u` for samplers.** `root->shadow_sampler = 0u` and
  `root->transmission_sampler = 0u` (`:2192`, `:2194`) hardcode the sentinel
  index rather than naming it. Add
  `VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX = 0` and use it — the sentinel contract
  is load-bearing (design spec §4.4) and currently expressed as a bare literal
  in four places.
- **Three runtime arrays alias one binding.** `packet.slang:150-152` declares
  `g_textures[]`, `g_cube_textures[]`, and `g_array_textures[]` all at
  `[[vk::binding(0, 0)]]`. This is a legitimate and common bindless technique,
  but nothing on the host side records which view type a given sampled slot
  holds, so a 2D texture indexed as a cube is undetectable until GPU-assisted
  validation catches it. Storing the view type alongside the slot and asserting
  it at material-row construction would cost one byte per slot and make the
  hazard a CPU-side failure.
- **Dirty-range flush is a hull, not a union.**
  `vkr_bindless_vk_flush_publication_ranges()` (`:1113-1134`) flushes
  `[min_offset, max_end)`. Publishing slot 0 and slot 16383 in one batch flushes
  the whole heap. Correct, and only costs anything on non-coherent UPLOAD
  memory, which the target device does not expose — but worth a comment saying
  so, since the design spec §5.4 says "union of written ranges" and this is the
  bounding interval.
- **Manual ABI padding in the shader.** `packet.slang:93` declares
  `uint2 shadow_address_padding` to match the C compiler's implicit 8 bytes
  before `Mat4 view`. It is correct today and the reflection cross-check would
  catch a break — but it is hand-maintained, and the C side has no comment
  explaining why the shader has a field it does not. Add one on both sides.

---

## 6. Prioritized recommendations

Ordered by (risk removed) ÷ (risk introduced). Every item below is independently
landable.

| # | Change | Why now | Gate required |
|---|---|---|---|
| 1 | Resolve the BRDF divergence (§2.1) and delete or wire the dead root fields | Backends currently render differently; this is the only finding that changes pixels | Exact-capture witness on the changed backend |
| 2 | Give geometry its own capacity; stop sizing five arrays by `sampled_image_capacity` (§2.3) | Silent draw rejection plus ~20 MiB of unused reservation | CPU suite; publication fixture |
| 3 | Delete the fences or adopt `vkr_atomic.h` in `vkr_gpu_slot_table.c` (§2.4) | Removes a false thread-safety signal in a two-backend shared core | CPU suite on both platforms |
| 4 | `_Static_assert` the enum pairing in `vkr_metal_material_table.c` (§2.5) | Two lines; prevents a silent error-code mistranslation | Compile |
| 5 | Free-list the slot/allocation scans and swap-remove the retirement compaction (§3.1, §3.2) | O(n²) → O(n) on scene load, in shared code | `metal_material_test`, `bindless_vulkan_test`, Metal snapshot |
| 6 | Route Metal's 45 `calloc`/`malloc` sites through its existing `VkrAllocator` (§4.4) | Restores ADR-006/ADR-015 accounting on the iteration platform | Metal snapshot byte-identical |
| 7 | Delete `graph_buffer_barriers`; correct the spec's §8.3 status (§2.2) | Dead allocation and a status claim the code contradicts | Compile; doc update |
| 8 | Split the frame-constant computation into shared backend-neutral code (§5.2) | Prerequisite for #10; structurally prevents another §2.1 | Exact captures on both backends |
| 9 | Split `vkr_bindless_vulkan_renderer.c`, leaf units first (§5.1) | 8,431 lines is the main maintainability cost in the tree | Full Windows gate **per split** |
| 10 | Frame-root / draw-root split and move material params into the GPU row (§1.1) | The actual "fully bindless" gap; ~80% of per-draw upload bytes | Predeclared-tolerance Release pairs on both backends **plus** exact captures |
| 11 | Adopt `vkr_internal` in both backends (§4.1) | Cosmetic; do it opportunistically inside #9, not as its own change | Compile |

**Status: recommendations 1–11 and their declared cross-backend correctness
gates are complete (§7).**

The audit originally required a matched Release profile before starting item
10. The user later explicitly authorized the source implementation despite the
unavailable authoritative cross-platform pair. Section 7.6 records that
deviation and makes no performance claim; matched evidence and exact captures
from both selected backends remain the acceptance policy. Section 7.7 records
the completed Metal half of that policy.

---

## 7. Implementation record (2026-08-12)

The initial pass implemented recommendations 1–8 and 11. What follows is what
changed and what proved it; §§7.5–7.7 record the later Windows implementation
and native Metal closure work.
Every gate in §7.1 ran on macOS against Vulkan SDK 1.4.357.0
(the repository minimum; the machine's default `VULKAN_SDK` still points at
1.4.313.0, which cannot compile the bindless backend — see §7.1).

### 7.1 Evidence

| Gate | Result |
|---|---|
| CPU suite (`build_test.sh`) | 460 passing, up from 456; the four new tests are named below |
| `build.sh Debug` | clean, no new warnings |
| `build_release.sh` | clean |
| Metal text snapshot | `final_color` `sha256:019ba775…`, `picking_ids` `sha256:ed47dbf1…` — **byte-identical to the pre-change baseline and to the reference recorded in the bindless spec** |
| Metal API + GPU validation, focused text case | pass; stderr contains only the two validation-enabled notices |
| Metal IBL snapshot | intentionally changed by the BRDF resolution; quantified below |

**A pre-existing environment defect surfaced first.** The baseline CPU suite
fails outright with the machine's default `VULKAN_SDK` (1.4.313.0): the bindless
backend uses `VkSwapchainPresentFenceInfoKHR`,
`VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR`, and
`VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT`, none of which exist in those headers.
`AGENTS.md` already requires 1.4.357+; the environment simply does not point at
it. Nothing here changes that, but every gate above was run with
`VULKAN_SDK` set explicitly, and it is worth a shell profile fix.

### 7.2 What changed

**1 — BRDF divergence resolved (Metal → analytic).** Metal now calls
`vkr_metal_packet_brdf_approximation()`, numerically identical to
`packet_brdf_approximation()` in `packet.slang`. Removed: the `brdf_lut.metal`
kernel, `VKR_METAL_PACKET_ABI_BRDF_ROOT` and `VkrMetalPacketBrdfRoot`, the
`ibl_brdf` RGBA16F target and its create/retire/collect plumbing, the per-bake
dispatch, the write-only `ibl_brdf`/`ibl_brdf_size` result fields, the capture
blit that read the LUT back, and the CMake rule.

*Measured effect:* on the local-IBL fixture, **4.20% of pixels changed, every
one of them by exactly 1/255** (max channel delta 1, mean 1). The two forms
agree to within one 8-bit quantization step. The text snapshot, which has no
specular IBL, is byte-identical — which is also what proves the ABI edit below
was layout-neutral.

*Deviation from the plan, stated explicitly:* the dead root fields were retired
as **named padding** (`reserved_brdf_lut` on Metal, `reserved_brdf_texture` /
`reserved_brdf_sampler` on Vulkan) rather than deleted. Deleting them would have
been offset-neutral only because C, MSL, and Slang each independently pad a
`float4` to 16 bytes — three toolchains agreeing implicitly, two of which cannot
be validated on this machine. Explicit padding makes the offsets provable. Their
ABI manifest rows were removed because an unused scalar is eliminated from the
reflected struct, so the row asserted a member that no longer exists; the
neighbouring `prefilter@96` and `view_position@112` rows still pin the layout on
both sides of the hole.

**2 — Capacity conflation fixed.** `VkrBindlessVulkanRendererConfig` gains
`geometry_capacity` and `texture_capacity`. All 24 conflated uses of
`sampled_image_capacity` were routed to the correct one; the four genuine
descriptor-heap uses remain. `create()` now rejects a `texture_capacity`
exceeding the sampled-image heap, since every published texture also consumes a
sampled slot. The frontend sets both to 16384 to match the geometry and texture
systems' actual ID spaces, so behaviour is unchanged — the fix is that the three
are now independent and validated rather than accidentally equal.

**3 — Slot-table fences removed.** `vkr_gpu_slot_table.c` no longer includes
`<stdatomic.h>`. The two unpaired fences are gone and the single-threaded
contract is documented on the type in the header, including *why* a bare fence
would not have helped and what to do (`core/vkr_atomic.h`) if that ever changes.

**4 — Enum and handle casts removed.**
`vkr_metal_material_table.c` now translates every shared-core status through an
exhaustive switch and converts output handles through local values. No
out-parameter is accessed through an incompatible struct pointer, so the seam
does not depend on matching enum ordinals or strict-aliasing accidents.

**5 — O(n²) removed from both shared cores.** `vkr_gpu_slot_table.c` and
`vkr_gpu_memory.c` now take free slots from a free-index stack instead of
scanning, and collect by swap-remove instead of shifting the tail per record.
Publication order from a virgin table is unchanged (0, 1, 2, …), which the
sentinel contract depends on; reuse after collection is now LIFO, which the
header states no caller may depend on.

The allocator needed one thing the scan did not: it reserves a handle slot
*before* searching for a byte range, so every range-search failure must return
that slot. New test `test_metal_memory_failed_allocation_returns_handle`
exercises sixteen consecutive byte-exhaustion failures and then proves both
remaining handles are still available — without the return, the third attempt
would report `OUT_OF_HANDLES`. New test
`test_shared_slot_table_recycles_every_collected_slot` runs ten full
drain/refill cycles and pins both invariants.

**6 — Metal allocations routed through `VkrAllocator`.** All 45 raw
`calloc`/`malloc`/`free` sites are gone; `grep` finds none under
`lib/src/renderer/metal/`.

This did not work on the first attempt and the failure is worth recording.
`max_meshes * max_submeshes_per_mesh` is 16384 × 512, so the submesh array alone
is **exactly 96 MiB** — larger than the whole renderer arena. `calloc` hid that
because the pages were never touched and therefore never backed; a committing
allocator cannot. The record arrays now share one dedicated `VkrDMemory`
reservation (the pattern the bindless Vulkan renderer already uses for capture
storage), which keeps them from displacing every other renderer allocation and
collapses ten sized frees to one destroy. Both dedicated reservations are
exposed through `VkrAllocator`, so their suballocations remain tagged; teardown
releases the aggregate accounting immediately before the bulk destroy. The
large submesh array is not zero-filled at startup because entries are read only
below a live mesh's initialized count. **The 16384 × 512 worst-case product is
the real problem and is left alone deliberately** — it is a capacity decision,
not a plumbing one.

**7 — Dead allocation removed.** `graph_buffer_barriers` and its size field are
gone. The rejection path now names the pass and the barrier count instead of
returning a bare `false_v`, and the image-barrier bound check explains why the
count is not implied by the image count. The design spec's §8.3 claim was
corrected to say buffer-barrier emission is designed but not implemented.

**8 — Shared draw constants extracted.** New
`lib/src/renderer/vkr_packet_constants.{h,c}` owns the frame- and
material-constant derivation both backends previously duplicated. Both now call
it and copy into their own root layout. At this intermediate stage, frame
constants were derived once per pass while material constants remained per draw.
Recommendation 10 subsequently moved material derivation to publication and
made the resulting row immutable (§7.6). The shadow bias literal exists once;
the larger frame helper stays out of line because it runs once per pass.
Deliberately not shared: resource references, device addresses, and the
backend-specific proof that IBL resources are ready. The resulting lighting,
IBL, and transmission flag semantics are now shared (§7.6).

*Proof it was neutral:* both Metal snapshots are byte-identical across this
change. Focused CPU tests pin the complete frame/material mapping, the unlit
defaults, and the defensive zero-extent behavior.

**11 — Engine linkage macros adopted.** 155 file-scope functions became
`vkr_internal`, 17 file-scope data tables `vkr_global`, and 5 function-local
statics `vkr_local_persist`, across both backends and the shared cores. The only
remaining bare `static` in these files are header `static inline` definitions
and shader source, both correct. Files reformatted with `clang-format`.

### 7.3 Follow-up implementation review

A second source-and-runtime review tightened the implementation without changing
the accepted rendered bytes:

- frame constants are derived once before each backend's draw loop, while the
  per-material helper is force-inlined at the draw call site;
- the Metal material adapter translates statuses and handles explicitly instead
  of accessing output objects through incompatible struct pointers;
- Metal renderer creation now uses full ownership-aware teardown on early
  failures, and its dedicated record/capture reservations participate in tagged
  allocator accounting;
- the 96 MiB worst-case submesh reservation is not eagerly zero-filled; live
  mesh counts remain the initialization boundary;
- the Vulkan draw root has an exact 512-byte compile-time assertion; and
- two backend-neutral constants tests pin the frame defaults, full lighting
  mapping, material mapping, and zero-extent defense.

The final Release text snapshot
(`build/_artifacts/snapshot/20260812T132542.968Z-00e919/report.json`) retained
the pre-review `final_color` and `picking_ids` digests, and the final local-IBL
snapshot (`20260812T132544.611Z-00e8ad`) retained its pre-review final-color
digest. Both reports passed but remain non-authoritative because the profile is
local, the tree is dirty, and no accepted baseline is installed.

### 7.4 Found while implementing and closed by the completion work

- **A second write-only BRDF path existed in the frontend.** The original audit
  found a 128×128 allocation. At the completion-pass base (`7ed07ec`), that
  allocation was already absent, but `VkrWorldResources::ibl_brdf_lut`,
  `VkrMaterialSystem::ibl_brdf_lut`, and the unused setter parameter remained.
  The completion pass removed that residual state and API surface (§7.5).
- **The `flags`/`material_flags` predicate differed between backends.** The
  shared frame-flag contract in §7.6 closes it.
- **Frame-upload exhaustion was counted and logged** per §2.7, but was not a
  pre-registered ADR-015 metric row. The completion pass added the row (§7.5).

### 7.5 Windows completion pass

Recommendation 9 is implemented at base `7ed07ec`. The former 8,413-line
`vkr_bindless_vulkan_renderer.c` is now a 1,509-line lifecycle coordinator plus
dependency-closed capture, draw, graph, IBL, pipeline, publisher, resource, and
target translation units. Shared private state and cross-unit declarations live
in `vkr_bindless_vulkan_internal.h`; no textual `.inc` partition was introduced.
The renderer/publisher ownership boundary is unchanged. The audit prescribed a
full Windows gate after each independently landed split; this completion pass
performed the dependency-closed extraction as one workspace change, so the
recorded full gate covers the aggregate result rather than intermediate slices.

The pass also closed two source-level follow-ups from §7.4:

- the remaining write-only frontend `ibl_brdf_lut` state and setter parameter
  were removed; and
- upload exhaustion now publishes the pre-registered
  `frame.upload_exhaustions` count metric through the renderer metrics registry.

The Windows evidence was: a green CPU suite before and after the change;
successful Debug and Release builds; byte-identical Debug and Release offscreen
text captures (`final_color` and `picking_ids`); and two passing repetitions of
a focused hidden-window validation case. The existing resize case reached the
requested three-image BGRA8-sRGB/D32/FIFO window configuration but was
unavailable because `resize.outbound_not_observed`. The local reports are
non-authoritative because the profile is local, no accepted baseline is
installed, and the working tree is dirty; they support correctness only and no
faster/slower claim.

At the end of that pass, two items still remained intentionally unchanged. Both
were subsequently implemented at the user's direction in §7.6:

- the cross-backend IBL predicate; and
- recommendation 10's frame-root/draw-root and material-row ABI change.

### 7.6 Recommendation 10 source implementation

The user explicitly requested completion despite the unavailable authoritative
cross-platform measurement prerequisite. The implementation preserves the
evidence policy: it makes no performance claim, does not mutate an accepted
baseline, and defers the native Metal closure evidence to §7.7.

The completed source change is:

- one shared flag contract: bit 0 enables lighting, bit 1 enables IBL only when
  the pass enables lighting, the packet requests IBL, and the backend has proved
  its derived maps ready, and bit 2 enables transmission;
- immutable material rows populated at publication, including the five PBR
  vectors and alpha mode (144 bytes Vulkan, 176 bytes Metal);
- one 432-byte frame root per non-empty indexed packet pass and one 32-byte draw
  root per draw; and
- nested host/shader ABI manifests covering the draw root, frame root, and
  material row. Vulkan validates the hierarchy recursively from SPIR-V at
  pipeline creation. Metal retains host-manifest tests and runtime reflection;
  offline Slang-to-Metal generation checks the shader half on Windows.

The material replacement and retirement paths did not change ownership: a
logical replacement publishes a new immutable row, and physical reuse remains
submit-completion gated. Source inspection finds no material derivation,
allocation, lock, string construction, pipeline creation, or wait in the draw
loop. The legacy 512-byte utility root remains intentionally scoped to text,
skybox, and fullscreen draws.

The implementation also exposed a build-system hole: shader custom commands
could succeed without invoking `slangc`, leaving stale SPIR-V to mask ABI
changes. `lib/CMakeLists.txt` now resolves `slangc` with a required
`find_program`; a clean test-tree build therefore recompiles every entry point.
The existing explicit-binding-overlap warnings remain expected for the shared
descriptor-buffer binding model.

Windows and offline evidence after the ABI change:

- a clean `build_test.bat` tree compiled every packet shader and passed the full
  CPU suite, including shared flag derivation, immutable Metal material-row,
  host ABI, and metric-catalog tests;
- fresh Debug and Release application/harness builds passed;
- all 14 production packet SPIR-V modules passed `spirv-val` for Vulkan 1.4
  with scalar-block layout, and Release runtime pipeline creation passed the
  recursive draw/frame/material reflection check;
- Release text snapshot `20260812T150910.384Z-00352d` passed and retained the
  exact pre-change final-color and picking hashes;
- local Release Bistro profile `20260812T151645.365Z-003d38` passed two
  repetitions and 600 measured frames with exactly 206 world calls (191 opaque,
  15 transparent) and zero `frame.upload_exhaustions` in every sample; and
- focused post-ABI Debug synchronization-validation profile
  `20260812T155805.852Z-000699` passed two repetitions with one indexed world
  call per measured frame, zero upload exhaustion, deterministic work volume,
  and empty child stderr; and
- offline Slang-to-Metal generation of `library.slang` passed with only the
  pre-existing ignored-entry-parameter-register warning.

The Bistro profile is explicitly non-authoritative (`profile.local_only`, dirty
provenance, unstable warmup). Its fingerprints match the pre-change observation,
but that observation had different executed draw work, so timing deltas are not
comparable and no performance result is reported. The focused Debug profile is
a correctness witness only: validation was enabled, its local/dirty profile is
non-authoritative, and unstable warmup prevents any timing interpretation. A
broader Debug Bistro attempt completed one child but exceeded the bounded
diagnostic window during unoptimized asset-manifest hashing; it remains
unavailable and is not needed for the focused command-recording verdict.

### 7.7 Native Metal closure

The native gate initially did its job and rejected two latent ABI-contract
defects before renderer creation:

- runtime validation treated Metal's reflected pointer-element and buffer
  alignments as exact host alignments. Xcode defines them as minimums; packed
  Slang records can therefore report 4- or 8-byte minimum alignment while the
  host safely places the same size/offset layout at 16-byte alignment. The
  validator now requires the host authority to be an equal-or-stronger multiple
  of the reflected minimum, with a focused CPU regression for compatible and
  incompatible cases;
- two Slang `uint3` padding members gave the generated Metal frame root 16-byte
  vector alignment, shifting later fields and producing a 464-byte record.
  Expressing those six padding words as scalars matches the host/native-MSL
  arrays and restores the reflected 432-byte frame-root contract. Runtime ABI
  diagnostics now report every mismatched field in one attempt.

On Apple M1 Pro with Metal 4, the final source passed `./build_test.sh`,
`./build_release.sh`, and `./build.sh Debug`. A normal Release, three-image,
960×540 offscreen text snapshot passed as report
`20260812T163826.676Z-01310f`, digest
`sha256:e47e599b4eda4f4c89ffb6fddcc039e4b7b00fa741af38360e702943bbb80126`.
Native pipeline creation/reflection accepted the draw, frame, material, vertex,
instance, probe, and cascade hierarchy, and child stderr was empty. Its exact
captures are:

- `final_color`:
  `sha256:019ba7752b653ea77dc8fce8e4125b042f67794d1978aa12044ed4c5b44ad3a6`;
- `picking_ids`:
  `sha256:ed47dbf1b5e6ade6820370e0313b257c3067d667dd277a1d0033c4d204a26388`.

A separate focused Debug replay with `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1` passed as report
`20260812T163901.620Z-0136ab`, digest
`sha256:59d09423ed774a8c091638b08ab8b2dd86a2e687441e8ad54e7daadac9deab69`.
Its stderr contains only the Metal API/GPU validation enablement notices, and
both capture digests are identical to Release. These local dirty-tree snapshots
are correctness witnesses, not accepted baselines or performance evidence; no
baseline was proposed or mutated. They close the audit because the missing gate
was native ABI/validation/exact-byte correctness, not performance authority.

## 8. Limits and evidence boundaries

The initial source audit and the subsequent implementation have different
evidence boundaries. The findings in §§1–6 were produced by reading the tree at
`e028164`; no code was executed while forming those findings. The implementation
and focused follow-up review did execute the gates recorded in §7. Nothing here
is evidence for platforms or configurations that were not run.

- **No authoritative performance result was produced.** The byte counts in
  §1.1, the complexity claims in §3, and the memory estimates in §2.3 are static
  analysis. Local profiles were marked unavailable for performance evidence
  (`profile.local_only`, dirty provenance, unstable warmup, and, for some
  witnesses, a present-mode mismatch). The recommendation-10 before/after
  observations also executed different draw work. Their timings support no
  faster/slower claim.
- **The BRDF divergence was initially a source finding.** The implementation
  subsequently measured its capture effect as recorded in §7.2; those local
  Metal results do not establish a cross-platform performance or visual result.
- **`sizeof(VkrBindlessVkPublishedTexture)` was computed by hand**, not
  compiled. The ~460-byte figure and the derived ~15 MiB are approximate; the
  conclusion (arrays sized by the wrong capacity) does not depend on the exact
  number.
- **The original 512-byte root size was initially computed by hand.** The
  implementation now pins the 432-byte frame root, 48-byte draw root, 144-byte
  Vulkan material row, and retained 512-byte utility root with compile-time and
  reflection checks.
- **Linux, mesh shaders, device-generated commands, shader objects, and the
  other §3.5 optional capabilities were out of scope**, as were the harness,
  the ECS, and the asset loaders except where they call the publisher.
