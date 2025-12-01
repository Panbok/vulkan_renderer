#include "vulkan_swapchain.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"
#include "vulkan_backend.h"
#include "vulkan_utils.h"

bool32_t vulkan_swapchain_create(VulkanBackendState *state) {
  VulkanSwapchainDetails swapchain_details = {0};
  vulkan_device_query_swapchain_details(state, state->device.physical_device,
                                        &swapchain_details);

  VkSurfaceFormatKHR *surface_format =
      vulkan_device_choose_swap_surface_format(&swapchain_details);
  VkPresentModeKHR present_mode =
      vulkan_device_choose_swap_present_mode(&swapchain_details);
  VkExtent2D extent =
      vulkan_device_choose_swap_extent(state, &swapchain_details);

  uint32_t image_count = swapchain_details.capabilities.minImageCount + 1;
  if (swapchain_details.capabilities.maxImageCount > 0 &&
      image_count > swapchain_details.capabilities.maxImageCount) {
    image_count = swapchain_details.capabilities.maxImageCount;
  }

  state->swapchain.image_count = image_count;

  // configure the number of frames to buffer (double/triple buffering)
  state->swapchain.max_in_flight_frames = Min(image_count, BUFFERING_FRAMES);

  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = state->surface,
      .minImageCount = image_count,
      .imageFormat = surface_format->format,
      .imageColorSpace = surface_format->colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .preTransform = swapchain_details.capabilities.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE,
  };

  Array_QueueFamilyIndex indices =
      find_queue_family_indices(state, state->device.physical_device);
  if (indices.length > 1) {
    VkrAllocator scratch_alloc = {.ctx = state->temp_arena};
    vkr_allocator_arena(&scratch_alloc);
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&scratch_alloc);
    if (!vkr_allocator_scope_is_valid(&scope)) {
      return false;
    }
    Array_uint32_t queue_family_indices =
        array_create_uint32_t(&scratch_alloc, indices.length);
    for (uint32_t i = 0; i < indices.length; i++) {
      QueueFamilyIndex *index = array_get_QueueFamilyIndex(&indices, i);
      array_set_uint32_t(&queue_family_indices, i, index->index);
    }

    create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    create_info.queueFamilyIndexCount = indices.length;
    create_info.pQueueFamilyIndices = queue_family_indices.data;

    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  } else {
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  VkSwapchainKHR swapchain;
  if (vkCreateSwapchainKHR(state->device.logical_device, &create_info,
                           state->allocator, &swapchain) != VK_SUCCESS) {
    return false;
  }

  state->swapchain.handle = swapchain;

  vkGetSwapchainImagesKHR(state->device.logical_device, state->swapchain.handle,
                          &image_count, NULL);
  if (image_count == 0) {
    log_error("Swapchain has no images");
    return false;
  }

  VkrAllocator swap_alloc = {.ctx = state->swapchain_arena};
  vkr_allocator_arena(&swap_alloc);

  state->swapchain.images =
      array_create_VkImage(&swap_alloc, image_count);
  state->swapchain.images.allocator = NULL; // arena-owned
  vkGetSwapchainImagesKHR(state->device.logical_device, state->swapchain.handle,
                          &image_count, state->swapchain.images.data);

  state->swapchain.format = surface_format->format;
  state->swapchain.extent = extent;

  state->swapchain.image_views =
      array_create_VkImageView(&swap_alloc, image_count);
  state->swapchain.image_views.allocator = NULL; // arena-owned
  for (uint32_t i = 0; i < image_count; i++) {
    VkImageViewCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = state->swapchain.images.data[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = state->swapchain.format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };

    if (vkCreateImageView(
            state->device.logical_device, &create_info, state->allocator,
            &state->swapchain.image_views.data[i]) != VK_SUCCESS) {
      for (uint32_t j = 0; j < i; j++) {
        vkDestroyImageView(state->device.logical_device,
                           state->swapchain.image_views.data[j],
                           state->allocator);
      }
      array_destroy_VkImageView(&state->swapchain.image_views);
      array_destroy_VkImage(&state->swapchain.images);
      return false;
    }
  }

  if (!vulkan_device_check_depth_format(&state->device)) {
    log_error("Failed to find suitable depth format");
    return false;
  }

  if (!vulkan_image_create(state, VK_IMAGE_TYPE_2D, extent.width, extent.height,
                           state->device.depth_format, VK_IMAGE_TILING_OPTIMAL,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, 1,
                           VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT,
                           &state->swapchain.depth_attachment)) {
    log_error("Failed to create depth attachment");
    return false;
  }

  log_debug("Swapchain created with handle %p", swapchain);

  return true;
}

