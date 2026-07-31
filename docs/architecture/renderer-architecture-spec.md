---
status: implemented
updated: 2026-07-31
authority: spec
---
# VKR Renderer — Architecture and Status Specification

**Document status:** Reviewed against the working tree on 2026-07-31
**Scope:** Renderer architecture, implemented features, CPU/GPU memory, data
transfer, synchronization, known issues, and recommended direction.
**Audience:** Contributors and reviewers.
**Companion:** [Architecture Decision Records](adr/README.md)

This is a status snapshot, not a promise that every design document under
`docs/` describes current code. Paths and symbols are preferred over volatile
line counts.

---

## 1. Executive Summary

VKR is a substantial Vulkan 1.2 renderer and engine framework written in C11.
Its strongest implemented work is architectural: a public frontend/backend
boundary, packet-based frame submission, a JSON-authored render graph, SPIR-V
reflection for descriptor and vertex layouts, explicit allocator families,
asynchronous CPU resource preparation, an ECS scene model, editor rendering,
picking, CSM, text, glTF, and a KTX2/UASTC asset pipeline.

It is beyond tutorial scale, but several claims in the earlier version of this
document overstated how completely those systems are integrated:

- The render packet is a useful frame contract, but submission begins the
  Vulkan frame before packet validation and mutates retained renderer systems.
  It is packet-based, not a purely stateless or replayable renderer.
- The render graph schedules and instruments declared work, but image barrier
  execution currently uses layout pairs rather than the compiler's access
  masks. Same-layout hazards and subresource-granular state are not handled,
  and picking/IBL executors perform undeclared rendering outside graph-owned
  passes.
- SPIR-V reflection drives descriptor layouts, push constants, and vertex ABI,
  but it does not currently reflect uniform members or cross-validate their
  offsets against `.shadercfg` declarations.
- Resource preparation is asynchronous, but Vulkan upload finalization waits
  on fences. The dedicated transfer queue is a synchronous upload path today,
  and parallel upload command pools require an explicitly unsafe opt-in.
- The world path still has no active frustum culling, draw batching, or MDI.
  Its instance stream is live, but most draws contain one instance.

There are also correctness issues that should take precedence over new visual
features: frame-stream buffers are indexed by swapchain image modulo three,
backend begin/end-frame failures are sometimes returned as success, render
graph execution failures do not propagate to packet submission, and rejected
packets end and present an already-acquired frame using an unsafe assumed old
layout.

The project remains a strong renderer showcase. Its next step should be to make
the current architecture trustworthy under failure and across swapchain
configurations, then demonstrate measured throughput improvements.

---

## 2. Project Structure

```text
lib/src/
├── application.h              Application lifecycle and packet assembly
├── core/                      Events, jobs, input, JSON, threads, ECS
├── math/                      Vector/matrix/quaternion math and frustum code
├── memory/                    Arena, DMemory, pool, allocator adapters
├── platform/                  Window and platform implementations
└── renderer/
    ├── renderer_frontend.c/h  Public orchestration and subsystem ownership
    ├── vkr_renderer.h         Public handles, descriptions, and API
    ├── vkr_render_packet.h    Versioned per-frame payload contract
    ├── vkr_render_graph.*     Graph declaration and resource management
    ├── vkr_rg_compile.c       Dependencies, scheduling, culling, barriers
    ├── vkr_rg_execute.c       Pass execution and timing
    ├── vkr_rg_json.c/h        JSON graph parsing and realization
    ├── passes/                Named pass executors
    ├── systems/               Scene-facing renderer subsystems
    ├── resources/loaders/     Asset loaders
    └── vulkan/                Vulkan backend and SPIR-V reflection
app/src/main.c                 Sample/editor application
assets/                        Shaders, materials, scenes, textures, graph JSON
tools/                         Offline texture packer and validation scripts
tests/src/                     CPU-side unit and subsystem tests
docs/                          Specifications, plans, investigations, ADRs
```

The renderer frontend owns the renderer subsystems and communicates with the
Vulkan backend through `VkrRendererBackendInterface`. Public resources use
opaque handles; Vulkan types stay behind the backend boundary. This is a real
seam, although it has only one implementation and still exposes concepts shaped
by Vulkan's render-pass and descriptor model.

---

## 3. Frame Architecture

### 3.1 Actual frame sequence

The normal frame path is:

1. `vkr_renderer_prepare_frame()` calls `vkr_renderer_begin_frame()`.
2. The backend waits for a frame-slot fence, acquires a swapchain image, resets
   and begins that image's command buffer.
