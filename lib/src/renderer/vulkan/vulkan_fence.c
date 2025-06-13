#include "vulkan_fence.h"
#include "core/logger.h"
#include "defines.h"
#include <stdint.h>

void vulkan_fence_create(VulkanBackendState *state, bool8_t is_signaled,
                         VulkanFence *out_fence) {
  assert_log(out_fence != NULL, "Vulkan fence is NULL");
  assert_log(state != NULL, "Vulkan backend state is NULL");

  VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .pNext = NULL,
  };

  if (is_signaled) {
    out_fence->is_signaled = true_v;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  }

  if (vkCreateFence(state->device, &fence_info, state->allocator,
                    &out_fence->handle) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan fence");
  }
}

void vulkan_fence_destroy(VulkanBackendState *state, VulkanFence *fence) {
  assert_log(state != NULL, "Vulkan backend state is NULL");

  if (fence->handle) {
    vkDestroyFence(state->device, fence->handle, state->allocator);
    fence->handle = VK_NULL_HANDLE;
  }
  fence->is_signaled = false_v;
}

bool8_t vulkan_fence_wait(VulkanBackendState *state, uint64_t timeout,
                          VulkanFence *fence) {
  assert_log(state != NULL, "Vulkan backend state is NULL");
  assert_log(fence != NULL, "Vulkan fence is NULL");

  if (fence->is_signaled) {
    return true_v;
  }

  VkResult result =
      vkWaitForFences(state->device, 1, &fence->handle, true_v, timeout);
  switch (result) {
  case VK_SUCCESS:
    fence->is_signaled = true_v;
    return true_v;
  case VK_TIMEOUT:
    log_warn("Vulkan fence timed out");
    return false_v;
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    log_error("Vulkan fence out of host memory");
    return false_v;
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    log_error("Vulkan fence out of device memory");
    return false_v;
  case VK_ERROR_DEVICE_LOST:
    log_error("Vulkan fence device lost");
    return false_v;
  default:
    log_error("Failed to wait for Vulkan fence");
    return false_v;
  }

  return false_v;
}

void vulkan_fence_reset(VulkanBackendState *state, VulkanFence *fence) {
  assert_log(state != NULL, "Vulkan backend state is NULL");
  assert_log(fence != NULL, "Vulkan fence is NULL");

  if (fence->is_signaled) {
    vkResetFences(state->device, 1, &fence->handle);
    fence->is_signaled = false_v;
  }
}