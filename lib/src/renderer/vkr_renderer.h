#pragma once

#include "containers/bitset.h"
#include "core/event.h"
#include "core/vkr_job_system.h"
#include "core/vkr_metrics.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "renderer/systems/vkr_camera.h"

/* Public packet-renderer contracts shared by the selected Metal and Vulkan
   implementations. Resource publication and frame submission are coarse
   operations; API-specific command and pipeline objects stay private. */

// ============================================================================
// Forward Declarations & Opaque Handles
// ============================================================================

typedef struct s_RendererFrontend *VkrRendererFrontendHandle;
typedef struct s_BufferResource *VkrBufferHandle;
typedef struct s_TextureHandle *VkrTextureOpaqueHandle;
typedef struct VkrRenderPacket VkrRenderPacket;
typedef struct VkrValidationError VkrValidationError;
typedef struct VkrRendererFrameMetrics VkrRendererFrameMetrics;
typedef struct VkrRendererMetricsProducerConfig
    VkrRendererMetricsProducerConfig;
typedef struct VkrUiTextConfig VkrUiTextConfig;
typedef struct VkrText3DConfig VkrText3DConfig;

typedef enum VkrRendererBackendType {
  VKR_RENDERER_BACKEND_TYPE_VULKAN,
  VKR_RENDERER_BACKEND_TYPE_DX12, // Future
  VKR_RENDERER_BACKEND_TYPE_METAL,
  VKR_RENDERER_BACKEND_TYPE_COUNT
} VkrRendererBackendType;

typedef enum VkrRendererError {
  VKR_RENDERER_ERROR_NONE = 0,
  VKR_RENDERER_ERROR_UNKNOWN,
  VKR_RENDERER_ERROR_INITIALIZATION_FAILED,
  VKR_RENDERER_ERROR_BACKEND_NOT_SUPPORTED,
  VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED,
  VKR_RENDERER_ERROR_INVALID_HANDLE,
  VKR_RENDERER_ERROR_INVALID_PARAMETER,
  /** Packet input cannot be represented by the shipping renderer contract. */
  VKR_RENDERER_ERROR_UNSUPPORTED_INPUT,
  VKR_RENDERER_ERROR_SHADER_COMPILATION_FAILED,
  VKR_RENDERER_ERROR_OUT_OF_MEMORY,
  VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED,
  VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED,
  VKR_RENDERER_ERROR_PRESENTATION_FAILED,
  VKR_RENDERER_ERROR_FRAME_IN_PROGRESS,
  VKR_RENDERER_ERROR_DEVICE_ERROR,
  VKR_RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED,
  VKR_RENDERER_ERROR_FILE_NOT_FOUND,
  VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED,
  VKR_RENDERER_ERROR_INCOMPATIBLE_SIGNATURE,
  /**
   * Recoverable: this frame produced no output and no frame state is left
   * active. Raised when the swapchain was recreated mid-frame or the window has
   * a zero-sized extent. Callers should skip the frame and continue, not abort.
   */
  VKR_RENDERER_ERROR_FRAME_SKIPPED,
  /** Queue submission failed; the frame's work never reached the device. */
  VKR_RENDERER_ERROR_SUBMISSION_FAILED,
  /** Recoverable: every capture-batch slot is still owned by earlier work. */
  VKR_RENDERER_ERROR_CAPTURE_BUSY,
  /** A requested capture channel or subresource is unavailable. */
  VKR_RENDERER_ERROR_CAPTURE_UNAVAILABLE,

  VKR_RENDERER_ERROR_COUNT
} VkrRendererError;

typedef enum VkrShaderStageBits {
  VKR_SHADER_STAGE_NONE = 0,
  VKR_SHADER_STAGE_VERTEX_BIT = 1 << 0,
  VKR_SHADER_STAGE_FRAGMENT_BIT = 1 << 1,
  VKR_SHADER_STAGE_COMPUTE_BIT = 1 << 2,                 // Future
  VKR_SHADER_STAGE_GEOMETRY_BIT = 1 << 3,                // Future
  VKR_SHADER_STAGE_TESSELLATION_CONTROL_BIT = 1 << 4,    // Future
  VKR_SHADER_STAGE_TESSELLATION_EVALUATION_BIT = 1 << 5, // Future
  VKR_SHADER_STAGE_ALL_GRAPHICS = VKR_SHADER_STAGE_VERTEX_BIT |
                                  VKR_SHADER_STAGE_FRAGMENT_BIT |
                                  VKR_SHADER_STAGE_GEOMETRY_BIT |
                                  VKR_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VKR_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
} VkrShaderStageBits;
typedef Bitset8 VkrShaderStageFlags; // Assuming Bitset8 is sufficient for now

// ShaderStageFlags helper functions
vkr_internal INLINE VkrShaderStageFlags vkr_shader_stage_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrShaderStageFlags
vkr_shader_stage_flags_from_bits(uint8_t bits) {
  VkrShaderStageFlags flags = bitset8_create();
  if (bits & VKR_SHADER_STAGE_VERTEX_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_VERTEX_BIT);
  if (bits & VKR_SHADER_STAGE_FRAGMENT_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_FRAGMENT_BIT);
  if (bits & VKR_SHADER_STAGE_COMPUTE_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_COMPUTE_BIT);
  if (bits & VKR_SHADER_STAGE_GEOMETRY_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_GEOMETRY_BIT);
  if (bits & VKR_SHADER_STAGE_TESSELLATION_CONTROL_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
  if (bits & VKR_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
    bitset8_set(&flags, VKR_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
  return flags;
}

#define VKR_SHADER_STAGE_FLAGS_VERTEX_FRAGMENT()                               \
  vkr_shader_stage_flags_from_bits(VKR_SHADER_STAGE_VERTEX_BIT |               \
                                   VKR_SHADER_STAGE_FRAGMENT_BIT)

#define VKR_SHADER_STAGE_FLAGS_ALL_GRAPHICS()                                  \
  vkr_shader_stage_flags_from_bits(VKR_SHADER_STAGE_ALL_GRAPHICS)

typedef enum VkrBufferUsageBits {
  VKR_BUFFER_USAGE_NONE = 0,
  VKR_BUFFER_USAGE_VERTEX_BUFFER = 1 << 0,
  VKR_BUFFER_USAGE_INDEX_BUFFER = 1 << 1,
  VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER = 1 << 2,
  VKR_BUFFER_USAGE_UNIFORM = 1 << 3,
  VKR_BUFFER_USAGE_STORAGE = 1 << 4,      // For compute/more advanced
  VKR_BUFFER_USAGE_TRANSFER_SRC = 1 << 5, // Can be source of a copy
  VKR_BUFFER_USAGE_TRANSFER_DST = 1 << 6, // Can be destination of a copy
  VKR_BUFFER_USAGE_INDIRECT = 1 << 7,     // Indirect draw commands
} VkrBufferUsageBits;
typedef Bitset8 VkrBufferUsageFlags;

// BufferUsageFlags helper functions
vkr_internal INLINE VkrBufferUsageFlags vkr_buffer_usage_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrBufferUsageFlags
vkr_buffer_usage_flags_from_bits(uint8_t bits) {
  VkrBufferUsageFlags flags = bitset8_create();
  if (bits & VKR_BUFFER_USAGE_VERTEX_BUFFER)
    bitset8_set(&flags, VKR_BUFFER_USAGE_VERTEX_BUFFER);
  if (bits & VKR_BUFFER_USAGE_INDEX_BUFFER)
    bitset8_set(&flags, VKR_BUFFER_USAGE_INDEX_BUFFER);
  if (bits & VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER)
    bitset8_set(&flags, VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER);
  if (bits & VKR_BUFFER_USAGE_UNIFORM)
    bitset8_set(&flags, VKR_BUFFER_USAGE_UNIFORM);
  if (bits & VKR_BUFFER_USAGE_STORAGE)
    bitset8_set(&flags, VKR_BUFFER_USAGE_STORAGE);
  if (bits & VKR_BUFFER_USAGE_TRANSFER_SRC)
    bitset8_set(&flags, VKR_BUFFER_USAGE_TRANSFER_SRC);
  if (bits & VKR_BUFFER_USAGE_TRANSFER_DST)
    bitset8_set(&flags, VKR_BUFFER_USAGE_TRANSFER_DST);
  if (bits & VKR_BUFFER_USAGE_INDIRECT)
    bitset8_set(&flags, VKR_BUFFER_USAGE_INDIRECT);
  return flags;
}

#define VKR_BUFFER_USAGE_FLAGS_VERTEX()                                        \
  vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_VERTEX_BUFFER |            \
                                   VKR_BUFFER_USAGE_TRANSFER_DST)

#define VKR_BUFFER_USAGE_FLAGS_INDEX()                                         \
  vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_INDEX_BUFFER |             \
                                   VKR_BUFFER_USAGE_TRANSFER_DST)

#define VKR_BUFFER_USAGE_FLAGS_UNIFORM()                                       \
  vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_UNIFORM |                  \
                                   VKR_BUFFER_USAGE_TRANSFER_DST)

