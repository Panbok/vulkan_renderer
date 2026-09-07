#pragma once

#include "core/vkr_text.h"
#include "renderer/systems/vkr_font_system.h"

// =============================================================================
// UI Text Types
// =============================================================================

/**
 * @brief Configuration for creating/updating UI text.
 *
 * This contains the INPUT parameters for text rendering.
 * Layout and bounds are computed internally.
 */
typedef struct VkrUiTextConfig {
  VkrFontHandle font; // Font to use (or invalid for default)
  /** Authored sRGB RGB and linear alpha. */
  Vec4 color;
  float32_t font_size;      // Authored logical units per em (0 = font default)
  float32_t letter_spacing; // Extra authored logical units between glyphs
  VkrTextLayoutOptions layout; // Word wrap, max dimensions, anchor
  // Bitmap-only atlas bleed inset. MTSDF generator bounds remain exact.
  float32_t uv_inset_px;
} VkrUiTextConfig;

/**
 * @brief Default UI text configuration.
 */
#define VKR_UI_TEXT_CONFIG_DEFAULT                                             \
  (VkrUiTextConfig) {                                                          \
    .font = VKR_FONT_HANDLE_INVALID, .color = {1.0f, 1.0f, 1.0f, 1.0f},        \
    .font_size = 0.0f, .letter_spacing = 0.0f,                                 \
    .layout =                                                                  \
        {                                                                      \
            .max_width = 0.0f,                                                 \
            .max_height = 0.0f,                                                \
            .anchor = {VKR_TEXT_ALIGN_LEFT, VKR_TEXT_BASELINE_TOP},            \
            .word_wrap = false_v,                                              \
            .clip = false_v,                                                   \
        },                                                                     \
    .uv_inset_px = 0.0f,                                                       \
  }

/** CPU-owned shaped geometry shared by backend lowering paths. */
typedef struct VkrUiTextGeometry {
  VkrTextVertex *vertices;
  uint32_t *indices;
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t vertex_capacity;
  uint32_t index_capacity;
  uint32_t revision;
} VkrUiTextGeometry;

/**
 * @brief UI text resource.
 *
 * Owns the text content, computed layout, and shaped packet geometry.
 */
typedef struct VkrUiText {
  // Dependencies
  VkrFontSystem *font_system;
  VkrAllocator *allocator;

  // Content & config
  String8 content; // Owned text content
  VkrUiTextConfig config;
  float32_t content_scale; // Authored logical units to device pixels
  Vec4 linear_color;       // Retained decode of config.color.
  VkrTransform transform;  // Position/rotation/scale

  // Computed state
  VkrTextLayout layout;   // Computed glyph positions
  VkrTextBounds bounds;   // Computed text bounds
  VkrFont *resolved_font; // Cached font pointer

  VkrUiTextGeometry geometry;

  // Dirty flags
  bool8_t layout_dirty;  // Need to recompute layout
  bool8_t buffers_dirty; // Need to regenerate shaped CPU geometry
} VkrUiText;
Vector(VkrUiText);

// =============================================================================
// UI Text API
// =============================================================================

/**
 * @brief Creates a UI text instance.
 * @param allocator The allocator for memory management.
 * @param font_system The font system.
 * @param content Initial text content (copied).
 * @param config Initial configuration (or NULL for defaults).
 * @param out_text Output text instance.
 * @param out_error Error output.
 * @return true on success.
 */
bool8_t vkr_ui_text_create(VkrAllocator *allocator, VkrFontSystem *font_system,
                           String8 content, const VkrUiTextConfig *config,
                           VkrUiText *out_text, VkrRendererError *out_error);

/**
 * @brief Destroys a UI text instance and releases all resources.
 */
void vkr_ui_text_destroy(VkrUiText *text);

/**
 * @brief Updates the text content.
 * @param text The UI text instance.
 * @param content New text content (copied).
 * @return true on success.
 */
bool8_t vkr_ui_text_set_content(VkrUiText *text, String8 content);

/**
 * @brief Updates the text configuration.
 */
void vkr_ui_text_set_config(VkrUiText *text, const VkrUiTextConfig *config);

/** Applies logical-to-device scale before the next layout and geometry build.
 */
void vkr_ui_text_set_content_scale(VkrUiText *text, float32_t content_scale);

/**
 * @brief Sets the text position in device pixels.
 */
void vkr_ui_text_set_position(VkrUiText *text, Vec2 position);

/**
 * @brief Sets the text color.
 */
void vkr_ui_text_set_color(VkrUiText *text, Vec4 color);

/**
 * @brief Gets the computed text bounds.
 */
VkrTextBounds vkr_ui_text_get_bounds(VkrUiText *text);

/** Prepares shaped CPU geometry without issuing renderer API calls. */
bool8_t vkr_ui_text_prepare_geometry(VkrUiText *text);
