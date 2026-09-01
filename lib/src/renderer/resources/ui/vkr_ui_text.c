#include "renderer/resources/ui/vkr_ui_text.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_text.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_color_transfer.h"

#include <math.h>

#define VKR_UI_TEXT_QUAD_COUNT 4
#define VKR_UI_TEXT_INDEX_COUNT 6
#define VKR_UI_TEXT_VERTEX_GROWTH_COUNT 64
#define VKR_UI_TEXT_INDEX_GROWTH_COUNT 96

vkr_internal String8 vkr_ui_text_copy_content(VkrAllocator *allocator,
                                              String8 content) {
  if (allocator == NULL || content.str == NULL || content.length == 0) {
    return (String8){0};
  }

  return string8_duplicate(allocator, &content);
}

vkr_internal VkrFont *vkr_ui_text_resolve_font(VkrFontSystem *font_system,
                                               VkrFontHandle handle) {
  VkrFont *font = vkr_font_system_get_by_handle(font_system, handle);
  if (!font) {
    font = vkr_font_system_get_default_mtsdf_font(font_system);
  }
  if (!font) {
    font = vkr_font_system_get_default_bitmap_font(font_system);
  }
  return font;
}

vkr_internal float32_t vkr_ui_text_device_font_size(const VkrUiText *text) {
  float32_t authored_size = text->config.font_size;
  if (authored_size <= 0.0f) {
    authored_size = (float32_t)text->resolved_font->size;
  }
  return authored_size * text->content_scale;
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

  const float32_t font_size = vkr_ui_text_device_font_size(text);

  VkrTextStyle style =
      vkr_text_style_new(text->config.font, font_size, text->config.color);
  style.letter_spacing = text->config.letter_spacing * text->content_scale;
  style = vkr_text_style_with_font_data(&style, text->resolved_font);

  VkrText text_for_layout = vkr_text_from_view(text->content, &style);
  VkrTextLayoutOptions layout = text->config.layout;
  if (layout.max_width > 0.0f) {
    layout.max_width *= text->content_scale;
  }
  if (layout.max_height > 0.0f) {
    layout.max_height *= text->content_scale;
  }
  text->layout =
      vkr_text_layout_compute(text->allocator, &text_for_layout, &layout);

  text->bounds.size = text->layout.bounds;

  const bool8_t cooked = text->resolved_font->glyphs_by_id.data != NULL;
  if (cooked) {
    text->bounds.ascent = text->resolved_font->em_ascender * font_size;
    text->bounds.descent = -text->resolved_font->em_descender * font_size;
  } else {
    const float32_t scale = font_size / (float32_t)text->resolved_font->size;
    text->bounds.ascent = (float32_t)text->resolved_font->ascent * scale;
    text->bounds.descent = (float32_t)text->resolved_font->descent * scale;
  }

  text->layout_dirty = false_v;
}

