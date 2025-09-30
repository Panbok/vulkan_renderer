#pragma once

#include "containers/bitset.h"
#include "defines.h"
#include "platform/vulkan_platform.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool32_t vulkan_device_pick_physical_device(VulkanBackendState *state);

void vulkan_device_release_physical_device(VulkanBackendState *state);

bool32_t vulkan_device_create_logical_device(VulkanBackendState *state);

void vulkan_device_destroy_logical_device(VulkanBackendState *state);

void vulkan_device_get_information(VulkanBackendState *state,
                                   VkrDeviceInformation *device_information,
                                   Arena *temp_arena);

void vulkan_device_query_queue_indices(VulkanBackendState *state,
                                       Array_QueueFamilyIndex *indicies);

bool32_t vulkan_device_check_depth_format(VulkanDevice *device);

void vulkan_device_query_swapchain_details(VulkanBackendState *state,
                                           VkPhysicalDevice device,
                                           VulkanSwapchainDetails *details);

VkSurfaceFormatKHR *vulkan_device_choose_swap_surface_format(
    VulkanSwapchainDetails *swapchain_details);

VkPresentModeKHR vulkan_device_choose_swap_present_mode(
    VulkanSwapchainDetails *swapchain_details);

VkExtent2D
vulkan_device_choose_swap_extent(VulkanBackendState *state,
                                 VulkanSwapchainDetails *swapchain_details);