typedef enum VkrBufferAccessFlags {
  VKR_BUFFER_ACCESS_NONE = 0,
  VKR_BUFFER_ACCESS_VERTEX = 1 << 0,
  VKR_BUFFER_ACCESS_INDEX = 1 << 1,
  VKR_BUFFER_ACCESS_UNIFORM = 1 << 2,
  VKR_BUFFER_ACCESS_STORAGE_READ = 1 << 3,
  VKR_BUFFER_ACCESS_STORAGE_WRITE = 1 << 4,
  VKR_BUFFER_ACCESS_TRANSFER_SRC = 1 << 5,
  VKR_BUFFER_ACCESS_TRANSFER_DST = 1 << 6,
  VKR_BUFFER_ACCESS_INDIRECT_READ = 1 << 7,
} VkrBufferAccessFlags;

/**
 * @brief How an image is accessed by a pipeline stage.
 *
 * Mirrors VkrBufferAccessFlags. The render graph aliases this as
 * VkrRgImageAccessFlags so that declaration and backend share one vocabulary
 * and access masks survive the trip to the barrier. Combine flags for
 * read+write.
 */
typedef enum VkrImageAccessFlags {
  VKR_IMAGE_ACCESS_NONE = 0,
  VKR_IMAGE_ACCESS_SAMPLED = 1 << 0,
  VKR_IMAGE_ACCESS_STORAGE_READ = 1 << 1,
  VKR_IMAGE_ACCESS_STORAGE_WRITE = 1 << 2,
  VKR_IMAGE_ACCESS_COLOR_ATTACHMENT = 1 << 3,
  VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT = 1 << 4,
  VKR_IMAGE_ACCESS_DEPTH_READ_ONLY = 1 << 5,
  VKR_IMAGE_ACCESS_TRANSFER_SRC = 1 << 6,
  VKR_IMAGE_ACCESS_TRANSFER_DST = 1 << 7,
  VKR_IMAGE_ACCESS_PRESENT = 1 << 8,
} VkrImageAccessFlags;

/**
 * @brief Backend-neutral execution stages carried by graph dependencies.
 *
 * ALL_GRAPHICS is intentionally a first-class bit rather than an expansion to

 * * individual stages: default declarations use it as a conservative portable

 * * scope, while explicit declarations may select narrower
 * shader/fixed-function
 * scopes.
 */
typedef enum VkrGpuStageFlags {
  VKR_GPU_STAGE_NONE = 0,
  VKR_GPU_STAGE_ALL_GRAPHICS = 1 << 0,
  VKR_GPU_STAGE_DRAW_INDIRECT = 1 << 1,
  VKR_GPU_STAGE_VERTEX_INPUT = 1 << 2,
  VKR_GPU_STAGE_VERTEX_SHADER = 1 << 3,
  VKR_GPU_STAGE_FRAGMENT_SHADER = 1 << 4,
  VKR_GPU_STAGE_EARLY_DEPTH = 1 << 5,
  VKR_GPU_STAGE_LATE_DEPTH = 1 << 6,
  VKR_GPU_STAGE_COLOR_OUTPUT = 1 << 7,
  VKR_GPU_STAGE_COMPUTE_SHADER = 1 << 8,
  VKR_GPU_STAGE_TRANSFER = 1 << 9,
  VKR_GPU_STAGE_HOST = 1 << 10,
  VKR_GPU_STAGE_TOP = 1 << 11,
  VKR_GPU_STAGE_BOTTOM = 1 << 12,
} VkrGpuStageFlags;

/** Visibility work required in addition to the execution dependency. */
typedef enum VkrGpuVisibilityFlags {
  VKR_GPU_VISIBILITY_NONE = 0,
  VKR_GPU_VISIBILITY_DEVICE = 1 << 0,
  VKR_GPU_VISIBILITY_RESOURCE_ALIAS = 1 << 1,
} VkrGpuVisibilityFlags;

/** Canonical dependency lowered once by each backend. */
typedef struct VkrGpuDependency {
  VkrGpuStageFlags src_stages;
  VkrGpuStageFlags dst_stages;
  VkrGpuVisibilityFlags visibility;
} VkrGpuDependency;

/** Conservative portable scopes used by declarations without explicit stages.
 */
VkrGpuStageFlags vkr_gpu_stages_for_buffer_access(VkrBufferAccessFlags access,
                                                  bool8_t is_src);
VkrGpuStageFlags vkr_gpu_stages_for_image_access(VkrImageAccessFlags access,
                                                 bool8_t is_src);
VkrGpuDependency
vkr_gpu_buffer_dependency_default(VkrBufferAccessFlags src_access,
                                  VkrBufferAccessFlags dst_access);
VkrGpuDependency
vkr_gpu_image_dependency_default(VkrImageAccessFlags src_access,
                                 VkrImageAccessFlags dst_access);

/**
 * @brief Subresource span addressed by a barrier.
 *
 * A count of 0 means "through the last one", so a zeroed struct addresses the
 * whole image. Making the safe case the zero case matters: uses that never
 * declare a slice are zero-initialized and must keep meaning whole-image.
 */
typedef struct VkrImageSubresourceRange {
  uint32_t base_mip;
  uint32_t mip_count; /**< 0 == all remaining mips */
  uint32_t base_layer;
  uint32_t layer_count; /**< 0 == all remaining layers */
} VkrImageSubresourceRange;

