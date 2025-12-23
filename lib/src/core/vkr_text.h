/**
 * @file vkr_text.h
 * @brief UTF-8 text primitives, styling, and layout helpers.
 */

#pragma once

#include "containers/array.h"
#include "containers/str.h"
#include "containers/vector.h"
#include "defines.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"
#include "renderer/resources/vkr_resources.h"

/////////////////////
// UTF-8 primitives
/////////////////////

/**
 * @brief A Unicode codepoint.
 * @param value The Unicode codepoint value.
 * @param byte_length The number of bytes consumed to decode the codepoint.
 */
typedef struct VkrCodepoint {
  uint32_t value;      // Unicode codepoint value (U+0000 to U+10FFFF)
  uint8_t byte_length; // Number of bytes consumed (1-4, 0 on error)
} VkrCodepoint;

/**
 * @brief An iterator over a UTF-8 encoded string.
 * @param str The string to iterate over.
 * @param byte_offset The current byte position.
 */
typedef struct VkrCodepointIter {
  const String8 *str;   // Source string being iterated
  uint64_t byte_offset; // Current byte position
} VkrCodepointIter;

/**
 * @brief Decodes a UTF-8 encoded string into a codepoint.
 * @param bytes The bytes to decode.
 * @param max_bytes The maximum number of bytes to decode.
 * @return The decoded codepoint.
 */
VkrCodepoint vkr_utf8_decode(const uint8_t *bytes, uint64_t max_bytes);

/**
 * @brief Encodes a codepoint into a UTF-8 encoded string.
 * @param codepoint The codepoint to encode.
 * @param out The output buffer to write the encoded string to.
 * @param max_bytes The maximum number of bytes available in the output buffer.
 * @return The number of bytes written to the output buffer.
 */
uint8_t vkr_utf8_encode(uint32_t codepoint, uint8_t *out, uint64_t max_bytes);

/**
 * @brief Begins iterating over a UTF-8 encoded string.
 * @param str The string to iterate over.
 * @return The iterator.
 */
VkrCodepointIter vkr_codepoint_iter_begin(const String8 *str);

/**
 * @brief Checks if there are more codepoints to iterate over.
 * @param iter The iterator to check.
 * @return true if there are more codepoints to iterate over, false otherwise.
 */
bool8_t vkr_codepoint_iter_has_next(const VkrCodepointIter *iter);

/**
 * @brief Advances the iterator to the next codepoint.
 * @param iter The iterator to advance.
 * @return The next codepoint.
 */
VkrCodepoint vkr_codepoint_iter_next(VkrCodepointIter *iter);

/**
 * @brief Peeks at the next codepoint without advancing the iterator.
 * @param iter The iterator to peek at.
 * @return The next codepoint.
 */
VkrCodepoint vkr_codepoint_iter_peek(const VkrCodepointIter *iter);

/**
 * @brief Counts the number of codepoints in a UTF-8 encoded string.
 * @param str The string to count the codepoints of.
 * @return The number of codepoints in the string.
 */
uint64_t vkr_string8_codepoint_count(const String8 *str);

/**
 * @brief Checks if a UTF-8 encoded string is valid.
 * @param str The string to check.
 * @return true if the string is valid, false otherwise.
 */
bool8_t vkr_string8_is_valid_utf8(const String8 *str);

/////////////////////
// Text styling
/////////////////////

/**
 * @brief A text style configuration.
 * @param font The font handle.
 * @param font_data Optional pointer to resolved font data (bitmap/system).
 * @param font_size The font size in points (for scaling from font's native
 * size).
 * @param color The text color.
 * @param line_height The line height multiplier (1.0 = use font's native line
 * height).
 * @param letter_spacing The letter spacing.
 */
typedef struct VkrTextStyle {
  VkrFontHandle font;       // Font resource handle (id + generation)
  const VkrFont *font_data; // Optional resolved font data
  float32_t font_size;      // Font size in points
  Vec4 color;               // RGBA text color
  float32_t line_height;    // Line height multiplier (1.0 = normal)
  float32_t letter_spacing; // Extra spacing between glyphs (pixels)
} VkrTextStyle;

/**
 * @brief The default text style.
 * @return The default text style.
 */
VkrTextStyle vkr_text_style_default();

/**
 * @brief Creates a new text style.
 * @param font The font handle.
 * @param font_size The font size.
 * @param color The text color.
 * @return The new text style.
 */
VkrTextStyle vkr_text_style_new(VkrFontHandle font, float32_t font_size,
                                Vec4 color);

/**
 * @brief Creates a new text style with resolved font data.
 * @param base The base text style.
 * @param font_data The resolved font data (optional).
 * @return The new text style.
 */
VkrTextStyle vkr_text_style_with_font_data(const VkrTextStyle *base,
                                           const VkrFont *font_data);

/**
 * @brief Creates a new text style with a specific color.
 * @param base The base text style.
 * @param color The text color.
 * @return The new text style.
 */
VkrTextStyle vkr_text_style_with_color(const VkrTextStyle *base, Vec4 color);

