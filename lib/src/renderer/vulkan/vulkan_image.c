#include "vulkan_image.h"
#include "vulkan_fence.h"

vkr_internal VkImageAspectFlags
vulkan_image_aspect_flags_from_format(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D32_SFLOAT:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

bool32_t vulkan_image_create(VulkanBackendState *state, VkImageType image_type,
                             uint32_t width, uint32_t height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage,
                             VkMemoryPropertyFlags memory_flags,
                             uint32_t mip_levels, uint32_t array_layers,
                             VkImageViewType view_type,
                             VkImageAspectFlags view_aspect_flags,
                             VulkanImage *out_image) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_image != NULL, "Output image is NULL");

  // todo: support configurable depth, sample count, and sharing mode.
  VkImageCreateFlags create_flags = 0;
  if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) {
    if (array_layers % 6 != 0) {
      log_fatal(
          "Cube map images require array_layers to be a multiple of 6, got %u",
          array_layers);
      return false_v;
    }
    create_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }

  VkImageCreateInfo image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = create_flags,
      .imageType = image_type,
      .extent.width = width,
      .extent.height = height,
      .extent.depth = 1,
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .format = format,
      .tiling = tiling,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .usage = usage,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };

  out_image->width = width;
  out_image->height = height;
  out_image->mip_levels = mip_levels;
  out_image->array_layers = array_layers;
  out_image->view = VK_NULL_HANDLE;
  out_image->memory_property_flags = memory_flags;

  if (vkCreateImage(state->device.logical_device, &image_create_info,
                    state->allocator, &out_image->handle) != VK_SUCCESS) {
    log_error("Failed to create image");
    return false;
  }

  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(state->device.logical_device, out_image->handle,
                               &memory_requirements);
  VkrAllocatorMemoryTag alloc_tag =
      (memory_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
          ? VKR_ALLOCATOR_MEMORY_TAG_GPU
          : VKR_ALLOCATOR_MEMORY_TAG_VULKAN;

  int32_t memory_type =
      find_memory_index(state->device.physical_device,
                        memory_requirements.memoryTypeBits, memory_flags);
  if (memory_type == -1) {
    log_error("Required memory type not found. Image not valid.");
    vkDestroyImage(state->device.logical_device, out_image->handle,
                   state->allocator);
    out_image->handle = VK_NULL_HANDLE;
    out_image->memory = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo memory_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = memory_type,
  };

  if (vkAllocateMemory(state->device.logical_device, &memory_allocate_info,
                       state->allocator, &out_image->memory) != VK_SUCCESS) {
    log_error("Failed to allocate memory");
    vkDestroyImage(state->device.logical_device, out_image->handle,
                   state->allocator);
    return false;
  }
  vkr_allocator_report(&state->alloc, memory_requirements.size, alloc_tag,
                       true_v);

  if (vkBindImageMemory(state->device.logical_device, out_image->handle,
                        out_image->memory, 0) != VK_SUCCESS) {
    log_error("Failed to bind memory");
    vkr_allocator_report(&state->alloc, memory_requirements.size, alloc_tag,
                         false_v);
    vkFreeMemory(state->device.logical_device, out_image->memory,
                 state->allocator);
    vkDestroyImage(state->device.logical_device, out_image->handle,
                   state->allocator);
    return false;
  }

  if (view_type != VK_IMAGE_VIEW_TYPE_MAX_ENUM) {
    out_image->view = 0;
    if (!vulkan_create_image_view(state, format, view_type, out_image,
                                  view_aspect_flags)) {
      log_error("Failed to create image view");
      vkr_allocator_report(&state->alloc, memory_requirements.size, alloc_tag,
                           false_v);
      vkFreeMemory(state->device.logical_device, out_image->memory,
                   state->allocator);
      vkDestroyImage(state->device.logical_device, out_image->handle,
                     state->allocator);
      return false;
    }
  }

  // log_debug("Created Vulkan image: %p", out_image->handle);
  return true;
}

