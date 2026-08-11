#include "renderer/vulkan/bindless/vkr_bindless_vulkan_dependency.h"

enum {
  VKR_BINDLESS_VK_KNOWN_STAGE_FLAGS =
      VKR_GPU_STAGE_ALL_GRAPHICS | VKR_GPU_STAGE_DRAW_INDIRECT |
      VKR_GPU_STAGE_VERTEX_INPUT | VKR_GPU_STAGE_VERTEX_SHADER |
      VKR_GPU_STAGE_FRAGMENT_SHADER | VKR_GPU_STAGE_EARLY_DEPTH |
      VKR_GPU_STAGE_LATE_DEPTH | VKR_GPU_STAGE_COLOR_OUTPUT |
      VKR_GPU_STAGE_COMPUTE_SHADER | VKR_GPU_STAGE_TRANSFER |
      VKR_GPU_STAGE_HOST | VKR_GPU_STAGE_TOP | VKR_GPU_STAGE_BOTTOM,
  VKR_BINDLESS_VK_KNOWN_VISIBILITY_FLAGS =
      VKR_GPU_VISIBILITY_DEVICE | VKR_GPU_VISIBILITY_RESOURCE_ALIAS,
  VKR_BINDLESS_VK_KNOWN_BUFFER_ACCESS_FLAGS =
      VKR_BUFFER_ACCESS_VERTEX | VKR_BUFFER_ACCESS_INDEX |
      VKR_BUFFER_ACCESS_UNIFORM | VKR_BUFFER_ACCESS_STORAGE_READ |
      VKR_BUFFER_ACCESS_STORAGE_WRITE | VKR_BUFFER_ACCESS_TRANSFER_SRC |
      VKR_BUFFER_ACCESS_TRANSFER_DST,
  VKR_BINDLESS_VK_KNOWN_IMAGE_ACCESS_FLAGS =
      VKR_IMAGE_ACCESS_SAMPLED | VKR_IMAGE_ACCESS_STORAGE_READ |
      VKR_IMAGE_ACCESS_STORAGE_WRITE | VKR_IMAGE_ACCESS_COLOR_ATTACHMENT |
      VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT | VKR_IMAGE_ACCESS_DEPTH_READ_ONLY |
      VKR_IMAGE_ACCESS_TRANSFER_SRC | VKR_IMAGE_ACCESS_TRANSFER_DST |
      VKR_IMAGE_ACCESS_PRESENT,
};

VkrBindlessVkDependencyResult
vkr_bindless_vk_lower_stages(VkrGpuStageFlags stages,
                             VkPipelineStageFlags2 *out_stages) {
  if (!out_stages ||
      ((uint32_t)stages & ~(uint32_t)VKR_BINDLESS_VK_KNOWN_STAGE_FLAGS) != 0u) {
    return VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT;
  }

  VkPipelineStageFlags2 lowered = VK_PIPELINE_STAGE_2_NONE;
  if (stages & VKR_GPU_STAGE_ALL_GRAPHICS)
    lowered |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
  if (stages & VKR_GPU_STAGE_DRAW_INDIRECT)
    lowered |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  if (stages & VKR_GPU_STAGE_VERTEX_INPUT) {
    lowered |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
               VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
  }
  if (stages & VKR_GPU_STAGE_VERTEX_SHADER)
    lowered |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
  if (stages & VKR_GPU_STAGE_FRAGMENT_SHADER)
    lowered |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  if (stages & VKR_GPU_STAGE_EARLY_DEPTH)
    lowered |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
  if (stages & VKR_GPU_STAGE_LATE_DEPTH)
    lowered |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  if (stages & VKR_GPU_STAGE_COLOR_OUTPUT)
    lowered |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (stages & VKR_GPU_STAGE_COMPUTE_SHADER)
    lowered |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  if (stages & VKR_GPU_STAGE_TRANSFER) {
    lowered |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
               VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
  }
  if (stages & VKR_GPU_STAGE_HOST)
    lowered |= VK_PIPELINE_STAGE_2_HOST_BIT;
  if (stages & VKR_GPU_STAGE_TOP)
    lowered |= VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  if (stages & VKR_GPU_STAGE_BOTTOM)
    lowered |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;

  *out_stages = lowered;
  return VKR_BINDLESS_VK_DEPENDENCY_OK;
}

vkr_internal VkrBindlessVkDependencyResult
vkr_bindless_vk_validate_dependency(const VkrGpuDependency *dependency) {
  if (((uint32_t)dependency->visibility &
       ~(uint32_t)VKR_BINDLESS_VK_KNOWN_VISIBILITY_FLAGS) != 0u) {
    return VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT;
  }
  if (dependency->visibility & VKR_GPU_VISIBILITY_RESOURCE_ALIAS) {
    return VKR_BINDLESS_VK_DEPENDENCY_RESOURCE_ALIAS_UNSUPPORTED;
  }
  return VKR_BINDLESS_VK_DEPENDENCY_OK;
}

vkr_internal VkAccessFlags2
vkr_bindless_vk_buffer_access(VkrBufferAccessFlags access) {
  VkAccessFlags2 flags = VK_ACCESS_2_NONE;
  if (access & VKR_BUFFER_ACCESS_VERTEX)
    flags |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
  if (access & VKR_BUFFER_ACCESS_INDEX)
    flags |= VK_ACCESS_2_INDEX_READ_BIT;
  if (access & VKR_BUFFER_ACCESS_UNIFORM)
    flags |= VK_ACCESS_2_UNIFORM_READ_BIT;
  if (access & VKR_BUFFER_ACCESS_STORAGE_READ)
    flags |= VK_ACCESS_2_SHADER_READ_BIT;
  if (access & VKR_BUFFER_ACCESS_STORAGE_WRITE)
    flags |= VK_ACCESS_2_SHADER_WRITE_BIT;
  if (access & VKR_BUFFER_ACCESS_TRANSFER_SRC)
    flags |= VK_ACCESS_2_TRANSFER_READ_BIT;
  if (access & VKR_BUFFER_ACCESS_TRANSFER_DST)
    flags |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  return flags;
}

