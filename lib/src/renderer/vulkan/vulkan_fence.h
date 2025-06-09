#pragma once

#include "defines.h"
#include "vulkan_types.h"

void vulkan_fence_create(VulkanBackendState *state, bool8_t is_signaled,
                         VulkanFence *out_fence);

void vulkan_fence_destroy(VulkanBackendState *state, VulkanFence *fence);

bool8_t vulkan_fence_wait(VulkanBackendState *state, VulkanFence *fence);

void vulkan_fence_reset(VulkanBackendState *state, VulkanFence *fence);