#include "vulkan_device.h"
#include "containers/bitset.h"
#include "defines.h"
#include "renderer/vulkan/vulkan_types.h"
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
      if (string_equals(required_extensions[i], array_get_VkExtensionProperties(
                                                    &available_extensions, j)
                                                    ->extensionName)) {
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
  QueueFamilyIndex *transfer_index =
      array_get_QueueFamilyIndex(&indices, QUEUE_FAMILY_TYPE_TRANSFER);
  const bool32_t has_required_queues = graphics_index->is_present &&
                                       present_index->is_present &&
                                       transfer_index->is_present;

  if (!has_required_queues) {
    array_destroy_QueueFamilyIndex(&indices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    return 0;
  }

  VulkanSwapchainDetails swapchain_details = {
      .formats = NULL,
      .present_modes = NULL,
      .capabilities = (VkSurfaceCapabilitiesKHR){0}};
  vulkan_device_query_swapchain_details(state, device, &swapchain_details);

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

  DeviceRequirements *req = state->device_requirements;

  if (bitset8_is_set(&req->supported_stages, SHADER_STAGE_GEOMETRY_BIT) &&
      !deviceFeatures.geometryShader) {
    array_destroy_VkSurfaceFormatKHR(&swapchain_details.formats);
    array_destroy_VkPresentModeKHR(&swapchain_details.present_modes);
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

VkSurfaceFormatKHR *vulkan_device_choose_swap_surface_format(
    VulkanSwapchainDetails *swapchain_details) {
  for (uint32_t i = 0; i < swapchain_details->formats.length; i++) {
    if (array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i)->format ==
            VK_FORMAT_B8G8R8A8_SRGB &&
        array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i)
                ->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return array_get_VkSurfaceFormatKHR(&swapchain_details->formats, i);
    }
  }

  return array_get_VkSurfaceFormatKHR(&swapchain_details->formats, 0);
}

VkPresentModeKHR vulkan_device_choose_swap_present_mode(
    VulkanSwapchainDetails *swapchain_details) {
  for (uint32_t i = 0; i < swapchain_details->present_modes.length; i++) {
    VkPresentModeKHR present_mode =
        *array_get_VkPresentModeKHR(&swapchain_details->present_modes, i);
    // this present mode enables triple buffering
    if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return present_mode;
    }
  }

  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D
vulkan_device_choose_swap_extent(VulkanBackendState *state,
                                 VulkanSwapchainDetails *swapchain_details) {
  if (swapchain_details->capabilities.currentExtent.width != UINT32_MAX) {
    return swapchain_details->capabilities.currentExtent;
  }

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(state->window);
  VkExtent2D current_extent = {
      .width = window_size.width,
      .height = window_size.height,
  };

  current_extent.width =
      Clamp(current_extent.width,
            swapchain_details->capabilities.minImageExtent.width,
            swapchain_details->capabilities.maxImageExtent.width);
  current_extent.height =
      Clamp(current_extent.height,
            swapchain_details->capabilities.minImageExtent.height,
            swapchain_details->capabilities.maxImageExtent.height);

  return current_extent;
}

void vulkan_device_query_swapchain_details(VulkanBackendState *state,
                                           VkPhysicalDevice device,
                                           VulkanSwapchainDetails *details) {
  assert_log(state != NULL, "State is NULL");
  assert_log(device != VK_NULL_HANDLE, "Device is NULL");
  assert_log(state->surface != VK_NULL_HANDLE, "Surface was not acquired");
  assert_log(details != NULL, "Details is NULL");

  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, state->surface,
                                            &details->capabilities);

  uint32_t format_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, state->surface, &format_count,
                                       NULL);
  if (format_count != 0) {
    details->formats =
        array_create_VkSurfaceFormatKHR(state->swapchain_arena, format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, state->surface, &format_count,
                                         details->formats.data);
  }

  uint32_t present_mode_count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, state->surface,
                                            &present_mode_count, NULL);
  if (present_mode_count != 0) {
    details->present_modes = array_create_VkPresentModeKHR(
        state->swapchain_arena, present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, state->surface,
                                              &present_mode_count,
                                              details->present_modes.data);
  }
}

bool32_t vulkan_device_check_depth_format(VulkanDevice *device) {
  assert_log(device != NULL, "Device is NULL");
  assert_log(device->physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");

  // Format candidates
  const uint32_t candidate_count = 3;
  VkFormat candidates[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                            VK_FORMAT_D24_UNORM_S8_UINT};

  uint32_t flags = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  for (uint32_t i = 0; i < candidate_count; ++i) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(device->physical_device, candidates[i],
                                        &properties);

    if ((properties.linearTilingFeatures & flags) == flags) {
      device->depth_format = candidates[i];
      return true;
    } else if ((properties.optimalTilingFeatures & flags) == flags) {
      device->depth_format = candidates[i];
      return true;
    }
  }

  return false;
}

