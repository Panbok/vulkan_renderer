---
status: partial
updated: 2026-07-31
authority: spec
---
# Stateless Renderer Spec (Full Rewrite)

Version: 2.0
Status: Design-only. No legacy compatibility.
Audience: Engine contributors and LLM-based implementers.

## 1) Scope
This document specifies a complete rewrite of the renderer frontend into a stateless packet consumer, integrated with the existing render graph and GPU resource caches (mesh manager, material system, geometry system, shader system, pipeline registry). The legacy view system is removed entirely. No incremental path is defined.

## 2) Goals
- Stateless frontend: the only per-frame input is an explicit render packet.
- Render graph stays: JSON graph and pass executors remain the orchestration mechanism.
- GPU resource caches stay: mesh/material/geometry/texture/pipeline systems remain but are treated as persistent caches, not per-frame state.
- No view system: all former view responsibilities are moved into packet data.
- Single entry point per frame: submit a packet and encode commands.

## 3) Non-Goals
- No legacy API support or migration layer.
- No mixed view/packet pipeline.
- No renderer state mutated by application-side globals (camera, lighting, editor state) outside the packet.

## 4) Architectural Summary

Flow (single frame):

```
app calls renderer_prepare_frame(&setup)
  -> build RenderPacket
  -> fill packet->frame.window_width/height from setup
  -> renderer_submit_packet(packet)
    -> validate + resolve handles
    -> apply text updates
    -> attach packet to render graph via vkr_rg_set_packet()
    -> render graph executes passes; executors access packet via context
    -> backend records and submits commands
```

The renderer frontend does not pull per-frame state from global systems. It consumes the packet and is otherwise a pure encoder. Persistent GPU state lives in the backend and cache systems.

**Key Integration Point**: The packet is attached to the existing `VkrRenderGraph` before execution. Pass executors access packet data through `vkr_rg_pass_get_payload()` helpers, not through a parallel execution system.

## 5) Ownership and State Model

### 5.1 Persistent state (renderer-owned)
- Backend state: Vulkan device, swapchain, queues, descriptor pools, readback ring.
- Resource caches: pipeline registry, shader system, texture system, material system, geometry system, mesh manager.
- Render graph: JSON + executor registry.
- CPU-side caches and allocators required for resource lifetime management.
- **Shadow system** (`VkrShadowSystem`): Owns shadow map textures, per-cascade render targets, shadow pipelines, and renderpass. Updated via `vkr_shadow_system_update()` before packet submission.
- **Picking system** (`VkrPickingContext`): Owns picking render target, depth attachment, pipelines, and async readback state machine. Request-driven, not per-frame.
- **Text management**: 3D/UI text slot tracking, dynamic geometry updates, picking ID encoding. Text resources are persistent; packet provides content updates.
- **Font system** (`VkrFontSystem`): Glyph atlases, font metadata caches.

### 5.2 Per-frame state (app-owned)
- RenderPacket and all data it references.
- Per-frame arrays (draw lists, light lists, instance data, ui commands).
- Any dynamic uniform data that is not cached as a persistent resource.

### 5.3 Transient state (renderer-owned, frame-local)
- Temporary scratch allocations during packet validation and encoding.
- Backend command buffers and per-frame ring buffers.

### 5.4 Lifetimes
- RenderPacket memory must remain valid until submit returns (synchronous encode).
- Persistent resource handles remain valid until explicitly destroyed.

## 6) Public API (New Frontend)

The new frontend is a packet consumer. The old begin_frame/draw_frame/end_frame model is removed.

