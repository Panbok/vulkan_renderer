#pragma once

#include "../vulkan_types.h"

const char **vulkan_platform_get_required_extensions(uint32_t *out_count);

/**
 * @brief True when a required instance extension exists only to create a
 * surface.
 *
 * An offscreen target never creates a `VkSurfaceKHR`, so it drops these rather
 * than depending on a driver exposing WSI at all. Keeping the classification
 * beside the platform's extension list means a new platform declares both in
 * one file.
 */
bool8_t vulkan_platform_extension_is_surface(const char *extension_name);

const char **
vulkan_platform_get_required_device_extensions(uint32_t *out_count);

bool8_t vulkan_platform_create_surface(VulkanBackendState *state);

void vulkan_platform_destroy_surface(VulkanBackendState *state);
