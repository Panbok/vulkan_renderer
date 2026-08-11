#pragma once

#include "renderer/vkr_renderer.h"

#include <vulkan/vulkan.h>

typedef enum VkrBindlessVkDependencyResult {
  VKR_BINDLESS_VK_DEPENDENCY_OK = 0,
  VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT,
  VKR_BINDLESS_VK_DEPENDENCY_RESOURCE_ALIAS_UNSUPPORTED,
} VkrBindlessVkDependencyResult;

/** Fully lowered Vulkan 1.4 synchronization2 dependency masks. */
typedef struct VkrBindlessVkDependency {
  VkPipelineStageFlags2 src_stages;
  VkPipelineStageFlags2 dst_stages;
  VkAccessFlags2 src_access;
  VkAccessFlags2 dst_access;
} VkrBindlessVkDependency;

VkrBindlessVkDependencyResult
vkr_bindless_vk_lower_stages(VkrGpuStageFlags stages,
                             VkPipelineStageFlags2 *out_stages);
VkrBindlessVkDependencyResult vkr_bindless_vk_lower_buffer_dependency(
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access,
    const VkrGpuDependency *dependency, VkrBindlessVkDependency *out_lowered);
VkrBindlessVkDependencyResult vkr_bindless_vk_lower_image_dependency(
    VkrImageAccessFlags src_access, VkrImageAccessFlags dst_access,
    const VkrGpuDependency *dependency, bool8_t layout_transition,
    VkrBindlessVkDependency *out_lowered);
const char *
vkr_bindless_vk_dependency_result_string(VkrBindlessVkDependencyResult result);
