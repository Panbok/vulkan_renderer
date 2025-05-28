#pragma once

#include "vulkan_types.h"

#include "platform/vulkan_platform.h"

bool32_t vulkan_instance_create(VulkanBackendState *state, Window *window);

void vulkan_instance_destroy(VulkanBackendState *state);