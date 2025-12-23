#include "core/vkr_text.h"

#include <stdio.h>

#include "core/logger.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

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
  int32_t written = snprintf(buffer, buffer_size, "%u", codepoint);
  if (written <= 0 || (uint64_t)written >= buffer_size) {
    buffer[0] = '\0';
    return false_v;
  }
  return true_v;
}

vkr_internal const VkrFontGlyph *vkr_text_font_find_glyph(const VkrFont *font,
                                                          uint32_t codepoint) {
  if (font == NULL || font->glyphs.data == NULL) {
    return NULL;
  }
  if (font->glyph_indices.entries != NULL && font->glyph_indices.size > 0) {
    char key[16];
    if (vkr_text_codepoint_key(key, sizeof(key), codepoint)) {
      uint32_t *index = vkr_hash_table_get_uint32_t(&font->glyph_indices, key);
      if (index && *index < font->glyphs.length) {
        return &font->glyphs.data[*index];
      }
    }
  }
  for (uint64_t i = 0; i < font->glyphs.length; ++i) {
    if (font->glyphs.data[i].codepoint == codepoint) {
      return &font->glyphs.data[i];
    }
  }
  return NULL;
}

vkr_internal int32_t vkr_text_font_get_kerning(const VkrFont *font,
                                               uint32_t prev_codepoint,
                                               uint32_t codepoint) {
  if (font == NULL || font->kernings.data == NULL) {
    return 0;
  }
  for (uint64_t i = 0; i < font->kernings.length; ++i) {
    const VkrFontKerning *kerning = &font->kernings.data[i];
    if (kerning->codepoint_0 == prev_codepoint &&
        kerning->codepoint_1 == codepoint) {
      return (int32_t)kerning->amount;
    }
  }
  return 0;
}

vkr_internal float32_t vkr_text_glyph_base_advance(const VkrTextStyle *style,
                                                   float32_t font_size,
                                                   float32_t font_scale,
                                                   uint32_t codepoint) {
  const VkrFont *font = style->font_data;
  if (font == NULL) {
    return VKR_TEXT_DEFAULT_GLYPH_WIDTH(style->font_size);
  }

  if (codepoint == '\t') {
    return font->tab_x_advance * font_scale;
  }

  const VkrFontGlyph *glyph = vkr_text_font_find_glyph(font, codepoint);
  if (glyph != NULL) {
    return (float32_t)glyph->x_advance * font_scale;
  }

  return VKR_TEXT_DEFAULT_GLYPH_WIDTH(font_size);
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

  float32_t current_width = 0.0f;
  float32_t max_line_width = 0.0f;
  uint32_t line_count = 1;
  bool8_t has_prev = false_v;
  uint32_t prev_codepoint = 0;

  VkrCodepointIter iter = vkr_codepoint_iter_begin(&text->content);
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }

    if (cp.value == '\n') {
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0f;
      line_count++;
      has_prev = false_v;
      continue;
    }

    float32_t glyph_width =
        vkr_text_glyph_base_advance(&style, font_size, font_scale, cp.value);
    if (style.letter_spacing != 0.0f) {
      glyph_width += style.letter_spacing;
    }

    float32_t kern = 0.0f;
    if (has_prev && font != NULL) {
      kern =
          (float32_t)vkr_text_font_get_kerning(font, prev_codepoint, cp.value) *
          font_scale;
    }
    float32_t total_advance = glyph_width + kern;

    if (word_wrap && max_width > 0.0f && current_width > 0.0f &&
        current_width + total_advance > max_width) {
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0f;
      line_count++;
      kern = 0.0f;
      total_advance = glyph_width;
      has_prev = false_v;
    }

    current_width += total_advance;
    prev_codepoint = cp.value;
    has_prev = true_v;
  }

  max_line_width = Max(max_line_width, current_width);

  bounds.size = vec2_new(max_line_width, line_height * (float32_t)line_count);
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

