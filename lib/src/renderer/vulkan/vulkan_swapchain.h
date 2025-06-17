#pragma once

#include "defines.h"
#include "vulkan_device.h"
#include "vulkan_image.h"
#include "vulkan_types.h"

bool32_t vulkan_swapchain_create(VulkanBackendState *state);

void vulkan_swapchain_destroy(VulkanBackendState *state);

bool8_t
vulkan_swapchain_acquire_next_image(VulkanBackendState *state, uint64_t timeout,
                                    VkSemaphore image_available_semaphore,
                                    VkFence in_flight_fence,
                                    uint32_t *out_image_index);

bool8_t vulkan_swapchain_present(VulkanBackendState *state,
                                 VkSemaphore queue_complete_semaphore,
                                 uint32_t image_index);

bool32_t vulkan_swapchain_recreate(VulkanBackendState *state);