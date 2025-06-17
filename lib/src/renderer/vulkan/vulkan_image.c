#include "vulkan_image.h"
#include "core/logger.h"
#include "defines.h"

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