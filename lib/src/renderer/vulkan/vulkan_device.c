#include "vulkan_device.h"
#include "vulkan_utils.h"

static bool32_t has_required_extensions(VulkanBackendState *state,
                                        VkPhysicalDevice device) {
  uint32_t extension_count = 0;
  vkEnumerateDeviceExtensionProperties(device, NULL, &extension_count, NULL);

  Scratch scratch = scratch_create(state->temp_arena);
  Array_VkExtensionProperties available_extensions =
      array_create_VkExtensionProperties(scratch.arena, extension_count);
  vkEnumerateDeviceExtensionProperties(device, NULL, &extension_count,
                                       available_extensions.data);

  uint32_t required_extension_count = 0;
  const char **required_extensions =
      vulkan_platform_get_required_device_extensions(&required_extension_count);

  for (uint32_t i = 0; i < required_extension_count; i++) {
    bool32_t extension_found = false;
    for (uint32_t j = 0; j < extension_count; j++) {
      if (strcmp(required_extensions[i],
                 array_get_VkExtensionProperties(&available_extensions, j)
                     ->extensionName) == 0) {
        extension_found = true;
        break;
      }
    }

    if (!extension_found) {
      array_destroy_VkExtensionProperties(&available_extensions);
      scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
      return false;
    }
  }

  array_destroy_VkExtensionProperties(&available_extensions);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  return true;
}

// todo: we need to check if the device supports the required extensions
// todo: pick up the best device based on the scoring algorithm
static bool32_t is_device_suitable(VulkanBackendState *state,
                                   VkPhysicalDevice device) {
  assert_log(device != NULL, "Device is NULL");

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(device, &properties);

  VkPhysicalDeviceFeatures deviceFeatures;
  vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

  Scratch scratch = scratch_create(state->temp_arena);
  Array_QueueFamilyIndex indices = find_queue_family_indices(state, device);

  QueueFamilyIndex *graphics_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);
  QueueFamilyIndex *present_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);
  const bool32_t has_required_queues =
      graphics_index->is_present && present_index->is_present;

  array_destroy_QueueFamilyIndex(&indices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  if (!has_required_extensions(state, device)) {
    return false;
  }

  VulkanSwapchainDetails swapchain_details = {
      .formats = NULL,
      .present_modes = NULL,
      .capabilities = (VkSurfaceCapabilitiesKHR){0}};
  vulkan_swapchain_query_details(state, device, &swapchain_details);

  if (array_is_null_VkSurfaceFormatKHR(&swapchain_details.formats) ||
      array_is_null_VkPresentModeKHR(&swapchain_details.present_modes)) {
    return false;
  }

  return properties.apiVersion >= VK_API_VERSION_1_2 &&
         (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
          properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) &&
         deviceFeatures.tessellationShader && has_required_queues;
}

bool32_t vulkan_device_pick_physical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->instance != NULL, "Instance is NULL");
  assert_log(state->physical_device == NULL, "Physical device already created");

  uint32_t device_count = 0;
  vkEnumeratePhysicalDevices(state->instance, &device_count, NULL);
  if (device_count == 0) {
    log_fatal("No Vulkan physical devices found");
    return false;
  }

  Scratch scratch = scratch_create(state->temp_arena);
  Array_VkPhysicalDevice physical_devices =
      array_create_VkPhysicalDevice(scratch.arena, device_count);
  vkEnumeratePhysicalDevices(state->instance, &device_count,
                             physical_devices.data);

  for (uint32_t i = 0; i < device_count; i++) {
    VkPhysicalDevice device = *array_get_VkPhysicalDevice(&physical_devices, i);
    if (is_device_suitable(state, device)) {
      state->physical_device = device;
      break;
    }
  }

  if (state->physical_device == VK_NULL_HANDLE) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("No suitable Vulkan physical device found");
    return false;
  }

  array_destroy_VkPhysicalDevice(&physical_devices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  log_debug("Physical device acquired with handle: %p", state->physical_device);

  return true;
}

void vulkan_device_release_physical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");

  log_debug("Unbinding physical device");

  state->physical_device = VK_NULL_HANDLE;
}

bool32_t vulkan_device_create_logical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  Scratch scratch = scratch_create(state->temp_arena);
  Array_QueueFamilyIndex indices =
      find_queue_family_indices(state, state->physical_device);

  Array_VkDeviceQueueCreateInfo queue_create_infos =
      array_create_VkDeviceQueueCreateInfo(scratch.arena, indices.length);

  static const float32_t queue_priority = 1.0f;
  for (uint32_t i = 0; i < indices.length; i++) {
    QueueFamilyIndex *index = array_get_QueueFamilyIndex(&indices, i);
    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = index->index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    array_set_VkDeviceQueueCreateInfo(&queue_create_infos, i,
                                      queue_create_info);
  }

  QueueFamilyIndex *graphics_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);

  VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = graphics_index->index,
  };

  array_destroy_QueueFamilyIndex(&indices);

  VkPhysicalDeviceFeatures device_features = {
      .tessellationShader = VK_TRUE,
  };

  const char **layer_names = (const char **)arena_alloc(
      scratch.arena, state->validation_layers.length * sizeof(char *),
      ARENA_MEMORY_TAG_RENDERER);
  for (uint32_t i = 0; i < state->validation_layers.length; i++) {
    layer_names[i] = (const char *)string8_cstr(
        array_get_String8(&state->validation_layers, i));
  }

  const char *extension_names[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      "VK_KHR_portability_subset",
  };
  uint32_t extension_count = ArrayCount(extension_names);

  VkDeviceCreateInfo device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = queue_create_infos.length,
      .pQueueCreateInfos = queue_create_infos.data,
      .pEnabledFeatures = &device_features,
      .enabledExtensionCount = extension_count,
      .ppEnabledExtensionNames = extension_names,
      .enabledLayerCount = state->validation_layers.length,
      .ppEnabledLayerNames = layer_names,
  };

  VkDevice device;
  if (vkCreateDevice(state->physical_device, &device_create_info,
                     state->allocator, &device) != VK_SUCCESS) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("Failed to create logical device");
    return false;
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  state->device = device;

  log_debug("Logical device created with handle: %p", state->device);

  if (vkCreateCommandPool(state->device, &pool_info, state->allocator,
                          &state->command_pool) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan command pool");
    return false_v;
  }

  log_debug("Created Vulkan command pool: %p", state->command_pool);

  vkGetDeviceQueue(state->device,
                   array_get_VkDeviceQueueCreateInfo(&queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_GRAPHICS)
                       ->queueFamilyIndex,
                   0, &state->graphics_queue);
  vkGetDeviceQueue(state->device,
                   array_get_VkDeviceQueueCreateInfo(&queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_PRESENT)
                       ->queueFamilyIndex,
                   0, &state->present_queue);

  array_destroy_VkDeviceQueueCreateInfo(&queue_create_infos);

  log_debug("Graphics queue: %p", state->graphics_queue);
  log_debug("Present queue: %p", state->present_queue);

  return true;
}

void vulkan_device_destroy_logical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  log_debug("Destroying logical device and command pool");

  vkDestroyCommandPool(state->device, state->command_pool, state->allocator);
  state->command_pool = VK_NULL_HANDLE;

  vkDestroyDevice(state->device, state->allocator);
  state->device = VK_NULL_HANDLE;
}