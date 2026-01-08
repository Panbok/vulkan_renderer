#include "renderer/resources/ui/vkr_ui_text.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_text.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_buffer.h"

#define VKR_UI_TEXT_QUAD_COUNT 4
#define VKR_UI_TEXT_INDEX_COUNT 6
#define VKR_UI_TEXT_VERTEX_GROWTH_COUNT 64
#define VKR_UI_TEXT_INDEX_GROWTH_COUNT 96

#define VKR_UI_TEXT_BUFFER_RETIRE_FRAMES 3

vkr_internal void vkr_ui_text_collect_retired_buffers(VkrUiText *text,
                                                      uint64_t current_frame) {
  assert_log(text != NULL, "Text is NULL");

  for (uint32_t i = 0; i < VKR_UI_TEXT_MAX_RETIRED_BUFFER_SETS; ++i) {
    VkrUiTextRetiredBufferSet *slot = &text->retired_buffers[i];
    if (slot->vertex_buffer.handle == NULL &&
        slot->index_buffer.handle == NULL) {
      continue;
    }

    if (current_frame < slot->retire_after_frame) {
      continue;
    }

    if (slot->vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(text->renderer, &slot->vertex_buffer);
    }
    if (slot->index_buffer.handle) {
      vkr_index_buffer_destroy(text->renderer, &slot->index_buffer);
    }

    MemZero(slot, sizeof(*slot));
  }
}

vkr_internal void vkr_ui_text_retire_buffers(VkrUiText *text,
                                             VkrVertexBuffer vertex_buffer,
                                             VkrIndexBuffer index_buffer,
                                             uint64_t current_frame) {
  assert_log(text != NULL, "Text is NULL");
  assert_log(vertex_buffer.handle != NULL || index_buffer.handle != NULL,
             "Vertex or index buffer is NULL");

  uint64_t retire_after_frame =
      current_frame + VKR_UI_TEXT_BUFFER_RETIRE_FRAMES;

  for (uint32_t i = 0; i < VKR_UI_TEXT_MAX_RETIRED_BUFFER_SETS; ++i) {
    VkrUiTextRetiredBufferSet *slot = &text->retired_buffers[i];
    if (slot->vertex_buffer.handle != NULL ||
        slot->index_buffer.handle != NULL) {
      continue;
    }

    slot->vertex_buffer = vertex_buffer;
    slot->index_buffer = index_buffer;
    slot->retire_after_frame = retire_after_frame;
    return;
  }

  // Edge case: too many pending resizes without enough frames progressing to
  // retire old buffers. Fall back to a full GPU idle wait to safely destroy.
  vkr_renderer_wait_idle(text->renderer);
  if (vertex_buffer.handle) {
    vkr_vertex_buffer_destroy(text->renderer, &vertex_buffer);
  }
  if (index_buffer.handle) {
    vkr_index_buffer_destroy(text->renderer, &index_buffer);
  }
}

vkr_internal bool8_t vkr_ui_text_codepoint_key(char *buffer,
                                               uint64_t buffer_size,
                                               uint32_t codepoint) {
  if (buffer == NULL || buffer_size == 0) {
    return false_v;
  }

  int32_t written = snprintf(buffer, buffer_size, "%u", codepoint);
  if (written <= 0 || (uint64_t)written >= buffer_size) {
    buffer[0] = '\0';
    return false_v;
  }

  return true_v;
}

vkr_internal const VkrFontGlyph *vkr_ui_text_find_glyph(const VkrFont *font,
                                                        uint32_t codepoint,
                                                        uint32_t *out_index) {
  if (font == NULL || font->glyphs.data == NULL) {
    return NULL;
  }

  if (font->glyph_indices.entries != NULL && font->glyph_indices.size > 0) {
    char key[16];
    if (vkr_ui_text_codepoint_key(key, sizeof(key), codepoint)) {
      uint32_t *index = vkr_hash_table_get_uint32_t(&font->glyph_indices, key);
      if (index && *index < font->glyphs.length) {
        if (out_index) {
          *out_index = *index;
        }
        return &font->glyphs.data[*index];
      }
    }
  }

  for (uint64_t i = 0; i < font->glyphs.length; ++i) {
    if (font->glyphs.data[i].codepoint == codepoint) {
      if (out_index) {
        *out_index = (uint32_t)i;
      }
      return &font->glyphs.data[i];
    }
  }

  return NULL;
}

