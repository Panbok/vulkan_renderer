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

#ifndef VKR_VULKAN_PARALLEL_UPLOAD
#define VKR_VULKAN_PARALLEL_UPLOAD 1
#endif

#define VKR_VULKAN_PARALLEL_MAX_WORKERS 8

/**
 * Reflection failure categories used by the SPIR-V reflection pipeline.
 *
 * These codes are intended to be stable diagnostics that can be bubbled up to
 * frontend creation APIs and logs. They intentionally separate parse failures,
 * shader contract mismatches, and Vulkan limit violations so
 * call sites can report precise remediation hints.
 */
typedef enum VkrReflectionError {
  VKR_REFLECTION_OK = 0,
  VKR_REFLECTION_ERROR_PARSE_FAILED,
  VKR_REFLECTION_ERROR_DUPLICATE_STAGE,
  VKR_REFLECTION_ERROR_ENTRY_POINT_NOT_FOUND,
  VKR_REFLECTION_ERROR_STAGE_MISMATCH,
  VKR_REFLECTION_ERROR_BINDING_TYPE_MISMATCH,
  VKR_REFLECTION_ERROR_BINDING_COUNT_MISMATCH,
  VKR_REFLECTION_ERROR_BINDING_SIZE_MISMATCH,
  VKR_REFLECTION_ERROR_UNIFORM_LAYOUT_MISMATCH,
  VKR_REFLECTION_ERROR_UNSUPPORTED_DESCRIPTOR,
  VKR_REFLECTION_ERROR_RUNTIME_ARRAY,
  VKR_REFLECTION_ERROR_MISSING_LOCATION,
  VKR_REFLECTION_ERROR_VERTEX_COMPONENT_DECORATION,
  VKR_REFLECTION_ERROR_DUPLICATE_VERTEX_LOCATION,
  VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
  VKR_REFLECTION_ERROR_PUSH_CONSTANT_ALIGNMENT,
  VKR_REFLECTION_ERROR_PUSH_CONSTANT_LIMIT,
} VkrReflectionError;

#define VKR_REFLECTION_ERROR_PROGRAM_NAME_MAX 256
#define VKR_REFLECTION_ERROR_MODULE_PATH_MAX 512
#define VKR_REFLECTION_ERROR_ENTRY_POINT_MAX 128

/**
 * Reflection error context for deterministic diagnostics.
 *
 * `set`, `binding`, and `location` use `UINT32_MAX` when not applicable.
 * `backend_result` is the raw reflection-library result code (if any), kept as
 * `int32_t` to avoid leaking third-party types into call sites that only need
 * renderer-level diagnostics.
 *
 * String fields point into fixed storage in this struct to avoid lifetime bugs
 * when reflection backends release temporary parse state before diagnostics are
 * logged.
 */
typedef struct VkrReflectionErrorContext {
  VkrReflectionError code;
  String8 program_name;
  String8 module_path;
  String8 entry_point;
  uint8_t program_name_storage[VKR_REFLECTION_ERROR_PROGRAM_NAME_MAX];
  uint8_t module_path_storage[VKR_REFLECTION_ERROR_MODULE_PATH_MAX];
  uint8_t entry_point_storage[VKR_REFLECTION_ERROR_ENTRY_POINT_MAX];
  VkShaderStageFlagBits stage;
  uint32_t set;
  uint32_t binding;
  uint32_t location;
  int32_t backend_result;
} VkrReflectionErrorContext;

typedef struct VkrShaderStageModuleDesc {
  VkShaderStageFlagBits stage;
  String8 path;
  String8 entry_point;
  const uint8_t *spirv_bytes;
  uint64_t spirv_size;
} VkrShaderStageModuleDesc;

typedef struct VkrSpirvReflectionCreateInfo {
  VkrAllocator *allocator;
  VkrAllocator *temp_allocator;
  String8 program_name;
  VkrVertexAbiProfile vertex_abi_profile;
  uint32_t module_count;
  const VkrShaderStageModuleDesc *modules;
  uint32_t max_push_constant_size; // 0 disables push-constant limit validation
} VkrSpirvReflectionCreateInfo;

