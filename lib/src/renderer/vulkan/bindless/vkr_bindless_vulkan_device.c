#include "renderer/vulkan/bindless/vkr_bindless_vulkan_device.h"

#include "core/logger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { VKR_BINDLESS_VK_MAX_EXTENSIONS = 256 };

typedef struct VkrBindlessVkFeatureSet {
  bool8_t shader_int64;
  bool8_t buffer_device_address;
  bool8_t timeline_semaphore;
  bool8_t descriptor_indexing;
  bool8_t runtime_descriptor_array;
  bool8_t sampled_image_non_uniform;
  bool8_t storage_image_non_uniform;
  bool8_t scalar_block_layout;
  bool8_t host_query_reset;
  bool8_t dynamic_rendering;
  bool8_t synchronization2;
  bool8_t maintenance4;
  bool8_t maintenance5;
  bool8_t host_image_copy;
  bool8_t descriptor_buffer;
  bool8_t descriptor_buffer_capture_replay;
  bool8_t descriptor_buffer_image_layout_ignored;
  bool8_t descriptor_buffer_push_descriptors;
} VkrBindlessVkFeatureSet;

typedef struct VkrBindlessVkCandidate {
  VkPhysicalDevice physical;
  VkPhysicalDeviceProperties2 properties;
  VkPhysicalDeviceDriverProperties driver;
  VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
  VkrBindlessVkFeatureSet features;
  uint32_t queue_family_index;
  uint32_t score;
  bool8_t common_viable;
  bool8_t window_viable;
  bool8_t has_descriptor_buffer_extension;
  bool8_t has_swapchain_extension;
  bool8_t has_swapchain_maintenance_extension;
} VkrBindlessVkCandidate;

struct VkrBindlessVulkanDevice {
  VkrAllocator *allocator;
  VkrBindlessVulkanDeviceConfig config;
  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  VkSurfaceKHR surface;
  VkDevice device;
  VkQueue queue;
  VkrBindlessVkCandidate candidates[VKR_BINDLESS_VK_MAX_CANDIDATES];
  uint32_t candidate_count;
  uint32_t selected_candidate_index;
  VkrBindlessVkCandidate *selected;
  VkrBindlessVkCapabilityProfile profile;
  VkrBindlessVulkanDescriptorLayout resource_layout;
  VkrBindlessVulkanDescriptorLayout sampler_layout;
  PFN_vkGetDescriptorSetLayoutSizeEXT get_layout_size;
  PFN_vkGetDescriptorSetLayoutBindingOffsetEXT get_binding_offset;
  PFN_vkGetDescriptorEXT get_descriptor;
  PFN_vkCmdBindDescriptorBuffersEXT cmd_bind_descriptor_buffers;
  PFN_vkCmdSetDescriptorBufferOffsetsEXT cmd_set_descriptor_offsets;
  uint32_t validation_setup_notice_count;
  uint32_t validation_warning_count;
  uint32_t validation_error_count;
  bool8_t gpu_assisted_unavailable;
  bool8_t ready;
};

static bool8_t
vkr_bindless_vk_extension_present(const VkExtensionProperties *extensions,
                                  uint32_t count, const char *name) {
  for (uint32_t i = 0; i < count; ++i) {
    if (strcmp(extensions[i].extensionName, name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t vkr_bindless_vk_layer_present(const char *name) {
  uint32_t count = 0;
  if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS ||
      count == 0 || count > VKR_BINDLESS_VK_MAX_EXTENSIONS) {
    return false_v;
  }
  VkLayerProperties layers[VKR_BINDLESS_VK_MAX_EXTENSIONS];
  if (vkEnumerateInstanceLayerProperties(&count, layers) != VK_SUCCESS) {
    return false_v;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (strcmp(layers[i].layerName, name) == 0) {
      return true_v;
    }
  }
  return false_v;
}

static void vkr_bindless_vk_report_add(VkrBindlessVkCandidateReport *report,
                                       VkrBindlessVkReportKind kind,
                                       const char *name, bool8_t required,
                                       bool8_t present, const char *detail) {
  if (report->entry_count >= VKR_BINDLESS_VK_MAX_REPORT_ENTRIES) {
    report->overflowed = true_v;
    return;
  }
  VkrBindlessVkReportEntry *entry = &report->entries[report->entry_count++];
  MemZero(entry, sizeof(*entry));
  entry->kind = kind;
  entry->required = required;
  entry->present = present;
  snprintf(entry->name, sizeof(entry->name), "%s", name ? name : "");
  snprintf(entry->detail, sizeof(entry->detail), "%s", detail ? detail : "");
}

static void vkr_bindless_vk_report_limit(VkrBindlessVkCandidateReport *report,
                                         const char *name, uint64_t actual,
                                         uint64_t minimum) {
  char detail[VKR_BINDLESS_VK_REPORT_DETAIL_CAPACITY];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " minimum=%" PRIu64,
           actual, minimum);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LIMIT, name, true_v,
                             actual >= minimum, detail);
}

static void
vkr_bindless_vk_report_record_limit(VkrBindlessVkCandidateReport *report,
                                    const char *name, uint64_t actual) {
  char detail[VKR_BINDLESS_VK_REPORT_DETAIL_CAPACITY];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64, actual);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LIMIT, name,
                             false_v, true_v, detail);
}

