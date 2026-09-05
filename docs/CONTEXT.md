---
status: implemented
updated: 2026-09-05
authority: context
---
# Project vocabulary

Terms here describe the current code. [ARCHITECTURE.md](ARCHITECTURE.md) connects
the owners; [INDEX.md](INDEX.md) locates decisions and future proposals. Paths
below are starting points for checking a definition, not alternate API specs.

## Frame and backend

| Term | Meaning in VKR | Owner |
|---|---|---|
| Renderer | Shared acquired-frame lifecycle, target state and full-frame rendering; scene and UI systems have application owners. | [vkr_renderer.c](../lib/src/renderer/vkr_renderer.c) |
| Native implementation | Metal or Vulkan operations selected by the platform build; `VkrRendererImpl` stores properties, not a dispatch table. | [vkr_renderer_impl.h](../lib/src/renderer/vkr_renderer_impl.h) |
| Frame input | Versioned `VkrFrameInput` containing caller metadata, settings and authoritative pass payloads. | [vkr_frame_input.h](../lib/src/renderer/vkr_frame_input.h) |
| Prepared frame | Private `VkrPreparedFrame`: borrowed input plus derived temporal, exposure, bloom and GTAO values. | [vkr_prepared_frame.h](../lib/src/renderer/vkr_prepared_frame.h) |
| Acquired frame | `VkrFrame`, identifying one target/command-slot acquisition with resolved dimensions and target generation. Render or cancel consumes it; acquisition number does not prove GPU completion. | [vkr_renderer.h](../lib/src/renderer/vkr_renderer.h) |
| Payload | The typed data a pass consumes, such as world candidates, shadows, UI draws, or picking requests. | [vkr_frame_input.h](../lib/src/renderer/vkr_frame_input.h) |
| Root / GPU ABI | A shader-visible record and its exact host/shader layout. Backend roots reference shared GPU tables; frame-input layout and GPU ABI are distinct contracts. | [vkr_gpu_abi.h](../lib/src/renderer/vkr_gpu_abi.h), [ADR-044](adr/044-shader-cross-backend-contract.md) |
| Bindless | GPU-addressed buffers and indexed texture/sampler tables, replacing per-draw descriptor binding. It does not mean unlimited resources. | [ADR-025](adr/025-selected-renderer-implementation-strategy.md), [ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md) |
| Frame slot | Bounded in-flight storage and command resources whose reuse requires GPU completion. | [Vulkan frame slots](../lib/src/renderer/vulkan/vkr_vulkan_internal.h), [Metal command slots](../lib/src/renderer/metal/vkr_metal_packet_renderer.m) |
| Submit serial | Monotonic identity used to associate completion, timing, and retirement with submitted work. CPU frame identity is recorded separately. | [vkr_renderer_impl.h](../lib/src/renderer/vkr_renderer_impl.h) |
| Present target | Window/swapchain or ordinary-image offscreen output, with explicit extent and attachment properties. | [vkr_renderer.h](../lib/src/renderer/vkr_renderer.h) |

## Graph and lifetime

| Term | Meaning in VKR | Owner |
|---|---|---|
| Authored graph | JSON resource/pass declarations, conditions, and executor names. | [main.rendergraph.json](../assets/render_graphs/main.rendergraph.json) |
| Compiled schedule | Shared ordering, dependency, culling, and resource-lifetime result lowered into native commands by each backend. | [vkr_rg_compile.c](../lib/src/renderer/vkr_rg_compile.c) |
| Executor | Named operation resolved to a backend ID during graph realization and recorded by that backend's native dispatcher. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Subresource | A mip/layer/aspect range tracked for accesses and dependencies. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Instance domain | Physical resource selection: single, per target image, or per frame slot. It is separate from content lifetime. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Transient | Frame-local contents backed by overlap-safe physical instances; the flag alone does not imply heap aliasing. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Retained | Contents remain valid in place across frames, tracked per physical instance and subresource. | [ADR-029](adr/029-retained-graph-resources.md) |
| History | A completion-managed ring that writes a new instance and reads an older instance. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Persistent | Graph flag relaxing read-before-write handling; it is not itself a retained-content validity proof. | [vkr_render_graph.h](../lib/src/renderer/vkr_render_graph.h) |
| Publication | Cold load/finalization operation that makes prepared assets resolvable to the selected renderer. | [vkr_asset_publisher.h](../lib/src/renderer/vkr_asset_publisher.h) |
| Handle generation | Identity component distinguishing a current slot occupant from an earlier occupant. | [vkr_gpu_slot_table.h](../lib/src/renderer/vkr_gpu_slot_table.h) |
| Retirement | Deferred physical release after logical invalidation, recorded-use resolution, and completion of submitted uses. | [vkr_gpu_memory.h](../lib/src/renderer/vkr_gpu_memory.h), [vkr_gpu_slot_table.h](../lib/src/renderer/vkr_gpu_slot_table.h) |
| DEVICE / UPLOAD / READBACK | GPU memory classes separating device working storage, CPU-to-GPU transfer, and GPU-to-CPU transfer. | [vkr_gpu_memory.h](../lib/src/renderer/vkr_gpu_memory.h) |
| Arena / DMemory / pool | CPU allocation choices for one bulk lifetime, independent frees, and fixed-size churn. | [ADR-006](adr/006-cpu-memory-allocators.md) |

## Visibility and shading

