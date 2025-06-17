#include "vulkan_utils.h"

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
    QueueFamilyIndex *transfer_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_TRANSFER);

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
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS, index);

      // it's ok to continue here, we need unique indices for graphics and
      // present queues
      continue;
    }

    if ((properties.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
        !transfer_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_TRANSFER, .is_present = true};
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_TRANSFER, index);

      // it's ok to continue here, we need unique indices for transfer and
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
