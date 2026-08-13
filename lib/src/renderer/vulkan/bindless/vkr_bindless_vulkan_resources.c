#include "renderer/vulkan/bindless/vkr_bindless_vulkan_internal.h"

VkDevice
vkr_bindless_vk_renderer_device(const VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vulkan_device_handle(renderer->device);
}

vkr_internal bool8_t vkr_bindless_vk_choose_memory_type(
    const VkrBindlessVulkanRenderer *renderer, uint32_t memory_type_bits,
    VkrBindlessVkMemoryClass memory_class, uint32_t *out_index,
    VkMemoryPropertyFlags *out_properties) {
  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_bindless_vulkan_device_memory_properties(renderer->device);
  int32_t best_rank = INT32_MAX;
  uint32_t best_index = UINT32_MAX;
  for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags available =
        memory->memoryTypes[i].propertyFlags;
    if (!(memory_type_bits & (1u << i)))
      continue;
    const int32_t rank =
        vkr_bindless_vulkan_memory_type_rank(memory_class, available);
    if (rank >= 0 && rank < best_rank) {
      best_rank = rank;
      best_index = i;
    }
  }
  if (best_index == UINT32_MAX)
    return false_v;
  *out_index = best_index;
  *out_properties = memory->memoryTypes[best_index].propertyFlags;
  if (memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE && best_rank == 2)
    log_warn("Bindless Vulkan DEVICE placement degraded to memory type %u",
             best_index);
  return true_v;
}

