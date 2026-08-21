#include "renderer/vulkan/vkr_vulkan_internal.h"

void vkr_vk_destroy_target_set(VkrVulkanRenderer *renderer,
                               VkrVulkanTargetSet *targets) {
  for (uint32_t i = 0; i < targets->image_count; ++i) {
    vkr_vk_destroy_image(renderer, &targets->images[i]);
  }
  MemZero(targets, sizeof(*targets));
}

bool8_t vkr_vk_create_target_set(VkrVulkanRenderer *renderer, uint32_t width,
                                 uint32_t height, uint32_t image_count,
                                 VkrVulkanTargetSet *out_targets) {
  if (!width || !height || !image_count ||
      image_count > VKR_VULKAN_TARGET_IMAGE_MAX) {
    return false_v;
  }
  MemZero(out_targets, sizeof(*out_targets));
  out_targets->width = width;
  out_targets->height = height;
  out_targets->image_count = image_count;
  for (uint32_t i = 0; i < image_count; ++i) {
    if (!vkr_vk_create_image(renderer, width, height,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             &out_targets->images[i])) {
      vkr_vk_destroy_target_set(renderer, out_targets);
      return false_v;
    }
  }
  return true_v;
}

void vkr_vk_destroy_window_target(VkrVulkanRenderer *renderer,
                                  VkrVulkanWindowTarget *target) {
  VkDevice device = vkr_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < target->image_count; ++i) {
    if (target->render_complete[i])
      vkDestroySemaphore(device, target->render_complete[i], NULL);
    if (target->present_complete[i])
      vkDestroyFence(device, target->present_complete[i], NULL);
  }
  if (target->swapchain)
    vkDestroySwapchainKHR(device, target->swapchain, NULL);
  MemZero(target, sizeof(*target));
}

bool8_t vkr_vk_window_presents_complete(VkrVulkanRenderer *renderer,
                                        VkrVulkanWindowTarget *target,
                                        bool8_t wait) {
  if (!vkr_vulkan_device_present_fences_enabled(renderer->device))
    return true_v;
  VkDevice device = vkr_vk_renderer_device(renderer);
  for (uint32_t i = 0u; i < target->image_count; ++i) {
    if (!target->present_fence_pending[i])
      continue;
    const VkResult result =
        wait ? vkWaitForFences(device, 1u, &target->present_complete[i],
                               VK_TRUE, UINT64_MAX)
             : vkGetFenceStatus(device, target->present_complete[i]);
    if (result == VK_NOT_READY)
      return false_v;
    if (result != VK_SUCCESS) {
      log_error("Vulkan present-fence completion failed: %d", result);
      return false_v;
    }
  }
  return true_v;
}

void vkr_vk_collect_retired_window_targets(VkrVulkanRenderer *renderer,
                                           uint64_t completed_submit_value) {
  (void)vkr_vulkan_reacquire_complete(&renderer->window_target.reacquire_state,
                                      completed_submit_value);
  if (!vkr_vulkan_device_present_fences_enabled(renderer->device) &&
      !renderer->window_target.reacquire_state.successor_present_complete)
    return;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    VkrVulkanRetiredWindowTarget *retired =
        &renderer->retired_window_targets[i];
    if (retired->occupied &&
        vkr_vk_window_presents_complete(renderer, &retired->target, false_v)) {
      vkr_vk_destroy_window_target(renderer, &retired->target);
      retired->occupied = false_v;
    }
  }
}

