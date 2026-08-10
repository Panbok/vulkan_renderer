# Vulkan backend patterns for VKR

This file records the real seams in `lib/src/renderer/` and the gaps that are
already known and tracked. Read it before proposing a backend or graph change,
so you extend the current design rather than rediscovering a logged issue.

Status authority for everything here is
`docs/architecture/renderer-architecture-spec.md`. Rationale authority is
`docs/architecture/adr/`.

## Contents

- [The seams that exist](#the-seams-that-exist)
- [Known gaps](#known-gaps--do-not-discover-these)
- [Adopt these invariants](#adopt-these-invariants)
- [Do not copy from elsewhere](#do-not-copy-from-elsewhere)

## The seams that exist

### Frontend/backend boundary — ADR-001

`renderer_frontend.c/h` owns the renderer subsystems and selects one coarse
`VkrRendererImpl` strategy at initialization. Metal, the retained legacy Vulkan
adaptor, and the Vulkan 1.4 bindless V1–V4 implementation are real strategies.
A normal successful frame crosses the strategy seam exactly twice, through
prepare and submit. The legacy adaptor alone reaches the old
`VkrRendererBackendInterface` function-pointer table declared in
`vkr_renderer.h`. Vulkan types remain behind `lib/src/renderer/vulkan/` and do
not appear in public headers; public code uses opaque handles
(`TextureHandle`, `BufferHandle`, `GraphicsPipeline`, `RenderPass`,
`RenderTarget`) with internal structs prefixed `s_`.

The retained backend table still exposes concepts shaped by Vulkan's render-pass
and descriptor model. Keep that legacy detail inside its strategy; extend the
coarse selected implementation contract only for behavior shared by real
implementations, and keep Vulkan types from leaking across it.

### JSON render graph — ADR-002, ADR-003

`assets/render_graphs/main.rendergraph.json` is parsed once at initialization
into a reusable model. Per frame, `vkr_rg_build_from_json()` realizes resources
and passes for current dimensions, editor condition, and cascade count;
`vkr_rg_execute()` compiles if needed and runs.

Split of responsibility:

| File | Owns |
|---|---|
| `vkr_render_graph.c` | Declaration, resource management, registries |
| `vkr_rg_compile.c` | Dependencies, topological order, culling, barrier planning |
| `vkr_rg_execute.c` | Pass execution and timing |
| `vkr_rg_json.c` | JSON parsing and per-frame realization |
| `passes/vkr_pass_*.c` | Named executors |

Executors resolve by name through `VkrRgExecutorRegistry`
(`vkr_rg_executor_registry_register`). Registered names today: `pass.world`,
`pass.shadow.cascade`, `pass.ui`, `pass.skybox`, `pass.editor`, `pass.picking`.
Adding a pass should mean a JSON declaration plus one registration — that is the
N+1 shape this design exists to provide.

### Packet submission — ADR-004

`VkrRenderPacket` (version 2) carries frame info, globals, and optional world,
shadow, skybox, UI, editor, picking, text-update, and debug payloads. Optional
pointers control pass participation. Validation produces `VkrValidationError`
with a field path. Payload arrays are caller-owned and must outlive submission.

Describe this as **packet-based**, not stateless: `prepare_frame` is required
and ordered, and submission mutates retained globals, UI/text resources,
descriptor state, graph caches, and metrics.

### Pipeline domains

`VkrPipelineDomain` has eleven values, not four:
`WORLD`, `UI`, `SHADOW`, `POST`, `COMPUTE`, `WORLD_TRANSPARENT`, `SKYBOX`,
`PICKING`, `PICKING_TRANSPARENT`, `WORLD_OVERLAY`, `PICKING_OVERLAY`.
Render passes start and stop automatically when the domain changes.

### Reflection-driven pipelines — ADR-005

`vulkan_spirv_reflection.c` (via `spirv_reflect`) supplies descriptor sets and
bindings, descriptor type/count, block byte sizes, push-constant ranges, vertex
inputs for host vertex-ABI validation, and semantic role resolution for
frame/draw sets. `.shadercfg` supplies shader identity, stages, render pass,
`vertex_abi`, and the frontend's named uniform declarations and CPU offsets.

Static assertions pin the `VkrVertex3d`, `VkrInstanceDataGPU`, and
`VkrIndirectDrawCommand` host layouts. Pipeline cache persistence is
implemented and validated by `./validate_pipeline_cache.sh`. Reflected uniform
members are validated against `.shadercfg` names, offsets, exact sizes,
scalar/vector/matrix shapes, array count/stride, and matrix stride at shader
creation; all shipped shader manifests are covered by the CPU suite.

### Frame synchronization — ADR-009

| Object | Count / index |
|---|---|
| Acquire semaphores | max frames in flight, indexed by frame slot |
| Submit fences | max frames in flight, indexed by frame slot |
| Render-complete semaphores | swapchain image count, indexed by acquired image |
| Image-in-flight fence refs | swapchain image count, indexed by acquired image |

Per-image render-complete semaphores exist so presentation never re-signals a
semaphore it still owns. `max_in_flight_frames` is capped by `BUFFERING_FRAMES`
(3). `vkDeviceWaitIdle` is not in the successful submit loop; it is used by
resize, shutdown, and some graph timing/cache/resource recreation paths.

### Per-frame data and readback — ADR-008

`VkrInstanceBufferPool` owns three persistently mapped buffers, fixed capacity
65,536 instances; overflow is reported and the pass can drop work, never
silently truncated. `VkrIndirectDrawSystem` mirrors this for 16,384 commands.
Global UBOs use swapchain-image-indexed regions, and global descriptor sets are
allocated per swapchain image. Descriptor updates are cached by generation plus
concrete view/sampler/buffer payloads. Picking uses a three-slot host-visible
readback ring; polling is deferred in the common case.

Do not conflate the two capacity values: the backend object descriptor pool may
grow to 8,192 states, while shader-system CPU tracking uses a separate
1,024-instance capacity depending on path.

## Known gaps — do not "discover" these

These are logged in the architecture spec §3.3, §7, and §8. If your task is not
one of them, do not opportunistically half-fix one; if it is, read the spec
entry first.

The former P0 correctness list is resolved: graph barriers carry access and
subresource ranges; frame streams use frame-in-flight slots; backend/graph
failures propagate; rejected packets cancel explicitly. Do not re-propose those
repairs from an older audit. The remaining boundaries are:

- **Graph authority.** Picking resources and readback are declared. IBL baking
  still creates targets and records passes inside an executor, so the graph
  cannot schedule that work; an explicit write→sample barrier makes the current
  path visible and correct, but does not make the graph authoritative.
- **Upload completion.** Uploads recorded during an active frame are deferred,
  and staging retirement is submit-serial/fence gated. Bootstrap and other
  uploads outside an active frame still use blocking single-use submits.
  Timeline-semaphore retirement is not implemented.
- **Readback pressure.** Picking's three-slot ring is deferred in the common
  case, but wrapping onto a pending slot waits on its associated frame fence.
- **Device memory.** Legacy Vulkan allocation accounting and
  `VK_EXT_memory_budget` telemetry are centralized in its backend wrapper; its
  Apple M1 Pro measurements argue against retrofitting pooling. Bindless Vulkan
  V4 instead uses keyed DEVICE/UPLOAD/READBACK blocks backed by
  `vkr_gpu_memory`, with dedicated-resource bypass and separate logical/physical
  metrics. Neither path provides defragmentation, eviction, or transient aliasing.
- **Validation breadth.** The corrected frame path and P2 throughput path have
  a three-image MoltenVK validation record. Two/four-image swapchains, wider
  failure injection, other GPUs/queue layouts, and a native Vulkan target remain
  unverified.

**Absent by design-so-far, not bugs**

The retained Vulkan 1.2 implementation has no dynamic rendering, descriptor
indexing, or bindless path. Metal has a production bindless implementation, and
the Vulkan 1.4 strategy completes V1–V4 on the RX 6700 XT; authored graph,
sync2/dynamic-rendering pass parity, and capture remain V5 work. No shader hot
reload. Legacy `compute` JSON passes currently orchestrate graphics/CPU work;
real compute dispatch is not exercised.

**Throughput baseline** — production payload extraction uses `vkr_frustum` for
camera and union-of-cascade visibility, CPU-merges compatible opaque candidates
into instanced draws, and calls `vkr_indirect_draw_alloc` through shared
world/shadow pass submission. The older `VkrDrawBatcher` module alone remains
unwired; production batching uses visibility and pass-local structures.
ADR-013 is **Accepted (partial)** pending native Vulkan validation. Read its
implemented behavior and measurements before proposing another submission
layer.

## Adopt these invariants

- Retain canonical descriptors with resource records; do not re-derive them.
- Encode explicit resource states and, where a slice is used, subresource
  ranges.
- Use fixed-capacity storage for GPU-limited collections; report overflow.
- Batch barrier commits rather than emitting them per resource touch.
- Gate physical resource retirement on a proven fence or submit serial.
- Keep pipelines immutable and prebuilt. Creating a pipeline variant during
  frame encoding is forbidden.
- Keep capabilities immutable typed data, not a growing table of queries.
- Keep third-party and Vulkan types in `renderer/vulkan/`; never downcast
  through one abstraction to reach the backend.

## Do not copy from elsewhere

- Per-draw virtual/function-pointer dispatch.
- Owning strings or dynamic arrays inside hot descriptors.
- Per-resource heap maps for common state tracking.
- Inferring GPU completion from frame lag instead of a completion token.
- `void *` native escape hatches through the public interface.