void vulkan_device_query_queue_indices(VulkanBackendState *state,
                                       Array_QueueFamilyIndex *indices) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->device.physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");

  *indices = find_queue_family_indices(state, state->device.physical_device);

  QueueFamilyIndex *graphics_index =
      array_get_QueueFamilyIndex(indices, QUEUE_FAMILY_TYPE_GRAPHICS);
  QueueFamilyIndex *present_index =
      array_get_QueueFamilyIndex(indices, QUEUE_FAMILY_TYPE_PRESENT);
  QueueFamilyIndex *transfer_index =
      array_get_QueueFamilyIndex(indices, QUEUE_FAMILY_TYPE_TRANSFER);

  state->device.graphics_queue_index = graphics_index->index;
  state->device.present_queue_index = present_index->index;
  state->device.transfer_queue_index = transfer_index->index;
}

bool32_t vulkan_device_pick_physical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->instance != NULL, "Instance is NULL");
  assert_log(state->device.physical_device == NULL,
             "Physical device already created");

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

  state->device.physical_device =
      pick_suitable_device(state, &physical_devices);
  if (state->device.physical_device == VK_NULL_HANDLE) {
    array_destroy_VkPhysicalDevice(&physical_devices);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("No suitable Vulkan physical device found");
    return false;
  }

  array_destroy_VkPhysicalDevice(&physical_devices);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  log_debug("Physical device acquired with handle: %p",
            state->device.physical_device);

  return true;
}

void vulkan_device_get_information(VulkanBackendState *state,
                                   DeviceInformation *device_information,
                                   Arena *temp_arena) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->device.physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(state->device.physical_device, &properties);

  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(state->device.physical_device, &features);

  VkPhysicalDeviceMemoryProperties memory_properties;
  vkGetPhysicalDeviceMemoryProperties(state->device.physical_device,
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
      find_queue_family_indices(state, state->device.physical_device);
  DeviceQueueFlags device_queues = bitset8_create();

  uint32_t queue_family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(state->device.physical_device,
                                           &queue_family_count, NULL);

  Array_VkQueueFamilyProperties queue_families =
      array_create_VkQueueFamilyProperties(temp_arena, queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(
      state->device.physical_device, &queue_family_count, queue_families.data);

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
  assert_log(state->device.physical_device != VK_NULL_HANDLE,
             "Physical device was not acquired");

  log_debug("Unbinding physical device");

  state->device.physical_device = VK_NULL_HANDLE;
}

bool32_t vulkan_device_create_logical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  Scratch scratch = scratch_create(state->temp_arena);
  Array_QueueFamilyIndex indices = {0};
  vulkan_device_query_queue_indices(state, &indices);

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

  uint32_t ext_count = 0;
  const char **extension_names =
      vulkan_platform_get_required_device_extensions(&ext_count);

  VkDeviceCreateInfo device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = queue_create_infos.length,
      .pQueueCreateInfos = queue_create_infos.data,
      .pEnabledFeatures = &device_features,
      .enabledExtensionCount = ext_count,
      .ppEnabledExtensionNames = extension_names,
#ifndef NDEBUG
      .enabledLayerCount = ArrayCount(VALIDATION_LAYERS),
      .ppEnabledLayerNames = VALIDATION_LAYERS,
#endif
  };

  VkDevice device;
  if (vkCreateDevice(state->device.physical_device, &device_create_info,
                     state->allocator, &device) != VK_SUCCESS) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);
    log_fatal("Failed to create logical device");
    return false;
  }

  state->device.logical_device = device;

  log_debug("Logical device created with handle: %p", state->device);

  if (vkCreateCommandPool(state->device.logical_device, &pool_info,
                          state->allocator,
                          &state->device.graphics_command_pool) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan command pool");
    return false_v;
  }

  log_debug("Created Vulkan command pool: %p",
            state->device.graphics_command_pool);

  vkGetDeviceQueue(state->device.logical_device,
                   array_get_VkDeviceQueueCreateInfo(&queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_GRAPHICS)
                       ->queueFamilyIndex,
                   0, &state->device.graphics_queue);
  vkGetDeviceQueue(state->device.logical_device,
                   array_get_VkDeviceQueueCreateInfo(&queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_PRESENT)
                       ->queueFamilyIndex,
                   0, &state->device.present_queue);
  vkGetDeviceQueue(state->device.logical_device,
                   array_get_VkDeviceQueueCreateInfo(&queue_create_infos,
                                                     QUEUE_FAMILY_TYPE_TRANSFER)
                       ->queueFamilyIndex,
                   0, &state->device.transfer_queue);

  array_destroy_VkDeviceQueueCreateInfo(&queue_create_infos);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_RENDERER);

  log_debug("Graphics queue: %p", state->device.graphics_queue);
  log_debug("Present queue: %p", state->device.present_queue);

  return true;
}

void vulkan_device_destroy_logical_device(VulkanBackendState *state) {
  assert_log(state != NULL, "State is NULL");

  log_debug("Destroying logical device and command pool");

  vkDestroyCommandPool(state->device.logical_device,
                       state->device.graphics_command_pool, state->allocator);
  state->device.graphics_command_pool = VK_NULL_HANDLE;

  vkDestroyDevice(state->device.logical_device, state->allocator);
  state->device.logical_device = VK_NULL_HANDLE;

  state->device.depth_format = VK_FORMAT_UNDEFINED;

  state->device.graphics_queue_index = -1;
  state->device.present_queue_index = -1;
  state->device.transfer_queue_index = -1;

  state->device.graphics_queue = VK_NULL_HANDLE;
  state->device.present_queue = VK_NULL_HANDLE;
  state->device.transfer_queue = VK_NULL_HANDLE;
}