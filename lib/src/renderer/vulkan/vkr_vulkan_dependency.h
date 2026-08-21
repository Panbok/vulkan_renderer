#pragma once

#include "renderer/vkr_renderer.h"

#include <vulkan/vulkan.h>

typedef enum VkrVulkanDependencyResult {
  VKR_VULKAN_DEPENDENCY_OK = 0,
  VKR_VULKAN_DEPENDENCY_INVALID_ARGUMENT,
  VKR_VULKAN_DEPENDENCY_RESOURCE_ALIAS_UNSUPPORTED,
} VkrVulkanDependencyResult;

/** Fully lowered Vulkan 1.4 synchronization2 dependency masks. */
typedef struct VkrVulkanDependency {
  VkPipelineStageFlags2 src_stages;
  VkPipelineStageFlags2 dst_stages;
  VkAccessFlags2 src_access;
  VkAccessFlags2 dst_access;
} VkrVulkanDependency;

VkrVulkanDependencyResult
vkr_vk_lower_stages(VkrGpuStageFlags stages, VkPipelineStageFlags2 *out_stages);
VkrVulkanDependencyResult vkr_vk_lower_buffer_dependency(
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access,
    const VkrGpuDependency *dependency, VkrVulkanDependency *out_lowered);
VkrVulkanDependencyResult vkr_vk_lower_image_dependency(
    VkrImageAccessFlags src_access, VkrImageAccessFlags dst_access,
    const VkrGpuDependency *dependency, bool8_t layout_transition,
    VkrVulkanDependency *out_lowered);
const char *vkr_vk_dependency_result_string(VkrVulkanDependencyResult result);