```c
// Phase 1: Acquire swapchain, prepare internal state
VkrRendererError vkr_renderer_prepare_frame(
    VkrRendererFrontendHandle renderer,
    VkrFrameSetup *out_setup);

// Phase 2: Submit draws (stateless)
VkrRendererError vkr_renderer_submit_packet(
    VkrRendererFrontendHandle renderer,
    const VkrRenderPacket *packet,
    VkrRendererFrameMetrics *out_metrics,
    VkrValidationError *out_validation_error);  // Optional, NULL to skip details

// Resource management remains similar to current API.
VkrBufferHandle vkr_renderer_create_buffer(...);
VkrTextureHandle vkr_renderer_create_texture(...);
VkrPipelineOpaqueHandle vkr_renderer_create_graphics_pipeline(...);

// Optional: asynchronous readback polling and device idle.
VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer);
VkrRendererError vkr_renderer_get_pixel_readback_result(
    VkrRendererFrontendHandle renderer,
    VkrPixelReadbackResult *out_result);
```

Notes:
- `vkr_renderer_prepare_frame` acquires the swapchain image and returns frame setup info (image index, swapchain dimensions, formats).
- `vkr_renderer_prepare_frame` must be called once per frame before `vkr_renderer_submit_packet`. It returns swapchain-dependent values that the renderer owns (image index, formats, dimensions).
- `vkr_renderer_submit_packet` performs validation, encoding, and present.
- `out_metrics` is optional. It is filled after encode.
- `out_validation_error` is optional. When provided, validation failures populate `field_path` and `message`.
- Resize handling is explicit via packet `frame.window_width/height` or via a dedicated resize event/command in the packet.

## 7) RenderPacket Schema

### 7.1 High-Level Layout
The packet is a single struct with explicit pointers to per-pass payloads and global frame data. **Pass participation is determined by non-NULL payload pointers** - no separate pass list is needed.

```c
typedef struct VkrRenderPacket {
  uint32_t packet_version;

  // Frame-level data
  VkrFrameInfo frame;
  VkrFrameGlobals globals;

  // Pass payloads - NULL means pass is disabled for this frame
  const VkrWorldPassPayload *world;
  const VkrShadowPassPayload *shadow;
  const VkrSkyboxPassPayload *skybox;
  const VkrUiPassPayload *ui;
  const VkrEditorPassPayload *editor;
  const VkrPickingPassPayload *picking;
  const VkrTextUpdatesPayload *text_updates;

  // Optional GPU debug/telemetry requests
  const VkrGpuDebugPayload *debug;
} VkrRenderPacket;
```

**Rationale**: Removing the `VkrPassRef` array simplifies the API. Pass enablement is implicit: `if (packet->world == NULL) return;` in executors. Some passes may add extra gating (e.g. picking skips when `pending == false`).

### 7.2 Frame Info
```c
typedef struct VkrFrameInfo {
  uint32_t frame_index;        // monotonic frame counter
  float64_t delta_time;
  uint32_t window_width;
  uint32_t window_height;
  uint32_t viewport_width;
  uint32_t viewport_height;
  bool8_t editor_enabled;
} VkrFrameInfo;
```

**Notes**:
- `window_width/height` should come from `VkrFrameSetup` (swapchain dimensions).
- `viewport_width/height == 0` means "use window dimensions from `window_width/height`".
- Swapchain image index and formats are owned by the renderer and provided by `vkr_renderer_prepare_frame`.
- `frame_index` is for app-level bookkeeping only; the renderer uses its own frame-in-flight index for buffering.

### 7.3 Global Frame Uniforms
```c
typedef struct VkrFrameGlobals {
  Mat4 view;
  Mat4 projection;
  Vec3 view_position;
  Vec4 ambient_color;
  uint32_t render_mode;
} VkrFrameGlobals;
```

### 7.4 Instance Data GPU Format

Instance data must match the existing `VkrInstanceDataGPU` layout for shader compatibility:

```c
typedef struct VkrInstanceDataGPU {
  Mat4 model;              // 64 bytes
  uint32_t object_id;      // picking ID (0 = background)
  uint32_t material_index; // material table index
  uint32_t flags;          // per-instance flags
  uint32_t _padding;       // std430 alignment
} VkrInstanceDataGPU;      // Total: 80 bytes, 16-byte aligned

_Static_assert(sizeof(VkrInstanceDataGPU) == 80, "Must be 80 bytes");
_Static_assert(sizeof(VkrInstanceDataGPU) % 16 == 0, "Must be 16-byte aligned");
```

