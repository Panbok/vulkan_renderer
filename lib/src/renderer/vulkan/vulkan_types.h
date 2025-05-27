#pragma once

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include "../renderer.h"
#include "str.h"

Array(VkExtensionProperties);
Array(VkLayerProperties);
Array(VkPhysicalDevice);

#define VK_EXT_METAL_SURFACE_EXTENSION_NAME "VK_EXT_metal_surface"
#define VK_LAYER_KHRONOS_VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"

typedef struct VulkanBackendState {
  Arena *arena;
  Arena *temp_arena;

  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  Array_VkExtensionProperties extension_properties;

  VkPhysicalDevice physical_device;
  VkDevice device;

  VkSurfaceKHR surface;
} VulkanBackendState;