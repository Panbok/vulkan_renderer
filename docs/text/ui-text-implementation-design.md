---
status: partial
updated: 2026-07-31
authority: design
---
# UI Text Implementation Design Document

**Legacy note:** This document references the deprecated view/layer system
(`VkrLayerContext`, `view system (removed)`) in examples. Render orchestration now
uses the render graph; stateless pass executors and `VkrUiSystem` own rendering.
UI text ships, but historical examples and the unchecked checklist have not
been fully reconciled with current code.

## Overview

This document describes the implementation plan for `vkr_ui_text.c` - the UI text resource that composes font system, text layout, and GPU buffers for text rendering.

**Target File:** `lib/src/renderer/resources/vkr_ui_text.c`
**Header:** `lib/src/renderer/resources/vkr_ui_text.h`
**Reference Implementations:**
- `lib/src/renderer/systems/vkr_geometry_system.c` (buffer creation/binding)
- `lib/src/renderer/passes/vkr_pass_ui.c` (UI rendering flow)

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Text Rendering Flow                           │
└─────────────────────────────────────────────────────────────────────────┘

┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  VkrUiText   │────▶│ VkrFontSystem│────▶│   VkrFont    │
│  (Resource)  │     │   (System)   │     │  (Resource)  │
└──────────────┘     └──────────────┘     └──────────────┘
       │                                         │
       │ content + config                        │ glyphs + atlas
       ▼                                         ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  VkrText +   │────▶│ VkrTextLayout│────▶│ Glyph Quads  │
│ VkrTextStyle │     │  (Computed)  │     │ (Positions)  │
└──────────────┘     └──────────────┘     └──────────────┘
                                                 │
                                                 ▼
                     ┌──────────────────────────────────────┐
                     │         GPU Buffer Generation        │
                     │  ┌────────────┐   ┌────────────────┐ │
                     │  │  Vertex    │   │     Index      │ │
                     │  │  Buffer    │   │     Buffer     │ │
                     │  │ (VkrText   │   │  (6 per quad)  │ │
                     │  │  Vertex)   │   │                │ │
                     │  └────────────┘   └────────────────┘ │
                     └──────────────────────────────────────┘
                                                 │
                                                 ▼
                     ┌──────────────────────────────────────┐
                     │           Rendering Pipeline         │
                     │  1. Bind UI pipeline                 │
                     │  2. Apply global uniforms            │
                     │  3. Apply local uniforms (model)     │
                     │  4. Bind font atlas texture          │
                     │  5. Draw indexed (vertex+index buf)  │
                     └──────────────────────────────────────┘