/** @brief True when the access performs any write. */
vkr_internal INLINE bool8_t
vkr_image_access_is_write(VkrImageAccessFlags access) {
  return (access &
          (VKR_IMAGE_ACCESS_STORAGE_WRITE | VKR_IMAGE_ACCESS_COLOR_ATTACHMENT |
           VKR_IMAGE_ACCESS_DEPTH_ATTACHMENT |
           VKR_IMAGE_ACCESS_TRANSFER_DST)) != 0
             ? true_v
             : false_v;
}

/** @brief Resolves a range's "0 == remaining" counts against real extents. */
vkr_internal INLINE void vkr_image_subresource_range_resolve(
    const VkrImageSubresourceRange *range, uint32_t mip_levels, uint32_t layers,
    uint32_t *out_base_mip, uint32_t *out_mip_count, uint32_t *out_base_layer,
    uint32_t *out_layer_count) {
  if (mip_levels == 0) {
    mip_levels = 1;
  }
  if (layers == 0) {
    layers = 1;
  }

  uint32_t base_mip = range ? range->base_mip : 0;
  uint32_t base_layer = range ? range->base_layer : 0;
  if (base_mip >= mip_levels) {
    base_mip = mip_levels - 1;
  }
  if (base_layer >= layers) {
    base_layer = layers - 1;
  }

  uint32_t mip_count = range ? range->mip_count : 0;
  uint32_t layer_count = range ? range->layer_count : 0;
  if (mip_count == 0 || mip_count > mip_levels - base_mip) {
    mip_count = mip_levels - base_mip;
  }
  if (layer_count == 0 || layer_count > layers - base_layer) {
    layer_count = layers - base_layer;
  }

  *out_base_mip = base_mip;
  *out_mip_count = mip_count;
  *out_base_layer = base_layer;
  *out_layer_count = layer_count;
}

// ============================================================================
// Device Resources
// ============================================================================
typedef enum VkrDeviceTypeBits {
  VKR_DEVICE_TYPE_DISCRETE_BIT = 1 << 0,
  VKR_DEVICE_TYPE_INTEGRATED_BIT = 1 << 1,
  VKR_DEVICE_TYPE_VIRTUAL_BIT = 1 << 2,
  VKR_DEVICE_TYPE_CPU_BIT = 1 << 3,
} VkrDeviceTypeBits;
typedef Bitset8 VkrDeviceTypeFlags;

typedef enum VkrDeviceQueueBits {
  VKR_DEVICE_QUEUE_GRAPHICS_BIT = 1 << 0,
  VKR_DEVICE_QUEUE_COMPUTE_BIT = 1 << 1,
  VKR_DEVICE_QUEUE_TRANSFER_BIT = 1 << 2,
  VKR_DEVICE_QUEUE_SPARSE_BINDING_BIT = 1 << 3,
  VKR_DEVICE_QUEUE_PROTECTED_BIT = 1 << 4,
  VKR_DEVICE_QUEUE_PRESENT_BIT = 1 << 5,
} VkrDeviceQueueBits;
typedef Bitset8 VkrDeviceQueueFlags;

typedef enum VkrSamplerFilterBits {
  VKR_SAMPLER_FILTER_ANISOTROPIC_BIT = 1 << 0,
  VKR_SAMPLER_FILTER_LINEAR_BIT = 1 << 1,
} VkrSamplerFilterBits;
typedef Bitset8 VkrSamplerFilterFlags;

typedef struct VkrDeviceRequirements {
  VkrShaderStageFlags supported_stages;
  VkrDeviceQueueFlags supported_queues;
  VkrDeviceTypeFlags allowed_device_types;
  VkrSamplerFilterFlags supported_sampler_filters;
} VkrDeviceRequirements;

/** Requested/actual presentation mode at the backend-neutral seam. */
typedef enum VkrPresentMode {
  VKR_PRESENT_MODE_DEFAULT = 0,
  VKR_PRESENT_MODE_IMMEDIATE,
  VKR_PRESENT_MODE_FIFO,
  VKR_PRESENT_MODE_MAILBOX,
} VkrPresentMode;

/** Backend-neutral kind of image target rendered by a frame. */
typedef enum VkrPresentTargetKind {
  VKR_PRESENT_TARGET_WINDOWED = 0,
  VKR_PRESENT_TARGET_OFFSCREEN,
} VkrPresentTargetKind;

/**
 * Selected before backend creation. Windowed targets derive extent and image
 * count from WSI; offscreen targets use the validated/clamped configuration.
 */
typedef struct VkrPresentTargetConfig {
  VkrPresentTargetKind kind;
  uint32_t width;
  uint32_t height;
  uint32_t image_count;
} VkrPresentTargetConfig;

#define VKR_PRESENT_TARGET_MAX_IMAGES 8u

typedef enum VkrSurfaceColorFormat {
  VKR_SURFACE_COLOR_FORMAT_UNKNOWN = 0,
  VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB,
  VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB,
  VKR_SURFACE_COLOR_FORMAT_BGRA8_UNORM,
  VKR_SURFACE_COLOR_FORMAT_RGBA8_UNORM,
} VkrSurfaceColorFormat;

typedef enum VkrSurfaceColorSpace {
  VKR_SURFACE_COLOR_SPACE_UNKNOWN = 0,
  VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR,
} VkrSurfaceColorSpace;

typedef enum VkrSurfaceDepthFormat {
  VKR_SURFACE_DEPTH_FORMAT_UNKNOWN = 0,
  VKR_SURFACE_DEPTH_FORMAT_D16_UNORM,
  VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT,
  VKR_SURFACE_DEPTH_FORMAT_D24_UNORM_S8_UINT,
} VkrSurfaceDepthFormat;

typedef enum VkrWorldRendererTopology {
  VKR_WORLD_RENDERER_TOPOLOGY_UNKNOWN = 0,
  VKR_WORLD_RENDERER_TOPOLOGY_DEFERRED,
} VkrWorldRendererTopology;

typedef struct VkrDeviceInformation {
  String8 device_name;
  String8 vendor_name;
  String8 driver_version;
  String8 api_version;
  uint64_t vram_size;
  uint64_t vram_local_size;
  uint64_t vram_shared_size;
  VkrDeviceTypeFlags device_types;
  VkrDeviceQueueFlags device_queues;
  VkrSamplerFilterFlags sampler_filters;
  float64_t max_sampler_anisotropy;
  bool8_t supports_texture_astc_4x4;
  bool8_t supports_texture_bc7;
  bool8_t supports_texture_etc2;
  bool8_t supports_texture_bc5;
  /** EAC RG11: the only compressed two-channel target on ETC2-class GPUs. */
  bool8_t supports_texture_eac_rg11;
  /** Exact RGBA16F source/cube combinations required by runtime HDR IBL. */
  bool8_t supports_hdr_ibl;
  uint32_t hdr_ibl_max_cube_extent;
  uint32_t hdr_ibl_max_mip_levels;
  bool8_t supports_multi_draw_indirect;
  bool8_t supports_draw_indirect_first_instance;
  uint32_t vendor_id;
  uint32_t device_id;
  VkrPresentTargetKind actual_target_kind;
  VkrPresentMode actual_present_mode;
  uint32_t actual_target_image_count;
  uint32_t actual_target_width;
  uint32_t actual_target_height;
  VkrSurfaceColorFormat actual_color_format;
  VkrSurfaceDepthFormat actual_depth_format;
  VkrSurfaceColorSpace actual_color_space;
  VkrWorldRendererTopology actual_world_renderer_topology;
} VkrDeviceInformation;