/**
 * @brief Creates a new text style with a specific font size.
 * @param base The base text style.
 * @param font_size The font size.
 * @return The new text style.
 */
VkrTextStyle vkr_text_style_with_size(const VkrTextStyle *base,
                                      float32_t font_size);

/////////////////////
// Text primitives
/////////////////////

/**
 * @brief A text primitive.
 * @param content The text content.
 * @param style The text style.
 * @param owns_content true if the content is owned by the text.
 */
typedef struct VkrText {
  String8 content;
  VkrTextStyle style;
  bool8_t owns_content;
} VkrText;

/**
 * @brief A text span.
 * @param start The start byte offset.
 * @param end The end byte offset.
 * @param style The text style.
 */
typedef struct VkrTextSpan {
  uint64_t start;
  uint64_t end;
  VkrTextStyle style;
} VkrTextSpan;
Vector(VkrTextSpan);

/**
 * @brief A text bounds.
 * @param size The size of the text bounds.
 * @param ascent The ascent of the text bounds.
 * @param descent The descent of the text bounds.
 */
typedef struct VkrTextBounds {
  Vec2 size;
  float32_t ascent;
  float32_t descent;
} VkrTextBounds;

/**
 * @brief Creates a new text from a view.
 * @param content The text content.
 * @param style The text style.
 * @return The new text.
 */
VkrText vkr_text_from_view(String8 content, const VkrTextStyle *style);

/**
 * @brief Creates a new text from a copy.
 * @param allocator The allocator to use.
 * @param content The text content.
 * @param style The text style.
 * @return The new text.
 */
VkrText vkr_text_from_copy(VkrAllocator *allocator, String8 content,
                           const VkrTextStyle *style);

/**
 * @brief Creates a new text from a C-string.
 * @param cstr The C-string to create the text from.
 * @param style The text style.
 * @return The new text.
 */
VkrText vkr_text_from_cstr(const char *cstr, const VkrTextStyle *style);

/**
 * @brief Creates a new text from a formatted string.
 * @param allocator The allocator to use.
 * @param style The text style.
 * @param fmt The format string.
 * @param ... The arguments to the format string.
 * @return The new text.
 */
VkrText vkr_text_formatted(VkrAllocator *allocator, const VkrTextStyle *style,
                           const char *fmt, ...);

/**
 * @brief Destroys a text.
 * @param allocator The allocator to use.
 * @param text The text to destroy.
 */
void vkr_text_destroy(VkrAllocator *allocator, VkrText *text);

/////////////////////
// Alignment & anchor
/////////////////////

/**
 * @brief A text alignment.
 * @param horizontal The horizontal alignment.
 * @param vertical The vertical alignment.
 */
typedef enum VkrTextAlign {
  VKR_TEXT_ALIGN_LEFT = 0, // Left align text
  VKR_TEXT_ALIGN_CENTER,   // Center align text
  VKR_TEXT_ALIGN_RIGHT,    // Right align text
  VKR_TEXT_ALIGN_JUSTIFY,  // Justify text
} VkrTextAlign;

/**
 * @brief A text baseline.
 * @param top The top baseline.
 * @param middle The middle baseline.
 * @param bottom The bottom baseline.
 * @param alphabetic The alphabetic baseline.
 */
typedef enum VkrTextBaseline {
  VKR_TEXT_BASELINE_TOP = 0,    // Top baseline
  VKR_TEXT_BASELINE_MIDDLE,     // Middle baseline
  VKR_TEXT_BASELINE_BOTTOM,     // Bottom baseline
  VKR_TEXT_BASELINE_ALPHABETIC, // Alphabetic baseline
} VkrTextBaseline;

/**
 * @brief A text anchor.
 * @param horizontal The horizontal alignment.
 * @param vertical The vertical alignment.
 */
typedef struct VkrTextAnchor {
  VkrTextAlign horizontal;  // Horizontal alignment
  VkrTextBaseline vertical; // Vertical alignment
} VkrTextAnchor;

/////////////////////
// Measurement & layout
/////////////////////

/**
 * @brief Gets the width of a glyph.
 * @param font_size The font size.
 * @return The width of the glyph.
 */
float32_t vkr_text_glyph_width(float32_t font_size);

/**
 * @brief Measures the bounds of a text.
 * @param text The text to measure.
 * @return The bounds of the text.
 */
VkrTextBounds vkr_text_measure(const VkrText *text);

/**
 * @brief Measures the bounds of a text with word wrap.
 * @param text The text to measure.
 * @param max_width The maximum width of the text.
 * @return The bounds of the text.
 */
VkrTextBounds vkr_text_measure_wrapped(const VkrText *text,
                                       float32_t max_width);

/**
 * @brief A text glyph.
 * @param codepoint The codepoint of the glyph.
 * @param position The position of the glyph.
 * @param advance The advance of the glyph.
 * @param page_id The atlas page id of the glyph.
 */
