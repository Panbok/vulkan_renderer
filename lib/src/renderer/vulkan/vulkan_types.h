#pragma once

#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#if defined(PLATFORM_APPLE)
#include <vulkan/vulkan_metal.h>
#endif

#include "containers/str.h"
#include "renderer/renderer.h"

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
Array(VkExtensionProperties);
Array(VkLayerProperties);
Array(VkPhysicalDevice);
Array(VkQueueFamilyProperties);
Array(VkDeviceQueueCreateInfo);
Array(VkSurfaceFormatKHR);
Array(VkPresentModeKHR);
Array(VkImage);
Array(VkImageView);

#define VK_EXT_METAL_SURFACE_EXTENSION_NAME "VK_EXT_metal_surface"
#define VK_LAYER_KHRONOS_VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"

typedef struct VulkanSwapchainDetails {
  VkSurfaceCapabilitiesKHR capabilities;
  Array_VkSurfaceFormatKHR formats;
  Array_VkPresentModeKHR present_modes;
} VulkanSwapchainDetails;

typedef struct VulkanBackendState {
  Arena *arena;
  Arena *temp_arena;
  Window *window;

  Array_String8 validation_layers;

  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  Array_VkExtensionProperties extension_properties;

  VkPhysicalDevice physical_device;
  VkDevice device;

  Array_VkDeviceQueueCreateInfo queue_create_infos;
  VkQueue graphics_queue;
  VkQueue present_queue;

  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkFormat swapChainImageFormat;
  VkExtent2D swapChainExtent;
  Array_VkImage swapChainImages;
  Array_VkImageView swapChainImageViews;
} VulkanBackendState;