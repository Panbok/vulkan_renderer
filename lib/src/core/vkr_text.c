#include "core/vkr_text.h"

#include "core/logger.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

#include <math.h>

#define VKR_TEXT_DEFAULT_FONT_SIZE 16.0f

/////////////////////
// UTF-8 helpers
/////////////////////
#define VKR_UTF8_IS_CONT(byte) ((byte & 0xC0) == 0x80)

#define VKR_UTF8_IS_INVALID_CODEPOINT(codepoint)                               \
  (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))

VkrCodepoint vkr_utf8_decode(const uint8_t *bytes, uint64_t max_bytes) {
  if (bytes == NULL || max_bytes == 0) {
    return (VkrCodepoint){0, 0};
  }

  uint8_t b0 = bytes[0];

  if ((b0 & 0x80) == 0) { // 1-byte ASCII
    return (VkrCodepoint){b0, 1};
  }

  if ((b0 & 0xE0) == 0xC0) { // 2-byte sequence
    if (max_bytes < 2 || !VKR_UTF8_IS_CONT(bytes[1])) {
      return (VkrCodepoint){0, 0};
    }
    uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(bytes[1] & 0x3F);
    if (cp < 0x80) { // overlong
      return (VkrCodepoint){0, 0};
    }
    return (VkrCodepoint){cp, 2};
  }

  if ((b0 & 0xF0) == 0xE0) { // 3-byte sequence
    if (max_bytes < 3 || !VKR_UTF8_IS_CONT(bytes[1]) ||
        !VKR_UTF8_IS_CONT(bytes[2])) {
      return (VkrCodepoint){0, 0};
    }
    uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                  ((uint32_t)(bytes[1] & 0x3F) << 6) |
                  (uint32_t)(bytes[2] & 0x3F);
    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) { // overlong or surrogate
      return (VkrCodepoint){0, 0};
    }
    return (VkrCodepoint){cp, 3};
  }

  if ((b0 & 0xF8) == 0xF0) { // 4-byte sequence
    if (max_bytes < 4 || !VKR_UTF8_IS_CONT(bytes[1]) ||
        !VKR_UTF8_IS_CONT(bytes[2]) || !VKR_UTF8_IS_CONT(bytes[3])) {
      return (VkrCodepoint){0, 0};
    }
    uint32_t cp =
        ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[1] & 0x3F) << 12) |
        ((uint32_t)(bytes[2] & 0x3F) << 6) | (uint32_t)(bytes[3] & 0x3F);
    if (cp < 0x10000 || VKR_UTF8_IS_INVALID_CODEPOINT(cp)) { // overlong/invalid
      return (VkrCodepoint){0, 0};
    }
    return (VkrCodepoint){cp, 4};
  }

  return (VkrCodepoint){0, 0};
}

uint8_t vkr_utf8_encode(uint32_t codepoint, uint8_t *out, uint64_t max_bytes) {
  if (out == NULL || VKR_UTF8_IS_INVALID_CODEPOINT(codepoint)) {
    return 0;
  }

  if (codepoint <= 0x7F) {
    if (max_bytes < 1)
      return 0;
    out[0] = (uint8_t)codepoint;
    return 1;
  } else if (codepoint <= 0x7FF) {
    if (max_bytes < 2)
      return 0;
    out[0] = 0xC0 | (uint8_t)(codepoint >> 6);
    out[1] = 0x80 | (uint8_t)(codepoint & 0x3F);
    return 2;
  } else if (codepoint <= 0xFFFF) {
    if (max_bytes < 3)
      return 0;
    out[0] = 0xE0 | (uint8_t)(codepoint >> 12);
    out[1] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
    out[2] = 0x80 | (uint8_t)(codepoint & 0x3F);
    return 3;
  } else if (codepoint <= 0x10FFFF) {
    if (max_bytes < 4)
      return 0;
    out[0] = 0xF0 | (uint8_t)(codepoint >> 18);
    out[1] = 0x80 | (uint8_t)((codepoint >> 12) & 0x3F);
    out[2] = 0x80 | (uint8_t)((codepoint >> 6) & 0x3F);
    out[3] = 0x80 | (uint8_t)(codepoint & 0x3F);
    return 4;
  }

  return 0;
}

VkrCodepointIter vkr_codepoint_iter_begin(const String8 *str) {
  assert_log(str != NULL && str->str != NULL, "Invalid string");
  VkrCodepointIter iter = {0};
  iter.str = str;
  iter.byte_offset = 0;
  return iter;
}