static bool8_t
vkr_bindless_vk_report_passes(const VkrBindlessVkCandidateReport *report) {
  if (report->overflowed) {
    return false_v;
  }
  for (uint32_t i = 0; i < report->entry_count; ++i) {
    const VkrBindlessVkReportEntry *entry = &report->entries[i];
    if (entry->required && !entry->present) {
      return false_v;
    }
  }
  return true_v;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vkr_bindless_vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
    void *user_data) {
  VkrBindlessVulkanDevice *device = user_data;
  const bool8_t validation_message =
      (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0u;
  const bool8_t gpuav_setup_notice =
      device->config.enable_gpu_assisted && callback_data &&
      callback_data->pMessageIdName &&
      strcmp(callback_data->pMessageIdName, "WARNING-Setting-Limit-Adjusted") ==
          0;
  if (device->config.enable_gpu_assisted && callback_data &&
      callback_data->pMessage &&
      strstr(callback_data->pMessage,
             "GPU-AV does not currently support validation of descriptor "
             "buffers")) {
    device->gpu_assisted_unavailable = true_v;
  }
  if (validation_message &&
      (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
    device->validation_error_count++;
    log_error("Bindless Vulkan validation: %s", callback_data->pMessage);
  } else if (validation_message &&
             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) &&
             gpuav_setup_notice) {
    device->validation_setup_notice_count++;
    log_info("Bindless Vulkan GPU-AV setup: %s", callback_data->pMessage);
  } else if (validation_message &&
             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
    device->validation_warning_count++;
    log_warn("Bindless Vulkan validation: %s", callback_data->pMessage);
  } else {
    log_debug("Bindless Vulkan loader: %s", callback_data->pMessage);
  }
  return VK_FALSE;
}

static bool8_t
vkr_bindless_vk_create_instance(VkrBindlessVulkanDevice *device) {
  uint32_t loader_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS ||
      loader_version < VK_API_VERSION_1_4) {
    log_error("Bindless Vulkan requires a Vulkan 1.4 loader");
    return false_v;
  }

  uint32_t available_count = 0;
  if (vkEnumerateInstanceExtensionProperties(NULL, &available_count, NULL) !=
          VK_SUCCESS ||
      available_count > VKR_BINDLESS_VK_MAX_EXTENSIONS) {
    return false_v;
  }
  VkExtensionProperties available[VKR_BINDLESS_VK_MAX_EXTENSIONS];
  if (vkEnumerateInstanceExtensionProperties(NULL, &available_count,
                                             available) != VK_SUCCESS) {
    return false_v;
  }

  const char *enabled_extensions[5];
  uint32_t enabled_extension_count = 0;
  if (device->config.windowed) {
    const char *window_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(PLATFORM_WINDOWS)
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };
    for (uint32_t i = 0; i < ArrayCount(window_extensions); ++i) {
      if (!vkr_bindless_vk_extension_present(available, available_count,
                                             window_extensions[i])) {
        log_error("Bindless Vulkan window profile missing instance extension: "
                  "%s",
                  window_extensions[i]);
        return false_v;
      }
      enabled_extensions[enabled_extension_count++] = window_extensions[i];
    }
  }
  const bool8_t validation_available =
      vkr_bindless_vk_layer_present("VK_LAYER_KHRONOS_validation");
  if (device->config.enable_validation && !validation_available) {
    log_error("Bindless Vulkan validation requested but layer is unavailable");
    return false_v;
  }
  if (device->config.enable_validation &&
      vkr_bindless_vk_extension_present(available, available_count,
                                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
    enabled_extensions[enabled_extension_count++] =
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  }

  const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
  VkApplicationInfo application_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "VKR bindless renderer",
      .applicationVersion = VK_MAKE_API_VERSION(0, 0, 3, 0),
      .pEngineName = "VKR",
      .engineVersion = VK_MAKE_API_VERSION(0, 0, 3, 0),
      .apiVersion = VK_API_VERSION_1_4,
  };
  VkDebugUtilsMessengerCreateInfoEXT debug_info = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = vkr_bindless_vk_debug_callback,
      .pUserData = device,
  };
  VkValidationFeatureEnableEXT validation_enables[2];
  uint32_t validation_enable_count = 0u;
  if (device->config.enable_synchronization_validation) {
    validation_enables[validation_enable_count++] =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
  }
  if (device->config.enable_gpu_assisted) {
    validation_enables[validation_enable_count++] =
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT;
  }
  const VkValidationFeatureDisableEXT validation_disables[] = {
      VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT,
  };
  const uint32_t validation_disable_count =
      device->config.enable_gpu_assisted &&
              !device->config.enable_synchronization_validation
          ? 1u
          : 0u;
  VkValidationFeaturesEXT validation_features = {
      .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
      .pNext = &debug_info,
      .enabledValidationFeatureCount = validation_enable_count,
      .pEnabledValidationFeatures = validation_enables,
      .disabledValidationFeatureCount = validation_disable_count,
      .pDisabledValidationFeatures = validation_disables,
  };
  VkInstanceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = device->config.enable_validation ? &validation_features : NULL,
      .pApplicationInfo = &application_info,
      .enabledLayerCount = device->config.enable_validation ? 1u : 0u,
      .ppEnabledLayerNames = device->config.enable_validation ? layers : NULL,
      .enabledExtensionCount = enabled_extension_count,
      .ppEnabledExtensionNames = enabled_extensions,
  };
  if (vkCreateInstance(&create_info, NULL, &device->instance) != VK_SUCCESS) {
    return false_v;
  }

  if (device->config.enable_validation) {
    PFN_vkCreateDebugUtilsMessengerEXT create_debug =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            device->instance, "vkCreateDebugUtilsMessengerEXT");
    if (create_debug && create_debug(device->instance, &debug_info, NULL,
                                     &device->debug_messenger) != VK_SUCCESS) {
      return false_v;
    }
  }

  if (device->config.windowed) {
#if defined(PLATFORM_WINDOWS)
    VkWin32SurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = vkr_window_get_win32_instance(device->config.window),
        .hwnd = vkr_window_get_win32_handle(device->config.window),
    };
    if (!surface_info.hinstance || !surface_info.hwnd ||
        vkCreateWin32SurfaceKHR(device->instance, &surface_info, NULL,
                                &device->surface) != VK_SUCCESS) {
      return false_v;
    }
