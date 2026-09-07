#pragma once

#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Bitmap Font Loader Types
// =============================================================================

typedef struct VkrBitmapFontLoaderContext {
  VkrJobSystem *job_system; /**< Optional job system for batch loading */
  VkrArenaPool *arena_pool; /**< Optional arena pool for result allocations */
} VkrBitmapFontLoaderContext;

typedef struct VkrBitmapFontLoaderResult {
  Arena *arena;     /**< Arena backing the font data (owned by result) */
  void *pool_chunk; /**< Pool chunk (NULL if not pooled) */
  VkrAllocator allocator;
  VkrFont font;
  Array_VkrBitmapFontPage pages; /**< Page descriptors indexed by page id */
  bool8_t success;
  VkrRendererError error;
} VkrBitmapFontLoaderResult;

// =============================================================================
// Resource Loader Factory
// =============================================================================

/**
 * @brief Creates a bitmap font loader.
 *
 * The loader supports both single-item and batch loading through the resource
 * system. Use vkr_resource_system_load() for single fonts and
 * vkr_resource_system_load_batch() for parallel batch loading.
 *
 * @param context The font loader context (must remain valid for the lifetime of
 * the loader, or NULL for default behavior)
 * @return The configured resource loader for bitmap fonts
 */
VkrResourceLoader
vkr_bitmap_font_loader_create(const VkrBitmapFontLoaderContext *context);