void vulkan_swapchain_destroy(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  log_debug("Destroying swapchain");

  vkDeviceWaitIdle(state->device.logical_device);

  vulkan_image_destroy(state, &state->swapchain.depth_attachment);

  // we are only destroying the array struct for holding images and
  // deallocating view images, "real" images are owned by the swapchain
  // and will be destroyed when the swapchain is destroyed
  for (uint32_t i = 0; i < state->swapchain.image_views.length; i++) {
    vkDestroyImageView(state->device.logical_device,
                       state->swapchain.image_views.data[i], state->allocator);
  }
  array_destroy_VkImageView(&state->swapchain.image_views);
  array_destroy_VkImage(&state->swapchain.images);

  vkDestroySwapchainKHR(state->device.logical_device, state->swapchain.handle,
                        state->allocator);

  arena_reset_to(state->swapchain_arena, 0, ARENA_MEMORY_TAG_RENDERER);

  state->swapchain.handle = VK_NULL_HANDLE;
}

bool8_t
vulkan_swapchain_acquire_next_image(VulkanBackendState *state, uint64_t timeout,
                                    VkSemaphore image_available_semaphore,
                                    VkFence in_flight_fence,
                                    uint32_t *out_image_index) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");
  assert_log(timeout > 0, "Timeout is 0");
  assert_log(image_available_semaphore != NULL,
             "Image available semaphore is NULL");
  assert_log(out_image_index != NULL, "Out image index is NULL");

  VkResult result = vkAcquireNextImageKHR(
      state->device.logical_device, state->swapchain.handle, timeout,
      image_available_semaphore, in_flight_fence, out_image_index);

  if (result != VK_SUCCESS) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      log_warn("Swapchain out of date during image acquisition, recreating...");

      // Attempt to recreate the swapchain
      if (!vulkan_backend_recreate_swapchain(state)) {
        log_error("Failed to recreate swapchain during image acquisition");
        return false;
      }

      // Try acquiring again after recreation
      result = vkAcquireNextImageKHR(
          state->device.logical_device, state->swapchain.handle, timeout,
          image_available_semaphore, in_flight_fence, out_image_index);

      if (result != VK_SUCCESS) {
        log_error("Failed to acquire image even after swapchain recreation: %d",
                  result);
        return false;
      }

      log_debug("Successfully acquired image after swapchain recreation");
      return true;
    } else if (result == VK_SUBOPTIMAL_KHR) {
      log_warn("Swapchain suboptimal during image acquisition");
      // Continue despite suboptimal result
    } else {
      log_error("Failed to acquire next image with error code: %d", result);
      return false;
    }
  }

  return true;
}

bool8_t vulkan_swapchain_present(VulkanBackendState *state,
                                 VkSemaphore queue_complete_semaphore,
                                 uint32_t image_index) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");
  assert_log(queue_complete_semaphore != NULL,
             "Queue complete semaphore is NULL");
  assert_log(image_index < state->swapchain.images.length,
             "Image index out of bounds");

  VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &queue_complete_semaphore,
      .swapchainCount = 1,
      .pSwapchains = &state->swapchain.handle,
      .pImageIndices = &image_index,
  };

  VkResult result =
      vkQueuePresentKHR(state->device.present_queue, &present_info);
  if (result != VK_SUCCESS) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      log_warn("Swapchain out of date during present, recreating...");

      // Attempt to recreate the swapchain
      if (!vulkan_backend_recreate_swapchain(state)) {
        log_error("Failed to recreate swapchain during present");
        return false;
      }

      log_debug("Swapchain recreated successfully after present failure");
      // Note: We don't retry present after recreation since the frame is
      // already done The next frame will use the new swapchain
    } else if (result == VK_SUBOPTIMAL_KHR) {
      log_warn("Swapchain suboptimal during present");
      // Continue despite suboptimal result
    } else {
      log_error("Failed to present image with error code: %d", result);
      return false;
    }
  }

  // Move to next frame (should cycle through max_in_flight_frames, not image
  // count)
  state->current_frame =
      (state->current_frame + 1) % state->swapchain.max_in_flight_frames;

  return true;
}

bool32_t vulkan_swapchain_recreate(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  log_debug("Recreating swapchain");

  vulkan_swapchain_destroy(state);
  if (!vulkan_swapchain_create(state)) {
    log_error("Failed to recreate swapchain");
    return false;
  }

  return true;
}