```

---

## Dependencies

```c
#include "renderer/resources/vkr_ui_text.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_text.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_buffer.h"
```

---

## Implementation Phases

### Phase 1: Core Create/Destroy Functions

#### `vkr_ui_text_create`

```c
bool8_t vkr_ui_text_create(VkrRendererFrontendHandle renderer,
                           VkrAllocator *allocator, VkrFontSystem *font_system,
                           String8 content, const VkrUiTextConfig *config,
                           VkrUiText *out_text, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(font_system != NULL, "Font system is NULL");
  assert_log(out_text != NULL, "Output text is NULL");

  if (out_error) *out_error = VKR_RENDERER_ERROR_NONE;

  MemZero(out_text, sizeof(VkrUiText));

  // Store dependencies
  out_text->renderer = renderer;
  out_text->font_system = font_system;
  out_text->allocator = allocator;

  // Copy content (owned by VkrUiText)
  out_text->content = string8_copy(allocator, content);

  // Apply config (or defaults)
  out_text->config = config ? *config : VKR_UI_TEXT_CONFIG_DEFAULT;

  // Initialize transform
  out_text->transform = vkr_transform_identity();

  // Mark as dirty to trigger initial layout/buffer generation
  out_text->layout_dirty = true_v;
  out_text->buffers_dirty = true_v;

  // Resolve font
  if (out_text->config.font.id != 0) {
    out_text->resolved_font = vkr_font_system_get_by_handle(
        font_system, out_text->config.font);
  }
  if (!out_text->resolved_font) {
    out_text->resolved_font = vkr_font_system_get_default_bitmap_font(font_system);
  }

  if (!out_text->resolved_font) {
    log_error("No font available for UI text");
    if (out_error) *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_FOUND;
    return false_v;
  }

  return true_v;
}
```

#### `vkr_ui_text_destroy`

```c
void vkr_ui_text_destroy(VkrUiText *text) {
  if (!text) return;

  // Destroy GPU buffers
  if (text->render.vertex_buffer.handle) {
    vkr_vertex_buffer_destroy(text->renderer, &text->render.vertex_buffer);
  }
  if (text->render.index_buffer.handle) {
    vkr_index_buffer_destroy(text->renderer, &text->render.index_buffer);
  }

  // Destroy layout (frees glyph array)
  vkr_text_layout_destroy(&text->layout);

  // Free owned content string
  if (text->content.str && text->allocator) {
    vkr_allocator_free(text->allocator, (void *)text->content.str,
                       text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  MemZero(text, sizeof(VkrUiText));
}
```

---

### Phase 2: Configuration Setters

#### `vkr_ui_text_set_content`

```c
bool8_t vkr_ui_text_set_content(VkrUiText *text, String8 content) {
  if (!text || !text->allocator) return false_v;

  // Free old content if owned
  if (text->content.str) {
    vkr_allocator_free(text->allocator, (void *)text->content.str,
                       text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  // Copy new content
  text->content = string8_copy(text->allocator, content);

  // Mark for re-layout and buffer regeneration
  text->layout_dirty = true_v;
  text->buffers_dirty = true_v;

  return true_v;
}
```

#### `vkr_ui_text_set_config`

```c
void vkr_ui_text_set_config(VkrUiText *text, const VkrUiTextConfig *config) {
  if (!text || !config) return;

  // Check if font changed
  bool8_t font_changed = (text->config.font.id != config->font.id ||
                          text->config.font.generation != config->font.generation);

  // Check if layout-affecting properties changed
  bool8_t layout_changed = (text->config.font_size != config->font_size ||
                            text->config.letter_spacing != config->letter_spacing ||
                            text->config.layout.max_width != config->layout.max_width ||
                            text->config.layout.max_height != config->layout.max_height ||
                            text->config.layout.word_wrap != config->layout.word_wrap);

  // Check if color changed (per-vertex, requires buffer update)
  bool8_t color_changed = !vec4_equal(text->config.color, config->color);

  text->config = *config;

  if (font_changed) {
    // Re-resolve font
    if (config->font.id != 0) {
      text->resolved_font = vkr_font_system_get_by_handle(
          text->font_system, config->font);
    }
    if (!text->resolved_font) {
      text->resolved_font = vkr_font_system_get_default_bitmap_font(text->font_system);
    }
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (layout_changed) {
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (color_changed) {
    // Color is per-vertex in VkrTextVertex
    text->buffers_dirty = true_v;
  }
}
```

#### `vkr_ui_text_set_position`

```c
void vkr_ui_text_set_position(VkrUiText *text, Vec2 position) {
  if (!text) return;
  vkr_transform_set_position(&text->transform,
                             vec3_new(position.x, position.y, 0.0f));
}
```

#### `vkr_ui_text_set_color`

```c
void vkr_ui_text_set_color(VkrUiText *text, Vec4 color) {
  if (!text) return;

  // Check if color actually changed
  if (vec4_equal(text->config.color, color)) return;

  text->config.color = color;

  // Color is per-vertex in VkrTextVertex, so buffers need regeneration
  text->buffers_dirty = true_v;
}
```

---

### Phase 3: Layout Computation

#### Internal: `vkr_ui_text_compute_layout`

```c
vkr_internal void vkr_ui_text_compute_layout(VkrUiText *text) {
  if (!text || !text->resolved_font) return;

  // Destroy previous layout if exists
  if (text->layout.glyphs.data) {
    vkr_text_layout_destroy(&text->layout);
  }

  // Build VkrTextStyle from config
  float32_t font_size = text->config.font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)text->resolved_font->size;
  }

  VkrTextStyle style = vkr_text_style_new(
      text->config.font, font_size, text->config.color);
  style.letter_spacing = text->config.letter_spacing;
  style = vkr_text_style_with_font_data(&style, text->resolved_font);

  // Create VkrText for layout computation
  VkrText text_for_layout = vkr_text_from_view(text->content, &style);

  // Compute layout using text system
  text->layout = vkr_text_layout_compute(
      text->allocator, &text_for_layout, &text->config.layout);

  // Store computed bounds
  text->bounds.size = text->layout.bounds;
  // Ascent/descent from font metrics scaled
  float32_t scale = font_size / (float32_t)text->resolved_font->size;
  text->bounds.ascent = (float32_t)text->resolved_font->ascent * scale;
  text->bounds.descent = (float32_t)text->resolved_font->descent * scale;

  text->layout_dirty = false_v;
}
```

---

### Phase 4: Buffer Generation

#### Internal: `vkr_ui_text_generate_buffers`

This is the core function that converts glyph positions to GPU vertex/index buffers.

```c
// Vertex format: VkrTextVertex (defined in vkr_buffer.h)
// struct VkrTextVertex {
//   Vec2 position;  // Screen position
//   Vec2 texcoord;  // Atlas UV
//   Vec4 color;     // Per-vertex color (from style)
// };

vkr_internal bool8_t vkr_ui_text_generate_buffers(VkrUiText *text) {
  if (!text || !text->resolved_font) return false_v;

  uint32_t glyph_count = (uint32_t)text->layout.glyphs.length;
  if (glyph_count == 0) {
    // No glyphs to render - clear buffers
    text->render.quad_count = 0;
    text->buffers_dirty = false_v;
    return true_v;
  }

  uint32_t vertex_count = glyph_count * 4;  // 4 vertices per quad
  uint32_t index_count = glyph_count * 6;   // 6 indices per quad (2 triangles)

  // Calculate required capacity
  bool8_t need_realloc = (vertex_count > text->render.vertex_capacity ||
                          index_count > text->render.index_capacity);

  // Allocate temp memory for vertex/index data
  VkrAllocatorScope scope = vkr_allocator_begin_scope(text->allocator);

  VkrTextVertex *vertices = vkr_allocator_alloc(
      text->allocator, sizeof(VkrTextVertex) * vertex_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(
      text->allocator, sizeof(uint32_t) * index_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!vertices || !indices) {
    vkr_allocator_end_scope(&scope);
    return false_v;
  }

  // Font metrics for UV calculation
  float32_t atlas_w = (float32_t)text->resolved_font->atlas_size_x;
  float32_t atlas_h = (float32_t)text->resolved_font->atlas_size_y;
  float32_t inv_atlas_w = 1.0f / atlas_w;
  float32_t inv_atlas_h = 1.0f / atlas_h;

  // Scale factor
  float32_t font_size = text->config.font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)text->resolved_font->size;
  }
  float32_t scale = font_size / (float32_t)text->resolved_font->size;

  // Generate quads for each glyph
  uint32_t vertex_idx = 0;
  uint32_t index_idx = 0;

  for (uint32_t i = 0; i < glyph_count; i++) {
    VkrTextGlyph *layout_glyph = &text->layout.glyphs.data[i];

    // Look up glyph data from font
    VkrFontGlyph *font_glyph = NULL;
    uint32_t *glyph_index_ptr = vkr_hash_table_get_uint32_t(
        &text->resolved_font->glyph_indices, layout_glyph->codepoint);

    if (glyph_index_ptr && *glyph_index_ptr < text->resolved_font->glyphs.length) {
      font_glyph = &text->resolved_font->glyphs.data[*glyph_index_ptr];
    }

    if (!font_glyph) continue;  // Skip missing glyphs

    // Calculate screen positions (with scaling and offsets)
    float32_t x0 = layout_glyph->position.x + (float32_t)font_glyph->x_offset * scale;
    float32_t y0 = layout_glyph->position.y + (float32_t)font_glyph->y_offset * scale;
    float32_t x1 = x0 + (float32_t)font_glyph->width * scale;
    float32_t y1 = y0 + (float32_t)font_glyph->height * scale;

    // Calculate UV coordinates from atlas
    float32_t u0 = (float32_t)font_glyph->x * inv_atlas_w;
    float32_t v0 = (float32_t)font_glyph->y * inv_atlas_h;
    float32_t u1 = (float32_t)(font_glyph->x + font_glyph->width) * inv_atlas_w;
    float32_t v1 = (float32_t)(font_glyph->y + font_glyph->height) * inv_atlas_h;

    // Generate 4 vertices for this glyph quad
    // Vertex order: top-left, bottom-right, bottom-left, top-right
    // (matching default plane2d winding)
    uint32_t base_vertex = vertex_idx;
    Vec4 color = text->config.color;  // Per-vertex color from config

    // Vertex 0: top-left
    vertices[vertex_idx].position = vec2_new(x0, y0);
    vertices[vertex_idx].texcoord = vec2_new(u0, v0);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    // Vertex 1: bottom-right
    vertices[vertex_idx].position = vec2_new(x1, y1);
    vertices[vertex_idx].texcoord = vec2_new(u1, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    // Vertex 2: bottom-left
    vertices[vertex_idx].position = vec2_new(x0, y1);
    vertices[vertex_idx].texcoord = vec2_new(u0, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    // Vertex 3: top-right
    vertices[vertex_idx].position = vec2_new(x1, y0);
    vertices[vertex_idx].texcoord = vec2_new(u1, v0);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    // Generate 6 indices for 2 triangles (CCW winding)
    // Triangle 1: 2, 1, 0 (bottom-left, bottom-right, top-left)
    // Triangle 2: 3, 0, 1 (top-right, top-left, bottom-right)
    indices[index_idx++] = base_vertex + 2;
    indices[index_idx++] = base_vertex + 1;
    indices[index_idx++] = base_vertex + 0;
    indices[index_idx++] = base_vertex + 3;
    indices[index_idx++] = base_vertex + 0;
    indices[index_idx++] = base_vertex + 1;
  }

  // Update actual counts (may be less than allocated if glyphs were skipped)
  vertex_count = vertex_idx;
  index_count = index_idx;
  text->render.quad_count = vertex_count / 4;

  // Create or update GPU buffers
  VkrRendererError buffer_err = VKR_RENDERER_ERROR_NONE;

  if (need_realloc || !text->render.vertex_buffer.handle) {
    // Destroy old buffers
    if (text->render.vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(text->renderer, &text->render.vertex_buffer);
    }
    if (text->render.index_buffer.handle) {
      vkr_index_buffer_destroy(text->renderer, &text->render.index_buffer);
    }

    // Create new buffers with some extra capacity for growth
    uint32_t new_vertex_capacity = vertex_count + 64;  // Small growth margin
    uint32_t new_index_capacity = index_count + 96;

    text->render.vertex_buffer = vkr_vertex_buffer_create(
        text->renderer,
        vertices,
        sizeof(VkrTextVertex),
        new_vertex_capacity,
        VKR_VERTEX_INPUT_RATE_VERTEX,
        string8_lit("ui_text_vertices"),
        &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      vkr_allocator_end_scope(&scope);
      return false_v;
    }

    text->render.index_buffer = vkr_index_buffer_create(
        text->renderer,
        indices,
        VKR_INDEX_TYPE_UINT32,
        new_index_capacity,
        string8_lit("ui_text_indices"),
        &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      vkr_vertex_buffer_destroy(text->renderer, &text->render.vertex_buffer);
      vkr_allocator_end_scope(&scope);
      return false_v;
    }

    text->render.vertex_capacity = new_vertex_capacity;
    text->render.index_capacity = new_index_capacity;
  } else {
    // Update existing buffers
    buffer_err = vkr_vertex_buffer_update(
        text->renderer, &text->render.vertex_buffer,
        vertices, 0, vertex_count);

    if (buffer_err == VKR_RENDERER_ERROR_NONE) {
      buffer_err = vkr_index_buffer_update(
          text->renderer, &text->render.index_buffer,
          indices, 0, index_count);
    }

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      vkr_allocator_end_scope(&scope);
      return false_v;
    }
  }

  // Update buffer metadata
  text->render.vertex_buffer.vertex_count = vertex_count;
  text->render.index_buffer.index_count = index_count;

  vkr_allocator_end_scope(&scope);
  text->buffers_dirty = false_v;
  return true_v;
}
```

---

### Phase 5: Prepare and Draw

#### `vkr_ui_text_prepare`

```c
bool8_t vkr_ui_text_prepare(VkrUiText *text) {
  if (!text) return false_v;

  // Recompute layout if needed
  if (text->layout_dirty) {
    vkr_ui_text_compute_layout(text);
  }

  // Regenerate buffers if needed
  if (text->buffers_dirty) {
    if (!vkr_ui_text_generate_buffers(text)) {
      log_error("Failed to generate UI text buffers");
      return false_v;
    }
  }

  // Check if we have valid render data
  return (text->render.quad_count > 0 &&
          text->render.vertex_buffer.handle != 0 &&
          text->render.index_buffer.handle != 0);
}
```

#### `vkr_ui_text_get_bounds`

```c
VkrTextBounds vkr_ui_text_get_bounds(VkrUiText *text) {
  if (!text) {
    return (VkrTextBounds){0};
  }

  // Ensure layout is up to date
  if (text->layout_dirty) {
    vkr_ui_text_compute_layout(text);
  }

  return text->bounds;
}
```

#### `vkr_ui_text_draw`

```c
void vkr_ui_text_draw(VkrUiText *text, VkrPipelineHandle pipeline) {
  if (!text || !text->renderer) return;

  // Ensure buffers are ready
  if (!vkr_ui_text_prepare(text)) return;

  if (text->render.quad_count == 0) return;

  RendererFrontend *rf = (RendererFrontend *)text->renderer;

  // Bind vertex buffer
  VkrVertexBufferBinding vbb = {
      .buffer = text->render.vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(text->renderer, &vbb);

  // Bind index buffer
  VkrIndexBufferBinding ibb = {
      .buffer = text->render.index_buffer.handle,
      .type = VKR_INDEX_TYPE_UINT32,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(text->renderer, &ibb);

  // Apply local material state (model matrix from transform)
  Mat4 model = vkr_transform_get_world(&text->transform);
  vkr_material_system_apply_local(&rf->material_system,
                                  &(VkrLocalMaterialState){.model = model});

  // Draw all glyph quads in one call
  vkr_renderer_draw_indexed(
      text->renderer,
      text->render.index_buffer.index_count,  // index count
      1,                                       // instance count
      0,                                       // first index
      0,                                       // vertex offset
      0                                        // first instance
  );

  text->render.last_frame_rendered = rf->frame_number;
}
```

---

## Integration with UI View

### Option A: Direct Integration in `vkr_ui_system.c`

Add text rendering after existing UI geometry rendering:

```c
// vkr_ui_system.c - render helper invoked by the render-graph pass executor
void vkr_pass_ui_execute(RendererFrontend *rf, uint32_t image_index,
                        float64_t delta_time) {
  (void)image_index;
  (void)delta_time;
  // ... existing pipeline/material setup ...

  // Render existing UI geometry
  vkr_geometry_system_render(
      rf, &rf->geometry_system,
      vkr_geometry_system_get_default_plane2d(&rf->geometry_system), 1);

  // Render UI text elements
  for (uint32_t i = 0; i < state->text_count; i++) {
    VkrUiText *text = &state->texts[i];

    // Bind font atlas texture for this text
    VkrFont *font = text->resolved_font;
    if (font && font->atlas.id != 0) {
      vkr_shader_system_set_sampler(
          &rf->shader_system, "texture",
          vkr_texture_system_get_by_handle(&rf->texture_system, font->atlas));
    }

    vkr_ui_text_draw(text, state->pipeline);
  }
}
```

### Option B: Separate Text Rendering Pass

For better batching by font atlas:

```c
// Sort text by font atlas to minimize texture switches
// Render all text using same font consecutively
```

---

## Multi-Page Font Support

For fonts with multiple atlas pages (`VkrFont.page_count > 1`):

### Approach 1: Batch by Page

```c
// Group glyphs by page_id
// For each page:
//   1. Generate vertices only for glyphs on this page
//   2. Bind atlas_pages[page_id]
//   3. Draw

vkr_internal void vkr_ui_text_draw_multipage(VkrUiText *text,
                                              VkrPipelineHandle pipeline) {
  VkrFont *font = text->resolved_font;

  for (uint32_t page = 0; page < font->page_count; page++) {
    // Count glyphs on this page
    uint32_t page_glyph_count = 0;
    for (uint32_t i = 0; i < text->layout.glyphs.length; i++) {
      VkrTextGlyph *g = &text->layout.glyphs.data[i];
      // ... lookup font_glyph ...
      if (font_glyph->page_id == page) page_glyph_count++;
    }

    if (page_glyph_count == 0) continue;

    // Generate buffers for this page's glyphs
    // Bind atlas_pages.data[page]
    // Draw
  }
}
```

### Approach 2: Pre-sort Glyphs

Sort `VkrTextLayout.glyphs` by `page_id` during layout computation.

---

## Text Shader (Required)

Since `VkrTextVertex` has a different layout than `VkrVertex2d`, a dedicated text shader is **required**.

### Vertex Format: `VkrTextVertex`

```c
// From vkr_buffer.h
typedef struct VkrTextVertex {
  Vec2 position;  // location 0: Screen position
  Vec2 texcoord;  // location 1: Atlas UV
  Vec4 color;     // location 2: Per-vertex color
} VkrTextVertex;
```

### Text Shader: `default.text.slang`

```slang
// Uniform buffer for MVP matrices (set 0, binding 0)
struct UniformBufferObject
{
    column_major float4x4 view;
    column_major float4x4 projection;
};

// Push constants for per-object transform
struct PushConstantsObject
{
    column_major float4x4 model;
};

// Global UBO (set 0, binding 0)
[[vk::binding(0, 0)]]
ConstantBuffer<UniformBufferObject> ubo;

// Font atlas texture (set 1, binding 1)
[[vk::binding(1, 1)]]
Texture2D<float4> texture;

// Sampler (set 1, binding 2)
[[vk::binding(2, 1)]]
SamplerState sampler;

// Model transform via push constants
[[vk::push_constant]]
ConstantBuffer<PushConstantsObject> push_constants;

struct VertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 texcoord : TEXCOORD;
    [[vk::location(2)]] float4 color    : COLOR;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD;
    float4 color    : COLOR;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;

    float4 worldPos = mul(push_constants.model, float4(input.position, 0.0, 1.0));
    float4 viewPos = mul(ubo.view, worldPos);
    float4 clipPos = mul(ubo.projection, viewPos);

    output.position = clipPos;
    output.texcoord = input.texcoord;
    output.color = input.color;

    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    // Sample font atlas (RGBA for BMFont)
    float4 texColor = texture.Sample(sampler, input.texcoord);

    // Multiply by vertex color for per-character tinting
    return texColor * input.color;
}
```

### Shader Config: `default.text.shadercfg`

```
# Default Text Shader Configuration
# This shader is used for rendering text with per-vertex color
# using VkrTextVertex format (position, texcoord, color).

version=1.0
name=shader.default.text
renderpass=Renderpass.Builtin.UI
stages=vertex,fragment
stagefiles=assets/shaders/default.text.spv
use_instance=1
use_local=1

# ============================================================================
# Vertex Attributes (VkrTextVertex layout)
# ============================================================================
attribute=vec2,in_position   # Location 0, offset 0,  size 8 (screen pos)
attribute=vec2,in_texcoord   # Location 1, offset 8,  size 8 (atlas UV)
attribute=vec4,in_color      # Location 2, offset 16, size 16 (per-vertex color)
# Total stride: 32 bytes per vertex

# ============================================================================
# Uniforms
# ============================================================================

# Global uniforms (orthographic projection for UI)
uniform=mat4,0,projection    # Orthographic projection (2D)
uniform=mat4,0,view          # View matrix (typically identity for UI)

# Instance uniforms (font atlas texture)
uniform=samp,1,diffuse_texture  # Font atlas texture

# Local uniforms (per text element transform)
uniform=mat4,2,model         # 2D transform (position, scale, rotation)
```

### Alternative: Single-Channel SDF Atlas

For Signed Distance Field fonts:

```slang
[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float distance = texture.Sample(sampler, input.texcoord).r;
    float alpha = smoothstep(0.5 - 0.1, 0.5 + 0.1, distance);
    return float4(input.color.rgb, alpha * input.color.a);
}
```

---

## Testing Checklist

### Unit Tests

- [ ] Create text with empty content
- [ ] Create text with ASCII content
- [ ] Create text with Unicode content (emoji, CJK)
- [ ] Update text content (smaller → larger → smaller)
- [ ] Set position updates transform correctly
- [ ] Set color updates config
- [ ] Bounds calculation matches text width/height
- [ ] Buffer regeneration on content change
- [ ] Buffer reuse when capacity sufficient

### Visual Tests

- [ ] Single line text renders correctly
- [ ] Multi-line text with word wrap
- [ ] Text alignment (left, center, right)
- [ ] Text baseline alignment
- [ ] Correct glyph spacing (kerning)
- [ ] Alpha blending for anti-aliased glyphs
- [ ] Different font sizes
- [ ] Multiple text instances simultaneously

### Performance Tests

- [ ] Large text (1000+ characters) performance
- [ ] Frequent text updates (frame counter)
- [ ] Buffer reallocation stress test

---

## Implementation Order

1. **Shader Setup**: Create `default.text.slang` and `default.text.shadercfg`
2. **Phase 1**: `vkr_ui_text_create` and `vkr_ui_text_destroy`
3. **Phase 2**: Configuration setters
4. **Phase 3**: `vkr_ui_text_compute_layout` (layout integration)
5. **Phase 4**: `vkr_ui_text_generate_buffers` (core rendering logic)
6. **Phase 5**: `vkr_ui_text_prepare` and `vkr_ui_text_draw`
7. **Integration**: Add to `vkr_ui_system.c` render loop (load text pipeline, render text)
8. **Testing**: Visual verification with sample text

---

## Files to Modify/Create

| File | Changes |
|------|---------|
| `lib/src/renderer/resources/vkr_ui_text.c` | Full implementation |
| `assets/shaders/default.text.slang` | **NEW**: Text shader with VkrTextVertex layout |
| `assets/shaders/default.text.shadercfg` | **NEW**: Shader configuration |
| `lib/src/renderer/passes/vkr_pass_ui.c` | Add text rendering in `on_render` |
| `lib/src/renderer/systems/vkr_ui_system.h` | Add text storage to `VkrUiSystem` |
| `lib/src/renderer/renderer_frontend.h` | (optional) Add UI text manager |

---

## Error Handling

| Error Condition | Response |
|-----------------|----------|
| No font available | Return `VKR_RENDERER_ERROR_RESOURCE_NOT_FOUND` |
| Buffer creation failed | Return `false`, log error |
| Empty text content | Valid - render nothing |
| Invalid font handle | Fall back to default font |
| Missing glyph | Skip glyph (don't crash) |

---

## Future Enhancements

1. **Rich Text Support**: Use `VkrRichText` with spans for mixed styles per character
2. **Text Effects**: Outline, shadow, glow (shader-based)
3. **SDF Fonts**: Signed Distance Field for scalable text
4. **Text Caching**: Cache layout for static text
5. **Batch Rendering**: Combine multiple texts into single draw call
6. **Animation**: Per-character transforms for effects
7. **Color Tint Optimization**: For uniform color text, pass color via instance UBO instead of per-vertex to avoid buffer regeneration on color change
8. **Instanced Text**: Use instancing for repeated text patterns (numbers, labels)
