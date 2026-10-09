---
status: implemented
updated: 2026-10-08
authority: context
---
# Project vocabulary

Terms here describe the current code. [ARCHITECTURE.md](ARCHITECTURE.md) connects
the owners; [INDEX.md](INDEX.md) locates decisions and future proposals. Paths
below are starting points for checking a definition, not alternate API specs.

## Frame and backend

| Term | Meaning in VKR | Owner |
|---|---|---|
| Renderer | Shared acquired-frame lifecycle, target state and full-frame rendering; scene and UI systems have application owners. | [vkr_renderer.c](../renderer/src/vkr_renderer.c) |
| Native implementation | Metal or Vulkan operations selected by the platform build; `VkrRendererImpl` stores properties, not a dispatch table. | [vkr_renderer_impl.h](../renderer/src/vkr_renderer_impl.h) |
| Frame input | Versioned `VkrFrameInput` containing caller metadata, settings and authoritative pass payloads. | [vkr_frame_input.h](../renderer/src/vkr_frame_input.h) |
| Prepared frame | Private `VkrPreparedFrame`: borrowed input plus derived temporal, exposure, bloom and GTAO values. | [vkr_prepared_frame.h](../renderer/src/vkr_prepared_frame.h) |
| Acquired frame | `VkrFrame`, identifying one target/command-slot acquisition with resolved dimensions and target generation. Render or cancel consumes it; acquisition number does not prove GPU completion. | [vkr_renderer.h](../renderer/src/vkr_renderer.h) |
| Payload | The typed data a pass consumes, such as world candidates, shadows, UI draws, or picking requests. | [vkr_frame_input.h](../renderer/src/vkr_frame_input.h) |
| Shared kernel | Portable shader arithmetic in `renderer/src/shaders/shared/` that Metal and Vulkan sources include; one kernel can have consumers in both pipeline classes. | [ADR-044](adr/044-shader-cross-backend-contract.md) |
| Root / GPU ABI | A shader-visible record and its exact host/shader layout. Backend roots reference shared GPU tables; frame-input layout and GPU ABI are distinct contracts. | [vkr_gpu_abi.h](../renderer/src/vkr_gpu_abi.h), [ADR-044](adr/044-shader-cross-backend-contract.md) |
| Bindless | GPU-addressed buffers and indexed texture/sampler tables, replacing per-draw descriptor binding. It does not mean unlimited resources. | [ADR-025](adr/025-selected-renderer-implementation-strategy.md), [ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md) |
| Frame slot | Bounded in-flight storage and command resources whose reuse requires GPU completion. | [Vulkan frame slots](../renderer/src/vulkan/vkr_vulkan_internal.h), [Metal command slots](../renderer/src/metal/vkr_metal_packet_renderer.m) |
| Submit serial | Monotonic identity used to associate completion, timing, and retirement with submitted work. CPU frame identity is recorded separately. | [vkr_renderer_impl.h](../renderer/src/vkr_renderer_impl.h) |
| Frame-loop thread | The thread running the application host loop (the main thread): update, extraction, coupled acquisition, recording asset publications and frame completion. Older documents call it the render thread. | [vkr_application_host.c](../runtime/src/application/vkr_application_host.c) |
| Render thread | Renderer-owned worker, on by default, that acquires and renders a submitted frame while the frame-loop thread builds the next; every other renderer call waits for it. | [ADR-082](adr/082-renderer-owned-render-thread.md) |
| Decoupled frame | A frame built from the previous frame's target values and acquired by the render thread; coupled frames are acquired on the frame-loop thread. | [ADR-082](adr/082-renderer-owned-render-thread.md) |
| Present target | Window/swapchain or ordinary-image offscreen output, with explicit extent and attachment properties. | [vkr_renderer.h](../renderer/src/vkr_renderer.h) |
| Graphics pipeline class | Desktop or tiled (`VkrGraphicsPipelineClass`): the render graph and shading path a renderer runs. It follows the backend (`vkr_graphics_pipeline_for_backend`): Metal runs the tiled class, which shades forward in one multisampled render pass with baked lightmaps, and Vulkan the desktop class. | [ADR-087](adr/087-gpu-class-graphics-pipelines.md) |

## Graph and lifetime