typedef struct VkrDescriptorBindingDesc {
  uint32_t binding;
  VkDescriptorType type;
  uint32_t count;
  uint32_t
      byte_size; // Buffer descriptor block size in bytes; 0 for non-buffers.
  VkShaderStageFlags stages;
  String8 name;
} VkrDescriptorBindingDesc;

typedef enum VkrDescriptorSetRole {
  VKR_DESCRIPTOR_SET_ROLE_NONE = 0,
  VKR_DESCRIPTOR_SET_ROLE_FRAME,
  VKR_DESCRIPTOR_SET_ROLE_MATERIAL,
  VKR_DESCRIPTOR_SET_ROLE_DRAW,
  VKR_DESCRIPTOR_SET_ROLE_FEATURE,
  VKR_DESCRIPTOR_SET_ROLE_COUNT
} VkrDescriptorSetRole;

typedef struct VkrDescriptorSetDesc {
  uint32_t set;
  VkrDescriptorSetRole role;
  uint32_t binding_count;
  VkrDescriptorBindingDesc *bindings;
} VkrDescriptorSetDesc;

typedef struct VkrPushConstantRangeDesc {
  uint32_t offset;
  uint32_t size;
  VkShaderStageFlags stages;
} VkrPushConstantRangeDesc;

typedef struct VkrVertexInputBindingDesc {
  uint32_t binding;
  uint32_t stride;
  VkVertexInputRate rate;
} VkrVertexInputBindingDesc;

typedef struct VkrVertexInputAttributeDesc {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
  String8 name;
} VkrVertexInputAttributeDesc;

typedef enum VkrUniformScalarKind {
  VKR_UNIFORM_SCALAR_KIND_UNKNOWN = 0,
  VKR_UNIFORM_SCALAR_KIND_FLOAT,
  VKR_UNIFORM_SCALAR_KIND_SINT,
  VKR_UNIFORM_SCALAR_KIND_UINT,
} VkrUniformScalarKind;

typedef struct VkrUniformMemberDesc {
  String8 name;
  uint32_t offset;
  uint32_t size;
  uint32_t array_stride;
  uint32_t array_count;
  uint32_t matrix_stride;
  uint32_t matrix_columns;
  uint32_t matrix_rows;
  uint32_t vector_component_count;
  uint32_t scalar_width;
  VkrUniformScalarKind scalar_kind;
} VkrUniformMemberDesc;

typedef struct VkrUniformBlockDesc {
  String8 name;
  uint32_t set;
  uint32_t binding;
  uint32_t size;
  uint32_t member_count;
  VkrUniformMemberDesc *members;
} VkrUniformBlockDesc;

typedef struct VkrShaderReflection {
  uint32_t set_count;         // Number of non-empty descriptor sets
  VkrDescriptorSetDesc *sets; // Sorted by set index
  uint32_t layout_set_count;  // max_set + 1 (includes sparse holes)

  uint32_t push_constant_range_count;
  VkrPushConstantRangeDesc *push_constant_ranges;

  uint32_t vertex_binding_count;
  VkrVertexInputBindingDesc *vertex_bindings;
  uint32_t vertex_attribute_count;
  VkrVertexInputAttributeDesc *vertex_attributes;

  uint32_t uniform_block_count;
  VkrUniformBlockDesc *uniform_blocks;
} VkrShaderReflection;

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
  VkrGpuAllocationOwner allocation_owner;

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
  uint64_t allocation_size;
  VkrGpuAllocationOwner allocation_owner;
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

typedef enum VulkanQueueSubmitRole {
  VULKAN_QUEUE_SUBMIT_ROLE_TRANSFER = 0,
  VULKAN_QUEUE_SUBMIT_ROLE_GRAPHICS_UPLOAD = 1,
  VULKAN_QUEUE_SUBMIT_ROLE_PRESENT = 2,
  VULKAN_QUEUE_SUBMIT_ROLE_COUNT = 3
} VulkanQueueSubmitRole;