#else
    return false_v;
#endif
  }
  return true_v;
}

static void vkr_bindless_vk_add_feature(VkrBindlessVkCandidateReport *report,
                                        const char *name, bool8_t present) {
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_FEATURE, name,
                             true_v, present, present ? "enabled" : "missing");
}

static void vkr_bindless_vk_query_candidate(
    VkrBindlessVulkanDevice *device, uint32_t candidate_index,
    const VkExtensionProperties *instance_extensions,
    uint32_t instance_extension_count) {
  VkrBindlessVkCandidate *candidate = &device->candidates[candidate_index];
  VkrBindlessVkCandidateReport *report =
      &device->profile.candidates[candidate_index];
  candidate->driver = (VkPhysicalDeviceDriverProperties){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
  };
  candidate
      ->descriptor_properties = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){
      .sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,
  };
  candidate->properties = (VkPhysicalDeviceProperties2){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &candidate->driver,
  };
  candidate->driver.pNext = &candidate->descriptor_properties;
  vkGetPhysicalDeviceProperties2(candidate->physical, &candidate->properties);
  vkGetPhysicalDeviceMemoryProperties(candidate->physical,
                                      &candidate->memory_properties);

  snprintf(report->device_name, sizeof(report->device_name), "%s",
           candidate->properties.properties.deviceName);
  snprintf(report->driver_name, sizeof(report->driver_name), "%s",
           candidate->driver.driverName);
  snprintf(report->driver_info, sizeof(report->driver_info), "%s",
           candidate->driver.driverInfo);
  report->api_version = candidate->properties.properties.apiVersion;
  report->driver_id = candidate->driver.driverID;
  report->conformance_version = candidate->driver.conformanceVersion;

  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
  };
  VkPhysicalDeviceVulkan14Features features14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = &descriptor,
  };
  VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features14,
  };
  VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features13,
  };
  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features12,
  };
  vkGetPhysicalDeviceFeatures2(candidate->physical, &features2);
  candidate->features = (VkrBindlessVkFeatureSet){
      .shader_int64 = features2.features.shaderInt64,
      .buffer_device_address = features12.bufferDeviceAddress,
      .timeline_semaphore = features12.timelineSemaphore,
      .descriptor_indexing = features12.descriptorIndexing,
      .runtime_descriptor_array = features12.runtimeDescriptorArray,
      .sampled_image_non_uniform =
          features12.shaderSampledImageArrayNonUniformIndexing,
      .storage_image_non_uniform =
          features12.shaderStorageImageArrayNonUniformIndexing,
      .scalar_block_layout = features12.scalarBlockLayout,
      .host_query_reset = features12.hostQueryReset,
      .dynamic_rendering = features13.dynamicRendering,
      .synchronization2 = features13.synchronization2,
      .maintenance4 = features13.maintenance4,
      .maintenance5 = features14.maintenance5,
      .host_image_copy = features14.hostImageCopy,
      .descriptor_buffer = descriptor.descriptorBuffer,
      .descriptor_buffer_capture_replay =
          descriptor.descriptorBufferCaptureReplay,
      .descriptor_buffer_image_layout_ignored =
          descriptor.descriptorBufferImageLayoutIgnored,
      .descriptor_buffer_push_descriptors =
          descriptor.descriptorBufferPushDescriptors,
  };

  uint32_t extension_count = 0;
  VkExtensionProperties extensions[VKR_BINDLESS_VK_MAX_EXTENSIONS];
  if (vkEnumerateDeviceExtensionProperties(
          candidate->physical, NULL, &extension_count, NULL) == VK_SUCCESS &&
      extension_count <= VKR_BINDLESS_VK_MAX_EXTENSIONS &&
      vkEnumerateDeviceExtensionProperties(candidate->physical, NULL,
                                           &extension_count,
                                           extensions) == VK_SUCCESS) {
    candidate->has_descriptor_buffer_extension =
        vkr_bindless_vk_extension_present(
            extensions, extension_count,
            VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    candidate->has_swapchain_extension = vkr_bindless_vk_extension_present(
        extensions, extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    candidate->has_swapchain_maintenance_extension =
        vkr_bindless_vk_extension_present(
            extensions, extension_count,
            VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
  }

  const bool8_t api_present =
      candidate->properties.properties.apiVersion >= VK_API_VERSION_1_4;
  char api_detail[64];
  snprintf(api_detail, sizeof(api_detail), "actual=%u.%u.%u minimum=1.4.0",
           VK_API_VERSION_MAJOR(report->api_version),
           VK_API_VERSION_MINOR(report->api_version),
           VK_API_VERSION_PATCH(report->api_version));
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_API_VERSION,
                             "apiVersion", true_v, api_present, api_detail);
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_DEVICE_EXTENSION,
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, true_v,
      candidate->has_descriptor_buffer_extension,
      candidate->has_descriptor_buffer_extension ? "available" : "missing");
  vkr_bindless_vk_add_feature(report, "shaderInt64",
                              candidate->features.shader_int64);
  vkr_bindless_vk_add_feature(report, "bufferDeviceAddress",
                              candidate->features.buffer_device_address);
  vkr_bindless_vk_add_feature(report, "timelineSemaphore",
                              candidate->features.timeline_semaphore);
  vkr_bindless_vk_add_feature(report, "descriptorIndexing",
                              candidate->features.descriptor_indexing);
  vkr_bindless_vk_add_feature(report, "runtimeDescriptorArray",
                              candidate->features.runtime_descriptor_array);
  vkr_bindless_vk_add_feature(report,
                              "shaderSampledImageArrayNonUniformIndexing",
                              candidate->features.sampled_image_non_uniform);
  vkr_bindless_vk_add_feature(report,
                              "shaderStorageImageArrayNonUniformIndexing",
                              candidate->features.storage_image_non_uniform);
  vkr_bindless_vk_add_feature(report, "scalarBlockLayout",
                              candidate->features.scalar_block_layout);
  vkr_bindless_vk_add_feature(report, "hostQueryReset",
                              candidate->features.host_query_reset);
  vkr_bindless_vk_add_feature(report, "dynamicRendering",
                              candidate->features.dynamic_rendering);
  vkr_bindless_vk_add_feature(report, "synchronization2",
                              candidate->features.synchronization2);
  vkr_bindless_vk_add_feature(report, "maintenance4",
                              candidate->features.maintenance4);
  vkr_bindless_vk_add_feature(report, "maintenance5",
                              candidate->features.maintenance5);
  vkr_bindless_vk_add_feature(report, "descriptorBuffer",
                              candidate->features.descriptor_buffer);
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_FEATURE, "hostImageCopy", false_v,
      candidate->features.host_image_copy,
      candidate->features.host_image_copy ? "recorded" : "unavailable");
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_FEATURE, "descriptorBufferCaptureReplay",
      false_v, candidate->features.descriptor_buffer_capture_replay,
      candidate->features.descriptor_buffer_capture_replay ? "recorded"
                                                           : "unavailable");

  uint32_t queue_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(candidate->physical, &queue_count,
                                           NULL);
  VkQueueFamilyProperties queues[VK_MAX_MEMORY_TYPES];
  if (queue_count > ArrayCount(queues)) {
    queue_count = ArrayCount(queues);
  }
  vkGetPhysicalDeviceQueueFamilyProperties(candidate->physical, &queue_count,
                                           queues);
  candidate->queue_family_index = UINT32_MAX;
  const VkQueueFlags required_queue =
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
  for (uint32_t i = 0; i < queue_count; ++i) {
    VkBool32 present = VK_TRUE;
    if (device->config.windowed &&
        vkGetPhysicalDeviceSurfaceSupportKHR(
            candidate->physical, i, device->surface, &present) != VK_SUCCESS) {
      present = VK_FALSE;
    }
    if ((queues[i].queueFlags & required_queue) == required_queue && present) {
      candidate->queue_family_index = i;
      break;
    }
  }
  char queue_detail[64];
  snprintf(queue_detail, sizeof(queue_detail), "family=%u flags=G|C|T%s",
           candidate->queue_family_index, device->config.windowed ? "|P" : "");
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_QUEUE, "graphics+compute+transfer", true_v,
      candidate->queue_family_index != UINT32_MAX, queue_detail);

  const VkPhysicalDeviceLimits *limits =
      &candidate->properties.properties.limits;
  const VkrBindlessVulkanDeviceConfig *config = &device->config;
  vkr_bindless_vk_report_limit(report, "maxPerStageDescriptorSampledImages",
                               limits->maxPerStageDescriptorSampledImages,
                               config->sampled_image_capacity);
  vkr_bindless_vk_report_limit(report, "maxDescriptorSetSampledImages",
                               limits->maxDescriptorSetSampledImages,
                               config->sampled_image_capacity);
  vkr_bindless_vk_report_limit(report, "maxPerStageDescriptorSamplers",
                               limits->maxPerStageDescriptorSamplers,
                               config->sampler_capacity);
  vkr_bindless_vk_report_limit(report, "maxDescriptorSetSamplers",
                               limits->maxDescriptorSetSamplers,
                               config->sampler_capacity);
  vkr_bindless_vk_report_limit(report, "maxPerStageDescriptorStorageImages",
                               limits->maxPerStageDescriptorStorageImages,
                               config->storage_image_capacity);
  vkr_bindless_vk_report_limit(report, "maxDescriptorSetStorageImages",
                               limits->maxDescriptorSetStorageImages,
                               config->storage_image_capacity);
  vkr_bindless_vk_report_limit(report, "maxPerStageResources",
                               limits->maxPerStageResources,
                               (uint64_t)config->sampled_image_capacity +
                                   config->storage_image_capacity + 1u);
  vkr_bindless_vk_report_limit(report, "maxPushConstantsSize",
                               limits->maxPushConstantsSize,
                               config->root_push_constant_size);
  vkr_bindless_vk_report_limit(report, "maxBoundDescriptorSets",
                               limits->maxBoundDescriptorSets, 2u);
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *descriptor_properties =
      &candidate->descriptor_properties;
  vkr_bindless_vk_report_limit(
      report, "maxDescriptorBufferBindings",
      descriptor_properties->maxDescriptorBufferBindings, 2u);
  vkr_bindless_vk_report_limit(
      report, "maxResourceDescriptorBufferBindings",
      descriptor_properties->maxResourceDescriptorBufferBindings, 1u);
  vkr_bindless_vk_report_limit(
      report, "maxSamplerDescriptorBufferBindings",
      descriptor_properties->maxSamplerDescriptorBufferBindings, 1u);
  vkr_bindless_vk_report_limit(
      report, "sampledImageDescriptorSize",
      descriptor_properties->sampledImageDescriptorSize, 1u);
  vkr_bindless_vk_report_limit(
      report, "storageImageDescriptorSize",
      descriptor_properties->storageImageDescriptorSize, 1u);
  vkr_bindless_vk_report_limit(report, "samplerDescriptorSize",
                               descriptor_properties->samplerDescriptorSize,
                               1u);
  vkr_bindless_vk_report_limit(
      report, "descriptorBufferOffsetAlignment",
      descriptor_properties->descriptorBufferOffsetAlignment, 1u);
  vkr_bindless_vk_report_record_limit(report, "bufferImageGranularity",
                                      limits->bufferImageGranularity);
  vkr_bindless_vk_report_record_limit(report, "nonCoherentAtomSize",
                                      limits->nonCoherentAtomSize);
  vkr_bindless_vk_report_record_limit(report, "minMemoryMapAlignment",
                                      limits->minMemoryMapAlignment);
  vkr_bindless_vk_report_record_limit(report,
                                      "optimalBufferCopyOffsetAlignment",
                                      limits->optimalBufferCopyOffsetAlignment);
  vkr_bindless_vk_report_record_limit(
      report, "optimalBufferCopyRowPitchAlignment",
      limits->optimalBufferCopyRowPitchAlignment);

  VkFormatProperties3 format3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 format2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &format3,
  };
  vkGetPhysicalDeviceFormatProperties2(candidate->physical,
                                       VK_FORMAT_R8G8B8A8_UNORM, &format2);
  const VkFormatFeatureFlags2 texture_floor =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  const VkFormatFeatureFlags2 target_floor =
      VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_FORMAT,
                             "R8G8B8A8_UNORM sampled+transfer-dst", true_v,
                             (format3.optimalTilingFeatures & texture_floor) ==
                                 texture_floor,
                             "optimal tiling");
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_FORMAT,
                             "R8G8B8A8_UNORM color+transfer-src", true_v,
                             (format3.optimalTilingFeatures & target_floor) ==
                                 target_floor,
                             "optimal tiling");

  const char *window_instance_extensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(PLATFORM_WINDOWS)
      VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
  };
  for (uint32_t i = 0; i < ArrayCount(window_instance_extensions); ++i) {
    const bool8_t present = vkr_bindless_vk_extension_present(
        instance_extensions, instance_extension_count,
        window_instance_extensions[i]);
    vkr_bindless_vk_report_add(
        report, VKR_BINDLESS_VK_REPORT_INSTANCE_EXTENSION,
        window_instance_extensions[i], device->config.windowed, present,
        device->config.windowed ? "window floor" : "offscreen omitted");
  }
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_DEVICE_EXTENSION,
      VK_KHR_SWAPCHAIN_EXTENSION_NAME, device->config.windowed,
      candidate->has_swapchain_extension,
      device->config.windowed ? "window floor" : "offscreen omitted");
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_DEVICE_EXTENSION,
                             VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
                             false_v,
                             candidate->has_swapchain_maintenance_extension,
                             candidate->has_swapchain_maintenance_extension
                                 ? "optional present-fence path available"
                                 : "reacquisition completion path");

  candidate->common_viable = vkr_bindless_vk_report_passes(report);
  candidate->window_viable =
      candidate->common_viable && candidate->has_swapchain_extension;
  report->offscreen_viable = candidate->common_viable;
  report->window_viable = candidate->window_viable;
  report->queue_family_index = candidate->queue_family_index;
  switch (candidate->properties.properties.deviceType) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    candidate->score = 400u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    candidate->score = 300u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    candidate->score = 200u;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_CPU:
    candidate->score = 100u;
    break;
  default:
    candidate->score = 0u;
    break;
  }
}