// ============================================================================
// Resource Descriptions
// ============================================================================

/**
 * @brief Logical owner supplied when a renderer resource is created.
 *
 * This is allocation telemetry, not a Vulkan memory classification. The value
 * travels into the device-allocation tracker (invalid values become UNKNOWN);
 * it must never be inferred from a debug name, usage flags, or memory type.
 */
typedef enum VkrGpuAllocationOwner {
  VKR_GPU_ALLOCATION_OWNER_UNKNOWN = 0,
  VKR_GPU_ALLOCATION_OWNER_MESH,
  VKR_GPU_ALLOCATION_OWNER_TEXTURE,
  VKR_GPU_ALLOCATION_OWNER_FONT,
  VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH,
  VKR_GPU_ALLOCATION_OWNER_SHADER,
  VKR_GPU_ALLOCATION_OWNER_INSTANCE,
  VKR_GPU_ALLOCATION_OWNER_INDIRECT,
  VKR_GPU_ALLOCATION_OWNER_STAGING,
  VKR_GPU_ALLOCATION_OWNER_READBACK,
  VKR_GPU_ALLOCATION_OWNER_SWAPCHAIN,
  VKR_GPU_ALLOCATION_OWNER_COUNT,
} VkrGpuAllocationOwner;

/**
 * @brief Coerces a caller-supplied owner into a reportable bucket.
 *
 * An out-of-range value becomes UNKNOWN rather than indexing past the owner
 * arrays. UNKNOWN is first-class in every report, so a missed or external
 * caller stays visible instead of being folded into a real owner's row.
 */
vkr_internal INLINE VkrGpuAllocationOwner
vkr_gpu_allocation_owner_normalize(VkrGpuAllocationOwner owner) {
  return owner < VKR_GPU_ALLOCATION_OWNER_COUNT
             ? owner
             : VKR_GPU_ALLOCATION_OWNER_UNKNOWN;
}

/**
 * @brief Device-memory accounting for one logical owner bucket.
 *
 * Kept as one struct per owner rather than parallel per-statistic arrays: an
 * allocation or free updates exactly one of these, so the six counters share a
 * cache line, and a new statistic is one field instead of one array in each of
 * the backend tracker and the public snapshot.
 *
 * `total_*` are cumulative and stay exact even after the handle table
 * saturates. `live_*` and `peak_*` are derived from that table and drift once
 * it does, which `VkrDeviceMemoryStats.live_totals_exact` reports.
 */
typedef struct VkrGpuAllocationOwnerTotals {
  uint64_t live_bytes;
  uint64_t peak_bytes;
  uint64_t total_bytes;
  uint64_t live_allocation_count;
  uint64_t peak_allocation_count;
  uint64_t total_allocation_count;
} VkrGpuAllocationOwnerTotals;

typedef enum VkrTextureType {
  VKR_TEXTURE_TYPE_2D = 0,
  VKR_TEXTURE_TYPE_CUBE_MAP = 1,
  VKR_TEXTURE_TYPE_2D_ARRAY = 2,
  VKR_TEXTURE_TYPE_CUBE_MAP_ARRAY = 3,
  VKR_TEXTURE_TYPE_COUNT,
} VkrTextureType;

typedef enum VkrTextureFormat {
  // RGBA formats
  VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
  VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
  VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM,
  VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB,
  VKR_TEXTURE_FORMAT_R8G8B8A8_UINT,
  VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM,
  VKR_TEXTURE_FORMAT_R8G8B8A8_SINT,
  VKR_TEXTURE_FORMAT_BC7_UNORM,
  VKR_TEXTURE_FORMAT_BC7_SRGB,
  VKR_TEXTURE_FORMAT_BC5_UNORM,
  VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM,
  VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB,
  VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM,
  VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB,
  /** Two-channel EAC; the only compressed RG target on ETC2-class devices. */
  VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM,
  /** Linear half-float RGBA used by HDR environments, IBL, and scene color. */
  VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
  // Single/dual channel formats
  VKR_TEXTURE_FORMAT_R8_UNORM,
  VKR_TEXTURE_FORMAT_R16_SFLOAT,
  VKR_TEXTURE_FORMAT_R32_SFLOAT,
  VKR_TEXTURE_FORMAT_R32_UINT,
  VKR_TEXTURE_FORMAT_R8G8_UNORM,
  // Depth/stencil formats
  VKR_TEXTURE_FORMAT_D16_UNORM,
  VKR_TEXTURE_FORMAT_D32_SFLOAT,
  VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT,
  /** Two-channel visibility-buffer payload: visible row plus primitive ID. */
  VKR_TEXTURE_FORMAT_R32G32_UINT,
  /** Two-channel signed-normalized octahedral world normal. */
  VKR_TEXTURE_FORMAT_R16G16_SNORM,

  VKR_TEXTURE_FORMAT_COUNT,
} VkrTextureFormat;

/** Backend-independent storage metadata for one texture format. */
typedef struct VkrTextureFormatInfo {
  uint8_t channel_count;
  uint8_t block_width;
  uint8_t block_height;
  uint8_t bytes_per_block;
  bool8_t is_block_compressed;
  bool8_t is_depth_stencil;
} VkrTextureFormatInfo;

/** Returns false for an invalid/unsupported enum value. */
bool8_t vkr_texture_format_get_info(VkrTextureFormat format,
                                    VkrTextureFormatInfo *out_info);

/** Exact bytes occupied by a tightly packed 2D mip/layer region. */
uint64_t vkr_texture_format_region_size(VkrTextureFormat format, uint32_t width,
                                        uint32_t height);

#define VKR_CAPTURE_MAX_ITEMS 16u
#define VKR_CAPTURE_CHANNEL_INVALID UINT16_MAX

/**
 * Published batch-buffer layout contract. The frontend lays every item out at
 * `VKR_CAPTURE_BUFFER_ALIGNMENT` and no canonical source texel exceeds
 * `VKR_CAPTURE_MAX_BYTES_PER_PIXEL`, so a caller that must size the backend
 * ring before the renderer exists can compute the same upper bound instead of
 * re-deriving one that silently drifts out of agreement.
 */
#define VKR_CAPTURE_BUFFER_ALIGNMENT 256u
#define VKR_CAPTURE_MAX_BYTES_PER_PIXEL 8u

typedef uint16_t VkrCaptureChannelId;
typedef uint64_t VkrCaptureRequestId;

typedef enum VkrCaptureValueKind {
  VKR_CAPTURE_VALUE_COLOR = 0,
  VKR_CAPTURE_VALUE_DEPTH,
  VKR_CAPTURE_VALUE_UINT,
} VkrCaptureValueKind;

typedef enum VkrCaptureColorSpace {
  VKR_CAPTURE_COLOR_SPACE_NONE = 0,
  VKR_CAPTURE_COLOR_SPACE_LINEAR,
  VKR_CAPTURE_COLOR_SPACE_SRGB,
} VkrCaptureColorSpace;

typedef enum VkrCaptureOrigin {
  VKR_CAPTURE_ORIGIN_TOP_LEFT = 0,
  VKR_CAPTURE_ORIGIN_BOTTOM_LEFT,
} VkrCaptureOrigin;