typedef struct VulkanQueueSubmitState {
  VkrMutex mutexes[VULKAN_QUEUE_SUBMIT_ROLE_COUNT];
} VulkanQueueSubmitState;

typedef struct VulkanParallelWorkerContext {
  bool8_t initialized;
  VkCommandPool transfer_command_pool;
  VkCommandPool graphics_upload_command_pool;
} VulkanParallelWorkerContext;

typedef struct VulkanParallelRuntime {
  bool8_t enabled;
  VkrJobSystem *job_system;
  uint32_t worker_count;
  VulkanParallelWorkerContext workers[VKR_VULKAN_PARALLEL_MAX_WORKERS];
} VulkanParallelRuntime;

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
  VkFormat shadow_depth_format;
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
  VkColorSpaceKHR color_space;
  VkExtent2D extent;

  uint32_t image_count;
  uint8_t max_in_flight_frames;

  VulkanImage depth_attachment;

  Array_VkImage images;
  Array_VkImageView image_views;
  Array_VulkanFramebuffer framebuffers;
} VulkanSwapchain;

/**
 * Access/layout an offscreen target image pair carries between submissions.
 * Ordinary images are never acquired, so the state a frame leaves behind is
 * the state the next graph import must declare.
 */
typedef struct VulkanPresentTargetState {
  VkImageLayout color_layout;
  VkImageLayout depth_layout;
  VkrImageAccessFlags color_access;
  VkrImageAccessFlags depth_access;
} VulkanPresentTargetState;

/** One offscreen color/depth pair and the state it retains. */
typedef struct VulkanPresentTargetImage {
  VulkanImage color;
  VulkanImage depth;
  VulkanPresentTargetState state;
} VulkanPresentTargetImage;

Array(VulkanPresentTargetImage);

/**
 * Outcome of acquiring or completing a frame's target image.
 *
 * `SKIP` and `FAILED` must stay distinct: a resized surface produces no image
 * this frame but the device is fine, while collapsing them turns every window
 * resize into a fatal error.
 */
typedef enum VulkanPresentTargetResult {
  VULKAN_PRESENT_TARGET_RESULT_OK = 0,
  /** No image was produced this frame; the caller should skip and retry next.
   */
  VULKAN_PRESENT_TARGET_RESULT_SKIP,
  /** Unrecoverable device or surface error. */
  VULKAN_PRESENT_TARGET_RESULT_FAILED,
} VulkanPresentTargetResult;

/**
 * Submit synchronization a target requires for the frame it just handed out.
 * A target with no presentation engine to hand the image back to leaves both
 * handles null and submits with fences alone.
 */
typedef struct VulkanPresentTargetSync {
  VkSemaphore wait;
  VkSemaphore signal;
} VulkanPresentTargetSync;

typedef struct VulkanBackendState VulkanBackendState;

/**
 * One present-target implementation.
 *
 * The frame path asks the target what to do rather than asking which kind it
 * is: acquisition, submit synchronization, completion, cancellation, and
 * recovery policy all differ between a WSI swapchain and ordinary images, and
 * nothing else in the backend needs to know which of the two it holds.
 */