vkr_internal VkSurfaceFormatKHR vkr_vk_choose_surface_format(
    const VkSurfaceFormatKHR *formats, uint32_t count) {
  if (count == 1u && formats[0].format == VK_FORMAT_UNDEFINED)
    return (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_SRGB,
                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  return formats[0];
}

vkr_internal VkPresentModeKHR
vkr_vk_choose_present_mode(const VkPresentModeKHR *modes, uint32_t count,
                           VkrPresentMode requested_mode) {
  VkPresentModeKHR requested = VK_PRESENT_MODE_FIFO_KHR;
  if (requested_mode == VKR_PRESENT_MODE_IMMEDIATE)
    requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
  else if (requested_mode == VKR_PRESENT_MODE_MAILBOX ||
           requested_mode == VKR_PRESENT_MODE_DEFAULT)
    requested = VK_PRESENT_MODE_MAILBOX_KHR;
  for (uint32_t i = 0; i < count; ++i) {
    if (modes[i] == requested)
      return modes[i];
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

bool8_t vkr_vk_create_window_target(VkrVulkanRenderer *renderer,
                                    uint32_t requested_width,
                                    uint32_t requested_height,
                                    uint32_t requested_image_count,
                                    VkSwapchainKHR old_swapchain,
                                    VkrVulkanWindowTarget *out_target) {
  VkPhysicalDevice physical = vkr_vulkan_device_physical(renderer->device);
  VkDevice device = vkr_vk_renderer_device(renderer);
  VkSurfaceKHR surface = vkr_vulkan_device_surface(renderer->device);
  VkSurfaceCapabilitiesKHR capabilities = {0};
  uint32_t format_count = 0, mode_count = 0;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface,
                                                &capabilities) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           NULL) != VK_SUCCESS ||
      !format_count || format_count > 64u ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                NULL) != VK_SUCCESS ||
      !mode_count || mode_count > 64u)
    return false_v;
  VkSurfaceFormatKHR formats[64];
  VkPresentModeKHR modes[64];
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           formats) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                modes) != VK_SUCCESS)
    return false_v;

  const VkSurfaceFormatKHR surface_format =
      vkr_vk_choose_surface_format(formats, format_count);
  VkFormatProperties source_properties = {0}, target_properties = {0};
  vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                      &source_properties);
  vkGetPhysicalDeviceFormatProperties(physical, surface_format.format,
                                      &target_properties);
  if ((source_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0u ||
      (target_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0u)
    return false_v;
  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = Clamp(requested_width, capabilities.minImageExtent.width,
                         capabilities.maxImageExtent.width);
    extent.height = Clamp(requested_height, capabilities.minImageExtent.height,
                          capabilities.maxImageExtent.height);
  }
  if (!extent.width || !extent.height)
    return false_v;
  if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
      0u)
    return false_v;
  uint32_t minimum_count =
      Max(requested_image_count, capabilities.minImageCount);
  if (capabilities.maxImageCount)
    minimum_count = Min(minimum_count, capabilities.maxImageCount);
  VkCompositeAlphaFlagBitsKHR composite_alpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  const VkCompositeAlphaFlagBitsKHR composite_candidates[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
  };
  for (uint32_t i = 0; i < ArrayCount(composite_candidates); ++i) {
    if (capabilities.supportedCompositeAlpha & composite_candidates[i]) {
      composite_alpha = composite_candidates[i];
      break;
    }
  }
  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = minimum_count,
      .imageFormat = surface_format.format,
      .imageColorSpace = surface_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1u,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = composite_alpha,
      .presentMode = vkr_vk_choose_present_mode(
          modes, mode_count, renderer->config.requested_present_mode),
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
  };
  MemZero(out_target, sizeof(*out_target));
  if (vkCreateSwapchainKHR(device, &create_info, NULL,
                           &out_target->swapchain) != VK_SUCCESS)
    return false_v;
  uint32_t actual_count = 0;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              NULL) != VK_SUCCESS ||
      !actual_count || actual_count > VKR_VULKAN_SWAPCHAIN_IMAGE_MAX) {
    vkr_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->image_count = actual_count;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              out_target->images) != VK_SUCCESS) {
    vkr_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->width = extent.width;
  out_target->height = extent.height;
  out_target->format = surface_format.format;
  out_target->color_space = surface_format.colorSpace;
  out_target->present_mode = create_info.presentMode;
  for (uint32_t i = 0; i < actual_count; ++i) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &semaphore_info, NULL,
                          &out_target->render_complete[i]) != VK_SUCCESS) {
      vkr_vk_destroy_window_target(renderer, out_target);
      return false_v;
    }
    if (vkr_vulkan_device_present_fences_enabled(renderer->device)) {
      const VkFenceCreateInfo fence_info = {
          .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      if (vkCreateFence(device, &fence_info, NULL,
                        &out_target->present_complete[i]) != VK_SUCCESS) {
        vkr_vk_destroy_window_target(renderer, out_target);
        return false_v;
      }
    }
  }
  out_target->occupied = true_v;
  return true_v;
}

