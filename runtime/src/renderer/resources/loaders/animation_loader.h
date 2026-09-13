#pragma once

#include "assets/vkr_animation.h"
#include "memory/arena.h"
#include "renderer/systems/vkr_resource_system.h"

/* Immutable CPU bank owned by one resource result. All views, including the
 * source path, expire when its request is unloaded. Playback state is separate.
 * A consumer keeping this pointer must retain its resource request. */
typedef struct VkrAnimationLoaderResult {
  Arena *arena;
  VkrAllocator allocator;
  String8 source_path;
  VkrAnimationAsset asset;
} VkrAnimationLoaderResult;

/* CPU-only, context-free loader; the generic async worker owns each result
 * until completion publication or cancellation unload. */
VkrResourceLoader vkr_animation_loader_create(void);
