#include "renderer/resources/world/vkr_text_3d.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_texture_system.h"

#define VKR_TEXT_3D_QUAD_COUNT 4
#define VKR_TEXT_3D_INDEX_COUNT 6
#define VKR_TEXT_3D_VERTEX_GROWTH_COUNT 64
#define VKR_TEXT_3D_INDEX_GROWTH_COUNT 96

vkr_internal bool8_t vkr_text_3d_codepoint_key(char *buffer,
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

vkr_internal const VkrFontGlyph *vkr_text_3d_find_glyph(const VkrFont *font,
                                                        uint32_t codepoint,
                                                        uint32_t *out_index) {
  if (font == NULL || font->glyphs.data == NULL) {
    return NULL;
  }

  if (font->glyph_indices.entries != NULL && font->glyph_indices.size > 0) {
    char key[16];
    if (vkr_text_3d_codepoint_key(key, sizeof(key), codepoint)) {
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

vkr_internal String8 vkr_text_3d_copy_text(VkrAllocator *allocator,
                                           String8 text) {
  if (!allocator || !text.str || text.length == 0) {
    return (String8){0};
  }
  return string8_duplicate(allocator, &text);
}

vkr_internal void vkr_text_3d_compute_layout(VkrText3D *text_3d,
                                             const VkrFont *font) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  assert_log(font != NULL, "Font is NULL");

  if (text_3d->layout.allocator != NULL) {
    vkr_text_layout_destroy(&text_3d->layout);
  }

  if (!text_3d->text.str || text_3d->text.length == 0) {
    text_3d->layout = (VkrTextLayout){0};
    text_3d->bounds = (VkrTextBounds){0};
    text_3d->layout_dirty = false_v;
    return;
  }

  float32_t font_size = text_3d->font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)font->size;
  }

  VkrTextStyle style =
      vkr_text_style_new(text_3d->font, font_size, text_3d->color);
  style = vkr_text_style_with_font_data(&style, font);

  VkrText text_for_layout = vkr_text_from_view(text_3d->text, &style);
  text_3d->layout = vkr_text_layout_compute(
      text_3d->allocator, &text_for_layout, &text_3d->layout_options);

  text_3d->bounds.size = text_3d->layout.bounds;

  float32_t scale = font_size / (float32_t)font->size;
  text_3d->bounds.ascent = (float32_t)font->ascent * scale;
  text_3d->bounds.descent = (float32_t)font->descent * scale;

  text_3d->layout_dirty = false_v;
}

typedef struct VkrText3DGlyphQuad {
  bool8_t valid;
  float32_t glyph_w;
  float32_t glyph_h;
  float32_t x0;
  float32_t y0;
  float32_t x1;
  float32_t y1;
} VkrText3DGlyphQuad;

typedef struct VkrText3DContentBounds {
  bool8_t have_bounds;
  float32_t min_x;
  float32_t min_y;
  float32_t max_x;
  float32_t max_y;
} VkrText3DContentBounds;

vkr_internal VkrText3DGlyphQuad vkr_text_3d_compute_glyph_quad(
    const VkrFont *font, const VkrFontGlyph *font_glyph, uint32_t glyph_index,
    const VkrTextGlyph *layout_glyph, float32_t scale, float32_t font_size,
    float32_t line_top, float32_t layout_bottom) {
  VkrText3DGlyphQuad result = {0};
  if (!font || !font_glyph || !layout_glyph) {
    return result;
  }

  float32_t glyph_w = (float32_t)font_glyph->width * scale;
  float32_t glyph_h = (float32_t)font_glyph->height * scale;

  if (font->type == VKR_FONT_TYPE_MTSDF && font->mtsdf_glyphs.data &&
      glyph_index < font->mtsdf_glyphs.length) {
    const VkrMtsdfGlyph *mtsdf_glyph = &font->mtsdf_glyphs.data[glyph_index];
    if (mtsdf_glyph->has_geometry) {
      glyph_w =
          (mtsdf_glyph->plane_right - mtsdf_glyph->plane_left) * font_size;
      glyph_h =
          (mtsdf_glyph->plane_top - mtsdf_glyph->plane_bottom) * font_size;
    } else {
      // This happens for e.g. whitespace in some MTSDF exports.
      glyph_w = 0.0f;
      glyph_h = 0.0f;
    }
  }

  if (glyph_w <= 0.0f || glyph_h <= 0.0f) {
    return result;
  }

  float32_t x0 =
      layout_glyph->position.x + (float32_t)font_glyph->x_offset * scale;
  float32_t y0_raw = line_top + (float32_t)font_glyph->y_offset * scale;
  float32_t x1 = x0 + glyph_w;
  float32_t y1_raw = y0_raw + glyph_h;

  // Convert from baseline-up layout into top-down local quad space.
  float32_t y0 = layout_bottom - y1_raw;
  float32_t y1 = layout_bottom - y0_raw;

  result.valid = true_v;
  result.glyph_w = glyph_w;
  result.glyph_h = glyph_h;
  result.x0 = x0;
  result.y0 = y0;
  result.x1 = x1;
  result.y1 = y1;
  return result;
}

vkr_internal VkrText3DContentBounds vkr_text_3d_compute_content_bounds(
    const VkrText3D *text_3d, const VkrFont *font, uint32_t glyph_count,
    float32_t scale, float32_t font_size, float32_t layout_bottom) {
  VkrText3DContentBounds bounds = {0};
  if (!text_3d || !font || glyph_count == 0) {
    return bounds;
  }

  for (uint32_t i = 0; i < glyph_count; ++i) {
    const VkrTextGlyph *layout_glyph = &text_3d->layout.glyphs.data[i];
    uint32_t glyph_index = 0;
    const VkrFontGlyph *font_glyph =
        vkr_text_3d_find_glyph(font, layout_glyph->codepoint, &glyph_index);
    if (!font_glyph) {
      continue;
    }

    float32_t line_top = layout_glyph->position.y - text_3d->bounds.ascent;
    VkrText3DGlyphQuad quad = vkr_text_3d_compute_glyph_quad(
        font, font_glyph, glyph_index, layout_glyph, scale, font_size, line_top,
        layout_bottom);
    if (!quad.valid) {
      continue;
    }

    if (!bounds.have_bounds) {
      bounds.min_x = quad.x0;
      bounds.max_x = quad.x1;
      bounds.min_y = quad.y0;
      bounds.max_y = quad.y1;
      bounds.have_bounds = true_v;
    } else {
      bounds.min_x = Min(bounds.min_x, quad.x0);
      bounds.max_x = Max(bounds.max_x, quad.x1);
      bounds.min_y = Min(bounds.min_y, quad.y0);
      bounds.max_y = Max(bounds.max_y, quad.y1);
    }
  }

  return bounds;
}

vkr_internal void vkr_text_3d_compute_content_offsets(
    const VkrText3D *text_3d, const VkrText3DContentBounds *bounds,
    float32_t *out_offset_x, float32_t *out_offset_y) {
  assert_log(out_offset_x != NULL, "out_offset_x is NULL");
  assert_log(out_offset_y != NULL, "out_offset_y is NULL");

  *out_offset_x = 0.0f;
  *out_offset_y = 0.0f;

  if (!text_3d || !bounds || !bounds->have_bounds) {
    return;
  }

  float32_t content_w = bounds->max_x - bounds->min_x;
  float32_t content_h = bounds->max_y - bounds->min_y;

  if (text_3d->texture_width > 0) {
    if (content_w < (float32_t)text_3d->texture_width) {
      *out_offset_x = ((float32_t)text_3d->texture_width - content_w) * 0.5f -
                      bounds->min_x;
    } else if (bounds->min_x < 0.0f) {
      *out_offset_x = -bounds->min_x;
    }
  } else {
    *out_offset_x = -bounds->min_x;
  }

  if (text_3d->texture_height > 0) {
    if (content_h < (float32_t)text_3d->texture_height) {
      *out_offset_y = ((float32_t)text_3d->texture_height - content_h) * 0.5f -
                      bounds->min_y;
    } else if (bounds->min_y < 0.0f) {
      *out_offset_y = -bounds->min_y;
    }
  } else {
    *out_offset_y = -bounds->min_y;
  }
}

vkr_internal void vkr_text_3d_generate_vertices(
    const VkrText3D *text_3d, const VkrFont *font, uint32_t glyph_count,
    float32_t scale, float32_t font_size, float32_t layout_bottom,
    float32_t inv_atlas_w, float32_t inv_atlas_h, float32_t offset_x,
    float32_t offset_y, VkrTextVertex *vertices, uint32_t *indices,
    uint32_t *out_vertex_count, uint32_t *out_index_count) {
  assert_log(out_vertex_count != NULL, "out_vertex_count is NULL");
  assert_log(out_index_count != NULL, "out_index_count is NULL");

  *out_vertex_count = 0;
  *out_index_count = 0;

  if (!text_3d || !font || glyph_count == 0 || !vertices || !indices) {
    return;
  }

  uint32_t vertex_idx = 0;
  uint32_t index_idx = 0;
  Vec4 color = text_3d->color;

  for (uint32_t i = 0; i < glyph_count; ++i) {
    const VkrTextGlyph *layout_glyph = &text_3d->layout.glyphs.data[i];
    uint32_t glyph_index = 0;
    const VkrFontGlyph *font_glyph =
        vkr_text_3d_find_glyph(font, layout_glyph->codepoint, &glyph_index);
    if (!font_glyph) {
      continue;
    }

    float32_t line_top = layout_glyph->position.y - text_3d->bounds.ascent;
    VkrText3DGlyphQuad quad = vkr_text_3d_compute_glyph_quad(
        font, font_glyph, glyph_index, layout_glyph, scale, font_size, line_top,
        layout_bottom);
    if (!quad.valid) {
      continue;
    }

    float32_t x0 = quad.x0 + offset_x;
    float32_t x1 = quad.x1 + offset_x;
    float32_t y0 = quad.y0 + offset_y;
    float32_t y1 = quad.y1 + offset_y;

    float32_t u0_raw = (float32_t)font_glyph->x * inv_atlas_w;
    float32_t u1_raw =
        (float32_t)(font_glyph->x + font_glyph->width) * inv_atlas_w;
    float32_t v0_raw =
        1.0f - (float32_t)(font_glyph->y + font_glyph->height) * inv_atlas_h;
    float32_t v1_raw = 1.0f - (float32_t)font_glyph->y * inv_atlas_h;

    float32_t inset_px = text_3d->uv_inset_px;
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

    vertices[vertex_idx].position = vec2_new(x0, y0);
    vertices[vertex_idx].texcoord = vec2_new(u0, v0);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x1, y1);
    vertices[vertex_idx].texcoord = vec2_new(u1, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x0, y1);
    vertices[vertex_idx].texcoord = vec2_new(u0, v1);
    vertices[vertex_idx].color = color;
    vertex_idx++;

    vertices[vertex_idx].position = vec2_new(x1, y0);
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

  *out_vertex_count = vertex_idx;
  *out_index_count = index_idx;
}

vkr_internal bool8_t vkr_text_3d_generate_buffers(VkrText3D *text_3d,
                                                  VkrFont *font) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  assert_log(font != NULL, "Font is NULL");

  uint32_t glyph_count = (uint32_t)text_3d->layout.glyphs.length;
  if (glyph_count == 0) {
    text_3d->quad_count = 0;
    text_3d->buffers_dirty = false_v;
    return true_v;
  }

  uint32_t required_vertex_count = glyph_count * VKR_TEXT_3D_QUAD_COUNT;
  uint32_t required_index_count = glyph_count * VKR_TEXT_3D_INDEX_COUNT;

  bool8_t has_buffers = text_3d->vertex_buffer.handle != NULL &&
                        text_3d->index_buffer.handle != NULL;
  bool8_t need_realloc = !has_buffers ||
                         required_vertex_count > text_3d->vertex_capacity ||
                         required_index_count > text_3d->index_capacity;

  uint32_t alloc_vertex_count = required_vertex_count;
  uint32_t alloc_index_count = required_index_count;
  if (need_realloc) {
    alloc_vertex_count =
        required_vertex_count + VKR_TEXT_3D_VERTEX_GROWTH_COUNT;
    alloc_index_count = required_index_count + VKR_TEXT_3D_INDEX_GROWTH_COUNT;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(text_3d->allocator);
  bool8_t use_scope = scope.allocator != NULL;

  VkrTextVertex *vertices = vkr_allocator_alloc(
      text_3d->allocator, sizeof(VkrTextVertex) * alloc_vertex_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = vkr_allocator_alloc(text_3d->allocator,
                                          sizeof(uint32_t) * alloc_index_count,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!vertices || !indices) {
    if (use_scope) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      if (vertices) {
        vkr_allocator_free(text_3d->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      if (indices) {
        vkr_allocator_free(text_3d->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    }
    return false_v;
  }

  MemZero(vertices, sizeof(VkrTextVertex) * alloc_vertex_count);
  MemZero(indices, sizeof(uint32_t) * alloc_index_count);

  float32_t atlas_w = (float32_t)font->atlas_size_x;
  float32_t atlas_h = (float32_t)font->atlas_size_y;
  if (atlas_w <= 0.0f || atlas_h <= 0.0f) {
    if (use_scope) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      vkr_allocator_free(text_3d->allocator, vertices,
                         sizeof(VkrTextVertex) * alloc_vertex_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_allocator_free(text_3d->allocator, indices,
                         sizeof(uint32_t) * alloc_index_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return false_v;
  }
  float32_t inv_atlas_w = 1.0f / atlas_w;
  float32_t inv_atlas_h = 1.0f / atlas_h;

  float32_t font_size = text_3d->font_size;
  if (font_size <= 0.0f) {
    font_size = (float32_t)font->size;
  }
  float32_t scale = font_size / (float32_t)font->size;

  float32_t layout_bottom =
      (text_3d->layout.baseline.y - text_3d->bounds.ascent) +
      text_3d->bounds.size.y;

  VkrText3DContentBounds bounds = vkr_text_3d_compute_content_bounds(
      text_3d, font, glyph_count, scale, font_size, layout_bottom);

  float32_t offset_x = 0.0f;
  float32_t offset_y = 0.0f;
  vkr_text_3d_compute_content_offsets(text_3d, &bounds, &offset_x, &offset_y);

  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  vkr_text_3d_generate_vertices(text_3d, font, glyph_count, scale, font_size,
                                layout_bottom, inv_atlas_w, inv_atlas_h,
                                offset_x, offset_y, vertices, indices,
                                &vertex_count, &index_count);
  text_3d->quad_count = vertex_count / VKR_TEXT_3D_QUAD_COUNT;

  if (vertex_count == 0 || index_count == 0) {
    text_3d->buffers_dirty = false_v;
    if (use_scope) {
      vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    } else {
      vkr_allocator_free(text_3d->allocator, vertices,
                         sizeof(VkrTextVertex) * alloc_vertex_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_allocator_free(text_3d->allocator, indices,
                         sizeof(uint32_t) * alloc_index_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    return true_v;
  }

  VkrRendererError buffer_err = VKR_RENDERER_ERROR_NONE;
  if (need_realloc) {
    if (text_3d->vertex_buffer.handle) {
      vkr_vertex_buffer_destroy(text_3d->renderer, &text_3d->vertex_buffer);
    }
    if (text_3d->index_buffer.handle) {
      vkr_index_buffer_destroy(text_3d->renderer, &text_3d->index_buffer);
    }

    text_3d->vertex_buffer = vkr_vertex_buffer_create_dynamic(
        text_3d->renderer, vertices, sizeof(VkrTextVertex), alloc_vertex_count,
        VKR_VERTEX_INPUT_RATE_VERTEX, string8_lit("text_3d_vertices"),
        &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text_3d->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text_3d->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }

    text_3d->index_buffer = vkr_index_buffer_create_dynamic(
        text_3d->renderer, indices, VKR_INDEX_TYPE_UINT32, alloc_index_count,
        string8_lit("text_3d_indices"), &buffer_err);

    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      vkr_vertex_buffer_destroy(text_3d->renderer, &text_3d->vertex_buffer);
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text_3d->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text_3d->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }

    text_3d->vertex_capacity = alloc_vertex_count;
    text_3d->index_capacity = alloc_index_count;
  } else {
    buffer_err = vkr_vertex_buffer_update(
        text_3d->renderer, &text_3d->vertex_buffer, vertices, 0, vertex_count);
    if (buffer_err == VKR_RENDERER_ERROR_NONE) {
      buffer_err = vkr_index_buffer_update(
          text_3d->renderer, &text_3d->index_buffer, indices, 0, index_count);
    }
    if (buffer_err != VKR_RENDERER_ERROR_NONE) {
      if (use_scope) {
        vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      } else {
        vkr_allocator_free(text_3d->allocator, vertices,
                           sizeof(VkrTextVertex) * alloc_vertex_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_allocator_free(text_3d->allocator, indices,
                           sizeof(uint32_t) * alloc_index_count,
                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
      return false_v;
    }
  }

  if (use_scope) {
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else {
    vkr_allocator_free(text_3d->allocator, vertices,
                       sizeof(VkrTextVertex) * alloc_vertex_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(text_3d->allocator, indices,
                       sizeof(uint32_t) * alloc_index_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  text_3d->buffers_dirty = false_v;
  return true_v;
}

bool8_t vkr_text_3d_create(VkrText3D *text_3d,
                           VkrRendererFrontendHandle renderer,
                           VkrFontSystem *font_system, VkrAllocator *allocator,
                           const VkrText3DConfig *config,
                           VkrRendererError *out_error) {
  assert_log(text_3d != NULL, "Text3D is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(font_system != NULL, "Font system is NULL");
  assert_log(allocator != NULL, "Allocator is NULL");

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }

  MemZero(text_3d, sizeof(*text_3d));
  text_3d->instance_state.id = VKR_INVALID_ID;

  text_3d->allocator = allocator;
  text_3d->renderer = renderer;
  text_3d->font_system = font_system;

  VkrText3DConfig cfg = config ? *config : VKR_TEXT_3D_CONFIG_DEFAULT;

  text_3d->font =
      cfg.font.id != 0 ? cfg.font : font_system->default_mtsdf_font_handle;
  text_3d->font_size = cfg.font_size;
  text_3d->color = cfg.color;
  text_3d->uv_inset_px = cfg.uv_inset_px;
  text_3d->text = vkr_text_3d_copy_text(allocator, cfg.text);

  text_3d->texture_width = cfg.texture_width;
  text_3d->texture_height = cfg.texture_height;
  if (text_3d->texture_width == 0) {
    text_3d->texture_width = VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;
  }
  if (text_3d->texture_height == 0) {
    text_3d->texture_height = VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;

  text_3d->pipeline = cfg.pipeline;
  if (text_3d->pipeline.id == 0) {
    VkrRendererError pipe_err = VKR_RENDERER_ERROR_NONE;
    String8 name = string8_lit("shader.default.world_text");
    if (vkr_pipeline_registry_acquire_by_name(&rf->pipeline_registry, name,
                                              true_v, &text_3d->pipeline,
                                              &pipe_err)) {
      text_3d->pipeline_ref_acquired = true_v;
    }
  }

  if (text_3d->pipeline.id != 0) {
    VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, text_3d->pipeline, &text_3d->instance_state,
            &inst_err)) {
      if (out_error) {
        *out_error = inst_err;
      }
      vkr_text_3d_destroy(text_3d);
      return false_v;
    }
  }

  text_3d->layout_options = vkr_text_layout_options_default();
  text_3d->layout_options.word_wrap = false_v;
  text_3d->layout_options.anchor.horizontal = VKR_TEXT_ALIGN_LEFT;
  text_3d->layout_options.anchor.vertical = VKR_TEXT_BASELINE_TOP;

  text_3d->transform = vkr_transform_identity();
  text_3d->world_width = 1.0f;
  if (text_3d->texture_width > 0) {
    text_3d->world_height =
        (float32_t)text_3d->texture_height / text_3d->texture_width;
  } else {
    text_3d->world_height = 1.0f;
  }

  text_3d->layout_dirty = true_v;
  text_3d->buffers_dirty = true_v;
  text_3d->initialized = true_v;

  return true_v;
}

void vkr_text_3d_destroy(VkrText3D *text_3d) {
  if (!text_3d) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)text_3d->renderer;

  if (text_3d->instance_state.id != VKR_INVALID_ID &&
      text_3d->pipeline.id != 0 && rf) {
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, text_3d->pipeline, text_3d->instance_state,
        &(VkrRendererError){0});
  }

  if (text_3d->pipeline_ref_acquired && text_3d->pipeline.id != 0 && rf) {
    vkr_pipeline_registry_release(&rf->pipeline_registry, text_3d->pipeline);
  }

  if (text_3d->vertex_buffer.handle) {
    vkr_vertex_buffer_destroy(text_3d->renderer, &text_3d->vertex_buffer);
  }
  if (text_3d->index_buffer.handle) {
    vkr_index_buffer_destroy(text_3d->renderer, &text_3d->index_buffer);
  }

  vkr_text_layout_destroy(&text_3d->layout);

  if (text_3d->text.str && text_3d->allocator) {
    vkr_allocator_free(text_3d->allocator, (void *)text_3d->text.str,
                       text_3d->text.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  MemZero(text_3d, sizeof(*text_3d));
}

void vkr_text_3d_set_text(VkrText3D *text_3d, String8 text) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  assert_log(text_3d->allocator != NULL, "Allocator is NULL");

  if (text_3d->text.str) {
    vkr_allocator_free(text_3d->allocator, (void *)text_3d->text.str,
                       text_3d->text.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  text_3d->text = vkr_text_3d_copy_text(text_3d->allocator, text);
  text_3d->layout_dirty = true_v;
  text_3d->buffers_dirty = true_v;
}

void vkr_text_3d_set_color(VkrText3D *text_3d, Vec4 color) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  text_3d->color = color;
  text_3d->buffers_dirty = true_v;
}

void vkr_text_3d_set_transform(VkrText3D *text_3d, VkrTransform transform) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  text_3d->transform = transform;
}

void vkr_text_3d_update(VkrText3D *text_3d) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  assert_log(text_3d->initialized, "Text3D instance is not initialized");

  VkrFont *font =
      vkr_font_system_get_by_handle(text_3d->font_system, text_3d->font);
  if (!font) {
    font = vkr_font_system_get_default_mtsdf_font(text_3d->font_system);
  }

  if (!font) {
    log_warn("Text3D: no font available for rasterization");
    return;
  }

  if (text_3d->layout_dirty) {
    vkr_text_3d_compute_layout(text_3d, font);
  }

  if (text_3d->buffers_dirty) {
    if (!vkr_text_3d_generate_buffers(text_3d, font)) {
      log_warn("Text3D: failed to build glyph buffers");
    }
  }
}

void vkr_text_3d_draw(VkrText3D *text_3d) {
  assert_log(text_3d != NULL, "Text3D instance is NULL");
  assert_log(text_3d->initialized, "Text3D instance is not initialized");
  assert_log(text_3d->renderer != NULL, "Renderer is NULL");

  vkr_text_3d_update(text_3d);
  if (text_3d->quad_count == 0) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)text_3d->renderer;
  assert_log(rf != NULL, "Renderer frontend is NULL");

  rf->draw_state.instance_state = text_3d->instance_state;

  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_pipeline.id != text_3d->pipeline.id ||
      current_pipeline.generation != text_3d->pipeline.generation) {
    vkr_pipeline_registry_bind_pipeline(
        &rf->pipeline_registry, text_3d->pipeline, &(VkrRendererError){0});
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.default.world_text")) {
    return;
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "view", &rf->globals.view);
  vkr_shader_system_uniform_set(&rf->shader_system, "projection",
                                &rf->globals.projection);
  if (!vkr_shader_system_apply_global(&rf->shader_system)) {
    return;
  }

  Mat4 model = vkr_transform_get_world(&text_3d->transform);
  if (text_3d->texture_width > 0 && text_3d->texture_height > 0) {
    Vec3 scale = vec3_new(
        text_3d->world_width / (float32_t)text_3d->texture_width,
        text_3d->world_height / (float32_t)text_3d->texture_height, 1.0f);
    model = mat4_mul(model, mat4_scale(scale));
  }

  vkr_material_system_apply_local(&rf->material_system,
                                  &(VkrLocalMaterialState){.model = model});

  vkr_shader_system_bind_instance(&rf->shader_system,
                                  text_3d->instance_state.id);

  VkrFont *font =
      vkr_font_system_get_by_handle(text_3d->font_system, text_3d->font);
  if (!font) {
    font = vkr_font_system_get_default_mtsdf_font(text_3d->font_system);
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

  Vec4 diffuse_color = {1.0f, 1.0f, 1.0f, 1.0f};
  vkr_shader_system_uniform_set(&rf->shader_system, "diffuse_color",
                                &diffuse_color);

  float32_t screen_px_range = 0.0f;
  float32_t font_mode = 0.0f;
  if (font && font->type == VKR_FONT_TYPE_MTSDF) {
    float32_t render_size = text_3d->font_size;
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

  VkrVertexBufferBinding vbb = {
      .buffer = text_3d->vertex_buffer.handle,
      .binding = 0,
      .offset = 0,
  };
  vkr_renderer_bind_vertex_buffer(text_3d->renderer, &vbb);

  VkrIndexBufferBinding ibb = {
      .buffer = text_3d->index_buffer.handle,
      .type = VKR_INDEX_TYPE_UINT32,
      .offset = 0,
  };
  vkr_renderer_bind_index_buffer(text_3d->renderer, &ibb);

  uint32_t index_count = text_3d->quad_count * 6;
  vkr_renderer_draw_indexed(text_3d->renderer, index_count, 1, 0, 0, 0);
}
