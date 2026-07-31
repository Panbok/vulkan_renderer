---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# View/Layers System Implementation Plan

Goals: introduce a view system that owns a registry of render layers, each with its own size, view/projection, attached renderpasses, and per-layer render data. Layers are created from configs that also provide callbacks for resize/render. World/UI rendering logic currently inside `renderer_frontend.c` is migrated into built-in layers. The system stays rendering-focused and becomes the building block for a future render graph.

## Current renderer state (key observations)
- `renderer_frontend.c`: initialization builds two renderpasses (`Renderpass.Builtin.World` with color+depth clear, `Renderpass.Builtin.UI` color only) and regenerates render targets per swapchain image via `renderer_frontend_regenerate_render_targets`; resize callback uses `g_renderer_rt_refresh`.
- `RendererFrontend` holds world/ui-specific state: renderpasses and render targets, pipelines (`world_pipeline`, `ui_pipeline`), shader configs, materials, UI instance state/transform, `draw_state`, plus cached globals and window sizes.
- `vkr_renderer_draw_frame` records two passes manually:
  - World: iterate `VkrMeshManager`, resolve/bind pipeline per submesh, apply globals once, apply local+instance materials, draw geometry.
  - UI: bind UI pipeline/instance state, apply UI globals/local, draw default plane.
- `renderer_frontend_recompute_ui_globals` computes orthographic `globals.ui_projection/ui_view` from window size; called from resize path.
- `vkr_renderer_default_scene` loads shader configs/materials, creates world/UI pipelines, acquires UI instance state, loads demo meshes (falcon/sponza), and seeds UI transform. World/UI setup belongs in layer definitions; demo assets can stay here.
- Application loop (`application_start`) updates camera, writes `renderer.globals.view/projection/view_position`, updates mesh transforms, then calls `vkr_renderer_draw_frame`.

## Proposed architecture: view system and layers
- New module `vkr_view_system` manages ordered `VkrLayer` entries and handles RT rebuild, resize propagation, and per-pass rendering.
- Data model (public types in `vkr_renderer.h`):
  ```c
  typedef struct VkrLayerHandle { uint32_t id; uint32_t generation; } VkrLayerHandle;
  #define VKR_LAYER_HANDLE_INVALID ((VkrLayerHandle){0, 0})

  typedef struct VkrLayerContext VkrLayerContext; // opaque to callers

  typedef struct VkrLayerRenderInfo {
    uint32_t image_index;         // swapchain image being rendered
    String8 renderpass_name;      // pass currently active
  } VkrLayerRenderInfo;

  typedef struct VkrLayerCallbacks {
    void (*on_attach)(VkrLayerContext *ctx);                              // optional
    void (*on_resize)(VkrLayerContext *ctx, uint32_t width, uint32_t height);
    void (*on_render)(VkrLayerContext *ctx, const VkrLayerRenderInfo *info);
    void (*on_detach)(VkrLayerContext *ctx);                              // optional
  } VkrLayerCallbacks;

  typedef struct VkrLayerPassConfig {
    String8 renderpass_name;       // e.g. "Renderpass.Builtin.World"
    bool8_t use_swapchain_color;   // true for window-presentable layers
    bool8_t use_depth;             // attach depth buffer
  } VkrLayerPassConfig;

  typedef struct VkrLayerConfig {
    String8 name;
    uint32_t order;                // lower draws first
    uint32_t width, height;        // initial layer extent (default: window)
    Mat4 view;
    Mat4 projection;
    uint8_t pass_count;
    VkrLayerPassConfig *passes;
    VkrLayerCallbacks callbacks;
    void *user_data;               // layer-owned payload
  } VkrLayerConfig;

  // View system API
  bool32_t vkr_view_system_init(VkrRendererFrontendHandle renderer);
  void     vkr_view_system_shutdown(VkrRendererFrontendHandle renderer);
  bool32_t vkr_view_system_register_layer(VkrRendererFrontendHandle renderer,
                                          const VkrLayerConfig *cfg,
                                          VkrLayerHandle *out_handle,
                                          VkrRendererError *out_error);
  void     vkr_view_system_unregister_layer(VkrRendererFrontendHandle renderer,
                                            VkrLayerHandle handle);
  bool32_t vkr_view_system_set_layer_camera(VkrRendererFrontendHandle renderer,
                                            VkrLayerHandle handle,
                                            const Mat4 *view,
                                            const Mat4 *projection);
  void     vkr_view_system_on_resize(VkrRendererFrontendHandle renderer,
                                     uint32_t width, uint32_t height);
  void     vkr_view_system_rebuild_targets(VkrRendererFrontendHandle renderer);
  void     vkr_view_system_draw_all(VkrRendererFrontendHandle renderer,
                                    uint32_t image_index);
  ```