bool8_t vkr_vk_create_acquire_semaphores(VkrVulkanRenderer *renderer) {
  VkSemaphoreCreateInfo info = {.sType =
                                    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
    if (vkCreateSemaphore(vkr_vk_renderer_device(renderer), &info, NULL,
                          &renderer->acquire_semaphores[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

void vkr_vk_collect_retired_targets(VkrVulkanRenderer *renderer,
                                    uint64_t completed_value) {
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    VkrVulkanRetiredTargetSet *retired = &renderer->retired_targets[i];
    if (retired->occupied && retired->retire_value <= completed_value) {
      vkr_vk_destroy_target_set(renderer, &retired->targets);
      MemZero(retired, sizeof(*retired));
    }
  }
}

bool8_t vkr_vk_recreate_window_target(VkrVulkanRenderer *renderer,
                                      uint32_t width, uint32_t height,
                                      uint32_t image_count) {
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN)
    return false_v;
  if (!vkr_vulkan_renderer_wait_idle(renderer))
    return false_v;
  vkr_vk_collect_retired_window_targets(renderer, renderer->completed_value);
  VkrVulkanRetiredWindowTarget *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    if (!renderer->retired_window_targets[i].occupied) {
      retired = &renderer->retired_window_targets[i];
      break;
    }
  }
  if (!retired) {
    log_error("Vulkan exhausted %u deferred swapchains before a "
              "successor presentation completed",
              (uint32_t)ArrayCount(renderer->retired_window_targets));
    return false_v;
  }

  VkrVulkanWindowTarget replacement_window = {0};
  if (!vkr_vk_create_window_target(renderer, width, height, image_count,
                                   renderer->window_target.swapchain,
                                   &replacement_window))
    return false_v;
  VkrVulkanTargetSet replacement_targets = {0};
  if (!vkr_vk_create_target_set(
          renderer, replacement_window.width, replacement_window.height,
          replacement_window.image_count, &replacement_targets)) {
    vkr_vk_destroy_window_target(renderer, &replacement_window);
    return false_v;
  }
  retired->target = renderer->window_target;
  retired->occupied = true_v;
  renderer->window_target = replacement_window;
  vkr_vk_destroy_target_set(renderer, &renderer->targets);
  renderer->targets = replacement_targets;
  renderer->config.width = replacement_window.width;
  renderer->config.height = replacement_window.height;
  renderer->config.image_count = replacement_window.image_count;
  renderer->target_dirty = false_v;
  return true_v;
}

vkr_internal bool8_t vkr_vk_present_cancelled_frame(VkrVulkanRenderer *renderer,
                                                    VkrVulkanFrameSlot *slot) {
  VkDevice device = vkr_vk_renderer_device(renderer);
  VkrVulkanWindowTarget *window = &renderer->window_target;
  const uint32_t image_index = slot->image_index;
  if (vkResetCommandPool(device, slot->command_pool, 0u) != VK_SUCCESS)
    return false_v;
  const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
    return false_v;
  if (!window->image_presented[image_index]) {
    vkr_vk_cmd_image_barrier(
        slot->command_buffer, window->images[image_index],
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  if (vkEndCommandBuffer(slot->command_buffer) != VK_SUCCESS)
    return false_v;

  const uint64_t signal_value = renderer->submit_value + 1u;
  const VkCommandBufferSubmitInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command_buffer,
  };
  const VkSemaphoreSubmitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = renderer->acquire_semaphores[renderer->active_frame_slot],
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkSemaphoreSubmitInfo signals[] = {
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = renderer->timeline,
          .value = signal_value,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = window->render_complete[image_index],
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
  };
  const VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1u,
      .pWaitSemaphoreInfos = &wait_info,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_info,
      .signalSemaphoreInfoCount = ArrayCount(signals),
      .pSignalSemaphoreInfos = signals,
  };
  if (vkQueueSubmit2(vkr_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE) != VK_SUCCESS)
    return false_v;

  renderer->submit_value = signal_value;
  slot->retire_value = signal_value;
  vkr_vulkan_reacquire_record(&window->reacquire_state,
                              slot->reacquired_presented_image, signal_value);
  window->image_last_submit_value[image_index] = signal_value;
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    log_fatal("Vulkan command ring lost a cancelled frame after queue submit");

  const VkSwapchainPresentFenceInfoKHR present_fence_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
      .swapchainCount = 1u,
      .pFences = &window->present_complete[image_index],
  };
  const VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext =
          window->present_complete[image_index] ? &present_fence_info : NULL,
      .waitSemaphoreCount = 1u,
      .pWaitSemaphores = &window->render_complete[image_index],
      .swapchainCount = 1u,
      .pSwapchains = &window->swapchain,
      .pImageIndices = &image_index,
  };
  const VkrVulkanPresentResult disposition =
      vkr_vulkan_present_result_classify(vkQueuePresentKHR(
          vkr_vulkan_device_queue(renderer->device), &present_info));
  if (disposition.present_completion_tracking_required) {
    window->image_presented[image_index] = true_v;
    window->present_fence_pending[image_index] =
        window->present_complete[image_index] != VK_NULL_HANDLE;
  }
  if (disposition.target_recreate_required)
    renderer->target_dirty = true_v;
  if (!disposition.enqueue_state_known || disposition.device_lost ||
      disposition.acquired_image_recovery_required) {
    log_error("Vulkan could not enqueue cancelled-frame presentation");
    renderer->terminal_failure = true_v;
  }
  slot->acquired_window_image = false_v;
  slot->reacquired_presented_image = false_v;
  return true_v;
}

