#include "vulkan_utils.h"

VulkanShaderStageFlagResult
vulkan_shader_stage_to_vk(VkrShaderStageFlags stage) {
  int stage_count = 0;
  VkShaderStageFlagBits result = 0;

  if (bitset8_is_set(&stage, VKR_SHADER_STAGE_VERTEX_BIT)) {
    result = VK_SHADER_STAGE_VERTEX_BIT;
    stage_count++;
  } else if (bitset8_is_set(&stage, VKR_SHADER_STAGE_FRAGMENT_BIT)) {
    result = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage_count++;
  } else if (bitset8_is_set(&stage, VKR_SHADER_STAGE_GEOMETRY_BIT)) {
    result = VK_SHADER_STAGE_GEOMETRY_BIT;
    stage_count++;
  } else if (bitset8_is_set(&stage,
                            VKR_SHADER_STAGE_TESSELLATION_CONTROL_BIT)) {
    result = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    stage_count++;
  } else if (bitset8_is_set(&stage,
                            VKR_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) {
    result = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    stage_count++;
  } else if (bitset8_is_set(&stage, VKR_SHADER_STAGE_COMPUTE_BIT)) {
    result = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_count++;
  }

  if (stage_count != 1) {
    log_error(
        "Invalid shader stage configuration: exactly one stage must be set");
    return (VulkanShaderStageFlagResult){.flag = VK_SHADER_STAGE_ALL_GRAPHICS,
                                         .is_valid = false};
  }

  return (VulkanShaderStageFlagResult){.flag = result, .is_valid = true};
}

QueueFamilyIndexResult find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device) {
  QueueFamilyIndexResult result = {0};
  result.length = QUEUE_FAMILY_TYPE_COUNT;
  for (uint32_t i = 0; i < QUEUE_FAMILY_TYPE_COUNT; i++) {
    result.indices[i] = (QueueFamilyIndex){
        .index = 0, .type = i, .is_present = false};
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);
  if (queue_family_count == 0) {
    return result;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    return result;
  }

  Array_VkQueueFamilyProperties queue_family_properties =
      array_create_VkQueueFamilyProperties(&state->temp_scope,
                                           queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           queue_family_properties.data);

  for (uint32_t i = 0; i < queue_family_count; i++) {
    QueueFamilyIndex *graphics_index = &result.indices[QUEUE_FAMILY_TYPE_GRAPHICS];
    QueueFamilyIndex *present_index = &result.indices[QUEUE_FAMILY_TYPE_PRESENT];
    QueueFamilyIndex *transfer_index = &result.indices[QUEUE_FAMILY_TYPE_TRANSFER];

    if (graphics_index->is_present && present_index->is_present &&
        transfer_index->is_present) {
      break;
    }

    VkQueueFamilyProperties properties =
        *array_get_VkQueueFamilyProperties(&queue_family_properties, i);

    if ((properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
        !graphics_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_GRAPHICS, .is_present = true};
      result.indices[QUEUE_FAMILY_TYPE_GRAPHICS] = index;
      continue;
    }

    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, state->surface,
                                         &presentSupport);
    if (presentSupport && !present_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_PRESENT, .is_present = true};
      result.indices[QUEUE_FAMILY_TYPE_PRESENT] = index;
      continue;
    }

    if ((properties.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
        !transfer_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_TRANSFER, .is_present = true};
      result.indices[QUEUE_FAMILY_TYPE_TRANSFER] = index;
    }
  }

  array_destroy_VkQueueFamilyProperties(&queue_family_properties);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return result;
}

int32_t find_memory_index(VkPhysicalDevice device, uint32_t type_filter,
                          uint32_t property_flags) {
  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(device, &memory_properties);

  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
    // Check each memory type to see if its bit is set to 1.
    if (type_filter & (1 << i) &&
        (memory_properties.memoryTypes[i].propertyFlags & property_flags) ==
            property_flags) {
      return i;
    }
  }

  log_warn("Unable to find suitable memory type");
  return -1;
}

VkFormat vulkan_vertex_format_to_vk(VkrVertexFormat format) {
  switch (format) {
  case VKR_VERTEX_FORMAT_R32_SFLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case VKR_VERTEX_FORMAT_R32G32_SFLOAT:
    return VK_FORMAT_R32G32_SFLOAT;
  case VKR_VERTEX_FORMAT_R32G32B32_SFLOAT:
    return VK_FORMAT_R32G32B32_SFLOAT;
  case VKR_VERTEX_FORMAT_R32G32B32A32_SFLOAT:
    return VK_FORMAT_R32G32B32A32_SFLOAT;
  case VKR_VERTEX_FORMAT_R32_SINT:
    return VK_FORMAT_R32_SINT;
  case VKR_VERTEX_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  case VKR_VERTEX_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  default:
    log_error("Unknown vertex format");
    return VK_FORMAT_UNDEFINED;
  }
}

