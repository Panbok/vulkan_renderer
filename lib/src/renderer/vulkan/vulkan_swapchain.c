#include "vulkan_swapchain.h"
#include "logger.h"
#include "vulkan_utils.h"

static VkSurfaceFormatKHR *
choose_swap_surface_format(VulkanSwapchainDetails *swapchain_details) {
  for (uint32_t i = 0; i < swapchain_details->formats.length; i++) {
    if (array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i)->format ==
            VK_FORMAT_B8G8R8A8_SRGB &&
        array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i)
                ->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i);
    }
  }

  return array_get_VkSurfaceFormatKHR(&swapchain_details->formats, 0);
}

static VkPresentModeKHR
choose_swap_present_mode(VulkanSwapchainDetails *swapchain_details) {
  for (uint32_t i = 0; i < swapchain_details->present_modes.length; i++) {
    VkPresentModeKHR present_mode =
        *array_get_VkPresentModeKHR(&swapchain_details->present_modes, i);
    if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return present_mode;
    }
  }

  return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D
choose_swap_extent(VulkanBackendState *state,
                   VulkanSwapchainDetails *swapchain_details) {
  if (swapchain_details->capabilities.currentExtent.width != UINT32_MAX) {
    return swapchain_details->capabilities.currentExtent;
  }

  WindowPixelSize window_size = window_get_pixel_size(state->window);
  VkExtent2D current_extent = {
      .width = window_size.width,
      .height = window_size.height,
  };

  current_extent.width =
      Clamp(current_extent.width,
            swapchain_details->capabilities.minImageExtent.width,
            swapchain_details->capabilities.maxImageExtent.width);
  current_extent.height =
      Clamp(current_extent.height,
            swapchain_details->capabilities.minImageExtent.height,
            swapchain_details->capabilities.maxImageExtent.height);

  return current_extent;
}

bool32_t vulkan_swapchain_create(VulkanBackendState *state) {
  VulkanSwapchainDetails swapchain_details = {0};
  vulkan_swapchain_query_details(state, state->physical_device,
                                 &swapchain_details);

  VkSurfaceFormatKHR *surface_format =
      choose_swap_surface_format(&swapchain_details);
  VkPresentModeKHR present_mode = choose_swap_present_mode(&swapchain_details);
  VkExtent2D extent = choose_swap_extent(state, &swapchain_details);

  uint32_t image_count = swapchain_details.capabilities.minImageCount + 1;
  if (swapchain_details.capabilities.maxImageCount > 0 &&
      image_count > swapchain_details.capabilities.maxImageCount) {
    image_count = swapchain_details.capabilities.maxImageCount;
  }

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
      find_queue_family_indices(state, state->physical_device);
  if (indices.length > 1) {
    // we are fine with alloc temp arena for indicies, Vulkan will copy indices
    // to device memory once the swapchain is created
    Scratch scratch = scratch_create(state->temp_arena);
    Array_uint32_t queue_family_indices =
        array_create_uint32_t(scratch.arena, indices.length);
    for (uint32_t i = 0; i < indices.length; i++) {
      QueueFamilyIndex *index = array_get_QueueFamilyIndex(&indices, i);
      array_set_uint32_t(&queue_family_indices, i, index->index);
    }

    create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    create_info.queueFamilyIndexCount = indices.length;
    create_info.pQueueFamilyIndices = queue_family_indices.data;

    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
  } else {
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  VkSwapchainKHR swapchain;
  if (vkCreateSwapchainKHR(state->device, &create_info, NULL, &swapchain) !=
      VK_SUCCESS) {
    return false;
  }

  state->swapchain = swapchain;

  vkGetSwapchainImagesKHR(state->device, state->swapchain, &image_count, NULL);
  if (image_count != 0) {
    state->swapChainImages = array_create_VkImage(state->arena, image_count);
    vkGetSwapchainImagesKHR(state->device, state->swapchain, &image_count,
                            state->swapChainImages.data);
  }

  state->swapChainImageFormat = surface_format->format;
  state->swapChainExtent = extent;

  state->swapChainImageViews =
      array_create_VkImageView(state->arena, image_count);
  for (uint32_t i = 0; i < image_count; i++) {
    VkImageViewCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = state->swapChainImages.data[i],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = state->swapChainImageFormat,
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

    if (vkCreateImageView(state->device, &create_info, NULL,
                          &state->swapChainImageViews.data[i]) != VK_SUCCESS) {
      return false;
    }
  }

  log_debug("Swapchain created with handle %p", swapchain);

  return true;
}

void vulkan_swapchain_destroy(VulkanBackendState *state) {
  assert_log(state != NULL, "State not initialized");
  assert_log(state->swapchain != VK_NULL_HANDLE, "Swapchain not initialized");

  log_debug("Destroying swapchain");

  for (uint32_t i = 0; i < state->swapChainImageViews.length; i++) {
    vkDestroyImageView(state->device, state->swapChainImageViews.data[i], NULL);
  }
  array_destroy_VkImageView(&state->swapChainImageViews);
  array_destroy_VkImage(&state->swapChainImages);
  vkDestroySwapchainKHR(state->device, state->swapchain, NULL);
  state->swapchain = VK_NULL_HANDLE;
}