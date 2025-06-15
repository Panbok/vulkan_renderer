#include "vulkan_device.h"
#include "containers/bitset.h"
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

static uint32_t score_device(VulkanBackendState *state,
                             VkPhysicalDevice device) {
  assert_log(device != NULL, "Device is NULL");

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(device, &properties);

  VkPhysicalDeviceFeatures deviceFeatures;
  vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

  uint32_t score = 0;

  if (!has_required_extensions(state, device)) {
    return 0;
  }

  Scratch scratch = scratch_create(state->temp_arena);
  Array_QueueFamilyIndex indices = find_queue_family_indices(state, device);

  QueueFamilyIndex *graphics_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_GRAPHICS);
  QueueFamilyIndex *present_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);
  const bool32_t has_required_queues =
      graphics_index->is_present && present_index->is_present;

  if (!has_required_queues) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  VulkanSwapchainDetails swapchain_details = {
      .formats = NULL,
      .present_modes = NULL,
      .capabilities = (VkSurfaceCapabilitiesKHR){0}};
  vulkan_swapchain_query_details(state, device, &swapchain_details);

  if (array_is_null_VkSurfaceFormatKHR(&swapchain_details.formats) ||
      array_is_null_VkPresentModeKHR(&swapchain_details.present_modes)) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  if (properties.apiVersion < VK_API_VERSION_1_2) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  if (!deviceFeatures.tessellationShader) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  DeviceRequirements *req = state->device_requirements;

  if (bitset8_is_set(&req->supported_stages, SHADER_STAGE_GEOMETRY_BIT) &&
      !deviceFeatures.geometryShader) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  if (bitset8_is_set(&req->supported_stages,
                     SHADER_STAGE_TESSELLATION_CONTROL_BIT) &&
      !deviceFeatures.tessellationShader) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  if (bitset8_is_set(&req->supported_stages,
                     SHADER_STAGE_TESSELLATION_EVALUATION_BIT) &&
      !deviceFeatures.tessellationShader) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);

  Array_VkQueueFamilyProperties queue_families =
      array_create_VkQueueFamilyProperties(scratch.arena, queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count,
                                           queue_families.data);

  bool32_t has_required_graphics =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_GRAPHICS_BIT);
  bool32_t has_required_compute =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_COMPUTE_BIT);
  bool32_t has_required_transfer =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_TRANSFER_BIT);
  bool32_t has_required_sparse =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_SPARSE_BINDING_BIT);
  bool32_t has_required_protected =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_PROTECTED_BIT);
  bool32_t has_required_present =
      !bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_PRESENT_BIT);

  for (uint32_t i = 0; i < queue_family_count; i++) {
    VkQueueFamilyProperties *family =
        array_get_VkQueueFamilyProperties(&queue_families, i);

    if (family->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      has_required_graphics = true;
    }
    if (family->queueFlags & VK_QUEUE_COMPUTE_BIT) {
      has_required_compute = true;
    }
    if (family->queueFlags & VK_QUEUE_TRANSFER_BIT) {
      has_required_transfer = true;
    }
    if (family->queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) {
      has_required_sparse = true;
    }
    if (family->queueFlags & VK_QUEUE_PROTECTED_BIT) {
      has_required_protected = true;
    }
  }

  // Check present support separately (surface-dependent)
  if (bitset8_is_set(&req->supported_queues, DEVICE_QUEUE_PRESENT_BIT)) {
    QueueFamilyIndex *present_index =
        array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_PRESENT);
    has_required_present = present_index->is_present;
  }

  array_destroy_VkQueueFamilyProperties(&queue_families);

  if (!has_required_graphics || !has_required_compute ||
      !has_required_transfer || !has_required_sparse ||
      !has_required_protected || !has_required_present) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  if (bitset8_is_set(&req->supported_sampler_filters,
                     SAMPLER_FILTER_ANISOTROPIC_BIT) &&
      !deviceFeatures.samplerAnisotropy) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  // Note: Linear filtering is universally supported in Vulkan, so no need to
  // check

  // Start with base score for meeting minimum requirements
  score = 100;
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
      bitset8_is_set(&req->allowed_device_types, DEVICE_TYPE_DISCRETE_BIT)) {
    score += 1000; // Discrete GPU gets highest preference
  } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
             bitset8_is_set(&req->allowed_device_types,
                            DEVICE_TYPE_INTEGRATED_BIT)) {
    score += 500; // Integrated GPU gets medium preference
  } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU &&
             bitset8_is_set(&req->allowed_device_types,
                            DEVICE_TYPE_VIRTUAL_BIT)) {
    score += 200; // Virtual GPU gets lower preference
  } else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU &&
             bitset8_is_set(&req->allowed_device_types, DEVICE_TYPE_CPU_BIT)) {
    score += 50; // CPU gets lowest preference
  } else {
    // Device type not in allowed types, but still give some score if it meets
    // basic requirements
    score += 10;
  }

  // Bonus for more VRAM (for discrete/integrated GPUs)
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
      properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(device, &memProperties);

    uint64_t vram_size = 0;
    for (uint32_t i = 0; i < memProperties.memoryHeapCount; i++) {
      if (memProperties.memoryHeaps[i].flags &
          VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        vram_size = memProperties.memoryHeaps[i].size;
        break;
      }
    }

    // Add points based on VRAM size (1 point per GB, capped at 32 points)
    score += (uint32_t)((vram_size / (1024 * 1024 * 1024)) > 32
                            ? 32
                            : (vram_size / (1024 * 1024 * 1024)));
  }

  // Bonus points for additional features beyond requirements
  if (deviceFeatures.geometryShader) {
    score += 25;
  }

  if (deviceFeatures.samplerAnisotropy) {
    score += 25;
  }

  if (deviceFeatures.wideLines) {
    score += 10;
  }

  if (deviceFeatures.largePoints) {
    score += 10;
  }

  // Bonus for having multiple queue families of the same type (better
  // parallelism)
  uint32_t graphics_queue_count = 0;
  uint32_t compute_queue_count = 0;

  uint32_t bonus_queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &bonus_queue_family_count,
                                           NULL);

  Array_VkQueueFamilyProperties bonus_queue_families =
      array_create_VkQueueFamilyProperties(scratch.arena,
                                           bonus_queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &bonus_queue_family_count,
                                           bonus_queue_families.data);

  for (uint32_t i = 0; i < bonus_queue_family_count; i++) {
    VkQueueFamilyProperties *family =
        array_get_VkQueueFamilyProperties(&bonus_queue_families, i);
    if (family->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      graphics_queue_count += family->queueCount;
    }
    if (family->queueFlags & VK_QUEUE_COMPUTE_BIT) {
      compute_queue_count += family->queueCount;
    }
  }

  array_destroy_VkQueueFamilyProperties(&bonus_queue_families);

  // Bonus for multiple queues (better parallelism)
  if (graphics_queue_count > 1) {
    score += 15;
  }
  if (compute_queue_count > 1) {
    score += 15;
  }

  array_destroy_QueueFamilyIndex(&indices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  return score;
}

