#pragma once

#include "vulkan_types.h"

bool32_t vulkan_swapchain_create(VulkanBackendState *state);

void vulkan_swapchain_destroy(VulkanBackendState *state);