static bool8_t
vkr_bindless_vk_enumerate_candidates(VkrBindlessVulkanDevice *device) {
  uint32_t instance_extension_count = 0;
  VkExtensionProperties instance_extensions[VKR_BINDLESS_VK_MAX_EXTENSIONS];
  if (vkEnumerateInstanceExtensionProperties(NULL, &instance_extension_count,
                                             NULL) != VK_SUCCESS ||
      instance_extension_count > VKR_BINDLESS_VK_MAX_EXTENSIONS ||
      vkEnumerateInstanceExtensionProperties(
          NULL, &instance_extension_count, instance_extensions) != VK_SUCCESS) {
    return false_v;
  }
  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(device->instance, &count, NULL) !=
          VK_SUCCESS ||
      count == 0 || count > VKR_BINDLESS_VK_MAX_CANDIDATES) {
    return false_v;
  }
  VkPhysicalDevice physical[VKR_BINDLESS_VK_MAX_CANDIDATES];
  if (vkEnumeratePhysicalDevices(device->instance, &count, physical) !=
      VK_SUCCESS) {
    return false_v;
  }
  device->candidate_count = count;
  device->profile.candidate_count = count;
  for (uint32_t i = 0; i < count; ++i) {
    device->candidates[i].physical = physical[i];
    vkr_bindless_vk_query_candidate(device, i, instance_extensions,
                                    instance_extension_count);
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_load_descriptor_functions(VkrBindlessVulkanDevice *device) {
  device->get_layout_size =
      (PFN_vkGetDescriptorSetLayoutSizeEXT)vkGetDeviceProcAddr(
          device->device, "vkGetDescriptorSetLayoutSizeEXT");
  device->get_binding_offset =
      (PFN_vkGetDescriptorSetLayoutBindingOffsetEXT)vkGetDeviceProcAddr(
          device->device, "vkGetDescriptorSetLayoutBindingOffsetEXT");
  device->get_descriptor = (PFN_vkGetDescriptorEXT)vkGetDeviceProcAddr(
      device->device, "vkGetDescriptorEXT");
  device->cmd_bind_descriptor_buffers =
      (PFN_vkCmdBindDescriptorBuffersEXT)vkGetDeviceProcAddr(
          device->device, "vkCmdBindDescriptorBuffersEXT");
  device->cmd_set_descriptor_offsets =
      (PFN_vkCmdSetDescriptorBufferOffsetsEXT)vkGetDeviceProcAddr(
          device->device, "vkCmdSetDescriptorBufferOffsetsEXT");
  return device->get_layout_size && device->get_binding_offset &&
         device->get_descriptor && device->cmd_bind_descriptor_buffers &&
         device->cmd_set_descriptor_offsets;
}

static void
vkr_bindless_vk_destroy_logical_device(VkrBindlessVulkanDevice *device) {
  if (!device->device) {
    return;
  }
  if (device->resource_layout.handle) {
    vkDestroyDescriptorSetLayout(device->device, device->resource_layout.handle,
                                 NULL);
  }
  if (device->sampler_layout.handle) {
    vkDestroyDescriptorSetLayout(device->device, device->sampler_layout.handle,
                                 NULL);
  }
  vkDestroyDevice(device->device, NULL);
  device->device = VK_NULL_HANDLE;
  device->queue = VK_NULL_HANDLE;
  device->get_layout_size = NULL;
  device->get_binding_offset = NULL;
  device->get_descriptor = NULL;
  device->cmd_bind_descriptor_buffers = NULL;
  device->cmd_set_descriptor_offsets = NULL;
  MemZero(&device->resource_layout, sizeof(device->resource_layout));
  MemZero(&device->sampler_layout, sizeof(device->sampler_layout));
}

static bool8_t
vkr_bindless_vk_create_layouts(VkrBindlessVulkanDevice *device,
                               VkrBindlessVkCandidate *candidate,
                               VkrBindlessVkCandidateReport *report) {
  VkDescriptorSetLayoutBinding resource_bindings[] = {
      {
          .binding = 0u,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = device->config.sampled_image_capacity,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      {
          .binding = 1u,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = device->config.storage_image_capacity,
          .stageFlags =
              VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
      },
  };
  VkDescriptorSetLayoutBinding sampler_binding = {
      .binding = 0u,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = device->config.sampler_capacity,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  VkDescriptorSetLayoutCreateInfo resource_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .bindingCount = ArrayCount(resource_bindings),
      .pBindings = resource_bindings,
  };
  VkDescriptorSetLayoutCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .bindingCount = 1u,
      .pBindings = &sampler_binding,
  };
  VkDescriptorSetLayoutSupport resource_support = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
  };
  VkDescriptorSetLayoutSupport sampler_support = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
  };
  vkGetDescriptorSetLayoutSupport(device->device, &resource_info,
                                  &resource_support);
  vkGetDescriptorSetLayoutSupport(device->device, &sampler_info,
                                  &sampler_support);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LAYOUT,
                             "resource descriptor-buffer layout support",
                             true_v, resource_support.supported,
                             "vkGetDescriptorSetLayoutSupport");
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LAYOUT,
                             "sampler descriptor-buffer layout support", true_v,
                             sampler_support.supported,
                             "vkGetDescriptorSetLayoutSupport");
  if (!resource_support.supported || !sampler_support.supported ||
      vkCreateDescriptorSetLayout(device->device, &resource_info, NULL,
                                  &device->resource_layout.handle) !=
          VK_SUCCESS ||
      vkCreateDescriptorSetLayout(device->device, &sampler_info, NULL,
                                  &device->sampler_layout.handle) !=
          VK_SUCCESS) {
    return false_v;
  }

  device->get_layout_size(device->device, device->resource_layout.handle,
                          &device->resource_layout.size);
  device->get_layout_size(device->device, device->sampler_layout.handle,
                          &device->sampler_layout.size);
  device->get_binding_offset(device->device, device->resource_layout.handle, 0u,
                             &device->resource_layout.sampled_image_offset);
  device->get_binding_offset(device->device, device->resource_layout.handle, 1u,
                             &device->resource_layout.storage_image_offset);
  device->get_binding_offset(device->device, device->sampler_layout.handle, 0u,
                             &device->sampler_layout.sampler_offset);

  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      &candidate->descriptor_properties;
  const bool8_t resource_range = device->resource_layout.size <=
                                 properties->maxResourceDescriptorBufferRange;
  const bool8_t sampler_range = device->sampler_layout.size <=
                                properties->maxSamplerDescriptorBufferRange;
  const bool8_t resource_space =
      device->resource_layout.size <=
      properties->resourceDescriptorBufferAddressSpaceSize;
  const bool8_t sampler_space =
      device->sampler_layout.size <=
      properties->samplerDescriptorBufferAddressSpaceSize;
  const bool8_t combined_space =
      device->sampler_layout.size <=
          properties->descriptorBufferAddressSpaceSize &&
      device->resource_layout.size <=
          properties->descriptorBufferAddressSpaceSize -
              device->sampler_layout.size;
  char detail[VKR_BINDLESS_VK_REPORT_DETAIL_CAPACITY];
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " maximum=%" PRIu64,
           (uint64_t)device->resource_layout.size,
           (uint64_t)properties->maxResourceDescriptorBufferRange);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LIMIT,
                             "resource descriptor layout bytes", true_v,
                             resource_range && resource_space, detail);
  snprintf(detail, sizeof(detail), "actual=%" PRIu64 " maximum=%" PRIu64,
           (uint64_t)device->sampler_layout.size,
           (uint64_t)properties->maxSamplerDescriptorBufferRange);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LIMIT,
                             "sampler descriptor layout bytes", true_v,
                             sampler_range && sampler_space, detail);
  snprintf(detail, sizeof(detail), "resource=%" PRIu64 " sampler=%" PRIu64,
           (uint64_t)device->resource_layout.size,
           (uint64_t)device->sampler_layout.size);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_LIMIT,
                             "descriptor buffer address spaces", true_v,
                             combined_space, detail);
  return vkr_bindless_vk_report_passes(report);
}

