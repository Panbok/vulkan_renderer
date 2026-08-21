# Vulkan backend patterns for VKR

This file records the real seams in `lib/src/renderer/` and the gaps that are
already known and tracked. Read it before proposing a backend or graph change,
so you extend the current design rather than rediscovering a logged issue.

Status authority is
`docs/architecture/renderer-architecture-spec.md`. Rationale authority is
`docs/architecture/adr/`.

## The seams that exist

### Selected implementation boundary — ADR-025, ADR-026

`renderer_frontend.c/h` owns scene-facing CPU systems and selects one coarse
`VkrRendererImpl` strategy at initialization: Metal on macOS or bindless
Vulkan on Windows. A normal successful frame crosses the strategy seam through
`prepare_frame` and `submit_packet`; lifecycle, asset-publication, capture,
and metrics operations are also coarse strategy calls.

There is no Vulkan 1.2 adaptor, generic backend interface, view/layer system, or
temporary `vulkan-bindless` selector. Vulkan types stay under
`lib/src/renderer/vulkan/bindless/` and do not appear in public renderer
headers. Do not recreate a command RHI or a second descriptor-set path.

### Authored JSON render graph — ADR-002, ADR-003

`assets/render_graphs/main.rendergraph.json` is parsed into a reusable authored
model. Per frame, `vkr_rg_build_from_json()` realizes resources and passes for
the current packet and dimensions, and `vkr_rg_compile_schedule()` produces
the shared order, culling, conditions, and image dependencies.

The selected implementation owns graph resource realization and command
recording. Metal encodes through its packet implementation; bindless Vulkan
lowers the shared dependency records to synchronization2 barriers and executes
dynamic-rendering/transfer/compute pass categories. The graph owns semantic
state, not API objects.

### Packet submission — ADR-004

`VkrRenderPacket` carries frame info, globals, and optional world, shadow,
skybox, UI, editor, picking, text-update, capture, and debug payloads. Optional
pointers control pass participation. Validation produces
`VkrValidationError` with a field path. Payload arrays are caller-owned and
must outlive submission.

Describe this as packet-based, not stateless: `prepare_frame` is ordered before
submission, while resource systems, published assets, graph realizations,
pipeline caches, and metrics persist across frames.

### Asset-publication boundary

Geometry, textures, samplers, materials, writable textures, and IBL work enter a
selected implementation through `VkrAssetPublisher`. Handles are
generation-safe. Physical resources and descriptor/material slots are retired
only after the last proven submit value. Samplers are canonical and shared;
replacing one republishes dependent material rows before retiring the old slot.

### Bindless Vulkan pipelines and ABI

Production Vulkan shaders are Slang sources under
`lib/src/renderer/vulkan/bindless/shaders/`. The packet graphics and IBL
compute pipelines are prebuilt and share one descriptor-buffer pipeline layout.
The host validates the shared GPU ABI and reflects the packet draw-root layout
from production SPIR-V. The retired walking shader/pipeline and standalone
V0/V3 executables are historical evidence only and no longer exist.

Descriptor-buffer offsets and strides come from queried device properties and
layout sizes. Shaders receive logical heap indices; descriptor byte strides
never cross the shader ABI.

### Frame synchronization and presentation

The Vulkan implementation uses a timeline semaphore for submit completion and a
three-slot command ring. Acquire semaphores are frame-slot owned.
Render-complete semaphores are swapchain-image owned. A reacquired presented
image is reusable only after a completed submit has consumed its next acquire
semaphore; optional swapchain-maintenance present fences provide a stronger
per-image proof when supported.

No blocking wait belongs in a successful frame. `vkDeviceWaitIdle` is confined
to shutdown and exceptional target recreation/diagnostics where completion
cannot otherwise be proven.

### Memory, descriptors, and capture

Bindless Vulkan uses keyed DEVICE, UPLOAD, and READBACK memory pools, with
dedicated-allocation bypass when required. Descriptor and material slots use
`vkr_gpu_slot_table`; command slices use `vkr_gpu_submit_ring`; asynchronous
multi-channel capture uses `vkr_capture_ring`. All have fixed capacities,
explicit overflow, and completion-gated reuse.

## Known gaps — do not “discover” these

Read §8 of the renderer architecture spec before changing one of these
boundaries. Current high-value gaps include:

- Linux platform support and native device/driver breadth remain absent.
- Wider Vulkan validation coverage, fault injection, and long-running lifecycle
  evidence remain thinner than the target Windows witness.
- Device-memory defragmentation, eviction, budget telemetry, and transient graph
  aliasing are not implemented.
- Shader hot reload is not implemented.
- `VkrDrawBatcher` remains unwired proposed API-neutral functionality;
  production batching uses visibility and pass-local structures.
- Performance claims still require matched clean Release harness evidence. The
  V7 and artifact-cleanup observations are correctness evidence only.

Do not treat a historical V0–V7 stage description as a current executable
surface. The normal application, CPU suite, and structured harness are the
supported evidence paths.

## Adopt these invariants

- Retain canonical descriptors with resource records; do not re-derive them.
- Encode explicit access, stages, layouts, and subresource ranges.
- Use fixed-capacity storage for GPU-limited collections; report overflow.
- Batch barrier and publication-range commits.
- Gate physical resource retirement and slot reuse on a proven submit value.
- Keep pipelines immutable and prebuilt; no frame-time variant creation.
- Label every command encoder and pass from its graph pass name; unlabelled GPU
  work is anonymous in a profiler trace.
- Keep capabilities immutable typed data, not a growing query interface.
- Keep Vulkan types in `renderer/vulkan/bindless/`.
- Preserve one coarse selected-implementation seam; do not add per-draw
  function-pointer dispatch.

## Do not copy from elsewhere

- Per-draw virtual/function-pointer dispatch.
- Owning strings or dynamic arrays inside hot descriptors.
- Per-resource heap maps for common state tracking.
- Inferring GPU completion from frame lag.
- `void *` native escape hatches through public interfaces.
- Compatibility shims for the deleted Vulkan 1.2 or standalone diagnostic
  paths.
