#include "vulkan_swapchain.h"
#include "core/logger.h"
#include "defines.h"
#include "memory/arena.h"
#include "platform/vkr_platform.h"
#include "vulkan_backend.h"
#include "vulkan_utils.h"

bool32_t vulkan_swapchain_create(VulkanBackendState *state) {
  VulkanSwapchainDetails swapchain_details = {0};
  vulkan_device_query_swapchain_details(state, state->device.physical_device,
                                        &swapchain_details);

  VkSurfaceFormatKHR *surface_format =
      vulkan_device_choose_swap_surface_format(&swapchain_details);
  VkPresentModeKHR present_mode = vulkan_device_choose_swap_present_mode(
      &swapchain_details, state->requested_present_mode);
  state->actual_present_mode =
      present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? VKR_PRESENT_MODE_IMMEDIATE
      : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? VKR_PRESENT_MODE_MAILBOX
                                                    : VKR_PRESENT_MODE_FIFO;
  VkExtent2D extent =
      vulkan_device_choose_swap_extent(state, &swapchain_details);

  uint32_t requested_image_count =
      swapchain_details.capabilities.minImageCount + 1;
  if (swapchain_details.capabilities.maxImageCount > 0 &&
      requested_image_count > swapchain_details.capabilities.maxImageCount) {
    requested_image_count = swapchain_details.capabilities.maxImageCount;
  }
  if (state->capture_enabled &&
      !(swapchain_details.capabilities.supportedUsageFlags &
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    log_error("Surface does not support swapchain transfer-source capture");
    return false;
  }

  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = state->surface,
      .minImageCount = requested_image_count,
      .imageFormat = surface_format->format,
      .imageColorSpace = surface_format->colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          (state->capture_enabled ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
      .preTransform = swapchain_details.capabilities.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = VK_NULL_HANDLE,
  };

  QueueFamilyIndexResult indices =
      find_queue_family_indices(state, state->device.physical_device);
  uint32_t queue_family_indices[QUEUE_FAMILY_TYPE_COUNT] = {0};
  if (indices.length > 1) {
    for (uint32_t i = 0; i < indices.length; i++) {
      queue_family_indices[i] = indices.indices[i].index;
    }

    create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    create_info.queueFamilyIndexCount = indices.length;
    create_info.pQueueFamilyIndices = queue_family_indices;
  } else {
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  VkSwapchainKHR swapchain;
  if (vkCreateSwapchainKHR(state->device.logical_device, &create_info,
                           state->allocator, &swapchain) != VK_SUCCESS) {
    return false;
  }

  state->swapchain.handle = swapchain;

  uint32_t image_count = 0;
  VkResult images_result =
      vkGetSwapchainImagesKHR(state->device.logical_device,
                              state->swapchain.handle, &image_count, NULL);
  if (images_result != VK_SUCCESS || image_count == 0) {
    log_error("Failed to query swapchain images: %d", images_result);
    return false;
  }

  // The implementation may allocate more images than minImageCount. Every
  // per-image array must follow this actual count, not the request.
  state->swapchain.image_count = image_count;
  state->swapchain.max_in_flight_frames = Min(image_count, BUFFERING_FRAMES);

  state->swapchain.images =
      array_create_VkImage(&state->swapchain_alloc, image_count);
  images_result = vkGetSwapchainImagesKHR(state->device.logical_device,
                                          state->swapchain.handle, &image_count,
                                          state->swapchain.images.data);
  if (images_result != VK_SUCCESS) {
    log_error("Failed to retrieve swapchain images: %d", images_result);
    array_destroy_VkImage(&state->swapchain.images);
    return false;
  }

  state->swapchain.format = surface_format->format;
  state->swapchain.color_space = surface_format->colorSpace;
  state->swapchain.extent = extent;

  state->swapchain.image_views =
      array_create_VkImageView(&state->swapchain_alloc, image_count);
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

  if (!vulkan_device_check_depth_format(&state->device,
                                        state->capture_enabled)) {
    log_error("Failed to find suitable depth format");
    return false;
  }

  const VulkanImageDescription depth_desc = {
      .image_type = VK_IMAGE_TYPE_2D,
      .width = extent.width,
      .height = extent.height,
      .format = state->device.depth_format,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               (state->capture_enabled ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
      .memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .mip_levels = 1,
      .array_layers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .view_type = VK_IMAGE_VIEW_TYPE_2D,
      .view_aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN,
  };
  if (!vulkan_image_create(state, &depth_desc,
                           &state->swapchain.depth_attachment)) {
    log_error("Failed to create depth attachment");
    return false;
  }

  log_debug("Swapchain created with handle %p", swapchain);

  return true;
}

vkr_internal VulkanSwapchainResult vulkan_swapchain_create_with_old(
    VulkanBackendState *state, VkSwapchainKHR old_swapchain) {
  // Query new swapchain details FIRST, before destroying anything
  VulkanSwapchainDetails swapchain_details = {0};
  vulkan_device_query_swapchain_details(state, state->device.physical_device,
                                        &swapchain_details);

  VkSurfaceFormatKHR *surface_format =
      vulkan_device_choose_swap_surface_format(&swapchain_details);
  VkPresentModeKHR present_mode = vulkan_device_choose_swap_present_mode(
      &swapchain_details, state->requested_present_mode);
  state->actual_present_mode =
      present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? VKR_PRESENT_MODE_IMMEDIATE
      : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? VKR_PRESENT_MODE_MAILBOX
                                                    : VKR_PRESENT_MODE_FIFO;
  VkExtent2D extent =
      vulkan_device_choose_swap_extent(state, &swapchain_details);

  // Check for zero extent (window minimized) - skip recreation
  // Important: return false WITHOUT destroying anything
  if (extent.width == 0 || extent.height == 0) {
    log_warn("Swapchain extent is zero, skipping recreation");
    return VULKAN_SWAPCHAIN_RESULT_SKIP;
  }

  uint32_t requested_image_count =
      swapchain_details.capabilities.minImageCount + 1;
  if (swapchain_details.capabilities.maxImageCount > 0 &&
      requested_image_count > swapchain_details.capabilities.maxImageCount) {
    requested_image_count = swapchain_details.capabilities.maxImageCount;
  }
  if (state->capture_enabled &&
      !(swapchain_details.capabilities.supportedUsageFlags &
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    log_error("Surface no longer supports swapchain transfer-source capture");
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = state->surface,
      .minImageCount = requested_image_count,
      .imageFormat = surface_format->format,
      .imageColorSpace = surface_format->colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1,
      .imageUsage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          (state->capture_enabled ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
      .preTransform = swapchain_details.capabilities.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain, // Pass old swapchain for smooth transition
  };

  QueueFamilyIndexResult indices =
      find_queue_family_indices(state, state->device.physical_device);
  uint32_t queue_family_indices[QUEUE_FAMILY_TYPE_COUNT] = {0};
  if (indices.length > 1) {
    for (uint32_t i = 0; i < indices.length; i++) {
      queue_family_indices[i] = indices.indices[i].index;
    }

    create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    create_info.queueFamilyIndexCount = indices.length;
    create_info.pQueueFamilyIndices = queue_family_indices;
  } else {
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  // Try to create new swapchain FIRST (before destroying old resources)
  VkSwapchainKHR new_swapchain;
  VkResult result =
      vkCreateSwapchainKHR(state->device.logical_device, &create_info,
                           state->allocator, &new_swapchain);
  if (result != VK_SUCCESS) {
    log_error("Failed to create swapchain with old reference: %d", result);
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  uint32_t image_count = 0;
  result = vkGetSwapchainImagesKHR(state->device.logical_device, new_swapchain,
                                   &image_count, NULL);
  if (result != VK_SUCCESS || image_count == 0) {
    log_error("Failed to query recreated swapchain images: %d", result);
    vkDestroySwapchainKHR(state->device.logical_device, new_swapchain,
                          state->allocator);
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  // New swapchain created successfully - NOW destroy old resources
  vulkan_image_destroy(state, &state->swapchain.depth_attachment);

  for (uint32_t i = 0; i < state->swapchain.image_views.length; i++) {
    vkDestroyImageView(state->device.logical_device,
                       state->swapchain.image_views.data[i], state->allocator);
  }
  array_destroy_VkImageView(&state->swapchain.image_views);
  array_destroy_VkImage(&state->swapchain.images);

  // Reset swapchain arena for new allocations
  arena_reset_to(state->swapchain_arena, 0, ARENA_MEMORY_TAG_RENDERER);

  // Update state with new values
  state->swapchain.image_count = image_count;
  state->swapchain.max_in_flight_frames = Min(image_count, BUFFERING_FRAMES);
  state->swapchain.handle = new_swapchain;

  state->swapchain.images =
      array_create_VkImage(&state->swapchain_alloc, image_count);
  result = vkGetSwapchainImagesKHR(state->device.logical_device,
                                   state->swapchain.handle, &image_count,
                                   state->swapchain.images.data);
  if (result != VK_SUCCESS) {
    log_error("Failed to retrieve recreated swapchain images: %d", result);
    array_destroy_VkImage(&state->swapchain.images);
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  state->swapchain.format = surface_format->format;
  state->swapchain.color_space = surface_format->colorSpace;
  state->swapchain.extent = extent;

  state->swapchain.image_views =
      array_create_VkImageView(&state->swapchain_alloc, image_count);
  for (uint32_t i = 0; i < image_count; i++) {
    VkImageViewCreateInfo view_create_info = {
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
            state->device.logical_device, &view_create_info, state->allocator,
            &state->swapchain.image_views.data[i]) != VK_SUCCESS) {
      for (uint32_t j = 0; j < i; j++) {
        vkDestroyImageView(state->device.logical_device,
                           state->swapchain.image_views.data[j],
                           state->allocator);
      }
      array_destroy_VkImageView(&state->swapchain.image_views);
      array_destroy_VkImage(&state->swapchain.images);
      return VULKAN_SWAPCHAIN_RESULT_FAILED;
    }
  }

  if (!vulkan_device_check_depth_format(&state->device,
                                        state->capture_enabled)) {
    log_error("Failed to find suitable depth format");
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  const VulkanImageDescription depth_desc = {
      .image_type = VK_IMAGE_TYPE_2D,
      .width = extent.width,
      .height = extent.height,
      .format = state->device.depth_format,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               (state->capture_enabled ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
      .memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .mip_levels = 1,
      .array_layers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .view_type = VK_IMAGE_VIEW_TYPE_2D,
      .view_aspect_flags = VK_IMAGE_ASPECT_DEPTH_BIT,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN,
  };
  if (!vulkan_image_create(state, &depth_desc,
                           &state->swapchain.depth_attachment)) {
    log_error("Failed to create depth attachment");
    return VULKAN_SWAPCHAIN_RESULT_FAILED;
  }

  log_debug("Swapchain recreated with handle %p (old: %p)", new_swapchain,
            old_swapchain);

  return VULKAN_SWAPCHAIN_RESULT_OK;
}

// Internal function to destroy old swapchain handle after recreation
static void vulkan_swapchain_destroy_old_handle(VulkanBackendState *state,
                                                VkSwapchainKHR old_swapchain) {
  // Destroy the old swapchain handle
  // Note: When oldSwapchain is passed to vkCreateSwapchainKHR, the driver
  // retires the old swapchain but doesn't destroy it - we still need to do that
  // The vkQueueWaitIdle at the start of recreation ensures GPU is done with it
  if (old_swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(state->device.logical_device, old_swapchain,
                          state->allocator);
  }

  log_debug("Old swapchain handle destroyed");
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

VulkanSwapchainResult
vulkan_swapchain_acquire_next_image(VulkanBackendState *state, uint64_t timeout,
                                    VkSemaphore image_available_semaphore,
                                    VkFence in_flight_fence,
                                    uint32_t *out_image_index) {
  assert_log(state != NULL, "State not initialized");
  assert_log(timeout > 0, "Timeout is 0");
  assert_log(image_available_semaphore != NULL,
             "Image available semaphore is NULL");
  assert_log(out_image_index != NULL, "Out image index is NULL");

  // Recreation in progress or failed; not an error, just nothing to draw into.
  if (state->swapchain.handle == VK_NULL_HANDLE) {
    log_warn("Swapchain handle is NULL, skipping acquire");
    return VULKAN_SWAPCHAIN_RESULT_SKIP;
  }

  // Zero-sized swapchain (window minimized).
  if (state->swapchain.extent.width == 0 ||
      state->swapchain.extent.height == 0) {
    log_debug("Swapchain extent is zero, skipping acquire");
    return VULKAN_SWAPCHAIN_RESULT_SKIP;
  }

  // Store current handle to detect if recreation happened during acquire
  VkSwapchainKHR current_handle = state->swapchain.handle;

  VkResult result = vkAcquireNextImageKHR(
      state->device.logical_device, current_handle, timeout,
      image_available_semaphore, in_flight_fence, out_image_index);

  if (result != VK_SUCCESS) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      log_warn("Swapchain out of date during image acquisition, recreating...");

      VkrRendererError recreate_result =
          vulkan_backend_recreate_swapchain(state);
      if (recreate_result == VKR_RENDERER_ERROR_FRAME_SKIPPED) {
        return VULKAN_SWAPCHAIN_RESULT_SKIP;
      }
      if (recreate_result != VKR_RENDERER_ERROR_NONE) {
        log_error("Failed to recreate swapchain during image acquisition");
        return VULKAN_SWAPCHAIN_RESULT_FAILED;
      }

      // Do not retry the acquire here. The recreate destroyed and rebuilt the
      // sync objects, so image_available_semaphore may no longer be the one the
      // caller will wait on. Skip this frame and let the next one acquire
      // against consistent state.
      log_debug("Swapchain recreated; skipping this frame's acquire");
      return VULKAN_SWAPCHAIN_RESULT_SKIP;
    } else if (result == VK_SUBOPTIMAL_KHR) {
      log_warn("Swapchain suboptimal during image acquisition");
      // Continue despite suboptimal result: the semaphore was still signalled.
    } else {
      log_error("Failed to acquire next image with error code: %d", result);
      return VULKAN_SWAPCHAIN_RESULT_FAILED;
    }
  }

  return VULKAN_SWAPCHAIN_RESULT_OK;
}

VulkanSwapchainResult
vulkan_swapchain_present(VulkanBackendState *state,
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

#if VKR_METRICS_ENABLED
  const float64_t present_start = vkr_platform_get_absolute_time();
#endif
  VkResult result = vulkan_backend_queue_present_locked(
      state, state->device.present_queue, &present_info);
#if VKR_METRICS_ENABLED
  state->last_present_duration_ns =
      (uint64_t)((vkr_platform_get_absolute_time() - present_start) *
                 1000000000.0);
  state->last_present_duration_valid = true_v;
#endif
  if (result != VK_SUCCESS) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      log_warn("Swapchain out of date during present, recreating...");

      VkrRendererError recreate_result =
          vulkan_backend_recreate_swapchain(state);
      if (recreate_result == VKR_RENDERER_ERROR_FRAME_SKIPPED) {
        return VULKAN_SWAPCHAIN_RESULT_SKIP;
      }
      if (recreate_result != VKR_RENDERER_ERROR_NONE) {
        log_error("Failed to recreate swapchain during present");
        return VULKAN_SWAPCHAIN_RESULT_FAILED;
      }

      // The frame's work was already submitted and its fence will signal; only
      // the presentation was lost. Report SKIP, not FAILED.
      log_debug("Swapchain recreated successfully after present failure");
      return VULKAN_SWAPCHAIN_RESULT_SKIP;
    } else if (result == VK_SUBOPTIMAL_KHR) {
      log_warn("Swapchain suboptimal during present");
      // Continue despite suboptimal result
    } else {
      log_error("Failed to present image with error code: %d", result);
      return VULKAN_SWAPCHAIN_RESULT_FAILED;
    }
  }

  // The frame-in-flight slot is advanced by renderer_vulkan_end_frame, right
  // after queue submit and before this call. Advancing again here would skip a
  // slot every frame -- and with max_in_flight_frames == 2 it pinned the slot
  // at 0 forever, reusing one fence and one acquire semaphore for every frame.
  return VULKAN_SWAPCHAIN_RESULT_OK;
}

VulkanSwapchainResult vulkan_swapchain_recreate(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain.handle != VK_NULL_HANDLE,
             "Swapchain not initialized");

  log_debug("Recreating swapchain");

  // Store old swapchain handle for proper recreation
  VkSwapchainKHR old_swapchain = state->swapchain.handle;

  // Create the new swapchain with the old swapchain reference. SKIP is the only
  // outcome that guarantees the old WSI state remains usable; FAILED may occur
  // after vkCreateSwapchainKHR retired the old handle and is therefore fatal to
  // this frame lifecycle.
  VulkanSwapchainResult result =
      vulkan_swapchain_create_with_old(state, old_swapchain);
  if (result != VULKAN_SWAPCHAIN_RESULT_OK) {
    if (state->swapchain.handle != old_swapchain) {
      // The helper committed the new handle before a later view/depth setup
      // failure. The old handle is retired and no longer stored in state.
      vulkan_swapchain_destroy_old_handle(state, old_swapchain);
    }
    log_warn("Swapchain recreation skipped or failed");
    return result;
  }

  // Creation succeeded - now destroy the old swapchain handle
  // Note: Old image views and depth attachment were already destroyed in
  // vulkan_swapchain_create_with_old after the new swapchain was successfully
  // created
  vulkan_swapchain_destroy_old_handle(state, old_swapchain);

  return VULKAN_SWAPCHAIN_RESULT_OK;
}
