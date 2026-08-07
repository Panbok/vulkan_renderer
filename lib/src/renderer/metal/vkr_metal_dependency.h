#pragma once

#include "renderer/vkr_renderer.h"

/** Native Metal 4 masks produced from one canonical graph dependency. */
typedef struct VkrMetalDependency {
  uint64_t src_stages;
  uint64_t dst_stages;
  uint64_t visibility_options;
} VkrMetalDependency;

bool8_t vkr_metal_dependency_lower(const VkrGpuDependency *dependency,
                                   VkrMetalDependency *out_dependency);

#if defined(__OBJC__)
#import <Metal/Metal.h>

bool8_t
vkr_metal_dependency_encode_producer(id<MTL4CommandEncoder> encoder,
                                     const VkrMetalDependency *dependency);
bool8_t
vkr_metal_dependency_encode_consumer(id<MTL4CommandEncoder> encoder,
                                     const VkrMetalDependency *dependency);
bool8_t vkr_metal_dependency_encode_intra(id<MTL4CommandEncoder> encoder,
                                          const VkrMetalDependency *dependency);
#endif