bool32_t vulkan_create_image_view(VulkanBackendState *state, VkFormat format,
                                  VkImageViewType view_type, VulkanImage *image,
                                  VkImageAspectFlags aspect_flags) {
  VkImageViewCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image->handle,
      .viewType = view_type,
      .format = format,
      .components =
          {
              .r = VK_COMPONENT_SWIZZLE_IDENTITY,
              .g = VK_COMPONENT_SWIZZLE_IDENTITY,
              .b = VK_COMPONENT_SWIZZLE_IDENTITY,
              .a = VK_COMPONENT_SWIZZLE_IDENTITY,
          },
      .subresourceRange =
          {
              .aspectMask = aspect_flags,
              .baseMipLevel = 0,
              .levelCount = image->mip_levels,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
  };

  if (vkCreateImageView(state->device.logical_device, &create_info,
                        state->allocator, &image->view) != VK_SUCCESS) {
    log_error("Failed to create image view");
    return false;
  }

  // log_debug("Created Vulkan image view: %p", image->view);

  return true;
}

bool8_t vulkan_image_transition_layout(VulkanBackendState *state,
                                       VulkanImage *image,
                                       VulkanCommandBuffer *command_buffer,
                                       VkFormat format,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout) {
  return vulkan_image_transition_layout_range(
      state, image, command_buffer, format, old_layout, new_layout, NULL);
}

bool8_t vulkan_image_transition_layout_range(
    VulkanBackendState *state, VulkanImage *image,
    VulkanCommandBuffer *command_buffer, VkFormat format,
    VkImageLayout old_layout, VkImageLayout new_layout,
    const VkImageSubresourceRange *subresource_range) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(format != VK_FORMAT_UNDEFINED, "Format is undefined");
  assert_log(new_layout != VK_IMAGE_LAYOUT_UNDEFINED,
             "New layout is undefined");

  VkImageSubresourceRange range =
      subresource_range
          ? *subresource_range
          : (VkImageSubresourceRange){
                .aspectMask = vulkan_image_aspect_flags_from_format(format),
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            };

  VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = 0,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange = range,
  };

  VkPipelineStageFlags source_stage;
  VkPipelineStageFlags destination_stage;

  if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
      new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
             new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
             new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  } else {
    log_fatal("Unsupported layout transition!");
    return false_v;
  }

  vkCmdPipelineBarrier(command_buffer->handle, source_stage, destination_stage,
                       0, 0, NULL, 0, NULL, 1, &barrier);

  return true_v;
}

bool8_t vulkan_image_copy_from_buffer(VulkanBackendState *state,
                                      VulkanImage *image, VkBuffer buffer,
                                      VulkanCommandBuffer *command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(buffer != VK_NULL_HANDLE, "Buffer is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");

  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent = {image->width, image->height, 1},
  };

  vkCmdCopyBufferToImage(command_buffer->handle, buffer, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  return true_v;
}

bool8_t vulkan_image_copy_cube_faces_from_buffer(
    VulkanBackendState *state, VulkanImage *image, VkBuffer buffer,
    VulkanCommandBuffer *command_buffer, uint64_t face_size) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(buffer != VK_NULL_HANDLE, "Buffer is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(image->array_layers == 6, "Cube map must have 6 layers");
  assert_log(face_size > 0, "Face size must be greater than 0");

  VkBufferImageCopy regions[6];
  for (uint32_t face = 0; face < 6; face++) {
    regions[face] = (VkBufferImageCopy){
        .bufferOffset = face * face_size,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = face,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image->width, image->height, 1},
    };
  }

  vkCmdCopyBufferToImage(command_buffer->handle, buffer, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

  return true_v;
}

