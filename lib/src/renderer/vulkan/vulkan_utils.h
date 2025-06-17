#pragma once

#include "vulkan_types.h"

Array_QueueFamilyIndex find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device);

int32_t find_memory_index(VkPhysicalDevice device, uint32_t type_filter,
                          uint32_t property_flags);