vkr_internal String8 vkr_ui_text_copy_content(VkrAllocator *allocator,
                                              String8 content) {
  if (allocator == NULL || content.str == NULL || content.length == 0) {
    return (String8){0};
  }

  return string8_duplicate(allocator, &content);
}

vkr_internal void vkr_ui_text_compute_layout(VkrUiText *text) {
  if (!text || !text->resolved_font) {
    return;
  }

  if (text->layout.allocator != NULL) {
    vkr_text_layout_destroy(&text->layout);
  }

  if (text->content.str == NULL || text->content.length == 0) {
    text->layout = (VkrTextLayout){0};
    text->bounds = (VkrTextBounds){0};
    text->layout_dirty = false_v;
    return;
  }

  float32_t font_size = text->config.font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)text->resolved_font->size;
  }

  VkrTextStyle style =
      vkr_text_style_new(text->config.font, font_size, text->config.color);
  style.letter_spacing = text->config.letter_spacing;
  style = vkr_text_style_with_font_data(&style, text->resolved_font);

  VkrText text_for_layout = vkr_text_from_view(text->content, &style);
  text->layout = vkr_text_layout_compute(text->allocator, &text_for_layout,
                                         &text->config.layout);

  text->bounds.size = text->layout.bounds;

  float32_t scale = font_size / (float32_t)text->resolved_font->size;
  text->bounds.ascent = (float32_t)text->resolved_font->ascent * scale;
  text->bounds.descent = (float32_t)text->resolved_font->descent * scale;

  text->layout_dirty = false_v;
}