typedef enum VkrCaptureStatus {
  VKR_CAPTURE_STATUS_NOT_FOUND = 0,
  VKR_CAPTURE_STATUS_PENDING,
  VKR_CAPTURE_STATUS_READY,
  VKR_CAPTURE_STATUS_FAILED,
} VkrCaptureStatus;

typedef enum VkrCaptureAspect {
  VKR_CAPTURE_ASPECT_COLOR = 0,
  VKR_CAPTURE_ASPECT_DEPTH,
} VkrCaptureAspect;

typedef struct VkrCaptureItemRequest {
  VkrCaptureChannelId channel;
  uint32_t mip;
  uint32_t layer;
} VkrCaptureItemRequest;

typedef struct VkrCaptureBatchRequest {
  VkrCaptureRequestId request_id;
  const VkrCaptureItemRequest *items;
  uint32_t item_count;
} VkrCaptureBatchRequest;

typedef struct VkrCaptureItemResult {
  VkrCaptureChannelId channel;
  /** Resolved graph resource copied by this item (not a channel alias). */
  char producer_resource[64];
  uint32_t width;
  uint32_t height;
  uint64_t row_pitch;
  VkrTextureFormat format;
  VkrCaptureValueKind value_kind;
  VkrCaptureColorSpace color_space;
  VkrCaptureOrigin origin;
  const void *data;
  uint64_t data_size;
  uint32_t mip;
  uint32_t layer;
  /** Manual exposure used when canonicalizing an HDR color source. */
  float32_t display_exposure;
} VkrCaptureItemResult;

typedef struct VkrCapturePollResult {
  VkrCaptureStatus status;
  VkrRendererError error;
  const VkrCaptureItemResult *items;
  uint32_t item_count;
  uint64_t source_frame_index;
  uint64_t submit_serial;
} VkrCapturePollResult;

/** Stable direct-capture catalog entry. `source_name` names a graph resource.
 */
typedef struct VkrCaptureChannelDescription {
  VkrCaptureChannelId id;
  const char *name;
  const char *source_name;
  uint32_t required_subsystem; /**< VkrRendererSubsystem, or COUNT for none. */
  VkrCaptureAspect aspect;
  VkrCaptureValueKind value_kind;
  VkrCaptureColorSpace color_space;
  const char *canonical_encoding;
  uint32_t version;
} VkrCaptureChannelDescription;

/** Frontend-computed immutable layout reserved by the backend before mutation.
 */
typedef struct VkrCaptureBackendItemPlan {
  VkrCaptureItemResult result;
  uint64_t buffer_offset;
} VkrCaptureBackendItemPlan;

typedef enum VkrTextureUsageBits {
  VKR_TEXTURE_USAGE_NONE = 0,
  VKR_TEXTURE_USAGE_SAMPLED = 1 << 0,
  VKR_TEXTURE_USAGE_COLOR_ATTACHMENT = 1 << 1,
  VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT = 1 << 2,
  VKR_TEXTURE_USAGE_TRANSFER_SRC = 1 << 3,
  VKR_TEXTURE_USAGE_TRANSFER_DST = 1 << 4,
  VKR_TEXTURE_USAGE_STORAGE = 1 << 5,
} VkrTextureUsageBits;
typedef Bitset8 VkrTextureUsageFlags;

vkr_internal INLINE VkrTextureUsageFlags vkr_texture_usage_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrTextureUsageFlags
vkr_texture_usage_flags_from_bits(uint8_t bits) {
  VkrTextureUsageFlags flags = bitset8_create();
  if (bits & VKR_TEXTURE_USAGE_SAMPLED)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_SAMPLED);
  if (bits & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_COLOR_ATTACHMENT);
  if (bits & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT);
  if (bits & VKR_TEXTURE_USAGE_TRANSFER_SRC)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_TRANSFER_SRC);
  if (bits & VKR_TEXTURE_USAGE_TRANSFER_DST)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_TRANSFER_DST);
  if (bits & VKR_TEXTURE_USAGE_STORAGE)
    bitset8_set(&flags, VKR_TEXTURE_USAGE_STORAGE);
  return flags;
}

typedef enum VkrTextureLayout {
  VKR_TEXTURE_LAYOUT_UNDEFINED = 0,
  VKR_TEXTURE_LAYOUT_GENERAL,
  VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
  VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL,
  VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,

  // Legacy aliases for backward compatibility
  VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY =
      VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT =
      VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT =
      VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_SRC = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_DST = VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL,
} VkrTextureLayout;

typedef enum VkrTexturePropertyBits {
  VKR_TEXTURE_PROPERTY_WRITABLE_BIT = 1 << 0,
  VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT = 1 << 1,
  VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT = 1 << 2,
  // Backend handle is owned externally; the texture system must not destroy it.
  VKR_TEXTURE_PROPERTY_EXTERNAL_BIT = 1 << 3,
} VkrTexturePropertyBits;
typedef Bitset8 VkrTexturePropertyFlags;

vkr_internal INLINE VkrTexturePropertyFlags
vkr_texture_property_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrTexturePropertyFlags
vkr_texture_property_flags_from_bits(uint8_t bits) {
  VkrTexturePropertyFlags flags = bitset8_create();
  if (bits & VKR_TEXTURE_PROPERTY_WRITABLE_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_WRITABLE_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_EXTERNAL_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_EXTERNAL_BIT);
  return flags;
}

typedef enum VkrTextureRepeatMode {
  VKR_TEXTURE_REPEAT_MODE_REPEAT = 0,
  VKR_TEXTURE_REPEAT_MODE_MIRRORED_REPEAT = 1,
  VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE = 2,
  VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER = 3,
  VKR_TEXTURE_REPEAT_MODE_COUNT,
} VkrTextureRepeatMode;

typedef enum VkrFilter {
  VKR_FILTER_NEAREST = 0,
  VKR_FILTER_LINEAR = 1,
  VKR_FILTER_COUNT,
} VkrFilter;

typedef enum VkrMipFilter {
  VKR_MIP_FILTER_NONE = 0,    // sample base level only
  VKR_MIP_FILTER_NEAREST = 1, // nearest mip selection
  VKR_MIP_FILTER_LINEAR = 2,  // linear mip interpolation (trilinear)
  VKR_MIP_FILTER_COUNT,
} VkrMipFilter;

typedef enum VkrSampleCount {
  VKR_SAMPLE_COUNT_1 = 1,
  VKR_SAMPLE_COUNT_2 = 2,
  VKR_SAMPLE_COUNT_4 = 4,
  VKR_SAMPLE_COUNT_8 = 8,
  VKR_SAMPLE_COUNT_16 = 16,
  VKR_SAMPLE_COUNT_32 = 32,
  VKR_SAMPLE_COUNT_64 = 64,
} VkrSampleCount;

typedef struct VkrTextureDescription {
  uint32_t id;
  uint32_t width;
  uint32_t height;
  uint32_t generation;
  uint32_t channels;
  uint32_t mip_levels;
  uint32_t array_layers;

  VkrTextureType type;
  VkrTextureFormat format;
  VkrGpuAllocationOwner allocation_owner;
  VkrSampleCount
      sample_count; // MSAA sample count (default: VKR_SAMPLE_COUNT_1)
  VkrTexturePropertyFlags properties;

  VkrTextureRepeatMode u_repeat_mode;
  VkrTextureRepeatMode v_repeat_mode;
  VkrTextureRepeatMode w_repeat_mode;

  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  bool8_t anisotropy_enable;
} VkrTextureDescription;

