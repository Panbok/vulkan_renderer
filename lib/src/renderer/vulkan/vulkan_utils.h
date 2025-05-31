#pragma once

#include "vulkan_types.h"

void vulkan_swapchain_query_details(VulkanBackendState *state,
                                    VkPhysicalDevice device,
                                    VulkanSwapchainDetails *details);

Array_QueueFamilyIndex find_queue_family_indices(VulkanBackendState *state,
                                                 VkPhysicalDevice device);