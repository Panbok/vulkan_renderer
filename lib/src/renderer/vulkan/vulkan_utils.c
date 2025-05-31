#include "vulkan_utils.h"

void vulkan_swapchain_query_details(VulkanBackendState *state,
                                    VkPhysicalDevice device,
                                    VulkanSwapchainDetails *details) {
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, state->surface,
                                            &details->capabilities);

  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, state->surface, &format_count,
                                       NULL);
  if (format_count != 0) {
    details->formats =
        array_create_VkSurfaceFormatKHR(state->arena, format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, state->surface, &format_count,
                                         details->formats.data);
  }

  uint32_t present_mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, state->surface,
                                            &present_mode_count, NULL);
  if (present_mode_count != 0) {
    details->present_modes =
        array_create_VkPresentModeKHR(state->arena, present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, state->surface,
                                              &present_mode_count,
                                              details->present_modes.data);
  }
}

Array_QueueFamilyIndex find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device) {
  Array_QueueFamilyIndex indices =
      array_create_QueueFamilyIndex(state->temp_arena, QUEUE_FAMILY_TYPE_COUNT);
  for (uint32_t i = 0; i < QUEUE_FAMILY_TYPE_COUNT; i++) {
    QueueFamilyIndex invalid_index = {
        .index = 0, .type = i, .is_present = false};
    array_set_QueueFamilyIndex(&indices, i, invalid_index);
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);
  if (queue_family_count == 0) {
    return indices;
  }

  Scratch scratch = scratch_create(state->temp_arena);
  Array_VkQueueFamilyProperties queue_family_properties =
      array_create_VkQueueFamilyProperties(state->temp_arena,
                                           queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           queue_family_properties.data);

  for (uint32_t i = 0; i < queue_family_count; i++) {
    QueueFamilyIndex *graphics_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);
    QueueFamilyIndex *present_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);

    if (graphics_index->is_present && present_index->is_present) {
      break;
    }

    VkQueueFamilyProperties properties =
        *array_get_VkQueueFamilyProperties(&queue_family_properties, i);

    if ((properties.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
        !graphics_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_GRAPHICS, .is_present = true};
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS, index);

      // it's ok to continue here, we need unique indices for graphics and
      // present queues
      continue;
    }

    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, state->surface,
                                         &presentSupport);
    if (presentSupport && !present_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_PRESENT, .is_present = true};
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT, index);
    }
  }

  array_destroy_VkQueueFamilyProperties(&queue_family_properties);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  return indices;
}