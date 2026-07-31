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

`renderer_frontend.c/h` owns the renderer subsystems and reaches the backend
only through the `VkrRendererBackendInterface` function-pointer table declared
in `vkr_renderer.h`. Vulkan types live behind `lib/src/renderer/vulkan/` and do
not appear in public headers; public code uses opaque handles
(`TextureHandle`, `BufferHandle`, `GraphicsPipeline`, `RenderPass`,
`RenderTarget`) with internal structs prefixed `s_`.

This is a real seam, but it has **one** implementation and still exposes
concepts shaped by Vulkan's render-pass and descriptor model. Do not widen the
interface for a hypothetical second backend; do keep Vulkan types from leaking
across it.

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
implemented and validated by `./validate_pipeline_cache.sh`.

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

**P0 — correctness and error contracts**

- Graph image barriers execute from layout pairs, dropping the compiler's
  `src_access`/`dst_access`. Barriers are skipped when layouts match, so
  write→read and write→write hazards in the same layout are unrepresented.
  Buffer barriers are likewise skipped on matching access flags.
- Image state and barriers cover whole images even when an attachment names a
  mip/layer slice. Subresource granularity is not tracked.
- The graph is not the sole authority over GPU work: the picking executor opens
  its own retained render pass/target, and the IBL executor creates targets and
  records several graphics passes. Neither declares those resources or accesses.
- Instance and indirect stream arrays hold three buffers but are reset using
  `image_index % 3`. A four-image swapchain aliases images 0 and 3 while both
  can be in flight.
- Fence wait, acquire, command-buffer begin/end, submit, and present failures
  are logged in several paths but returned as `VKR_RENDERER_ERROR_NONE`.
- `vkr_rg_execute()` returns `void`, so compile, barrier, and begin/end-pass
  failures cannot reach `vkr_renderer_submit_packet()`.
- Ending a rejected packet assumes the acquired image was in
  `COLOR_ATTACHMENT_OPTIMAL`, though no pass may have transitioned it.

**P1 — architectural completion**

- `vulkan_spirv_shader_reflection_create()` sets `uniform_block_count` to zero.
  There is no member-level reflection and no cross-validation of `.shadercfg`
  names, types, strides, or offsets against SPIR-V. Treat the manifest uniform
  layout as manually synchronized data.
- Image and buffer upload helpers submit and then wait on fences, so upload
  completion does not overlap later rendering and staging is not retired
  asynchronously. The dedicated transfer queue is selected when available but is
  a synchronous path today. Per-worker parallel upload pools require both
  `VKR_PARALLEL_UPLOAD` and `VKR_PARALLEL_UPLOAD_UNSAFE=1`; that is not the
  shipped path.
- KTX2 normal-map fallback: the format selector returns `R8G8_UNORM` when
  neither BC5 nor ASTC is supported, but the transcode mapper has no
  `R8G8_UNORM` case and returns `KTX_TTF_NOSELECTION`.

**Absent by design-so-far, not bugs**

No VMA or `VkDeviceMemory` block allocator — `vkAllocateMemory` is called per
image, per buffer create/resize, and per readback buffer (four direct backend
sites). No dynamic rendering. No descriptor indexing or bindless. No HDR,
tonemap, or post chain — the `POST` domain is reserved. No shader hot reload.
`compute` JSON passes currently orchestrate graphics/CPU work; real compute
dispatch is not exercised.

**Plumbed but unused** — `vkr_frustum` (culling), `VkrDrawBatcher` (batching),
and `vkr_indirect_draw_alloc` (MDI) all exist with tests but have no production
call sites. ADR-013 is the **Proposed** plan for wiring them; read it before
adding a caller.

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
