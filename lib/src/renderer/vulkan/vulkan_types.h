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
#include "defines.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_dmemory.h"
#include "renderer/vkr_renderer.h"

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

  // DMemory Allocator for offset tracking (not actual memory, just bookkeeping)
  VkrAllocator allocator;
  VkrDMemory offset_allocator; // Tracks which offsets are allocated
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

  Vec2 position;
  Vec4 color;

  float32_t width, height;

  float32_t depth;

  uint32_t stencil;

  VulkanRenderPassState state;
  VkrPipelineDomain domain;
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

typedef struct VulkanTexture {
  VulkanImage image;
  VkSampler sampler;
} VulkanTexture;

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
  VkrBufferDescription description;
};

#define VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT 3
typedef struct VulkanShaderObjectDescriptorState {
  // Per-frame descriptor generation tracking; length == frame_count
  uint32_t *generations;
} VulkanShaderObjectDescriptorState;

#define VULKAN_SHADER_OBJECT_LOCAL_STATE_COUNT 1024
typedef struct VulkanShaderObjectLocalState {
  // Per-frame descriptor sets; length == frame_count
  VkDescriptorSet *descriptor_sets;

  VulkanShaderObjectDescriptorState
      descriptor_states[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings
      [VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];

} VulkanShaderObjectLocalState;

typedef struct VulkanShaderObject {
  VkPipelineShaderStageCreateInfo stages[VKR_SHADER_STAGE_COUNT];
  VkShaderModule modules[VKR_SHADER_STAGE_COUNT];

  VkDescriptorPool global_descriptor_pool;
  // Per-frame global descriptor sets; length == frame_count
  VkDescriptorSet *global_descriptor_sets;
  VkDescriptorSetLayout global_descriptor_set_layout;
  VkDescriptorSetLayoutBinding global_descriptor_set_layout_binding;
  struct s_BufferHandle global_uniform_buffer;

  // todo: rework into free list of objects
  uint32_t frame_count;
  uint32_t local_uniform_buffer_count;
  uint32_t local_state_free_count;
  uint32_t local_state_free_ids[VULKAN_SHADER_OBJECT_LOCAL_STATE_COUNT];
  VkDescriptorPool local_descriptor_pool;
  VkDescriptorSetLayout local_descriptor_set_layout;
  struct s_BufferHandle local_uniform_buffer;
  VulkanShaderObjectLocalState
      local_states[VULKAN_SHADER_OBJECT_LOCAL_STATE_COUNT];
} VulkanShaderObject;

struct s_GraphicsPipeline {
  VkrGraphicsPipelineDescription desc;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;

  VulkanShaderObject shader_object;
};

struct s_TextureHandle {
  VulkanTexture texture;
  VkrTextureDescription description;
};

/**
 * @brief Vulkan backend state containing all rendering resources and state
 *
 * This structure manages the entire Vulkan rendering pipeline including the
 * automatic multi-render pass system (P14 implementation). Key features:
 *
 * MULTI-RENDER PASS SYSTEM (P14):
 * - Domain-based render passes allow automatic switching between rendering
 *   contexts (WORLD, UI, SHADOW, POST, COMPUTE)
 * - Render passes are started/stopped automatically based on pipeline domain
 * - No explicit render pass management required by application code
 *
 * RENDER PASS LIFECYCLE:
 * 1. begin_frame: Does NOT start any render pass (state.render_pass_active =
 * false)
 * 2. pipeline bind: Automatically starts domain-specific render pass if needed
 * 3. pipeline domain change: Automatically ends current pass, starts new pass
 * 4. end_frame: Automatically ends any active render pass
 *
 * DOMAIN CONFIGURATIONS:
 * - WORLD: Color+Depth, finalLayout=COLOR_ATTACHMENT_OPTIMAL (chains to UI)
 * - UI: Color-only, loadOp=LOAD (preserves world), finalLayout=PRESENT_SRC_KHR
 * - SHADOW: Depth-only, for shadow map generation
 * - POST: Color-only, for post-processing effects
 */
typedef struct VulkanBackendState {
  Arena *arena;
  Arena *temp_arena;
  Arena *swapchain_arena;
  VkrWindow *window;
  VkrDeviceRequirements *device_requirements;

  VkAllocationCallbacks *allocator;

  bool8_t is_swapchain_recreation_requested;

  float64_t frame_delta;
  uint32_t current_frame;
  uint32_t image_index;

  VkInstance instance;

#ifndef NDEBUG
  VkDebugUtilsMessengerEXT debug_messenger;
#endif

  VulkanDevice device;

  /**
   * Domain-specific render passes indexed by VkrPipelineDomain.
   * Each domain has unique attachment configurations and pipeline states:
   * - WORLD: Color + Depth attachments
   * - UI: Color only (preserves world rendering)
   * - SHADOW: Depth only
   * - POST: Color only (for post-processing)
   */
  VulkanRenderPass *domain_render_passes[VKR_PIPELINE_DOMAIN_COUNT];

  /**
   * Framebuffers for each domain, per swapchain image.
   * Indexed as: domain_framebuffers[domain][swapchain_image_index]
   * Recreated on swapchain resize/recreation.
   */
  Array_VulkanFramebuffer domain_framebuffers[VKR_PIPELINE_DOMAIN_COUNT];

  /** Tracks which domains have been initialized */
  bool8_t domain_initialized[VKR_PIPELINE_DOMAIN_COUNT];

  /**
   * Currently active render pass domain.
   * Set to VKR_PIPELINE_DOMAIN_COUNT when no pass is active.
   * Used by automatic switching logic to detect domain changes.
   */
  VkrPipelineDomain current_render_pass_domain;

  /**
   * Indicates if a render pass is currently recording.
   * - Set to false in begin_frame (no pass started)
   * - Set to true when pipeline binding starts a render pass
   * - Set to false in end_frame after ending any active pass
   */
  bool8_t render_pass_active;

  uint32_t active_image_index;

  /**
   * Tracks if the swapchain image is in PRESENT_SRC_KHR layout.
   * Used to avoid redundant layout transitions in end_frame.
   * Set to true when UI or POST domain ends (transitions to PRESENT).
   */
  bool8_t swapchain_image_is_present_ready;

  VkSurfaceKHR surface;

  VulkanSwapchain swapchain;

  Array_VkSemaphore image_available_semaphores;
  Array_VkSemaphore queue_complete_semaphores;
  Array_VulkanFence in_flight_fences;
  Array_VulkanFencePtr images_in_flight;

  Array_VulkanCommandBuffer graphics_command_buffers;
} VulkanBackendState;