All pass payloads reference instance data as typed arrays of `VkrInstanceDataGPU`, not `void*` with byte offsets. Use the existing definition in `vkr_instance_buffer.h` (do not create a duplicate type).

### 7.5 Draw Items
All passes that draw geometry use a shared draw item schema referencing cache handles. **Domain is not included** - the pass executor determines domain from its context.

```c
typedef struct VkrDrawItem {
  VkrMeshHandle mesh;            // mesh manager handle
  uint32_t submesh_index;        // which submesh
  VkrMaterialHandle material;    // material system handle

  // Instance data - typed array reference
  uint32_t instance_count;
  uint32_t first_instance;       // index into payload's instance array

  // Sorting keys (app computed)
  uint64_t sort_key;

  // Optional overrides
  VkrPipelineHandle pipeline_override;
} VkrDrawItem;
```

**Change from v1.0**: Removed `domain` field. The pass executor knows its domain from `VkrRgPassContext`. Replaced `instance_data_offset` (byte offset) with `first_instance` (array index) for type safety.

**Notes**:
- Draw items with `instance_count == 0` are skipped by the executor.
- Handles must be valid (no "invalid" sentinel) unless the pass explicitly documents optional fields.

### 7.6 World Pass Payload
```c
typedef struct VkrWorldPassPayload {
  const VkrDrawItem *opaque_draws;
  uint32_t opaque_draw_count;

  const VkrDrawItem *transparent_draws;
  uint32_t transparent_draw_count;

  // Typed instance data array
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrWorldPassPayload;
```

### 7.7 Shadow Pass Payload

The shadow pass payload provides cascade matrices, draw lists, and optional config overrides. The renderer's `VkrShadowSystem` owns shadow map resources and produces `VkrShadowFrameData` for shader binding.

```c
typedef struct VkrShadowPassPayload {
  // Cascade matrices computed by app from VkrShadowSystem state
  uint32_t cascade_count;
  Mat4 light_view_proj[VKR_SHADOW_CASCADE_COUNT_MAX];
  float32_t split_depths[VKR_SHADOW_CASCADE_COUNT_MAX];

  // Draw lists - shared with world pass or culled separately
  const VkrDrawItem *opaque_draws;
  uint32_t opaque_draw_count;
  const VkrDrawItem *alpha_draws;
  uint32_t alpha_draw_count;

  // Typed instance data
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;

  // Optional config overrides (NULL = use VkrShadowSystem defaults)
  const VkrShadowConfigOverride *config_override;
} VkrShadowPassPayload;

typedef struct VkrShadowConfigOverride {
  float32_t depth_bias_constant;
  float32_t depth_bias_slope;
  float32_t depth_bias_clamp;
} VkrShadowConfigOverride;
```

**Cross-Pass Data Flow**: The world pass executor calls `vkr_shadow_system_get_frame_data()` to obtain `VkrShadowFrameData` for shader binding. This includes:
- Shadow map texture handle
- Per-cascade bias parameters, UV margins, PCF radius
- Debug visualization flags

The packet does not duplicate this data. The shadow system remains renderer-internal.

### 7.8 UI Pass Payload
```c
typedef struct VkrUiPassPayload {
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrUiPassPayload;
```

### 7.9 Skybox Pass Payload
```c
typedef struct VkrSkyboxPassPayload {
  VkrTextureHandle cubemap;
  VkrMaterialHandle material;
} VkrSkyboxPassPayload;
```

### 7.10 Editor Pass Payload
```c
typedef struct VkrEditorPassPayload {
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrEditorPassPayload;
```

Editor viewport dimensions come from `packet->frame.viewport_width/height`.

### 7.11 Picking Pass Payload

Picking is **request-based**, not per-frame. The picking pass only renders when a pick is pending. Draw lists are reused from the world pass.