vkr_internal float32_t vkr_text_align_offset(float32_t line_width,
                                             float32_t max_line_width,
                                             VkrTextAlign align) {
  float32_t available = Max(0.0f, max_line_width - line_width);
  switch (align) {
  case VKR_TEXT_ALIGN_CENTER:
    return available * 0.5f;
  case VKR_TEXT_ALIGN_RIGHT:
    return available;
  case VKR_TEXT_ALIGN_JUSTIFY:
    // Placeholder: proper justify requires extra spacing logic
  case VKR_TEXT_ALIGN_LEFT:
  default:
    return 0.0f;
  }
}

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

  uint64_t cp_capacity = vkr_string8_codepoint_count(&text->content);
  uint32_t line_count = 1;
  float32_t max_line_width = 0.0f;
  float32_t current_width = 0.0f;
  uint32_t glyph_count = 0;
  uint32_t line_index = 0;
  bool8_t has_prev = false_v;
  uint32_t prev_codepoint = 0;

  float *line_widths = NULL;
  uint64_t line_capacity = cp_capacity + 1;
  uint64_t line_widths_size = line_capacity * sizeof(float32_t);

  if (allocator != NULL && line_capacity > 0) {
    line_widths = vkr_allocator_alloc(allocator, line_widths_size,
                                      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    assert_log(line_widths != NULL, "Failed to allocate line widths buffer");
    MemZero(line_widths, line_widths_size);
  }

  VkrCodepointIter iter = vkr_codepoint_iter_begin(&text->content);
  while (vkr_codepoint_iter_has_next(&iter)) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }

    if (cp.value == '\n') {
      if (line_widths != NULL && line_index < line_capacity) {
        line_widths[line_index] = current_width;
      }
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0f;
      line_index++;
      line_count++;
      has_prev = false_v;
      continue;
    }

    float32_t glyph_width =
        vkr_text_glyph_base_advance(&style, font_size, font_scale, cp.value);
    if (style.letter_spacing != 0.0f) {
      glyph_width += style.letter_spacing;
    }

    float32_t kern = 0.0f;
    if (has_prev && font != NULL) {
      kern =
          (float32_t)vkr_text_font_get_kerning(font, prev_codepoint, cp.value) *
          font_scale;
    }
    float32_t total_advance = glyph_width + kern;

    if (opts.word_wrap && opts.max_width > 0.0f && current_width > 0.0f &&
        current_width + total_advance > opts.max_width) {
      if (line_widths != NULL && line_index < line_capacity) {
        line_widths[line_index] = current_width;
      }
      max_line_width = Max(max_line_width, current_width);
      current_width = 0.0f;
      line_index++;
      line_count++;
      kern = 0.0f;
      total_advance = glyph_width;
      has_prev = false_v;
      if (opts.clip && opts.max_height > 0.0f &&
          (float32_t)line_count * line_height > opts.max_height) {
        break;
      }
    }

    current_width += total_advance;
    glyph_count++;
    prev_codepoint = cp.value;
    has_prev = true_v;
  }

  if (line_widths != NULL && line_index < line_capacity) {
    line_widths[line_index] = current_width;
  }
  max_line_width = Max(max_line_width, current_width);

  float32_t total_height = line_height * (float32_t)line_count;
  float32_t origin_y = 0.0f;

  switch (opts.anchor.vertical) {
  case VKR_TEXT_BASELINE_MIDDLE:
    origin_y = -(total_height * 0.5f);
    break;
  case VKR_TEXT_BASELINE_BOTTOM:
    origin_y = -total_height;
    break;
  case VKR_TEXT_BASELINE_ALPHABETIC:
    origin_y = -ascent * line_height_multiplier;
    break;
  case VKR_TEXT_BASELINE_TOP:
  default:
    origin_y = 0.0f;
    break;
  }

  float32_t first_baseline = origin_y + ascent * line_height_multiplier;
  layout.baseline = vec2_new(0.0f, first_baseline);
  layout.bounds = vec2_new(max_line_width, total_height);
  layout.line_count = line_count;
  layout.allocator = allocator;

  if (glyph_count == 0 || allocator == NULL) {
    if (line_widths != NULL) {
      vkr_allocator_free(allocator, line_widths, line_widths_size,
                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    }
    return layout;
  }

  Array_VkrTextGlyph glyphs = array_create_VkrTextGlyph(allocator, glyph_count);

  // Second pass: place glyphs using recorded line widths
  iter = vkr_codepoint_iter_begin(&text->content);
  line_index = 0;
  current_width = 0.0f;
  float32_t line_width = line_widths != NULL ? line_widths[line_index] : 0.0f;
  float32_t align_offset =
      vkr_text_align_offset(line_width, max_line_width, opts.anchor.horizontal);
  float32_t baseline_y = first_baseline;
  uint32_t written = 0;
  has_prev = false_v;
  prev_codepoint = 0;

  while (vkr_codepoint_iter_has_next(&iter) && written < glyph_count) {
    VkrCodepoint cp = vkr_codepoint_iter_next(&iter);
    if (cp.byte_length == 0) {
      continue;
    }

    if (cp.value == '\n') {
      line_index++;
      current_width = 0.0f;
      baseline_y += line_height;
      line_width = line_widths != NULL && line_index < line_capacity
                       ? line_widths[line_index]
                       : 0.0f;
      align_offset = vkr_text_align_offset(line_width, max_line_width,
                                           opts.anchor.horizontal);
      has_prev = false_v;
      continue;
    }

    float32_t glyph_width =
        vkr_text_glyph_base_advance(&style, font_size, font_scale, cp.value);
    if (style.letter_spacing != 0.0f) {
      glyph_width += style.letter_spacing;
    }

    float32_t kern = 0.0f;
    if (has_prev && font != NULL) {
      kern =
          (float32_t)vkr_text_font_get_kerning(font, prev_codepoint, cp.value) *
          font_scale;
    }
    float32_t total_advance = glyph_width + kern;

    if (opts.word_wrap && opts.max_width > 0.0f && current_width > 0.0f &&
        current_width + total_advance > opts.max_width) {
      line_index++;
      current_width = 0.0f;
      baseline_y += line_height;
      if (opts.clip && opts.max_height > 0.0f &&
          (float32_t)(line_index + 1) * line_height > opts.max_height) {
        break;
      }

      line_width = line_widths != NULL && line_index < line_capacity
                       ? line_widths[line_index]
                       : 0.0f;
      align_offset = vkr_text_align_offset(line_width, max_line_width,
                                           opts.anchor.horizontal);
      kern = 0.0f;
      total_advance = glyph_width;
      has_prev = false_v;
    }

    uint8_t page_id = 0;
    if (font != NULL) {
      const VkrFontGlyph *font_glyph = vkr_text_font_find_glyph(font, cp.value);
      if (font_glyph != NULL) {
        page_id = font_glyph->page_id;
      }
    }

    Vec2 pos = vec2_new(align_offset + current_width + kern, baseline_y);

    array_set_VkrTextGlyph(&glyphs, written,
                           (VkrTextGlyph){
                               .codepoint = cp.value,
                               .position = pos,
                               .advance = total_advance,
                               .page_id = page_id,
                           });

    current_width += total_advance;
    written++;
    prev_codepoint = cp.value;
    has_prev = true_v;
  }

  layout.glyphs = glyphs;

  if (line_widths != NULL) {
    vkr_allocator_free(allocator, line_widths, line_widths_size,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }

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