static bool8_t vkr_bindless_vk_try_candidate(VkrBindlessVulkanDevice *device,
                                             uint32_t candidate_index) {
  VkrBindlessVkCandidate *candidate = &device->candidates[candidate_index];
  VkrBindlessVkCandidateReport *report =
      &device->profile.candidates[candidate_index];
  float32_t priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = candidate->queue_family_index,
      .queueCount = 1u,
      .pQueuePriorities = &priority,
  };
  VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
      .descriptorBuffer = VK_TRUE,
#if !defined(NDEBUG)
      .descriptorBufferCaptureReplay =
          candidate->features.descriptor_buffer_capture_replay ? VK_TRUE
                                                               : VK_FALSE,
#endif
  };
  VkPhysicalDeviceVulkan14Features features14 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
      .pNext = &descriptor,
      .maintenance5 = VK_TRUE,
  };
  VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features14,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
      .maintenance4 = VK_TRUE,
  };
  VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .pNext = &features13,
      .descriptorIndexing = VK_TRUE,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .scalarBlockLayout = VK_TRUE,
      .hostQueryReset = VK_TRUE,
      .timelineSemaphore = VK_TRUE,
      .bufferDeviceAddress = VK_TRUE,
  };
  VkPhysicalDeviceFeatures2 features2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = &features12,
      .features = {.shaderInt64 = VK_TRUE},
  };
  const char *extensions[] = {
      VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };
  VkDeviceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features2,
      .queueCreateInfoCount = 1u,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = device->config.windowed ? 2u : 1u,
      .ppEnabledExtensionNames = extensions,
  };
  const VkResult result =
      vkCreateDevice(candidate->physical, &create_info, NULL, &device->device);
  char detail[96];
  snprintf(detail, sizeof(detail), "VkResult=%d", result);
  vkr_bindless_vk_report_add(report, VKR_BINDLESS_VK_REPORT_DEVICE_CREATE,
                             "vkCreateDevice", true_v, result == VK_SUCCESS,
                             detail);
  if (result != VK_SUCCESS) {
    return false_v;
  }
  vkGetDeviceQueue(device->device, candidate->queue_family_index, 0u,
                   &device->queue);
  const bool8_t functions_loaded =
      vkr_bindless_vk_load_descriptor_functions(device);
  vkr_bindless_vk_report_add(
      report, VKR_BINDLESS_VK_REPORT_DEVICE_CREATE,
      "descriptor-buffer function table", true_v, functions_loaded,
      functions_loaded ? "all required entry points resolved"
                       : "required entry point was NULL");
  return functions_loaded &&
         vkr_bindless_vk_create_layouts(device, candidate, report);
}

