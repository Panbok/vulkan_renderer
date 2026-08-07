#pragma once

#include "renderer/vkr_renderer.h"

#include <vulkan/vulkan.h>

/** Fully lowered legacy Vulkan 1.2 dependency masks. */
typedef struct VulkanLegacyDependency {
  VkPipelineStageFlags src_stages;
  VkPipelineStageFlags dst_stages;
  VkAccessFlags src_access;
  VkAccessFlags dst_access;
} VulkanLegacyDependency;

VkPipelineStageFlags vulkan_legacy_lower_stages(VkrGpuStageFlags stages,
                                                bool8_t is_src);
VulkanLegacyDependency
vulkan_legacy_lower_buffer_dependency(VkrBufferAccessFlags src_access,
                                      VkrBufferAccessFlags dst_access,
                                      const VkrGpuDependency *dependency);
VulkanLegacyDependency
vulkan_legacy_lower_image_dependency(VkrImageAccessFlags src_access,
                                     VkrImageAccessFlags dst_access,
                                     const VkrGpuDependency *dependency);
