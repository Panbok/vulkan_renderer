#pragma once

#include "assets/vkr_font_cooked.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VkrFontCookedEncodeInfo {
  String8 face;
  uint8_t identity[32];
  uint32_t cooker_version;
  VkrFontCookedFieldKind field_kind;
  uint32_t fallback_glyph_id;
  VkrFontCookedMetrics metrics;
  const VkrFontCookedGlyph *glyphs;
  uint32_t glyph_count;
  const VkrFontCookedCodepoint *codepoints;
  uint32_t codepoint_count;
  const VkrFontCookedKerning *kernings;
  uint32_t kerning_count;
  const VkrFontCookedPage *pages;
  uint32_t page_count;
} VkrFontCookedEncodeInfo;

/** Serializes each field explicitly; no native structure image is written. */
bool8_t vkr_font_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrFontCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size);

/** Writes a sibling temporary file and atomically replaces the destination. */
bool8_t vkr_font_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size);

#ifdef __cplusplus
}
#endif