vkr_internal VkAccessFlags2
vkr_bindless_vk_image_access(VkrImageAccessFlags access, bool8_t is_src) {
  VkAccessFlags2 flags = VK_ACCESS_2_NONE;
  if (access & VKR_IMAGE_ACCESS_STORAGE_WRITE)
    flags |= VK_ACCESS_2_SHADER_WRITE_BIT;
  if (access & VKR_IMAGE_ACCESS_COLOR_ATTACHMENT)
    flags |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  if (access & VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT)
    flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  if (access & VKR_IMAGE_ACCESS_TRANSFER_DST)
    flags |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
  if (is_src)
    return flags;

  if (access & (VKR_IMAGE_ACCESS_SAMPLED | VKR_IMAGE_ACCESS_STORAGE_READ))
    flags |= VK_ACCESS_2_SHADER_READ_BIT;
  if (access & VKR_IMAGE_ACCESS_COLOR_ATTACHMENT)
    flags |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
  if (access & VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT)
    flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  if (access & VKR_IMAGE_ACCESS_DEPTH_READ_ONLY) {
    flags |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
             VK_ACCESS_2_SHADER_READ_BIT;
  }
  if (access & VKR_IMAGE_ACCESS_TRANSFER_SRC)
    flags |= VK_ACCESS_2_TRANSFER_READ_BIT;
  if (access & VKR_IMAGE_ACCESS_PRESENT)
    flags |= VK_ACCESS_2_MEMORY_READ_BIT;
  return flags;
}

VkrBindlessVkDependencyResult vkr_bindless_vk_lower_buffer_dependency(
    VkrBufferAccessFlags src_access, VkrBufferAccessFlags dst_access,
    const VkrGpuDependency *dependency, VkrBindlessVkDependency *out_lowered) {
  if (!out_lowered ||
      (((uint32_t)src_access | (uint32_t)dst_access) &
       ~(uint32_t)VKR_BINDLESS_VK_KNOWN_BUFFER_ACCESS_FLAGS) != 0u) {
    return VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT;
  }
  *out_lowered = (VkrBindlessVkDependency){0};

  const VkrGpuDependency fallback =
      vkr_gpu_buffer_dependency_default(src_access, dst_access);
  const VkrGpuDependency *selected = dependency ? dependency : &fallback;
  VkrBindlessVkDependencyResult result =
      vkr_bindless_vk_validate_dependency(selected);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;
  result = vkr_bindless_vk_lower_stages(selected->src_stages,
                                        &out_lowered->src_stages);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;
  result = vkr_bindless_vk_lower_stages(selected->dst_stages,
                                        &out_lowered->dst_stages);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;

  if (selected->visibility & VKR_GPU_VISIBILITY_DEVICE) {
    out_lowered->src_access = vkr_bindless_vk_buffer_access(src_access);
    out_lowered->dst_access = vkr_bindless_vk_buffer_access(dst_access);
  }
  return VKR_BINDLESS_VK_DEPENDENCY_OK;
}

VkrBindlessVkDependencyResult vkr_bindless_vk_lower_image_dependency(
    VkrImageAccessFlags src_access, VkrImageAccessFlags dst_access,
    const VkrGpuDependency *dependency, bool8_t layout_transition,
    VkrBindlessVkDependency *out_lowered) {
  if (!out_lowered ||
      (((uint32_t)src_access | (uint32_t)dst_access) &
       ~(uint32_t)VKR_BINDLESS_VK_KNOWN_IMAGE_ACCESS_FLAGS) != 0u) {
    return VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT;
  }
  *out_lowered = (VkrBindlessVkDependency){0};

  const VkrGpuDependency fallback =
      vkr_gpu_image_dependency_default(src_access, dst_access);
  const VkrGpuDependency *selected = dependency ? dependency : &fallback;
  VkrBindlessVkDependencyResult result =
      vkr_bindless_vk_validate_dependency(selected);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;
  result = vkr_bindless_vk_lower_stages(selected->src_stages,
                                        &out_lowered->src_stages);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;
  result = vkr_bindless_vk_lower_stages(selected->dst_stages,
                                        &out_lowered->dst_stages);
  if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
    return result;

  if (selected->visibility & VKR_GPU_VISIBILITY_DEVICE) {
    out_lowered->src_access = vkr_bindless_vk_image_access(src_access, true_v);
    out_lowered->dst_access = vkr_bindless_vk_image_access(dst_access, false_v);
  } else if (layout_transition) {
    /* The transition itself is an image write. Even when no prior resource
       write needs visibility, its destination scope must include the first
       access in the new layout so that use cannot race the transition. */
    out_lowered->dst_access = vkr_bindless_vk_image_access(dst_access, false_v);
  }
  return VKR_BINDLESS_VK_DEPENDENCY_OK;
}

const char *
vkr_bindless_vk_dependency_result_string(VkrBindlessVkDependencyResult result) {
  switch (result) {
  case VKR_BINDLESS_VK_DEPENDENCY_OK:
    return "ok";
  case VKR_BINDLESS_VK_DEPENDENCY_INVALID_ARGUMENT:
    return "invalid argument";
  case VKR_BINDLESS_VK_DEPENDENCY_RESOURCE_ALIAS_UNSUPPORTED:
    return "resource-alias visibility is unsupported without graph placement "
           "aliasing";
  default:
    return "unknown bindless Vulkan dependency error";
  }
}