bool8_t vkr_codepoint_iter_has_next(const VkrCodepointIter *iter) {
  if (iter == NULL || iter->str == NULL || iter->str->str == NULL) {
    return false_v;
  }
  return iter->byte_offset < iter->str->length;
}

VkrCodepoint vkr_codepoint_iter_next(VkrCodepointIter *iter) {
  assert_log(iter != NULL && iter->str != NULL && iter->str->str != NULL,
             "Invalid iterator");
  if (!vkr_codepoint_iter_has_next(iter)) {
    return (VkrCodepoint){0, 0};
  }

  const uint64_t remaining = iter->str->length - iter->byte_offset;
  const uint8_t *bytes = iter->str->str + iter->byte_offset;

  VkrCodepoint cp = vkr_utf8_decode(bytes, remaining);
  if (cp.byte_length == 0) {
    iter->byte_offset += 1; // advance at least one byte to avoid infinite loop
  } else {
    iter->byte_offset += cp.byte_length;
  }

  return cp;
}

VkrCodepoint vkr_codepoint_iter_peek(const VkrCodepointIter *iter) {
  assert_log(iter != NULL && iter->str != NULL && iter->str->str != NULL,
             "Invalid iterator");
  if (!vkr_codepoint_iter_has_next(iter)) {
    return (VkrCodepoint){0, 0};
  }

  const uint64_t remaining = iter->str->length - iter->byte_offset;
  const uint8_t *bytes = iter->str->str + iter->byte_offset;
  return vkr_utf8_decode(bytes, remaining);
}

uint64_t vkr_string8_codepoint_count(const String8 *str) {
  if (str == NULL || str->str == NULL) {
    return 0;
  }

  VkrCodepointIter iter = vkr_codepoint_iter_begin(str);
  uint64_t count = 0;
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }
    count++;
  }
  return count;
}

bool8_t vkr_string8_is_valid_utf8(const String8 *str) {
  if (str == NULL || str->str == NULL) {
    return false_v;
  }

  VkrCodepointIter iter = vkr_codepoint_iter_begin(str);
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      return false_v;
    }
  }

  return true_v;
}

Vec2 vkr_text_mtsdf_unit_range(float32_t distance_range, uint32_t atlas_width,
                               uint32_t atlas_height) {
  assert_log(isfinite(distance_range) && distance_range > 0.0f,
             "MTSDF distance range must be finite and positive");
  assert_log(atlas_width > 0 && atlas_height > 0,
             "MTSDF atlas extent must be nonzero");
  return vec2_new(distance_range / (float32_t)atlas_width,
                  distance_range / (float32_t)atlas_height);
}

float32_t vkr_text_uv_inset(float32_t configured_inset_px,
                            bool8_t preserve_exact_bounds) {
  if (preserve_exact_bounds || configured_inset_px <= 0.0f) {
    return 0.0f;
  }
  return configured_inset_px;
}

/////////////////////
// Helpers
/////////////////////

vkr_internal VkrTextStyle vkr_text_resolve_style(const VkrTextStyle *style) {
  if (style != NULL) {
    return *style;
  }
  return vkr_text_style_default();
}

#define VKR_TEXT_DEFAULT_ASCENT(font_size) ((font_size) * 0.8f)
#define vkr_text_default_descent(font_size) ((font_size) * 0.2f)
#define VKR_TEXT_DEFAULT_LINE_GAP() 0.0f
#define VKR_TEXT_DEFAULT_GLYPH_WIDTH(font_size) ((font_size) * 0.6f)

vkr_internal float32_t vkr_text_resolve_font_size(const VkrTextStyle *style,
                                                  const VkrFont *font) {
  if (style->font_size > 0.0f) {
    return style->font_size;
  }
  if (font != NULL && font->size > 0) {
    return (float32_t)font->size;
  }
  return VKR_TEXT_DEFAULT_FONT_SIZE;
}

vkr_internal float32_t vkr_text_font_scale_for_size(const VkrFont *font,
                                                    float32_t font_size) {
  if (font == NULL) {
    return 1.0f;
  }
  if (font->glyphs_by_id.data != NULL && font->codepoint_map.data != NULL) {
    return font_size;
  }
  float32_t base_size = (float32_t)font->size;
  if (base_size <= 0.0f || font_size <= 0.0f) {
    return 1.0f;
  }
  return font_size / base_size;
}