void vulkan_image_destroy(VulkanBackendState *state, VulkanImage *image) {
  if (image->view != VK_NULL_HANDLE) {
    vkDestroyImageView(state->device.logical_device, image->view,
                       state->allocator);
  }

  if (image->handle != VK_NULL_HANDLE) {
    if (image->memory != VK_NULL_HANDLE) {
      VkMemoryRequirements memory_requirements;
      vkGetImageMemoryRequirements(state->device.logical_device, image->handle,
                                   &memory_requirements);
      VkrAllocatorMemoryTag alloc_tag =
          (image->memory_property_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
              ? VKR_ALLOCATOR_MEMORY_TAG_GPU
              : VKR_ALLOCATOR_MEMORY_TAG_VULKAN;

      vkr_allocator_report(&state->alloc, memory_requirements.size, alloc_tag,
                           false_v);
      vkFreeMemory(state->device.logical_device, image->memory,
                   state->allocator);
    }
    vkDestroyImage(state->device.logical_device, image->handle,
                   state->allocator);
  }
}

bool8_t vulkan_image_generate_mipmaps(VulkanBackendState *state,
                                      VulkanImage *image, VkFormat image_format,
                                      VulkanCommandBuffer *cmd) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(cmd != NULL, "Command buffer is NULL");

  if (image->mip_levels <= 1)
    return true_v;

  VkFormatProperties format_props;
  vkGetPhysicalDeviceFormatProperties(state->device.physical_device,
                                      image_format, &format_props);
  if (!(format_props.optimalTilingFeatures &
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
    log_warn("Linear blitting not supported for format; skipping mipmap "
             "generation");
    return false_v;
  }

  int32_t mip_width = (int32_t)image->width;
  int32_t mip_height = (int32_t)image->height;

  for (uint32_t mip = 1; mip < image->mip_levels; mip++) {
    VkImageMemoryBarrier barrier_to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = mip - 1,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(cmd->handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &barrier_to_src);

    VkImageBlit blit = {
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip - 1,
                           .baseArrayLayer = 0,
                           .layerCount = image->array_layers},
        .srcOffsets = {{0, 0, 0},
                       {mip_width > 0 ? mip_width : 1,
                        mip_height > 0 ? mip_height : 1, 1}},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = mip,
                           .baseArrayLayer = 0,
                           .layerCount = image->array_layers},
        .dstOffsets = {{0, 0, 0},
                       {mip_width > 1 ? mip_width / 2 : 1,
                        mip_height > 1 ? mip_height / 2 : 1, 1}},
    };

    vkCmdBlitImage(cmd->handle, image->handle,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->handle,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    VkImageMemoryBarrier barrier_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = mip - 1,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(cmd->handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier_to_read);

    if (mip_width > 1)
      mip_width /= 2;
    if (mip_height > 1)
      mip_height /= 2;
  }

  VkImageMemoryBarrier final_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .baseMipLevel = image->mip_levels - 1,
                           .levelCount = 1,
                           .baseArrayLayer = 0,
                           .layerCount = image->array_layers},
  };

  vkCmdPipelineBarrier(cmd->handle, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                       NULL, 1, &final_barrier);

  return true_v;
}

