---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../rendering/render-graph-design.md`](../rendering/render-graph-design.md). Retained for history; do not treat as current.
# Render Graph Progress

## Phase 1: JSON Contract + Loader (Jan 30, 2026)

### Implemented
- Added JSON schema and the initial render graph definition file.
- Implemented a JSON loader + validation layer that parses resources, passes,
  attachments, conditions, repeat blocks, usage/access enums, and outputs.
- Added the pass executor registry (registration + lookup + cleanup).
- Wired renderer startup to validate the graph JSON from
  `assets/render_graphs/main.rendergraph.json` using a scratch scope.

### Implementation Details
- New files:
  - `lib/src/renderer/vkr_render_graph.h`
  - `lib/src/renderer/vkr_render_graph.c`
  - `lib/src/renderer/vkr_rg_json.h`
  - `lib/src/renderer/vkr_rg_json.c`
  - `assets/render_graphs/main.rendergraph.json`
  - `docs/rendering/render-graph-schema.json`
- Renderer frontend changes:
  - Initialize `VkrRgExecutorRegistry` during systems init and destroy it on
    shutdown.
  - Validate render graph JSON at startup; invalid graphs fail init early.

### Notes / Deferred
- Executor name validation against registry is deferred until pass modules are
  implemented (Phase 3).
- JSON is loaded and validated only (no runtime build/compile/execute yet).
- Hot reload not implemented in Phase 1.

## Phase 2: Render Graph Core (Jan 30, 2026)

### Implemented
- Public render graph API expanded: handles, resource/pass descriptors, builder
  functions, and compile/execute entry points.
- Internal graph model + lifetime ownership rules (resources persist across
  frames, passes are rebuilt each frame).
- Dependency analysis (per-resource last-writer/last-readers) with edge
  construction and deduping.
- Pass culling from outputs + NO_CULL passes, topological sort, and execution
  order storage.
- Resource lifetime tracking (first/last pass in execution order).
- Barrier planning (image + buffer state transitions) recorded per pass.
- Execute path added (walk execution order and call pass callbacks).

### Implementation Details
- New files:
  - `lib/src/renderer/vkr_render_graph_internal.h`
  - `lib/src/renderer/vkr_rg_compile.c`
  - `lib/src/renderer/vkr_rg_execute.c`
- Updated files:
  - `lib/src/renderer/vkr_render_graph.h`
  - `lib/src/renderer/vkr_render_graph.c`

### Notes / Deferred
- Render pass / render target creation is not wired yet (passes execute without
  auto-created render targets). This is deferred to Phase 3 when porting the
  actual rendering passes.
- Barrier plans are generated but not yet applied to Vulkan command buffers.

## Phase 3: Port Rendering Passes (Jan 30, 2026)

### Implemented
- Added render graph build step from JSON into the core builder API.
- Added render-graph pass modules (shadow, skybox, world, UI, editor) and
  executor registration.
- Integrated render graph build/compile/execute into the frame draw path.
- Added per-pass execution bridge that rendered view-system layers through the
  graph for parity while the view system owned render targets (removed in
  Phase 4).
- Added swapchain format access for resolving `SWAPCHAIN` format tokens.

### Implementation Details
- New files:
  - `lib/src/renderer/passes/vkr_pass_shadow.c`
  - `lib/src/renderer/passes/vkr_pass_shadow.h`
  - `lib/src/renderer/passes/vkr_pass_skybox.c`
  - `lib/src/renderer/passes/vkr_pass_skybox.h`
  - `lib/src/renderer/passes/vkr_pass_world.c`
  - `lib/src/renderer/passes/vkr_pass_world.h`
  - `lib/src/renderer/passes/vkr_pass_ui.c`
  - `lib/src/renderer/passes/vkr_pass_ui.h`
  - `lib/src/renderer/passes/vkr_pass_editor.c`
  - `lib/src/renderer/passes/vkr_pass_editor.h`
- Updated files:
  - `lib/src/renderer/vkr_rg_json.h` (graph builder API)
  - `lib/src/renderer/vkr_rg_json.c` (JSON -> graph build, repeat/condition
    expansion, executor lookup)
  - `lib/src/renderer/vkr_render_graph.c` (single-layer pass draw helper;
    temporary, removed in Phase 4)
  - `lib/src/renderer/renderer_frontend.c` (graph init + per-frame build/execute)
  - `lib/src/renderer/renderer_frontend.h` (render graph state)
  - `lib/src/renderer/vkr_renderer.h` (swapchain format getter, view pass draw)
  - `lib/src/renderer/vulkan/vulkan_backend.c` (swapchain format getter)

### Notes / Deferred
- Deferred items from Phase 3 (graph-owned render targets/barriers, editor
  viewport sizing, and view-system removal) were resolved in Phases 4-5.

## Phase 4: Remove View System (Complete) (Jan 30, 2026)

### Implemented
- Removed the shadow data request message path; world rendering now queries the
  shadow system directly for per-frame shadow data.
- Replaced editor/app offscreen toggles and size updates with direct world
  resources APIs (no view-layer messages).
- Editor viewport mapping lookup now reads state directly (no view-layer
  message roundtrip).
- World text create/update/transform/destroy now uses direct world resources
  APIs instead of layer messages.
- UI text create/update/destroy now uses direct UI system APIs instead of
  view-layer messaging.
- Shadow instance invalidation on scene shutdown now calls a direct shadow
  system API (no view-layer message).
- Render graph import resources now record swapchain/depth formats and flags,
  preparing for graph-owned render targets.
- Render graph now allocates image resources and builds render passes/targets
  from pass attachments, then executes passes without the view-system target
  bridge.
- Render graph applies per-pass image layout transitions and transitions the
  present image back to `PRESENT_SRC_KHR` after execution.
- Render graph applies per-pass buffer access barriers (Vulkan backend).
- Render graph now allocates graph-owned buffers (including PER_IMAGE) and
  uses them for barrier application.
- Added render-graph helpers to resolve image/buffer handles for pass
  executors (per-image aware).
- Removed view-system render fallback; render graph is now the only draw path.
- Added direct pass render entry points (world/skybox/ui/editor/shadow) and
  wired render-graph pass executors to call them without view-layer bridging.
- Removed `vkr_pass_view_layer.*` and stopped routing passes through
  layer-context render helpers.
- Removed the view system API surface and deleted `view system (removed)`.
- Added per-view lifecycle wrappers (`*_unregister`, `*_resize`) and wired the
  renderer frontend to call them directly.
- View modules no longer depend on view-system headers.
- Render graph viewport sizing now uses the editor viewport mapping when the
  editor is enabled.

### Implementation Details
- Updated `lib/src/renderer/passes/vkr_pass_world.c` to call
  `vkr_shadow_system_get_frame_data()` and gate on `shadow_system.initialized`
  instead of the view-layer message bridge.
- Added editor viewport utilities (`vkr_editor_viewport_compute_mapping`,
  `vkr_viewport_mapping_window_to_target_pixel`) and built editor payloads
  directly in `app/src/main.c`.
- Routed world/UI text creation and updates through stateless resource owners
  (`vkr_world_resources`, `vkr_ui_system`) from `lib/src/renderer/systems/vkr_scene_system.c`
  and `app/src/main.c`.
- Extended `VkrRenderGraphFrameInfo` with swapchain depth format, and updated
  `lib/src/renderer/vkr_rg_json.c` to stamp import image descriptors with
  formats, sizes, and flags.
- Added graph-owned image allocation and renderpass/target creation in
  `lib/src/renderer/vkr_rg_compile.c`, plus cache-backed cleanup in
  `lib/src/renderer/vkr_render_graph.c`.
- Synced render-graph `scene_color` textures into `RendererFrontend` offscreen
  handles so the editor composite can sample the graph output.
- Pass executors now call stateless resource owners for draw logic:
  - `lib/src/renderer/passes/vkr_pass_world.c` → `vkr_world_resources`
  - `lib/src/renderer/passes/vkr_pass_ui.c` → `vkr_ui_system`
  - `lib/src/renderer/passes/vkr_pass_skybox.c` → `vkr_skybox_system`
- Application/editor toggles now feed the packet and editor viewport mapping,
  not view APIs.
- Applied render-graph image barriers in `lib/src/renderer/vkr_rg_execute.c`.
- Added renderer buffer barrier API and Vulkan implementation, then wired it
  into render graph execution.
- Added render-graph buffer allocation (imported + owned), with usage-driven
  memory properties and per-image counts.
- Added public accessors for resolving graph resources in pass contexts.
- Removed view-system entry points from `lib/src/renderer/vkr_renderer.h` and
  deleted `lib/src/renderer/systems/view system (removed)`.
- Added view resize/unregister helpers in:
  - `lib/src/renderer/passes/vkr_pass_world.c`
  - `lib/src/renderer/passes/vkr_pass_ui.c`
  - `lib/src/renderer/passes/vkr_pass_editor.c`
  - `lib/src/renderer/passes/vkr_pass_skybox.c`
- Updated `lib/src/renderer/renderer_frontend.c` to call the new view
  lifecycle helpers on resize and shutdown.
- View modules now render without layer-context scaffolding or view-system
  headers.
- Updated render-graph frame setup in
  `lib/src/renderer/renderer_frontend.c` to consume editor viewport mapping
  for `viewport_width`/`viewport_height`.

### Notes / Deferred
- Items noted here were resolved in Phase 5 (typed layer messages removed and
  offscreen target plumbing deleted).

## Phase 5: Render Graph Cleanup + Validation (In Progress) (Jan 31, 2026)

### Implemented
- Removed typed layer messaging (deleted `vkr_layer_messages.*` and
  `on_data_received` callbacks).
- Trimmed UI offscreen render-target plumbing; UI now uses viewport override
  sizing only.
- Editor viewport sizing now updates UI layout directly (no World offscreen
  toggle path); demo app toggles editor without changing World offscreen state.
- Editor viewport sizing now resizes camera projections to match render-graph
  viewport dimensions and resets on disable.
- Removed World offscreen render-target creation and offscreen pipeline variants;
  World now relies on render-graph render targets.
- Added Vulkan layout transitions for `PRESENT_SRC_KHR` <-> color attachment to
  support swapchain barriers from the render graph.
- Added render-graph resource lifetime stats (live/peak counts + buffer bytes).
- Logged render-graph resource stats on scene unload to help spot growth across
  reloads.
- Switched swapchain/depth imports to `UNDEFINED` initial layout and recorded
  final per-image layouts during compile. Removed the render-graph present
  transition so the Vulkan backend’s end-frame path owns the swapchain
  PRESENT barrier (avoids double-transition warnings).
- Shadow maps are now a single layered depth array resource; shadow passes slice
  into the array per cascade, and world sampling uses the render-graph shadow
  map handle.
- Shadow pass execution now pushes per-cascade light VP constants to satisfy
  shader push-constant requirements.
- Shader instance release now resolves the owning pipeline by shader name to
  avoid out-of-bounds instance frees during shutdown.
- Render-graph barrier planning now uses DEPTH_STENCIL_READ_ONLY for sampled
  depth images to match descriptor layouts.
- Added Vulkan transitions for DEPTH_STENCIL_READ_ONLY layout changes.
- Standardized instance-state invalid handling to VKR_INVALID_ID across view,
  UI text, mesh, and shader systems; shader instance acquire/release now resolves
  the owning pipeline by name, and renderer shutdown releases shader instances
  before pipeline registry teardown.
- Guarded pipeline registry instance-state releases against VKR_INVALID_ID and
  quieted shadow MDI fallback logging (invalid MDI batches now silently fall
  back to direct draws).
- Shadow material instance arrays now initialize to VKR_INVALID_ID to avoid
  releasing unacquired instance ids during scene shutdown.
- Vulkan shader instance release now guards against out-of-bounds ids and logs
  instead of asserting; invalid instance handle releases are ignored in the
  Vulkan backend.
- Shadow view now only acquires/binds per-material instance state for alpha-test
  draws, keeping opaque shadow paths free of mismatched instance ids.
- Shadow MDI path now packs valid commands per chunk and falls back to direct
  draws for only the invalid submeshes instead of disabling MDI for the whole
  batch.
- World MDI path now packs valid commands per chunk and falls back to direct
  draws for only the invalid submeshes; invalid-MDI warnings are logged once.
- Shadow invalid-MDI warnings are now rate-limited to once per frame across
  cascades.
- MDI batching now separates opaque-index-eligible submeshes from non-eligible
  ones (range_id override) to avoid false invalid-submesh warnings; shadow MDI
  respects the range_id flag when deciding opaque-index usage.
- Render graph now warns when a non-imported, non-persistent resource is read
  before any write (helps catch missing writer passes).
- Render graph now validates image/buffer access flags against declared usage
  and fails compile on mismatches.
- Render graph now validates attachment slices against image layer/mip counts.
- Skybox view no longer uses layer-context scaffolding; it now initializes and
  renders directly from renderer/state.
- Editor view no longer uses layer-context scaffolding; it now initializes,
  resizes, and renders directly from renderer/state.
- UI view no longer uses layer-context scaffolding; it now initializes,
  resizes, and renders directly from renderer/state.
- Shadow view no longer uses layer-context scaffolding; it now initializes and
  renders directly from renderer/state with render targets provided by the
  render graph.
- World view no longer uses layer-context scaffolding; it now initializes,
  resizes, and renders directly from renderer/state.
- Removed the remaining layer system types and helpers (`vkr_layer.*`) and
  stripped layer-related public API from `vkr_renderer.h`.
- Documentation cleanup started: render-graph design and UI system overview
  updated to remove view-system references; docs index marks legacy view-layer
  docs.
- Documentation cleanup continued: added legacy notes to editor/scene/ui/text/
  terrain/CSM docs, updated docs index descriptions, and refreshed render-pass
  notes to reference render-graph ownership.
- Added render-graph DOT exporter (`vkr_rg_debug.h/.c`) to visualize pass and
  resource dependencies.

### Implementation Details
- Updated files:
  - `lib/src/renderer/vkr_render_graph.h`
  - `lib/src/renderer/vkr_render_graph_internal.h`
  - `lib/src/renderer/vkr_render_graph.c`
  - `lib/src/renderer/vkr_rg_compile.c`
  - `lib/src/renderer/vkr_rg_execute.c`
  - `lib/src/renderer/vkr_rg_json.c`
  - `lib/src/renderer/vkr_rg_json.h`
  - `lib/src/renderer/vkr_rg_compile.c`
  - `lib/src/renderer/vkr_render_graph.c`
  - `lib/src/renderer/vkr_render_graph.h`
  - `lib/src/renderer/passes/vkr_pass_world.c`
  - `lib/src/renderer/passes/vkr_pass_shadow.c`
  - `lib/src/renderer/systems/vkr_pipeline_registry.c`
  - `lib/src/renderer/systems/vkr_pipeline_registry.h`
  - `lib/src/renderer/systems/vkr_shader_system.c`
  - `lib/src/renderer/systems/vkr_pipeline_registry.c`
  - `lib/src/renderer/systems/vkr_mesh_manager.c`
  - `lib/src/renderer/renderer_frontend.c`
  - `lib/src/renderer/passes/vkr_pass_ui.c`
  - `lib/src/renderer/passes/vkr_pass_skybox.c`
  - `lib/src/renderer/passes/vkr_pass_editor.c`
  - `lib/src/renderer/passes/vkr_pass_shadow.c`
  - `lib/src/renderer/resources/ui/vkr_ui_text.c`
  - `assets/render_graphs/main.rendergraph.json`
  - `docs/rendering/render-graph-schema.json`
  - `lib/src/renderer/systems/vkr_scene_system.c`
  - `lib/src/renderer/passes/vkr_pass_ui.c`
  - `lib/src/renderer/passes/vkr_pass_ui.h`
  - `lib/src/renderer/passes/vkr_pass_editor.c`
  - `lib/src/renderer/passes/vkr_pass_editor.h`
  - `lib/src/renderer/passes/vkr_pass_world.c`
  - `lib/src/renderer/passes/vkr_pass_world.h`
  - `lib/src/renderer/vulkan/vulkan_image.c`
  - `app/src/main.c`
- Deleted files:
  - `lib/src/renderer/systems/vkr_layer_messages.c`
  - `lib/src/renderer/systems/vkr_layer_messages.h`

### Notes / Deferred
- None.