vkr_internal void vkr_text_compute_metrics(const VkrTextStyle *style,
                                           float32_t font_scale,
                                           float32_t *out_ascent,
                                           float32_t *out_descent,
                                           float32_t *out_line_gap,
                                           float32_t *out_metrics_height) {
  if (style->font_data == NULL) {
    float32_t ascent = VKR_TEXT_DEFAULT_ASCENT(style->font_size);
    float32_t descent = vkr_text_default_descent(style->font_size);
    float32_t line_gap = VKR_TEXT_DEFAULT_LINE_GAP();
    if (out_ascent) {
      *out_ascent = ascent;
    }
    if (out_descent) {
      *out_descent = descent;
    }
    if (out_line_gap) {
      *out_line_gap = line_gap;
    }
    if (out_metrics_height) {
      *out_metrics_height = ascent + descent + line_gap;
    }
    return;
  }

  const VkrFont *font = style->font_data;
  if (font->glyphs_by_id.data != NULL && font->codepoint_map.data != NULL) {
    const float32_t ascent = font->em_ascender * font_scale;
    const float32_t descent = -font->em_descender * font_scale;
    const float32_t metrics_height = font->em_line_height * font_scale;
    if (out_ascent) {
      *out_ascent = ascent;
    }
    if (out_descent) {
      *out_descent = descent;
    }
    if (out_line_gap) {
      *out_line_gap = metrics_height - ascent - descent;
    }
    if (out_metrics_height) {
      *out_metrics_height = metrics_height;
    }
    return;
  }
  float32_t base_ascent = (float32_t)font->ascent;
  float32_t base_descent = (float32_t)font->descent;
  float32_t base_line_height = (float32_t)font->line_height;
  if (base_line_height <= 0.0f) {
    base_line_height = base_ascent + base_descent;
  }
  float32_t base_line_gap =
      Max(0.0f, base_line_height - (base_ascent + base_descent));

  float32_t ascent = base_ascent * font_scale;
  float32_t descent = base_descent * font_scale;
  float32_t line_gap = base_line_gap * font_scale;

  if (out_ascent) {
    *out_ascent = ascent;
  }
  if (out_descent) {
    *out_descent = descent;
  }
  if (out_line_gap) {
    *out_line_gap = line_gap;
  }
  if (out_metrics_height) {
    *out_metrics_height = ascent + descent + line_gap;
  }
}

