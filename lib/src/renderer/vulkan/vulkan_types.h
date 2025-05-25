#pragma once

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include "../renderer.h"
#include "str.h"

Array(VkExtensionProperties);
Array(VkLayerProperties);

typedef struct VulkanBackendState {
  Arena *arena;

  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  Array_VkExtensionProperties extension_properties;
  Array_String8 validation_layer;

  VkPhysicalDevice physical_device;
  VkDevice device;

  VkSurfaceKHR surface;
} VulkanBackendState;