```c
typedef struct VkrPickingPassPayload {
  // Pick request (only one at a time)
  bool8_t pending;
  uint32_t x;           // render-target pixel coordinate
  uint32_t y;

  // Draw lists - typically same as world pass
  // If NULL, reuse world pass draws automatically
  const VkrDrawItem *draws;
  uint32_t draw_count;
  const VkrInstanceDataGPU *instances;
  uint32_t instance_count;
} VkrPickingPassPayload;
```

**Change from v1.0**:
- Removed `VkrPickingRequest` array - only one pick at a time is supported by the async readback system.
- Added `pending` flag - if false, picking pass is skipped entirely.
- Made draw lists optional - NULL means reuse world draws.

**Notes**:
- Coordinates are in render-target pixel space with origin at top-left.
- When reusing world draws, the picking pass must reuse the same instance array indexing.

### 7.12 Text Updates (NEW)

Text is managed as persistent renderer resources. The packet provides content updates, not full text definitions.

```c
typedef struct VkrTextUpdate {
  uint32_t text_id;          // persistent text slot ID
  String8 content;           // new content (NULL = no change)
  const VkrTransform *transform;  // new transform (NULL = no change)
} VkrTextUpdate;

typedef struct VkrTextUpdatesPayload {
  const VkrTextUpdate *world_text_updates;
  uint32_t world_text_update_count;

  const VkrTextUpdate *ui_text_updates;
  uint32_t ui_text_update_count;
} VkrTextUpdatesPayload;
```

Text creation/destruction uses persistent API calls, not packet data:
```c
uint32_t vkr_renderer_create_world_text(VkrRendererFrontendHandle rf, const VkrText3DConfig *config);
void vkr_renderer_destroy_world_text(VkrRendererFrontendHandle rf, uint32_t text_id);
```

**Notes**:
- `String8` and `transform` pointers must remain valid until `vkr_renderer_submit_packet` returns.
- Text updates are applied during submit before render graph execution; they do not correspond to a JSON render graph pass.

### 7.13 GPU Debug Payload (Optional)
```c
typedef struct VkrGpuDebugPayload {
  bool8_t enable_timing;
  bool8_t capture_pass_timestamps;
} VkrGpuDebugPayload;
```

GPU debug requests are best-effort; the backend may ignore them if debug tooling is disabled or unavailable.

## 8) Render Graph Integration

**Critical**: The packet integrates with the existing `vkr_render_graph.h` infrastructure. This spec does **not** define a parallel execution system.

### 8.1 Attaching Packet to Graph

```c
// Attach packet to render graph for the current frame
void vkr_rg_set_packet(VkrRenderGraph *graph, const VkrRenderPacket *packet);

// Pass executors access packet via context helper
const VkrWorldPassPayload *vkr_rg_pass_get_world_payload(const VkrRgPassContext *ctx);
const VkrShadowPassPayload *vkr_rg_pass_get_shadow_payload(const VkrRgPassContext *ctx);
// ... etc for other pass types
```

`vkr_rg_set_packet` stores a pointer only for the duration of the current `vkr_rg_execute` call. It must not retain packet memory across frames.

### 8.2 Executor Contract

Render graph pass executors receive `VkrRgPassContext` (existing type) and access packet payloads through helpers:

```c
void pass_world_execute(VkrRgPassContext *ctx, void *user_data) {
  const VkrWorldPassPayload *payload = vkr_rg_pass_get_world_payload(ctx);
  if (!payload) return;  // Pass disabled this frame

  // Access renderer systems via ctx->renderer
  // Access render target via ctx->render_target
  // Encode draws using payload data
}
```

Picking executors must also check `payload->pending` before issuing any draw commands.

### 8.3 Mapping JSON Passes to Payloads

The JSON render graph (`main.rendergraph.json`) defines pass **execution order and resource dependencies**. The packet provides **per-frame data** for each pass.

