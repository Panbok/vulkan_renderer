#pragma once

#include "core/vkr_job_system.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/resources/loaders/mtsdf_font_loader.h"
#include "renderer/systems/vkr_resource_system.h"

typedef struct VkrTextureSystem VkrTextureSystem;

typedef struct VkrCookedFontLoaderResult {
  Arena *arena;
  void *pool_chunk;
  VkrAllocator allocator;
  VkrFont font;
  String8 atlas_texture_name;
  bool8_t success;
  VkrRendererError error;
} VkrCookedFontLoaderResult;

VkrResourceLoader
vkr_cooked_font_loader_create(VkrMtsdfFontLoaderContext *context);