VkPrimitiveTopology
vulkan_primitive_topology_to_vk(VkrPrimitiveTopology topology) {
  switch (topology) {
  case VKR_PRIMITIVE_TOPOLOGY_POINT_LIST:
    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case VKR_PRIMITIVE_TOPOLOGY_LINE_LIST:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case VKR_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  default:
    log_fatal("Invalid primitive topology: %d", topology);
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkPolygonMode vulkan_polygon_mode_to_vk(VkrPolygonMode mode) {
  switch (mode) {
  case VKR_POLYGON_MODE_FILL:
    return VK_POLYGON_MODE_FILL;
  case VKR_POLYGON_MODE_LINE:
    return VK_POLYGON_MODE_LINE;
  case VKR_POLYGON_MODE_POINT:
    return VK_POLYGON_MODE_POINT;
  default:
    log_fatal("Invalid polygon mode: %d", mode);
    return VK_POLYGON_MODE_FILL;
  }
}

VkBufferUsageFlags vulkan_buffer_usage_to_vk(VkrBufferUsageFlags usage) {
  VkBufferUsageFlags vk_usage = 0;

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_VERTEX_BUFFER)) {
    vk_usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_INDEX_BUFFER)) {
    vk_usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_UNIFORM) ||
      bitset8_is_set(&usage, VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER)) {
    vk_usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_TRANSFER_SRC)) {
    vk_usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_TRANSFER_DST)) {
    vk_usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_STORAGE)) {
    vk_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  }

  if (bitset8_is_set(&usage, VKR_BUFFER_USAGE_INDIRECT)) {
    vk_usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  }

  if (vk_usage == 0) {
    log_fatal("Invalid buffer usage: no valid flags set");
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  }

  return vk_usage;
}

VkImageUsageFlags
vulkan_image_usage_from_texture_usage(VkrTextureUsageFlags usage) {
  VkImageUsageFlags vk_usage = 0;

  if (bitset8_is_set(&usage, VKR_TEXTURE_USAGE_SAMPLED)) {
    vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }

  if (bitset8_is_set(&usage, VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)) {
    vk_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }

  if (bitset8_is_set(&usage, VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)) {
    vk_usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  }

  if (bitset8_is_set(&usage, VKR_TEXTURE_USAGE_TRANSFER_SRC)) {
    vk_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }

  if (bitset8_is_set(&usage, VKR_TEXTURE_USAGE_TRANSFER_DST)) {
    vk_usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }

  if (vk_usage == 0) {
    log_fatal("Invalid texture usage: no valid flags set");
    return VK_IMAGE_USAGE_SAMPLED_BIT;
  }

  return vk_usage;
}

VkMemoryPropertyFlags
vulkan_memory_property_flags_to_vk(VkrMemoryPropertyFlags flags) {
  VkMemoryPropertyFlags vk_flags = 0;

  if (bitset8_is_set(&flags, VKR_MEMORY_PROPERTY_HOST_VISIBLE)) {
    vk_flags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  }

  if (bitset8_is_set(&flags, VKR_MEMORY_PROPERTY_HOST_COHERENT)) {
    vk_flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  }

  if (bitset8_is_set(&flags, VKR_MEMORY_PROPERTY_HOST_CACHED)) {
    vk_flags |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  }

  if (bitset8_is_set(&flags, VKR_MEMORY_PROPERTY_DEVICE_LOCAL)) {
    vk_flags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }

  if (vk_flags == 0) {
    log_fatal("Invalid memory property flags: no valid flags set");
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  }

  return vk_flags;
}

VkFormat vulkan_image_format_from_texture_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
    return VK_FORMAT_R8G8B8A8_UINT;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
    return VK_FORMAT_R8G8B8A8_SNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return VK_FORMAT_R8G8B8A8_SINT;
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return VK_FORMAT_R8_UNORM;
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return VK_FORMAT_R16_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return VK_FORMAT_R8G8_UNORM;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return VK_FORMAT_D32_SFLOAT;
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  default:
    log_fatal("Invalid texture format: %d", format);
    return VK_FORMAT_UNDEFINED;
  }
}

VkSamplerAddressMode
vulkan_sampler_address_mode_from_repeat(VkrTextureRepeatMode mode) {
  switch (mode) {
  case VKR_TEXTURE_REPEAT_MODE_MIRRORED_REPEAT:
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case VKR_TEXTURE_REPEAT_MODE_REPEAT:
  default:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

VkCullModeFlags vulkan_cull_mode_to_vk(VkrCullMode mode) {
  switch (mode) {
  case VKR_CULL_MODE_NONE:
    return VK_CULL_MODE_NONE;
  case VKR_CULL_MODE_FRONT:
    return VK_CULL_MODE_FRONT_BIT;
  case VKR_CULL_MODE_BACK:
    return VK_CULL_MODE_BACK_BIT;
  case VKR_CULL_MODE_FRONT_AND_BACK:
    return VK_CULL_MODE_FRONT_AND_BACK;
  default:
    return VK_CULL_MODE_BACK_BIT;
  }
}
