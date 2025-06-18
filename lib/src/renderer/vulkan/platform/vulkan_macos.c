#include "vulkan_platform.h"

#if defined(PLATFORM_APPLE)

const char **vulkan_platform_get_required_extensions(uint32_t *out_count) {
  static const char *extensions[] = {
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_EXT_METAL_SURFACE_EXTENSION_NAME,
#ifndef NDEBUG
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
      VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
#endif
  };
  *out_count = ArrayCount(extensions);
  return extensions;
}

const char **
vulkan_platform_get_required_device_extensions(uint32_t *out_count) {
  static const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  *out_count = ArrayCount(extensions);
  return extensions;
}

bool8_t vulkan_platform_create_surface(VulkanBackendState *state) {
  assert_log(state != NULL, "State is not set");
  assert_log(state->window != NULL, "Window is not set");

  void *metal_layer = window_get_metal_layer(state->window);
  if (metal_layer == NULL) {
    log_fatal("Failed to get Metal layer from window");
    return false;
  }

  log_debug("Creating Vulkan Metal surface with layer: %p", metal_layer);

  VkMetalSurfaceCreateInfoEXT create_info = {
      .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
      .pNext = NULL,
      .flags = 0,
      .pLayer = metal_layer,
  };

  VkResult result = vkCreateMetalSurfaceEXT(state->instance, &create_info, NULL,
                                            &state->surface);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to create Metal surface");
    return false;
  }

  log_debug("Vulkan Metal surface created successfully with handle: %p",
            state->surface);

  return true;
}

void vulkan_platform_destroy_surface(VulkanBackendState *state) {
  assert_log(state != NULL, "State is not set");
  assert_log(state->surface != VK_NULL_HANDLE, "Surface is not set");

  log_debug("Destroying Vulkan surface");
  vkDestroySurfaceKHR(state->instance, state->surface, NULL);
}
#endif