bool8_t vulkan_image_upload_with_mipmaps(VulkanBackendState *state,
                                         VulkanImage *image,
                                         VkBuffer staging_buffer,
                                         VkFormat image_format,
                                         bool8_t generate_mipmaps) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(staging_buffer != VK_NULL_HANDLE, "Staging buffer is NULL");

  bool8_t use_transfer_queue = (state->device.transfer_queue_index !=
                                state->device.graphics_queue_index);

  VkCommandPool transfer_pool = use_transfer_queue
                                    ? state->device.transfer_command_pool
                                    : state->device.graphics_command_pool;
  VkQueue transfer_queue = use_transfer_queue ? state->device.transfer_queue
                                              : state->device.graphics_queue;

  // Pre-allocate graphics command buffer before transfer work to ensure we can
  // complete the queue ownership transfer. This prevents leaving the image in
  // an undefined state if graphics cmd allocation fails after transfer
  // completes.
  VkCommandBuffer graphics_cmd = VK_NULL_HANDLE;
  bool8_t need_graphics_phase = use_transfer_queue || generate_mipmaps;
  if (need_graphics_phase) {
    VkCommandBufferAllocateInfo graphics_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = state->device.graphics_command_pool,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(state->device.logical_device, &graphics_alloc,
                                 &graphics_cmd) != VK_SUCCESS) {
      log_error("Failed to pre-allocate graphics command buffer for image "
                "upload");
      return false_v;
    }
  }

  // Transfer base level via transfer queue
  VkCommandBufferAllocateInfo transfer_alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandPool = transfer_pool,
      .commandBufferCount = 1,
  };

  VkCommandBuffer transfer_cmd;
  if (vkAllocateCommandBuffers(state->device.logical_device, &transfer_alloc,
                               &transfer_cmd) != VK_SUCCESS) {
    log_error("Failed to allocate transfer command buffer");
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(transfer_cmd, &begin_info) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1,
                         &transfer_cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Transition to TRANSFER_DST
  VkImageMemoryBarrier barrier_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = image->mip_levels,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
  };

  vkCmdPipelineBarrier(transfer_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier_to_dst);

  // Copy base level
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent = {image->width, image->height, 1},
  };

  vkCmdCopyBufferToImage(transfer_cmd, staging_buffer, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // If not generating mipmaps and same queue family, transition directly
  if (!generate_mipmaps && !use_transfer_queue) {
    VkImageMemoryBarrier barrier_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(transfer_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier_to_read);
  } else if (use_transfer_queue) {
    // Release ownership to graphics queue family
    VkImageMemoryBarrier release_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(transfer_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &release_barrier);
  }

  if (vkEndCommandBuffer(transfer_cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1,
                         &transfer_cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Submit transfer phase
  VulkanFence transfer_fence;
  vulkan_fence_create(state, false_v, &transfer_fence);

  VkSubmitInfo transfer_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &transfer_cmd,
  };

  if (vkQueueSubmit(transfer_queue, 1, &transfer_submit,
                    transfer_fence.handle) != VK_SUCCESS) {
    vulkan_fence_destroy(state, &transfer_fence);
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1,
                         &transfer_cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  vulkan_fence_wait(state, UINT64_MAX, &transfer_fence);
  vkQueueWaitIdle(transfer_queue);
  vulkan_fence_destroy(state, &transfer_fence);
  vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1,
                       &transfer_cmd);

  // If no mipmaps and same queue, we're done
  if (!need_graphics_phase) {
    return true_v;
  }

  // Graphics queue for mipmaps/ownership - use pre-allocated command buffer
  if (vkBeginCommandBuffer(graphics_cmd, &begin_info) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1, &graphics_cmd);
    log_error("Failed to begin graphics command buffer; image ownership may be "
              "inconsistent");
    return false_v;
  }

  // If different queue families, acquire ownership
  if (use_transfer_queue) {
    VkImageMemoryBarrier acquire_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = generate_mipmaps ? VK_ACCESS_TRANSFER_READ_BIT
                                          : VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = generate_mipmaps
                         ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(graphics_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         generate_mipmaps
                             ? VK_PIPELINE_STAGE_TRANSFER_BIT
                             : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &acquire_barrier);
  }

  // Generate mipmaps if needed
  if (generate_mipmaps && image->mip_levels > 1) {
    // Wrap graphics_cmd in VulkanCommandBuffer for mipmap generation
    VulkanCommandBuffer wrapped_cmd = {.handle = graphics_cmd};

    // vulkan_image_generate_mipmaps handles all the mip level transitions
    // and ends with SHADER_READ_ONLY_OPTIMAL
    if (!vulkan_image_generate_mipmaps(state, image, image_format,
                                       &wrapped_cmd)) {
      log_warn("Mipmap generation failed, falling back to single level");
      // Transition to shader read if mipmaps failed
      VkImageMemoryBarrier final_barrier = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = image->handle,
          .subresourceRange =
              {
                  .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                  .baseMipLevel = 0,
                  .levelCount = image->mip_levels,
                  .baseArrayLayer = 0,
                  .layerCount = image->array_layers,
              },
      };

      vkCmdPipelineBarrier(graphics_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                           NULL, 1, &final_barrier);
    }
  } else if (!generate_mipmaps && use_transfer_queue) {
    // Already transitioned in acquire barrier above
  }

  if (vkEndCommandBuffer(graphics_cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1, &graphics_cmd);
    return false_v;
  }

  VulkanFence graphics_fence;
  vulkan_fence_create(state, false_v, &graphics_fence);

  VkSubmitInfo graphics_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &graphics_cmd,
  };

  if (vkQueueSubmit(state->device.graphics_queue, 1, &graphics_submit,
                    graphics_fence.handle) != VK_SUCCESS) {
    vulkan_fence_destroy(state, &graphics_fence);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1, &graphics_cmd);
    return false_v;
  }

  vulkan_fence_wait(state, UINT64_MAX, &graphics_fence);
  vkQueueWaitIdle(state->device.graphics_queue);
  vulkan_fence_destroy(state, &graphics_fence);
  vkFreeCommandBuffers(state->device.logical_device,
                       state->device.graphics_command_pool, 1, &graphics_cmd);

  return true_v;
}