3. Async resource finalization is pumped and the instance/indirect stream
   cursors are reset.
4. The application creates `VkrRenderPacket` and its arrays in caller-owned
   scratch memory.
5. `vkr_renderer_submit_packet()` validates the packet, applies retained text/UI
   updates and globals, realizes the JSON graph for the current frame, executes
   it, ends the command buffer, submits, and presents.

This ordering matters: swapchain acquisition and command recording have
already begun by the time validation runs. An invalid packet is therefore not
a zero-side-effect rejection.

### 3.2 Packet-based submission

`VkrRenderPacket` version 2 contains frame information, globals, and optional
world, shadow, skybox, UI, editor, picking, text-update, and debug payloads.
Important properties are implemented:

- optional payload pointers control pass participation;
- version and field validation produce `VkrValidationError` with a field path;
- payload arrays remain caller-owned and must live until submission returns;
- draw items refer to stable renderer handles rather than Vulkan objects;
- passes retrieve typed payloads from `VkrRgPassContext`.

The boundary should be described as **packet-based** or **value-like**, not
fully stateless:

- `prepare_frame` is a required, ordered operation;
- submission updates retained globals, UI/text resources, descriptor state,
  graph caches, upload queues, and metrics;
- the packet is not self-contained enough to serialize and replay without the
  retained handle registries and subsystem state;
- validation failures call `vkr_renderer_end_frame()` after image acquisition.

### 3.3 JSON render graph

`assets/render_graphs/main.rendergraph.json` is parsed into a reusable JSON
model at renderer initialization. On every submitted frame,
`vkr_rg_build_from_json()` resets and realizes the active resources and passes
for current dimensions, editor condition, and cascade count; `vkr_rg_execute()`
compiles the graph if needed and runs it.

Implemented graph capabilities:

- declared image/buffer reads, writes, and attachments;
- dependency construction, topological ordering, and pass culling;
- conditions and `${i}` repeat expansion;
- runtime extent/format aliases;
- named executor registry;
- graph-owned/imported resources and generation-bearing handles;
- CPU timing and optional buffered GPU timestamp results;
- live/peak graph-resource statistics.

The source JSON contains five resource declarations and ten pass declarations.
After conditions and the four-cascade repeat are resolved, the normal topology
contains nine active declarations without the editor composite and ten with it.
The fullscreen/editor variants are alternatives, not additive passes.

#### Synchronization boundary

The compiler records access and layout transitions, but execution is not yet a
complete access-aware synchronization implementation:

- image execution drops `src_access`/`dst_access` and calls
  `vkr_renderer_transition_texture_layout(old_layout, new_layout)`;
- image barriers are skipped when layouts match, so write→read or write→write
  hazards in the same layout are not represented;
- buffer barriers are likewise skipped when access flags match, even when a
  same-access write hazard may require ordering;
- image state and barriers cover whole images even when attachments name a
  mip/layer slice;
- the backend supports a finite set of old/new layout pairs rather than a
  general stage/access mapping;
- all graph passes record on the graphics command buffer; compute/transfer pass
  kinds do not provide queue scheduling or ownership-transfer semantics.

The graph is also not the sole authority over current GPU work. The picking
executor opens its own retained render pass/target, and the IBL executor invokes
helpers that create targets and record several graphics passes. Those internal
resources and accesses are not declared in the JSON graph. This must be fixed
before graph inference can be treated as a correctness guarantee.

`VKR_RG_RESOURCE_FLAG_TRANSIENT` currently means graph-owned/reusable rather
than “freed after each frame”: resources survive between realizations and are
recreated when their resolved description changes. Transient aliasing is not
implemented.

### 3.4 Shader and pipeline construction

SPIR-V reflection is implemented with `spirv_reflect` and currently supplies:

- descriptor sets, bindings, descriptor type/count, and block byte sizes;
- push constant ranges;
- vertex inputs used to build/validate supported host vertex ABIs;
- semantic role resolution for frame/draw sets and bindings.

`.shadercfg` remains important for shader identity, stages, render pass,
`vertex_abi`, and the frontend's named uniform declarations and CPU offsets.
However, `vulkan_spirv_shader_reflection_create()` currently sets
`uniform_block_count` to zero. There is no member-by-member reflection or
validation of manifest names, types, array strides, or offsets against SPIR-V.
Descriptor writes are validated by set/binding/type/count, but that does not
protect the CPU uniform staging layout from manifest drift.