typedef struct VkrTextureUploadRegion {
  uint32_t mip_level;
  uint32_t array_layer;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint64_t byte_offset;
  uint64_t byte_size;
} VkrTextureUploadRegion;

typedef enum VkrRenderMode {
  VKR_RENDER_MODE_DEFAULT = 0,
  VKR_RENDER_MODE_LIGHTING = 1,
  VKR_RENDER_MODE_NORMAL = 2,
  VKR_RENDER_MODE_UNLIT = 3,
  VKR_RENDER_MODE_DIRECT_DIFFUSE = 4,
  VKR_RENDER_MODE_DIRECT_SPECULAR = 5,
  VKR_RENDER_MODE_MATERIAL_PARAMS = 6,
  VKR_RENDER_MODE_COUNT,
} VkrRenderMode;

typedef struct VkrGlobalMaterialState {
  Mat4 projection;
  Mat4 view;
  Mat4 ui_projection;
  Mat4 ui_view;
  Vec4 ambient_color;
  Vec3 view_position;
  float32_t exposure;
  VkrRenderMode render_mode;
} VkrGlobalMaterialState;

// =============================================================================
// Text
// =============================================================================
typedef enum VkrUiTextAnchor {
  VKR_UI_TEXT_ANCHOR_TOP_LEFT = 0,
  VKR_UI_TEXT_ANCHOR_TOP_RIGHT,
  VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT,
  VKR_UI_TEXT_ANCHOR_BOTTOM_RIGHT,
} VkrUiTextAnchor;

typedef struct VkrUiTextCreateData {
  uint32_t text_id;
  String8 content;
  const VkrUiTextConfig *config; // Optional; NULL uses defaults
  VkrUiTextAnchor anchor;
  Vec2 padding;
} VkrUiTextCreateData;

typedef struct VkrWorldTextCreateData {
  uint32_t text_id;
  String8 content;
  const VkrText3DConfig *config; // Optional; NULL uses defaults
  VkrTransform transform;
} VkrWorldTextCreateData;

typedef enum VkrPipelineDomain {
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  VKR_PIPELINE_DOMAIN_SHADOW = 2,
  VKR_PIPELINE_DOMAIN_POST = 3,
  VKR_PIPELINE_DOMAIN_COMPUTE = 4,
  VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT = 5,
  VKR_PIPELINE_DOMAIN_SKYBOX = 6,
  VKR_PIPELINE_DOMAIN_PICKING = 7,
  // Picking variant for transparent drawables: depth-tested but does not write
  // depth to match the visible transparent render path.
  VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT = 8,
  VKR_PIPELINE_DOMAIN_WORLD_OVERLAY = 9,
  VKR_PIPELINE_DOMAIN_PICKING_OVERLAY = 10,

  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;

typedef enum VkrAttachmentLoadOp {
  VKR_ATTACHMENT_LOAD_OP_LOAD = 0,
  VKR_ATTACHMENT_LOAD_OP_CLEAR,
  VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
} VkrAttachmentLoadOp;

typedef enum VkrAttachmentStoreOp {
  VKR_ATTACHMENT_STORE_OP_STORE = 0,
  VKR_ATTACHMENT_STORE_OP_DONT_CARE,
} VkrAttachmentStoreOp;

typedef union VkrClearValue {
  struct {
    float32_t r, g, b, a;
  } color_f32;
  struct {
    uint32_t r, g, b, a;
  } color_u32;
  struct {
    float32_t depth;
    uint32_t stencil;
  } depth_stencil;
} VkrClearValue;

typedef struct VkrViewport {
  float32_t x;
  float32_t y;
  float32_t width;
  float32_t height;
  float32_t min_depth;
  float32_t max_depth;
} VkrViewport;

/** Initialization timings retained for publication after metrics begin. */
typedef struct VkrRendererBootMetrics {
  uint64_t instance_ns;
  uint64_t device_ns;
  uint64_t target_ns;
  uint64_t systems_ns;
  uint64_t graph_ns;
} VkrRendererBootMetrics;

typedef enum VkrBootProfile {
  VKR_BOOT_PROFILE_FULL = 0,
  VKR_BOOT_PROFILE_AUTOMATION,
} VkrBootProfile;

/**
 * Stable initialization units used by dependency-resolved boot plans.
 *
 * The mandatory units come first and always initialize; the optional units
 * follow `VKR_RENDERER_SUBSYSTEM_WORLD` and are the only ones a plan may omit.
 * A new unit therefore belongs on the side of that boundary that matches
 * whether `vkr_renderer_systems_initialize()` actually gates it.
 */
typedef enum VkrRendererSubsystem {
  VKR_RENDERER_SUBSYSTEM_CAMERA = 0,
  VKR_RENDERER_SUBSYSTEM_RENDER_GRAPH,
  VKR_RENDERER_SUBSYSTEM_FRAME_STREAMS,
  VKR_RENDERER_SUBSYSTEM_RESOURCES,
  VKR_RENDERER_SUBSYSTEM_GEOMETRY,
  VKR_RENDERER_SUBSYSTEM_TEXTURES,
  VKR_RENDERER_SUBSYSTEM_MATERIALS,
  VKR_RENDERER_SUBSYSTEM_MESHES,
  VKR_RENDERER_SUBSYSTEM_FONTS,
  VKR_RENDERER_SUBSYSTEM_LIGHTING,
  VKR_RENDERER_SUBSYSTEM_SHADOWS,
  VKR_RENDERER_SUBSYSTEM_WORLD,
  VKR_RENDERER_SUBSYSTEM_UI,
  VKR_RENDERER_SUBSYSTEM_SKYBOX,
  VKR_RENDERER_SUBSYSTEM_EDITOR,
  VKR_RENDERER_SUBSYSTEM_GIZMO,
  VKR_RENDERER_SUBSYSTEM_PICKING,
  VKR_RENDERER_SUBSYSTEM_COUNT,
} VkrRendererSubsystem;

typedef uint64_t VkrSubsystemMask;

#define VKR_RENDERER_SUBSYSTEM_BIT(SUBSYSTEM)                                  \
  ((VkrSubsystemMask)1u << (uint32_t)(SUBSYSTEM))
#define VKR_RENDERER_SUBSYSTEM_ALL                                             \
  (VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_COUNT) - 1u)
/** Units every plan contains; no boot profile can omit them. */
#define VKR_RENDERER_SUBSYSTEM_MANDATORY                                       \
  (VKR_RENDERER_SUBSYSTEM_BIT(VKR_RENDERER_SUBSYSTEM_UI) - 1u)
/** The complement: the only units a workload may leave out. */
#define VKR_RENDERER_SUBSYSTEM_OPTIONAL                                        \
  (VKR_RENDERER_SUBSYSTEM_ALL & ~VKR_RENDERER_SUBSYSTEM_MANDATORY)

/* The two masks are derived from the enum order, so a unit inserted on the
   wrong side of the boundary would silently become excludable — or stop being
   excludable — without touching either definition. */