bool8_t vulkan_image_upload_cube_via_transfer(VulkanBackendState *state,
                                              VulkanImage *image,
                                              VkBuffer staging_buffer,
                                              VkFormat image_format,
                                              uint64_t face_size) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(staging_buffer != VK_NULL_HANDLE, "Staging buffer is NULL");
  assert_log(image->array_layers == 6, "Cube map must have 6 layers");

  bool8_t use_transfer_queue = (state->device.transfer_queue_index !=
                                state->device.graphics_queue_index);

  VkCommandPool transfer_pool = use_transfer_queue
                                    ? state->device.transfer_command_pool
                                    : state->device.graphics_command_pool;
  VkQueue transfer_queue = use_transfer_queue ? state->device.transfer_queue
                                              : state->device.graphics_queue;

  // Pre-allocate graphics command buffer before transfer work to ensure we can
  // complete the queue ownership transfer. This prevents leaving the image in
  // an undefined state if graphics cmd allocation fails after transfer
  // completes.
  VkCommandBuffer graphics_cmd = VK_NULL_HANDLE;
  if (use_transfer_queue) {
    VkCommandBufferAllocateInfo graphics_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = state->device.graphics_command_pool,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(state->device.logical_device, &graphics_alloc,
                                 &graphics_cmd) != VK_SUCCESS) {
      log_error("Failed to pre-allocate graphics command buffer for cube map "
                "upload");
      return false_v;
    }
  }

  VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandPool = transfer_pool,
      .commandBufferCount = 1,
  };

  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(state->device.logical_device, &alloc_info,
                               &cmd) != VK_SUCCESS) {
    log_error("Failed to allocate cube map transfer command buffer");
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Transition all 6 faces to TRANSFER_DST
  VkImageMemoryBarrier barrier_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = image->mip_levels,
              .baseArrayLayer = 0,
              .layerCount = 6,
          },
  };

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier_to_dst);

  // Copy each face to its array layer
  VkBufferImageCopy regions[6];
  for (uint32_t face = 0; face < 6; face++) {
    regions[face] = (VkBufferImageCopy){
        .bufferOffset = face * face_size,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = face,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image->width, image->height, 1},
    };
  }

  vkCmdCopyBufferToImage(cmd, staging_buffer, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

  // Transition to shader read (handle queue ownership if needed)
  if (!use_transfer_queue) {
    VkImageMemoryBarrier barrier_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 6,
            },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier_to_read);
  } else {
    // Release ownership to graphics queue family
    VkImageMemoryBarrier release_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 6,
            },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &release_barrier);
  }

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  VulkanFence fence;
  vulkan_fence_create(state, false_v, &fence);

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
  };

  if (vkQueueSubmit(transfer_queue, 1, &submit_info, fence.handle) !=
      VK_SUCCESS) {
    vulkan_fence_destroy(state, &fence);
    vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  vulkan_fence_wait(state, UINT64_MAX, &fence);
  vkQueueWaitIdle(transfer_queue);
  vulkan_fence_destroy(state, &fence);
  vkFreeCommandBuffers(state->device.logical_device, transfer_pool, 1, &cmd);

  // If different queue families, acquire ownership on graphics queue using
  // pre-allocated command buffer
  if (use_transfer_queue) {
    if (vkBeginCommandBuffer(graphics_cmd, &begin_info) != VK_SUCCESS) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      log_error("Failed to begin graphics command buffer; image ownership may "
                "be inconsistent");
      return false_v;
    }

    VkImageMemoryBarrier acquire_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = 6,
            },
    };

    vkCmdPipelineBarrier(graphics_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &acquire_barrier);

    if (vkEndCommandBuffer(graphics_cmd) != VK_SUCCESS) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      return false_v;
    }

    VulkanFence graphics_fence;
    vulkan_fence_create(state, false_v, &graphics_fence);

    VkSubmitInfo graphics_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &graphics_cmd,
    };

    if (vkQueueSubmit(state->device.graphics_queue, 1, &graphics_submit,
                      graphics_fence.handle) != VK_SUCCESS) {
      vulkan_fence_destroy(state, &graphics_fence);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      return false_v;
    }

    vulkan_fence_wait(state, UINT64_MAX, &graphics_fence);
    vkQueueWaitIdle(state->device.graphics_queue);
    vulkan_fence_destroy(state, &graphics_fence);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1, &graphics_cmd);
  }

  return true_v;
}

