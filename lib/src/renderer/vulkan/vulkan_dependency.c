#include "renderer/vulkan/vulkan_dependency.h"

VkPipelineStageFlags vulkan_legacy_lower_stages(VkrGpuStageFlags stages,
                                                bool8_t is_src) {
  if (stages == VKR_GPU_STAGE_NONE) {
    return is_src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                  : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  }

  VkPipelineStageFlags result = 0;
  if (stages & VKR_GPU_STAGE_ALL_GRAPHICS) {
    result |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
  }
  if (stages & VKR_GPU_STAGE_DRAW_INDIRECT) {
    result |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
  }
  if (stages & VKR_GPU_STAGE_VERTEX_INPUT) {
    result |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
  }
  if (stages & VKR_GPU_STAGE_VERTEX_SHADER) {
    result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
  }
  if (stages & VKR_GPU_STAGE_FRAGMENT_SHADER) {
    result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }
  if (stages & VKR_GPU_STAGE_EARLY_DEPTH) {
    result |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  }
  if (stages & VKR_GPU_STAGE_LATE_DEPTH) {
    result |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  }
  if (stages & VKR_GPU_STAGE_COLOR_OUTPUT) {
    result |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  if (stages & VKR_GPU_STAGE_COMPUTE_SHADER) {
    result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  if (stages & VKR_GPU_STAGE_TRANSFER) {
    result |= VK_PIPELINE_STAGE_TRANSFER_BIT;
  }
  if (stages & VKR_GPU_STAGE_HOST) {
    result |= VK_PIPELINE_STAGE_HOST_BIT;
  }
  if (stages & VKR_GPU_STAGE_TOP) {
    result |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  }
  if (stages & VKR_GPU_STAGE_BOTTOM) {
    result |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  }
  return result ? result : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

vkr_internal VkAccessFlags
vulkan_legacy_buffer_access(VkrBufferAccessFlags access) {
  VkAccessFlags flags = 0;
  if (access & VKR_BUFFER_ACCESS_VERTEX) {
    flags |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_INDEX) {
    flags |= VK_ACCESS_INDEX_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_UNIFORM) {
    flags |= VK_ACCESS_UNIFORM_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_STORAGE_READ) {
    flags |= VK_ACCESS_SHADER_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_STORAGE_WRITE) {
    flags |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_SRC) {
    flags |= VK_ACCESS_TRANSFER_READ_BIT;
  }
  if (access & VKR_BUFFER_ACCESS_TRANSFER_DST) {
    flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  return flags;
}

vkr_internal VkAccessFlags
vulkan_legacy_image_access(VkrImageAccessFlags access, bool8_t is_src) {
  VkAccessFlags flags = 0;
  if (access & VKR_IMAGE_ACCESS_STORAGE_WRITE) {
    flags |= VK_ACCESS_SHADER_WRITE_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_COLOR_ATTACHMENT) {
    flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT) {
    flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_TRANSFER_DST) {
    flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  if (is_src) {
    return flags;
  }

  if (access & (VKR_IMAGE_ACCESS_SAMPLED | VKR_IMAGE_ACCESS_STORAGE_READ)) {
    flags |= VK_ACCESS_SHADER_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_COLOR_ATTACHMENT) {
    flags |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT) {
    flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_DEPTH_READ_ONLY) {
    flags |=
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_TRANSFER_SRC) {
    flags |= VK_ACCESS_TRANSFER_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_PRESENT) {
    flags |= VK_ACCESS_MEMORY_READ_BIT;
  }
  return flags;
}

VulkanLegacyDependency
vulkan_legacy_lower_buffer_dependency(VkrBufferAccessFlags src_access,
                                      VkrBufferAccessFlags dst_access,
                                      const VkrGpuDependency *dependency) {
  const VkrGpuDependency fallback =
      vkr_gpu_buffer_dependency_default(src_access, dst_access);
  const VkrGpuDependency *selected = dependency ? dependency : &fallback;
  return (VulkanLegacyDependency){
      .src_stages = vulkan_legacy_lower_stages(selected->src_stages, true_v),
      .dst_stages = vulkan_legacy_lower_stages(selected->dst_stages, false_v),
      .src_access = vulkan_legacy_buffer_access(src_access),
      .dst_access = vulkan_legacy_buffer_access(dst_access),
  };
}

VulkanLegacyDependency
vulkan_legacy_lower_image_dependency(VkrImageAccessFlags src_access,
                                     VkrImageAccessFlags dst_access,
                                     const VkrGpuDependency *dependency) {
  const VkrGpuDependency fallback =
      vkr_gpu_image_dependency_default(src_access, dst_access);
  const VkrGpuDependency *selected = dependency ? dependency : &fallback;
  return (VulkanLegacyDependency){
      .src_stages = vulkan_legacy_lower_stages(selected->src_stages, true_v),
      .dst_stages = vulkan_legacy_lower_stages(selected->dst_stages, false_v),
      .src_access = vulkan_legacy_image_access(src_access, true_v),
      .dst_access = vulkan_legacy_image_access(dst_access, false_v),
  };
}
