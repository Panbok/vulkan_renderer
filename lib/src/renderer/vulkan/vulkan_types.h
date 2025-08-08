#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#if defined(PLATFORM_APPLE)
#include <vulkan/vulkan_metal.h>
#endif

#if defined(PLATFORM_WINDOWS)
#include <vulkan/vulkan_win32.h>
#endif

#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "renderer/renderer.h"

// todo: make this configurable
#define BUFFERING_FRAMES 3

typedef enum QueueFamilyType {
  QUEUE_FAMILY_TYPE_GRAPHICS,
  QUEUE_FAMILY_TYPE_PRESENT,
  QUEUE_FAMILY_TYPE_TRANSFER,
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
Array(VkFramebuffer);
Array(VkSemaphore);
Array(VkVertexInputBindingDescription);
Array(VkVertexInputAttributeDescription);

#define VK_EXT_METAL_SURFACE_EXTENSION_NAME "VK_EXT_metal_surface"

#ifndef NDEBUG
#define VK_LAYER_KHRONOS_VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"
static const char *VALIDATION_LAYERS[] = {"VK_LAYER_KHRONOS_validation"};
#endif

typedef struct VulkanFence {
  VkFence handle;
  bool8_t is_signaled;
} VulkanFence;

typedef struct VulkanBuffer {
  VkBuffer handle;
  VkDeviceMemory memory;
  VkBufferUsageFlagBits usage;
  uint64_t total_size;

  bool8_t is_locked;

  int32_t memory_index;
  uint32_t memory_property_flags;

  VkCommandPool command_pool;
  VkQueue queue;
} VulkanBuffer;

typedef enum VulkanRenderPassState {
  RENDER_PASS_STATE_READY,
  RENDER_PASS_STATE_RECORDING,
  RENDER_PASS_STATE_IN_RENDER_PASS,
  RENDER_PASS_STATE_RECORDING_ENDED,
  RENDER_PASS_STATE_SUBMITTED,
  RENDER_PASS_STATE_NOT_ALLOCATED
} VulkanRenderPassState;

typedef struct VulkanRenderPass {
  VkRenderPass handle;
  float32_t x, y, width, height;
  float32_t r, g, b, a;

  float32_t depth;

  uint32_t stencil;

  VulkanRenderPassState state;
} VulkanRenderPass;

typedef enum VulkanCommandBufferState {
  COMMAND_BUFFER_STATE_READY,
  COMMAND_BUFFER_STATE_RECORDING,
  COMMAND_BUFFER_STATE_IN_RENDER_PASS,
  COMMAND_BUFFER_STATE_RECORDING_ENDED,
  COMMAND_BUFFER_STATE_SUBMITTED,
  COMMAND_BUFFER_STATE_NOT_ALLOCATED
} VulkanCommandBufferState;

typedef struct VulkanCommandBuffer {
  VkCommandBuffer handle;
  VulkanCommandBufferState state;
} VulkanCommandBuffer;

typedef struct VulkanImage {
  VkImage handle;
  VkDeviceMemory memory;
  VkImageView view;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t array_layers;
} VulkanImage;

Array(VulkanImage);

typedef struct VulkanSwapchainDetails {
  VkSurfaceCapabilitiesKHR capabilities;
  Array_VkSurfaceFormatKHR formats;
  Array_VkPresentModeKHR present_modes;
} VulkanSwapchainDetails;

/* non-copyable */
typedef struct VulkanDevice {
  VkPhysicalDevice physical_device;
  VkDevice logical_device;
  VkCommandPool graphics_command_pool;

  VulkanSwapchainDetails swapchain_details;

  int32_t graphics_queue_index;
  int32_t present_queue_index;
  int32_t transfer_queue_index;

  VkQueue graphics_queue;
  VkQueue present_queue;
  VkQueue transfer_queue;

  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceFeatures features;
  VkPhysicalDeviceMemoryProperties memory;

  VkFormat depth_format;
} VulkanDevice;

typedef struct VulkanFramebuffer {
  VkFramebuffer handle;
  Array_VkImageView attachments;
  VulkanRenderPass *renderpass;
} VulkanFramebuffer;

Array(VulkanFramebuffer);
typedef struct VulkanSwapchain {
  VkSwapchainKHR handle;
  VkFormat format;
  VkExtent2D extent;

  uint32_t image_count;
  uint8_t max_in_flight_frames;

  VulkanImage depth_attachment;

  Array_VkImage images;
  Array_VkImageView image_views;
  Array_VulkanFramebuffer framebuffers;
} VulkanSwapchain;

typedef VulkanFence *VulkanFencePtr;

Array(VulkanCommandBuffer);
Array(VulkanFence);
Array(VulkanFencePtr);

struct s_BufferHandle {
  VulkanBuffer buffer;
  BufferDescription description;
};

typedef struct VulkanShaderObject {
  VkPipelineShaderStageCreateInfo stages[SHADER_STAGE_COUNT];
  VkShaderModule modules[SHADER_STAGE_COUNT];

  VkDescriptorPool global_descriptor_pool;
  VkDescriptorSet global_descriptor_sets[3];
  VkDescriptorSetLayout global_descriptor_set_layout;
  VkDescriptorSetLayoutBinding global_descriptor_set_layout_binding;
  struct s_BufferHandle global_uniform_buffer;
} VulkanShaderObject;

struct s_GraphicsPipeline {
  const GraphicsPipelineDescription *desc;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;

  VulkanShaderObject shader_object;
};

typedef struct VulkanBackendState {
  Arena *arena;
  Arena *temp_arena;
  Arena *swapchain_arena;
  Window *window;
  DeviceRequirements *device_requirements;

  VkAllocationCallbacks *allocator;

  bool8_t is_swapchain_recreation_requested;

  uint32_t current_frame;
  uint32_t image_index;

  VkInstance instance;

#ifndef NDEBUG
  VkDebugUtilsMessengerEXT debug_messenger;
#endif

  VulkanDevice device;

  VulkanRenderPass *main_render_pass;

  VkSurfaceKHR surface;

  VulkanSwapchain swapchain;

  Array_VkSemaphore image_available_semaphores;
  Array_VkSemaphore queue_complete_semaphores;
  Array_VulkanFence in_flight_fences;
  Array_VulkanFencePtr images_in_flight;

  Array_VulkanCommandBuffer graphics_command_buffers;
} VulkanBackendState;