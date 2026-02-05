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
#include "memory/vkr_pool.h"
#include "renderer/vkr_renderer.h"
#include "vulkan_allocator.h"

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
  uint64_t allocation_size;

  bool8_t is_locked;
  void *mapped_ptr;

  int32_t memory_index;
  uint32_t memory_property_flags;

  VkCommandPool command_pool;
  VkQueue queue;

  // DMemory Allocator for offset tracking (not actual memory, just bookkeeping)
  VkrAllocator allocator;
  VkrDMemory offset_allocator; // Tracks which offsets are allocated
} VulkanBuffer;

typedef struct VulkanRenderPass {
  VkRenderPass handle;
  VkrPipelineDomain domain;

  // Cached signature for compatibility checking and pipeline state derivation
  VkrRenderPassSignature signature;
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
  VkDescriptorSet bound_global_descriptor_set;
  VkPipelineLayout bound_global_pipeline_layout;
} VulkanCommandBuffer;

typedef struct VulkanImage {
  VkImage handle;
  VkDeviceMemory memory;
  VkImageView view;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t array_layers;
  uint32_t memory_property_flags;
  VkSampleCountFlagBits samples;
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
  VkCommandPool transfer_command_pool;

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

#define VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT                            \
  (1 + (VKR_MAX_INSTANCE_TEXTURES * 2))
typedef struct VulkanShaderObjectDescriptorState {
  // Per-frame descriptor generation tracking; length == frame_count
  uint32_t *generations;
} VulkanShaderObjectDescriptorState;

#define VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT 8192
typedef struct VulkanShaderObjectInstanceState {
  // Per-frame descriptor sets; length == frame_count
  VkDescriptorSet *descriptor_sets;

  VulkanShaderObjectDescriptorState
      descriptor_states[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings
      [VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];

  // Submit serial when release requested; used to defer freeing until safe.
  uint64_t release_serial;
  bool8_t release_pending;

} VulkanShaderObjectInstanceState;

typedef struct VulkanShaderObject {
  VkPipelineShaderStageCreateInfo stages[VKR_SHADER_STAGE_COUNT];
  VkShaderModule modules[VKR_SHADER_STAGE_COUNT];

  VkDescriptorPool global_descriptor_pool;
  // Per-frame global descriptor sets; length == frame_count
  VkDescriptorSet *global_descriptor_sets;
  uint32_t *global_descriptor_generations;
  VkBuffer *global_descriptor_instance_buffers;
  VkDescriptorSetLayout global_descriptor_set_layout;
  VkDescriptorSetLayoutBinding global_descriptor_set_layout_binding;
  struct s_BufferHandle global_uniform_buffer;

  uint32_t frame_count;
  uint32_t instance_uniform_buffer_count;
  uint32_t instance_state_free_count;
  uint32_t instance_state_free_ids[VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT];
  VkDescriptorPool instance_descriptor_pool;
  VkDescriptorSetLayout instance_descriptor_set_layout;
  struct s_BufferHandle instance_uniform_buffer;
  VulkanShaderObjectInstanceState
      instance_states[VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT];
  // Deferred instance releases awaiting GPU completion.
  uint32_t pending_release_count;
  uint32_t pending_release_ids[VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT];

  uint64_t global_ubo_size;
  uint64_t global_ubo_stride;
  uint64_t instance_ubo_size;
  uint64_t instance_ubo_stride;
  uint64_t push_constant_size;
  uint32_t global_texture_count;
  uint32_t instance_texture_count;
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
#ifndef NDEBUG
  uint32_t generation; // Debug: generation counter for liveness validation
#endif
};

// Max attachments: colors + depth/stencil + resolves
#define VKR_RENDER_TARGET_MAX_ATTACHMENTS \
  (VKR_MAX_COLOR_ATTACHMENTS * 2 + 1)

struct s_RenderPass {
  VulkanRenderPass *vk;
  String8 name;
  uint8_t attachment_count;
  VkClearValue clear_values[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
  uint8_t resolve_attachment_count;
  VkrResolveAttachmentRef resolve_attachments[VKR_MAX_COLOR_ATTACHMENTS];
  bool8_t ends_in_present;
};

struct s_RenderTarget {
  VkFramebuffer handle;
  uint32_t width, height;
  uint32_t layer_count;
  bool8_t sync_to_window_size;
  uint8_t attachment_count;
  struct s_TextureHandle *attachments[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
  VkImageView attachment_views[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
  bool8_t attachment_view_owned[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
#ifndef NDEBUG
  // Captured texture generations at render target creation for liveness validation
  uint32_t attachment_generations[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
#endif
};

typedef struct VkrRenderPassEntry {
  String8 name;
  struct s_RenderPass *pass;
} VkrRenderPassEntry;
Array(VkrRenderPassEntry);

// ============================================================================
// Framebuffer Cache - avoids redundant framebuffer creation
// ============================================================================

#define VKR_FRAMEBUFFER_CACHE_MAX_ENTRIES 64

typedef struct VkrFramebufferCacheKey {
  VkRenderPass render_pass;
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint8_t attachment_count;
  VkImageView attachments[VKR_RENDER_TARGET_MAX_ATTACHMENTS];
} VkrFramebufferCacheKey;

typedef struct VkrFramebufferCacheEntry {
  VkrFramebufferCacheKey key;
  VkFramebuffer framebuffer;
  bool8_t in_use;
} VkrFramebufferCacheEntry;

typedef struct VkrFramebufferCache {
  VkrFramebufferCacheEntry entries[VKR_FRAMEBUFFER_CACHE_MAX_ENTRIES];
  uint32_t entry_count;
} VkrFramebufferCache;

// ============================================================================
// Deferred Destruction Queue - delays resource destruction until GPU is done
// ============================================================================

#define VKR_DEFERRED_DESTROY_QUEUE_SIZE 256

typedef enum VkrDeferredDestroyKind {
  VKR_DEFERRED_DESTROY_FRAMEBUFFER = 0,
  VKR_DEFERRED_DESTROY_RENDERPASS,
  VKR_DEFERRED_DESTROY_IMAGE,
  VKR_DEFERRED_DESTROY_IMAGE_VIEW,
  VKR_DEFERRED_DESTROY_SAMPLER,
  VKR_DEFERRED_DESTROY_BUFFER,
  VKR_DEFERRED_DESTROY_TEXTURE_WRAPPER,
  VKR_DEFERRED_DESTROY_BUFFER_WRAPPER,
  VKR_DEFERRED_DESTROY_RENDER_TARGET_WRAPPER,
} VkrDeferredDestroyKind;

typedef struct VkrDeferredDestroyEntry {
  VkrDeferredDestroyKind kind;
  uint64_t submit_serial; // Frame serial when destruction was requested
  union {
    VkFramebuffer framebuffer;
    VkRenderPass renderpass;
    VkImage image;
    VkImageView image_view;
    VkSampler sampler;
    VkBuffer buffer;
    void *wrapper; // For TEXTURE_WRAPPER, BUFFER_WRAPPER, RENDER_TARGET_WRAPPER
  } payload;
  VkDeviceMemory memory; // Optional memory to free (for images/buffers)
  VkrAllocator *pool_alloc; // Allocator to return wrapper to (if applicable)
  uint64_t wrapper_size;    // Size of wrapper struct (for pool free)
} VkrDeferredDestroyEntry;

typedef struct VkrDeferredDestroyQueue {
  VkrDeferredDestroyEntry entries[VKR_DEFERRED_DESTROY_QUEUE_SIZE];
  uint32_t head;  // Next slot to read from
  uint32_t tail;  // Next slot to write to
  uint32_t count; // Number of entries in queue
} VkrDeferredDestroyQueue;

// ============================================================================
// Pixel Readback System (for picking and screenshots)
// ============================================================================

#define VKR_READBACK_RING_SIZE                                                 \
  BUFFERING_FRAMES // Number of readback slots in flight

typedef enum VulkanReadbackSlotState {
  VULKAN_READBACK_SLOT_IDLE = 0, // Available for use
  VULKAN_READBACK_SLOT_PENDING,  // Copy command submitted, waiting for GPU
  VULKAN_READBACK_SLOT_READY,    // GPU done, data ready for CPU read
} VulkanReadbackSlotState;

typedef struct VulkanReadbackSlot {
  VulkanBuffer buffer;            // HOST_VISIBLE buffer for readback
  VulkanFence fence;              // Fence to track completion
  VulkanReadbackSlotState state;  // Current slot state
  uint32_t requested_x;           // Requested pixel X coordinate
  uint32_t requested_y;           // Requested pixel Y coordinate
  uint32_t width;                 // Width of copied region
  uint32_t height;                // Height of copied region
  uint32_t pixel_size;            // Size per pixel (e.g., 4 for R32_UINT)
  bool8_t is_coherent;            // True if memory is HOST_COHERENT
  uint32_t request_frame;         // Frame index when readback was requested
  uint64_t request_submit_serial; // Monotonic submit serial at request time
} VulkanReadbackSlot;

typedef struct VulkanReadbackRing {
  VulkanReadbackSlot slots[VKR_READBACK_RING_SIZE];
  uint32_t write_index;   // Next slot to use for requests
  uint32_t read_index;    // Oldest pending slot to check
  uint32_t pending_count; // Number of slots in PENDING state
  bool8_t initialized;    // True if ring has been initialized
} VulkanReadbackRing;

typedef struct VulkanRgTimingState {
  bool8_t supported;
  uint32_t query_capacity;
  VkQueryPool query_pools[BUFFERING_FRAMES];
  uint32_t frame_pass_counts[BUFFERING_FRAMES];
  uint64_t *query_results;
  uint32_t query_results_capacity;
  float64_t *last_pass_ms;
  bool8_t *last_pass_valid;
  uint32_t last_pass_capacity;
  uint32_t last_pass_count;
} VulkanRgTimingState;

/**
 * @brief Vulkan backend state containing all rendering resources and state
 *
 *
 * DOMAIN CONFIGURATIONS:
 * - WORLD: Color+Depth, finalLayout=COLOR_ATTACHMENT_OPTIMAL (chains to UI)
 * - UI: Color-only, loadOp=LOAD (preserves world), finalLayout=PRESENT_SRC_KHR
 * - SHADOW: Depth-only, for shadow map generation
 * - POST: Color-only, for post-processing effects
 */
typedef struct VulkanBackendState {
  Arena *arena;
  VkrAllocator alloc;
  Arena *temp_arena;
  VkrAllocator temp_scope;
  Arena *swapchain_arena;
  VkrAllocator swapchain_alloc;
  VkrWindow *window;
  VkrDeviceRequirements *device_requirements;

  VulkanAllocator vk_allocator;
  VkAllocationCallbacks *allocator;

  bool8_t is_swapchain_recreation_requested;

  float64_t frame_delta;
  uint64_t submit_serial;
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

  /** Tracks which domains have been initialized */
  bool8_t domain_initialized[VKR_PIPELINE_DOMAIN_COUNT];
  Array_VkrRenderPassEntry render_pass_registry;
  uint32_t render_pass_count;

  /**
   * Currently active render pass domain.
   * Set to VKR_PIPELINE_DOMAIN_COUNT when no pass is active.
   * Tracks the active pass domain for validation and state tracking.
   */
  VkrPipelineDomain current_render_pass_domain;

  struct s_RenderPass *active_named_render_pass;

  /**
   * Indicates if a render pass is currently recording.
   * - Set to false in begin_frame (no pass started)
   * - Set to true in begin_render_pass
   * - Set to false in end_render_pass/end_frame
   */
  bool8_t render_pass_active;
  bool8_t frame_active;

  uint32_t active_image_index;

  /**
   * Tracks if the swapchain image is in PRESENT_SRC_KHR layout.
   * Used to avoid redundant layout transitions in end_frame.
   * Set to true when UI or POST domain ends (transitions to PRESENT).
   */
  bool8_t swapchain_image_is_present_ready;

  struct s_TextureHandle **swapchain_image_textures;
  struct s_TextureHandle *depth_texture;
  struct s_TextureHandle
      *default_2d_texture; // Fallback for empty sampler slots
  struct s_BufferHandle *instance_buffer; // Per-frame instance data buffer

  void (*on_render_target_refresh_required)();

  VkSurfaceKHR surface;

  VulkanSwapchain swapchain;

  Array_VkSemaphore image_available_semaphores;
  Array_VkSemaphore queue_complete_semaphores;
  Array_VulkanFence in_flight_fences;
  Array_VulkanFencePtr images_in_flight;

  Array_VulkanCommandBuffer graphics_command_buffers;
  uint64_t descriptor_writes_avoided; // telemetry

  // Pixel readback system for picking and screenshots
  VulkanReadbackRing readback_ring;
  VulkanRgTimingState rg_timing;

  // Framebuffer cache for reusing framebuffers with same attachments
  VkrFramebufferCache framebuffer_cache;

  // Deferred destruction queue - delays resource destruction until GPU is done
  VkrDeferredDestroyQueue deferred_destroy_queue;

  // Resource handle pools - fixed-size allocators for texture/buffer handles.
  // Using pools instead of arena allows proper free on resource destroy.
  // Each pool has a corresponding VkrAllocator for tracking statistics.
  VkrPool texture_handle_pool;
  VkrPool buffer_handle_pool;
  VkrPool render_target_pool;
  VkrAllocator texture_pool_alloc;
  VkrAllocator buffer_pool_alloc;
  VkrAllocator render_target_alloc;

#ifndef NDEBUG
  // Debug: monotonic counter for texture liveness validation
  uint32_t texture_generation_counter;
#endif
} VulkanBackendState;