| JSON `execute` name | Packet payload field |
|---------------------|---------------------|
| `pass.world` | `packet->world` |
| `pass.shadow.cascade` | `packet->shadow` |
| `pass.skybox` | `packet->skybox` |
| `pass.ui` | `packet->ui` |
| `pass.editor` | `packet->editor` |
| `pass.picking` | `packet->picking` |

If a payload is NULL, the executor early-returns. The render graph handles resource transitions regardless.

Text updates (`packet->text_updates`) are applied before graph execution and do not map to a JSON pass.

### 8.4 Pass Inputs

Executors resolve resources through persistent caches:
- Mesh data: mesh manager
- Material/shader: material + shader systems
- Pipeline: pipeline registry
- Textures: texture system
- Shadow data: `vkr_shadow_system_get_frame_data()`

No pass should reference view state or application global state outside the packet.

## 9) Packet Validation Rules

Validation occurs at submit time and must be deterministic and strict.

### 9.1 Validation Error Details

```c
typedef struct VkrValidationError {
  VkrRendererError code;
  const char *field_path;  // e.g., "world.opaque_draws[3].mesh"
  const char *message;     // human-readable description
} VkrValidationError;

VkrRendererError vkr_renderer_submit_packet(
    VkrRendererFrontendHandle renderer,
    const VkrRenderPacket *packet,
    VkrRendererFrameMetrics *out_metrics,
    VkrValidationError *out_validation_error);  // Optional, NULL to skip details
```

### 9.2 Mandatory Checks
- `packet_version` matches renderer supported version.
- `packet` pointer is non-NULL.
- `frame.window_width/height` are non-zero.
- All non-NULL payloads have valid internal state:
  - Draw items reference valid handles (mesh, material, pipeline override).
  - `first_instance + instance_count <= payload->instance_count` for all draws.
  - Instance array pointer is non-NULL if instance_count > 0.
- Shadow cascade count in range `[1, VKR_SHADOW_CASCADE_COUNT_MAX]` if shadow payload present.
- Picking coordinates within render target bounds if picking pending.
- If `picking->draws == NULL` then `packet->world` must be non-NULL and `picking->instances` must either be NULL (reuse world instances) or point to the same instance array.
- Text updates reference valid `text_id` values; `String8` data must be non-NULL when `content.len > 0`.

## 10) Command Encoding Rules

Encoding is deterministic and does not modify the packet.

High-level encoder steps:
1) `vkr_renderer_prepare_frame` - acquire swapchain (caller responsibility)
2) Validate packet and apply text updates to text system
3) `vkr_rg_set_packet(graph, packet)` - attach packet to graph
4) Upload instance data to backend-visible buffers (instance buffer pool)
5) `vkr_rg_execute(graph, renderer)` - render graph executes passes
   - Each pass executor accesses payload via `vkr_rg_pass_get_*_payload()`
   - Bind pipeline (from pipeline registry or override)
   - Bind resources from caches
   - Emit draws in packet order or sorted by `sort_key`
6) Backend end_frame (present)

The renderer must not retain any packet pointers after submit returns.
On validation failure, no draw commands are recorded; the renderer may still perform minimal work to release the acquired swapchain image.

Sorting:
- The packet supplies pre-sorted draw lists.
- Renderer may stable-sort by `sort_key` if configured, but the default is no reordering.

## 11) Per-Frame Dynamic Buffers
- Instance data is packed by the app into typed `VkrInstanceDataGPU` arrays.
- Renderer uploads into backend-visible buffers (instance buffer pool).
- Pass executors use `first_instance` to index the instance buffer.
- When multiple passes share the same instance array pointer and count, the renderer may upload once and reuse the allocation.

## 12) Metrics and Telemetry
`VkrRendererFrameMetrics` is output from submit and contains:
- world: draws, batches, indirect draws
- shadow: draws/batches per cascade
- additional counters can be added but are never pulled from hidden state