vkr_internal bool8_t vkr_text_codepoint_key(char *buffer, uint64_t buffer_size,
                                            uint32_t codepoint) {
  if (buffer == NULL || buffer_size == 0) {
    return false_v;
  }

  char tmp[16];
  uint32_t v = codepoint;
  uint32_t tmp_len = 0;
  do {
    tmp[tmp_len++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u && tmp_len < (uint32_t)sizeof(tmp));

  if (v != 0u) {
    buffer[0] = '\0';
    return false_v;
  }

  if ((uint64_t)tmp_len + 1u > buffer_size) {
    buffer[0] = '\0';
    return false_v;
  }

  for (uint32_t i = 0; i < tmp_len; ++i) {
    buffer[i] = tmp[tmp_len - 1u - i];
  }

  buffer[tmp_len] = '\0';
  return true_v;
}

typedef struct VkrTextResolvedGlyph {
  uint32_t glyph_id;
  uint32_t glyph_index;
  float32_t advance;
  uint8_t page_id;
  bool8_t cooked;
  bool8_t valid;
} VkrTextResolvedGlyph;

vkr_internal const VkrFontGlyph *
vkr_text_font_find_legacy_glyph(const VkrFont *font, uint32_t codepoint,
                                uint32_t *out_index) {
  if (font == NULL || font->glyphs.data == NULL) {
    return NULL;
  }
  if (font->glyph_indices.entries != NULL && font->glyph_indices.size > 0) {
    char key[16];
    if (vkr_text_codepoint_key(key, sizeof(key), codepoint)) {
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

vkr_internal INLINE uint64_t vkr_text_kerning_pair_key(uint32_t codepoint_0,
                                                       uint32_t codepoint_1) {
  return ((uint64_t)codepoint_0 << 32u) | (uint64_t)codepoint_1;
}

vkr_internal const VkrFontCodepointMapEntry *
vkr_text_font_find_cooked_mapping(const VkrFont *font, uint32_t codepoint) {
  uint64_t first = 0u;
  uint64_t count = font->codepoint_map.length;
  while (count != 0u) {
    const uint64_t step = count / 2u;
    const uint64_t index = first + step;
    const VkrFontCodepointMapEntry *candidate =
        &font->codepoint_map.data[index];
    if (candidate->codepoint < codepoint) {
      first = index + 1u;
      count -= step + 1u;
    } else if (candidate->codepoint > codepoint) {
      count = step;
    } else {
      return candidate;
    }
  }
  return NULL;
}

vkr_internal VkrTextResolvedGlyph
vkr_text_font_resolve_glyph(const VkrFont *font, float32_t font_size,
                            float32_t font_scale, uint32_t codepoint) {
  VkrTextResolvedGlyph resolved = {
      .glyph_index = VKR_INVALID_ID,
      .advance = VKR_TEXT_DEFAULT_GLYPH_WIDTH(font_size),
  };
  if (font == NULL) {
    return resolved;
  }
  if (codepoint == '\t') {
    resolved.advance = font->tab_x_advance * font_scale;
    return resolved;
  }
  if (font->glyphs_by_id.data != NULL && font->codepoint_map.data != NULL) {
    const VkrFontCodepointMapEntry *mapping =
        vkr_text_font_find_cooked_mapping(font, codepoint);
    const uint32_t glyph_index =
        mapping ? mapping->glyph_index : font->fallback_glyph_index;
    if (glyph_index >= font->glyphs_by_id.length) {
      return resolved;
    }
    const VkrFontGlyphId *glyph = &font->glyphs_by_id.data[glyph_index];
    resolved.glyph_id = glyph->glyph_id;
    resolved.glyph_index = glyph_index;
    resolved.advance = glyph->advance * font_scale;
    resolved.page_id = (uint8_t)glyph->page_index;
    resolved.cooked = true_v;
    resolved.valid = true_v;
    return resolved;
  }
  uint32_t glyph_index = 0u;
  const VkrFontGlyph *glyph =
      vkr_text_font_find_legacy_glyph(font, codepoint, &glyph_index);
  if (glyph != NULL) {
    resolved.glyph_index = glyph_index;
    resolved.advance = (float32_t)glyph->x_advance * font_scale;
    resolved.page_id = glyph->page_id;
    resolved.valid = true_v;
  }
  return resolved;
}

vkr_internal float32_t vkr_text_font_get_legacy_kerning(const VkrFont *font,
                                                        uint32_t prev_codepoint,
                                                        uint32_t codepoint) {
  if (font == NULL || font->kernings.data == NULL) {
    return 0.0f;
  }

  const uint64_t target = vkr_text_kerning_pair_key(prev_codepoint, codepoint);
  uint64_t lo = 0;
  uint64_t hi = font->kernings.length;

  while (lo < hi) {
    uint64_t mid = lo + ((hi - lo) >> 1u);
    const VkrFontKerning *kerning = &font->kernings.data[mid];
    const uint64_t mid_key =
        vkr_text_kerning_pair_key(kerning->codepoint_0, kerning->codepoint_1);
    if (mid_key < target) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }

  if (lo < font->kernings.length) {
    const VkrFontKerning *kerning = &font->kernings.data[lo];
    if (kerning->codepoint_0 == prev_codepoint &&
        kerning->codepoint_1 == codepoint) {
      return (float32_t)kerning->amount;
    }
  }

  return 0.0f;
}

vkr_internal float32_t vkr_text_font_get_cooked_kerning(
    const VkrFont *font, uint32_t left_glyph_id, uint32_t right_glyph_id) {
  const uint64_t target =
      vkr_text_kerning_pair_key(left_glyph_id, right_glyph_id);
  uint64_t first = 0u;
  uint64_t count = font->glyph_kernings.length;
  while (count != 0u) {
    const uint64_t step = count / 2u;
    const uint64_t index = first + step;
    const VkrFontGlyphKerning *candidate = &font->glyph_kernings.data[index];
    const uint64_t key = vkr_text_kerning_pair_key(candidate->left_glyph_id,
                                                   candidate->right_glyph_id);
    if (key < target) {
      first = index + 1u;
      count -= step + 1u;
    } else if (key > target) {
      count = step;
    } else {
      return candidate->amount;
    }
  }
  return 0.0f;
}

vkr_internal float32_t vkr_text_font_get_kerning(
    const VkrFont *font, uint32_t prev_codepoint, uint32_t codepoint,
    const VkrTextResolvedGlyph *previous, const VkrTextResolvedGlyph *current,
    float32_t font_scale) {
  if (font == NULL || !previous->valid || !current->valid) {
    return 0.0f;
  }
  if (current->cooked) {
    return vkr_text_font_get_cooked_kerning(font, previous->glyph_id,
                                            current->glyph_id) *
           font_scale;
  }
  return vkr_text_font_get_legacy_kerning(font, prev_codepoint, codepoint) *
         font_scale;
}

/////////////////////
// Text styling
/////////////////////

VkrTextStyle vkr_text_style_default() {
  return (VkrTextStyle){.font = VKR_FONT_HANDLE_INVALID,
                        .font_data = NULL,
                        .font_size = 16.0f,
                        .color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f},
                        .line_height = 1.0f,
                        .letter_spacing = 0.0f};
}

VkrTextStyle vkr_text_style_new(VkrFontHandle font, float32_t font_size,
                                Vec4 color) {
  VkrTextStyle style = vkr_text_style_default();
  style.font = font;
  style.font_size = font_size;
  style.color = color;
  return style;
}

VkrTextStyle vkr_text_style_with_font_data(const VkrTextStyle *base,
                                           const VkrFont *font_data) {
  VkrTextStyle style = vkr_text_resolve_style(base);
  style.font_data = font_data;
  return style;
}

VkrTextStyle vkr_text_style_with_color(const VkrTextStyle *base, Vec4 color) {
  VkrTextStyle style = vkr_text_resolve_style(base);
  style.color = color;
  return style;
}

VkrTextStyle vkr_text_style_with_size(const VkrTextStyle *base,
                                      float32_t font_size) {
  VkrTextStyle style = vkr_text_resolve_style(base);
  style.font_size = font_size;
  return style;
}

/////////////////////
// Text primitives
/////////////////////

VkrText vkr_text_from_view(String8 content, const VkrTextStyle *style) {
  assert_log(content.str != NULL && content.length > 0, "Invalid content");
  VkrText text = {0};
  text.content = content;
  text.style = vkr_text_resolve_style(style);
  text.owns_content = false_v;
  return text;
}

VkrText vkr_text_from_copy(VkrAllocator *allocator, String8 content,
                           const VkrTextStyle *style) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(content.str != NULL && content.length > 0, "Invalid content");

  VkrText text = {0};
  text.style = vkr_text_resolve_style(style);

  if (allocator != NULL && content.str != NULL && content.length > 0) {
    text.content = string8_duplicate(allocator, &content);
    text.owns_content = true_v;
  } else {
    text.content = content;
    text.owns_content = false_v;
  }

  return text;
}

VkrText vkr_text_from_cstr(const char *cstr, const VkrTextStyle *style) {
  assert_log(cstr != NULL, "Input C-string is NULL");

  uint64_t len = (uint64_t)strlen(cstr);
  String8 view = {(uint8_t *)cstr, len};
  return vkr_text_from_view(view, style);
}

VkrText vkr_text_formatted(VkrAllocator *allocator, const VkrTextStyle *style,
                           const char *fmt, ...) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(fmt != NULL, "Format string is NULL");

  va_list args;
  va_start(args, fmt);
  String8 str = string8_create_formatted_v(allocator, fmt, args);
  va_end(args);

  VkrText text = {0};
  text.content = str;
  text.style = vkr_text_resolve_style(style);
  text.owns_content = true_v;
  return text;
}

void vkr_text_destroy(VkrAllocator *allocator, VkrText *text) {
  if (text == NULL) {
    return;
  }

  if (text->owns_content && text->content.str != NULL && allocator != NULL) {
    vkr_allocator_free(allocator, text->content.str, text->content.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  text->content = (String8){0};
  text->style = vkr_text_style_default();
  text->owns_content = false_v;
}

/////////////////////
// Measurement helpers
/////////////////////

float32_t vkr_text_glyph_width(float32_t font_size) {
  return VKR_TEXT_DEFAULT_GLYPH_WIDTH(font_size);
}

vkr_internal VkrTextBounds vkr_text_measure_internal(const VkrText *text,
                                                     float32_t max_width,
                                                     bool8_t word_wrap) {
  VkrTextBounds bounds = {0};
  if (text == NULL) {
    return bounds;
  }

  VkrTextStyle style = vkr_text_resolve_style(&text->style);
  const VkrFont *font = style.font_data;
  float32_t font_size = vkr_text_resolve_font_size(&style, font);
  float32_t font_scale = vkr_text_font_scale_for_size(font, font_size);
  float32_t ascent = 0.0f;
  float32_t descent = 0.0f;
  float32_t line_gap = 0.0f;
  float32_t metrics_height = 0.0f;
  vkr_text_compute_metrics(&style, font_scale, &ascent, &descent, &line_gap,
                           &metrics_height);
  float32_t line_height_multiplier =
      style.line_height <= 0.0f ? 1.0f : style.line_height;
  float32_t line_height = metrics_height * line_height_multiplier;

  float64_t current_width = 0.0;
  float64_t max_line_width = 0.0;
  uint32_t line_count = 1;
  bool8_t has_prev = false_v;
  uint32_t prev_codepoint = 0;
  VkrTextResolvedGlyph previous = {0};

  VkrCodepointIter iter = vkr_codepoint_iter_begin(&text->content);
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }

    if (cp.value == '\n') {
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0;
      line_count++;
      has_prev = false_v;
      previous = (VkrTextResolvedGlyph){0};
      continue;
    }

    const VkrTextResolvedGlyph current =
        vkr_text_font_resolve_glyph(font, font_size, font_scale, cp.value);
    float64_t glyph_width = (float64_t)current.advance;
    if (style.letter_spacing != 0.0f) {
      glyph_width += style.letter_spacing;
    }

    float64_t kern = 0.0;
    if (has_prev && font != NULL) {
      kern = vkr_text_font_get_kerning(font, prev_codepoint, cp.value,
                                       &previous, &current, font_scale);
    }
    float64_t total_advance = glyph_width + kern;

    if (word_wrap && max_width > 0.0f && current_width > 0.0 &&
        current_width + total_advance > (float64_t)max_width) {
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0;
      line_count++;
      kern = 0.0f;
      total_advance = glyph_width;
      has_prev = false_v;
      previous = (VkrTextResolvedGlyph){0};
    }

    current_width += total_advance;
    prev_codepoint = cp.value;
    previous = current;
    has_prev = true_v;
  }

  max_line_width = Max(max_line_width, current_width);

  bounds.size =
      vec2_new((float32_t)max_line_width, line_height * (float32_t)line_count);
  bounds.ascent = ascent * line_height_multiplier;
  bounds.descent = descent * line_height_multiplier;
  return bounds;
}

VkrTextBounds vkr_text_measure(const VkrText *text) {
  return vkr_text_measure_internal(text, 0.0f, false_v);
}

VkrTextBounds vkr_text_measure_wrapped(const VkrText *text,
                                       float32_t max_width) {
  return vkr_text_measure_internal(text, max_width, true_v);
}

/////////////////////
// Layout
/////////////////////

VkrTextLayoutOptions vkr_text_layout_options_default(void) {
  VkrTextLayoutOptions opts = {0};
  opts.max_width = 0.0f;
  opts.max_height = 0.0f;
  opts.anchor.horizontal = VKR_TEXT_ALIGN_LEFT;
  opts.anchor.vertical = VKR_TEXT_BASELINE_ALPHABETIC;
  opts.word_wrap = true_v;
  opts.clip = false_v;
  return opts;
}

vkr_internal float64_t vkr_text_align_offset(float64_t line_width,
                                             float64_t max_line_width,
                                             VkrTextAlign align) {
  float64_t available = Max(0.0, max_line_width - line_width);
  switch (align) {
  case VKR_TEXT_ALIGN_CENTER:
    return available * 0.5;
  case VKR_TEXT_ALIGN_RIGHT:
    return available;
  case VKR_TEXT_ALIGN_JUSTIFY:
    // Placeholder: proper justify requires extra spacing logic
  case VKR_TEXT_ALIGN_LEFT:
  default:
    return 0.0;
  }
}

typedef struct VkrTextGlyphPlacement {
  uint32_t codepoint;
  uint32_t glyph_id;
  uint32_t glyph_index;
  uint32_t line_index;
  float64_t x_in_line;
  float64_t advance;
  uint8_t page_id;
} VkrTextGlyphPlacement;
Vector(VkrTextGlyphPlacement);

VkrTextLayout vkr_text_layout_compute(VkrAllocator *allocator,
                                      const VkrText *text,
                                      const VkrTextLayoutOptions *options) {
  VkrTextLayout layout = {0};
  if (text == NULL) {
    return layout;
  }

  VkrTextLayoutOptions opts =
      options != NULL ? *options : vkr_text_layout_options_default();
  VkrTextStyle style = vkr_text_resolve_style(&text->style);
  const VkrFont *font = style.font_data;
  float32_t font_size = vkr_text_resolve_font_size(&style, font);
  float32_t font_scale = vkr_text_font_scale_for_size(font, font_size);
  float32_t ascent = 0.0f;
  float32_t descent = 0.0f;
  float32_t line_gap = 0.0f;
  float32_t metrics_height = 0.0f;
  vkr_text_compute_metrics(&style, font_scale, &ascent, &descent, &line_gap,
                           &metrics_height);
  float32_t line_height_multiplier =
      style.line_height <= 0.0f ? 1.0f : style.line_height;
  float32_t line_height = metrics_height * line_height_multiplier;

  uint32_t line_count = 1;
  float64_t max_line_width = 0.0;
  float64_t current_width = 0.0;
  uint32_t line_index = 0;
  bool8_t has_prev = false_v;
  uint32_t prev_codepoint = 0;
  VkrTextResolvedGlyph previous = {0};

  Vector_float64_t line_widths = {0};
  Vector_VkrTextGlyphPlacement placements = {0};
  bool8_t collect_positions = allocator != NULL;
  if (collect_positions) {
    line_widths = vector_create_float64_t_with_capacity(allocator, 8);
    uint64_t glyph_capacity = text->content.length;
    if (glyph_capacity < 8) {
      glyph_capacity = 8;
    }
    placements = vector_create_VkrTextGlyphPlacement_with_capacity(
        allocator, glyph_capacity);
  }

  VkrCodepointIter iter = vkr_codepoint_iter_begin(&text->content);
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }

    if (cp.value == '\n') {
      if (collect_positions) {
        vector_push_float64_t(&line_widths, current_width);
      }
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0;
      line_index++;
      line_count++;
      has_prev = false_v;
      previous = (VkrTextResolvedGlyph){0};
      if (opts.clip && opts.max_height > 0.0f &&
          (float64_t)line_count * (float64_t)line_height >
              (float64_t)opts.max_height) {
        break;
      }
      continue;
    }

    const VkrTextResolvedGlyph current =
        vkr_text_font_resolve_glyph(font, font_size, font_scale, cp.value);
    float64_t glyph_width = (float64_t)current.advance;
    if (style.letter_spacing != 0.0f) {
      glyph_width += style.letter_spacing;
    }

    float64_t kern = 0.0;
    if (has_prev && font != NULL) {
      kern = vkr_text_font_get_kerning(font, prev_codepoint, cp.value,
                                       &previous, &current, font_scale);
    }
    float64_t total_advance = glyph_width + kern;

    if (opts.word_wrap && opts.max_width > 0.0f && current_width > 0.0 &&
        current_width + total_advance > (float64_t)opts.max_width) {
      if (collect_positions) {
        vector_push_float64_t(&line_widths, current_width);
      }
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0;
      line_index++;
      line_count++;
      kern = 0.0f;
      total_advance = glyph_width;
      has_prev = false_v;
      previous = (VkrTextResolvedGlyph){0};
      if (opts.clip && opts.max_height > 0.0f &&
          (float64_t)line_count * (float64_t)line_height >
              (float64_t)opts.max_height) {
        break;
      }
    }

    if (collect_positions) {
      vector_push_VkrTextGlyphPlacement(&placements,
                                        (VkrTextGlyphPlacement){
                                            .codepoint = cp.value,
                                            .glyph_id = current.glyph_id,
                                            .glyph_index = current.glyph_index,
                                            .line_index = line_index,
                                            .x_in_line = current_width + kern,
                                            .advance = total_advance,
                                            .page_id = current.page_id,
                                        });
    }

    current_width += total_advance;
    prev_codepoint = cp.value;
    previous = current;
    has_prev = true_v;
  }

  if (collect_positions) {
    vector_push_float64_t(&line_widths, current_width);
  }
  max_line_width = Max(max_line_width, current_width);

  const float64_t total_height = (float64_t)line_height * (float64_t)line_count;
  float64_t origin_y = 0.0;

  switch (opts.anchor.vertical) {
  case VKR_TEXT_BASELINE_MIDDLE:
    origin_y = -(total_height * 0.5);
    break;
  case VKR_TEXT_BASELINE_BOTTOM:
    origin_y = -total_height;
    break;
  case VKR_TEXT_BASELINE_ALPHABETIC:
    origin_y = -ascent * line_height_multiplier;
    break;
  case VKR_TEXT_BASELINE_TOP:
  default:
    origin_y = 0.0;
    break;
  }

  const float64_t first_baseline =
      origin_y + (float64_t)ascent * (float64_t)line_height_multiplier;
  layout.baseline = vec2_new(0.0f, (float32_t)first_baseline);
  layout.bounds = vec2_new((float32_t)max_line_width, (float32_t)total_height);
  layout.line_count = line_count;
  layout.allocator = allocator;

  if (!collect_positions || placements.length == 0) {
    if (collect_positions) {
      vector_destroy_float64_t(&line_widths);
      vector_destroy_VkrTextGlyphPlacement(&placements);
    }
    return layout;
  }

  const uint32_t glyph_count = (uint32_t)placements.length;
  Array_VkrTextGlyph glyphs = array_create_VkrTextGlyph(allocator, glyph_count);

  for (uint32_t i = 0; i < glyph_count; ++i) {
    const VkrTextGlyphPlacement *p =
        vector_get_VkrTextGlyphPlacement(&placements, (uint64_t)i);

    float64_t line_width = 0.0;
    if (p->line_index < line_widths.length) {
      line_width = *vector_get_float64_t(&line_widths, p->line_index);
    }
    const float64_t align_offset = vkr_text_align_offset(
        line_width, max_line_width, opts.anchor.horizontal);
    const float64_t baseline_y =
        first_baseline + (float64_t)p->line_index * (float64_t)line_height;

    array_set_VkrTextGlyph(
        &glyphs, (uint64_t)i,
        (VkrTextGlyph){
            .codepoint = p->codepoint,
            .glyph_id = p->glyph_id,
            .glyph_index = p->glyph_index,
            .position = vec2_new((float32_t)(align_offset + p->x_in_line),
                                 (float32_t)baseline_y),
            .advance = (float32_t)p->advance,
            .page_id = p->page_id,
        });
  }

  layout.glyphs = glyphs;

  vector_destroy_float64_t(&line_widths);
  vector_destroy_VkrTextGlyphPlacement(&placements);

  return layout;
}