vkr_internal bool8_t vkr_ui_text_generate_geometry(VkrUiText *text) {
  if (!text || !text->resolved_font) {
    return false_v;
  }

  if (text->layout.glyphs.length > UINT32_MAX) {
    log_error("Glyph count exceeds maximum supported: %llu",
              text->layout.glyphs.length);
    return false_v;
  }

  uint32_t glyph_count = (uint32_t)text->layout.glyphs.length;
  if (glyph_count == 0) {
    text->geometry.vertex_count = 0;
    text->geometry.index_count = 0;
    text->geometry.revision++;
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

  if (required_vertex_count > text->geometry.vertex_capacity) {
    const uint32_t growth = Min(VKR_UI_TEXT_VERTEX_GROWTH_COUNT,
                                UINT32_MAX - required_vertex_count);
    const uint32_t capacity = required_vertex_count + growth;
    VkrTextVertex *vertices = vkr_allocator_realloc(
        text->allocator, text->geometry.vertices,
        (uint64_t)text->geometry.vertex_capacity * sizeof(VkrTextVertex),
        (uint64_t)capacity * sizeof(VkrTextVertex),
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!vertices)
      return false_v;
    text->geometry.vertices = vertices;
    text->geometry.vertex_capacity = capacity;
  }
  if (required_index_count > text->geometry.index_capacity) {
    const uint32_t growth =
        Min(VKR_UI_TEXT_INDEX_GROWTH_COUNT, UINT32_MAX - required_index_count);
    const uint32_t capacity = required_index_count + growth;
    uint32_t *indices = vkr_allocator_realloc(
        text->allocator, text->geometry.indices,
        (uint64_t)text->geometry.index_capacity * sizeof(uint32_t),
        (uint64_t)capacity * sizeof(uint32_t), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!indices)
      return false_v;
    text->geometry.indices = indices;
    text->geometry.index_capacity = capacity;
  }

  VkrTextVertex *vertices = text->geometry.vertices;
  uint32_t *indices = text->geometry.indices;
  MemZero(vertices, sizeof(VkrTextVertex) * required_vertex_count);
  MemZero(indices, sizeof(uint32_t) * required_index_count);

  float32_t atlas_w = (float32_t)text->resolved_font->atlas_size_x;
  float32_t atlas_h = (float32_t)text->resolved_font->atlas_size_y;
  float32_t inv_atlas_w = 1.0f / atlas_w;
  float32_t inv_atlas_h = 1.0f / atlas_h;

  const float32_t font_size = vkr_ui_text_device_font_size(text);
  const bool8_t cooked = text->resolved_font->glyphs_by_id.data != NULL;
  const float32_t scale =
      cooked ? font_size : font_size / (float32_t)text->resolved_font->size;

  uint32_t vertex_idx = 0;
  uint32_t index_idx = 0;
  Vec4 color = text->linear_color;
  const bool8_t exact_mtsdf_bounds =
      text->resolved_font->type == VKR_FONT_TYPE_MTSDF;
  const float32_t inset_px =
      vkr_text_uv_inset(text->config.uv_inset_px, exact_mtsdf_bounds);
  // Flip layout Y (top-down) into UI screen space without changing winding.
  float32_t layout_bottom =
      (text->layout.baseline.y - text->bounds.ascent) + text->bounds.size.y;

  for (uint32_t i = 0; i < glyph_count; i++) {
    VkrTextGlyph *layout_glyph = &text->layout.glyphs.data[i];
    if (layout_glyph->glyph_index == VKR_INVALID_ID) {
      continue;
    }
    const VkrFontGlyph *font_glyph = NULL;
    const VkrFontGlyphId *cooked_glyph = NULL;
    const VkrMtsdfGlyph *mtsdf_glyph = NULL;
    float32_t x0 = 0.0f;
    float32_t y0 = 0.0f;
    float32_t glyph_w = 0.0f;
    float32_t glyph_h = 0.0f;
    if (cooked) {
      if (layout_glyph->glyph_index >=
          text->resolved_font->glyphs_by_id.length) {
        continue;
      }
      cooked_glyph =
          &text->resolved_font->glyphs_by_id.data[layout_glyph->glyph_index];
      if (!cooked_glyph->has_geometry) {
        continue;
      }
      x0 = layout_glyph->position.x + cooked_glyph->plane_left * font_size;
      y0 = layout_glyph->position.y - cooked_glyph->plane_top * font_size;
      glyph_w =
          (cooked_glyph->plane_right - cooked_glyph->plane_left) * font_size;
      glyph_h =
          (cooked_glyph->plane_top - cooked_glyph->plane_bottom) * font_size;
    } else {
      if (layout_glyph->glyph_index >= text->resolved_font->glyphs.length) {
        continue;
      }
      font_glyph = &text->resolved_font->glyphs.data[layout_glyph->glyph_index];
      x0 = layout_glyph->position.x + (float32_t)font_glyph->x_offset * scale;
      const float32_t line_top = layout_glyph->position.y - text->bounds.ascent;
      y0 = line_top + (float32_t)font_glyph->y_offset * scale;
      glyph_w = (float32_t)font_glyph->width * scale;
      glyph_h = (float32_t)font_glyph->height * scale;
      if (text->resolved_font->type == VKR_FONT_TYPE_MTSDF &&
          text->resolved_font->mtsdf_glyphs.data &&
          layout_glyph->glyph_index <
              text->resolved_font->mtsdf_glyphs.length) {
        mtsdf_glyph =
            &text->resolved_font->mtsdf_glyphs.data[layout_glyph->glyph_index];
        if (!mtsdf_glyph->has_geometry) {
          continue;
        }
        glyph_w =
            (mtsdf_glyph->plane_right - mtsdf_glyph->plane_left) * font_size;
        glyph_h =
            (mtsdf_glyph->plane_top - mtsdf_glyph->plane_bottom) * font_size;
      }
    }

    float32_t x1 = x0 + glyph_w;
    float32_t y1 = y0 + glyph_h;
    float32_t top_y = layout_bottom - y1;
    float32_t bottom_y = layout_bottom - y0;

    float32_t u0_raw = 0.0f;
    float32_t u1_raw = 0.0f;
    float32_t v0_raw = 0.0f;
    float32_t v1_raw = 0.0f;
    float32_t atlas_glyph_w = 0.0f;
    float32_t atlas_glyph_h = 0.0f;
    if (cooked_glyph) {
      u0_raw = cooked_glyph->uv_left;
      u1_raw = cooked_glyph->uv_right;
      v0_raw = cooked_glyph->uv_bottom;
      v1_raw = cooked_glyph->uv_top;
      atlas_glyph_w = (u1_raw - u0_raw) * atlas_w;
      atlas_glyph_h = (v1_raw - v0_raw) * atlas_h;
    } else if (mtsdf_glyph) {
      u0_raw = mtsdf_glyph->uv_left;
      u1_raw = mtsdf_glyph->uv_right;
      v0_raw = mtsdf_glyph->uv_bottom;
      v1_raw = mtsdf_glyph->uv_top;
      atlas_glyph_w = mtsdf_glyph->atlas_right - mtsdf_glyph->atlas_left;
      atlas_glyph_h = mtsdf_glyph->atlas_top - mtsdf_glyph->atlas_bottom;
    } else {
      u0_raw = (float32_t)font_glyph->x * inv_atlas_w;
      u1_raw = (float32_t)(font_glyph->x + font_glyph->width) * inv_atlas_w;
      v0_raw =
          1.0f - (float32_t)(font_glyph->y + font_glyph->height) * inv_atlas_h;
      v1_raw = 1.0f - (float32_t)font_glyph->y * inv_atlas_h;
      atlas_glyph_w = (float32_t)font_glyph->width;
      atlas_glyph_h = (float32_t)font_glyph->height;
    }

    float32_t u_inset = inset_px * inv_atlas_w;
    float32_t v_inset = inset_px * inv_atlas_h;
    if (atlas_glyph_w <= 1.0f) {
      u_inset = 0.0f;
    }
    if (atlas_glyph_h <= 1.0f) {
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
  if (vertex_count == 0 || index_count == 0) {
    text->geometry.vertex_count = 0;
    text->geometry.index_count = 0;
    text->geometry.revision++;
    text->buffers_dirty = false_v;
    return true_v;
  }

  text->geometry.vertex_count = vertex_count;
  text->geometry.index_count = index_count;
  text->geometry.revision++;
  text->buffers_dirty = false_v;
  return true_v;
}

bool8_t vkr_ui_text_create(VkrAllocator *allocator, VkrFontSystem *font_system,
                           String8 content, const VkrUiTextConfig *config,
                           VkrUiText *out_text, VkrRendererError *out_error) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(font_system != NULL, "Font system is NULL");
  assert_log(out_text != NULL, "Output text is NULL");

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }

  MemZero(out_text, sizeof(VkrUiText));

  out_text->font_system = font_system;
  out_text->allocator = allocator;
  out_text->content = vkr_ui_text_copy_content(allocator, content);
  out_text->config = config ? *config : VKR_UI_TEXT_CONFIG_DEFAULT;
  out_text->content_scale = 1.0f;
  out_text->linear_color = vkr_srgb_color_to_linear(out_text->config.color);
  out_text->transform = vkr_transform_identity();
  out_text->layout_dirty = true_v;
  out_text->buffers_dirty = true_v;

  out_text->resolved_font =
      vkr_ui_text_resolve_font(font_system, out_text->config.font);

  if (!out_text->resolved_font) {
    log_error("No font available for UI text");
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    }
    vkr_ui_text_destroy(out_text);
    return false_v;
  }

  return true_v;
}