- `VkrLayer` internal fields (in `vkr_view_system.c`): handle, name, order, active flag, size (width/height), cached view/projection, user_data, callbacks, array of pass records (name, resolved renderpass handle, per-image render targets, attachment flags), pointer to per-layer internal render data (layer-owned struct).
- `VkrLayerContext`: wraps renderer pointer, owning layer, current pass index; exposes accessors/helpers for callbacks (get renderer subsystems, layer size, view/proj, set view/proj and mark globals dirty, get user_data, current renderpass handle/target).
- Render target rebuild: query image count via `vkr_renderer_window_attachment_count`; resolve renderpass handles by name; build render targets per pass using swapchain color attachment when `use_swapchain_color` and depth attachment when `use_depth`; store target arrays on the layer. Triggered on init and via backend `on_render_target_refresh_required`.
- Resize: update layer sizes, call `callbacks.on_resize`, recompute any cached view/projection if the layer chooses, and mark global state dirty for pipelines. UI layer will recompute orthographic projection here.
- Draw: layers sorted stably by `order`; for each layer/pass:
  1) begin renderpass with the pass handle + matching render target for `image_index`,
  2) invoke `on_render`,
  3) end renderpass.
  Guard with renderer mutex during target rebuild to avoid race with backend resize.

## Renderer integration changes
- `RendererFrontend` struct: add `VkrViewSystem view_system; VkrLayerHandle world_layer; VkrLayerHandle ui_layer;` and remove layer-owned fields (`world_renderpass`, `ui_renderpass`, `world_render_targets`, `ui_render_targets`, `render_target_count`, `world_pipeline`, `ui_pipeline`, `ui_instance_state`, `ui_transform`, `draw_state`, `world_shader_config`, `ui_shader_config`, `ui_material`, `world_material`). Layer-specific state moves into per-layer internal structs.
- Backend callback: replace `renderer_frontend_on_target_refresh_required`/`g_renderer_rt_refresh` with a view-system trampoline (e.g., static function calling `vkr_view_system_rebuild_targets`). Hook it in `VkrRendererBackendConfig.on_render_target_refresh_required` during `vkr_renderer_initialize` (chain user-provided callback if present).
- `vkr_renderer_systems_initialize`: after camera/pipeline/material/mesh systems are up, call `vkr_view_system_init`, then register built-in layers (below).
- `vkr_renderer_resize`: keep backend `on_resize` and window size updates; drop inline RT rebuild and UI globals; call `vkr_view_system_on_resize`; pipeline registry global dirty marking can move into view system when view/proj change.
- `vkr_renderer_draw_frame`: replace world/UI blocks with `vkr_view_system_draw_all(renderer, vkr_renderer_window_image_index(renderer));`.
- `vkr_renderer_destroy`: call `vkr_view_system_shutdown` before tearing down pipelines/materials/meshes to release per-layer resources safely.
- `vkr_renderer_default_scene`: trim to demo asset loading only (falcon/sponza); built-in layers own pipeline/material setup.

