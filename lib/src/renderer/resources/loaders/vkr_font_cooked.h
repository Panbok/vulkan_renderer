#pragma once

#include "containers/str.h"
#include "memory/vkr_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VKR_FONT_COOKED_MAGIC 0x41464B56u /* little-endian 'VKFA' */
#define VKR_FONT_COOKED_VERSION 1u
#define VKR_FONT_COOKED_ENDIAN_TAG 0x01020304u
#define VKR_FONT_COOKED_HEADER_SIZE 192u
#define VKR_FONT_COOKED_SECTION_COUNT 6u
#define VKR_FONT_COOKED_SECTION_SIZE 32u
#define VKR_FONT_COOKED_DIRECTORY_OFFSET VKR_FONT_COOKED_HEADER_SIZE
#define VKR_FONT_COOKED_DIRECTORY_SIZE                                         \
  (VKR_FONT_COOKED_SECTION_COUNT * VKR_FONT_COOKED_SECTION_SIZE)
#define VKR_FONT_COOKED_DATA_OFFSET                                            \
  (VKR_FONT_COOKED_DIRECTORY_OFFSET + VKR_FONT_COOKED_DIRECTORY_SIZE)
#define VKR_FONT_COOKED_ALIGNMENT 16u
#define VKR_FONT_COOKED_GLYPH_SIZE 48u
#define VKR_FONT_COOKED_CODEPOINT_SIZE 8u
#define VKR_FONT_COOKED_KERNING_SIZE 16u
#define VKR_FONT_COOKED_PAGE_SIZE 48u
#define VKR_FONT_COOKED_HEADER_CRC_OFFSET 132u
#define VKR_FONT_COOKED_DIRECTORY_CRC_OFFSET 136u
#define VKR_FONT_COOKED_PAYLOAD_CRC_OFFSET 140u
#define VKR_FONT_COOKED_MAX_FILE_SIZE GB(1)
#define VKR_FONT_COOKED_MAX_GLYPHS 65536u
#define VKR_FONT_COOKED_MAX_CODEPOINTS 65536u
#define VKR_FONT_COOKED_MAX_KERNINGS 1048576u
#define VKR_FONT_COOKED_MAX_PAGES 16u
#define VKR_FONT_COOKED_MAX_FACE_BYTES 255u

typedef enum VkrFontCookedFieldKind {
  VKR_FONT_COOKED_FIELD_MTSDF = 1,
} VkrFontCookedFieldKind;

typedef enum VkrFontCookedPixelFormat {
  VKR_FONT_COOKED_PIXEL_RGBA8_UNORM = 1,
} VkrFontCookedPixelFormat;

typedef enum VkrFontCookedSectionKind {
  VKR_FONT_COOKED_SECTION_FACE = 1,
  VKR_FONT_COOKED_SECTION_GLYPHS = 2,
  VKR_FONT_COOKED_SECTION_CODEPOINTS = 3,
  VKR_FONT_COOKED_SECTION_KERNINGS = 4,
  VKR_FONT_COOKED_SECTION_PAGES = 5,
  VKR_FONT_COOKED_SECTION_PIXELS = 6,
} VkrFontCookedSectionKind;

typedef enum VkrFontCookedGlyphFlags {
  VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY = 1u << 0,
} VkrFontCookedGlyphFlags;

typedef struct VkrFontCookedMetrics {
  float32_t line_height;
  float32_t ascender;
  float32_t descender;
  float32_t underline_y;
  float32_t underline_thickness;
  float32_t distance_range;
  float32_t atlas_px_per_em;
  uint32_t units_per_em;
} VkrFontCookedMetrics;

typedef struct VkrFontCookedGlyph {
  uint32_t glyph_id;
  uint32_t page_index;
  uint32_t flags;
  float32_t advance;
  float32_t plane_left;
  float32_t plane_bottom;
  float32_t plane_right;
  float32_t plane_top;
  float32_t uv_left;
  float32_t uv_bottom;
  float32_t uv_right;
  float32_t uv_top;
} VkrFontCookedGlyph;

typedef struct VkrFontCookedCodepoint {
  uint32_t codepoint;
  uint32_t glyph_id;
} VkrFontCookedCodepoint;

typedef struct VkrFontCookedKerning {
  uint32_t left_glyph_id;
  uint32_t right_glyph_id;
  float32_t amount;
} VkrFontCookedKerning;

typedef struct VkrFontCookedPage {
  uint32_t width;
  uint32_t height;
  uint32_t row_stride;
  VkrFontCookedPixelFormat pixel_format;
  const uint8_t *pixels;
  uint64_t pixel_size;
} VkrFontCookedPage;

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

/**
 * Result-owned semantic tables decoded from a completely validated artifact.
 * Page pixel pointers borrow the input byte buffer and remain valid only while
 * that buffer remains alive; this prevents the runtime from retaining a second
 * CPU atlas after upload finalization.
 */
typedef struct VkrFontCookedDecoded {
  String8 face;
  uint8_t identity[32];
  uint32_t cooker_version;
  VkrFontCookedFieldKind field_kind;
  uint32_t fallback_glyph_id;
  VkrFontCookedMetrics metrics;
  VkrFontCookedGlyph *glyphs;
  uint32_t glyph_count;
  VkrFontCookedCodepoint *codepoints;
  uint32_t codepoint_count;
  VkrFontCookedKerning *kernings;
  uint32_t kerning_count;
  VkrFontCookedPage *pages;
  uint32_t page_count;
} VkrFontCookedDecoded;

typedef struct VkrFontCookedInspection {
  uint8_t identity[32];
  uint64_t file_size;
  uint32_t cooker_version;
  uint32_t glyph_count;
  uint32_t codepoint_count;
  uint32_t kerning_count;
  uint32_t page_count;
} VkrFontCookedInspection;

/** Serializes each field explicitly; no native structure image is written. */
bool8_t vkr_font_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrFontCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size);

/**
 * Validates header, directory, section bounds/non-overlap, and every checksum
 * without allocating semantic tables. Used by the cooker skip path.
 */
bool8_t vkr_font_cooked_inspect(const uint8_t *data, uint64_t size,
                                VkrFontCookedInspection *out_inspection);

/** Validates the complete v1 semantic contract before returning any table. */
bool8_t vkr_font_cooked_decode(VkrAllocator *result_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrFontCookedDecoded *out_decoded);

/** Writes a sibling temporary file and atomically replaces the destination. */
bool8_t vkr_font_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size);

#ifdef __cplusplus
}
#endif