bool8_t vulkan_image_upload_via_transfer(VulkanBackendState *state,
                                         VulkanImage *image,
                                         VkBuffer staging_buffer,
                                         VkFormat image_format) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(staging_buffer != VK_NULL_HANDLE, "Staging buffer is NULL");

  // Use transfer queue if it's a dedicated queue (different family from
  // graphics), otherwise fall back to graphics queue for simplicity
  bool8_t use_transfer_queue = (state->device.transfer_queue_index !=
                                state->device.graphics_queue_index);

  VkCommandPool command_pool = use_transfer_queue
                                   ? state->device.transfer_command_pool
                                   : state->device.graphics_command_pool;
  VkQueue queue = use_transfer_queue ? state->device.transfer_queue
                                     : state->device.graphics_queue;

  // Pre-allocate graphics command buffer before transfer work to ensure we can
  // complete the queue ownership transfer. This prevents leaving the image in
  // an undefined state if graphics cmd allocation fails after transfer
  // completes.
  VkCommandBuffer graphics_cmd = VK_NULL_HANDLE;
  if (use_transfer_queue) {
    VkCommandBufferAllocateInfo graphics_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandPool = state->device.graphics_command_pool,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(state->device.logical_device, &graphics_alloc,
                                 &graphics_cmd) != VK_SUCCESS) {
      log_error("Failed to pre-allocate graphics command buffer for image "
                "upload");
      return false_v;
    }
  }

  // Allocate command buffer
  VkCommandBufferAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandPool = command_pool,
      .commandBufferCount = 1,
  };

  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(state->device.logical_device, &alloc_info,
                               &cmd) != VK_SUCCESS) {
    log_error("Failed to allocate transfer command buffer");
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };

  if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
    log_error("Failed to begin transfer command buffer");
    vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Transition image to TRANSFER_DST_OPTIMAL
  VkImageMemoryBarrier barrier_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image->handle,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = 0,
              .levelCount = image->mip_levels,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
  };

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &barrier_to_dst);

  // Copy buffer to image
  VkBufferImageCopy region = {
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = image->array_layers,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent = {image->width, image->height, 1},
  };

  vkCmdCopyBufferToImage(cmd, staging_buffer, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Handle layout transition / queue ownership
  if (!use_transfer_queue) {
    // Same queue family - transition directly to shader read
    VkImageMemoryBarrier barrier_to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier_to_read);
  } else {
    // Different queue family - release ownership to graphics queue
    VkImageMemoryBarrier release_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                         NULL, 1, &release_barrier);
  }

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    log_error("Failed to end transfer command buffer");
    vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Create fence for synchronization
  VulkanFence fence;
  vulkan_fence_create(state, false_v, &fence);
  if (fence.handle == VK_NULL_HANDLE) {
    log_error("Failed to create transfer fence");
    vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Submit to transfer queue
  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
  };

  if (vkQueueSubmit(queue, 1, &submit_info, fence.handle) != VK_SUCCESS) {
    log_error("Failed to submit transfer command buffer");
    vulkan_fence_destroy(state, &fence);
    vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  // Wait for transfer to complete
  if (!vulkan_fence_wait(state, UINT64_MAX, &fence)) {
    log_error("Failed to wait for transfer fence");
    vulkan_fence_destroy(state, &fence);
    vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);
    if (graphics_cmd != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
    }
    return false_v;
  }

  vkQueueWaitIdle(queue);
  vulkan_fence_destroy(state, &fence);
  vkFreeCommandBuffers(state->device.logical_device, command_pool, 1, &cmd);

  // If using dedicated transfer queue, need to acquire on graphics queue
  // and transition to shader read using pre-allocated command buffer
  if (use_transfer_queue) {
    if (vkBeginCommandBuffer(graphics_cmd, &begin_info) != VK_SUCCESS) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      log_error("Failed to begin graphics command buffer; image ownership may "
                "be inconsistent");
      return false_v;
    }

    // Acquire ownership and transition to shader read
    VkImageMemoryBarrier acquire_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = (uint32_t)state->device.transfer_queue_index,
        .dstQueueFamilyIndex = (uint32_t)state->device.graphics_queue_index,
        .image = image->handle,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = image->mip_levels,
                .baseArrayLayer = 0,
                .layerCount = image->array_layers,
            },
    };

    vkCmdPipelineBarrier(graphics_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &acquire_barrier);

    if (vkEndCommandBuffer(graphics_cmd) != VK_SUCCESS) {
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      return false_v;
    }

    VulkanFence graphics_fence;
    vulkan_fence_create(state, false_v, &graphics_fence);

    VkSubmitInfo graphics_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &graphics_cmd,
    };

    if (vkQueueSubmit(state->device.graphics_queue, 1, &graphics_submit,
                      graphics_fence.handle) != VK_SUCCESS) {
      vulkan_fence_destroy(state, &graphics_fence);
      vkFreeCommandBuffers(state->device.logical_device,
                           state->device.graphics_command_pool, 1,
                           &graphics_cmd);
      return false_v;
    }

    vulkan_fence_wait(state, UINT64_MAX, &graphics_fence);
    vkQueueWaitIdle(state->device.graphics_queue);
    vulkan_fence_destroy(state, &graphics_fence);
    vkFreeCommandBuffers(state->device.logical_device,
                         state->device.graphics_command_pool, 1, &graphics_cmd);
  }

  return true_v;
}

