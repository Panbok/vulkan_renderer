---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../architecture/renderer-architecture-spec.md`](../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# View System (Layered Rendering) for Vulkan Renderer

### Goals

- **Introduce a view system** that manages an ordered registry of rendering layers.
- **Each layer** stores size, view/projection, attached render passes (by name only), and internal per-layer render data.
- **Create layers from configs** with callbacks for resize and render; the system owns render pass begin/end and all target selection.
- **Clean the renderer frontend**: move state and logic from `default_scene` and `draw_frame` into World/UI layers. Keep frontend operating on high-level primitives (meshes, materials, pipelines), no swapchain exposure.
- **Rendering surfaces are abstracted**: the view system selects targets internally (window or offscreen later). Public API never mentions swapchain/attachments.
- **Register built-in World/UI layers during renderer systems initialization** (not in `default_scene`).

### High-level design

- New module `vkr_view_system` maintains a registry of `VkrLayer` objects (ordered by `order`).
- A `VkrLayer` is created via `VkrLayerConfig` (name, order, size, optional initial view/proj, list of renderpass names, callbacks, user_data).
- For each layer pass, the system resolves the renderpass handle by name (e.g., `Renderpass.Builtin.World`, `Renderpass.Builtin.UI`) and internally builds per-image render targets using appropriate attachments (window color/depth). Attachments are not exposed to layers or callers.
- On frame render: for each layer and each configured pass: system begins the pass, invokes `on_render(ctx, info)` to record draws using high-level renderer systems, then ends the pass.
- On resize/swapchain refresh: system rebuilds per-image targets and calls `on_resize` for layers.

### Public API additions (in `lib/src/renderer/vkr_renderer.h`)

Provide only high-level layer constructs; no swapchain/attachment types are exposed.

```c
// Handles
typedef struct VkrLayerHandle { uint32_t id; uint32_t generation; } VkrLayerHandle;
#define VKR_LAYER_HANDLE_INVALID ((VkrLayerHandle){0})

// Opaque context passed to callbacks
typedef struct VkrLayerContext VkrLayerContext;

typedef struct VkrLayerRenderInfo {
  uint32_t image_index;              // current window image index
  String8  renderpass_name;          // name of the pass being rendered
} VkrLayerRenderInfo;

typedef struct VkrLayerCallbacks {
  void (*on_attach)(VkrLayerContext* ctx);                 // optional
  void (*on_resize)(VkrLayerContext* ctx, uint32_t w, uint32_t h);
  void (*on_render)(VkrLayerContext* ctx, const VkrLayerRenderInfo* info);
  void (*on_detach)(VkrLayerContext* ctx);                 // optional
} VkrLayerCallbacks;

typedef struct VkrLayerPassBinding {
  String8 renderpass_name;          // e.g., "Renderpass.Builtin.World"
} VkrLayerPassBinding;

typedef struct VkrLayerConfig {
  String8 name;
  uint32_t order;                    // render order; smaller renders first
  uint32_t initial_width, initial_height;
  Mat4 initial_view;                 // optional
  Mat4 initial_projection;           // optional
  uint8_t pass_count;                // how many passes this layer participates in
  VkrLayerPassBinding* passes;       // pass names only; targets are implicit
  VkrLayerCallbacks callbacks;
  void* user_data;                   // optional user state
} VkrLayerConfig;

// View system API (no swapchain exposure)
bool32_t vkr_view_system_init(VkrRendererFrontendHandle renderer);
void     vkr_view_system_shutdown(VkrRendererFrontendHandle renderer);

bool32_t vkr_view_system_register_layer(VkrRendererFrontendHandle renderer,
                                        const VkrLayerConfig* cfg,
                                        VkrLayerHandle* out_handle,
                                        VkrRendererError* out_error);
void     vkr_view_system_unregister_layer(VkrRendererFrontendHandle renderer,
                                          VkrLayerHandle handle);

void     vkr_view_system_on_resize(VkrRendererFrontendHandle renderer,
                                   uint32_t width, uint32_t height);
void     vkr_view_system_rebuild_targets(VkrRendererFrontendHandle renderer);
void     vkr_view_system_draw_all(VkrRendererFrontendHandle renderer,
                                  uint32_t image_index);
