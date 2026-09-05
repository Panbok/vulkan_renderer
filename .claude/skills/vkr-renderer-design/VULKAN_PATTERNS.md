# Backend and graph ownership

Use the architecture spec for current status and ADRs for rationale. This file
identifies the contracts to inspect, rather than maintaining another issue list.

## Selected implementation

`vkr_renderer.c` and `vkr_renderer_internal.h` use platform-selected typed native
functions. Metal runs on macOS; Vulkan runs natively on Windows. Acquisition,
rendering, native lifecycle, publication, capture and metrics cross this boundary.
Native Vulkan types remain inside `lib/src/renderer/vulkan/`.

Application owns scene/UI/camera/shadow/picking systems, frame scratch and resize
events. `VkrRenderAssets` owns asset systems, loaders and load scratch; its borrowed
publisher must outlive it. Pass concrete subsystem inputs into scene extraction.
Keep boot dependencies in `core/vkr_subsystem_plan`, separate from GPU lifecycle.

Acquire with explicit `VkrFrameConfig`, build authoritative `VkrFrameInput`, then
render or cancel the acquired `VkrFrame`. Use its dimensions and target generation
for app-side resize effects. Both backends prepare all enabled pass families before
emission. Prepared recorders consume resolved resources and return `void`; native
creation, encoder/command-buffer boundaries and submission still propagate failure.

ADR-025 records replacement of the generic backend interface. Keep native command
recording private; do not recreate the retired Vulkan 1.2 descriptor-set path
or a function-pointer dispatch per draw.

## Shared graph and publication

`assets/render_graphs/main.rendergraph.json` declares frame resources and passes.
`vkr_rg_build_from_json()` realizes prepared frame conditions and dimensions;
`vkr_rg_compile_schedule()` computes shared ordering, culling, and dependencies.
Metal and Vulkan realize API objects and lower those dependencies into native
commands. Inspect `vkr_rg_compile.c` plus the affected backend's graph and
recording code together.

`VkrAssetPublisher` publishes generation-safe resources and immutable material
rows. Preserve canonical descriptors with the owning record. Replacing a shared
sampler republishes dependent material rows before its slot can retire.

## Pipelines and resource indices

Vulkan production shaders live in `shaders/vulkan/slang/` under the renderer.
Its pipelines use descriptor buffers and reflected GPU ABI records. Descriptor
byte offsets and strides derive from queried layout/device properties; shader
records carry logical indices. Native roots remain backend-owned.

Keep pipelines ready before recording their work. Capability selection happens
at initialization using typed data. Use `vkr-shaders` for bindings, ABI, shader
algorithms, and Metal/Vulkan comparison.

## Completion and reuse

Vulkan uses a timeline semaphore and `VKR_VULKAN_FRAME_SLOT_COUNT` command slots.
Acquire semaphores belong to frame slots; render-complete semaphores belong to
swapchain images. A presented image's reacquisition is proven when a completed
submit has consumed its next acquire semaphore. Optional present fences provide
an additional per-image completion proof.

`vkr_vulkan_renderer_prepare_frame()` can wait for a busy slot, image, or present
fence. These waits enforce finite in-flight capacity. Measure them before
changing scheduling; never replace completion with an assumed frame lag.

The application pumps assets after acquisition. Metal reserves that frame slot;
upload work uses another completion-protected command slot, with at least two
native slots required. Scene unload and asset shutdown callers must prove GPU idle
and keep the publisher and worker allocators alive through final resource release.

Logical destruction invalidates handles immediately. Physical resources,
descriptor/material slots, buffer ranges, and capture slots remain retained
until every recorded use is cancelled or submitted and every submitted use is
complete. Submission, cancellation, resize, and teardown must preserve that rule.

Vulkan uses keyed DEVICE/UPLOAD/READBACK/STAGING pools with dedicated allocation
paths.
The shared `vkr_gpu_memory`, `vkr_gpu_slot_table`, `vkr_gpu_submit_ring`, and
`vkr_capture_ring` cores supply allocation, slots, command ranges, and captures.
Inspect their real Metal and Vulkan callers before changing a shared policy.
Report capacity exhaustion at the owning boundary.

Batch barriers and publication commits where their ordering permits. Label GPU
passes with their graph names so timings and traces identify the work. Keep
state, access masks, and subresource ranges explicit.

## Coverage boundaries

Check `docs/ARCHITECTURE.md` capability and evidence boundaries before selecting
evidence. Linux support, other devices/drivers, separate queue families, and
injected failure paths
cannot be inferred from the existing Windows witness. Select only supported
harness configurations; a nonexistent backend mode is not a missing test run.