| Term | Meaning in VKR | Owner |
|---|---|---|
| Authored graph | JSON resource/pass declarations, conditions, and executor names; one per graphics pipeline class. | [main.rendergraph.json](../assets/render_graphs/main.rendergraph.json), [tiled.rendergraph.json](../assets/render_graphs/tiled.rendergraph.json) |
| Compiled schedule | Shared ordering, dependency, culling, and resource-lifetime result lowered into native commands by each backend. | [vkr_rg_compile.c](../renderer/src/vkr_rg_compile.c) |
| Executor | Named operation resolved to a backend ID during graph realization and recorded by that backend's native dispatcher. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Subresource | A mip/layer/aspect range tracked for accesses and dependencies. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Instance domain | Physical resource selection: single, per target image, or per frame slot. It is separate from content lifetime. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Transient | Frame-local contents backed by overlap-safe physical instances; the flag alone does not imply heap aliasing. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Retained | Contents remain valid in place across frames, tracked per physical instance and subresource. | [ADR-029](adr/029-retained-graph-resources.md) |
| History | A completion-managed ring that writes a new instance and reads an older instance. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Persistent | Graph flag relaxing read-before-write handling; it is not itself a retained-content validity proof. | [vkr_render_graph.h](../renderer/src/vkr_render_graph.h) |
| Publication | Cold load/finalization operation that makes prepared assets resolvable to the selected renderer; recorded as a command and confirmed by its completion. | [vkr_asset_publisher.h](../renderer/src/vkr_asset_publisher.h) |
| Handle generation | Identity component distinguishing a current slot occupant from an earlier occupant. | [vkr_gpu_slot_table.h](../renderer/src/vkr_gpu_slot_table.h) |
| Retirement | Deferred physical release after logical invalidation, recorded-use resolution, and completion of submitted uses. | [vkr_gpu_memory.h](../renderer/src/vkr_gpu_memory.h), [vkr_gpu_slot_table.h](../renderer/src/vkr_gpu_slot_table.h) |
| DEVICE / UPLOAD / READBACK | GPU memory classes separating device working storage, CPU-to-GPU transfer, and GPU-to-CPU transfer. | [vkr_gpu_memory.h](../renderer/src/vkr_gpu_memory.h) |
| Arena / DMemory / pool | CPU allocation choices for one bulk lifetime, independent frees, and fixed-size churn. | [ADR-006](adr/006-cpu-memory-allocators.md) |

## Visibility and shading

| Term | Meaning in VKR | Owner |
|---|---|---|
| Projected decal | A `decal` component's box: it lays its material's base colour over the opaque surfaces it holds before lighting, in the tiled pipeline's forward shader and the desktop pipeline's G-buffer resolve. A mesh decal is imported geometry pushed off its surface by `vkr_decal_normal_offset_meters` and drawn as an ordinary surface. | [ADR-092](adr/092-projected-decals.md), [vkr_scene_decal.h](../runtime/src/renderer/systems/vkr_scene_decal.h) |
| Geometry megabuffer | Shared vertex/index GPU storage addressed by geometry rows and draw records. | [vkr_gpu_abi.h](../renderer/src/vkr_gpu_abi.h) |
| Candidate / visible draw | A potential world draw emitted by extraction / a GPU row surviving visibility selection. | [vkr_gpu_abi.h](../renderer/src/vkr_gpu_abi.h) |
| Visibility buffer | Rasterized primitive/draw identity used to recover geometry and materials in later resolve work. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| G-buffer / material resolve | Resolved surface attributes consumed by deferred lighting; resolve reconstructs attributes from visibility and geometry. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| HZB | Hierarchical depth representation used by visibility rejection. History validity is explicit. | [ADR-028](adr/028-gpu-driven-deferred-visibility-buffer.md) |
| Local shadow face | One perspective depth square of the shared local-shadow atlas for an opted-in punctual light: one per spot, six per point, resident while the light exists and drawn as complete lights within the face budget per frame. | [ADR-019](adr/019-bounded-forward-spatial-lighting.md) |
| CSM | Cascaded shadow mapping: directional shadow coverage split across depth intervals. | [vkr_frame_input.h](../renderer/src/vkr_frame_input.h) |
| SDSM | Sample Distribution Shadow Maps: optional cascade-range fitting from completed occupied-depth feedback. | [ADR-033](adr/033-occupied-depth-sdsm-feedback.md) |
| IBL | Image-based lighting derived from the global environment or local probes. Diffuse response uses SH; specular prefilter uses cubemaps. | [ADR-038](adr/038-sh-l2-diffuse-irradiance.md) |
| Atmosphere sun light | The enabled directional light whose `atmosphere_sun` flag makes it drive the atmosphere's sun direction, tinted irradiance and disc; scene files default the flag on, glTF imports off. The drawn sky and direct light follow it every frame; the sky light refreshes at most every 0.25 s. | [ADR-058](adr/058-revision-baked-sky-atmosphere.md) |
| Sun glow | Visual-only 1/theta^2 veiling glow around the sun disc, `atmosphere.sun_glow` (default 2, zero off), faded out 25 degrees from the sun; never part of lighting or the bake. | [ADR-058](adr/058-revision-baked-sky-atmosphere.md) |
| Sky light | The scene `environment` block: enable flag, intensity, diffuse/specular scale and SH window applied to the atmosphere or a constant global source. Disabling it keeps the atmosphere sky visible. | [ADR-058](adr/058-revision-baked-sky-atmosphere.md) |
| Lightmap set | A scene's baked lightmap layers as ASTC 4×4 HDR pages with the page rectangle of each lightmapped instance, keyed by document id, entity index and source node; one VKLM file named by the scene's `lightmaps` block. The runtime does not sample it yet. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Light layer | One baked layer of a lightmap set or diffuse volume: a sun key or a lamp group, stored as a 64-byte record. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Lightmap slot | A draw's lightmap rectangle index plus one, which the scene gives each matched mesh instance or generated mesh; zero draws without a lightmap. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Sun key | A light layer holding sun bounce and sky light for one sun direction, without the sun's direct term, which stays at runtime. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Lamp group | A light layer holding the light of the static lights of one light group (direct and bounce in a lightmap, bounce in a diffuse volume), scaled at runtime by the group's factor; the `default` group also holds surface emission. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Light group | The name (`light_group`) that assigns a static point or rectangle light to a lamp group; empty is `default`. A level bakes at most four. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Light mobility | Whether a point or rectangle light is `static`, baked into its light group's lamp group, or `dynamic`, left out of every bake and lit at runtime. | [ADR-088](adr/088-baked-lightmap-sets.md) |
| Light group factor | A light group's output scale each frame: its script-set intensity, times the night fade for a night group. Static lights multiply their output by it. | [ADR-090](adr/090-time-of-day.md) |
| Night group | A light group a World's time of day lists in `night_groups`: it lights only while the sun is down, fading over the sun's last 5 degrees above the horizon. | [ADR-090](adr/090-time-of-day.md) |
| Time of day | The World-only `time_of_day` component and its clock: an hour that turns the sun and moon about the celestial pole from their authored noon directions, advancing with the simulation by the day length. | [ADR-090](adr/090-time-of-day.md) |
| Sky-view lookup / aerial perspective | Per-frame atmosphere radiance around the camera, sampled for the visible sky / in-scatter and transmittance between the camera and a surface, applied before fog. | [ADR-058](adr/058-revision-baked-sky-atmosphere.md) |
| Cloud layer / cloud shadow map | One volumetric layer between two altitudes, traced at half resolution into a history composited over the sky / the layer's transmittance along the sun, sampled by every sun evaluation. | [ADR-074](adr/074-volumetric-cloud-layer.md) |
| SH L2 | Nine spherical-harmonic coefficients per color channel describing normalized diffuse response (`E/pi`), with authored deringing. | [vkr_ibl_math.h](../renderer/src/vkr_ibl_math.h) |
| Refractive transmission | Surface transport using declared scene-color/depth feedback and bounded layer handling. Alpha blending is a separate behavior. | [ADR-018](adr/018-graph-declared-transmission-feedback.md) |
| Thin-sheet diffuse transmission | A tinted opposite-hemisphere Lambert lobe funded by the base diffuse allocation; no refraction, medium crossing or spatial diffusion. Runtime backlighting uses direct light. | [ADR-065](adr/065-thin-sheet-diffuse-transmission.md) |
| Scene-linear / exposure / tonemap | Linear HDR scene values / brightness mapping / conversion of exposed HDR into display-range output. | [vkr_exposure.h](../renderer/src/vkr_exposure.h), [post shaders](../renderer/src/shaders/vulkan/slang/post/) |
| Bloom / GTAO | HDR bright-region filtering / ground-truth ambient occlusion from depth and surface information. | [vkr_bloom.h](../renderer/src/vkr_bloom.h), [vkr_gtao.h](../renderer/src/vkr_gtao.h) |
| TAA / jitter / reactivity | Temporal antialiasing / subpixel projection displacement / reduced history trust for changing composition. | [vkr_temporal.h](../renderer/src/vkr_temporal.h) |
| Internal render scale | Scene shading extent relative to output extent; UI and physical presentation remain at native output size. | [ADR-039](adr/039-metal-internal-render-scale.md) |
| Dynamic resolution / adaptive quality | Choosing bounded internal scale tiers from completed GPU timing / the tiled pipeline's use of it: the scale steps between 0.65 and native to hold the frame budget and the tonemap pass upscales spatially. Vulkan has neither. | [ADR-087](adr/087-gpu-class-graphics-pipelines.md) (decision 12) |