VkFormat vkr_bindless_vk_texture_format(VkrTextureFormat format) {
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
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
    return VK_FORMAT_BC7_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return VK_FORMAT_BC7_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return VK_FORMAT_BC5_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM:
    return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB:
    return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
    return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
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
  case VKR_TEXTURE_FORMAT_R32G32_UINT:
    return VK_FORMAT_R32G32_UINT;
  case VKR_TEXTURE_FORMAT_R16G16_SNORM:
    return VK_FORMAT_R16G16_SNORM;
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return VK_FORMAT_D16_UNORM;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return VK_FORMAT_D32_SFLOAT;
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

bool8_t vkr_bindless_vk_format_block_info(VkFormat format, uint32_t *out_width,
                                          uint32_t *out_height,
                                          uint32_t *out_bytes) {
  uint32_t width = 1u;
  uint32_t height = 1u;
  uint32_t bytes = 0u;
  switch (format) {
  case VK_FORMAT_R8_UNORM:
    bytes = 1u;
    break;
  case VK_FORMAT_R16_SFLOAT:
  case VK_FORMAT_R8G8_UNORM:
    bytes = 2u;
    break;
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UINT:
  case VK_FORMAT_R8G8B8A8_SNORM:
  case VK_FORMAT_R8G8B8A8_SINT:
  case VK_FORMAT_R16G16_SNORM:
  case VK_FORMAT_R32_SFLOAT:
  case VK_FORMAT_R32_UINT:
    bytes = 4u;
    break;
  case VK_FORMAT_R32G32_UINT:
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    bytes = 8u;
    break;
  case VK_FORMAT_BC7_UNORM_BLOCK:
  case VK_FORMAT_BC7_SRGB_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
  case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
  case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
  case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    width = 4u;
    height = 4u;
    bytes = 16u;
    break;
  default:
    return false_v;
  }
  *out_width = width;
  *out_height = height;
  *out_bytes = bytes;
  return true_v;
}

VkImageAspectFlags vkr_bindless_vk_format_aspects(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_D32_SFLOAT:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

VkImageLayout vkr_bindless_vk_texture_layout(VkrTextureLayout layout) {
  switch (layout) {
  case VKR_TEXTURE_LAYOUT_UNDEFINED:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  case VKR_TEXTURE_LAYOUT_GENERAL:
    return VK_IMAGE_LAYOUT_GENERAL;
  case VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR:
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  default:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

bool8_t vkr_bindless_vk_flush(const VkrBindlessVulkanRenderer *renderer,
                              const VkrBindlessVkAllocation *allocation,
                              VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkFlushMappedMemoryRanges(vkr_bindless_vk_renderer_device(renderer),
                                   1u, &range) == VK_SUCCESS;
}

bool8_t vkr_bindless_vk_mark_dirty(VkrBindlessVkDirtyRange *dirty,
                                   const VkrBindlessVkBuffer *buffer,
                                   VkDeviceSize offset, VkDeviceSize size) {
  if (!dirty || !buffer || !size || offset > buffer->size ||
      size > buffer->size - offset)
    return false_v;
  const VkDeviceSize end = offset + size;
  if (!dirty->dirty) {
    *dirty = (VkrBindlessVkDirtyRange){
        .offset = offset,
        .end = end,
        .dirty = true_v,
    };
  } else {
    dirty->offset = Min(dirty->offset, offset);
    dirty->end = Max(dirty->end, end);
  }
  return true_v;
}

bool8_t
vkr_bindless_vk_flush_publication_ranges(VkrBindlessVulkanRenderer *renderer) {
  VkrBindlessVkDirtyRange *ranges[] = {
      &renderer->resource_descriptor_dirty,
      &renderer->sampler_descriptor_dirty,
      &renderer->material_dirty,
  };
  VkrBindlessVkBuffer *buffers[] = {
      &renderer->resource_descriptors,
      &renderer->sampler_descriptors,
      &renderer->materials,
  };
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i) {
    VkrBindlessVkDirtyRange *range = ranges[i];
    if (range->dirty &&
        !vkr_bindless_vk_flush(renderer, &buffers[i]->allocation, range->offset,
                               range->end - range->offset))
      return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i)
    MemZero(ranges[i], sizeof(*ranges[i]));
  return true_v;
}

bool8_t vkr_bindless_vk_invalidate(const VkrBindlessVulkanRenderer *renderer,
                                   const VkrBindlessVkAllocation *allocation,
                                   VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkInvalidateMappedMemoryRanges(
             vkr_bindless_vk_renderer_device(renderer), 1u, &range) ==
         VK_SUCCESS;
}

vkr_internal bool8_t vkr_bindless_vk_release_allocation(
    VkrBindlessVulkanRenderer *renderer, VkrBindlessVkAllocation *allocation) {
  if (allocation->pooled)
    return vkr_bindless_vulkan_memory_pool_release(
        renderer->memory_pool, &allocation->pooled_allocation,
        renderer->completed_value, renderer->completed_value);
  if (!allocation->memory)
    return true_v;

  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (allocation->mapped)
    vkUnmapMemory(device, allocation->memory);
  vkFreeMemory(device, allocation->memory, NULL);
  if (allocation->dedicated)
    vkr_bindless_vulkan_memory_pool_record_dedicated_release(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size,
        allocation->retired);
  return true_v;
}

void vkr_bindless_vk_destroy_buffer(VkrBindlessVulkanRenderer *renderer,
                                    VkrBindlessVkBuffer *buffer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (buffer->handle)
    vkDestroyBuffer(device, buffer->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &buffer->allocation))
    log_error("Bindless Vulkan failed to release a proven buffer placement");
  MemZero(buffer, sizeof(*buffer));
}

bool8_t vkr_bindless_vk_retire_allocation(VkrBindlessVulkanRenderer *renderer,
                                          VkrBindlessVkAllocation *allocation,
                                          uint64_t retire_value) {
  if (!allocation || allocation->retired)
    return false_v;
  if (allocation->pooled) {
    if (!vkr_bindless_vulkan_memory_pool_retire(renderer->memory_pool,
                                                &allocation->pooled_allocation,
                                                retire_value))
      return false_v;
  } else if (allocation->dedicated) {
    if (!vkr_bindless_vulkan_memory_pool_record_dedicated_retire(
            renderer->memory_pool, allocation->pool_key,
            allocation->memory_size))
      return false_v;
  } else {
    return false_v;
  }
  allocation->retired = true_v;
  return true_v;
}

bool8_t vkr_bindless_vk_retire_buffer(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkBuffer *buffer,
                                      uint64_t retire_value) {
  return buffer && buffer->handle &&
         vkr_bindless_vk_retire_allocation(renderer, &buffer->allocation,
                                           retire_value);
}

bool8_t vkr_bindless_vk_create_buffer(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkMemoryClass memory_class,
                                      VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      VkrBindlessVkBuffer *out_buffer) {
  MemZero(out_buffer, sizeof(*out_buffer));
  out_buffer->size = size;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceBufferMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &buffer_info,
  };
  vkGetDeviceBufferMemoryRequirements(device, &device_requirements,
                                      &requirements);
  VkrBindlessVkAllocation *allocation = &out_buffer->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          memory_class, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  const bool8_t has_address =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = memory_class,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_BUFFER,
      .memory_type_index = allocation->memory_type_index,
      .device_address_required = has_address,
  };
  const uint64_t pool_block_size =
      memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD
          ? renderer->config.upload_buffer_block_size
      : memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_READBACK
          ? renderer->config.readback_buffer_block_size
          : renderer->config.device_buffer_block_size;
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation ||
      requirements.memoryRequirements.size > pool_block_size;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation))
    return false_v;
  if (vkCreateBuffer(device, &buffer_info, NULL, &out_buffer->handle) !=
      VK_SUCCESS) {
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = out_buffer->handle,
    };
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = has_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u,
        .pNext = &dedicated_info,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext =
            has_address ? (const void *)&flags : (const void *)&dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    const VkResult allocate_result =
        vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory);
    if (allocate_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated buffer allocation failed "
                "(size=%llu, type=%u, class=%u, result=%d)",
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (uint32_t)memory_class,
                (int)allocate_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
    VkResult map_result = VK_SUCCESS;
    if ((allocation->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        memory_class != VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE)
      map_result =
          vkMapMemory(device, allocation->memory, 0u, allocation->memory_size,
                      0u, &allocation->mapped);
    if (map_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated buffer map failed "
                "(size=%llu, type=%u, class=%u, result=%d)",
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (uint32_t)memory_class,
                (int)map_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
    allocation->mapped = allocation->pooled_allocation.mapped;
  }
  const VkResult bind_result = vkBindBufferMemory(
      device, out_buffer->handle, allocation->memory, allocation->offset);
  if (bind_result != VK_SUCCESS) {
    log_error("Bindless Vulkan buffer bind failed "
              "(size=%llu, type=%u, class=%u, offset=%llu, result=%d)",
              (unsigned long long)allocation->memory_size,
              allocation->memory_type_index, (uint32_t)memory_class,
              (unsigned long long)allocation->offset, (int)bind_result);
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  if (has_address) {
    const VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out_buffer->handle,
    };
    out_buffer->address = vkGetBufferDeviceAddress(device, &address_info);
    if (!out_buffer->address) {
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t
vkr_bindless_vk_create_upload_buffers(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_gpu_abi_validate_host()) {
    log_error("Bindless Vulkan shared host ABI validation failed");
    return false_v;
  }
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  return vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             resource_layout->size,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->resource_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             sampler_layout->size,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->sampler_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             VKR_BINDLESS_VK_SENTINEL_UPLOAD_SIZE,
             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &renderer->upload) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             (VkDeviceSize)renderer->config.material_slot_capacity *
                 sizeof(VkrBindlessVkMaterialGpuRow),
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, &renderer->materials);
}

void vkr_bindless_vk_destroy_image(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkImage *image) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (image->view)
    vkDestroyImageView(device, image->view, NULL);
  if (image->handle)
    vkDestroyImage(device, image->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &image->allocation))
    log_error("Bindless Vulkan failed to release a proven image placement");
  MemZero(image, sizeof(*image));
}