Static assertions correctly pin `VkrVertex3d`, `VkrInstanceDataGPU`, and
`VkrIndirectDrawCommand` host layouts. Pipeline cache persistence is also
implemented.

### 3.5 Scene and render extraction

`VkrWorld` is an archetype ECS with registered component layouts, entity
generation/world validation, archetypes, and compiled queries.
`VkrScene` adds transform hierarchy, visibility, renderer references, lights,
text/shapes, stable render IDs, environment IBL state, and a fixed-size array of
scene-level reflection probes.

The scene is authoritative retained state, but packets are not extracted
directly from ECS archetype arrays. `vkr_scene_handle_sync()` mirrors dirty
scene state into `VkrMeshManager`; application packet construction then scans
mesh-manager slots and submeshes. Packet construction performs a count pass and
a population pass. This bridge is useful for decoupling, but it duplicates
state and means ECS locality is not yet the direct draw-collection path.

Scene resource loading is asynchronous at the CPU preparation level and is
activated once dependency/GPU finalization completes.

---

## 4. Feature Status

| Area | Status | Current boundary |
|---|---|---|
| Frontend/backend interface | Implemented | One Vulkan backend; portability seam unproven |
| Packet submission | Implemented, partial | Versioned/validated, but ordered and state-mutating |
| JSON render graph | Implemented, partial | Scheduling/culling/timing; incomplete access synchronization and graph coverage |
| SPIR-V reflection | Implemented, partial | Descriptors, push constants, vertex ABI; no uniform-member reflection |
| Pipeline cache | Implemented | Disk-backed Vulkan cache |
| Cascaded shadow maps | Implemented | Four-cascade default with debug/fit controls |
| PBR materials | Implemented, evolving | Metallic-roughness and texture slots present |
| IBL | In progress | Runtime bake paths and scene/probe model present; graph integration incomplete |
| glTF and scene loading | Implemented | CPU async pipeline with render-thread finalization |
| KTX2/UASTC textures | Implemented, partial | BC7/BC5, ASTC, ETC2, and RGBA32 paths; normal-map fallback gap |
| Editor viewport and picking | Implemented | Picking readback usually deferred but ring wrap can block |
| Text | Implemented | Bitmap, MTSDF, system-font, UI and world text paths |
| Instance stream | Implemented, underused | Fixed 65,536 entries; current world extraction usually emits one instance/draw |
| CPU frustum culling | Not integrated | Frustum module has no production call site |
| Draw batching | Not integrated | Batcher module has no production call site |
| Multi-draw indirect | Plumbed, unused | Stream/backend entry exists; no command allocation caller |
| Compute dispatch | Not exercised | “compute” JSON passes currently orchestrate graphics/CPU work |
| Device-memory suballocation | Absent | One `VkDeviceMemory` allocation per Vulkan buffer/image/readback buffer |
| Bindless/descriptor indexing | Absent | Per-material descriptor binding model |
| HDR/tonemap/post chain | Absent | Post domain is reserved |
| Shader hot reload | Absent | Build-time shader compilation only |

---

## 5. CPU Memory

### 5.1 Allocator model

`VkrAllocator` adapts three allocation backends:

| Backend | Lifetime model | Typical use |
|---|---|---|
| `Arena` | Bump allocation and bulk reset/destroy | Scratch or data sharing one lifetime |
| `VkrDMemory` | Reserved/committed virtual memory with individual free | Registries, keys, reloadable objects |
| `VkrPool` | Fixed-size slots | Homogeneous objects |

`VkrArenaPool` is a separate thread-safe pool of fixed-size chunks used to make
asset-loader scratch arenas. It is not a fourth `VkrAllocator` backend.

The interface provides optional scopes, aligned operations, tagged local/global
accounting, and `_ts` wrappers that synchronize through a caller-supplied
mutex. Allocator objects are not intrinsically thread-safe. Global counters are
atomic.

Arena bulk destruction must be paired with
`vkr_allocator_release_global_accounting()` when global tag totals must remain
accurate. The scene runtime does this before destroying its scene arena. The
repository guidelines correctly require freeable allocation for removable hash
keys and acquire/release symmetry for renderer handles.

These policies reduce scene-reload growth; they do not prove that all reload
paths are leak-free. Repeated-load measurements and handle counters remain the
required validation.

### 5.2 Vulkan host allocator

Vulkan `VkAllocationCallbacks` use a dedicated `VkrDMemory` reservation plus a
small refcounted command-scope arena. This accounts driver host allocations in
the project's allocator statistics. It is unrelated to GPU device-memory
suballocation.

---

## 6. GPU Memory