```

Notes:

- Passes are referenced **by name only**. The system resolves them and selects appropriate targets internally (window color/depth for built-ins). This keeps layers operating at the same high-level as current world/UI code.

### New files/modules

- `lib/src/renderer/systems/vkr_view_system.h/.c`
  - Implements registry, per-layer contexts (renderer*, size, view/proj, per-pass resolved pass handles, internal per-image render targets, user_data), (re)build targets, draw, resize, shutdown.
  - Opaque `VkrLayerContext` exposes accessors to needed high-level subsystems (camera, pipeline registry, material, geometry, mesh) via the renderer pointer.
- `lib/src/renderer/systems/vkr_view_layers_builtin.h/.c`
  - Built-in World and UI layer callbacks and `vkr_view_layers_register_builtin(renderer)` that registers them during renderer systems initialization.

### Renderer integration changes and cleanup

- `lib/src/renderer/renderer_frontend.c`
  - Init (systems): after backend creation and core systems init (camera, shader, pipeline registry, material, geometry, mesh), call:
    - `vkr_view_system_init(renderer)`
    - `vkr_view_layers_register_builtin(renderer)` to register World/UI layers:
      - World layer: passes = {"Renderpass.Builtin.World"}
      - UI layer:    passes = {"Renderpass.Builtin.UI"}
  - Remove render-target management from frontend:
    - Delete `renderer_frontend_regenerate_render_targets()` and callback plumbing (`g_renderer_rt_refresh`, `renderer_frontend_on_target_refresh_required`).
    - Remove fields from `RendererFrontend` (moved into view system): `world_render_targets`, `ui_render_targets`, `render_target_count`, `world_renderpass`, `ui_renderpass`.
  - Move world/UI-specific state out of frontend into layer contexts:
    - Remove: `world_pipeline`, `ui_pipeline`, `ui_instance_state`, `ui_transform`, and `draw_state` from `RendererFrontend`.
    - Keep `globals` in frontend; UI layer `on_resize` recomputes `ui_projection`/`ui_view`.
  - Frame draw:
    - Replace the world/UI blocks with:
```c
// inside vkr_renderer_draw_frame(...)
uint32_t image_index = vkr_renderer_window_image_index(renderer);
vkr_view_system_draw_all(renderer, image_index);
```

  - Resize:
    - Keep window size updates; call `vkr_view_system_on_resize(renderer, width, height)`; remove direct UI target rebuilds; UI layer recomputes orthographic projection.
  - Destroy:
    - Call `vkr_view_system_shutdown(renderer)` before subsystem shutdown.

- `lib/src/renderer/vkr_renderer.h`
  - Add the public API/types above.

- `lib/src/application.h`
  - No API change; built-in layers are registered during renderer systems initialization (not by `vkr_renderer_default_scene`).

### Default scene (cleaned)

- `vkr_renderer_default_scene` no longer registers layers or manages passes/targets.
- It may load demo content (falcon/sponza) using existing resource/mesh systems. All rendering behavior lives inside layer callbacks.

### Default layers (migrating logic out of frontend)

- World Layer
  - Pass: `Renderpass.Builtin.World` (depth-enabled, clears color+depth based on backend config).
  - `on_attach`: ensure world pipeline(s) exist via registry (using loaded shader config); allocate per-layer state if needed.
  - `on_render`: move current world block: resolve/bind pipeline, apply globals once, iterate meshes, apply materials/instances, draw geometry.
  - `on_resize`: optional; camera-driven view/proj updated in app loop; can mark global state dirty as needed.

- UI Layer
  - Pass: `Renderpass.Builtin.UI` (no depth, transparent overlay; clear settings from backend config).
  - `on_attach`: acquire per-instance state for UI pipeline; move `ui_transform` into layer context.
  - `on_render`: move current UI block: bind UI pipeline, apply UI globals, bind instance/material, draw default plane.
  - `on_resize`: recompute orthographic `ui_projection` and set `globals.ui_*`.

### Render target lifecycle (hidden)

- At init and on backend RT refresh:
  - View system queries swapchain image count and resolves actual images internally using existing frontend helpers.
  - For each layer/pass, builds per-image `VkrRenderTargetHandle` using window color and depth where appropriate. This is entirely internal and not exposed in the API.

### Step-by-step implementation

1) Add public API/types in `vkr_renderer.h` (no attachment/swapchain types).

2) Implement `vkr_view_system` with registry, per-layer contexts, target build (internal), draw, resize, shutdown.

3) Add built-in layer callbacks and `vkr_view_layers_register_builtin` by moving code from `vkr_renderer_draw_frame` world/UI sections and relevant setup from init.

4) Integrate in `renderer_frontend.c`: init (register layers in systems init), delegate resize and RT-refresh to view system, replace draw, shutdown.

5) Refactor `vkr_renderer_default_scene` to only load optional demo content; drop layer registration and pass/target handling.

6) Remove obsolete fields and functions from frontend; compile and fix references.

7) Smoke test parity: world renders, UI overlay renders, resize works.

### Risks and mitigations

- **Resource lifetime**: Ensure UI instance state and per-pass targets are released in view system shutdown before pipeline/material shutdown.
- **State duplication**: Only `globals` live in frontend; layer-local state in contexts.
- **Ordering**: Numeric `order` with stable sorting ensures world→UI render sequence.

### Future-proofing for render graph

- Layers reference passes by name; attachments are internal. This can map naturally to a future render-graph where the view system schedules nodes and manages surfaces without public API changes.
