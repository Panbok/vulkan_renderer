#pragma once

#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Constants
// =============================================================================

#define VKR_SYSTEM_FONT_DEFAULT_SIZE 32
#define VKR_SYSTEM_FONT_MIN_SIZE 8
#define VKR_SYSTEM_FONT_MAX_SIZE 128
#define VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE 1024
#define VKR_SYSTEM_FONT_MAX_ATLAS_SIZE 4096
#define VKR_SYSTEM_FONT_FIRST_CODEPOINT 32
#define VKR_SYSTEM_FONT_LAST_CODEPOINT 255
#define VKR_SYSTEM_FONT_GLYPH_COUNT                                            \
  (VKR_SYSTEM_FONT_LAST_CODEPOINT - VKR_SYSTEM_FONT_FIRST_CODEPOINT + 1)
#define VKR_SYSTEM_FONT_ATLAS_PADDING 1

// =============================================================================
// System Font Loader Types
// =============================================================================

typedef struct VkrTextureSystem VkrTextureSystem;

/**
 * @brief A system font loader context.
 * @param job_system The job system.
 * @param arena_pool The arena pool.
 * @param texture_system The texture system.
 */
typedef struct VkrSystemFontLoaderContext {
  VkrJobSystem *job_system; /**< Optional job system for batch loading */
  VkrArenaPool *arena_pool; /**< Optional arena pool for result allocations */
  VkrTextureSystem
      *texture_system; /**< Texture system for atlas registration */
} VkrSystemFontLoaderContext;

/**
 * @brief A system font loader result.
 * @param arena The arena.
 * @param pool_chunk The pool chunk.
 * @param allocator The allocator.
 * @param font The font.
 * @param pages The pages.
 * @param atlas_texture_name The name of the atlas texture.
 * @param success The success flag.
 * @param error The error.
 */
typedef struct VkrSystemFontLoaderResult {
  Arena *arena;     /**< Arena backing the font data (owned by result) */
  void *pool_chunk; /**< Pool chunk (NULL if not pooled) */
  VkrAllocator allocator;
  VkrFont font;
  String8 atlas_texture_name; /**< Registered texture name for atlas cleanup */
  bool8_t success;
  VkrRendererError error;
} VkrSystemFontLoaderResult;

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a system font loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single fonts and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @param context The font loader context (pointer is stored; must remain valid
 * for loader lifetime)
 * @return The configured resource loader for system fonts
 */
VkrResourceLoader
vkr_system_font_loader_create(VkrSystemFontLoaderContext *context);