typedef struct VkrTextGlyph {
  uint32_t codepoint; // Unicode codepoint
  Vec2 position;      // Baseline position for this glyph
  float32_t advance;  // Advance used during layout
  uint8_t page_id;    // Atlas page id for this glyph
} VkrTextGlyph;
Array(VkrTextGlyph);

/**
 * @brief A text layout options.
 * @param max_width The maximum width of the text.
 * @param max_height The maximum height of the text.
 * @param anchor The anchor of the text.
 * @param word_wrap The word wrap flag.
 * @param clip The clip flag.
 */
typedef struct VkrTextLayoutOptions {
  float32_t max_width;  // Maximum width of the text
  float32_t max_height; // Maximum height of the text
  VkrTextAnchor anchor; // Anchor of the text
  bool8_t word_wrap;    // Word wrap flag
  bool8_t clip;         // Clip flag
} VkrTextLayoutOptions;

/**
 * @brief A text layout.
 * @param bounds The bounds of the text.
 * @param baseline The baseline of the text.
 * @param line_count The number of lines in the text.
 * @param glyphs The glyphs in the text.
 * @param allocator The allocator to use.
 */
typedef struct VkrTextLayout {
  Vec2 bounds;               // Total width/height of laid-out text
  Vec2 baseline;             // Baseline of the first line relative to origin
  uint32_t line_count;       // Number of lines after layout
  Array_VkrTextGlyph glyphs; // Glyph positions (owned by layout)
  VkrAllocator *allocator;   // Allocator used for memory management
} VkrTextLayout;

/**
 * @brief Gets the default text layout options.
 * @return The default text layout options.
 */
VkrTextLayoutOptions vkr_text_layout_options_default();

/**
 * @brief Computes a text layout.
 * @param allocator The allocator to use.
 * @param text The text to layout.
 * @param options The layout options.
 * @return The text layout.
 */
VkrTextLayout vkr_text_layout_compute(VkrAllocator *allocator,
                                      const VkrText *text,
                                      const VkrTextLayoutOptions *options);

/**
 * @brief Destroys a text layout.
 * @param layout The text layout to destroy.
 */
void vkr_text_layout_destroy(VkrTextLayout *layout);

/////////////////////
// Rich text
/////////////////////

/**
 * @brief A rich text.
 * @param content The text content.
 * @param base_style The base text style.
 * @param spans The spans in the rich text.
 * @param allocator The allocator to use.
 */
typedef struct VkrRichText {
  String8 content;          // Full text content
  VkrTextStyle base_style;  // Default style for unstyled regions
  Vector_VkrTextSpan spans; // Vector of styled spans
  VkrAllocator *allocator;  // Allocator used for memory management
} VkrRichText;

/**
 * @brief Creates a new rich text.
 * @param allocator The allocator to use.
 * @param content The text content.
 * @param base_style The base text style.
 * @return The new rich text.
 */
VkrRichText vkr_rich_text_create(VkrAllocator *allocator, String8 content,
                                 const VkrTextStyle *base_style);

/**
 * @brief Adds a span to a rich text.
 * @param rt The rich text to add the span to.
 * @param start The start byte offset.
 * @param end The end byte offset.
 * @param style The text style.
 */
void vkr_rich_text_add_span(VkrRichText *rt, uint64_t start, uint64_t end,
                            const VkrTextStyle *style);

/**
 * @brief Clears the spans from a rich text.
 * @param rt The rich text to clear the spans from.
 */
void vkr_rich_text_clear_spans(VkrRichText *rt);

/**
 * @brief Destroys a rich text.
 * @param rt The rich text to destroy.
 */
void vkr_rich_text_destroy(VkrRichText *rt);

/////////////////////
// Convenience
/////////////////////

/**
 * @brief Creates a new text from a C-string.
 * @param str The C-string to create the text from.
 * @return The new text.
 */
#define vkr_text_lit(str) vkr_text_from_cstr(str, NULL)

/**
 * @brief The white text color.
 * @return The white text color.
 */
#define VKR_TEXT_COLOR_WHITE (Vec4){1.0f, 1.0f, 1.0f, 1.0f}

/**
 * @brief The black text color.
 * @return The black text color.
 */
#define VKR_TEXT_COLOR_BLACK (Vec4){0.0f, 0.0f, 0.0f, 1.0f}

/**
 * @brief The red text color.
 * @return The red text color.
 */
#define VKR_TEXT_COLOR_RED (Vec4){1.0f, 0.0f, 0.0f, 1.0f}

/**
 * @brief The green text color.
 * @return The green text color.
 */
#define VKR_TEXT_COLOR_GREEN (Vec4){0.0f, 1.0f, 0.0f, 1.0f}

/**
 * @brief The blue text color.
 * @return The blue text color.
 */
#define VKR_TEXT_COLOR_BLUE (Vec4){0.0f, 0.0f, 1.0f, 1.0f}

/**
 * @brief The yellow text color.
 * @return The yellow text color.
 */
#define VKR_TEXT_COLOR_YELLOW (Vec4){1.0f, 1.0f, 0.0f, 1.0f}