static VkPhysicalDevice pick_suitable_device(VulkanBackendState *state,
                                             Array_VkPhysicalDevice *devices) {
  assert_log(state != NULL, "State is NULL");
  assert_log(devices != NULL, "Devices array is NULL");
  assert_log(devices->length > 0, "No devices provided");

  VkPhysicalDevice best_device = VK_NULL_HANDLE;
  uint32_t best_score = 0;
  for (uint32_t i = 0; i < devices->length; i++) {
    VkPhysicalDevice device = *array_get_VkPhysicalDevice(devices, i);
    uint32_t score = score_device(state, device);

    if (score > best_score) {
      best_score = score;
      best_device = device;
    }

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device, &properties);
    log_debug("Device '%s' scored %u points", properties.deviceName, score);
  }

  if (best_device != VK_NULL_HANDLE) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(best_device, &properties);
    log_debug("Selected device '%s' with score %u", properties.deviceName,
              best_score);
  }

  return best_device;
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

  state->physical_device = pick_suitable_device(state, &physical_devices);
  if (state->physical_device == VK_NULL_HANDLE) {
    array_destroy_VkPhysicalDevice(&physical_devices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("No suitable Vulkan physical device found");
    return false;
  }

  array_destroy_VkPhysicalDevice(&physical_devices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  log_debug("Physical device acquired with handle: %p", state->physical_device);

  return true;
}

void vulkan_device_get_information(VulkanBackendState *state,
                                   DeviceInformation *device_information,
                                   Arena *temp_arena) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(state->physical_device, &properties);

  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(state->physical_device, &features);

  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(state->physical_device,
                                      &memory_properties);

  // Device name
  String8 device_name =
      string8_create_formatted(temp_arena, "%s", properties.deviceName);

  // Vendor name based on vendor ID
  String8 vendor_name;
  switch (properties.vendorID) {
  case 0x1002:
    vendor_name = string8_lit("AMD");
    break;
  case 0x10DE:
    vendor_name = string8_lit("NVIDIA");
    break;
  case 0x8086:
    vendor_name = string8_lit("Intel");
    break;
  case 0x13B5:
    vendor_name = string8_lit("ARM");
    break;
  case 0x5143:
    vendor_name = string8_lit("Qualcomm");
    break;
  case 0x1010:
    vendor_name = string8_lit("ImgTec");
    break;
  default: {
    vendor_name = string8_create_formatted(temp_arena, "Unknown (0x%X)",
                                           properties.vendorID);
    break;
  }
  }

  // Driver version (vendor-specific formatting)
  String8 driver_version;
  if (properties.vendorID == 0x10DE) { // NVIDIA
    uint32_t major = (properties.driverVersion >> 22) & 0x3FF;
    uint32_t minor = (properties.driverVersion >> 14) & 0xFF;
    uint32_t secondary = (properties.driverVersion >> 6) & 0xFF;
    uint32_t tertiary = properties.driverVersion & 0x3F;
    driver_version = string8_create_formatted(temp_arena, "%u.%u.%u.%u", major,
                                              minor, secondary, tertiary);
  } else if (properties.vendorID == 0x8086) { // Intel
    uint32_t major = properties.driverVersion >> 14;
    uint32_t minor = properties.driverVersion & 0x3FFF;
    driver_version =
        string8_create_formatted(temp_arena, "%u.%u", major, minor);
  } else { // AMD and others - use standard Vulkan version format
    uint32_t major = VK_VERSION_MAJOR(properties.driverVersion);
    uint32_t minor = VK_VERSION_MINOR(properties.driverVersion);
    uint32_t patch = VK_VERSION_PATCH(properties.driverVersion);
    driver_version =
        string8_create_formatted(temp_arena, "%u.%u.%u", major, minor, patch);
  }

  // API version
  uint32_t api_major = VK_VERSION_MAJOR(properties.apiVersion);
  uint32_t api_minor = VK_VERSION_MINOR(properties.apiVersion);
  uint32_t api_patch = VK_VERSION_PATCH(properties.apiVersion);
  String8 api_version = string8_create_formatted(
      temp_arena, "%u.%u.%u", api_major, api_minor, api_patch);

  // Memory information
  uint64_t vram_size = 0;
  uint64_t vram_local_size = 0;
  uint64_t vram_shared_size = 0;

  for (uint32_t i = 0; i < memory_properties.memoryHeapCount; i++) {
    VkMemoryHeap heap = memory_properties.memoryHeaps[i];
    if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      vram_local_size += heap.size;
    } else {
      vram_shared_size += heap.size;
    }
    vram_size += heap.size;
  }

  // Device type flags
  DeviceTypeFlags device_types = bitset8_create();
  switch (properties.deviceType) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    bitset8_set(&device_types, DEVICE_TYPE_DISCRETE_BIT);
    break;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    bitset8_set(&device_types, DEVICE_TYPE_INTEGRATED_BIT);
    break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    bitset8_set(&device_types, DEVICE_TYPE_VIRTUAL_BIT);
    break;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    bitset8_set(&device_types, DEVICE_TYPE_CPU_BIT);
    break;
  default:
    break;
  }

  // Queue family capabilities
  Array_QueueFamilyIndex queue_indices =
      find_queue_family_indices(state, state->physical_device);
  DeviceQueueFlags device_queues = bitset8_create();

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(state->physical_device,
                                           &queue_family_count, NULL);

  Array_VkQueueFamilyProperties queue_families =
      array_create_VkQueueFamilyProperties(temp_arena, queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(
      state->physical_device, &queue_family_count, queue_families.data);

  for (uint32_t i = 0; i < queue_family_count; i++) {
    VkQueueFamilyProperties *family =
        array_get_VkQueueFamilyProperties(&queue_families, i);

    if (family->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      bitset8_set(&device_queues, DEVICE_QUEUE_GRAPHICS_BIT);
    }
    if (family->queueFlags & VK_QUEUE_COMPUTE_BIT) {
      bitset8_set(&device_queues, DEVICE_QUEUE_COMPUTE_BIT);
    }
    if (family->queueFlags & VK_QUEUE_TRANSFER_BIT) {
      bitset8_set(&device_queues, DEVICE_QUEUE_TRANSFER_BIT);
    }
    if (family->queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) {
      bitset8_set(&device_queues, DEVICE_QUEUE_SPARSE_BINDING_BIT);
    }
    if (family->queueFlags & VK_QUEUE_PROTECTED_BIT) {
      bitset8_set(&device_queues, DEVICE_QUEUE_PROTECTED_BIT);
    }
  }

  // Check for present support (this is surface-dependent)
  QueueFamilyIndex *present_index =
      array_get_QueueFamilyIndex(&queue_indices, QUEUE_FAMILY_TYPE_PRESENT);
  if (present_index->is_present) {
    bitset8_set(&device_queues, DEVICE_QUEUE_PRESENT_BIT);
  }

  // Sampler filter capabilities
  SamplerFilterFlags sampler_filters = bitset8_create();
  if (features.samplerAnisotropy) {
    bitset8_set(&sampler_filters, SAMPLER_FILTER_ANISOTROPIC_BIT);
  }
  // Linear filtering is almost universally supported
  bitset8_set(&sampler_filters, SAMPLER_FILTER_LINEAR_BIT);

  array_destroy_QueueFamilyIndex(&queue_indices);
  array_destroy_VkQueueFamilyProperties(&queue_families);

  String8 persistent_device_name = string8_create_formatted(
      state->arena, "%.*s", (int)device_name.length, device_name.str);
  String8 persistent_vendor_name = string8_create_formatted(
      state->arena, "%.*s", (int)vendor_name.length, vendor_name.str);
  String8 persistent_driver_version = string8_create_formatted(
      state->arena, "%.*s", (int)driver_version.length, driver_version.str);
  String8 persistent_api_version = string8_create_formatted(
      state->arena, "%.*s", (int)api_version.length, api_version.str);

  device_information->device_name = persistent_device_name;
  device_information->vendor_name = persistent_vendor_name;
  device_information->driver_version = persistent_driver_version;
  device_information->api_version = persistent_api_version;
  device_information->vram_size = vram_size;
  device_information->vram_local_size = vram_local_size;
  device_information->vram_shared_size = vram_shared_size;
  device_information->device_types = device_types;
  device_information->device_queues = device_queues;
  device_information->sampler_filters = sampler_filters;
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