static bool8_t vkr_bindless_vk_select_device(VkrBindlessVulkanDevice *device) {
  bool8_t attempted[VKR_BINDLESS_VK_MAX_CANDIDATES] = {0};
  for (uint32_t attempt = 0; attempt < device->candidate_count; ++attempt) {
    uint32_t best = UINT32_MAX;
    uint32_t best_score = 0;
    for (uint32_t i = 0; i < device->candidate_count; ++i) {
      const VkrBindlessVkCandidate *candidate = &device->candidates[i];
      const bool8_t viable = device->config.windowed ? candidate->window_viable
                                                     : candidate->common_viable;
      if (!attempted[i] && viable &&
          (best == UINT32_MAX || candidate->score > best_score)) {
        best = i;
        best_score = candidate->score;
      }
    }
    if (best == UINT32_MAX) {
      break;
    }
    attempted[best] = true_v;
    if (vkr_bindless_vk_try_candidate(device, best)) {
      device->selected_candidate_index = best;
      device->profile.selected_candidate_index = best;
      device->selected = &device->candidates[best];
      device->ready = true_v;
      return true_v;
    }
    vkr_bindless_vk_destroy_logical_device(device);
  }
  return false_v;
}

bool8_t
vkr_bindless_vulkan_device_create(const VkrBindlessVulkanDeviceConfig *config,
                                  VkrBindlessVulkanDevice **out_device) {
  if (!config || !config->allocator || !out_device ||
      config->sampled_image_capacity == 0 ||
      config->storage_image_capacity == 0 || config->sampler_capacity == 0 ||
      config->root_push_constant_size == 0 ||
      (config->windowed && !config->window)) {
    return false_v;
  }
  *out_device = NULL;
  VkrBindlessVulkanDevice *device = vkr_allocator_alloc(
      config->allocator, sizeof(*device), VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
  if (!device) {
    return false_v;
  }
  MemZero(device, sizeof(*device));
  device->allocator = config->allocator;
  device->config = *config;
  device->profile.windowed = config->windowed;
  device->profile.selected_candidate_index = UINT32_MAX;
  device->selected_candidate_index = UINT32_MAX;
  *out_device = device;
  if (!vkr_bindless_vk_create_instance(device) ||
      !vkr_bindless_vk_enumerate_candidates(device) ||
      !vkr_bindless_vk_select_device(device)) {
    return false_v;
  }
  return true_v;
}

void vkr_bindless_vulkan_device_destroy(VkrBindlessVulkanDevice *device) {
  if (!device) {
    return;
  }
  VkrAllocator *allocator = device->allocator;
  vkr_bindless_vk_destroy_logical_device(device);
  if (device->surface) {
    vkDestroySurfaceKHR(device->instance, device->surface, NULL);
  }
  if (device->debug_messenger) {
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            device->instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroy_debug) {
      destroy_debug(device->instance, device->debug_messenger, NULL);
    }
  }
  if (device->instance) {
    vkDestroyInstance(device->instance, NULL);
  }
  vkr_allocator_free(allocator, device, sizeof(*device),
                     VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
}

bool8_t
vkr_bindless_vulkan_device_is_ready(const VkrBindlessVulkanDevice *device) {
  return device && device->ready;
}

const VkrBindlessVkCapabilityProfile *
vkr_bindless_vulkan_device_profile(const VkrBindlessVulkanDevice *device) {
  return device ? &device->profile : NULL;
}

VkInstance
vkr_bindless_vulkan_device_instance(const VkrBindlessVulkanDevice *device) {
  return device ? device->instance : VK_NULL_HANDLE;
}

VkPhysicalDevice
vkr_bindless_vulkan_device_physical(const VkrBindlessVulkanDevice *device) {
  return device && device->selected ? device->selected->physical
                                    : VK_NULL_HANDLE;
}

VkDevice
vkr_bindless_vulkan_device_handle(const VkrBindlessVulkanDevice *device) {
  return device ? device->device : VK_NULL_HANDLE;
}

VkQueue
vkr_bindless_vulkan_device_queue(const VkrBindlessVulkanDevice *device) {
  return device ? device->queue : VK_NULL_HANDLE;
}

VkSurfaceKHR
vkr_bindless_vulkan_device_surface(const VkrBindlessVulkanDevice *device) {
  return device ? device->surface : VK_NULL_HANDLE;
}

uint32_t
vkr_bindless_vulkan_device_queue_family(const VkrBindlessVulkanDevice *device) {
  return device && device->selected ? device->selected->queue_family_index
                                    : UINT32_MAX;
}

const VkPhysicalDeviceProperties2 *
vkr_bindless_vulkan_device_properties(const VkrBindlessVulkanDevice *device) {
  return device && device->selected ? &device->selected->properties : NULL;
}

const VkPhysicalDeviceMemoryProperties *
vkr_bindless_vulkan_device_memory_properties(
    const VkrBindlessVulkanDevice *device) {
  return device && device->selected ? &device->selected->memory_properties
                                    : NULL;
}

const VkPhysicalDeviceDescriptorBufferPropertiesEXT *
vkr_bindless_vulkan_device_descriptor_properties(
    const VkrBindlessVulkanDevice *device) {
  return device && device->selected ? &device->selected->descriptor_properties
                                    : NULL;
}

const VkrBindlessVulkanDescriptorLayout *
vkr_bindless_vulkan_device_resource_layout(
    const VkrBindlessVulkanDevice *device) {
  return device ? &device->resource_layout : NULL;
}

const VkrBindlessVulkanDescriptorLayout *
vkr_bindless_vulkan_device_sampler_layout(
    const VkrBindlessVulkanDevice *device) {
  return device ? &device->sampler_layout : NULL;
}

PFN_vkGetDescriptorEXT vkr_bindless_vulkan_device_get_descriptor(
    const VkrBindlessVulkanDevice *device) {
  return device ? device->get_descriptor : NULL;
}

PFN_vkCmdBindDescriptorBuffersEXT
vkr_bindless_vulkan_device_cmd_bind_descriptor_buffers(
    const VkrBindlessVulkanDevice *device) {
  return device ? device->cmd_bind_descriptor_buffers : NULL;
}

PFN_vkCmdSetDescriptorBufferOffsetsEXT
vkr_bindless_vulkan_device_cmd_set_descriptor_offsets(
    const VkrBindlessVulkanDevice *device) {
  return device ? device->cmd_set_descriptor_offsets : NULL;
}

void vkr_bindless_vulkan_device_validation_stats(
    const VkrBindlessVulkanDevice *device,
    VkrBindlessVulkanValidationStats *out_stats) {
  if (!out_stats) {
    return;
  }
  *out_stats = device ? (VkrBindlessVulkanValidationStats){
                            .setup_notice_count =
                                device->validation_setup_notice_count,
                            .warning_count = device->validation_warning_count,
                            .error_count = device->validation_error_count,
                            .gpu_assisted_unavailable =
                                device->gpu_assisted_unavailable,
                        }
                      : (VkrBindlessVulkanValidationStats){0};
}