## Built-in layers module
- New files `lib/src/renderer/systems/vkr_view_layers_builtin.h/.c`.
- Provide `bool32_t vkr_view_layers_register_builtin(RendererFrontend *rf);` that registers two layers using the view system and stores handles in `rf->world_layer`/`rf->ui_layer`.
- World layer:
  - Pass config: `Renderpass.Builtin.World` with swapchain color + depth.
  - Internal state: world shader config, pipeline handle, optional default material handle, scratch `VkrShaderStateObject` if needed.
  - `on_attach`: load shader config via resource system, create pipeline via pipeline registry (aliasing by shader name), cache material handle if default material already loaded.
  - `on_render`: migrate world block from `vkr_renderer_draw_frame` (mesh iteration, pipeline resolve/bind, apply globals once, apply local/instance materials, draw geometry). Keep per-submesh instance state refresh semantics.
  - `on_resize`: likely no-op beyond letting app update camera/view; mark globals dirty if layer view/proj changed.
- UI layer:
  - Pass config: `Renderpass.Builtin.UI` with swapchain color only.
  - Internal state: UI shader config, UI pipeline handle, UI material handle, UI instance state, `VkrTransform ui_transform`, cached UI ortho matrices.
  - `on_attach`: load shader config/pipeline, acquire UI instance state, cache UI material if already loaded.
  - `on_resize`: recompute orthographic projection (migrated from `renderer_frontend_recompute_ui_globals`) and update layer/global UI matrices; mark pipeline globals dirty.
  - `on_render`: migrate UI block from `vkr_renderer_draw_frame` (resolve pipeline, ensure instance state matches pipeline, apply UI globals/local/instance, draw default plane geometry).

## Layer data flow for camera/view
- Application continues updating `renderer.globals.view/projection/view_position` from the active camera each frame. The world layer should mirror these into its layer state (via `vkr_view_system_set_layer_camera`) before rendering so pipeline globals match. UI layer manages its own view/proj from resize.
- When layer view/proj are updated, call `vkr_pipeline_registry_mark_global_state_dirty` to force uniform updates on next draw.

## Implementation steps for Codex
1) Add public API/types to `vkr_renderer.h` (layer handles/configs/callbacks, view system entry points).
2) Create `vkr_view_system.h/.c`: define `VkrLayer`, `VkrLayerPass`, `VkrViewSystem`, `VkrLayerContext`, registry management, target rebuild, resize, draw, and helper accessors for callbacks.
3) Create `vkr_view_layers_builtin.h/.c`: implement world/UI layer internal structs and callbacks, migrate logic from `renderer_frontend_recompute_ui_globals`, world/UI sections of `vkr_renderer_draw_frame`, and pipeline/material setup from `vkr_renderer_default_scene`.
4) Refactor `renderer_frontend.c/h`:
   - Add view system fields; remove world/ui-specific renderpass/target/pipeline fields.
   - Replace render-target regeneration callback with view-system hook.
   - Wire `vkr_view_system_init`/`shutdown`, register built-in layers in systems init, delegate resize/draw to view system.
   - Simplify `vkr_renderer_default_scene` to load demo meshes/materials only.
5) Adjust application code if needed to update layer view/projection via `vkr_view_system_set_layer_camera` (or let world layer read from globals each frame) and ensure resize path goes through new view system.
6) Validate: world scene renders, UI plane visible, resize works, swapchain recreation rebuilds targets, no stale pipelines/instance states left on shutdown.

## Risks / considerations
- Keep resource lifetime ordering: layer shutdown should release instance states/pipelines before registry/material/mesh shutdown.
- Ensure render targets rebuilt on both explicit resize and backend-triggered swapchain recreation.
- Stable ordering for multiple layers (future render graph) and avoidance of duplicate pass begins.
- Avoid race conditions during resize by using existing renderer mutex when rebuilding targets or drawing.