bool8_t vulkan_image_copy_to_buffer_ex(VulkanBackendState *state,
                                       VulkanImage *image, VkBuffer buffer,
                                       uint64_t buffer_offset, uint32_t x,
                                       uint32_t y, uint32_t width,
                                       uint32_t height,
                                       VkImageAspectFlags aspect_flags,
                                       VulkanCommandBuffer *command_buffer) {
  assert_log(state != NULL, "State is NULL");
  assert_log(image != NULL, "Image is NULL");
  assert_log(buffer != VK_NULL_HANDLE, "Buffer is NULL");
  assert_log(command_buffer != NULL, "Command buffer is NULL");
  assert_log(width > 0 && height > 0,
             "Width and height must be greater than 0");
  assert_log(x + width <= image->width, "Region exceeds image width");
  assert_log(y + height <= image->height, "Region exceeds image height");

  VkBufferImageCopy region = {
      .bufferOffset = buffer_offset,
      .bufferRowLength = 0,   // Tightly packed
      .bufferImageHeight = 0, // Tightly packed
      .imageSubresource =
          {
              .aspectMask = aspect_flags,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .imageOffset = {(int32_t)x, (int32_t)y, 0},
      .imageExtent = {width, height, 1},
  };

  vkCmdCopyImageToBuffer(command_buffer->handle, image->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                         &region);

  return true_v;
}

bool8_t vulkan_image_copy_to_buffer(VulkanBackendState *state,
                                    VulkanImage *image, VkBuffer buffer,
                                    uint64_t buffer_offset, uint32_t x,
                                    uint32_t y, uint32_t width, uint32_t height,
                                    VulkanCommandBuffer *command_buffer) {
  return vulkan_image_copy_to_buffer_ex(
      state, image, buffer, buffer_offset, x, y, width, height,
      VK_IMAGE_ASPECT_COLOR_BIT, command_buffer);
}