There is no VMA or custom `VkDeviceMemory` block allocator. The backend calls
`vkAllocateMemory` for image creation, buffer creation/resize, and readback
buffers. A `VulkanBuffer` also owns a `VkrDMemory` offset allocator for ranges
inside that buffer; this permits suballocation where callers deliberately share
a buffer, but does not make arbitrary buffers or images share device memory.

Device allocation sizes are reported through allocator tags, and the graph
tracks sizes of its own resources. Missing capabilities are:

- heap budget/usage reporting;
- pooled blocks by memory type;
- allocation-count telemetry;
- defragmentation/eviction;
- transient image/buffer aliasing.

Per-resource allocation is simple and currently functional, but scalability
must be measured rather than inferred from the presence of large scene assets.
The earlier claim that Sponza/San Miguel approach a particular allocation limit
was not backed by captured telemetry.

---

## 7. CPU↔GPU Communication and Synchronization

### 7.1 Per-frame mapped data

`VkrInstanceBufferPool` owns three persistently mapped buffers with a fixed
65,536-instance capacity. Overflow is reported and the affected pass can drop
work; it is not silent. `VkrIndirectDrawSystem` mirrors the design for 16,384
commands but is unused by passes.

Global shader UBOs use swapchain-image-indexed regions, and global descriptor
sets are allocated/indexed per swapchain image. Descriptor updates are cached
using generations plus concrete image view/sampler/buffer payloads. Instance
descriptor-state retirement is delayed by submit serial.

Two capacity values must not be conflated: the backend object descriptor pool
may grow up to 8,192 states, while shader-system CPU tracking currently starts
with/fixes a separate 1,024-instance capacity depending on the path.

### 7.2 Resource preparation and upload

`VkrResourceLoader` separates worker-thread `prepare_async` from render-thread
`finalize_async`. The resource pump has count/op/byte budgets and guarantees
forward progress by permitting the first oversized request. This bounds how
much finalization is started in a pump; it does not bound wall time because the
GPU upload functions wait indefinitely for their transfer and graphics upload
fences.

A dedicated transfer queue is selected when the device exposes a separate
family, including queue-family ownership work. The current image and buffer
upload helpers submit and then wait, so they do not overlap upload completion
with later rendering or retire staging data asynchronously.

Per-worker transfer/graphics-upload command pools are created only when both
`VKR_PARALLEL_UPLOAD` and `VKR_PARALLEL_UPLOAD_UNSAFE=1` enable the experimental
parallel runtime. It is not the default shipped upload path.

### 7.3 Readback

Picking uses a three-slot host-visible readback ring. Result polling normally
checks completion without waiting and may return a result one or more frames
later. If a new request wraps onto a still-pending slot,
`renderer_vulkan_request_pixel_readback()` waits indefinitely on the associated
frame fence. The correct description is **deferred in the common case**, not
“never blocking.”

### 7.4 Frame synchronization

The WSI object sizing is deliberate:

| Object | Count/index |
|---|---|
| Acquire semaphores | max frames in flight / current frame slot |
| Submit fences | max frames in flight / current frame slot |
| Render-complete semaphores | swapchain image count / acquired image |
| Image-in-flight fence references | swapchain image count / acquired image |

Per-image render-complete semaphores avoid re-signaling a semaphore still owned
by presentation. `max_in_flight_frames` is capped by `BUFFERING_FRAMES` (3).

This part is sound, but the wider frame implementation has unresolved issues:

- instance and indirect stream arrays contain three buffers but are reset using
  acquired `image_index % 3`; a four-image swapchain aliases images 0 and 3,
  even though both can be in flight;
- fence wait, acquire, command-buffer begin/end, queue submit, and present
  failures are logged in several paths but returned as
  `VKR_RENDERER_ERROR_NONE`;
- `vkr_rg_execute()` returns `void`; compile, barrier, and begin/end-pass
  failures cannot reach `vkr_renderer_submit_packet()`;
- ending a rejected packet assumes the acquired swapchain image was in
  `COLOR_ATTACHMENT_OPTIMAL`, although no pass may have transitioned it from
  its acquired/present state.

`vkDeviceWaitIdle` is not part of the normal successful submit loop, but it is
used by resize/shutdown and some graph timing/cache/resource recreation paths.
Upload paths instead contain blocking fence waits.

---

## 8. Prioritized Issues

### P0 — Correctness and error contracts

1. **Index frame streams by the frame-in-flight slot, or size/index them by
   swapchain image.** Do not modulo an arbitrary image count into a three-entry
   ring.
