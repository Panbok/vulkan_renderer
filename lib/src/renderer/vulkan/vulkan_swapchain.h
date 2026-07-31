#pragma once

#include "defines.h"
#include "vulkan_device.h"
#include "vulkan_image.h"
#include "vulkan_types.h"

bool32_t vulkan_swapchain_create(VulkanBackendState *state);

void vulkan_swapchain_destroy(VulkanBackendState *state);

/**
 * @brief Outcome of a swapchain acquire or present.
 *
 * `SKIP` and `FAILED` must stay distinct: a recreated or zero-extent swapchain
 * is a normal consequence of resizing and minimizing, while `FAILED` means the
 * device is unusable. Collapsing them turns every window resize into a fatal.
 */
typedef enum VulkanSwapchainResult {
  VULKAN_SWAPCHAIN_RESULT_OK = 0,
  /** No image was produced this frame; the caller should skip and retry next.
   */
  VULKAN_SWAPCHAIN_RESULT_SKIP,
  /** Unrecoverable device or surface error. */
  VULKAN_SWAPCHAIN_RESULT_FAILED,
} VulkanSwapchainResult;

VulkanSwapchainResult
vulkan_swapchain_acquire_next_image(VulkanBackendState *state, uint64_t timeout,
                                    VkSemaphore image_available_semaphore,
                                    VkFence in_flight_fence,
                                    uint32_t *out_image_index);

VulkanSwapchainResult
vulkan_swapchain_present(VulkanBackendState *state,
                         VkSemaphore queue_complete_semaphore,
                         uint32_t image_index);

VulkanSwapchainResult vulkan_swapchain_recreate(VulkanBackendState *state);
