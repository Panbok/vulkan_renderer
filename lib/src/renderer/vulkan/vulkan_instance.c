#include "vulkan_instance.h"
#include "defines.h"

#ifndef NDEBUG
static bool32_t check_validation_layer_support(VulkanBackendState *state,
                                               const char **layer_names,
                                               uint32_t layer_count) {
  uint32_t available_layer_count = 0;
  vkEnumerateInstanceLayerProperties(&available_layer_count, NULL);

  Array_VkLayerProperties layer_properties =
      array_create_VkLayerProperties(state->temp_arena, available_layer_count);
  vkEnumerateInstanceLayerProperties(&available_layer_count,
                                     layer_properties.data);

  for (uint32_t i = 0; i < layer_count; i++) {
    bool32_t layer_found = false;
    for (uint32_t j = 0; j < available_layer_count; j++) {
      const char *vk_layer_name =
          (const char *)array_get_VkLayerProperties(&layer_properties, j)
              ->layerName;
      if (strcmp(layer_names[i], vk_layer_name) == 0) {
        layer_found = true;
        break;
      }
    }

    if (!layer_found) {
      array_destroy_VkLayerProperties(&layer_properties);
      return false;
    }
  }

  array_destroy_VkLayerProperties(&layer_properties);
  return true;
}
#endif

bool32_t vulkan_instance_create(VulkanBackendState *state, VkrWindow *window) {
  assert_log(state != NULL, "State is NULL");
  assert_log(window != NULL, "Window is NULL");

  VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = window->title,
      .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
      .pEngineName = "Vulkan Renderer",
      .engineVersion = VK_MAKE_VERSION(1, 0, 0),
      .apiVersion = VK_API_VERSION_1_2,
  };

  VkInstanceCreateInfo create_info = {0};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;

  uint32_t extension_count = 0;
  const char **extension_names =
      vulkan_platform_get_required_extensions(&extension_count);

#ifndef NDEBUG
  if (!check_validation_layer_support(state, VALIDATION_LAYERS,
                                      ArrayCount(VALIDATION_LAYERS))) {
    log_fatal("Validation layers not supported");
    return false;
  }
#endif

  log_debug("Validation layers supported");

  create_info.enabledExtensionCount = extension_count;
  create_info.ppEnabledExtensionNames = extension_names;
#ifndef NDEBUG
  create_info.enabledLayerCount = ArrayCount(VALIDATION_LAYERS);
  create_info.ppEnabledLayerNames = VALIDATION_LAYERS;
#endif
  create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

  VkResult result =
      vkCreateInstance(&create_info, state->allocator, &state->instance);
  if (result != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan instance");
    return false;
  }

  log_debug("Vulkan instance created with handle: %p", state->instance);

  return true;
}

void vulkan_instance_destroy(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  log_debug("Destroying Vulkan instance");

  vkDestroyInstance(state->instance, state->allocator);
  state->instance = VK_NULL_HANDLE;
}