## Scene, assets, and tools

| Term | Meaning in VKR | Owner |
|---|---|---|
| ECS / archetype / chunk | Entity-component storage / entities sharing a component layout / contiguous storage processed by queries. | [vkr_entity.h](../runtime/src/core/vkr_entity.h) |
| Application host | Reusable owner of window, input, event dispatch, timing and lifecycle. It invokes callbacks with caller-owned state and has no scene policy. | [vkr_application_host.h](../runtime/src/application/vkr_application_host.h) |
| Standard scene runtime | Optional conventional scene owner that uses an application host and constructs frame inputs from scene, assets, camera, lighting, UI and picking systems. | [vkr_standard_scene_runtime.h](../runtime/src/application/vkr_standard_scene_runtime.h) |
| Sample runtime | Optional app/editor control and presentation policy layered on the standard runtime; it is not required by a custom runtime client. | [vkr_sample_runtime.h](../runtime/src/vkr_sample_runtime.h) |
| Render assets | `VkrRenderAssets` owns asset systems, persistent text, loaders and load scratch; it borrows the longer-lived renderer publisher. | [vkr_render_assets.h](../runtime/src/renderer/systems/vkr_render_assets.h) |
| Frame globals | Standard-scene-runtime or custom-client-owned `VkrFrameGlobals` settings copied into the authoritative frame input. | [vkr_frame_input.h](../renderer/src/vkr_frame_input.h) |
| Scene extraction | Conversion of scene/ECS state into renderable candidates and typed frame payloads. | [vkr_scene_system.c](../runtime/src/renderer/systems/vkr_scene_system.c), [vkr_standard_scene_runtime.h](../runtime/src/application/vkr_standard_scene_runtime.h) |
| Character motor | Scene-owned Jolt CharacterVirtual capsule, stepped from C before physics and published as evaluated root translation. | [ADR-073](adr/073-native-gameplay-foundation.md) |
| Evaluated transform | Transient world-matrix override used for gameplay presentation; separate from authored TRS and removed with its owning entity/client. | [ADR-073](adr/073-native-gameplay-foundation.md) |
| Player animation controller | FPS module playback owner mapping accepted actions and motor state to interruptible named clips; the scene advances animation time. | [fps_player_animation.h](../scripts/fps/src/fps_player_animation.h) |
| Locomotion controller | FPS module pose owner for banks with the mannequin's clip names: one footfall phase advanced by distance, direction/gait/crouch/jump blends on its own clocks. | [fps_locomotion.h](../scripts/fps/src/fps_locomotion.h) |
| Default mannequin | VKR's generated UE5-style character in the engine content, the player's body unless the project places a model with `fps_player`. | [ADR-080](adr/080-default-mannequin-character.md) |
| Spawned model | A cooked mesh and optional bank instantiated under a live wrapper at runtime; the scene releases it with the wrapper. | [vkr_scene_model.h](../runtime/src/renderer/systems/vkr_scene_model.h) |
| Camera rig | FPS module first/third-person or shoulder pose calculation with optional obstruction sweep; target pose and look are supplied separately. | [fps_camera_rig.h](../scripts/fps/src/fps_camera_rig.h) |
| Script module | Game code built with a project that calls the engine only through `sdk.h` and describes its components, behaviors, data and hooks. | [sdk.h](../sdk/sdk.h), [ADR-079](adr/079-c-script-modules.md) |
| Script host | Runtime owner of the SDK table, registered modules and the one session run on the played scene's clock. | [vkr_script_host.h](../runtime/src/script/vkr_script_host.h) |
| Script package | A `Scripts/<Name>/` folder described by `<Name>.script.json`: a module with an entry point, or a library of shared code the packages listing it in `dependencies` include. | [vkr_bakery_script.c](../tools/bakery/vkr_bakery_script.c), [ADR-079](adr/079-c-script-modules.md) |
| Project library | The one shared library every script package of a project links into; `vkr_project_modules` lists its modules, which load and reload together. | [vkr_script_host.h](../runtime/src/script/vkr_script_host.h) |
| Script task | A function a script runs on a worker over its own copy of data, without SDK access; its scope waits for it when it ends. | [sdk.h](../sdk/sdk.h) |
| Script instance | A module running on one attached container, with its data and ledger; container-scoped modules run one per container, World-scoped ones once on the World. | [vkr_script_host.h](../runtime/src/script/vkr_script_host.h) |
| Behavior | A module's hooks run for every entity carrying one of its components, with that entity's own ledger. | [sdk.h](../sdk/sdk.h) |
| Ledger | The record of what one instance or behavior acquired (entities, characters, models, runtime state, render poses, tasks), released newest first when its scope ends. | [vkr_script_internal.h](../runtime/src/script/vkr_script_internal.h) |
| Queued edit | A structural SDK call made in a fixed update, replayed in order right after the tick; a spawn's reserved ID is valid at once. | [vkr_script_sdk.c](../runtime/src/script/vkr_script_sdk.c) |
| Transient entity | An entity tagged runtime-only, such as anything a script spawned; saving skips it. | [vkr_scene_system.h](../runtime/src/renderer/systems/vkr_scene_system.h) |
| Script object | An object kind creating an entity with one registered script component; the same component attaches through Add component. | [ADR-079](adr/079-c-script-modules.md) |
| Hot reload | Swapping a script library's code between frames while instances keep their data; a changed data shape or version restarts the session and a changed component layout is refused. | [vkr_script_host.h](../runtime/src/script/vkr_script_host.h) |
| Script editor | The editor's floating window for script sources, with highlighting, completion and compiler diagnostics. | [editor_code.c](../editor/src/editor_code.c) |
| Player Start | Engine component whose entity's world transform is the spawn pose the scene resolves for a game's player. | [vkr_scene_types.c](../runtime/src/renderer/systems/vkr_scene_types.c) |
| Scene simulation | Scene-owned fixed clock and optional C hooks around native animation/physics; distinct from display-frame callbacks. | [ADR-073](adr/073-native-gameplay-foundation.md) |
| Physics body | Entity owning motion, mass/material settings, sensor role and collision membership/mask; runtime state is separate from authored TRS. | [vkr_scene_physics.h](../runtime/src/renderer/systems/vkr_scene_physics.h), [ADR-072](adr/072-entity-collision-and-rigid-body-physics.md) |
| Collider child | Direct child of one physics body, with stable authored ID, primitive/cooked geometry, local pose, positive scale and enable state; enabled children form one compound. | [vkr_scene_physics.h](../runtime/src/renderer/systems/vkr_scene_physics.h) |
| Physics snapshot | Complete authored body/collider value used for staged edits, structural undo and overlay persistence; excludes velocities, solver handles and session mutes. | [vkr_scene_edit.h](../runtime/src/renderer/systems/vkr_scene_edit.h) |
| Collision asset | Versioned VKC1 static proxy containing convex-hull or triangle-mesh geometry, with checksum and source fingerprint. | [vkr_collision_cooked.h](../runtime/src/assets/vkr_collision_cooked.h) |
| Collision matrix / preset | Scene-owned symmetric membership rules / named copy of membership, mask and sensor role for an Inspector draft. | [vkr_scene_collision_layers.h](../runtime/src/renderer/systems/vkr_scene_collision_layers.h) |
| Bone physics attachment | Body pose relative to an evaluated global animation node; Dynamic drive publishes solved bones for ragdolls. | [vkr_scene_physics.h](../runtime/src/renderer/systems/vkr_scene_physics.h) |
| Physics tick / debt | One completed 1/60-second solver update / elapsed time still awaiting bounded catch-up work. | [vkr_scene_physics.c](../runtime/src/renderer/systems/vkr_scene_physics.c) |
| Sensor | Static or Kinematic body reporting buffered overlap begin/end membership without physical response. | [vkr_physics.h](../runtime/src/physics/vkr_physics.h) |
| Cooked asset | Offline-prepared versioned artifact validated by a runtime loader; cooked textures are host-native. | [ADR-030](adr/030-offline-mesh-optimization-and-cooking.md), [ADR-034](adr/034-offline-cooked-font-artifacts.md) |
| Animation bank | Standalone `.vka` source-node, skin and exact clip data with CPU sampling; not a live animator or skinned mesh. | [ADR-071](adr/071-animation-bank-and-reference-pose.md) |
| Animation player | Per-wrapper seconds-based playback and evaluated CPU pose/palette buffers, separate from authored transforms and rendered history. | [ADR-071](adr/071-animation-bank-and-reference-pose.md), [vkr_animation_player.h](../runtime/src/animation/vkr_animation_player.h) |
| Deformation stream | Per-instance compute output and producer-matched previous positions, separate from immutable geometry. | [ADR-071](adr/071-animation-bank-and-reference-pose.md) |
| Mesh skin data | Four-influence records following cooked vertex order, with node-selected skin palettes and animation-source identity; stored in skinned `.vkb` version 18. | [ADR-030](adr/030-offline-mesh-optimization-and-cooking.md), [vkr_mesh_skin.h](../runtime/src/assets/vkr_mesh_skin.h) |
| KTX2 | Texture container of every cooked `.vkt`, holding host-native ASTC or BC blocks; there is no transcodable encoding. | [ADR-012](adr/012-texture-compression-pipeline.md) |
| Native ASTC texture | `.vkt` holding ASTC blocks, 6x6 colours and data masks and 4x4 normals, that a workspace built for an ASTC host; uploaded without transcoding. `astc` comes from astcenc; `astc-fast`, from Apple's system encoder at the editor's fast encode speed. | [ADR-012](adr/012-texture-compression-pipeline.md) |
| Native BC texture | `.vkt` holding BC7 (colour and data, bc7e) or BC5 (normals, rgbcx) blocks that a workspace built for an x86-64 host; uploaded without transcoding. `bc-fast` encodes colours with bc7e's fastest profile at the editor's fast encode speed. | [ADR-012](adr/012-texture-compression-pipeline.md) |
| MTSDF / em / DPI | Multi-channel signed-distance field with true-distance alpha / font-relative layout unit / display scale used before UI layout. | [ADR-035](adr/035-canonical-mtsdf-screen-pixel-range-shading.md), [ADR-036](adr/036-dpi-derived-ui-text-scale.md) |
| Immediate-mode UI | Widgets are declared each frame while stable IDs retain interaction, layout, and text caches. | [ADR-027](adr/027-immediate-mode-grid-ui.md) |
| Picking | Rendered object-ID selection in Scene viewport coordinates, with physics ray selection for enabled collider debug display and priority for gizmos. | [vkr_frame_input.h](../renderer/src/vkr_frame_input.h) |
| Case / profile | Harness workload definition / execution and evidence policy. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Snapshot / baseline | Captured run artifacts / reviewed immutable reference generation. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Authoritative measurement | A report satisfying its provenance, comparability, validity, and repetition policy; process success is a separate result. | [ADR-051](adr/051-renderer-harness-and-evidence.md) |
| Accepted / partial / proposed | Decision in force / explicit remaining integration / unimplemented feature or unsettled design. Native evidence limitations are stated separately. | [documentation skill](../.codex/skills/vkr-docs/SKILL.md) |