typedef struct VulkanPresentTargetOps {
  /** Name used in diagnostics. */
  const char *name;
  /**
   * Presented through a surface. Instance surface extensions, the swapchain
   * device extension, a present queue family, and a real present mode exist
   * only for these targets, and device selection runs before any target does.
   */
  bool8_t uses_wsi;
  /**
   * A failed frame is recovered by recreating the target. Windowed recreation
   * rebuilds the acquire semaphore a dead frame left signalled; a target that
   * owns no such semaphore has no equivalent recovery and retires the device.
   */
  bool8_t recovers_by_recreation;
  /** Access/layout the graph must leave the present image in. */
  VkrPresentTargetImageState terminal_state;

  /** Opens the target's images, views, and per-image state. */
  bool8_t (*create)(VulkanBackendState *state);
  /** Retires everything `create` opened. */
  void (*destroy)(VulkanBackendState *state);
  /** Reacts to a new surface extent; targets without a resize event ignore it.
   */
  void (*resize)(VulkanBackendState *state, uint32_t width, uint32_t height);

  /** Per-frame preamble run before the frame-slot fence wait. */
  VkrRendererError (*frame_begin)(VulkanBackendState *state);
  /**
   * Selects this frame's image into `state->image_index` and reports the
   * submit synchronization it needs.
   */
  VulkanPresentTargetResult (*acquire)(VulkanBackendState *state,
                                       VulkanPresentTargetSync *out_sync);
  /** Hands the submitted image to its presentation engine, if it has one. */
  VulkanPresentTargetResult (*complete)(VulkanBackendState *state);
  /**
   * Restores the image state a cancelled frame found and records whatever
   * transition the discarded image still owes.
   */
  void (*frame_cancel)(VulkanBackendState *state,
                       VulkanCommandBuffer *command_buffer);
} VulkanPresentTargetOps;

/** Private implementation storage for the selected present-target kind. */
typedef struct VulkanPresentTarget {
  VkrPresentTargetConfig config;
  /** Bound from `config.kind` before any Vulkan object is created. */
  const VulkanPresentTargetOps *ops;
  /** Offscreen only; windowed targets own their images through the swapchain.
   */
  Array_VulkanPresentTargetImage images;
  uint32_t next_offscreen_image;
} VulkanPresentTarget;

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
#define VULKAN_SHADER_OBJECT_MAX_INSTANCE_POOLS 8
typedef struct VulkanShaderObjectDescriptorState {
  // Per-frame descriptor generation tracking; length == frame_count
  uint32_t *generations;
  // Per-frame descriptor payload tracking used to detect handle changes even
  // when backend texture generations are reused across scene reloads.
  VkImageView *image_views;
  VkSampler *samplers;
} VulkanShaderObjectDescriptorState;

