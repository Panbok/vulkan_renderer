#include "vulkan_platform.h"

#if defined(PLATFORM_WINDOWS)
const char **vulkan_platform_get_required_extensions(uint32_t *out_count) {
  static const char *extensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#ifndef NDEBUG
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
  };
  *out_count = ArrayCount(extensions);
  return extensions;
}

const char **
vulkan_platform_get_required_device_extensions(uint32_t *out_count) {
  static const char *extensions[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };
  *out_count = ArrayCount(extensions);
  return extensions;
}

bool8_t vulkan_platform_create_surface(VulkanBackendState *state) {
  assert_log(state != NULL, "State is not set");
  assert_log(state->window != NULL, "Window is not set");

  void *win32_handle = vkr_window_get_win32_handle(state->window);
  if (win32_handle == NULL) {
    log_fatal("Failed to get Win32 handle from window");
    return false_v;
  }

  void *win32_instance = vkr_window_get_win32_instance(state->window);
  if (win32_instance == NULL) {
    log_fatal("Failed to get Win32 instance from window");
    return false_v;
  }

  VkWin32SurfaceCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
      .pNext = NULL,
      .flags = 0,
      .hinstance = win32_instance,
      .hwnd = win32_handle,
  };

  VkResult result = vkCreateWin32SurfaceKHR(state->instance, &create_info,
                                            state->allocator, &state->surface);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to create Win32 surface");
    return false_v;
  }

  return true_v;
}

void vulkan_platform_destroy_surface(VulkanBackendState *state) {
  assert_log(state != NULL, "State is not set");
  assert_log(state->surface != VK_NULL_HANDLE, "Surface is not set");

  vkDestroySurfaceKHR(state->instance, state->surface, state->allocator);
  state->surface = VK_NULL_HANDLE;
}
#endif