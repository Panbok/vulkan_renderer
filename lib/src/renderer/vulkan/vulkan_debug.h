#pragma once

#ifndef NDEBUG

#include "vulkan_types.h"

bool32_t vulkan_debug_create_debug_messenger(VulkanBackendState *state);

void vulkan_debug_destroy_debug_messenger(VulkanBackendState *state);

#endif // NDEBUG