## 13) Threading and Synchronization
- Packet construction can occur on any thread; renderer submit must occur on the render thread.
- Resource caches must be thread-safe for read access during submit. Updates (resource creation) are serialized.
- No per-frame data is stored in renderer state except backend frame resources.

## 14) Error Handling
`vkr_renderer_submit_packet` returns:
- `VKR_RENDERER_ERROR_NONE` on success
- `VKR_RENDERER_ERROR_INVALID_PARAMETER` for validation failures (check `out_validation_error`)
- `VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED` for backend errors
- `VKR_RENDERER_ERROR_DEVICE_ERROR` for device loss

`vkr_renderer_prepare_frame` may fail if the swapchain is out of date or the surface is lost; callers must handle this by recreating swapchain resources before the next submit.

## 15) File Layout (Proposed)

New/rewritten files:
- `lib/src/renderer/renderer_frontend.h`
- `lib/src/renderer/renderer_frontend.c`
- `lib/src/renderer/vkr_render_packet.h`
- `lib/src/renderer/vkr_render_packet.c` (builder helpers)
- `lib/src/renderer/passes/*` (updated executor signatures)

Removed:
- `lib/src/renderer/systems/views/*`
- view registration and update logic in application layer

## 16) Application Responsibilities (New)
- Build the `VkrRenderPacket` each frame.
- Call `vkr_renderer_prepare_frame` and copy swapchain dimensions from `VkrFrameSetup` into `packet->frame.window_width/height`.
- **Camera/Input**: Process raw input (WASD, mouse, gamepad) and produce view/projection matrices. The renderer does not handle input.
- **Shadow Cascade Computation**: Call `vkr_shadow_system_update()` with camera and light direction, then copy cascade matrices to packet.
- Compute editor viewport mapping if editor is enabled and set `frame.viewport_width/height`.
- Produce draw lists for world/ui/shadow/editor with explicit sort keys.
- **Text Management**: Create/destroy text via persistent API. Provide content updates in packet.
- **Picking**: Set `picking->pending = true` and coordinates when user clicks. Poll `vkr_renderer_get_pixel_readback_result()` on subsequent frames.
- Submit the packet and consume metrics.

## 17) Constraints for LLM Implementers
- Do not reference or call any legacy view API.
- Do not use global renderer state as implicit per-frame data.
- All per-frame data must flow through `VkrRenderPacket`.
- Render graph and cache systems remain; only their inputs change.
- Access packet data through `vkr_rg_pass_get_*_payload()` helpers, not direct struct access in executors.

## 18) Packet Builder API

Optional builder pattern reduces boilerplate and validates during construction:

```c
VkrRenderPacketBuilder builder;
vkr_packet_builder_init(&builder, allocator);

vkr_packet_builder_set_frame_info(&builder, &frame_info);
vkr_packet_builder_set_globals(&builder, &globals);

// Text updates (optional)
vkr_packet_builder_set_text_updates(&builder, &text_updates);

// World pass
vkr_packet_builder_begin_world(&builder);
vkr_packet_builder_add_opaque_draws(&builder, draws, count);
vkr_packet_builder_set_instances(&builder, instances, instance_count);
vkr_packet_builder_end_world(&builder);

// Shadow pass (optional)
if (shadows_enabled) {
  vkr_packet_builder_begin_shadow(&builder);
  vkr_packet_builder_set_cascades(&builder, matrices, split_depths, cascade_count);
  vkr_packet_builder_set_shadow_draws(&builder, opaque, opaque_count, alpha, alpha_count);
  vkr_packet_builder_end_shadow(&builder);
}

VkrRenderPacket packet;
VkrValidationError err;
if (!vkr_packet_builder_finish(&builder, &packet, &err)) {
  // Handle validation error during build
}
```

Builder helpers are convenience-only and must not copy or retain packet memory beyond the call to `vkr_packet_builder_finish`.

## 19) Minimal Example Flow (Pseudo)