Editor workflow terms:

| Term | Meaning in VKR | Owner |
|---|---|---|
| Workspace | User-selected directory whose `.vkreditor` child contains managed projects, editor bundles and caches. | [Project store](../editor/src/editor_project_store.h) |
| Workbench | Editor tab under the top bar: a dock layout, the floating windows open in it and the Scene's editing mode for one task, such as Level Design. Not a workspace. | [ADR-089](adr/089-editor-workbenches.md) |
| Host path | Filesystem location represented as UTF-8 at C interfaces and converted to native syntax at I/O. | [ADR-070](adr/070-portable-path-boundaries.md) |
| Managed reference | Owner-relative serialized path with `/` separators and validated raw segments, resolved with physical containment checks. | [ADR-070](adr/070-portable-path-boundaries.md) |
| Resource reference | Runtime asset reference with explicit owner-relative or legacy repository-root semantics, separate from a source format URI. | [Asset resolver](../lib/src/filesystem/vkr_asset_path.h) |
| Project | Version 1 JSON owner of a name, scene membership, default font, asset inventory and editor preferences. | [ADR-069](adr/069-editor-projects-and-workspaces.md) |
| Managed scene | Version 5 authored scene document with stable ID, typed asset references and separate build revisions; its asset records live in the immutable inventory revision it names (version 3 kept them inline). `vkr_bakery project` lowers it to runtime inputs. | [Project jobs](../tools/bakery/project/vkr_project_lower.c) |
| Derived texture cache | Workspace `cache/generated` directory of cooker-derived textures named by source content and parameters; managed bundles hold clones of the variants their materials use. | [Mesh cooker](../tools/assets/vkr_mesh_cook_source.c) |
| Source identity | Stable managed-scene or cooked source-node identity used to bind authored edits independently of imported file location. | [Scene loader](../runtime/src/renderer/resources/loaders/scene_loader.c) |
| Bakery | Editor queue that runs `vkr_bakery` recipes, project jobs, project packages and scene bakes one child at a time; managed project jobs publish into an explicit workspace. Under Develop in the menu bar. | [Bakery](../editor/src/editor_bakery.c) |
| Game settings / build profile | A project's shipped identity and startup in `game.json` / one named target of it: platform, `development` or `shipping`, output folder, extra includes and options. | [ADR-078](adr/078-project-build-and-packaging.md) |
| Package | Output of a project build: the player as `<executable>`, one backend's shader catalog, `content/game.vkpak`, `content/engine.vkpak` and `bundle.json` version 2. | [Package](../tools/bakery/vkr_bakery_package.c) |
| Editor distribution | Relocatable folder `build_editor_dist.sh` installs: the editor, its companion programs, shader catalog, engine `content/` and player template, with writable state in per-user directories. | [ADR-078](adr/078-project-build-and-packaging.md) |
| Content identity | Path below a package's content root: `project/...` for project and scene files, `editor/...` for editor-bundle assets, `assets/...` for engine resources; never absolute or `..`. | [ADR-078](adr/078-project-build-and-packaging.md) |
| Player template | Prebuilt `vkr_player` (development) and `vkr_player_shipping` with `template.json` and the engine resources every package carries, under `<build>/player`; packaging copies it and never compiles per project. | [Player](../player/src/main.c) |
| `vkr_bakery` | The single asset and shader build program: producers, action cache, scheduler, event stream, project jobs and bakes. | [ADR-077](adr/077-asset-build-system.md) |
| Producer / action | A registered source-to-product transform / one keyed run of it on one source and recipe; an unchanged key reuses cached products. | [Registry](../tools/bakery/vkr_bakery_registry.c) |
| Action cache | Per-user content-addressed store of products, action records and a path-hash index, shared by checkouts and workspaces. | [Cache](../tools/bakery/vkr_bakery_cache.c) |
| Bakery daemon | `vkr_bakery serve`: a local-socket process that runs Bakery commands for clients and reruns a command when watched files settle; the editor supervises one per process. | [Daemon](../tools/bakery/vkr_bakery_serve.c) |
| Content root | Directory relative asset paths resolve against: the repository, an installed program's `content/` directory, or a mounted bundle's `content/` directory. | [Mounts](../lib/src/filesystem/vkr_vfs.h) |
| Ready log | JSON lines a finalize job appends for each material whose final textures exist, which the editor applies to the open scene's live materials before the job publishes. | [ADR-077](adr/077-asset-build-system.md) |
| Texture tier | How far a managed import builds textures: `final`, `preview` (1,024-pixel mip floor) or `deferred` (none); `finalize_textures` brings preview and deferred assets to final. | [ADR-077](adr/077-asset-build-system.md) |
| `.vkpak` archive / bundle | Content archive of identity-addressed, hash-deduplicated entries / a directory with the runtime, shader catalog, archives and `bundle.json` that runs without the repository. | [ADR-077](adr/077-asset-build-system.md) |
| Script module | `<module>.script.json` naming C sources that `vkr_bakery` compiles into a hot-reload library and a static archive; the editor loads the library, and packaged games do not yet. | [Producers](../tools/bakery/vkr_bakery_script.c) |
| Shader catalog | Directory of compiled SPIR-V, MSL and metallib files with per-backend manifests that the renderer resolves shader files through. | [Catalog](../renderer/src/vkr_shader_catalog.c) |
| Content | Folder browser over project, scene and editor assets, scenes, presets, the World and built-in objects, with tile and list views and texture/material previews. | [Content browser](../editor/src/editor_content.c) |
| Brush / brush face | A convex solid entity with a `brush` component / one of its child entities with `brush_face`, a plane in the brush's space with a surface tag, a mark, an art-owned material and a texture projection. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md) |
| Heightfield / terrain | A square of 16-bit heights and four layer weights per sample in a `.vkrhf` file / an entity with a `terrain` component that names one; the scene keeps its samples, tile meshes and height field body. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#terrain) |
| Streaming source | A world position content streams around: the camera that draws the viewport, or a player; the host sets each container's every frame. | [ADR-086](adr/086-world-partition.md) |
| Cell / cell document | A square of the ground plane a partitioned scene streams editor-created roots by / the JSON file in `<scene>.cells/` that holds a cell's objects. | [ADR-086](adr/086-world-partition.md#cells) |
| Overview / proxy | A streamed terrain's every 16th sample, drawn where fine tiles are not / a baked, simplified mesh of an unloaded cell's brushes. | [ADR-086](adr/086-world-partition.md) |
| Origin rebase | Moving every container back by whole kilometres during Play so positions near the camera stay small; Reset restores the documents' positions exactly. | [ADR-086](adr/086-world-partition.md#origin-rebase) |
| LOD row / geomorph | A range's detail levels (index ranges and errors) in its geometry's decode metadata, selected per culling view / a terrain tile vertex's move toward the next level so that level switches change no geometry. | [ADR-085](adr/085-gpu-geometry-lod-and-terrain-geomorphing.md) |
| Spline / spline point | A curve through ordered control points: an entity with a `spline` component / one of its child entities with `spline_point`, which the curve visits by `order`. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#population) |
| Spline mesh / scatter / copy | Population rules: copies of a cooked mesh every `spacing` metres along a spline / seeded copies dropped onto the ground in a box; a copy is a runtime mesh instance the scene rebuilds and never saves. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#population) |
| Generated body | A static or sensor physics body a scene builds from authored data, such as brush or terrain collision, outside snapshots, documents and Reset. | [vkr_scene_physics.h](../runtime/src/renderer/systems/vkr_scene_physics.h) |
| Level checks | `level.lint` and `query.reachable`: a region's walkable floor sampled with physics raycasts and judged against the player capsule's size, `step_up` and slope limit. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md) |
| Output / input / connection | Entity IO (ADR-084): an output is a fact an entity fires, an input a request it handles, and a connection (an `io_connection` child of the source) wires one output to one input of an entity in the same container. | [vkr_io_router.h](../runtime/src/script/vkr_io_router.h) |
| Agent channel | The editor's per-user socket of typed operations (`ops.list`) and the `vkr_mcp` adapter that serves them over MCP 2026-07-28. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md) |
| Journal group | Journal entries that undo and redo as one step; a batch of agent edits is one group. | [vkr_scene_edit.h](../runtime/src/renderer/systems/vkr_scene_edit.h) |
| Pending change | A reviewed agent batch the designer has not accepted or rejected; Reject reverts its journal group. | [editor ops](../editor/src/editor_ops.h) |
| Settle | Wait until no brush, shape, terrain or population rule rebuilds after earlier edits and no scene loads; agent reads of collision or built geometry settle by default, and a write settles with `settle`. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#settling) |
| Surface tag / mark / greybox look | What a brush face is made of (concrete, metal, wood and so on) / a level-design accent over it (hazard stripes or a wayfinding colour) / the fixed engine material a tag and mark show, per face orientation, until the art pass gives the face a material. | [vkr_surface.h](../runtime/src/level/vkr_surface.h) |
| Surface theme | A `.surfaces` document binding surface tags to materials; a container's `surface_theme` selects one, and tags it leaves unbound take the World's. Faces without their own material show their tag's binding. | [ADR-094](adr/094-surface-themes-and-art-pass.md) |
| World size | A material's `world_size`: the meters one texture repeat covers on brush faces, which a face's UV scale multiplies. | [ADR-094](adr/094-surface-themes-and-art-pass.md) |
| Material graph / instance | A `.mtg` document of typed nodes whose surface output gives the material model's inputs / a `.mt` that names a graph (`graph=`) and overrides its exposed parameters (`param.<name>=`). | [ADR-093](adr/093-material-graphs-and-art-workbench.md) |
| Standard tier | Graphs that lower to `.mt` factors, textures and state, which the fixed shaders read: no shader and no pipeline of their own. | [ADR-093](adr/093-material-graphs-and-art-workbench.md) |
| Layered material | An opaque Standard material that blends up to three layer materials (`layer1=` to `layer3=`) over its own surface, weighed by the vertex colour, a mask texture, slope or height; a terrain is one weighed by vertex colour. | [ADR-095](adr/095-layered-standard-materials.md) |
| Custom tier / Custom graph | Graphs the Standard tier cannot lower: each becomes a generated surface function (`vkr_custom_<hash>`) with its own tiled pipelines, shared by every instance; Metal only, Vulkan draws the fallback. | [ADR-096](adr/096-custom-material-graphs.md) |
| Project material library | `project_materials.metallib` in the shader catalog: every Custom graph's function and entries, which `vkr_bakery materials` compiles and the renderer reloads after an edit. | [ADR-096](adr/096-custom-material-graphs.md) |
| Look volume | A `look_volume` box that overrides exposure, grading, bloom, height fog and sky light near the camera, blended by priority and blend distance once a frame on the CPU. | [ADR-097](adr/097-look-volumes.md) |
| Environment preset | A `.environment` document holding a container's sky light, atmosphere, clouds, fogs, post process, time of day, sun and moon; applying it copies the values in. | [ADR-098](adr/098-environment-panel-and-presets.md) |
| Artist view / data view | A render mode for checking art (ADR-099): data views (base colour, roughness, metallic, normals, material cost, texel density) show values without exposure or tone curve; the exposure view false colours stops from middle grey. | [ADR-099](adr/099-artist-views-and-luminance-queries.md) |
| Bake settings | The lightmap and diffuse volume options the editor sends with a project's Bake lighting job; a zero keeps Bakery's default. | [ADR-100](adr/100-lighting-tools.md) |
| Scatter area | A painted disc under a scatter (`scatter_area`): radius, density and spacing; a scatter with areas places its copies only in them. | [ADR-102](adr/102-scatter-painting.md) |
| Fallback / late draw | A Custom material's Standard look from its graph's constant outputs / a camera draw that used it because the graph's pipelines did not exist yet, counted in `pipelines.late`. | [ADR-096](adr/096-custom-material-graphs.md) |
| Document journal | The editor's undo history of material files: each write's text before and after, numbered from the scene journals' counters so Undo takes scene and document steps in order. | [editor_material.h](../editor/src/editor_material.h) |
| Quick read / author | An agent operation cheap enough to share a build with other quick reads / the name a request carries (`agent`), recorded on its changes and limiting its undo to its own batches. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#socket-and-messages) |
| Claim / change feed | A box of a scene one agent builds in, where other agents' writes are refused / the ordered events of what every author applied, accepted, rejected, claimed or released. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#working-beside-other-agents) |
| Tag | A category word an entity carries in its `tags` component, stored lowercase with a `#` (`#labs`, `#chair`), by which designers and agents find objects to reuse. | [ADR-084](adr/084-agent-channel-and-level-design-toolkit.md#tags) |
| Scene edit overlay | Authored overrides validated against source identities. Legacy saves use `<scene>.editor.json`; managed saves publish immutable overlay revisions referenced by the scene manifest. | [Scene edit owner](../runtime/src/renderer/systems/vkr_scene_edit.c), [project store](../editor/src/editor_project_store.c) |

Object model terms ([ADR-076](adr/076-project-object-model.md)):

| Term | Meaning in VKR | Owner |
|---|---|---|
| Object | Anything the editor can select, inspect and search: an asset or an entity. | [ADR-076](adr/076-project-object-model.md) |
| Type descriptor / property table | Static description of a component type / its ordered property list driving Details, JSON, validation, the journal, presets and Cmd paths. | [vkr_type_desc.h](../runtime/src/core/vkr_type_desc.h) |
| Component / entity | Typed plain data stored in ECS chunks / an ID plus its components, the UE5-style actor. | [vkr_entity.h](../runtime/src/core/vkr_entity.h), [scene types](../runtime/src/renderer/systems/vkr_scene_types.h) |
| Object kind | A creatable object: empty, cube, text, a light kind or one live world component type; Content lists each in Objects. | [scene panels](../editor/src/editor_scene_panels.h) |
| Singleton component | World component type with at most one effective instance per frame: the primary scene's enabled, visible instance, else the World's while the scene inherits it. | [ADR-076](adr/076-project-object-model.md) |
| Container | One loaded `VkrScene`: the World, the primary scene or an additive scene, each with its own ECS world, document, overlay and journal; the entity ID world field names it. | [vkr_scene_system.h](../runtime/src/renderer/systems/vkr_scene_system.h) |
| World | The project's root container (`VKR_SCENE_WORLD_ROOT_ID`), loaded from `world.scene.json` while a project is open; holds the sun, sky, fog and post process every scene can inherit. | [ADR-076](adr/076-project-object-model.md) |
| Content / System | Content browser roots: the World with its objects, a folder per scene, project assets and folders / what the editor ships: Assets, Objects (object kinds) and Editor. | [editor content](../editor/src/editor_content.c) |
| Primary / additive scene | The scene the Scene panel shows (world 0) / scenes loaded beside it with `scene.add` (worlds 1 to 6), whose singletons have no effect. | [ADR-076](adr/076-project-object-model.md) |
| Inherit World | Per-scene undoable setting; off makes the scene use only its own objects, except World-only types. |
| World-only type | A type only the World holds, which every scene resolves from it: physics settings (gravity, collision layers) and animation settings (clock scale). | [vkr_type_desc.h](../runtime/src/core/vkr_type_desc.h) |
| Set primary | Makes an added project scene the primary scene and adds the previous primary back beside it. | [editor projects](../editor/src/editor_projects.h) | [vkr_scene_edit.h](../runtime/src/renderer/systems/vkr_scene_edit.h) |
| Physics set | One Jolt world shared by every loaded container; the primary scene drives its clock and the World's physics settings set gravity. | [vkr_scene_physics.h](../runtime/src/renderer/systems/vkr_scene_physics.h) |
| Registered type | A component descriptor a module outside the renderer registers before scenes initialize; it is stored and edited like a world type. | [scene types](../runtime/src/renderer/systems/vkr_scene_types.h) |
| Preset | Named typed value of one world component or light type in the project's `presets.json`, applied as an undoable edit. | [project store](../editor/src/editor_project_store.h) |
| Prefab instance | A copy of another project scene placed under one new root entity, with new ids and no link to its source; linked prefabs belong to the behavior proposal. | [ADR-076](adr/076-project-object-model.md) |
| Document id / entity reference | An entity's UUID (`VkrEntityRef`): its document's id, or the random one the editor gave an entity it created. Overlays bind document entities through it; an `ENTITY` property or script field stores one to name an entity of its own container. | [vkr_entity_ref.h](../lib/src/core/vkr_entity_ref.h) |
| Content folder / tag | Virtual folder path or comma-separated label over an item ID in `content.labels.json`; files never move. System folders (Objects, Editor, Scene assets, Presets) are fixed. | [project store](../editor/src/editor_project_store.h) |
| Outliner / Details | The World's entity tree with loaded scenes nested under it / the descriptor-generated property editor below it. | [scene panels](../editor/src/editor_scene_panels.c), [Details](../editor/src/editor_details.h) |
| Preferences | Machine-local graphics gates edited through descriptors; never project content. | [vkr_graphics_settings.h](../runtime/src/vkr_graphics_settings.h) |