void vkr_ui_text_destroy(VkrUiText *text) {
  if (!text) {
    return;
  }

  vkr_text_layout_destroy(&text->layout);

  if (text->content.str && text->allocator) {
    vkr_allocator_free(text->allocator, (void *)text->content.str,
                       text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (text->geometry.vertices && text->allocator) {
    vkr_allocator_free(text->allocator, text->geometry.vertices,
                       (uint64_t)text->geometry.vertex_capacity *
                           sizeof(VkrTextVertex),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (text->geometry.indices && text->allocator) {
    vkr_allocator_free(text->allocator, text->geometry.indices,
                       (uint64_t)text->geometry.index_capacity *
                           sizeof(uint32_t),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  MemZero(text, sizeof(VkrUiText));
}

bool8_t vkr_ui_text_set_content(VkrUiText *text, String8 content) {
  if (!text || !text->allocator) {
    return false_v;
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

  const bool8_t color_changed =
      !vec4_equal(text->config.color, config->color, VKR_FLOAT_EPSILON);

  text->config = *config;
  text->linear_color = vkr_srgb_color_to_linear(config->color);

  if (font_changed) {
    text->resolved_font =
        vkr_ui_text_resolve_font(text->font_system, config->font);
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (layout_changed) {
    text->layout_dirty = true_v;
    text->buffers_dirty = true_v;
  } else if (color_changed) {
    text->buffers_dirty = true_v;
  }
}

void vkr_ui_text_set_content_scale(VkrUiText *text, float32_t content_scale) {
  if (!text || !isfinite(content_scale) || content_scale <= 0.0f ||
      text->content_scale == content_scale) {
    return;
  }
  text->content_scale = content_scale;
  text->layout_dirty = true_v;
  text->buffers_dirty = true_v;
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
  text->linear_color = vkr_srgb_color_to_linear(color);
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

bool8_t vkr_ui_text_prepare_geometry(VkrUiText *text) {
  if (!text) {
    return false_v;
  }

  if (text->layout_dirty) {
    vkr_ui_text_compute_layout(text);
  }

  if (text->buffers_dirty) {
    if (!vkr_ui_text_generate_geometry(text)) {
      log_error("Failed to generate UI text geometry");
      return false_v;
    }
  }

  return text->geometry.vertex_count > 0 && text->geometry.index_count > 0;
}