void vkr_vulkan_renderer_cancel_frame(VkrVulkanRenderer *renderer) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const bool8_t submitted = slot->acquired_window_image
                                ? vkr_vk_present_cancelled_frame(renderer, slot)
                                : false_v;
  if (!submitted)
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
  if (slot->acquired_window_image && !submitted)
    renderer->terminal_failure = true_v;
  renderer->frame_active = false_v;
  vkr_rg_end_frame(renderer->graph);
}

bool8_t vkr_vulkan_renderer_resize(VkrVulkanRenderer *renderer, uint32_t width,
                                   uint32_t height, uint32_t image_count) {
  if (!renderer || renderer->frame_active || renderer->terminal_failure ||
      !width || !height || !image_count ||
      image_count > VKR_VULKAN_TARGET_IMAGE_MAX) {
    return false_v;
  }
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    renderer->config.width = width;
    renderer->config.height = height;
    renderer->config.image_count = image_count;
    renderer->target_dirty = true_v;
    return true_v;
  }
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  vkr_vk_collect_retired_targets(renderer, completed);
  VkrVulkanRetiredTargetSet *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    if (!renderer->retired_targets[i].occupied) {
      retired = &renderer->retired_targets[i];
      break;
    }
  }
  if (!retired) {
    return false_v;
  }
  VkrVulkanTargetSet replacement;
  if (!vkr_vk_create_target_set(renderer, width, height, image_count,
                                &replacement)) {
    return false_v;
  }
  retired->targets = renderer->targets;
  retired->retire_value = renderer->submit_value;
  retired->occupied = true_v;
  renderer->targets = replacement;
  renderer->config.width = width;
  renderer->config.height = height;
  renderer->config.image_count = image_count;
  renderer->next_image_index = 0u;
  vkr_vk_collect_retired_targets(renderer, completed);
  return true_v;
}
