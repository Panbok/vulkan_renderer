#include "vulkan_instance.h"

// TODO: should probably only use in debug builds
static bool32_t check_validation_layer_support(VulkanBackendState *state) {
  uint32_t layer_count = 0;
  vkEnumerateInstanceLayerProperties(&layer_count, NULL);

  Array_VkLayerProperties layer_properties =
      array_create_VkLayerProperties(state->arena, layer_count);
  vkEnumerateInstanceLayerProperties(&layer_count, layer_properties.data);

  for (uint32_t i = 0; i < state->validation_layer.length; i++) {
    bool32_t layer_found = false;
    for (uint32_t j = 0; j < layer_count; j++) {
      uint8_t *layer_name =
          string8_cstr(array_get_String8(&state->validation_layer, i));
      const char *vk_layer_name =
          (const char *)array_get_VkLayerProperties(&layer_properties, j)
              ->layerName;
      if (strcmp((const char *)layer_name, vk_layer_name) == 0) {
        layer_found = true;
        break;
      }
    }

    if (!layer_found) {
      return false;
    }
  }

  array_destroy_VkLayerProperties(&layer_properties);
  return true;
}

bool32_t vulkan_instance_create(VulkanBackendState *state, Window *window) {
  assert_log(state != NULL, "State is NULL");
  assert_log(window != NULL, "Window is NULL");

  VkApplicationInfo app_info = {0};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = window->title;
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "Vulkan Renderer";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_API_VERSION_1_4;

  const char *extensions[] = {
      VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
      VK_KHR_SURFACE_EXTENSION_NAME,
      "VK_EXT_metal_surface",
      // TODO: remove this in release builds
      VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
      VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
  };

  VkInstanceCreateInfo create_info = {0};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &app_info;
  create_info.enabledExtensionCount = ArrayCount(extensions);
  create_info.ppEnabledExtensionNames = extensions;

  state->validation_layer = array_create_String8(state->arena, 1);
  array_set_String8(&state->validation_layer, 0,
                    string8_lit("VK_LAYER_KHRONOS_validation"));

  if (!check_validation_layer_support(state)) {
    log_fatal("Validation layers not supported");
    return false;
  }

  Scratch scratch = scratch_create(state->arena);
  const char **layer_names = (const char **)arena_alloc(
      scratch.arena, state->validation_layer.length * sizeof(char *),
      ARENA_MEMORY_TAG_RENDERER);
  for (uint32_t i = 0; i < state->validation_layer.length; i++) {
    layer_names[i] = (const char *)string8_cstr(
        array_get_String8(&state->validation_layer, i));
  }

  create_info.enabledLayerCount = state->validation_layer.length;
  create_info.ppEnabledLayerNames = layer_names;
  create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

  VkResult result = vkCreateInstance(&create_info, NULL, &state->instance);

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  if (result != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan instance: %s", string_VkResult(result));
    return false;
  }

  log_debug("Vulkan instance created");

  uint32_t extension_count = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &extension_count, NULL);

  state->extension_properties =
      array_create_VkExtensionProperties(state->arena, extension_count);
  vkEnumerateInstanceExtensionProperties(NULL, &extension_count,
                                         state->extension_properties.data);

  log_debug("Avaliable extensions: %d", extension_count);
  for (uint32_t i = 0; i < extension_count; i++) {
    log_debug("Extension %d: %s", i,
              array_get_VkExtensionProperties(&state->extension_properties, i)
                  ->extensionName);
  }

  log_debug("Validation layers supported");

  return true;
}

void vulkan_instance_destroy(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  array_destroy_VkExtensionProperties(&state->extension_properties);
  array_destroy_String8(&state->validation_layer);
  vkDestroyInstance(state->instance, NULL);
}