| Term | Meaning in VKR | Owner |
|---|---|---|
| Geometry megabuffer | Shared vertex/index GPU storage addressed by geometry rows and draw records. | [vkr_gpu_abi.h](../lib/src/renderer/vkr_gpu_abi.h) |
| Candidate / visible draw | A potential world draw emitted by extraction / a GPU row surviving visibility selection. | [vkr_gpu_abi.h](../lib/src/renderer/vkr_gpu_abi.h) |
| Visibility buffer | Rasterized primitive/draw identity used to recover geometry and materials in later resolve work. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| G-buffer / material resolve | Resolved surface attributes consumed by deferred lighting; resolve reconstructs attributes from visibility and geometry. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| HZB | Hierarchical depth representation used by visibility rejection. History validity is explicit. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| CSM | Cascaded shadow mapping: directional shadow coverage split across depth intervals. | [vkr_frame_input.h](../lib/src/renderer/vkr_frame_input.h) |
| SDSM | Sample Distribution Shadow Maps: optional cascade-range fitting from completed occupied-depth feedback. | [ADR-033](adr/033-occupied-depth-sdsm-feedback.md) |
| IBL | Image-based lighting derived from environment sources. Diffuse response uses SH; skybox and specular prefilter use cubemaps. | [ADR-038](adr/038-sh-l2-diffuse-irradiance.md) |
| SH L2 | Nine spherical-harmonic coefficients per color channel describing normalized diffuse response (`E/pi`), with authored deringing. | [vkr_ibl_math.h](../lib/src/renderer/vkr_ibl_math.h) |
| Transmission | Material light transport through a surface, using declared scene-color/depth feedback and bounded layer handling. Alpha blending alone is a separate behavior. | [ADR-018](adr/018-graph-declared-transmission-feedback.md) |
| Scene-linear / exposure / tonemap | Linear HDR scene values / brightness mapping / conversion of exposed HDR into display-range output. | [vkr_exposure.h](../lib/src/renderer/vkr_exposure.h), [post shaders](../lib/src/renderer/shaders/vulkan/slang/post/) |
| Bloom / GTAO | HDR bright-region filtering / ground-truth ambient occlusion from depth and surface information. | [vkr_bloom.h](../lib/src/renderer/vkr_bloom.h), [vkr_gtao.h](../lib/src/renderer/vkr_gtao.h) |
| TAA / jitter / reactivity | Temporal antialiasing / subpixel projection displacement / reduced history trust for changing composition. | [vkr_temporal.h](../lib/src/renderer/vkr_temporal.h) |
| Internal render scale | Scene shading extent relative to output extent; UI and physical presentation remain at native output size. | [ADR-039](adr/039-metal-internal-render-scale.md) |
| MetalFX / dynamic resolution | Metal temporal reconstruction / choosing bounded internal scale tiers from completed GPU timing. | [ADR-040](adr/040-metalfx-temporal-dynamic-resolution.md) |

## Scene, assets, and tools

| Term | Meaning in VKR | Owner |
|---|---|---|
| ECS / archetype / chunk | Entity-component storage / entities sharing a component layout / contiguous storage processed by queries. | [vkr_entity.h](../lib/src/core/vkr_entity.h) |
| Render assets | `VkrRenderAssets` owns asset systems, persistent text, loaders and load scratch; it borrows the longer-lived renderer publisher. | [vkr_render_assets.h](../lib/src/renderer/systems/vkr_render_assets.h) |
| Frame globals | Application-owned `VkrFrameGlobals` settings copied into the authoritative frame input. | [vkr_frame_input.h](../lib/src/renderer/vkr_frame_input.h) |
| Scene extraction | Conversion of scene/ECS state into renderable candidates and typed frame payloads. | [vkr_scene_system.c](../lib/src/renderer/systems/vkr_scene_system.c), [application.h](../lib/src/application.h) |
| Cooked asset | Offline-prepared versioned artifact validated by a runtime loader. It is distinct from a runtime transcode cache. | [ADR-030](adr/030-offline-mesh-optimization-and-cooking.md), [ADR-034](adr/034-offline-cooked-font-artifacts.md) |
| KTX2 / UASTC | Texture container / intermediate block encoding used for target-format transcoding. | [ADR-012](adr/012-texture-compression-pipeline.md) |
| MTSDF / em / DPI | Multi-channel signed-distance field with true-distance alpha / font-relative layout unit / display scale used before UI layout. | [ADR-035](adr/035-canonical-mtsdf-screen-pixel-range-shading.md), [ADR-036](adr/036-dpi-derived-ui-text-scale.md) |
| Immediate-mode UI | Widgets are declared each frame while stable IDs retain interaction, layout, and text caches. | [ADR-027](adr/027-immediate-mode-grid-ui.md) |
| Picking | Rendering/reading object IDs to resolve a selection in Scene viewport coordinates. | [vkr_frame_input.h](../lib/src/renderer/vkr_frame_input.h) |
| Case / profile | Harness workload definition / execution and evidence policy. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Snapshot / baseline | Captured run artifacts / reviewed immutable reference generation. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Authoritative measurement | A report satisfying its provenance, comparability, validity, and repetition policy; process success is a separate result. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Accepted / partial / proposed | Decision in force / explicit remaining integration / unimplemented feature or unsettled design. Native evidence limitations are stated separately. | [documentation skill](../.codex/skills/vkr-docs/SKILL.md) |