void vkr_text_layout_destroy(VkrTextLayout *layout) {
  if (layout == NULL || layout->allocator == NULL) {
    return;
  }

  array_destroy_VkrTextGlyph(&layout->glyphs);
  layout->bounds = vec2_new(0.0f, 0.0f);
  layout->baseline = vec2_new(0.0f, 0.0f);
  layout->line_count = 0;
  layout->allocator = NULL;
}

/////////////////////
// Rich text
/////////////////////

VkrRichText vkr_rich_text_create(VkrAllocator *allocator, String8 content,
                                 const VkrTextStyle *base_style) {
  VkrRichText rt = {0};
  rt.content = content;
  rt.base_style = vkr_text_resolve_style(base_style);
  rt.allocator = allocator;
  rt.spans = vector_create_VkrTextSpan(allocator);

  return rt;
}

void vkr_rich_text_add_span(VkrRichText *rt, uint64_t start, uint64_t end,
                            const VkrTextStyle *style) {
  assert_log(rt != NULL, "Rich text is NULL");
  assert_log(start <= end, "Start must be <= end");
  assert_log(end <= rt->content.length, "Span end exceeds content length");

  vector_push_VkrTextSpan(&rt->spans,
                          (VkrTextSpan){
                              .start = start,
                              .end = end,
                              .style = vkr_text_resolve_style(style),
                          });
}

void vkr_rich_text_clear_spans(VkrRichText *rt) {
  if (rt == NULL) {
    return;
  }

  vector_clear_VkrTextSpan(&rt->spans);
}

void vkr_rich_text_destroy(VkrRichText *rt) {
  if (rt == NULL) {
    return;
  }

  vector_destroy_VkrTextSpan(&rt->spans);
  rt->allocator = NULL;
}
