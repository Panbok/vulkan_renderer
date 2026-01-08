#pragma once

#include "core/vkr_text.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_buffer.h"

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
  VkrFontHandle font;          // Font to use (or invalid for default)
  Vec4 color;                  // Text color (RGBA)
  float32_t font_size;         // Font size in points (0 = use font's native)
  float32_t letter_spacing;    // Extra spacing between glyphs
  VkrTextLayoutOptions layout; // Word wrap, max dimensions, anchor
  float32_t uv_inset_px; // Half-texel inset (in atlas pixels) to avoid bleeding
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

/**
 * @brief Internal render state for UI text.
 */
typedef struct VkrUiTextRenderState {
  VkrPipelineHandle pipeline;
  VkrVertexBuffer vertex_buffer;
  VkrIndexBuffer index_buffer;
  VkrRendererInstanceStateHandle instance_state;
  uint32_t quad_count;      // Number of glyph quads
  uint32_t vertex_capacity; // Allocated vertex count
  uint32_t index_capacity;  // Allocated index count
  uint64_t last_frame_rendered;
} VkrUiTextRenderState;

/**
 * @brief Retired buffer set waiting for GPU completion.
 *
 * UI text can resize its dynamic vertex/index buffers when content grows.
 * To avoid destroying buffers that may still be referenced by in-flight command
 * buffers, old buffers are retained for a few frames and destroyed later.
 */
typedef struct VkrUiTextRetiredBufferSet {
  VkrVertexBuffer vertex_buffer;
  VkrIndexBuffer index_buffer;
  uint64_t retire_after_frame;
} VkrUiTextRetiredBufferSet;

/** @brief Maximum number of buffer sets kept alive after resizing. */
#define VKR_UI_TEXT_MAX_RETIRED_BUFFER_SETS 8

/**
 * @brief UI text resource.
 *
 * Owns the text content, computed layout, and GPU resources for rendering.
 */
typedef struct VkrUiText {
  // Dependencies
  VkrRendererFrontendHandle renderer;
  VkrFontSystem *font_system;
  VkrAllocator *allocator;

  // Content & config
  String8 content; // Owned text content
  VkrUiTextConfig config;
  VkrTransform transform; // Position/rotation/scale

  // Computed state
  VkrTextLayout layout;   // Computed glyph positions
  VkrTextBounds bounds;   // Computed text bounds
  VkrFont *resolved_font; // Cached font pointer

  // Render state
  VkrUiTextRenderState render;

  // Retired GPU buffers pending safe destruction.
  VkrUiTextRetiredBufferSet retired_buffers[VKR_UI_TEXT_MAX_RETIRED_BUFFER_SETS];

  // Dirty flags
  bool8_t layout_dirty;  // Need to recompute layout
  bool8_t buffers_dirty; // Need to regenerate GPU buffers
} VkrUiText;
Vector(VkrUiText);

// =============================================================================
// UI Text API
// =============================================================================

/**
 * @brief Creates a UI text instance.
 * @param renderer The renderer frontend handle.
 * @param allocator The allocator for memory management.
 * @param font_system The font system.
 * @param pipeline The pipeline to use for rendering.
 * @param content Initial text content (copied).
 * @param config Initial configuration (or NULL for defaults).
 * @param out_text Output text instance.
 * @param out_error Error output.
 * @return true on success.
 */
bool8_t vkr_ui_text_create(VkrRendererFrontendHandle renderer,
                           VkrAllocator *allocator, VkrFontSystem *font_system,
                           VkrPipelineHandle pipeline, String8 content,
                           const VkrUiTextConfig *config, VkrUiText *out_text,
                           VkrRendererError *out_error);

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

/**
 * @brief Sets the text position.
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

/**
 * @brief Prepares text for rendering (rebuilds buffers if dirty).
 * Call before draw if layout/content changed.
 * @return true if buffers are valid and ready for drawing.
 */
bool8_t vkr_ui_text_prepare(VkrUiText *text);

/**
 * @brief Submits text draw command to the renderer.
 * @param text The UI text instance.
 */
void vkr_ui_text_draw(VkrUiText *text);