bool8_t vkr_bindless_vk_create_image_ex(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t mip_levels, uint32_t array_layers, VkFormat format,
    VkImageCreateFlags flags, VkImageViewType view_type,
    VkImageUsageFlags usage, VkrBindlessVkImage *out_image) {
  if (!width || !height || !mip_levels || !array_layers ||
      format == VK_FORMAT_UNDEFINED)
    return false_v;
  MemZero(out_image, sizeof(*out_image));
  out_image->width = width;
  out_image->height = height;
  out_image->mip_levels = mip_levels;
  out_image->array_layers = array_layers;
  out_image->format = format;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = flags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {.width = width, .height = height, .depth = 1u},
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceImageMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
      .pCreateInfo = &image_info,
  };
  vkGetDeviceImageMemoryRequirements(device, &device_requirements,
                                     &requirements);
  VkrBindlessVkAllocation *allocation = &out_image->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_IMAGE,
      .memory_type_index = allocation->memory_type_index,
  };
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation ||
      requirements.memoryRequirements.size >
          renderer->config.device_image_block_size;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation)) {
    log_error("Bindless Vulkan image pool allocation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, bytes=%llu, type=%u)",
              width, height, mip_levels, array_layers, format,
              (unsigned long long)requirements.memoryRequirements.size,
              allocation->memory_type_index);
    return false_v;
  }
  const VkResult create_result =
      vkCreateImage(device, &image_info, NULL, &out_image->handle);
  if (create_result != VK_SUCCESS) {
    log_error("Bindless Vulkan native image creation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, result=%d)",
              width, height, mip_levels, array_layers, format,
              (int)create_result);
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = out_image->handle,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    const VkResult allocate_result =
        vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory);
    if (allocate_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated image allocation failed "
                "(%ux%u, mips=%u, layers=%u, format=%u, bytes=%llu, type=%u, "
                "result=%d)",
                width, height, mip_levels, array_layers, format,
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (int)allocate_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_image(renderer, out_image);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
  }
  const VkResult bind_result = vkBindImageMemory(
      device, out_image->handle, allocation->memory, allocation->offset);
  if (bind_result != VK_SUCCESS) {
    log_error("Bindless Vulkan image bind failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, offset=%llu, result=%d)",
              width, height, mip_levels, array_layers, format,
              (unsigned long long)allocation->offset, (int)bind_result);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out_image->handle,
      .viewType = view_type,
      .format = format,
      .subresourceRange = {.aspectMask = vkr_bindless_vk_format_aspects(format),
                           .levelCount = mip_levels,
                           .layerCount = array_layers},
  };
  const VkResult view_result =
      vkCreateImageView(device, &view_info, NULL, &out_image->view);
  if (view_result != VK_SUCCESS) {
    log_error("Bindless Vulkan image view creation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, view=%u, result=%d)",
              width, height, mip_levels, array_layers, format, view_type,
              (int)view_result);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_bindless_vk_create_image(VkrBindlessVulkanRenderer *renderer,
                                     uint32_t width, uint32_t height,
                                     VkImageUsageFlags usage,
                                     VkrBindlessVkImage *out_image) {
  return vkr_bindless_vk_create_image_ex(
      renderer, width, height, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM, 0u,
      VK_IMAGE_VIEW_TYPE_2D, usage, out_image);
}

vkr_internal bool8_t
vkr_bindless_vk_create_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  const VkDeviceSize readback_size = 4u;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex =
            vkr_bindless_vulkan_device_queue_family(renderer->device),
    };
    if (vkCreateCommandPool(device, &pool_info, NULL, &slot->command_pool) !=
        VK_SUCCESS) {
      return false_v;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = slot->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    const VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u,
    };
    if (vkAllocateCommandBuffers(device, &command_info,
                                 &slot->command_buffer) != VK_SUCCESS ||
        vkCreateQueryPool(device, &query_info, NULL, &slot->timestamp_pool) !=
            VK_SUCCESS ||
        !vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_READBACK, readback_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot->readback) ||
        !vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
            VKR_BINDLESS_VK_FRAME_UPLOAD_SIZE,
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            &slot->frame_upload) ||
        (renderer->config.capture_ring_capacity > 0u &&
         !vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_READBACK,
             renderer->config.capture_max_batch_bytes,
             VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot->capture_readback))) {
      return false_v;
    }
  }
  return true_v;
}