#define VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT 8192
typedef struct VulkanShaderObjectInstanceState {
  // Per-frame descriptor sets; length == frame_count
  VkDescriptorSet *descriptor_sets;
  VkDescriptorPool descriptor_pool;

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
  bool8_t has_reflection;
  VkrShaderReflection reflection;

  // Runtime set indices resolved from reflection roles/fallback conventions.
  uint32_t frame_set_index;
  uint32_t draw_set_index;

  // Resolved binding indices used by legacy frontend state upload paths.
  uint32_t frame_uniform_binding;
  uint32_t frame_instance_buffer_binding;
  uint32_t draw_uniform_binding;
  uint32_t draw_sampled_image_binding_base;
  uint32_t draw_sampler_binding_base;

  // Dynamic descriptor counts for zero-offset bind calls.
  uint32_t frame_dynamic_offset_count;
  uint32_t draw_dynamic_offset_count;

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
  VkDescriptorPool
      instance_descriptor_pools[VULKAN_SHADER_OBJECT_MAX_INSTANCE_POOLS];
  uint32_t instance_descriptor_pool_count;
  uint32_t instance_pool_instance_capacities
      [VULKAN_SHADER_OBJECT_MAX_INSTANCE_POOLS];
  uint32_t instance_pool_fallback_allocations;
  uint32_t instance_pool_overflow_creations;
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
#define VKR_RENDER_TARGET_MAX_ATTACHMENTS (VKR_MAX_COLOR_ATTACHMENTS * 2 + 1)

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
  // Captured texture generations at render target creation for liveness
  // validation
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

#define VKR_DEFERRED_DESTROY_QUEUE_SIZE 4096

typedef enum VkrDeferredDestroyKind {
  VKR_DEFERRED_DESTROY_FRAMEBUFFER = 0,
  VKR_DEFERRED_DESTROY_RENDERPASS,
  VKR_DEFERRED_DESTROY_IMAGE,
  VKR_DEFERRED_DESTROY_IMAGE_VIEW,
  VKR_DEFERRED_DESTROY_SAMPLER,
  VKR_DEFERRED_DESTROY_BUFFER,
  VKR_DEFERRED_DESTROY_COMMAND_SUBMISSION,
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
  uint64_t memory_size;  // Size used for allocator accounting (0 if unknown)
  VkrAllocatorMemoryTag memory_tag; // Accounting tag for memory_size
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

#define VKR_CAPTURE_RING_CAPACITY_MAX 8u

typedef enum VulkanCaptureSlotState {
  VULKAN_CAPTURE_SLOT_IDLE = 0,
  VULKAN_CAPTURE_SLOT_RESERVED,
  VULKAN_CAPTURE_SLOT_RECORDED,
  VULKAN_CAPTURE_SLOT_SUBMITTED,
  VULKAN_CAPTURE_SLOT_READY,
  VULKAN_CAPTURE_SLOT_ACQUIRED,
  VULKAN_CAPTURE_SLOT_FAILED,
  VULKAN_CAPTURE_SLOT_ABANDONED,
} VulkanCaptureSlotState;

typedef struct VulkanCaptureSlot {
  struct s_BufferHandle buffer;
  VulkanCaptureSlotState state;
  VkrCaptureRequestId request_id;
  VkrRendererError error;
  uint64_t source_frame_index;
  uint64_t submit_serial;
  uint32_t submit_frame_slot;
  uint32_t item_count;
  uint32_t recorded_count;
  uint64_t recorded_mask;
  VkrCaptureBackendItemPlan plans[VKR_CAPTURE_MAX_ITEMS];
  VkrCaptureItemResult results[VKR_CAPTURE_MAX_ITEMS];
} VulkanCaptureSlot;

typedef struct VulkanCaptureRing {
  VulkanCaptureSlot slots[VKR_CAPTURE_RING_CAPACITY_MAX];
  uint32_t capacity;
  uint64_t max_batch_bytes;
  VkrCaptureRequestId last_request_id;
  bool8_t initialized;
} VulkanCaptureRing;

typedef struct VulkanRgTimingState {
  bool8_t supported;
  uint32_t query_capacity;
  VkQueryPool query_pools[BUFFERING_FRAMES];
  uint32_t frame_pass_counts[BUFFERING_FRAMES];
  uint64_t frame_cpu_indices[BUFFERING_FRAMES];
  uint64_t *query_results;
  uint32_t query_results_capacity;
  float64_t *last_pass_ms;
  bool8_t *last_pass_valid;
  uint32_t last_pass_capacity;
  uint32_t last_pass_count;
  uint64_t last_source_frame_index;
  uint64_t last_source_submit_serial;
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
/** Bounded registry of live device allocations; see VulkanDeviceMemoryStats. */
#define VULKAN_DEVICE_MEMORY_TRACK_CAPACITY 8192

typedef struct VulkanDeviceMemoryEntry {
  VkDeviceMemory memory; /**< VK_NULL_HANDLE marks a free slot */
  uint64_t size;
  uint32_t memory_type_index;
  VkrGpuAllocationOwner owner;
} VulkanDeviceMemoryEntry;

/**
 * @brief Device-memory allocation telemetry.
 *
 * The renderer allocates one VkDeviceMemory per buffer/image/readback buffer.
 * Byte totals were already tracked through allocator tags, but allocation
 * *count*, per-memory-type distribution, and caller-declared logical owners
 * are retained here; those decide whether pooling is worth doing and what
 * block sizes it should use.
 *
 * Allocations are recorded in an open-addressed table keyed by the
 * VkDeviceMemory handle, so a free only needs the handle and the counters
 * cannot drift from the real state while tracking remains exact. If the table
 * saturates, the stats are marked inexact rather than presenting the remaining
 * live counters as authoritative.
 */
typedef struct VulkanDeviceMemoryStats {
  uint64_t live_allocation_count;
  uint64_t peak_allocation_count;
  uint64_t total_allocation_count; /**< Cumulative, never decremented */
  uint64_t live_bytes;
  uint64_t peak_bytes;
  uint64_t live_bytes_by_type[VK_MAX_MEMORY_TYPES];
  uint64_t live_count_by_type[VK_MAX_MEMORY_TYPES];
  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];
  VulkanDeviceMemoryEntry *entries; /**< Capacity-sized, zeroed on init */
  uint32_t entry_capacity;
  bool8_t tracking_exact; /**< False once the table has overflowed */
} VulkanDeviceMemoryStats;

typedef struct VulkanBackendState {
  Arena *arena;
  VkrAllocator alloc;
  Arena *temp_arena;
  VkrAllocator temp_scope;
  Arena *swapchain_arena;
  VkrAllocator swapchain_alloc;
  VkrWindow *window;
  // Thread that owns frame command recording and must perform frame-active
  // upload command emission.
  uint64_t render_thread_id;
  VkrDeviceRequirements *device_requirements;
  VkrPresentMode requested_present_mode;
  VkrPresentMode actual_present_mode;
  VulkanPresentTarget present_target;
  bool8_t capture_enabled;

  VulkanAllocator vk_allocator;
  VkAllocationCallbacks *allocator;

  bool8_t is_swapchain_recreation_requested;

  /**
   * Set when a frame consumed a swapchain acquire but never submitted, leaving
   * an acquire semaphore signalled with no waiter. The next begin_frame must
   * idle the device and recreate the swapchain -- which rebuilds the sync
   * objects -- before acquiring again, or vkAcquireNextImageKHR would be handed
   * an already-signalled semaphore.
   */
  bool8_t frame_recovery_required;

  /**
   * Set when swapchain/device recovery failed after WSI state may have been
   * partially rebuilt. No later frame or resize may touch that state; the
   * frontend must surface DEVICE_ERROR and shut the renderer down.
   */
  bool8_t device_unusable;

  float64_t frame_delta;
  uint64_t submit_serial;
  uint64_t completed_submit_serial;
  uint64_t frame_submit_serial[BUFFERING_FRAMES];
  // Debug counters for validating that async upload/mutation paths remain
  // non-blocking during streaming.
  uint64_t upload_path_fence_wait_count;
  uint64_t upload_path_queue_wait_idle_count;
  uint64_t upload_path_device_wait_idle_count;
  uint64_t last_present_duration_ns;
  bool8_t last_present_duration_valid;
  VkrMetricEventProducer pipeline_create_metrics;
  VkrMetricEventProducer shader_load_metrics;
  VkrMetricEventProducer shader_reflection_metrics;
  VulkanDeviceMemoryStats device_memory_stats;
  /** True when VK_EXT_memory_budget is enabled and heap usage can be queried.
   */
  bool8_t supports_memory_budget;
  uint32_t current_frame;
  uint32_t image_index;

  VkInstance instance;

#ifndef NDEBUG
  VkDebugUtilsMessengerEXT debug_messenger;
#endif

  VulkanDevice device;
  VulkanQueueSubmitState queue_submit_state;
  bool8_t parallel_upload_enabled;
  bool8_t parallel_upload_unsafe_enabled;
  VulkanParallelRuntime parallel_runtime;

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
  struct s_RenderTarget *active_named_render_target;

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
   * Layout of swapchain.images[image_index] as recorded so far this frame.
   *
   * UNDEFINED at begin_frame, because acquiring an image does not preserve its
   * contents. Updated in end_render_pass to the ended pass's final layout when
   * that pass targeted the acquired swapchain image. Consumed by end_frame and
   * cancel_frame to build the present transition.
   *
   * This must be tracked rather than assumed: a frame that ends without any
   * pass touching the swapchain image (a rejected packet, an aborted graph) has
   * it in UNDEFINED, and naming COLOR_ATTACHMENT_OPTIMAL as the barrier's
   * oldLayout there is undefined behaviour.
   */
  VkImageLayout swapchain_image_layout;

  /**
   * Submit synchronization the target handed out with this frame's image.
   * Written by acquire, consumed by the submit; both handles are null for a
   * target with no presentation engine.
   */
  VulkanPresentTargetSync frame_submit_sync;

  /**
   * Offscreen only: the state of the image pair this frame started from.
   * Cancellation restores it so a discarded frame leaves no phantom
   * transition behind for the next import.
   */
  VulkanPresentTargetState frame_target_initial;

  struct s_TextureHandle **swapchain_image_textures;
  /** One depth wrapper per target image; windowed entries share one image. */
  struct s_TextureHandle **depth_textures;
  struct s_TextureHandle
      *default_2d_texture;                // Fallback for empty sampler slots
  struct s_BufferHandle *instance_buffer; // Per-frame instance data buffer

  void (*on_render_target_refresh_required)();

  VkSurfaceKHR surface;

  VulkanSwapchain swapchain;
  VkPipelineCache pipeline_cache;
  String8 pipeline_cache_path;

  Array_VkSemaphore image_available_semaphores;
  Array_VkSemaphore queue_complete_semaphores;
  Array_VulkanFence in_flight_fences;
  Array_VulkanFencePtr images_in_flight;

  Array_VulkanCommandBuffer graphics_command_buffers;
  uint64_t descriptor_writes_avoided; // telemetry

  // Pixel readback system for picking and screenshots
  VulkanReadbackRing readback_ring;
  VulkanCaptureRing capture_ring;
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

/** The bound implementation. Valid from target selection until shutdown. */
vkr_internal INLINE const VulkanPresentTargetOps *
vulkan_present_target_ops(const VulkanBackendState *state) {
  return state->present_target.ops;
}

/**
 * True when the target is presented through a surface.
 *
 * Instance/device extension filtering, physical-device scoring, and queue
 * selection all run before a target is opened, so they ask this rather than
 * calling into the implementation.
 */
vkr_internal INLINE bool8_t
vulkan_present_target_uses_wsi(const VulkanBackendState *state) {
  return state && state->present_target.ops &&
         state->present_target.ops->uses_wsi;
}

/**
 * True when frames render into ordinary images.
 *
 * Only per-image state retention and reported provenance depend on this;
 * everything the frame path does differently goes through the ops table.
 */
vkr_internal INLINE bool8_t
vulkan_present_target_is_offscreen(const VulkanBackendState *state) {
  return state &&
         state->present_target.config.kind == VKR_PRESENT_TARGET_OFFSCREEN;
}

/** Layout the graph must leave the present image in. */
vkr_internal INLINE VkrTextureLayout
vulkan_present_target_terminal_layout(const VulkanBackendState *state) {
  return vulkan_present_target_ops(state)->terminal_state.layout;
}

/**
 * Allocator for the command buffers and fences rebuilt with the target.
 *
 * Explicit offscreen recreation resets the swapchain arena, so anything it
 * rebuilds must live there to be reclaimed. Swapchain recreation keeps that
 * arena, so its frame synchronization outlives an individual swapchain.
 */
vkr_internal INLINE VkrAllocator *
vulkan_present_target_frame_storage(VulkanBackendState *state) {
  return vulkan_present_target_uses_wsi(state) ? &state->alloc
                                               : &state->swapchain_alloc;
}

/**
 * Returns the active graphics command buffer for the current frame.
 * Must be called from the main render thread while image_index is stable.
 * Returns NULL if state is invalid or image_index is out of bounds.
 */
vkr_internal INLINE VulkanCommandBuffer *
vulkan_backend_get_active_graphics_command_buffer(VulkanBackendState *state) {
  if (!state) {
    return NULL;
  }

  if (state->image_index >= state->graphics_command_buffers.length) {
    return NULL;
  }

  return array_get_VulkanCommandBuffer(&state->graphics_command_buffers,
                                       state->image_index);
}
