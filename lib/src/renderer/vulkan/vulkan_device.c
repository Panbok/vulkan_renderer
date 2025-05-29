#include "vulkan_device.h"
#include "defines.h"

typedef enum QueueFamilyType : uint32_t {
  QUEUE_FAMILY_TYPE_GRAPHICS,
  QUEUE_FAMILY_TYPE_PRESENT,
  QUEUE_FAMILY_TYPE_COUNT,
} QueueFamilyType;

typedef struct QueueFamilyIndex {
  uint32_t index;
  QueueFamilyType type;
  bool32_t is_present;
} QueueFamilyIndex;

Array(QueueFamilyIndex);

static Array_QueueFamilyIndex find_queue_family_index(VulkanBackendState *state,
                                                      VkPhysicalDevice device) {
  Array_QueueFamilyIndex indices =
      array_create_QueueFamilyIndex(state->temp_arena, QUEUE_FAMILY_TYPE_COUNT);
  for (uint32_t i = 0; i < QUEUE_FAMILY_TYPE_COUNT; i++) {
    QueueFamilyIndex invalid_index = {
        .index = 0, .type = i, .is_present = false};
    array_set_QueueFamilyIndex(&indices, i, invalid_index);
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);
  if (queue_family_count == 0) {
    return indices;
  }

  Scratch scratch = scratch_create(state->temp_arena);
  Array_VkQueueFamilyProperties queue_family_properties =
      array_create_VkQueueFamilyProperties(state->temp_arena,
                                           queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           queue_family_properties.data);

  for (uint32_t i = 0; i < queue_family_count; i++) {
    QueueFamilyIndex *graphics_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);
    QueueFamilyIndex *present_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);

    if (graphics_index->is_present && present_index->is_present) {
      break;
    }

    VkQueueFamilyProperties properties =
        *array_get_VkQueueFamilyProperties(&queue_family_properties, i);

    if (properties.queueFlags & VK_QUEUE_GRAPHICS_BIT &&
        !graphics_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_GRAPHICS, .is_present = true};
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS, index);
      continue;
    }

    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, state->surface,
                                         &presentSupport);
    if (presentSupport && !present_index->is_present) {
      QueueFamilyIndex index = {
          .index = i, .type = QUEUE_FAMILY_TYPE_PRESENT, .is_present = true};
      array_set_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT, index);
    }
  }

  array_destroy_VkQueueFamilyProperties(&queue_family_properties);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  return indices;
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
  Array_QueueFamilyIndex indices = find_queue_family_index(state, device);

  QueueFamilyIndex *graphics_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);
  QueueFamilyIndex *present_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);

  bool32_t has_required_queues =
      graphics_index->is_present && present_index->is_present;

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

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
      find_queue_family_index(state, state->physical_device);

  state->queue_create_infos =
      array_create_VkDeviceQueueCreateInfo(state->arena, indices.length);

  static const float32_t queue_priority = 1.0f;
  for (uint32_t i = 0; i < indices.length; i++) {
    QueueFamilyIndex *index = array_get_QueueFamilyIndex(&indices, i);
    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = index->index,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    array_set_VkDeviceQueueCreateInfo(&state->queue_create_infos, i,
                                      queue_create_info);
  }

  array_destroy_QueueFamilyIndex(&indices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  VkPhysicalDeviceFeatures device_features = {
      .tessellationShader = VK_TRUE,
  };

  scratch = scratch_create(state->temp_arena);
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
      .queueCreateInfoCount = state->queue_create_infos.length,
      .pQueueCreateInfos = state->queue_create_infos.data,
      .pEnabledFeatures = &device_features,
      .enabledExtensionCount = extension_count,
      .ppEnabledExtensionNames = extension_names,
      .enabledLayerCount = state->validation_layers.length,
      .ppEnabledLayerNames = layer_names,
  };

  VkDevice device;
  if (vkCreateDevice(state->physical_device, &device_create_info, NULL,
                     &device) != VK_SUCCESS) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("Failed to create logical device");
    return false;
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  state->device = device;

  log_debug("Logical device created with handle: %p", state->device);

  vkGetDeviceQueue(state->device,
                   array_get_VkDeviceQueueCreateInfo(&state->queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_GRAPHICS)
                       ->queueFamilyIndex,
                   0, &state->graphics_queue);
  vkGetDeviceQueue(state->device,
                   array_get_VkDeviceQueueCreateInfo(&state->queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_PRESENT)
                       ->queueFamilyIndex,
                   0, &state->present_queue);

  log_debug("Graphics queue: %p", state->graphics_queue);
  log_debug("Present queue: %p", state->present_queue);

  return true;
}

void vulkan_device_destroy_logical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  log_debug("Destroying logical device");

  array_destroy_VkDeviceQueueCreateInfo(&state->queue_create_infos);
  vkDestroyDevice(state->device, NULL);
  state->device = VK_NULL_HANDLE;
}