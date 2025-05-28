#pragma once

#include "../vulkan_types.h"

const char **vulkan_platform_get_required_extensions(uint32_t *out_count);

bool8_t vulkan_platform_create_surface(VulkanBackendState *state);

void vulkan_platform_destroy_surface(VulkanBackendState *state);