_Static_assert(VKR_RENDERER_SUBSYSTEM_UI == VKR_RENDERER_SUBSYSTEM_WORLD + 1,
               "Optional subsystems must directly follow the mandatory ones");
_Static_assert(VKR_RENDERER_SUBSYSTEM_COUNT ==
                   VKR_RENDERER_SUBSYSTEM_PICKING + 1,
               "Picking must remain the last optional subsystem");

/**
 * One immutable initialization contract. `requested_mask` describes workload
 * needs; `effective_mask` is their transitive dependency closure.
 */
typedef struct VkrSubsystemPlan {
  VkrBootProfile profile;
  VkrSubsystemMask requested_mask;
  VkrSubsystemMask excluded_mask;
  VkrSubsystemMask effective_mask;
} VkrSubsystemPlan;

/**
 * Builds and validates a plan before any frontend subsystem is initialized.
 * Dependencies that intersect `excluded_mask` make the request impossible.
 *
 * Callers describe intent only: `effective_mask` is always computed here, so a
 * zero-initialized request under `VKR_BOOT_PROFILE_FULL` resolves to
 * `VKR_RENDERER_SUBSYSTEM_ALL`.
 *
 * @param out_error Optional; receives the rejection reason when one is wanted.
 */
bool8_t vkr_renderer_subsystem_plan_build(VkrBootProfile profile,
                                          VkrSubsystemMask requested_mask,
                                          VkrSubsystemMask excluded_mask,
                                          VkrSubsystemPlan *out_plan,
                                          VkrRendererError *out_error);

bool8_t vkr_renderer_subsystem_plan_includes(const VkrSubsystemPlan *plan,
                                             VkrRendererSubsystem subsystem);

typedef struct VkrRendererBackendConfig {
  const char *application_name;
  VkrRendererBootMetrics *boot_metrics;
  VkrPresentTargetConfig present_target;
  VkrPresentMode requested_present_mode;
  bool8_t capture_enabled;
  /** Diagnostic-only API validation. Never part of a performance profile. */
  bool8_t validation_enabled;
  /** Diagnostic shader instrumentation; implies validation_enabled. */
  bool8_t gpu_assisted_validation;
  uint32_t capture_ring_capacity;
  uint64_t capture_max_batch_bytes;
} VkrRendererBackendConfig;

typedef enum VkrPresentTargetAttachment {
  VKR_PRESENT_TARGET_ATTACHMENT_COLOR = 0,
  VKR_PRESENT_TARGET_ATTACHMENT_DEPTH,
} VkrPresentTargetAttachment;

/** State retained by one imported target image between submissions. */
typedef struct VkrPresentTargetImageState {
  VkrImageAccessFlags access;
  VkrTextureLayout layout;
} VkrPresentTargetImageState;

// ============================================================================
// Stateless Frame Setup
// ============================================================================
/**
 * Proven retained state for the selected physical shadow-map instance.
 *
 * `valid_layer_mask` is authoritative for content existence. The shadow
 * system associates fits and caster signatures with `resource_generation`, but
 * never promotes its own history to content validity.
 */
typedef struct VkrRetainedShadowToken {
  uint64_t resource_generation;
  uint32_t valid_layer_mask;
} VkrRetainedShadowToken;

typedef struct VkrFrameSetup {
  uint32_t image_index;
  uint32_t window_width;
  uint32_t window_height;
  VkrTextureFormat swapchain_format;
  VkrTextureFormat swapchain_depth_format;
  VkrRetainedShadowToken retained_shadow;
} VkrFrameSetup;

typedef struct VkrRendererUploadWaitStats {
  /**
   * Legacy metric name for a blocking upload-completion wait. Backends
   * may use
   * an equivalent explicit completion primitive, such as a
   * timeline semaphore.
   */
  uint64_t fence_wait_count;
  uint64_t queue_wait_idle_count;
  uint64_t device_wait_idle_count;
  /** Frame-upload allocation failures observed since the previous read. */
  uint64_t frame_upload_exhaustion_count;
} VkrRendererUploadWaitStats;

/** Maximum device memory types/heaps reported; matches VK_MAX_MEMORY_TYPES. */
#define VKR_DEVICE_MEMORY_TYPE_MAX 32
#define VKR_DEVICE_MEMORY_HEAP_MAX 16

/**
 * @brief Device-memory allocation telemetry.
 *
 * The renderer makes one device allocation per buffer, image, and readback
 * buffer. These are the numbers that decide whether block pooling is worth
 * doing and what block size it should use, which is why they are captured
 * before any allocator is written.
 *
 * Owner rows use caller-declared resource metadata; they are never inferred
 * from memory types. `heap_usage_bytes`/`heap_budget_bytes` are populated only
 * when VK_EXT_memory_budget is available; `heap_usage_valid` says which it is.
 */
typedef struct VkrDeviceMemoryStats {
  uint64_t live_allocation_count;
  uint64_t peak_allocation_count;
  uint64_t total_allocation_count;
  /**
   * Device limit on simultaneous allocations. Peak against this is the number
   * that decides whether one-allocation-per-resource is a correctness risk or
   * only a performance question.
   */
  uint64_t max_allocation_count;
  uint64_t live_bytes;
  uint64_t peak_bytes;
  /** False once the tracking table overflowed; live figures are then inexact.
   */
  bool8_t live_totals_exact;

  VkrGpuAllocationOwnerTotals owners[VKR_GPU_ALLOCATION_OWNER_COUNT];

  uint32_t memory_type_count;
  uint64_t live_bytes_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  uint64_t live_count_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  uint32_t heap_index_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];
  uint32_t property_flags_by_type[VKR_DEVICE_MEMORY_TYPE_MAX];

  uint32_t heap_count;
  uint64_t heap_size_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  uint64_t heap_usage_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  uint64_t heap_budget_bytes[VKR_DEVICE_MEMORY_HEAP_MAX];
  bool8_t heap_usage_valid;
} VkrDeviceMemoryStats;

// ============================================================================
// Frontend API (User-Facing)
// ============================================================================

// --- START Initialization and Shutdown ---
bool32_t vkr_renderer_initialize(VkrRendererFrontendHandle renderer,
                                 VkrRendererBackendType type, VkrWindow *window,
                                 EventManager *event_manager,
                                 VkrDeviceRequirements *device_requirements,
                                 const VkrRendererBackendConfig *backend_config,
                                 uint64_t target_frame_rate,
                                 VkrRendererError *out_error);

bool32_t vkr_renderer_systems_initialize(
    VkrRendererFrontendHandle renderer, VkrJobSystem *job_system,
    const VkrRendererMetricsProducerConfig *metrics_producers,
    const VkrSubsystemPlan *subsystem_plan);

/**
 * @return The units that actually initialized. Some optional units can fail
 *         non-fatally, so this is the planned closure minus anything that did
 *         not come up — never the request. Reports and comparison identity use
 *         this value.
 */
VkrSubsystemMask
vkr_renderer_get_subsystem_mask(VkrRendererFrontendHandle renderer);

void vkr_renderer_destroy(VkrRendererFrontendHandle renderer);
// --- END Initialization and Shutdown ---

// --- START Utility ---
String8 vkr_renderer_get_error_string(VkrRendererError error);
VkrWindow *vkr_renderer_get_window(VkrRendererFrontendHandle renderer);
VkrRendererBackendType
vkr_renderer_get_backend_type(VkrRendererFrontendHandle renderer);
bool32_t vkr_renderer_is_frame_active(VkrRendererFrontendHandle renderer);
VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer);