void vkr_bindless_vk_destroy_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    vkr_bindless_vk_destroy_buffer(renderer, &slot->frame_upload);
    vkr_bindless_vk_destroy_buffer(renderer, &slot->capture_readback);
    vkr_bindless_vk_destroy_buffer(renderer, &slot->readback);
    if (slot->timestamp_pool)
      vkDestroyQueryPool(device, slot->timestamp_pool, NULL);
    if (slot->command_pool) {
      vkDestroyCommandPool(device, slot->command_pool, NULL);
    }
    MemZero(slot, sizeof(*slot));
  }
}

vkr_internal bool8_t
vkr_bindless_vk_write_upload_data(VkrBindlessVulkanRenderer *renderer) {
  uint8_t *mapped = renderer->upload.allocation.mapped;
  const uint8_t sentinel_pixel[] = {37u, 91u, 173u, 255u};
  MemCopy(mapped, sentinel_pixel, sizeof(sentinel_pixel));
  return vkr_bindless_vk_flush(renderer, &renderer->upload.allocation, 0u,
                               sizeof(sentinel_pixel));
}

bool8_t vkr_bindless_vk_create_resources(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_bindless_vk_create_upload_buffers(renderer)) {
    log_error("Bindless Vulkan failed to create upload buffers");
    return false_v;
  }
  if (!vkr_bindless_vk_create_image(renderer, 1u, 1u,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    &renderer->sentinel_image)) {
    log_error("Bindless Vulkan failed to create the sentinel image");
    return false_v;
  }
  if (!vkr_bindless_vk_create_target_set(
          renderer, renderer->config.width, renderer->config.height,
          renderer->config.image_count, &renderer->targets)) {
    log_error("Bindless Vulkan failed to create render targets");
    return false_v;
  }
  if (!vkr_bindless_vk_create_frame_slots(renderer)) {
    log_error("Bindless Vulkan failed to create frame slots");
    return false_v;
  }
  const VkDeviceSize descriptor_alignment =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device)
          ->descriptorBufferOffsetAlignment;
  if ((renderer->resource_descriptors.address % descriptor_alignment) != 0u ||
      (renderer->sampler_descriptors.address % descriptor_alignment) != 0u ||
      !vkr_bindless_vk_write_upload_data(renderer)) {
    log_error("Bindless Vulkan descriptor alignment or initial upload failed");
    return false_v;
  }
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
  };
  if (vkCreateSampler(vkr_bindless_vk_renderer_device(renderer), &sampler_info,
                      NULL, &renderer->sentinel_sampler) != VK_SUCCESS) {
    log_error("Bindless Vulkan failed to create the sentinel sampler");
    return false_v;
  }
  return true_v;
}

