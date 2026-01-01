#pragma once

#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_MTSDF_FONT_DEFAULT_SIZE 32
#define VKR_MTSDF_FONT_MAX_GLYPHS 65536
#define VKR_MTSDF_FONT_MAX_KERNINGS 65536

// =============================================================================
// MTSDF Font Loader Types
// =============================================================================

typedef struct VkrTextureSystem VkrTextureSystem;

/**
 * @brief MTSDF font metadata.
 */
typedef struct VkrMtsdfFontMetadata {
  // Atlas info
  float32_t distance_range; // SDF distance range (for shader)
  float32_t em_size;        // EM size used to generate atlas
  float32_t size;           // Size of the font in pixels
  uint32_t atlas_width;
  uint32_t atlas_height;
  bool8_t y_origin_bottom; // true if yOrigin = "bottom"

  // Metrics (normalized to EM)
  float32_t line_height;
  float32_t ascender;
  float32_t descender;
  float32_t underline_y;
  float32_t underline_thickness;

  // Glyphs and kerning
  Array_VkrMtsdfGlyph glyphs;
  Array_VkrFontKerning kernings;
} VkrMtsdfFontMetadata;

/**
 * @brief MTSDF font loader context.
 */
typedef struct VkrMtsdfFontLoaderContext {
  VkrJobSystem *job_system;
  VkrArenaPool *arena_pool;
  VkrTextureSystem *texture_system;
} VkrMtsdfFontLoaderContext;

/**
 * @brief MTSDF font loader result.
 */
typedef struct VkrMtsdfFontLoaderResult {
  Arena *arena;
  void *pool_chunk;
  VkrAllocator allocator;
  VkrFont font;
  VkrMtsdfFontMetadata metadata; // MTSDF-specific data
  String8 atlas_texture_name;
  bool8_t success;
  VkrRendererError error;
} VkrMtsdfFontLoaderResult;

// =============================================================================
// Resource Loader Factory
// =============================================================================

VkrResourceLoader
vkr_mtsdf_font_loader_create(VkrMtsdfFontLoaderContext *context);