2. **Propagate backend failures.** Return distinct errors for fence, acquire,
   record, submit, and present failures; preserve frame-active invariants on
   every exit.
3. **Validate before acquiring, or add an explicit cancel path.** A rejected
   packet must not present a command buffer with an invented old layout.
4. **Make graph execution fallible.** Barrier and render-pass failures must
   abort submission and reach the caller.
5. **Complete graph synchronization.** Emit barriers from stage/access/layout,
   including same-layout hazards, and track image subresources where slices are
   used.

### P1 — Architectural completion

6. **Bring picking and IBL GPU work into the graph.** Their attachments,
   accesses, and graphics passes must be declared rather than hidden inside
   nominal compute executors.
7. **Reflect and validate uniform members.** Until then, treat `.shadercfg`
   uniform layout as manually synchronized data and add tests that compare
   offsets/sizes against compiled shaders.
8. **Make uploads genuinely asynchronous.** Signal semaphores/timeline values,
   retain staging allocations until completion, and remove per-upload infinite
   waits from frame finalization.
9. **Fix KTX2 normal-map capability fallback.** The selector returns
   `R8G8_UNORM` when neither BC5 nor ASTC is supported, but the KTX transcode
   mapper has no `R8G8_UNORM` case and returns `KTX_TTF_NOSELECTION`. Strict
   `.vkt` loading therefore fails on that capability combination.
10. **Add device-memory pooling and budget telemetry.** Measure allocation count,
   heap use, and load time before selecting block sizes or VMA.

### P2 — Throughput

11. **Cull before materializing world payloads**, using correctly transformed
    bounds. Non-uniform scaling must conservatively expand spheres or transform
    AABBs.
12. **Use real instancing first.** Consecutive draws with the same pipeline,
    material, geometry buffers, and index range can become one indexed draw
    with `instance_count > 1` when instance records are contiguous.
13. **Use MDI only for meaningful binding-state groups.** Multiple indirect
    commands can share one call only while pipeline, descriptors, and
    vertex/index buffers remain compatible. Per-material descriptor sets and
    per-geometry buffers limit grouping; bindless/material tables and shared
    mega-buffers may be prerequisites for large wins.
14. **Keep camera and shadow visibility separate.** Camera-culled lists cannot
    be reused for CSM because off-camera objects may cast visible shadows.

### P3 — Feature growth after measurement

- HDR scene color, tonemap, and post-processing;
- real compute dispatch followed by GPU culling/compaction;
- clustered/tiled light assignment and a storage-buffer light list;
- shader hot reload with pipeline/descriptor invalidation rules;
- optional descriptor indexing/bindless and capability-gated modern Vulkan
  features.

---

## 9. Portfolio Assessment

The renderer demonstrates broad, relevant engineering skill:

- a non-trivial C11 engine and renderer split;
- explicit Vulkan resource/synchronization work rather than wrapper-only use;
- a data-authored graph with scheduling, culling, resources, and timing;
- allocator/lifetime discipline and reload-aware ownership policies;
- shader reflection, persistent pipeline cache, compressed textures, async CPU
  loading, ECS/editor integration, CSM, text, and GPU picking;
- a meaningful CPU test suite.

The most important portfolio weakness is not missing visual effects; it is that
some flagship abstractions are only partially authoritative. A graphics
reviewer will notice the per-draw world loop, synchronous uploads, undeclared
GPU work inside graph executors, and swallowed Vulkan errors. Fixing those with
validation-layer runs and before/after measurements will strengthen the project
more than adding another isolated effect.

A fair current description is: **strong engine/rendering architecture and
breadth, with prototype-level gaps in failure handling, graph synchronization,
GPU allocation, and draw throughput.**

---

## 10. Verification Record

Reviewed statically on 2026-07-31 against the current working tree. The
following checks were rerun:

| Check | Result |
|---|---|
| `./build_test.sh` | Exit 0; all registered test suites completed |
| `vkr_frustum` production references | None outside its module/tests/docs |
| `VkrDrawBatcher` production references | None outside its module/tests/docs |
| `vkr_indirect_draw_alloc` callers | None |
| `vkAllocateMemory` call sites | Four direct backend sites |
| VMA | Not present |
| Dynamic rendering | Not present |
| Descriptor indexing/bindless | Not enabled |
| Uniform block reflection output | Explicitly set to empty |

The test runner is predominantly CPU-side and does not replace Vulkan
validation-layer runs across multiple swapchain image counts, queue-family
layouts, GPUs, and resize/failure scenarios.