bool8_t vkr_bindless_vk_create_descriptor_slot_tables(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  if (!properties->sampledImageDescriptorSize ||
      !properties->storageImageDescriptorSize ||
      !properties->samplerDescriptorSize ||
      properties->sampledImageDescriptorSize > UINT32_MAX ||
      properties->storageImageDescriptorSize > UINT32_MAX ||
      properties->samplerDescriptorSize > UINT32_MAX) {
    log_error("Bindless Vulkan descriptor row size is not representable");
    return false_v;
  }
  const VkrGpuSlotTableConfig sampled_config = {
      .max_slots = renderer->config.sampled_image_capacity,
      .max_retirements = renderer->config.sampled_image_capacity,
      .row_size = (uint32_t)properties->sampledImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig sampler_config = {
      .max_slots = renderer->config.sampler_capacity,
      .max_retirements = renderer->config.sampler_capacity,
      .row_size = (uint32_t)properties->samplerDescriptorSize,
  };
  const VkrGpuSlotTableConfig storage_config = {
      .max_slots = renderer->config.storage_image_capacity,
      .max_retirements = renderer->config.storage_image_capacity,
      .row_size = (uint32_t)properties->storageImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig material_config = {
      .max_slots = renderer->config.material_slot_capacity,
      .max_retirements = renderer->config.material_slot_capacity,
      .row_size = sizeof(VkrBindlessVkMaterialGpuRow),
  };
  renderer->sampled_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampled_config);
  renderer->sampler_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampler_config);
  renderer->storage_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&storage_config);
  renderer->material_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&material_config);
  renderer->published_geometries_size =
      (uint64_t)renderer->config.geometry_capacity *
      sizeof(*renderer->published_geometries);
  renderer->retired_geometries_size =
      (uint64_t)renderer->config.geometry_capacity *
      sizeof(*renderer->retired_geometries);
  renderer->published_textures_size =
      (uint64_t)renderer->config.texture_capacity *
      sizeof(*renderer->published_textures);
  renderer->retired_textures_size =
      (uint64_t)renderer->config.texture_capacity *
      sizeof(*renderer->retired_textures);
  renderer->published_samplers_size =
      (uint64_t)renderer->config.sampler_capacity *
      sizeof(*renderer->published_samplers);
  renderer->published_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->published_materials);
  renderer->retired_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->retired_materials);
  renderer->pending_texture_initializations_size =
      (uint64_t)renderer->config.texture_capacity *
      sizeof(*renderer->pending_texture_initializations);
  renderer->pending_buffer_initialization_capacity =
      renderer->config.publication_staging_capacity;
  renderer->pending_buffer_initializations_size =
      (uint64_t)renderer->pending_buffer_initialization_capacity *
      sizeof(*renderer->pending_buffer_initializations);
  renderer->retired_staging_buffer_capacity =
      renderer->config.publication_staging_capacity;
  renderer->retired_staging_buffers_size =
      (uint64_t)renderer->retired_staging_buffer_capacity *
      sizeof(*renderer->retired_staging_buffers);
  renderer->descriptor_scratch_size =
      (uint32_t)Max(Max(properties->sampledImageDescriptorSize,
                        properties->storageImageDescriptorSize),
                    properties->samplerDescriptorSize);
  renderer->sampled_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampled_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->sampler_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampler_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->storage_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->storage_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->material_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->material_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->descriptor_scratch = vkr_allocator_alloc(
      renderer->allocator, renderer->descriptor_scratch_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_geometries = vkr_allocator_alloc(
      renderer->allocator, renderer->published_geometries_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_geometries = vkr_allocator_alloc(
      renderer->allocator, renderer->retired_geometries_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_textures = vkr_allocator_alloc(
      renderer->allocator, renderer->published_textures_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_textures =
      vkr_allocator_alloc(renderer->allocator, renderer->retired_textures_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_samplers = vkr_allocator_alloc(
      renderer->allocator, renderer->published_samplers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_materials = vkr_allocator_alloc(
      renderer->allocator, renderer->published_materials_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_materials =
      vkr_allocator_alloc(renderer->allocator, renderer->retired_materials_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_texture_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_texture_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_buffer_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_buffer_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_staging_buffers = vkr_allocator_alloc(
      renderer->allocator, renderer->retired_staging_buffers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer->sampled_image_slot_storage ||
      !renderer->storage_image_slot_storage ||
      !renderer->sampler_slot_storage || !renderer->material_slot_storage ||
      !renderer->descriptor_scratch || !renderer->published_geometries ||
      !renderer->retired_geometries || !renderer->published_textures ||
      !renderer->retired_textures || !renderer->published_samplers ||
      !renderer->published_materials || !renderer->retired_materials ||
      !renderer->pending_texture_initializations ||
      !renderer->pending_buffer_initializations ||
      !renderer->retired_staging_buffers) {
    return false_v;
  }
  MemZero(renderer->published_geometries, renderer->published_geometries_size);
  MemZero(renderer->retired_geometries, renderer->retired_geometries_size);
  MemZero(renderer->published_textures, renderer->published_textures_size);
  MemZero(renderer->retired_textures, renderer->retired_textures_size);
  MemZero(renderer->published_samplers, renderer->published_samplers_size);
  MemZero(renderer->published_materials, renderer->published_materials_size);
  MemZero(renderer->retired_materials, renderer->retired_materials_size);
  MemZero(renderer->pending_texture_initializations,
          renderer->pending_texture_initializations_size);
  MemZero(renderer->pending_buffer_initializations,
          renderer->pending_buffer_initializations_size);
  MemZero(renderer->retired_staging_buffers,
          renderer->retired_staging_buffers_size);
  return vkr_gpu_slot_table_create(
             &sampled_config, renderer->sampled_image_slot_storage,
             renderer->sampled_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->sampled_image_offset,
             &renderer->sampled_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &sampler_config, renderer->sampler_slot_storage,
             renderer->sampler_slot_storage_size,
             (uint8_t *)renderer->sampler_descriptors.allocation.mapped +
                 sampler_layout->sampler_offset,
             &renderer->sampler_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &storage_config, renderer->storage_image_slot_storage,
             renderer->storage_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->storage_image_offset,
             &renderer->storage_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &material_config, renderer->material_slot_storage,
             renderer->material_slot_storage_size,
             renderer->materials.allocation.mapped,
             &renderer->material_slots) == VKR_GPU_SLOT_STATUS_OK;
}

bool8_t vkr_bindless_vk_publish_sentinel_descriptors(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorImageInfo image_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo storage_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  VkDescriptorGetInfoEXT image_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  VkDescriptorGetInfoEXT sampler_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &renderer->sentinel_sampler,
  };
  VkDescriptorGetInfoEXT storage_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &storage_info,
  };
  PFN_vkGetDescriptorEXT get_descriptor =
      vkr_bindless_vulkan_device_get_descriptor(renderer->device);
  VkrGpuSlotHandle sampled_handle = {0};
  VkrGpuSlotHandle sampler_handle = {0};
  VkrGpuSlotHandle storage_handle = {0};
  VkrGpuSlotHandle material_handle = {0};
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &image_get,
                 properties->sampledImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 &sampled_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampled_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &sampler_get,
                 properties->samplerDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 &sampler_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampler_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &storage_get,
                 properties->storageImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 &storage_handle) != VKR_GPU_SLOT_STATUS_OK ||
      storage_handle.index != 0u) {
    return false_v;
  }
  const VkrBindlessVkMaterialGpuRow material = {
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .base_color_texture = sampled_handle.index,
      .normal_texture = sampled_handle.index,
      .orm_texture = sampled_handle.index,
      .emissive_texture = sampled_handle.index,
      .base_color_sampler = sampler_handle.index,
      .normal_sampler = sampler_handle.index,
      .orm_sampler = sampler_handle.index,
      .emissive_sampler = sampler_handle.index,
      .material_id = 0xffad5b25u,
      .material_surface = {0.0f, 1.0f, 1.0f, 1.0f},
      .material_alpha = {0.5f, 0.0f, 1.5f, 0.0f},
      .material_attenuation_color = {1.0f, 1.0f, 1.0f, 0.0f},
  };
  if (vkr_gpu_slot_table_publish(renderer->material_slots, &material,
                                 &material_handle) != VKR_GPU_SLOT_STATUS_OK ||
      material_handle.index != 0u) {
    return false_v;
  }
  return vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->sampled_image_offset,
                                    properties->sampledImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->storage_image_offset,
                                    properties->storageImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->sampler_descriptor_dirty,
                                    &renderer->sampler_descriptors,
                                    sampler_layout->sampler_offset,
                                    properties->samplerDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->material_dirty,
                                    &renderer->materials, 0u, sizeof(material));
}
