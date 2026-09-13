#pragma once

#include "assets/vkr_animation.h"

/* Tool-only source import; creates no materials/textures and does not modify
 * source files. The result arena owns all copied data and is discarded by the
 * caller on failure. Scratch must be a separate scoped allocator. */
bool8_t vkr_animation_import_gltf(VkrAllocator *result_allocator,
                                  VkrAllocator *scratch_allocator,
                                  String8 source_path,
                                  VkrAnimationAsset *out_asset,
                                  const char **error);