```c
// Prepare frame (acquire swapchain)
VkrFrameSetup setup;
VkrRendererError prep_err = vkr_renderer_prepare_frame(renderer, &setup);
if (prep_err != VKR_RENDERER_ERROR_NONE) {
  // handle swapchain out-of-date / surface lost
}

// Update shadow system
vkr_shadow_system_update(&shadow_system, camera, light_enabled, light_dir);

// Build packet
VkrRenderPacket packet = {
  .packet_version = VKR_RENDER_PACKET_VERSION,
  .frame = {
    .frame_index = frame_number,
    .delta_time = delta_time,
    .window_width = setup.window_width,
    .window_height = setup.window_height,
    // ...
  },
  .globals = {
    .view = camera_view,
    .projection = camera_projection,
    .view_position = camera_position,
    // ...
  },
  .world = &world_payload,
  .shadow = shadows_enabled ? &shadow_payload : NULL,
  .skybox = &skybox_payload,
  .ui = &ui_payload,
  .picking = picking_pending ? &picking_payload : NULL,
};

VkrRendererFrameMetrics metrics = {0};
VkrValidationError validation_error = {0};
VkrRendererError err = vkr_renderer_submit_packet(renderer, &packet, &metrics, &validation_error);
```

## 20) Hard Cutover Checklist
- Delete view system and all registrations.
- Remove all view update calls from application loop.
- Replace `begin_frame/draw_frame/end_frame` with `prepare_frame/submit_packet`.
- Add `vkr_rg_set_packet()` and payload accessor functions to render graph.
- Update render graph pass executors to access packet via context helpers.
- Ensure all per-frame state (camera, lights, transforms) is in the packet.
- Move camera/input handling to application layer.
- Convert text management to persistent API + packet updates.

---

## Appendix A: Changes from v1.0

| Section | Change | Rationale |
|---------|--------|-----------|
| 5.1 | Added shadow, picking, text systems to persistent state | These systems own GPU resources and have lifecycle beyond per-frame |
| 6 | Added `vkr_renderer_prepare_frame` | Split frame lifecycle for cleaner swapchain acquisition |
| 7.1 | Removed `passes` array and `pass_count` | Null-payload check is simpler than explicit pass list |
| 7.2 | Removed swapchain image index/formats from frame info | Renderer owns swapchain state; avoids mismatch |
| 7.4 | Added `VkrInstanceDataGPU` format | Typed instance data prevents format mismatches |
| 7.5 | Removed `domain` from `VkrDrawItem` | Redundant - executor knows its domain from context |
| 7.5 | Changed `instance_data_offset` to `first_instance` | Type-safe array indexing vs byte offsets |
| 7.7 | Simplified shadow payload, added config override | Shadow system owns resources; packet provides matrices + draw lists |
| 7.11 | Made picking request-based with single pending pick | Matches async readback limitation; reuses world draws |
| 7.10 | Removed editor target size from payload | Uses `frame.viewport_width/height` instead of duplicate fields |
| 7.12 | Added text update payload | Text is persistent; packet provides updates not definitions |
| 8 | Clarified render graph integration | Spec extends existing vkr_render_graph.h, not parallel system |
| 9.1 | Added `VkrValidationError` with field path | Better debugging for validation failures |
| 16 | Expanded app responsibilities | Camera input, shadow update, text management moved to app |
| 18 | Added packet builder API | Reduces boilerplate and validates during construction |

## Appendix B: Critical Files Reference

| File | Relevance |
|------|-----------|
| `lib/src/renderer/vkr_render_graph.h` | Existing pass context and executor registry to extend |
| `lib/src/renderer/systems/vkr_shadow_system.h` | Shadow resource ownership and frame data structure |
| `lib/src/renderer/renderer_frontend.h` | Current frontend state model |
| `lib/src/renderer/passes/vkr_pass_world.c` | Draw batching, text, camera handling patterns |
| `lib/src/renderer/vkr_instance_buffer.h` | Instance data GPU format definition |
| `assets/render_graphs/main.rendergraph.json` | Current pass structure and dependencies |
