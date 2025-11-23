#include "vulkan_image.h"

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
  VkImageCreateInfo image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
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

  if (vkCreateImage(state->device.logical_device, &image_create_info,
                    state->allocator, &out_image->handle) != VK_SUCCESS) {
    log_error("Failed to create image");
    return false;
  }

  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(state->device.logical_device, out_image->handle,
                               &memory_requirements);

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

  if (vkBindImageMemory(state->device.logical_device, out_image->handle,
                        out_image->memory, 0) != VK_SUCCESS) {
    log_error("Failed to bind memory");
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
      vkFreeMemory(state->device.logical_device, out_image->memory,
                   state->allocator);
      vkDestroyImage(state->device.logical_device, out_image->handle,
                     state->allocator);
      return false;
    }
  }

  log_debug("Created Vulkan image: %p", out_image->handle);
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

  log_debug("Created Vulkan image view: %p", image->view);

  return true;
}

bool8_t vulkan_image_transition_layout(VulkanBackendState *state,
                                       VulkanImage *image,
                                       VulkanCommandBuffer *command_buffer,
                                       VkFormat format,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout) {
  return vulkan_image_transition_layout_range(state, image, command_buffer,
                                              format, old_layout, new_layout,
                                              NULL);
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

  VkImageSubresourceRange range = subresource_range
                                      ? *subresource_range
                                      : (VkImageSubresourceRange){
                                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
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

void vulkan_image_destroy(VulkanBackendState *state, VulkanImage *image) {
  log_debug("Destroying Vulkan image: %p", image->handle);

  if (image->view != VK_NULL_HANDLE) {
    vkDestroyImageView(state->device.logical_device, image->view,
                       state->allocator);
  }

  vkDestroyImage(state->device.logical_device, image->handle, state->allocator);
  vkFreeMemory(state->device.logical_device, image->memory, state->allocator);

  log_debug("Destroyed Vulkan image: %p", image->handle);
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