vkr_internal bool8_t vkr_ui_text_generate_buffers(VkrUiText *text) {
  if (!text || !text->resolved_font) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)text->renderer;
  uint64_t current_frame = rf ? rf->frame_number : 0;
  vkr_ui_text_collect_retired_buffers(text, current_frame);

  if (text->layout.glyphs.length > UINT32_MAX) {
    log_error("Glyph count exceeds maximum supported: %llu",
              text->layout.glyphs.length);
    return false_v;
  }

  uint32_t glyph_count = (uint32_t)text->layout.glyphs.length;
  if (glyph_count == 0) {
    text->render.quad_count = 0;
    text->buffers_dirty = false_v;
    return true_v;
  }

  if (glyph_count > UINT32_MAX / VKR_UI_TEXT_QUAD_COUNT) {
    log_error("Glyph count too large for vertex buffer: %u", glyph_count);
    return false_v;
  }

  if (glyph_count > UINT32_MAX / VKR_UI_TEXT_INDEX_COUNT) {
    log_error("Glyph count too large for index buffer: %u", glyph_count);
    return false_v;
  }

  uint32_t required_vertex_count = glyph_count * VKR_UI_TEXT_QUAD_COUNT;
  uint32_t required_index_count = glyph_count * VKR_UI_TEXT_INDEX_COUNT;

  bool8_t has_buffers = text->render.vertex_buffer.handle != NULL &&
                        text->render.index_buffer.handle != NULL;
  bool8_t need_realloc = !has_buffers ||
                         required_vertex_count > text->render.vertex_capacity ||
                         required_index_count > text->render.index_capacity;

  uint32_t alloc_vertex_count = required_vertex_count;
  uint32_t alloc_index_count = required_index_count;
  if (need_realloc) {
    alloc_vertex_count =
        required_vertex_count + VKR_UI_TEXT_VERTEX_GROWTH_COUNT;
    alloc_index_count = required_index_count + VKR_UI_TEXT_INDEX_GROWTH_COUNT;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(text->allocator);
  bool8_t use_scope = scope.allocator != NULL;

  VkrTextVertex *vertices = vkr_allocator_alloc(
      text->allocator, sizeof(VkrTextVertex) * alloc_vertex_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices =
      vkr_allocator_alloc(text->allocator, sizeof(uint32_t) * alloc_index_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!vertices || !indices) {
    if (use_scope) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      if (vertices) {
        vkr_allocator_free(text->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (indices) {
        vkr_allocator_free(text->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    }
    return false_v;
  }

  MemZero(vertices, sizeof(VkrTextVertex) * alloc_vertex_count);
  MemZero(indices, sizeof(uint32_t) * alloc_index_count);

  float32_t atlas_w = (float32_t)text->resolved_font->atlas_size_x;
  float32_t atlas_h = (float32_t)text->resolved_font->atlas_size_y;
  float32_t inv_atlas_w = 1.0f / atlas_w;
  float32_t inv_atlas_h = 1.0f / atlas_h;

  float32_t font_size = text->config.font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)text->resolved_font->size;
  }
  float32_t scale = font_size / (float32_t)text->resolved_font->size;

  uint32_t vertex_idx = 0;
  uint32_t index_idx = 0;
  Vec4 color = text->config.color;
  // Flip layout Y (top-down) into UI screen space without changing winding.
  float32_t layout_bottom =
      (text->layout.baseline.y - text->bounds.ascent) + text->bounds.size.y;

  for (uint32_t i = 0; i < glyph_count; i++) {
    VkrTextGlyph *layout_glyph = &text->layout.glyphs.data[i];
    uint32_t glyph_index = 0;
    const VkrFontGlyph *font_glyph = vkr_ui_text_find_glyph(
        text->resolved_font, layout_glyph->codepoint, &glyph_index);
    if (!font_glyph) {
      continue;
    }

    float32_t x0 =
        layout_glyph->position.x + (float32_t)font_glyph->x_offset * scale;
    float32_t line_top = layout_glyph->position.y - text->bounds.ascent;
    float32_t y0 = line_top + (float32_t)font_glyph->y_offset * scale;
    float32_t glyph_w = (float32_t)font_glyph->width * scale;
    float32_t glyph_h = (float32_t)font_glyph->height * scale;

    if (text->resolved_font->type == VKR_FONT_TYPE_MTSDF &&
        text->resolved_font->mtsdf_glyphs.data &&
        glyph_index < text->resolved_font->mtsdf_glyphs.length) {
      const VkrMtsdfGlyph *mtsdf_glyph =
          &text->resolved_font->mtsdf_glyphs.data[glyph_index];
      if (mtsdf_glyph->has_geometry) {
        glyph_w =
            (mtsdf_glyph->plane_right - mtsdf_glyph->plane_left) * font_size;
        glyph_h =
            (mtsdf_glyph->plane_top - mtsdf_glyph->plane_bottom) * font_size;
      } else {
        glyph_w = 0.0f;
        glyph_h = 0.0f;
      }
    }

    float32_t x1 = x0 + glyph_w;
    float32_t y1 = y0 + glyph_h;
    float32_t top_y = layout_bottom - y1;
    float32_t bottom_y = layout_bottom - y0;

    float32_t u0_raw = (float32_t)font_glyph->x * inv_atlas_w;
    float32_t u1_raw =
        (float32_t)(font_glyph->x + font_glyph->width) * inv_atlas_w;
    float32_t v0_raw =
        1.0f - (float32_t)(font_glyph->y + font_glyph->height) * inv_atlas_h;
    float32_t v1_raw = 1.0f - (float32_t)font_glyph->y * inv_atlas_h;

    float32_t inset_px = text->config.uv_inset_px;
    if (inset_px < 0.0f) {
      inset_px = 0.0f;
    }
    float32_t u_inset = inset_px * inv_atlas_w;
    float32_t v_inset = inset_px * inv_atlas_h;
    if (font_glyph->width <= 1) {
      u_inset = 0.0f;
    }
    if (font_glyph->height <= 1) {
      v_inset = 0.0f;
    }

    float32_t u0 = u0_raw + u_inset;
    float32_t u1 = u1_raw - u_inset;
    float32_t v0 = v0_raw + v_inset;
    float32_t v1 = v1_raw - v_inset;
    if (u1 <= u0) {
      u0 = u0_raw;
      u1 = u1_raw;
    }
    if (v1 <= v0) {
      v0 = v0_raw;
      v1 = v1_raw;
    }

    uint32_t base_vertex = vertex_idx;

    vertices[vertex_idx].position = vec2_new(x0, top_y);
    vertices[vertex_idx].texcoord = vec2_new(u0, v0);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x1, bottom_y);
    vertices[vertex_idx].texcoord = vec2_new(u1, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x0, bottom_y);
    vertices[vertex_idx].texcoord = vec2_new(u0, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x1, top_y);
    vertices[vertex_idx].texcoord = vec2_new(u1, v0);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    indices[index_idx++] = base_vertex + 2;
    indices[index_idx++] = base_vertex + 1;
    indices[index_idx++] = base_vertex + 0;
    indices[index_idx++] = base_vertex + 3;
    indices[index_idx++] = base_vertex + 0;
    indices[index_idx++] = base_vertex + 1;
  }

  uint32_t vertex_count = vertex_idx;
  uint32_t index_count = index_idx;
  text->render.quad_count = vertex_count / VKR_UI_TEXT_QUAD_COUNT;

  if (vertex_count == 0 || index_count == 0) {
    text->buffers_dirty = false_v;
    if (use_scope) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      vkr_allocator_free(text->allocator, vertices,
                         sizeof(VkrTextVertex) * alloc_vertex_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_allocator_free(text->allocator, indices,
                         sizeof(uint32_t) * alloc_index_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return true_v;
  }

  VkrRendererError buffer_err = VKR_RENDERER_ERROR_NONE;
  if (need_realloc) {
    // Use dynamic buffers for UI text (host-visible, no GPU sync on update).
    // Create new buffers first; old buffers are retired after a successful
    // swap.
    VkrVertexBuffer new_vertex_buffer = vkr_vertex_buffer_create_dynamic(
        text->renderer, vertices, sizeof(VkrTextVertex), alloc_vertex_count,
        VKR_VERTEX_INPUT_RATE_VERTEX, string8_lit("ui_text_vertices"),
        &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }

    VkrIndexBuffer new_index_buffer = vkr_index_buffer_create_dynamic(
        text->renderer, indices, VKR_INDEX_TYPE_UINT32, alloc_index_count,
        string8_lit("ui_text_indices"), &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      vkr_vertex_buffer_destroy(text->renderer, &new_vertex_buffer);
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }

    vkr_ui_text_retire_buffers(text, text->render.vertex_buffer,
                               text->render.index_buffer, current_frame);

    text->render.vertex_buffer = new_vertex_buffer;
    text->render.index_buffer = new_index_buffer;
    text->render.vertex_capacity = alloc_vertex_count;
    text->render.index_capacity = alloc_index_count;
  } else {
    buffer_err = vkr_vertex_buffer_update(
        text->renderer, &text->render.vertex_buffer, vertices, 0, vertex_count);
    if (buffer_err == VKR_RENDERER_ERROR_NONE) {
      buffer_err = vkr_index_buffer_update(
          text->renderer, &text->render.index_buffer, indices, 0, index_count);
    }
    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }
  }

  if (use_scope) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    vkr_allocator_free(text->allocator, vertices,
                       sizeof(VkrTextVertex) * alloc_vertex_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(text->allocator, indices,
                       sizeof(uint32_t) * alloc_index_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  text->buffers_dirty = false_v;
  return true_v;
}

bool8_t vkr_ui_text_create(VkrRendererFrontendHandle renderer,
                           VkrAllocator *allocator, VkrFontSystem *font_system,
                           VkrPipelineHandle pipeline, String8 content,
                           const VkrUiTextConfig *config, VkrUiText *out_text,
                           VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(font_system != NULL, "Font system is NULL");
  assert_log(out_text != NULL, "Output text is NULL");

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }

  MemZero(out_text, sizeof(VkrUiText));

  out_text->renderer = renderer;
  out_text->font_system = font_system;
  out_text->allocator = allocator;
  out_text->content = vkr_ui_text_copy_content(allocator, content);
  out_text->config = config ? *config : VKR_UI_TEXT_CONFIG_DEFAULT;
  out_text->transform = vkr_transform_identity();
  out_text->layout_dirty = true_v;
  out_text->buffers_dirty = true_v;
  out_text->render.pipeline = pipeline;
  out_text->render.instance_state = (VkrRendererInstanceStateHandle){0};

  if (out_text->config.font.id != 0) {
    out_text->resolved_font =
        vkr_font_system_get_by_handle(font_system, out_text->config.font);
  }
  if (!out_text->resolved_font) {
    out_text->resolved_font =
        vkr_font_system_get_default_bitmap_font(font_system);
  }

  if (!out_text->resolved_font) {
    log_error("No font available for UI text");
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    }
    vkr_ui_text_destroy(out_text);
    return false_v;
  }

  VkrRendererError text_ls_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &out_text->renderer->pipeline_registry, pipeline,
          &out_text->render.instance_state, &text_ls_err)) {
    vkr_ui_text_destroy(out_text);
    String8 err_str = vkr_renderer_get_error_string(text_ls_err);
    if (out_error) {
      *out_error = text_ls_err;
    }
    log_error("Failed to acquire instance state for text pipeline: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  return true_v;
}

void vkr_ui_text_destroy(VkrUiText *text) {
  if (!text) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)text->renderer;
  if (rf) {
    vkr_ui_text_collect_retired_buffers(text, rf->frame_number);
  }

  if (text->render.instance_state.id != 0 && text->render.pipeline.id != 0) {
    vkr_pipeline_registry_release_instance_state(
        &text->renderer->pipeline_registry, text->render.pipeline,
        text->render.instance_state, &(VkrRendererError){0});
  }

  if (text->render.vertex_buffer.handle) {
    vkr_vertex_buffer_destroy(text->renderer, &text->render.vertex_buffer);
  }
  if (text->render.index_buffer.handle) {
    vkr_index_buffer_destroy(text->renderer, &text->render.index_buffer);
  }

  for (uint32_t i = 0; i < VKR_UI_TEXT_MAX_RETIRED_BUFFER_SETS; ++i) {
    VkrUiTextRetiredBufferSet *slot = &text->retired_buffers[i];
    if (slot->vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(text->renderer, &slot->vertex_buffer);
    }
    if (slot->index_buffer.handle) {
      vkr_index_buffer_destroy(text->renderer, &slot->index_buffer);
    }
  }

  vkr_text_layout_destroy(&text->layout);

  if (text->content.str && text->allocator) {
    vkr_allocator_free(text->allocator, (void *)text->content.str,
                       text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  MemZero(text, sizeof(VkrUiText));
}

bool8_t vkr_ui_text_set_content(VkrUiText *text, String8 content) {
  if (!text || !text->allocator) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)text->renderer;
  if (rf) {
    vkr_ui_text_collect_retired_buffers(text, rf->frame_number);
  }

  if (text->content.str) {
    vkr_allocator_free(text->allocator, (void *)text->content.str,
                       text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  text->content = vkr_ui_text_copy_content(text->allocator, content);
  text->layout_dirty = true_v;
  text->buffers_dirty = true_v;
  return true_v;
}

void vkr_ui_text_set_config(VkrUiText *text, const VkrUiTextConfig *config) {
  if (!text || !config) {
    return;
  }

  bool8_t font_changed =
      text->config.font.id != config->font.id ||
      text->config.font.generation != config->font.generation;

  bool8_t layout_changed =
      text->config.font_size != config->font_size ||
      text->config.letter_spacing != config->letter_spacing ||
      text->config.layout.max_width != config->layout.max_width ||
      text->config.layout.max_height != config->layout.max_height ||
      text->config.layout.word_wrap != config->layout.word_wrap ||
      text->config.layout.clip != config->layout.clip ||
      text->config.layout.anchor.horizontal !=
          config->layout.anchor.horizontal ||
      text->config.layout.anchor.vertical != config->layout.anchor.vertical;

  bool8_t color_changed =
      !vec4_equal(text->config.color, config->color, VKR_FLOAT_EPSILON);

  text->config = *config;

  if (font_changed) {
    if (config->font.id != 0) {
      text->resolved_font =
          vkr_font_system_get_by_handle(text->font_system, config->font);
    }
    if (!text->resolved_font) {
      text->resolved_font =
          vkr_font_system_get_default_bitmap_font(text->font_system);
    }
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (layout_changed) {
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (color_changed) {
    text->buffers_dirty = true_v;
  }
}

void vkr_ui_text_set_position(VkrUiText *text, Vec2 position) {
  if (!text) {
    return;
  }
  vkr_transform_set_position(&text->transform,
                             vec3_new(position.x, position.y, 0.0f));
}

void vkr_ui_text_set_color(VkrUiText *text, Vec4 color) {
  if (!text) {
    return;
  }

  if (vec4_equal(text->config.color, color, VKR_FLOAT_EPSILON)) {
    return;
  }

  text->config.color = color;
  text->buffers_dirty = true_v;
}

VkrTextBounds vkr_ui_text_get_bounds(VkrUiText *text) {
  if (!text) {
    return (VkrTextBounds){0};
  }

  if (text->layout_dirty) {
    vkr_ui_text_compute_layout(text);
  }

  return text->bounds;
}

bool8_t vkr_ui_text_prepare(VkrUiText *text) {
  if (!text) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)text->renderer;
  if (rf) {
    vkr_ui_text_collect_retired_buffers(text, rf->frame_number);
  }

  if (text->layout_dirty) {
    vkr_ui_text_compute_layout(text);
  }

  if (text->buffers_dirty) {
    if (!vkr_ui_text_generate_buffers(text)) {
      log_error("Failed to generate UI text buffers");
      return false_v;
    }
  }

  return (text->render.quad_count > 0 &&
          text->render.vertex_buffer.handle != NULL &&
          text->render.index_buffer.handle != NULL);
}

void vkr_ui_text_draw(VkrUiText *text) {
  assert_log(text != NULL, "Text is NULL");

  if (!vkr_ui_text_prepare(text)) {
    return;
  }

  if (text->render.quad_count == 0) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)text->renderer;

  const char *text_shader = "shader.default.text";
  if (!vkr_shader_system_use(&rf->shader_system, text_shader)) {
    log_warn("Failed to bind text shader; skipping UI text");
    return;
  }

  VkrPipelineHandle current_text_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_text_pipeline.id != text->render.pipeline.id ||
      current_text_pipeline.generation != text->render.pipeline.generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_bind_pipeline(
            &rf->pipeline_registry, text->render.pipeline, &bind_err)) {
      String8 err_str = vkr_renderer_get_error_string(bind_err);
      log_error("Failed to bind text pipeline: %s", string8_cstr(&err_str));
      return;
    }
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_UI);

  VkrFont *font = text->resolved_font;
  if (!font) {
    font = vkr_font_system_get_by_handle(&rf->font_system, text->config.font);
  }

  VkrTexture *atlas_texture = NULL;
  if (font && font->atlas.id != 0) {
    atlas_texture =
        vkr_texture_system_get_by_handle(&rf->texture_system, font->atlas);
  }
  if (!atlas_texture) {
    atlas_texture = vkr_texture_system_get_default(&rf->texture_system);
  }

  if (atlas_texture) {
    vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                  atlas_texture->handle);
  }

  rf->draw_state.instance_state = text->render.instance_state;
  vkr_shader_system_bind_instance(&rf->shader_system,
                                  text->render.instance_state.id);

  VkrVertexBufferBinding vbb = {
      .buffer = text->render.vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(text->renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = text->render.index_buffer.handle,
      .type = VKR_INDEX_TYPE_UINT32,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(text->renderer, &ibb);

  Mat4 model = vkr_transform_get_world(&text->transform);
  vkr_material_system_apply_local(&rf->material_system,
                                  &(VkrLocalMaterialState){.model = model});

  Vec4 diffuse_color = {1.0f, 1.0f, 1.0f, 1.0f};
  vkr_shader_system_uniform_set(&rf->shader_system, "diffuse_color",
                                &diffuse_color);

  float32_t screen_px_range = 0.0f;
  float32_t font_mode = 0.0f;
  if (font && font->type == VKR_FONT_TYPE_MTSDF) {
    float32_t render_size = text->config.font_size;
    if (render_size <= 0.0f) {
      render_size = (float32_t)font->size;
    }
    if (font->em_size > 0.0f) {
      font_mode = 1.0f;
      screen_px_range =
          font->sdf_distance_range * (render_size / font->em_size);
      if (screen_px_range < 1.0f) {
        screen_px_range = 1.0f;
      }
      if (screen_px_range > 4.0f) {
        screen_px_range = 4.0f;
      }
    }
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "screen_px_range",
                                &screen_px_range);
  vkr_shader_system_uniform_set(&rf->shader_system, "font_mode", &font_mode);

  if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
    return;
  }

  uint32_t index_count = text->render.quad_count * 6;
  vkr_renderer_draw_indexed(text->renderer, index_count, 1, 0, 0, 0);

  text->render.last_frame_rendered = rf->frame_number;
}