/** One completed GPU queue submission, associated with its source frame. */
typedef struct VkrGpuSubmissionTiming {
  uint64_t submit_serial;
  uint64_t source_frame_index;
  uint64_t duration_ns;
  VkrMetricReason unavailable_reason;
  bool8_t valid;
} VkrGpuSubmissionTiming;

/**
 * Polls the earliest retained submission timing newer than
 * `after_submit_serial`. The call is nonblocking; `false` means no newer
 * completion is ready.
 */
bool8_t
vkr_renderer_gpu_submission_timing_poll(VkrRendererFrontendHandle renderer,
                                        uint64_t after_submit_serial,
                                        VkrGpuSubmissionTiming *out_timing);

void vkr_renderer_get_device_information(
    VkrRendererFrontendHandle renderer,
    VkrDeviceInformation *device_information, Arena *temp_arena);
uint64_t vkr_renderer_get_target_frame_rate(VkrRendererFrontendHandle renderer);
uint64_t vkr_renderer_get_submit_serial(VkrRendererFrontendHandle renderer);
uint64_t
vkr_renderer_get_completed_submit_serial(VkrRendererFrontendHandle renderer);
bool8_t vkr_renderer_get_and_reset_upload_wait_stats(
    VkrRendererFrontendHandle renderer, VkrRendererUploadWaitStats *out_stats);
/** Returns and resets CPU waits caused by bounded command-slot reuse. */
bool8_t vkr_renderer_get_and_reset_command_slot_wait_count(
    VkrRendererFrontendHandle renderer, uint64_t *out_wait_count);
/** @brief Snapshots device-memory allocation telemetry. Non-resetting. */
bool8_t vkr_renderer_get_device_memory_stats(VkrRendererFrontendHandle renderer,
                                             VkrDeviceMemoryStats *out_stats);
// --- END Utility ---

// Text creation/destruction (persistent resources)
bool8_t vkr_renderer_create_ui_text(VkrRendererFrontendHandle renderer,
                                    const VkrUiTextCreateData *payload,
                                    uint32_t *out_text_id);
bool8_t vkr_renderer_destroy_ui_text(VkrRendererFrontendHandle renderer,
                                     uint32_t text_id);
bool8_t vkr_renderer_create_world_text(VkrRendererFrontendHandle renderer,
                                       const VkrWorldTextCreateData *payload);
bool8_t vkr_renderer_destroy_world_text(VkrRendererFrontendHandle renderer,
                                        uint32_t text_id);
// --- END Resource Management ---

/**
 * @brief Frame-in-flight slot currently being recorded.
 *
 * Use this, not the swapchain image index, to index per-frame CPU-written
 * buffers: the slot's fence is waited on in begin_frame, so its previous
 * contents are guaranteed to be free of GPU readers.
 */
uint32_t vkr_renderer_frame_in_flight_index(VkrRendererFrontendHandle renderer);
/** @brief Number of distinct frame-in-flight slots (<= BUFFERING_FRAMES). */
uint32_t vkr_renderer_frame_in_flight_count(VkrRendererFrontendHandle renderer);
/**
 * Target-neutral attachment and frame configuration queries.
 *
 * These are the only way to reach the images a frame renders into. The
 * `swapchain`-named graph imports resolve through them, so a windowed and an
 * offscreen renderer present the same contract to every caller above the
 * backend.
 */
uint32_t
vkr_renderer_present_target_image_count(VkrRendererFrontendHandle renderer);
VkrPresentTargetKind
vkr_renderer_present_target_kind(VkrRendererFrontendHandle renderer);
void vkr_renderer_present_target_extent(VkrRendererFrontendHandle renderer,
                                        uint32_t *out_width,
                                        uint32_t *out_height);
VkrTextureFormat
vkr_renderer_present_target_format(VkrRendererFrontendHandle renderer,
                                   VkrPresentTargetAttachment attachment);
/**
 * @brief Recreates an offscreen target outside an active frame.
 *
 * This is the only resize path for an offscreen target. It waits for GPU
 * completion before replacing all per-image attachments and synchronization.
 */
VkrRendererError
vkr_renderer_present_target_recreate(VkrRendererFrontendHandle renderer,
                                     uint32_t width, uint32_t height,
                                     uint32_t image_count);
VkrTextureFormat
vkr_renderer_get_shadow_depth_format(VkrRendererFrontendHandle renderer);
// --- END Render Pass & Target Management ---

// --- START Frame Lifecycle & Rendering Commands ---
VkrRendererError vkr_renderer_prepare_frame(VkrRendererFrontendHandle renderer,
                                            VkrFrameSetup *out_setup);
VkrRendererError
vkr_renderer_submit_packet(VkrRendererFrontendHandle renderer,
                           const VkrRenderPacket *packet,
                           VkrRendererFrameMetrics *out_metrics,
                           VkrValidationError *out_validation_error);

uint32_t vkr_renderer_capture_channel_count(void);
const VkrCaptureChannelDescription *
vkr_renderer_capture_channel_get(uint32_t index);
VkrCaptureChannelId vkr_renderer_capture_channel_from_name(const char *name);
VkrCaptureStatus vkr_renderer_capture_poll(VkrRendererFrontendHandle renderer,
                                           VkrCaptureRequestId request_id,
                                           VkrCapturePollResult *out_result);
bool8_t vkr_renderer_capture_release(VkrRendererFrontendHandle renderer,
                                     VkrCaptureRequestId request_id);

void vkr_renderer_resize(VkrRendererFrontendHandle renderer, uint32_t width,
                         uint32_t height);

// --- END Frame Lifecycle & Rendering Commands ---

// --- START Pixel Readback API (for picking and screenshots) ---

/**
 * @brief Status of an asynchronous pixel readback operation
 */
typedef enum VkrReadbackStatus {
  VKR_READBACK_STATUS_IDLE = 0, // No readback pending
  VKR_READBACK_STATUS_PENDING,  // Readback in progress (wait for next frame)
  VKR_READBACK_STATUS_READY,    // Data ready to read
  VKR_READBACK_STATUS_ERROR,    // An error occurred
} VkrReadbackStatus;

/**
 * @brief Result of a pixel readback operation
 */
typedef struct VkrPixelReadbackResult {
  VkrReadbackStatus status; // Current status
  uint32_t x;               // Requested X coordinate
  uint32_t y;               // Requested Y coordinate
  uint32_t data;            // Pixel data (for R32_UINT format)
  bool8_t valid;            // True if data is valid
} VkrPixelReadbackResult;

/**
 * @brief Get the result of a previously requested pixel readback.
 *
 * @param renderer The renderer frontend handle
 * @param out_result Output structure for the readback result
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError
vkr_renderer_get_pixel_readback_result(VkrRendererFrontendHandle renderer,
                                       VkrPixelReadbackResult *out_result);

// --- END Pixel Readback API ---

// ============================================================================
// Utility functions
// ============================================================================

/**
 * @brief Get the allocator for the backend.
 *
 * @param renderer The renderer frontend handle
 * @return The allocator
 */
VkrAllocator *
vkr_renderer_get_backend_allocator(VkrRendererFrontendHandle renderer);
