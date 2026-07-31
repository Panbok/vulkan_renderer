---
status: partial
updated: 2026-07-31
authority: design
---
# Editor Viewport, Off-Screen Rendering, and Pixel-Perfect 3D Object Picking

**Note:** The view/layer system has been removed. Render orchestration now uses
the render graph; editor viewport layout is computed via
`vkr_editor_viewport_compute_mapping()` and packet payloads. The viewport and
picking path ships, but the deferred editor-integration example still uses
pre-render-graph APIs and is not current implementation guidance.

## Document Purpose

This design document outlines the implementation plan for:
1. **Off-screen render targets** - Render to textures instead of swapchain
2. **Dynamic viewport/scissor commands** - Runtime viewport control within render passes
3. **Render-to-texture sampling** - Use rendered scene as texture in UI
4. **Pixel-perfect 3D object selection** - Color-based object picking

This enables an editor-style layout with UI panels surrounding a resizable 3D viewport, plus precise object selection for scene editing.

---

## Table of Contents

1. [Current Architecture Analysis](#1-current-architecture-analysis)
2. [Off-Screen Render Targets](#2-off-screen-render-targets)
3. [Dynamic Viewport Commands](#3-dynamic-viewport-commands)
4. [Render-to-Texture Sampling](#4-render-to-texture-sampling)
5. [Pixel-Perfect Object Picking](#5-pixel-perfect-object-picking)
6. [Implementation Phases](#6-implementation-phases)
6.1 [Current Implementation Status](#61-current-implementation-status)
7. [API Reference](#7-api-reference)
8. [File Changes Summary](#8-file-changes-summary)

---

## 1. Current Architecture Analysis

### 1.1 What Already Exists

#### Render Target Infrastructure
```c
// vkr_renderer.h - Already supports custom dimensions and attachments
typedef struct VkrRenderTargetDesc {
  bool8_t sync_to_window_size;
  uint8_t attachment_count;
  VkrTextureOpaqueHandle *attachments;
  uint32_t width;
  uint32_t height;
} VkrRenderTargetDesc;
```

#### Render Graph with Per-Pass Render Targets
```c
// vkr_render_graph.h - Per-pass attachments drive render targets
VkrRgAttachmentDesc color_desc = {
  .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
  .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
};

VkrRgPassBuilder pass = vkr_rg_add_pass(graph, VKR_RG_PASS_TYPE_GRAPHICS,
                                        str8_lit("pass.editor"));
vkr_rg_pass_add_color_attachment(&pass, scene_color, &color_desc);
vkr_rg_pass_set_depth_attachment(&pass, scene_depth, &depth_desc, false);
```

#### Render Pass Configuration with Render Area
```c
// vkr_renderer.h - render_area already supports custom viewport regions
typedef struct VkrRenderPassConfig {
  Vec4 render_area;    // x, y, width, height
  Vec4 clear_color;
  // ...
} VkrRenderPassConfig;
```

#### Viewport/Scissor Set During Render Pass Begin
```c
// vulkan_backend.c:2841-2851 - Sets viewport/scissor from render_area
VkViewport viewport = {
    .x = (float32_t)render_area.offset.x,
    .y = (float32_t)render_area.offset.y,
    .width = (float32_t)render_area.extent.width,
    .height = (float32_t)render_area.extent.height,
    // ...
};
vkCmdSetViewport(command_buffer->handle, 0, 1, &viewport);
vkCmdSetScissor(command_buffer->handle, 0, 1, &render_area);
```

#### Buffer Memory Mapping (for readback)
```c
// vulkan_buffer.c - Lock/unlock memory for CPU access
void *vulkan_buffer_lock_memory(VulkanBackendState *state, VulkanBuffer *buffer,
                                uint64_t offset, uint64_t size, uint32_t flags);
bool8_t vulkan_buffer_unlock_memory(VulkanBackendState *state, VulkanBuffer *buffer);
```

#### Object Identification via Mesh/SubMesh System
```c
// vkr_resources.h - Each mesh has unique index in mesh manager
typedef struct VkrMesh {
  VkrTransform transform;
  Mat4 model;
  Array_VkrSubMesh submeshes;
} VkrMesh;

// Mesh manager provides uint32_t index for each mesh
VkrMesh *vkr_mesh_manager_get(VkrMeshManager *manager, uint32_t index);
```

#### Push Constants for Per-Object Data
```c
// default.world.shadercfg - Local (push constant) scope for model matrix
uniform=mat4,2,model  // Per-object transform
```

### 1.2 Current Gaps

| Feature | Status | Gap |
|---------|--------|-----|
| Off-screen color attachment | 🟡 Partial | Need `VK_IMAGE_USAGE_SAMPLED_BIT` on color attachments |
| Dynamic viewport commands | ❌ Missing | Not exposed in frontend API |
| Image-to-buffer copy | ❌ Missing | No `vkCmdCopyImageToBuffer` wrapper |
| Picking shader | ❌ Missing | Need shader that outputs object ID as color |
| Picking render pass | ❌ Missing | Need separate pass with R32_UINT format |
| Object ID in shader | ❌ Missing | Need to pass mesh index via push constants |

---

## 2. Off-Screen Render Targets

### 2.1 Overview

Off-screen render targets allow rendering the 3D scene to a texture that can later be displayed in a UI panel (e.g., ImGui viewport widget).

### 2.2 Required Texture Usage Flags

For a texture to serve as both a render target and a sampled texture:

```c
VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT  // Render target
                        | VK_IMAGE_USAGE_SAMPLED_BIT           // Shader sampling
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;     // Optional: for screenshots
```

### 2.3 New API: Create Render Target Texture

```c
// vkr_renderer.h - New function
typedef struct VkrRenderTargetTextureDesc {
  uint32_t width;
  uint32_t height;
  VkrTextureFormat format;          // VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB for color
  bool8_t use_as_sampled;           // Add SAMPLED_BIT for shader access
  bool8_t use_as_transfer_src;      // Add TRANSFER_SRC_BIT for readback
} VkrRenderTargetTextureDesc;

VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture(
    VkrRendererFrontendHandle renderer,
    const VkrRenderTargetTextureDesc *desc,
    VkrRendererError *out_error);

// Depth attachment variant
VkrTextureOpaqueHandle vkr_renderer_create_depth_attachment(
    VkrRendererFrontendHandle renderer,
    uint32_t width,
    uint32_t height,
    VkrRendererError *out_error);
```

### 2.4 Implementation Details

**File: `vulkan_backend.c`**

```c
VkrBackendResourceHandle renderer_vulkan_create_render_target_texture(
    void *backend_state,
    const VkrRenderTargetTextureDesc *desc) {

  VulkanBackendState *state = (VulkanBackendState *)backend_state;

  // Build usage flags
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (desc->use_as_sampled) {
    usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  if (desc->use_as_transfer_src) {
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }

  // Create image with appropriate format
  VkFormat vk_format = vulkan_vkr_format_to_vk(desc->format);

  VulkanImage image;
  vulkan_image_create(state, VK_IMAGE_TYPE_2D, desc->width, desc->height,
                      vk_format, VK_IMAGE_TILING_OPTIMAL, usage,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1,
                      VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                      &image);

  // Create sampler if needed for shader access
  // ... sampler creation ...

  // Wrap in texture handle
  // ...
}
```

### 2.5 Image Layout Transitions

The texture needs proper layout transitions:

| Stage | Layout |
|-------|--------|
| Initial | `VK_IMAGE_LAYOUT_UNDEFINED` |
| As render target | `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` |
| For shader sampling | `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` |
| For pixel readback | `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` |

**Transition helper needed:**
```c
VkrRendererError vkr_renderer_transition_texture_layout(
    VkrRendererFrontendHandle renderer,
    VkrTextureOpaqueHandle texture,
    VkrTextureLayout old_layout,
    VkrTextureLayout new_layout);
```

---

## 3. Dynamic Viewport Commands

### 3.1 Overview

Allow setting viewport and scissor dynamically during rendering, enabling multiple viewports within a single render pass or fine-grained control for UI composition.

### 3.2 New API

```c
// vkr_renderer.h - New functions
typedef struct VkrViewport {
  float32_t x;
  float32_t y;
  float32_t width;
  float32_t height;
  float32_t min_depth;  // Usually 0.0f
  float32_t max_depth;  // Usually 1.0f
} VkrViewport;

typedef struct VkrScissor {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
} VkrScissor;

void vkr_renderer_set_viewport(VkrRendererFrontendHandle renderer,
                               const VkrViewport *viewport);

void vkr_renderer_set_scissor(VkrRendererFrontendHandle renderer,
                              const VkrScissor *scissor);

// Convenience: set both at once
void vkr_renderer_set_viewport_scissor(VkrRendererFrontendHandle renderer,
                                       float32_t x, float32_t y,
                                       float32_t width, float32_t height);
```

### 3.3 Implementation

**File: `vulkan_backend.c`**

```c
void renderer_vulkan_set_viewport(void *backend_state,
                                  const VkrViewport *viewport) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VulkanCommandBuffer *cmd = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  VkViewport vk_viewport = {
    .x = viewport->x,
    .y = viewport->y,
    .width = viewport->width,
    .height = viewport->height,
    .minDepth = viewport->min_depth,
    .maxDepth = viewport->max_depth,
  };

  vkCmdSetViewport(cmd->handle, 0, 1, &vk_viewport);
}

void renderer_vulkan_set_scissor(void *backend_state,
                                 const VkrScissor *scissor) {
  VulkanBackendState *state = (VulkanBackendState *)backend_state;
  VulkanCommandBuffer *cmd = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, state->image_index);

  VkRect2D vk_scissor = {
    .offset = { scissor->x, scissor->y },
    .extent = { scissor->width, scissor->height },
  };

  vkCmdSetScissor(cmd->handle, 0, 1, &vk_scissor);
}
```

### 3.4 Backend Interface Addition

```c
// VkrRendererBackendInterface addition
void (*set_viewport)(void *backend_state, const VkrViewport *viewport);
void (*set_scissor)(void *backend_state, const VkrScissor *scissor);
```

---

## 4. Render-to-Texture Sampling

### 4.1 Overview

After rendering the scene to an off-screen texture, sample it in a UI shader to display the viewport in a panel.

### 4.2 Workflow

```
1. Create off-screen color + depth attachments
2. Create render target with these attachments
3. Render 3D scene to off-screen target
4. Transition color attachment to SHADER_READ_ONLY_OPTIMAL
5. Bind color attachment as texture in UI layer
6. Render fullscreen quad with scene texture
```

### 4.3 Scene Viewport Layer Example

```c
typedef struct SceneViewportData {
  // Off-screen attachments
  VkrTextureOpaqueHandle color_attachment;
  VkrTextureOpaqueHandle depth_attachment;
  VkrRenderTargetHandle scene_render_target;
  VkrRenderPassHandle scene_pass;

  // Viewport dimensions
  uint32_t viewport_width;
  uint32_t viewport_height;

  // For displaying in UI
  VkrMaterialHandle viewport_material;
  VkrGeometryHandle fullscreen_quad;
} SceneViewportData;

// Create off-screen resources
void scene_viewport_create(SceneViewportData *data,
                           VkrRendererFrontendHandle renderer,
                           uint32_t width, uint32_t height) {
  // Create color attachment
  VkrRenderTargetTextureDesc color_desc = {
    .width = width,
    .height = height,
    .format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
    .use_as_sampled = true_v,
  };
  data->color_attachment = vkr_renderer_create_render_target_texture(
      renderer, &color_desc, NULL);

  // Create depth attachment
  data->depth_attachment = vkr_renderer_create_depth_attachment(
      renderer, width, height, NULL);

  // Create render target
  VkrTextureOpaqueHandle attachments[2] = {
    data->color_attachment,
    data->depth_attachment
  };
  VkrRenderTargetDesc rt_desc = {
    .sync_to_window_size = false_v,
    .width = width,
    .height = height,
    .attachment_count = 2,
    .attachments = attachments,
  };
  data->scene_render_target = vkr_renderer_render_target_create(
      renderer, &rt_desc, data->scene_pass);
}

// Resize handler
void scene_viewport_resize(SceneViewportData *data,
                           VkrRendererFrontendHandle renderer,
                           uint32_t new_width, uint32_t new_height) {
  // Destroy old resources
  vkr_renderer_destroy_texture(renderer, data->color_attachment);
  vkr_renderer_destroy_texture(renderer, data->depth_attachment);
  vkr_renderer_render_target_destroy(renderer, data->scene_render_target);

  // Recreate with new size
  scene_viewport_create(data, renderer, new_width, new_height);
}
```

### 4.4 UI Viewport Shader

```hlsl
// default.viewport_display.slang
[[vk::binding(0, 0)]]
ConstantBuffer<UiGlobalUBO> g_ubo;

[[vk::binding(1, 1)]]
Texture2D<float4> scene_texture;

[[vk::binding(2, 1)]]
SamplerState scene_sampler;

struct VertexInput {
  [[vk::location(0)]] float2 position : POSITION;
  [[vk::location(1)]] float2 texcoord : TEXCOORD;
};

struct VertexOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
  VertexOutput output;
  output.position = mul(g_ubo.projection, float4(input.position, 0.0, 1.0));
  output.texcoord = input.texcoord;
  return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target {
  return scene_texture.Sample(scene_sampler, input.texcoord);
}
```

---

## 5. Pixel-Perfect Object Picking

### 5.1 Overview

Color picking renders the scene with each object assigned a unique color (derived from its ID), then reads back the pixel at the cursor position to determine which object was clicked.

### 5.2 Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Picking Pipeline                             │
└─────────────────────────────────────────────────────────────────┘
                              │
         ┌────────────────────┼────────────────────┐
         ▼                    ▼                    ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  Picking Shader │  │  Picking Pass   │  │  Pixel Readback │
│  (Object ID →   │  │  (Render to     │  │  (Copy to host  │
│   Color)        │  │   R32_UINT)     │  │   buffer)       │
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

### 5.3 Object ID Encoding

Each mesh in `VkrMeshManager` has a unique `uint32_t` index. This index is passed to the picking shader via push constants.

```c
// Extended push constants for picking
struct PickingPushConstants {
  Mat4 model;       // Transform
  uint32_t object_id;  // Mesh index + 1 (0 = no object)
  uint32_t submesh_id; // SubMesh index for compound objects
  uint32_t _pad[2];    // Alignment
};
```

### 5.4 Picking Shader

**File: `assets/shaders/picking.shadercfg`**
```ini
version=1.0
name=shader.picking
renderpass=Renderpass.Picking
stages=vertex,fragment
stagefiles=assets/shaders/picking.spv
use_instance=0
use_local=1

attribute=vec3,in_position

uniform=mat4,0,projection
uniform=mat4,0,view
uniform=mat4,2,model
uniform=uint32,2,object_id
```

**File: `assets/shaders/picking.slang`**
```hlsl
struct GlobalUBO {
  column_major float4x4 projection;
  column_major float4x4 view;
};

struct PushConstants {
  column_major float4x4 model;
  uint32_t object_id;
};

[[vk::binding(0, 0)]]
ConstantBuffer<GlobalUBO> g_ubo;

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexInput {
  [[vk::location(0)]] float3 position : POSITION;
};

struct VertexOutput {
  float4 position : SV_Position;
  nointerpolation uint32_t object_id : OBJECT_ID;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
  VertexOutput output;
  float4 world_pos = mul(push.model, float4(input.position, 1.0));
  float4 view_pos = mul(g_ubo.view, world_pos);
  output.position = mul(g_ubo.projection, view_pos);
  output.object_id = push.object_id;
  return output;
}

[shader("fragment")]
uint32_t fragmentMain(VertexOutput input) : SV_Target {
  return input.object_id;
}
```

### 5.5 Picking Render Pass

```c
// Create picking pass with R32_UINT format
VkrRenderPassConfig picking_pass_cfg = {
  .name = string8_lit("Renderpass.Picking"),
  .clear_color = { 0.0f, 0.0f, 0.0f, 0.0f },  // 0 = no object
  .clear_flags = VKR_RENDERPASS_CLEAR_COLOR | VKR_RENDERPASS_CLEAR_DEPTH,
  .domain = VKR_PIPELINE_DOMAIN_WORLD,  // Or new PICKING domain
};

// Picking attachment (R32_UINT for object IDs)
VkrRenderTargetTextureDesc picking_texture_desc = {
  .width = viewport_width,
  .height = viewport_height,
  .format = VKR_TEXTURE_FORMAT_R32_UINT,  // NEW format needed
  .use_as_sampled = false_v,
  .use_as_transfer_src = true_v,  // For CPU readback
};
```

### 5.6 Pixel Readback System

```c
// New types for picking
typedef struct VkrPickingContext {
  VkrTextureOpaqueHandle picking_texture;
  VkrTextureOpaqueHandle picking_depth;
  VkrRenderTargetHandle picking_target;
  VkrRenderPassHandle picking_pass;
  VkrPipelineHandle picking_pipeline;

  // Readback buffer (HOST_VISIBLE for CPU access)
  VkrBufferHandle readback_buffer;
  uint32_t readback_width;
  uint32_t readback_height;

  // Async readback state
  bool8_t readback_pending;
  uint32_t pending_x;
  uint32_t pending_y;
} VkrPickingContext;

// New API
typedef struct VkrPickResult {
  uint32_t object_id;      // Mesh index (0 = nothing)
  uint32_t submesh_id;     // SubMesh index
  bool8_t hit;             // True if an object was hit
} VkrPickResult;

// Initialize picking system
bool8_t vkr_picking_init(VkrRendererFrontendHandle renderer,
                         VkrPickingContext *ctx,
                         uint32_t width, uint32_t height);

// Render picking pass (call after scene setup, before regular render)
void vkr_picking_render(VkrRendererFrontendHandle renderer,
                        VkrPickingContext *ctx,
                        VkrMeshManager *mesh_manager);

// Request pixel readback at coordinates
void vkr_picking_request(VkrPickingContext *ctx,
                         uint32_t x, uint32_t y);

// Get result (may return no-hit if readback not complete)
VkrPickResult vkr_picking_get_result(VkrRendererFrontendHandle renderer,
                                     VkrPickingContext *ctx);

// Shutdown
void vkr_picking_shutdown(VkrRendererFrontendHandle renderer,
                          VkrPickingContext *ctx);
```

### 5.7 Image-to-Buffer Copy

**New function in `vulkan_image.h`:**
```c
bool8_t vulkan_image_copy_to_buffer(VulkanBackendState *state,
                                    VulkanImage *image,
                                    VkBuffer buffer,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    VulkanCommandBuffer *command_buffer);
```

**Implementation:**
```c
bool8_t vulkan_image_copy_to_buffer(VulkanBackendState *state,
                                    VulkanImage *image,
                                    VkBuffer buffer,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    VulkanCommandBuffer *command_buffer) {
  VkBufferImageCopy region = {
    .bufferOffset = 0,
    .bufferRowLength = 0,    // Tightly packed
    .bufferImageHeight = 0,  // Tightly packed
    .imageSubresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
    .imageOffset = { (int32_t)x, (int32_t)y, 0 },
    .imageExtent = { width, height, 1 },
  };

  vkCmdCopyImageToBuffer(command_buffer->handle,
                         image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         buffer, 1, &region);
  return true_v;
}
```

### 5.8 Picking Workflow

```
1. User clicks in scene viewport
   │
   ▼
2. Convert click coords to picking texture coords
   (account for viewport position/size)
   │
   ▼
3. Next frame: Render picking pass
   │
   ├─► For each visible mesh:
   │     - Bind picking pipeline
   │     - Set push constants (model matrix + object_id)
   │     - Draw mesh geometry
   │
   ▼
4. Copy single pixel to readback buffer
   │
   ├─► Transition picking texture to TRANSFER_SRC
   ├─► vkCmdCopyImageToBuffer (single pixel)
   ├─► Pipeline barrier (ensure transfer complete)
   │
   ▼
5. Map readback buffer and read object_id
   │
   ├─► vkMapMemory
   ├─► Read uint32_t value
   ├─► vkUnmapMemory
   │
   ▼
6. Lookup mesh by object_id
   │
   └─► vkr_mesh_manager_get(manager, object_id - 1)
```

### 5.9 Performance Considerations

| Technique | Pros | Cons |
|-----------|------|------|
| **Single-pixel readback** | Minimal bandwidth | 1-2 frame latency |
| **Full texture readback** | No latency | High bandwidth, CPU copy |
| **Async compute** | No render stall | Complex synchronization |

**Recommendation:** Single-pixel async readback with 1-frame delay is sufficient for editor use.

---

## 6. Implementation Phases

### Phase 1: Off-Screen Render Target Textures (2-3 days)

**Priority: HIGH**

1. Add `VKR_TEXTURE_FORMAT_R32_UINT` to format enum
2. Add texture usage flags support (`VkrTextureUsageFlags`)
3. Implement `vkr_renderer_create_render_target_texture()`
4. Implement `vkr_renderer_create_depth_attachment()`
5. Add image layout transition helper
6. Test: Create off-screen target, render cube, verify texture

**Files to modify:**
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/vulkan/vulkan_utils.c` (format conversion)

### Phase 2: Dynamic Viewport Commands (1 day)

**Priority: MEDIUM**

1. Add `VkrViewport` and `VkrScissor` types
2. Add `set_viewport` and `set_scissor` to backend interface
3. Implement Vulkan backend functions
4. Add frontend wrapper functions
5. Test: Multiple viewports in single frame

**Files to modify:**
- `lib/src/renderer/vkr_renderer.h`
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_backend.h`
- `lib/src/renderer/renderer_frontend.c`

### Phase 3: Render-to-Texture Sampling (2 days)

**Priority: HIGH**

1. Add `vkr_renderer_transition_texture_layout()` function
2. Ensure texture layout tracking in backend state
3. Create viewport display shader (`default.viewport_display.slang`)
4. Create scene viewport layer example
5. Test: Render scene to texture, display in UI quad

**Files to modify:**
- `lib/src/renderer/vulkan/vulkan_backend.c`
- `lib/src/renderer/vulkan/vulkan_image.c`
- `assets/shaders/default.viewport_display.shadercfg` (new)
- `assets/shaders/default.viewport_display.slang` (new)

### Phase 4: Viewport Rendering Hardening & Future-Proofing ✅ COMPLETE (1-3 days)

**Priority: HIGH**

This phase compiles the known issues/improvements discovered after Phases 1–3 were implemented.
The focus is **viewport rendering correctness and picking-ready coordinate mapping** (editor docking/layout, input focus routing, and overlays are intentionally deferred).

#### 4.1 What was implemented

1. Dynamic viewport/scissor precondition (verified)
   - Vulkan pipelines are created with `VK_DYNAMIC_STATE_VIEWPORT` and `VK_DYNAMIC_STATE_SCISSOR` enabled by default in `lib/src/renderer/vulkan/vulkan_pipeline.c`.
2. Coordinate conventions
   - Standardized on **window pixel coordinates**, **origin top-left**, **Y increases downward** (matches window input).
3. Viewport mapping + letterboxing support
   - Added `VkrViewportMapping` which tracks:
     - `panel_rect_px` (full viewport panel)
     - `image_rect_px` (actual rendered image rect when aspect is preserved)
     - `target_width/target_height` (render-target resolution)
   - Added `VkrViewportFitMode`:
     - `VKR_VIEWPORT_FIT_STRETCH` (default)
     - `VKR_VIEWPORT_FIT_CONTAIN` (letterbox/pillarbox)
   - Editor viewport quad now uses `image_rect_px` for its transform, so the displayed image matches mapping.
4. Render scale (for future picking + perf knobs)
   - Added `render_scale` (clamped) to render the offscreen target at panel resolution * scale.
5. Resize hardening / redundant work reduction
   - Editor layer caches the last offscreen size it notified to avoid spamming World with identical sizes.
   - World ignores window-resize-driven offscreen resizes when an explicit editor-driven offscreen size is set.
   - World ignores redundant `SET_OFFSCREEN_SIZE` messages when size didn’t change.
   - UI offscreen switching early-outs when the same offscreen configuration is re-applied (avoids repeated wait-idle and framebuffer rebuilds).

#### 4.2 Deferred (intentionally out of scope for current viewport+picking focus)

- Render-target pooling (reuse images by size/format) to eliminate allocation spikes while resizing.
- MSAA resolve path (MSAA render → resolve image for UI sampling).
- HDR/linear scene targets + tone-map pass.
- Per-subresource layout tracking / render-graph style dependency management.

**Files to modify:**
 - `lib/src/renderer/passes/vkr_pass_editor.c`
 - `lib/src/renderer/systems/vkr_editor_viewport.h`
 - `lib/src/renderer/passes/vkr_pass_world.c`
 - `lib/src/renderer/passes/vkr_pass_ui.c`

### Phase 5: Image-to-Buffer Copy & Pixel Readback API ✅ COMPLETE (1-2 days)

**Priority: HIGH (prerequisite for picking)**

1. Add/verify `vulkan_image_copy_to_buffer()` supports:
   - Integer formats (e.g. `R32_UINT`) and non-color formats if needed later.
   - Correct pipeline barriers for TRANSFER → HOST reads.
2. Add/verify frontend wrapper `vkr_renderer_read_texture_pixel()` behavior:
   - No render-thread stall (avoid `vkQueueWaitIdle`).
   - Explicit 1-frame (or N-frame) latency contract.
3. Implement robust async readback plumbing
   - Use a small ring of readback buffers + fences (or timeline semaphores) to avoid reusing an in-flight buffer.
   - Handle non-coherent memory (`vkInvalidateMappedMemoryRanges`) if not HOST_COHERENT.
4. Test cases
   - Copy single pixel from `R32_UINT` picking texture.
   - Copy a small region (e.g. 8×8) for debugging/visualization.

**Files modified:**
 - `lib/src/renderer/vulkan/vulkan_image.h`
 - `lib/src/renderer/vulkan/vulkan_image.c`
 - `lib/src/renderer/vulkan/vulkan_types.h`
 - `lib/src/renderer/vulkan/vulkan_backend.h`
 - `lib/src/renderer/vulkan/vulkan_backend.c`
 - `lib/src/renderer/vkr_renderer.h`
 - `lib/src/renderer/renderer_frontend.c`

### Phase 6: Picking Shader and Pass ✅ COMPLETE (2-3 days)

**Priority: MEDIUM**

1. Add `VKR_PIPELINE_DOMAIN_PICKING` domain
2. Create picking render pass configuration
3. Create picking shader (`picking.shadercfg`, `picking.slang`)
4. Fix/extend clear semantics for integer attachments
   - `R32_UINT` attachments require integer clear values (current `Vec4 clear_color` is float-based).
   - Either add an integer clear path or switch to a normalized/float ID encoding (less ideal).
5. Decide ID payload
   - If you need both `object_id` and `submesh_id`, define packing into a single `uint32_t` or use a
     multi-channel integer target (e.g. `R32G32_UINT`), and update shader/output accordingly.
6. Extend push constants structure for picking ID payload
7. Test: Render meshes with stable, unique IDs

**Files modified:**
- `lib/src/renderer/vkr_renderer.h` - Added PICKING domain, extended VkrLocalMaterialState
- `lib/src/renderer/vulkan/vulkan_types.h` - Added clear_color_uint field
- `lib/src/renderer/vulkan/vulkan_renderpass.c` - Added picking render pass creation
- `lib/src/renderer/vulkan/vulkan_renderpass.h` - Added function declaration
- `lib/src/renderer/vulkan/vulkan_framebuffer.c` - Added PICKING case (deferred)
- `lib/src/renderer/passes/vkr_pass_world.c` - Pass object_id in push constants
- `assets/shaders/picking.shadercfg` (NEW)
- `assets/shaders/picking.slang` (NEW)

### Phase 7: Picking System Integration ✅ COMPLETE (2-4 days)

**Priority: MEDIUM**

1. Create `VkrPickingContext` structure
2. Implement picking system init/shutdown
3. Implement picking render pass execution (only when a pick is requested, or when hover-picking is enabled)
4. Implement pixel readback with async handling (using Phase 5 ring/fence approach)
5. Coordinate mapping correctness
   - Use the viewport panel’s effective image rect (letterboxing/pillarboxing) to map mouse → pixel.
6. ID stability strategy
   - Short-term: mesh-manager index is OK for prototypes.
   - Medium-term: plan a stable entity/scene ID (optionally 64-bit) and a mapping layer to picking payload.
7. Test: Click on objects, verify correct selection (including resized viewport + aspect-preserved view)

**Files to create:**
- `lib/src/renderer/systems/vkr_picking_system.h`
- `lib/src/renderer/systems/vkr_picking_system.c`

**Files to modify:**
- `lib/src/renderer/renderer_frontend.h`
- `lib/src/renderer/renderer_frontend.c`

### Phase 8: Editor Integration Example (Deferred) (1-2 days)

**Priority: LOW (example/documentation)**

1. Create editor viewport layer
2. Integrate picking with mouse input
3. Visual feedback for selected object
4. Resize handling

---

## 6.1 Current Implementation Status

### Phase 1: Off-Screen Render Target Textures ✅ COMPLETE

| Task | Status |
|------|--------|
| Add `VKR_TEXTURE_FORMAT_R32_UINT` to format enum | ✅ Done |
| Add texture usage flags support (`VkrTextureUsageFlags`) | ✅ Done |
| Implement `vkr_renderer_create_render_target_texture()` | ✅ Done |
| Implement `vkr_renderer_create_depth_attachment()` | ✅ Done |
| Add image layout transition helper | ✅ Done |
| Test: Create off-screen target, render cube, verify texture | ✅ Done |

### Phase 2: Dynamic Viewport Commands ✅ COMPLETE

| Task | Status |
|------|--------|
| Add `VkrViewport` and `VkrScissor` types | ✅ Done |
| Add `set_viewport` and `set_scissor` to backend interface | ✅ Done |
| Implement Vulkan backend functions | ✅ Done |
| Add frontend wrapper functions | ✅ Done |
| Test: Multiple viewports in single frame | ✅ Done |

### Phase 3: Render-to-Texture Sampling ✅ COMPLETE

| Task | Status |
|------|--------|
| Add `vkr_renderer_transition_texture_layout()` function | ✅ Done |
| Ensure texture layout tracking in backend state | ✅ Done |
| Create viewport display shader (`default.viewport_display.slang`) | ✅ Done |
| Create scene viewport layer (Editor layer) | ✅ Done |
| Test: Render scene to texture, display in UI quad | ✅ Done |

### Phase 4: Viewport Rendering Hardening & Future-Proofing ✅ COMPLETE

| Task | Status |
|------|--------|
| Coordinate conventions (window pixels, top-left origin, Y down) | ✅ Done |
| Viewport mapping data (`panel_rect_px` ↔ `image_rect_px` ↔ target size) | ✅ Done |
| Letterboxing mode support (`VKR_VIEWPORT_FIT_CONTAIN`) | ✅ Done |
| Render-scale support for offscreen targets | ✅ Done |
| Helper: window pixel → render-target pixel conversion (picking-ready) | ✅ Done |
| Resize thrash reduction (avoid redundant resizes/rebuilds) | ✅ Done |
| Dynamic viewport/scissor requirement | ✅ Verified (Vulkan pipelines use dynamic state by default) |

#### Phase 4 implementation notes (how it was implemented)

- **New mapping types and APIs**:
  - Added `VkrViewportMapping` / `VkrViewportFitMode` in
    `lib/src/renderer/vkr_viewport.h`
  - Editor viewport utilities expose mapping via
    `vkr_editor_viewport_compute_mapping()`.
- **Mapping computation**:
  - Editor layer computes a pixel-aligned `viewport_rect` (panel rect).
  - `render_scale` scales panel size → target size (clamped).
  - If fit mode is CONTAIN, editor computes `image_rect_px` inside the panel and snaps it to integer pixels.
  - Editor viewport quad transform uses `image_rect_px` so the displayed image rect matches mapping.
- **Offscreen resize ownership**:
  - Editor layer is the single “source of truth” for offscreen target size (derived from panel + render_scale).
  - World ignores window-resize-driven offscreen resizes when an explicit editor-driven size is set.
  - World ignores redundant SET_OFFSCREEN_SIZE messages when the size doesn’t change.
- **Avoid redundant rebuilds/stalls**:
  - UI offscreen switching early-outs when the same config is re-applied (prevents repeated wait-idle and framebuffer rebuilds).

#### Picking usage example (window mouse → target pixel)

```c
#include "renderer/systems/vkr_editor_viewport.h"

VkrViewportMapping m = {0};
if (vkr_editor_viewport_compute_mapping(window_width, window_height, fit_mode,
                                        render_scale, &m)) {
  uint32_t px = 0, py = 0;
  if (vkr_viewport_mapping_window_to_target_pixel(&m, mouse_x, mouse_y, &px, &py)) {
    // Use (px, py) as coordinates into the picking render target.
    // vkr_picking_request(&picking_ctx, px, py);
  }
}
```

### Phase 5: Image-to-Buffer Copy & Pixel Readback API ✅ COMPLETE

| Task | Status |
|------|--------|
| Implement `vulkan_image_copy_to_buffer()` with aspect flag support | ✅ Done |
| Implement `vulkan_image_copy_to_buffer_ex()` for custom aspect masks | ✅ Done |
| Add `VulkanReadbackRing` with 3-slot ring buffer architecture | ✅ Done |
| Add `VulkanReadbackSlot` with fence-based GPU synchronization | ✅ Done |
| Add `VkrReadbackStatus` and `VkrPixelReadbackResult` frontend types | ✅ Done |
| Implement `renderer_vulkan_readback_ring_init/shutdown()` | ✅ Done |
| Implement `renderer_vulkan_request_pixel_readback()` | ✅ Done |
| Implement `renderer_vulkan_get_pixel_readback_result()` with fence wait | ✅ Done |
| Implement `renderer_vulkan_update_readback_ring()` for polling | ✅ Done |
| Add frontend wrappers in `renderer_frontend.c` | ✅ Done |
| Handle non-coherent memory with `vkInvalidateMappedMemoryRanges` | ✅ Done |

#### Phase 5 implementation notes (how it was implemented)

- **Ring buffer architecture**:
  - `VulkanReadbackRing` manages 3 slots (`VKR_READBACK_RING_SIZE`) to avoid reusing in-flight buffers.
  - Each `VulkanReadbackSlot` contains a HOST_VISIBLE buffer, fence, and state tracking.
  - States: `IDLE` → `PENDING` (copy submitted) → `READY` (fence signaled, data available).
- **Image-to-buffer copy**:
  - `vulkan_image_copy_to_buffer()` uses `vkCmdCopyImageToBuffer` with proper `VkBufferImageCopy` region.
  - `vulkan_image_copy_to_buffer_ex()` allows custom aspect flags (e.g., for depth readback).
  - Image must be in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` before copy.
- **Async readback flow**:
  - `vkr_renderer_request_pixel_readback()` finds an IDLE slot, transitions image layout, issues copy command with fence.
  - `vkr_renderer_get_pixel_readback_result()` checks fence status, maps memory, reads pixel data.
  - Non-coherent memory is handled with `vkInvalidateMappedMemoryRanges` before reading.
  - Submission tracking uses a **monotonic submit serial** (not `current_frame`, which wraps) to avoid edge cases where a request can get stuck in PENDING forever.
- **Memory allocation**:
  - Readback buffers use `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT`.
  - Falls back to `HOST_COHERENT` if cached memory unavailable.
- **Backend interface additions**:
  - Added `readback_ring_init`, `readback_ring_shutdown`, `request_pixel_readback`, `get_pixel_readback_result`, `update_readback_ring` to `VkrRendererBackendInterface`.
  - Backend shutdown calls `readback_ring_shutdown` to ensure slot buffers/memory/fences are destroyed before `vkDestroyDevice()`.

### Phase 6: Picking Shader and Pass ✅ COMPLETE

| Task | Status |
|------|--------|
| Add `VKR_PIPELINE_DOMAIN_PICKING` to enum | ✅ Done |
| Add `clear_color_uint` field for integer clear values | ✅ Done |
| Create `vulkan_renderpass_create_picking()` function | ✅ Done |
| Add PICKING cases to render pass begin (uint32 clear) | ✅ Done |
| Add PICKING case to framebuffer regeneration (deferred) | ✅ Done |
| Extend `VkrLocalMaterialState` with `object_id` field | ✅ Done |
| Update world render to pass `mesh_index + 1` as object_id | ✅ Done |
| Create `picking.shadercfg` shader configuration | ✅ Done |
| Create `picking.slang` shader source | ✅ Done |

#### Phase 6 implementation notes (how it was implemented)

- **Design decisions**:
  - ID granularity: Mesh-level only (`uint32_t object_id`), no submesh packing
  - Depth buffer: Own dedicated depth attachment (no cross-pass sync)
  - Render timing: On-demand only (picking pass rendered when pick is requested)
  - Format: R32_UINT for direct integer output, no encoding/decoding

- **Pipeline domain addition**:
  - Added `VKR_PIPELINE_DOMAIN_PICKING = 7` to `VkrPipelineDomain` enum
  - Picking domain is deferred at startup - render pass/framebuffers created on-demand by picking system

- **Integer clear value support**:
  - Added `uint32_t clear_color_uint` to `VulkanRenderPass` struct
  - PICKING case in `vulkan_renderpass_begin()` uses `clear_values[0].color.uint32[0]`
  - Clear value 0 = no object (background)

- **Picking render pass configuration**:
  - Color attachment: `VK_FORMAT_R32_UINT` with CLEAR → TRANSFER_SRC_OPTIMAL layout
  - Depth attachment: Device depth format with CLEAR → DEPTH_STENCIL_ATTACHMENT_OPTIMAL
  - Subpass dependencies optimized for transfer readback after rendering

- **Push constants extension**:
  - `VkrLocalMaterialState` now includes `uint32_t object_id`
  - World view passes `mesh_index + 1` (0 reserved for background/no object)
  - Push constant size: 68 bytes (well under 128 byte limit)

- **Picking shader**:
  - Minimal vertex shader: transforms position using model/view/projection
  - Fragment shader: outputs `object_id` directly as `uint32_t` to R32_UINT target
  - Uses `nointerpolation` qualifier on object_id to prevent interpolation artifacts
  - Same vertex layout as world shader for geometry compatibility

- **Deferred initialization**:
  - PICKING render pass creation returns early with success (deferred to picking system)
  - PICKING framebuffer regeneration returns early (custom off-screen attachments needed)
  - Avoids validation errors from format mismatch (R32_UINT vs swapchain format)

### Phase 7: Picking System Integration ✅ COMPLETE

| Task | Status |
|------|--------|
| Create `VkrPickingContext` structure | ✅ Done |
| Implement picking system init/shutdown | ✅ Done |
| Implement on-demand picking render pass execution | ✅ Done |
| Implement async pixel readback polling | ✅ Done |
| Coordinate mapping contract (window → target pixel) | ✅ Done (API expects target coords; editor mapping helper exists) |
| ID strategy (mesh-manager index for prototype) | ✅ Done |
| Test: hover/click selection demo | ✅ Done |

#### Phase 7 implementation notes (how it was implemented)

- **Core API + state machine**:
  - Implemented in `lib/src/renderer/systems/vkr_picking_system.c/.h`.
  - `VkrPickingContext` owns the off-screen picking target (R32_UINT + depth), the picking pass, and the picking pipeline.
  - State machine: `IDLE` → `RENDER_PENDING` → `READBACK_PENDING` → `IDLE` (result stored in `ctx->result_object_id`).
  - `vkr_picking_get_result()` returns the **last known** pick result, so “no new readback this frame” does not look like “no hit”.

- **On-demand rendering**:
  - `vkr_picking_request(ctx, x, y)` records the target pixel and arms the request.
  - `vkr_picking_render(renderer, ctx, mesh_manager)` only executes when a request is armed (no continuous picking cost unless enabled by the caller).

- **Picking pass render**:
  - Uses `shader.picking` + the registered picking pipeline.
  - Iterates meshes from `VkrMeshManager` and draws all submeshes to the picking target.
  - Pushes `model` and `object_id = mesh_index + 1` (0 reserved for background/no object).

- **Async readback integration**:
  - After the picking pass, requests a 1×1 pixel readback via `vkr_renderer_request_pixel_readback(picking_texture, x, y)`.
  - Polls completion via `vkr_renderer_get_pixel_readback_result()` and stores the decoded `object_id`.
  - Readback submission detection uses a monotonic submit serial to avoid deadlocks caused by wrapping frame indices.

- **Shutdown correctness**:
  - The Vulkan backend calls `renderer_vulkan_readback_ring_shutdown()` during shutdown so the readback ring’s slot buffers/memory/fences are destroyed before device teardown (prevents validation errors).

### Summary of Completed Features

**Core Infrastructure:**
- Offscreen render targets for World, Skybox, and UI layers
- Editor layer sampling offscreen color target as a viewport plane
- Render-target texture creation with configurable usage flags
- Depth attachment creation for offscreen rendering
- Layout transitions and layout tracking for custom color attachments

**Dynamic Rendering:**
- Dynamic viewport/scissor API for runtime viewport control
- Viewport display shader and material pipeline
- Editor viewport layer with panel-style layout computation
- UI render targets routed to offscreen viewport in editor mode

**Runtime Features:**
- Toggle between offscreen and swapchain rendering (F6 key)
- Proper framebuffer cleanup during mode switching (no leaks)
- Automatic resize handling for offscreen targets
- Retina display support on macOS

**Phase 4 Hardening (Viewport + Picking Prep):**
- Viewport mapping and coordinate conversion for future pixel-perfect picking
- Render-scale control for offscreen targets
- Reduced redundant offscreen resize and framebuffer rebuild work

**Phase 5 Pixel Readback (Picking Infrastructure):**
- Async pixel readback with 3-slot ring buffer architecture
- Fence-based GPU synchronization for host reads
- Image-to-buffer copy operations (`vulkan_image_copy_to_buffer`)
- Non-coherent memory handling with proper invalidation
- Frontend API: `vkr_renderer_request_pixel_readback()`, `vkr_renderer_get_pixel_readback_result()`

**Phase 6 Picking Shader and Pass:**
- `VKR_PIPELINE_DOMAIN_PICKING` domain with R32_UINT color attachment
- Integer clear value support (`clear_color_uint` in render pass)
- `vulkan_renderpass_create_picking()` with proper layout transitions for readback
- Extended `VkrLocalMaterialState` with `object_id` field in push constants
- Picking shader (`picking.slang`) outputs object_id as uint32 directly
- World view now passes `mesh_index + 1` as object_id for all rendered meshes
- Deferred initialization pattern (render pass/framebuffers created on-demand)

**Phase 7 Picking System Integration:**
- Picking context + state machine (`vkr_picking_system.c/.h`)
- On-demand picking render + 1×1 pixel readback polling
- “Sticky” last known pick result semantics for hover/selection UX

**Bug Fixes Applied:**
- Fixed framebuffer leaks when switching rendering modes
- Fixed Retina display scaling issues on macOS
- Fixed CAMetalLayer contentsScale and drawableSize initialization
- Fixed async readback completion detection (avoid `current_frame` wrap deadlock)
- Fixed Vulkan shutdown leaks by destroying readback ring resources before `vkDestroyDevice()`

### Phases Not Implemented Yet

- **Phase 8:** Editor integration example (deferred)

### Files Updated

**View Layer System:**
- `lib/src/renderer/passes/vkr_pass_world.c` - Offscreen target management (Phase 6: pass object_id)
- `lib/src/renderer/systems/vkr_world_resources.h` - Public API and data types
- `lib/src/renderer/passes/vkr_pass_skybox.c` - Custom target support
- `lib/src/renderer/systems/vkr_skybox_system.h` - Public API
- `lib/src/renderer/passes/vkr_pass_ui.c` - Offscreen mode switching
- `lib/src/renderer/systems/vkr_ui_system.h` - Public API
- `lib/src/renderer/passes/vkr_pass_editor.c` - Editor viewport layer (NEW)
- `lib/src/renderer/systems/vkr_editor_viewport.h` - Public API (NEW)
- `lib/src/renderer/vkr_rg_compile.c` - Pass attachment -> render target creation

**Renderer Core:**
- `lib/src/renderer/vkr_renderer.h` - New types and API (Phase 5: readback; Phase 6: PICKING domain, object_id)
- `lib/src/renderer/renderer_frontend.c` - Frontend wrappers (Phase 5: readback API)
- `lib/src/renderer/vulkan/vulkan_backend.c` - Backend implementations (Phase 5: readback ring; Phase 7: shutdown/readback fixes)
- `lib/src/renderer/vulkan/vulkan_backend.h` - Backend interface (Phase 5: readback functions)
- `lib/src/renderer/vulkan/vulkan_types.h` - Vulkan types (Phase 5: readback; Phase 6: clear_color_uint; Phase 7: submit serial)
- `lib/src/renderer/vulkan/vulkan_renderpass.c` - Render pass creation (Phase 6: picking pass)
- `lib/src/renderer/vulkan/vulkan_renderpass.h` - Render pass API (Phase 6: picking function)
- `lib/src/renderer/vulkan/vulkan_framebuffer.c` - Framebuffer creation (Phase 6: PICKING case)
- `lib/src/renderer/vulkan/vulkan_utils.c` - Format conversions
- `lib/src/renderer/vulkan/vulkan_image.c` - Image creation (Phase 5: copy-to-buffer)
- `lib/src/renderer/vulkan/vulkan_image.h` - Image API (Phase 5: copy functions)

**Platform:**
- `lib/src/platform/vkr_window_macos.m` - Retina display fixes

**Assets:**
- `assets/shaders/default.viewport_display.shadercfg` (NEW)
- `assets/shaders/default.viewport_display.slang` (NEW)
- `assets/shaders/picking.shadercfg` (NEW - Phase 6)
- `assets/shaders/picking.slang` (NEW - Phase 6)
- `assets/materials/default.viewport_display.mt` (NEW)

**Application:**
- `app/src/main.c` - F6 toggle for editor mode
- `app/src/main.c` - Picking hover demo (updates UI text)

## 7. API Reference

### 7.1 Render Target Textures

```c
// Create color attachment for off-screen rendering
VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture(
    VkrRendererFrontendHandle renderer,
    const VkrRenderTargetTextureDesc *desc,
    VkrRendererError *out_error);

// Create depth attachment
VkrTextureOpaqueHandle vkr_renderer_create_depth_attachment(
    VkrRendererFrontendHandle renderer,
    uint32_t width,
    uint32_t height,
    VkrRendererError *out_error);

// Transition texture layout
VkrRendererError vkr_renderer_transition_texture_layout(
    VkrRendererFrontendHandle renderer,
    VkrTextureOpaqueHandle texture,
    VkrTextureLayout old_layout,
    VkrTextureLayout new_layout);
```

### 7.2 Dynamic Viewport

```c
void vkr_renderer_set_viewport(VkrRendererFrontendHandle renderer,
                               const VkrViewport *viewport);

void vkr_renderer_set_scissor(VkrRendererFrontendHandle renderer,
                              const VkrScissor *scissor);

void vkr_renderer_set_viewport_scissor(VkrRendererFrontendHandle renderer,
                                       float32_t x, float32_t y,
                                       float32_t width, float32_t height);
```

### 7.3 Editor Viewport Mapping (Phase 4)

```c
// Types
typedef enum VkrViewportFitMode {
  VKR_VIEWPORT_FIT_STRETCH = 0,
  VKR_VIEWPORT_FIT_CONTAIN = 1,
} VkrViewportFitMode;

typedef struct VkrViewportMapping {
  Vec4 panel_rect_px;  // (x, y, w, h) in window pixels
  Vec4 image_rect_px;  // (x, y, w, h) in window pixels
  uint32_t target_width;
  uint32_t target_height;
  VkrViewportFitMode fit_mode;
} VkrViewportMapping;

// Compute mapping from the editor viewport layout
bool8_t vkr_editor_viewport_compute_mapping(uint32_t window_width,
                                            uint32_t window_height,
                                            VkrViewportFitMode fit_mode,
                                            float32_t render_scale,
                                            VkrViewportMapping *out_mapping);

// Convert window mouse → render-target pixel (for picking)
bool8_t vkr_viewport_mapping_window_to_target_pixel(
    const VkrViewportMapping *mapping, int32_t window_x, int32_t window_y,
    uint32_t *out_x, uint32_t *out_y);

// Optional knobs live in `ApplicationEditorViewport` (fit mode + render scale)
```

**Picking usage example (window mouse → target pixel):**

```c
#include "renderer/systems/vkr_editor_viewport.h"

VkrViewportMapping m = {0};
if (vkr_editor_viewport_compute_mapping(window_width, window_height, fit_mode,
                                        render_scale, &m)) {
  uint32_t px = 0, py = 0;
  if (vkr_viewport_mapping_window_to_target_pixel(&m, mouse_x, mouse_y, &px, &py)) {
    // Use (px, py) as coordinates into the picking render target.
    // vkr_picking_request(&picking_ctx, px, py);
  }
}
```

### 7.4 Pixel Readback ✅ IMPLEMENTED

```c
// Readback status enum
typedef enum VkrReadbackStatus {
  VKR_READBACK_STATUS_IDLE = 0,    // No readback in progress
  VKR_READBACK_STATUS_PENDING,     // Copy submitted, waiting for GPU
  VKR_READBACK_STATUS_READY,       // Data ready for CPU read
  VKR_READBACK_STATUS_ERROR,       // An error occurred
} VkrReadbackStatus;

// Readback result structure
typedef struct VkrPixelReadbackResult {
  VkrReadbackStatus status;
  uint32_t x;          // Requested X coordinate
  uint32_t y;          // Requested Y coordinate
  uint32_t data;       // Pixel data (e.g., object ID for picking)
  bool8_t valid;       // True if result is valid
} VkrPixelReadbackResult;

// Request async pixel readback from a texture
VkrRendererError vkr_renderer_request_pixel_readback(
    VkrRendererFrontendHandle renderer,
    TextureHandle texture,
    uint32_t x, uint32_t y,
    uint32_t width, uint32_t height);

// Get result of a pending readback (non-blocking or with fence wait)
VkrRendererError vkr_renderer_get_pixel_readback_result(
    VkrRendererFrontendHandle renderer,
    VkrPixelReadbackResult *out_result);

// Poll/update readback ring state (call once per frame)
void vkr_renderer_update_readback_ring(VkrRendererFrontendHandle renderer);
```

**Usage example:**
```c
// Request readback at cursor position
vkr_renderer_request_pixel_readback(renderer, picking_texture, px, py, 1, 1);

// Next frame (or after fence): get result
VkrPixelReadbackResult result;
vkr_renderer_get_pixel_readback_result(renderer, &result);
if (result.valid && result.status == VKR_READBACK_STATUS_READY) {
  uint32_t object_id = result.data;
  // Use object_id for selection...
}
```

### 7.5 Picking System

```c
bool8_t vkr_picking_init(VkrRendererFrontendHandle renderer,
                         VkrPickingContext *ctx,
                         uint32_t width, uint32_t height);

void vkr_picking_resize(VkrRendererFrontendHandle renderer,
                        VkrPickingContext *ctx,
                        uint32_t new_width, uint32_t new_height);

void vkr_picking_render(VkrRendererFrontendHandle renderer,
                        VkrPickingContext *ctx,
                        VkrMeshManager *mesh_manager);

void vkr_picking_request(VkrPickingContext *ctx,
                         uint32_t x, uint32_t y);

VkrPickResult vkr_picking_get_result(VkrRendererFrontendHandle renderer,
                                     VkrPickingContext *ctx);

void vkr_picking_shutdown(VkrRendererFrontendHandle renderer,
                          VkrPickingContext *ctx);
```

---

## 8. File Changes Summary

Note: The tables below describe planned changes for remaining phases. The
implemented changes to date are listed in Section 6.1.

### New Files

| File | Description |
|------|-------------|
| `lib/src/renderer/systems/vkr_picking_system.h` | Picking system types and API |
| `lib/src/renderer/systems/vkr_picking_system.c` | Picking system implementation |
| `assets/shaders/picking.shadercfg` | Picking shader configuration |
| `assets/shaders/picking.slang` | Picking shader source |
| `assets/shaders/default.viewport_display.shadercfg` | Viewport display shader config |
| `assets/shaders/default.viewport_display.slang` | Viewport display shader source |

### Modified Files

| File | Changes |
|------|---------|
| `lib/src/renderer/vkr_renderer.h` | New types, enums, API functions |
| `lib/src/renderer/vulkan/vulkan_backend.h` | Backend interface additions |
| `lib/src/renderer/vulkan/vulkan_backend.c` | New backend implementations |
| `lib/src/renderer/vulkan/vulkan_image.h` | `vulkan_image_copy_to_buffer()` |
| `lib/src/renderer/vulkan/vulkan_image.c` | Image copy implementation |
| `lib/src/renderer/vulkan/vulkan_utils.c` | Format conversion for R32_UINT |
| `lib/src/renderer/vulkan/vulkan_renderpass.c` | Picking pass creation |
| `lib/src/renderer/vulkan/vulkan_framebuffer.c` | Picking framebuffer |
| `lib/src/renderer/renderer_frontend.c` | Frontend wrapper functions |
| `lib/src/renderer/renderer_frontend.h` | Picking context in frontend |

---

## Appendix A: Texture Layout Enum

```c
typedef enum VkrTextureLayout {
  VKR_TEXTURE_LAYOUT_UNDEFINED = 0,
  VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT,
  VKR_TEXTURE_LAYOUT_DEPTH_ATTACHMENT,
  VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY,
  VKR_TEXTURE_LAYOUT_TRANSFER_SRC,
  VKR_TEXTURE_LAYOUT_TRANSFER_DST,
  VKR_TEXTURE_LAYOUT_PRESENT,
} VkrTextureLayout;
```

## Appendix B: Texture Usage Flags

```c
typedef enum VkrTextureUsageBits {
  VKR_TEXTURE_USAGE_SAMPLED_BIT = 1 << 0,
  VKR_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT = 1 << 1,
  VKR_TEXTURE_USAGE_DEPTH_ATTACHMENT_BIT = 1 << 2,
  VKR_TEXTURE_USAGE_TRANSFER_SRC_BIT = 1 << 3,
  VKR_TEXTURE_USAGE_TRANSFER_DST_BIT = 1 << 4,
  VKR_TEXTURE_USAGE_STORAGE_BIT = 1 << 5,
} VkrTextureUsageBits;
typedef Bitset8 VkrTextureUsageFlags;
```

---

## Appendix C: Example Editor Integration

```c
// Example: Editor with scene viewport and object selection

typedef struct EditorState {
  VkrRendererFrontendHandle renderer;
  VkrMeshManager *mesh_manager;

  // Scene viewport
  VkrTextureOpaqueHandle scene_color;
  VkrTextureOpaqueHandle scene_depth;
  VkrRenderTargetHandle scene_target;
  uint32_t viewport_width;
  uint32_t viewport_height;

  // Picking
  VkrPickingContext picking;

  // Selection state
  uint32_t selected_mesh_index;
  bool8_t has_selection;
} EditorState;

void editor_on_mouse_click(EditorState *editor, int32_t x, int32_t y) {
  // Convert screen coords to viewport coords
  // (assuming viewport is at some offset in the window)
  int32_t vp_x = x - editor->viewport_offset_x;
  int32_t vp_y = y - editor->viewport_offset_y;

  if (vp_x >= 0 && vp_x < (int32_t)editor->viewport_width &&
      vp_y >= 0 && vp_y < (int32_t)editor->viewport_height) {
    vkr_picking_request(&editor->picking, (uint32_t)vp_x, (uint32_t)vp_y);
  }
}

void editor_update(EditorState *editor) {
  // Check for picking result
  VkrPickResult result = vkr_picking_get_result(editor->renderer,
                                                 &editor->picking);
  if (result.hit) {
    editor->selected_mesh_index = result.object_id - 1;
    editor->has_selection = true_v;

    VkrMesh *mesh = vkr_mesh_manager_get(editor->mesh_manager,
                                          editor->selected_mesh_index);
    // Highlight selected mesh, show properties panel, etc.
  }
}

void editor_render(EditorState *editor, float64_t delta_time) {
  // 1. Render picking pass (if request pending)
  vkr_picking_render(editor->renderer, &editor->picking,
                     editor->mesh_manager);

  // 2. Render scene to off-screen target
  vkr_renderer_begin_render_pass(editor->renderer,
                                  editor->scene_pass,
                                  editor->scene_target);
  // ... render meshes ...
  vkr_renderer_end_render_pass(editor->renderer);

  // 3. Transition scene texture for sampling
  vkr_renderer_transition_texture_layout(editor->renderer,
                                          editor->scene_color,
                                          VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT,
                                          VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY);

  // 4. Render UI with scene viewport
  // ... bind scene_color as texture, draw viewport quad ...
}
```

---

*Document Version: 1.6*
*Last Updated: 2026-01-02*
*Author: AI Assistant*
*Phases 1-7: Complete; Phase 7 integrates the picking system + async readback fixes; Phase 8 (editor integration example) remains deferred*
