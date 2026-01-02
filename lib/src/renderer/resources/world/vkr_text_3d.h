#pragma once

#include "core/vkr_text.h"
#include "math/vkr_transform.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/vkr_buffer.h"
#include "renderer/vkr_renderer.h"

// =============================================================================
// 3D Text Types
// =============================================================================

#define VKR_TEXT_3D_MAX_LENGTH 1024
#define VKR_TEXT_3D_DEFAULT_TEXTURE_SIZE 512

/**
 * @brief Configuration for 3D text.
 */
typedef struct VkrText3DConfig {
  String8 text;            // Owned text content
  VkrFontHandle font;      // Font to use (or invalid for default)
  float32_t font_size;     // Font size in points (0 = use font's native)
  Vec4 color;              // Text color (RGBA)
  uint32_t texture_width;  // Texture width (0 = auto-size)
  uint32_t texture_height; // Texture height (0 = auto-size)
  float32_t uv_inset_px; // Half-texel inset (in atlas pixels) to avoid bleeding
  VkrPipelineHandle pipeline; // Pipeline used for rendering (world/transparent)
} VkrText3DConfig;

#define VKR_TEXT_3D_CONFIG_DEFAULT                                             \
  (VkrText3DConfig) {                                                          \
    .text = {0}, .font = VKR_FONT_HANDLE_INVALID, .font_size = 0.0f,           \
    .color = {1.0f, 1.0f, 1.0f, 1.0f}, .texture_width = 0,                     \
    .texture_height = 0, .uv_inset_px = 0.5f,                                  \
    .pipeline = VKR_PIPELINE_HANDLE_INVALID                                    \
  }

/**
 * @brief 3D text resource.
 */
typedef struct VkrText3D {
  VkrAllocator *allocator;            // Allocator for memory management
  VkrRendererFrontendHandle renderer; // Renderer frontend handle
  VkrFontSystem *font_system;         // Font system

  String8 text;        // Owned text content
  VkrFontHandle font;  // Font to use (or invalid for default)
  float32_t font_size; // Font size in points (0 = use font's native)
  Vec4 color;          // Text color (RGBA)

  VkrTextLayout layout;                // Computed glyph positions
  VkrTextLayoutOptions layout_options; // Word wrap, max dimensions, anchor
  VkrTextBounds bounds;                // Computed text bounds
  bool8_t layout_dirty;                // Need to recompute layout
  bool8_t buffers_dirty;               // Need to regenerate GPU buffers

  uint32_t texture_width;  // Texture width (0 = auto-size)
  uint32_t texture_height; // Texture height (0 = auto-size)

  VkrPipelineHandle pipeline; // Pipeline used for rendering (world/transparent)
  bool8_t pipeline_ref_acquired;                 // Pipeline reference acquired
  VkrRendererInstanceStateHandle instance_state; // Renderer instance state
  VkrVertexBuffer vertex_buffer;                 // Vertex buffer
  VkrIndexBuffer index_buffer;                   // Index buffer
  uint32_t quad_count;                           // Number of glyph quads
  uint32_t vertex_capacity;                      // Allocated vertex count
  uint32_t index_capacity;                       // Allocated index count

  VkrTransform transform; // Position/rotation/scale
  float32_t world_width;  // Width in world units
  float32_t world_height; // Height in world units
  float32_t uv_inset_px; // Half-texel inset (in atlas pixels) to avoid bleeding

  bool8_t initialized; // Initialized flag
} VkrText3D;

// =============================================================================
// 3D Text API
// =============================================================================

/**
 * @brief Creates a 3D text instance.
 * @param text_3d The text 3D instance to create.
 * @param renderer The renderer frontend handle.
 * @param font_system The font system.
 * @param allocator The allocator for memory management.
 * @param config The configuration for the text 3D instance.
 * @param out_error The error output.
 * @return true on success.
 */
bool8_t vkr_text_3d_create(VkrText3D *text_3d,
                           VkrRendererFrontendHandle renderer,
                           VkrFontSystem *font_system, VkrAllocator *allocator,
                           const VkrText3DConfig *config,
                           VkrRendererError *out_error);

/**
 * @brief Destroys a 3D text instance.
 * @param text_3d The text 3D instance to destroy.
 */
void vkr_text_3d_destroy(VkrText3D *text_3d);

/**
 * @brief Sets the text content.
 * @param text_3d The text 3D instance.
 * @param text The text content to set.
 */
void vkr_text_3d_set_text(VkrText3D *text_3d, String8 text);

/**
 * @brief Sets the text color.
 * @param text_3d The text 3D instance.
 * @param color The text color to set.
 */
void vkr_text_3d_set_color(VkrText3D *text_3d, Vec4 color);

/**
 * @brief Sets the transform.
 * @param text_3d The text 3D instance.
 * @param transform The transform to set.
 */
void vkr_text_3d_set_transform(VkrText3D *text_3d, VkrTransform transform);

/**
 * @brief Updates the text 3D instance.
 * @param text_3d The text 3D instance.
 */
void vkr_text_3d_update(VkrText3D *text_3d);

/**
 * @brief Draws the text 3D instance.
 * @param text_3d The text 3D instance.
 * @param renderer The renderer frontend handle.
 */
void vkr_text_3d_draw(VkrText3D *text_3d, VkrRendererFrontendHandle renderer);
