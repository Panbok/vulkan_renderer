#pragma once

#include "containers/bitset.h"
#include "core/event.h"
#include "core/vkr_job_system.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "math/mat.h"
#include "renderer/systems/vkr_camera.h"

// ============================================================================
// Design Overview
// ============================================================================
/*
    Renderer Design: Frontend / Backend Architecture

    1. Frontend (Public API - renderer_frontend_*.h / this file):
       - Provides a graphics API-agnostic interface for the user.
       - Manages scene representation (though not explicitly detailed here,
         it would use the handles defined below).
       - Translates user requests into abstract rendering commands.
       - Manages high-level resources (Meshes, Textures, Materials) using
         opaque handles.
       - Can switch between different backend implementations.

    2. Backend (Internal Implementation - e.g., renderer_vulkan_backend.h/.c):
       - Implements the abstract rendering commands using a specific graphics
         API (e.g., Vulkan, DX12, Metal).
       - Manages GPU-specific resources.
       - Executes rendering operations.
       - The interface for a backend is defined by `RendererBackendInterface`.
         The frontend will call these functions.

    Key Concepts:
    - Handles: Opaque pointers (e.g., `BufferHandle`, `ShaderHandle`,
      `PipelineHandle`) are used in the frontend API to refer to resources.
      This hides internal details and backend-specific representations from
      the user.
    - Resource Descriptions: Structs (e.g., `BufferDescription`,
      `GraphicsPipelineDescription`) are used to specify parameters for
      resource creation.
    - Buffer Management: Generic buffers can be created for any purpose
      (vertex, index, uniform), but binding functions are specialized to
      provide context and type safety (e.g., `VertexBufferBinding`,
      `IndexBufferBinding`).
    - Vertex Layout Connection: Vertex input descriptions in pipelines define
      the layout, and vertex buffer bindings at runtime must reference the
      correct binding points defined in the pipeline.
    - Command Generation: The frontend internally generates a list of
      abstract rendering commands. These are then processed by the active
      backend. (The exact structure of these internal commands is not exposed
      in this public header but is implied by the backend interface functions).
    - State Objects: Pipeline State Objects (PSOs) encapsulate a large part
      of the GPU pipeline state (shaders, blend, depth, rasterizer states)
      to minimize redundant state changes.
*/

// ============================================================================
// Forward Declarations & Opaque Handles
// ============================================================================

typedef struct s_RendererFrontend *VkrRendererFrontendHandle;
typedef struct s_BufferResource *VkrBufferHandle;
typedef struct s_Pipeline *VkrPipelineOpaqueHandle;
typedef struct s_TextureHandle *VkrTextureOpaqueHandle;
typedef struct s_RenderPass *VkrRenderPassHandle;
typedef struct s_RenderTarget *VkrRenderTargetHandle;

typedef union {
  void *ptr;
  uint64_t id;
  struct {
    uint32_t type;
    uint32_t index;
  } typed;
} VkrBackendResourceHandle;

typedef enum VkrRendererBackendType {
  VKR_RENDERER_BACKEND_TYPE_VULKAN,
  VKR_RENDERER_BACKEND_TYPE_DX12,  // Future
  VKR_RENDERER_BACKEND_TYPE_METAL, // Future
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

typedef enum VkrPrimitiveTopology {
  VKR_PRIMITIVE_TOPOLOGY_POINT_LIST,
  VKR_PRIMITIVE_TOPOLOGY_LINE_LIST,
  VKR_PRIMITIVE_TOPOLOGY_LINE_STRIP,
  VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
  VKR_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, // Often not recommended
} VkrPrimitiveTopology;

typedef enum VkrVertexFormat {
  VKR_VERTEX_FORMAT_UNDEFINED = 0,
  VKR_VERTEX_FORMAT_R32_SFLOAT,
  VKR_VERTEX_FORMAT_R32G32_SFLOAT,
  VKR_VERTEX_FORMAT_R32G32B32_SFLOAT,
  VKR_VERTEX_FORMAT_R32G32B32A32_SFLOAT,
  VKR_VERTEX_FORMAT_R32_SINT,
  VKR_VERTEX_FORMAT_R32_UINT,
  VKR_VERTEX_FORMAT_R8G8B8A8_UNORM,
} VkrVertexFormat;

typedef enum VertexInputRate {
  VKR_VERTEX_INPUT_RATE_VERTEX,
  VKR_VERTEX_INPUT_RATE_INSTANCE
} VkrVertexInputRate;

typedef enum VkrIndexType {
  VKR_INDEX_TYPE_UINT16,
  VKR_INDEX_TYPE_UINT32,
} VkrIndexType;

typedef enum VkrPolygonMode {
  VKR_POLYGON_MODE_FILL = 0,
  VKR_POLYGON_MODE_LINE,
  VKR_POLYGON_MODE_POINT,
} VkrPolygonMode;

typedef enum VkrCullMode {
  VKR_CULL_MODE_NONE = 0,
  VKR_CULL_MODE_FRONT,
  VKR_CULL_MODE_BACK,
  VKR_CULL_MODE_FRONT_AND_BACK,
} VkrCullMode;

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

typedef enum VkrBufferTypeBits {
  VKR_BUFFER_TYPE_GRAPHICS = 1 << 0,
  VKR_BUFFER_TYPE_COMPUTE = 1 << 1,
  VKR_BUFFER_TYPE_TRANSFER = 1 << 2,
} VkrBufferTypeBits;
typedef Bitset8 VkrBufferTypeFlags;

typedef enum VkrMemoryPropertyBits {
  VKR_MEMORY_PROPERTY_DEVICE_LOCAL = 1 << 0,  // GPU optimal memory
  VKR_MEMORY_PROPERTY_HOST_VISIBLE = 1 << 1,  // CPU can map
  VKR_MEMORY_PROPERTY_HOST_COHERENT = 1 << 2, // No explicit flush needed
  VKR_MEMORY_PROPERTY_HOST_CACHED = 1 << 3,   // CPU cacheable
} VkrMemoryPropertyBits;
typedef Bitset8 VkrMemoryPropertyFlags;

// MemoryPropertyFlags helper functions
vkr_internal INLINE VkrMemoryPropertyFlags
vkr_memory_property_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrMemoryPropertyFlags
vkr_memory_property_flags_from_bits(uint8_t bits) {
  VkrMemoryPropertyFlags flags = bitset8_create();
  if (bits & VKR_MEMORY_PROPERTY_DEVICE_LOCAL)
    bitset8_set(&flags, VKR_MEMORY_PROPERTY_DEVICE_LOCAL);
  if (bits & VKR_MEMORY_PROPERTY_HOST_VISIBLE)
    bitset8_set(&flags, VKR_MEMORY_PROPERTY_HOST_VISIBLE);
  if (bits & VKR_MEMORY_PROPERTY_HOST_COHERENT)
    bitset8_set(&flags, VKR_MEMORY_PROPERTY_HOST_COHERENT);
  if (bits & VKR_MEMORY_PROPERTY_HOST_CACHED)
    bitset8_set(&flags, VKR_MEMORY_PROPERTY_HOST_CACHED);
  return flags;
}

#define VKR_MEMORY_PROPERTY_FLAGS_DEVICE_LOCAL()                               \
  vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_DEVICE_LOCAL)

#define VKR_MEMORY_PROPERTY_FLAGS_HOST_VISIBLE()                               \
  vkr_memory_property_flags_from_bits(VKR_MEMORY_PROPERTY_HOST_VISIBLE |       \
                                      VKR_MEMORY_PROPERTY_HOST_COHERENT)

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
  bool8_t supports_multi_draw_indirect;
  bool8_t supports_draw_indirect_first_instance;
} VkrDeviceInformation;

// ============================================================================
// Resource Descriptions
// ============================================================================

typedef struct VkrBufferDescription {
  uint64_t size;
  VkrBufferUsageFlags usage;
  VkrMemoryPropertyFlags memory_properties; // Hint for memory type
  // For staging, the frontend might create two buffers:
  // one HOST_VISIBLE for upload, one DEVICE_LOCAL for rendering.
  // Or the backend abstracts this.

  VkrBufferTypeFlags buffer_type;

  bool8_t bind_on_create;
  bool8_t persistently_mapped;
} VkrBufferDescription;

typedef enum VkrShaderStage {
  VKR_SHADER_STAGE_VERTEX = 0,
  VKR_SHADER_STAGE_FRAGMENT = 1,
  // Future: geometry_shader, tess_control_shader, tess_eval_shader
  VKR_SHADER_STAGE_COUNT,
} VkrShaderStage;

typedef enum VkrShaderFileFormat {
  VKR_SHADER_FILE_FORMAT_SPIR_V = 0,
  VKR_SHADER_FILE_FORMAT_HLSL,
  VKR_SHADER_FILE_FORMAT_GLSL,
} VkrShaderFileFormat;

typedef enum VkrShaderFileType {
  VKR_SHADER_FILE_TYPE_SINGLE = 0,
  VKR_SHADER_FILE_TYPE_MULTI,
} VkrShaderFileType;

typedef enum VkrTextureType {
  VKR_TEXTURE_TYPE_2D,
  VKR_TEXTURE_TYPE_CUBE_MAP,
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
  // Single/dual channel formats
  VKR_TEXTURE_FORMAT_R8_UNORM,
  VKR_TEXTURE_FORMAT_R16_SFLOAT,
  VKR_TEXTURE_FORMAT_R32_SFLOAT,
  VKR_TEXTURE_FORMAT_R32_UINT,
  VKR_TEXTURE_FORMAT_R8G8_UNORM,
  // Depth/stencil formats
  VKR_TEXTURE_FORMAT_D32_SFLOAT,
  VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT,

  VKR_TEXTURE_FORMAT_COUNT,
} VkrTextureFormat;

typedef enum VkrTextureUsageBits {
  VKR_TEXTURE_USAGE_NONE = 0,
  VKR_TEXTURE_USAGE_SAMPLED = 1 << 0,
  VKR_TEXTURE_USAGE_COLOR_ATTACHMENT = 1 << 1,
  VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT = 1 << 2,
  VKR_TEXTURE_USAGE_TRANSFER_SRC = 1 << 3,
  VKR_TEXTURE_USAGE_TRANSFER_DST = 1 << 4,
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
  VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY = VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT =
      VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_SRC = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
  VKR_TEXTURE_LAYOUT_TRANSFER_DST = VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL,
} VkrTextureLayout;

typedef enum VkrTexturePropertyBits {
  VKR_TEXTURE_PROPERTY_WRITABLE_BIT = 1 << 0,
  VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT = 1 << 1,
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

  VkrTextureType type;
  VkrTextureFormat format;
  VkrSampleCount sample_count; // MSAA sample count (default: VKR_SAMPLE_COUNT_1)
  VkrTexturePropertyFlags properties;

  VkrTextureRepeatMode u_repeat_mode;
  VkrTextureRepeatMode v_repeat_mode;
  VkrTextureRepeatMode w_repeat_mode;

  VkrFilter min_filter;
  VkrFilter mag_filter;
  VkrMipFilter mip_filter;
  bool8_t anisotropy_enable;
} VkrTextureDescription;

typedef struct VkrTextureWriteRegion {
  uint32_t mip_level;
  uint32_t array_layer;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} VkrTextureWriteRegion;

// ----------------------------------------------------------------------------
// Instance state & material state
// ----------------------------------------------------------------------------

typedef enum VkrRenderMode {
  VKR_RENDER_MODE_DEFAULT = 0,
  VKR_RENDER_MODE_LIGHTING = 1,
  VKR_RENDER_MODE_NORMAL = 2,
  VKR_RENDER_MODE_UNLIT = 3,
  VKR_RENDER_MODE_COUNT,
} VkrRenderMode;

typedef struct VkrGlobalMaterialState {
  Mat4 projection;
  Mat4 view;
  Mat4 ui_projection;
  Mat4 ui_view;
  Vec4 ambient_color;
  Vec3 view_position;
  VkrRenderMode render_mode;
} VkrGlobalMaterialState;

typedef struct VkrLocalMaterialState {
  Mat4 model;
  uint32_t object_id; // Encoded picking id (0 = background/no object)
} VkrLocalMaterialState;

typedef struct VkrRendererInstanceStateHandle {
  uint32_t id;
} VkrRendererInstanceStateHandle;

/*
  Vulkan backend descriptor layout
  - Descriptor set 0 (per-frame/global):
      binding 0 = uniform buffer (GlobalUniformObject: view, projection, etc.)
      binding 1 = storage buffer (instance data stream)
  - Descriptor set 1 (per-object/instance):
      binding 0 = uniform buffer (InstanceUniformObject: material uniforms)
      binding 1 = sampled image (combined image sampler slot 0)
      binding 2 = sampler (slot 0)

  Notes:
  - Materials currently bind exactly 1 texture (base color) via slot 0.
  - Additional textures (normal/metallic/emissive) are not yet exposed; future
    work may extend set 1 or use descriptor arrays.
*/

typedef struct VkrShaderStateObject {
  // Instance state management: hidden behind a typed handle.
  VkrRendererInstanceStateHandle instance_state;
  // Raw data for instance uniforms and push constants (config-sized)
  const void *instance_ubo_data;
  uint64_t instance_ubo_size;
  const void *push_constants_data;
  uint64_t push_constants_size;
} VkrShaderStateObject;

typedef struct VkrRendererMaterialState {
  // Per-material uniforms (raw mode only; legacy struct removed)
// Dynamic sampler slots (config-driven). Only the first texture_count are used.
#define VKR_MAX_INSTANCE_TEXTURES 8
  VkrTextureOpaqueHandle textures[VKR_MAX_INSTANCE_TEXTURES];
  bool8_t textures_enabled[VKR_MAX_INSTANCE_TEXTURES];
  uint32_t texture_count;
} VkrRendererMaterialState;

// =============================================================================
// Skybox
// =============================================================================
typedef struct VkrSkyboxHandle {
  uint32_t id;
  uint32_t generation;
} VkrSkyboxHandle;

#define VKR_SKYBOX_HANDLE_INVALID ((VkrSkyboxHandle){0, 0})

typedef struct VkrSkybox {
  VkrSkyboxHandle handle;
  VkrTextureOpaqueHandle cube_map_texture;
  VkrBackendResourceHandle pipeline;
  VkrBackendResourceHandle geometry;
  VkrRendererInstanceStateHandle instance_state;
} VkrSkybox;

typedef struct VkrShaderModuleDescription {
  VkrShaderStageFlags stages;
  /* Path to the shader file (same path for single file, different paths for
   * multi-file) */
  String8 path;
  /* Entry point for the shader (e.g., "main") */
  String8 entry_point;
  // Future: defines, include paths etc.
} VkrShaderModuleDescription;

typedef struct VkrShaderObjectDescription {
  /* Format of the shader file (e.g., SPIR-V, HLSL, GLSL) */
  VkrShaderFileFormat file_format;
  /* Determines if the shader is a single file or a multi-file shader (e.g.,
   * single, multi) */
  VkrShaderFileType file_type;

  VkrShaderModuleDescription modules[VKR_SHADER_STAGE_COUNT];

  // Deprecated fields removed: uniforms are config-driven

  uint64_t global_ubo_size;
  uint64_t global_ubo_stride;
  uint64_t instance_ubo_size;
  uint64_t instance_ubo_stride;
  uint64_t push_constant_size;
  uint32_t global_texture_count;
  uint32_t instance_texture_count;
} VkrShaderObjectDescription;

// Used at PIPELINE CREATION time to define vertex layout
typedef struct VkrVertexInputAttributeDescription {
  uint32_t location; // Shader input location (layout(location = X) in shader)
  uint32_t binding;  // Which vertex buffer binding this attribute uses
  VkrVertexFormat format; // Format of the attribute data
  uint32_t offset;        // Offset within the vertex stride
} VkrVertexInputAttributeDescription;

// Used at PIPELINE CREATION time to define vertex buffer bindings
typedef struct VkrVertexInputBindingDescription {
  uint32_t binding; // The binding number (referenced by attributes and runtime
                    // bindings)
  uint32_t stride;  // Distance between consecutive elements for this binding
  VkrVertexInputRate input_rate; // Per-vertex or per-instance
} VkrVertexInputBindingDescription;

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

// ============================================================================
// Render Pass Signature (for compatibility checking and MSAA support)
// ============================================================================

#define VKR_MAX_COLOR_ATTACHMENTS 8

/**
 * @brief Render pass signature for compatibility validation and pipeline state
 * derivation.
 *
 * Captures attachment metadata required for:
 * - Framebuffer compatibility checking
 * - Pipeline multisample state derivation
 * - Render target validation
 */
typedef struct VkrRenderPassSignature {
  uint8_t color_attachment_count;
  VkrTextureFormat color_formats[VKR_MAX_COLOR_ATTACHMENTS];
  VkrSampleCount color_samples[VKR_MAX_COLOR_ATTACHMENTS];
  bool8_t has_depth_stencil;
  VkrTextureFormat depth_stencil_format;
  VkrSampleCount depth_stencil_samples;
  bool8_t has_resolve_attachments;
  uint8_t resolve_attachment_count;
} VkrRenderPassSignature;

// ============================================================================
// VkrRenderPassDesc - Explicit render pass attachment configuration
// ============================================================================

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

typedef struct VkrRenderPassAttachmentDesc {
  VkrTextureFormat format;
  VkrSampleCount samples;
  VkrAttachmentLoadOp load_op;
  VkrAttachmentLoadOp stencil_load_op;
  VkrAttachmentStoreOp store_op;
  VkrAttachmentStoreOp stencil_store_op;
  VkrTextureLayout initial_layout;
  VkrTextureLayout final_layout;
  VkrClearValue clear_value;
} VkrRenderPassAttachmentDesc;

typedef struct VkrResolveAttachmentRef {
  uint8_t src_attachment_index; // Index into color_attachments
  uint8_t dst_attachment_index; // Index in resolve output
} VkrResolveAttachmentRef;

typedef struct VkrRenderPassDesc {
  String8 name;
  VkrPipelineDomain domain;
  uint8_t color_attachment_count;
  VkrRenderPassAttachmentDesc *color_attachments;
  VkrRenderPassAttachmentDesc *depth_stencil_attachment; // NULL if no depth
  uint8_t resolve_attachment_count;
  VkrResolveAttachmentRef *resolve_attachments;
} VkrRenderPassDesc;

typedef struct VkrViewport {
  float32_t x;
  float32_t y;
  float32_t width;
  float32_t height;
  float32_t min_depth;
  float32_t max_depth;
} VkrViewport;

typedef struct VkrScissor {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
} VkrScissor;

// ============================================================================
// VkrRenderTargetDesc - Extended render target with mip/layer addressing
// ============================================================================

/**
 * @brief Reference to a specific subresource of a texture for framebuffer use.
 *
 * Allows rendering to specific mip levels or array layers (e.g., cubemap faces).
 */
typedef struct VkrRenderTargetAttachmentRef {
  VkrTextureOpaqueHandle texture;
  uint32_t mip_level;   // Mip level to use (0 = base level)
  uint32_t base_layer;  // Base array layer (0 for 2D textures, 0-5 for cubemaps)
  uint32_t layer_count; // Number of layers (1 for single layer, 6 for full cubemap)
} VkrRenderTargetAttachmentRef;

/**
 * @brief Extended render target descriptor with mip/layer addressing support.
 *
 * Use this for advanced cases like:
 * - Rendering to specific mip levels (mip chain generation)
 * - Rendering to cubemap faces
 * - Rendering to texture array slices
 */
typedef struct VkrRenderTargetDesc {
  bool8_t sync_to_window_size;
  uint32_t width, height;
  uint8_t attachment_count;
  VkrRenderTargetAttachmentRef *attachments;
} VkrRenderTargetDesc;

typedef struct VkrRenderTargetTextureDesc {
  uint32_t width;
  uint32_t height;
  VkrTextureFormat format;
  VkrTextureUsageFlags usage;
} VkrRenderTargetTextureDesc;

typedef struct VkrGraphicsPipelineDescription {
  VkrShaderObjectDescription shader_object_description;

  uint32_t attribute_count;
  VkrVertexInputAttributeDescription *attributes;
  uint32_t binding_count;
  VkrVertexInputBindingDescription *bindings;

  VkrPrimitiveTopology topology;

  VkrPolygonMode polygon_mode;
  VkrCullMode cull_mode;

  VkrRenderPassHandle renderpass;
  VkrPipelineDomain domain;
} VkrGraphicsPipelineDescription;

typedef struct VkrRendererBackendConfig {
  const char *application_name;
  uint16_t renderpass_desc_count;
  const VkrRenderPassDesc *pass_descs;
  void (*on_render_target_refresh_required)();
} VkrRendererBackendConfig;

// ============================================================================
// View / Layer System
// ============================================================================
typedef struct VkrLayerHandle {
  uint32_t id;
  uint32_t generation;
} VkrLayerHandle;

#define VKR_LAYER_HANDLE_INVALID ((VkrLayerHandle){0, 0})

typedef struct VkrLayerContext VkrLayerContext; // Opaque to clients

typedef struct VkrLayerRenderInfo {
  uint32_t image_index;    // Swapchain image index being rendered
  float64_t delta_time;    // Delta time since last frame
  String8 renderpass_name; // Active renderpass name for this callback
} VkrLayerRenderInfo;

typedef struct VkrLayerUpdateInfo {
  float64_t delta_time;           // Time since last frame
  InputState *input_state;        // Pointer to window's input state (read-only)
  VkrCameraSystem *camera_system; // Access to cameras
  VkrCameraHandle active_camera;  // Currently active camera
  uint32_t frame_number;          // Current frame count
} VkrLayerUpdateInfo;

// Forward declaration for typed message header
typedef struct VkrLayerMsgHeader VkrLayerMsgHeader;

typedef struct VkrLayerCallbacks {
  bool32_t (*on_create)(
      VkrLayerContext *ctx);               // optional, return false on failure
  void (*on_attach)(VkrLayerContext *ctx); // Optional
  void (*on_resize)(VkrLayerContext *ctx, uint32_t width, uint32_t height);
  void (*on_render)(VkrLayerContext *ctx, const VkrLayerRenderInfo *info);
  void (*on_detach)(VkrLayerContext *ctx);  // Optional
  void (*on_destroy)(VkrLayerContext *ctx); // Optional

  bool8_t (*on_update)(VkrLayerContext *ctx, const VkrLayerUpdateInfo *info);
  void (*on_enable)(VkrLayerContext *ctx);
  void (*on_disable)(VkrLayerContext *ctx);
  /**
   * @brief Callback for receiving typed layer messages.
   * @param ctx Layer context.
   * @param msg Message header (payload follows immediately after).
   * @param out_rsp Buffer for typed response (NULL if none expected).
   * @param out_rsp_capacity Size of out_rsp buffer.
   * @param out_rsp_size Actual response size written.
   */
  void (*on_data_received)(VkrLayerContext *ctx, const VkrLayerMsgHeader *msg,
                           void *out_rsp, uint64_t out_rsp_capacity,
                           uint64_t *out_rsp_size);
} VkrLayerCallbacks;

typedef struct VkrLayerPassConfig {
  String8 renderpass_name; // e.g. "Renderpass.Builtin.World"
  bool8_t use_swapchain_color;
  bool8_t use_depth;
} VkrLayerPassConfig;

typedef struct VkrLayerConfig {
  String8 name;
  uint32_t order;
  uint32_t width;
  uint32_t height;
  Mat4 view;
  Mat4 projection;
  uint8_t pass_count;
  VkrLayerPassConfig *passes;
  VkrLayerCallbacks callbacks;
  void *user_data;
  bool8_t enabled;
  uint32_t flags;
} VkrLayerConfig;

typedef enum VkrLayerFlags {
  VKR_LAYER_FLAG_NONE = 0,
  VKR_LAYER_FLAG_ALWAYS_UPDATE = 1 << 0,
} VkrLayerFlags;

typedef struct VkrLayerBehavior {
  String8 name;
  void *behavior_data;

  void (*on_attach)(VkrLayerContext *ctx, void *behavior_data);
  void (*on_detach)(VkrLayerContext *ctx, void *behavior_data);
  bool8_t (*on_update)(VkrLayerContext *ctx, void *behavior_data,
                       const VkrLayerUpdateInfo *info);
  void (*on_render)(VkrLayerContext *ctx, void *behavior_data,
                    const VkrLayerRenderInfo *info);
  void (*on_data_received)(VkrLayerContext *ctx, void *behavior_data,
                           const VkrLayerMsgHeader *msg, void *out_rsp,
                           uint64_t out_rsp_capacity, uint64_t *out_rsp_size);
} VkrLayerBehavior;

typedef struct VkrLayerBehaviorHandle {
  uint32_t id;
  uint32_t generation;
} VkrLayerBehaviorHandle;

#define VKR_LAYER_BEHAVIOR_HANDLE_INVALID ((VkrLayerBehaviorHandle){0, 0})

// ============================================================================
// Buffer and Vertex/Index Data Structures
// ============================================================================

// Used at RUNTIME to bind actual buffers to the vertex input bindings defined
// in the pipeline
typedef struct VkrVertexBufferBinding {
  VkrBufferHandle buffer;
  uint32_t binding; // Must match a binding number from
                    // VertexInputBindingDescription in the current pipeline
  uint64_t offset;  // Offset into the buffer
} VkrVertexBufferBinding;

typedef struct VkrIndexBufferBinding {
  VkrBufferHandle buffer;
  VkrIndexType type; // uint16 or uint32
  uint64_t offset;   // Offset into the buffer
} VkrIndexBufferBinding;

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

bool32_t vkr_renderer_systems_initialize(VkrRendererFrontendHandle renderer,
                                         VkrJobSystem *job_system);

void vkr_renderer_destroy(VkrRendererFrontendHandle renderer);
// --- END Initialization and Shutdown ---

// --- START Utility ---
String8 vkr_renderer_get_error_string(VkrRendererError error);
VkrWindow *vkr_renderer_get_window(VkrRendererFrontendHandle renderer);
VkrRendererBackendType
vkr_renderer_get_backend_type(VkrRendererFrontendHandle renderer);
bool32_t vkr_renderer_is_frame_active(VkrRendererFrontendHandle renderer);
VkrRendererError vkr_renderer_wait_idle(VkrRendererFrontendHandle renderer);
void vkr_renderer_get_device_information(
    VkrRendererFrontendHandle renderer,
    VkrDeviceInformation *device_information, Arena *temp_arena);
uint64_t vkr_renderer_get_target_frame_rate(VkrRendererFrontendHandle renderer);
// --- END Utility ---

// --- START Resource Management ---
VkrBufferHandle vkr_renderer_create_buffer(
    VkrRendererFrontendHandle renderer, const VkrBufferDescription *description,
    const void *initial_data, VkrRendererError *out_error);

// Convenience functions for common buffer types
VkrBufferHandle
vkr_renderer_create_vertex_buffer(VkrRendererFrontendHandle renderer,
                                  uint64_t size, const void *initial_data,
                                  VkrRendererError *out_error);

VkrBufferHandle vkr_renderer_create_index_buffer(
    VkrRendererFrontendHandle renderer, uint64_t size, VkrIndexType type,
    const void *initial_data, VkrRendererError *out_error);

// Dynamic buffer creation (HOST_VISIBLE memory for frequent updates)
VkrBufferHandle vkr_renderer_create_vertex_buffer_dynamic(
    VkrRendererFrontendHandle renderer, uint64_t size, const void *initial_data,
    VkrRendererError *out_error);

VkrBufferHandle vkr_renderer_create_index_buffer_dynamic(
    VkrRendererFrontendHandle renderer, uint64_t size, VkrIndexType type,
    const void *initial_data, VkrRendererError *out_error);

VkrTextureOpaqueHandle
vkr_renderer_create_texture(VkrRendererFrontendHandle renderer,
                            const VkrTextureDescription *description,
                            const void *initial_data,
                            VkrRendererError *out_error);
VkrTextureOpaqueHandle
vkr_renderer_create_writable_texture(VkrRendererFrontendHandle renderer,
                                     const VkrTextureDescription *desc,
                                     VkrRendererError *out_error);

VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture(
    VkrRendererFrontendHandle renderer, const VkrRenderTargetTextureDesc *desc,
    VkrRendererError *out_error);

VkrTextureOpaqueHandle
vkr_renderer_create_depth_attachment(VkrRendererFrontendHandle renderer,
                                     uint32_t width, uint32_t height,
                                     VkrRendererError *out_error);
VkrTextureOpaqueHandle vkr_renderer_create_sampled_depth_attachment(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    VkrRendererError *out_error);

/**
 * @brief Creates an MSAA (multisampled) render target texture.
 *
 * Creates a texture with the specified sample count for use as an MSAA
 * attachment. MSAA textures cannot be sampled directly and must be resolved
 * to a single-sample texture before sampling.
 *
 * @param renderer Renderer handle
 * @param width Texture width
 * @param height Texture height
 * @param format Texture format
 * @param samples MSAA sample count (must be > 1 for actual MSAA)
 * @param out_error Optional error output
 * @return Handle to the created MSAA texture, or NULL on failure
 */
VkrTextureOpaqueHandle vkr_renderer_create_render_target_texture_msaa(
    VkrRendererFrontendHandle renderer, uint32_t width, uint32_t height,
    VkrTextureFormat format, VkrSampleCount samples, VkrRendererError *out_error);

VkrRendererError vkr_renderer_transition_texture_layout(
    VkrRendererFrontendHandle renderer, VkrTextureOpaqueHandle texture,
    VkrTextureLayout old_layout, VkrTextureLayout new_layout);

VkrRendererError vkr_renderer_write_texture(VkrRendererFrontendHandle renderer,
                                            VkrTextureOpaqueHandle texture,
                                            const void *data, uint64_t size);

VkrRendererError vkr_renderer_write_texture_region(
    VkrRendererFrontendHandle renderer, VkrTextureOpaqueHandle texture,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size);

VkrRendererError vkr_renderer_resize_texture(VkrRendererFrontendHandle renderer,
                                             VkrTextureOpaqueHandle texture,
                                             uint32_t new_width,
                                             uint32_t new_height,
                                             bool8_t preserve_contents);

void vkr_renderer_destroy_texture(VkrRendererFrontendHandle renderer,
                                  VkrTextureOpaqueHandle texture);

VkrRendererError
vkr_renderer_update_texture(VkrRendererFrontendHandle renderer,
                            VkrTextureOpaqueHandle texture,
                            const VkrTextureDescription *description);

void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer);

VkrPipelineOpaqueHandle vkr_renderer_create_graphics_pipeline(
    VkrRendererFrontendHandle renderer,
    const VkrGraphicsPipelineDescription *description,
    VkrRendererError *out_error);

void vkr_renderer_destroy_pipeline(VkrRendererFrontendHandle renderer,
                                   VkrPipelineOpaqueHandle pipeline);
// --- END Resource Management ---

// --- START Data Update ---
VkrRendererError vkr_renderer_update_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data);
void *vkr_renderer_buffer_get_mapped_ptr(VkrRendererFrontendHandle renderer,
                                         VkrBufferHandle buffer);
VkrRendererError vkr_renderer_flush_buffer(VkrRendererFrontendHandle renderer,
                                           VkrBufferHandle buffer,
                                           uint64_t offset, uint64_t size);
void vkr_renderer_set_instance_buffer(VkrRendererFrontendHandle renderer,
                                      VkrBufferHandle buffer);

VkrRendererError vkr_renderer_update_pipeline_state(
    VkrRendererFrontendHandle renderer, VkrPipelineOpaqueHandle pipeline,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material);

/**
 * @brief Update only the per-frame global state (e.g., view/projection). Call
 * once per frame before drawing renderables.
 *
 * @param renderer
 * @param pipeline
 * @param uniform
 * @return VkrRendererError
 */
VkrRendererError
vkr_renderer_update_global_state(VkrRendererFrontendHandle renderer,
                                 VkrPipelineOpaqueHandle pipeline,
                                 const void *uniform);

/**
 * @brief Update only the per-object local state (e.g., model matrix, material
 * uniforms, textures). Call per renderable.
 *
 * @param renderer
 * @param pipeline
 * @param data
 * @return VkrRendererError
 */
VkrRendererError vkr_renderer_update_instance_state(
    VkrRendererFrontendHandle renderer, VkrPipelineOpaqueHandle pipeline,
    const VkrShaderStateObject *data, const VkrRendererMaterialState *material);

// Instance state lifetime
VkrRendererError
vkr_renderer_acquire_instance_state(VkrRendererFrontendHandle renderer,
                                    VkrPipelineOpaqueHandle pipeline,
                                    VkrRendererInstanceStateHandle *out_handle);

VkrRendererError
vkr_renderer_release_instance_state(VkrRendererFrontendHandle renderer,
                                    VkrPipelineOpaqueHandle pipeline,
                                    VkrRendererInstanceStateHandle handle);

VkrRendererError vkr_renderer_upload_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data);
// --- END Data Update ---

// --- START Render Pass & Target Management ---
void vkr_renderer_renderpass_destroy(VkrRendererFrontendHandle renderer,
                                     VkrRenderPassHandle pass);
VkrRenderPassHandle
vkr_renderer_renderpass_get(VkrRendererFrontendHandle renderer, String8 name);

/**
 * @brief Retrieve the signature of a render pass for compatibility checking.
 *
 * @param renderer The renderer frontend handle
 * @param pass The render pass handle
 * @param out_signature Output signature struct (must not be NULL)
 * @return true if signature was retrieved, false if pass is invalid
 */
bool8_t vkr_renderer_renderpass_get_signature(VkrRendererFrontendHandle renderer,
                                              VkrRenderPassHandle pass,
                                              VkrRenderPassSignature *out_signature);

/**
 * @brief Compare two render pass signatures for compatibility.
 *
 * Two signatures are compatible if they have the same attachment count,
 * formats, and sample counts. This is required for framebuffer reuse
 * and pipeline compatibility.
 *
 * @param a First signature
 * @param b Second signature
 * @return true if signatures are compatible, false otherwise
 */
bool8_t vkr_renderpass_signature_compatible(const VkrRenderPassSignature *a,
                                            const VkrRenderPassSignature *b);

/**
 * @brief Policy for domain render pass override.
 */
typedef enum VkrDomainOverridePolicy {
  /** Require new pass signature to be compatible with current domain pass */
  VKR_DOMAIN_OVERRIDE_POLICY_REQUIRE_COMPATIBLE = 0,
  /** Force override even if signatures are incompatible (invalidates cache) */
  VKR_DOMAIN_OVERRIDE_POLICY_FORCE,
} VkrDomainOverridePolicy;

/**
 * @brief Override the render pass used for a specific pipeline domain.
 *
 * Allows replacing the built-in domain render pass with a custom one.
 * Useful for custom render pass configurations (e.g., different clear ops).
 *
 * @param renderer The renderer frontend handle
 * @param domain The pipeline domain to override
 * @param pass The new render pass to use for this domain
 * @param policy Override policy (REQUIRE_COMPATIBLE or FORCE)
 * @param out_error Optional output for error details
 * @return true if override succeeded, false otherwise
 */
bool8_t vkr_renderer_domain_renderpass_set(VkrRendererFrontendHandle renderer,
                                           VkrPipelineDomain domain,
                                           VkrRenderPassHandle pass,
                                           VkrDomainOverridePolicy policy,
                                           VkrRendererError *out_error);

/**
 * @brief Create a render pass from an explicit descriptor.
 *
 * This function creates a render pass with full control over attachment
 * configurations, including load/store ops, layouts, and clear values.
 * Use this for custom render passes that don't fit the standard domain patterns.
 *
 * @param renderer The renderer frontend handle
 * @param desc Render pass descriptor with explicit attachment configuration
 * @param out_error Optional output for error details
 * @return Handle to the created render pass, or NULL on failure
 */
VkrRenderPassHandle
vkr_renderer_renderpass_create_desc(VkrRendererFrontendHandle renderer,
                                    const VkrRenderPassDesc *desc,
                                    VkrRendererError *out_error);

/**
 * @brief Create a render target with extended mip/layer addressing.
 *
 * This function creates a render target that can address specific mip levels
 * and array layers of texture attachments. Useful for rendering to:
 * - Specific mip levels during mip chain generation
 * - Individual cubemap faces
 * - Texture array slices
 *
 * @param renderer The renderer frontend handle
 * @param desc Extended render target descriptor
 * @param pass Render pass this target will be used with
 * @param out_error Optional output for error details
 * @return Handle to the created render target, or NULL on failure
 */
VkrRenderTargetHandle
vkr_renderer_render_target_create(VkrRendererFrontendHandle renderer,
                                   const VkrRenderTargetDesc *desc,
                                   VkrRenderPassHandle pass,
                                   VkrRendererError *out_error);

void vkr_renderer_render_target_destroy(VkrRendererFrontendHandle renderer,
                                        VkrRenderTargetHandle target);
VkrTextureOpaqueHandle
vkr_renderer_window_attachment_get(VkrRendererFrontendHandle renderer,
                                   uint32_t image_index);
VkrTextureOpaqueHandle
vkr_renderer_depth_attachment_get(VkrRendererFrontendHandle renderer);
uint32_t
vkr_renderer_window_attachment_count(VkrRendererFrontendHandle renderer);
uint32_t vkr_renderer_window_image_index(VkrRendererFrontendHandle renderer);
// --- END Render Pass & Target Management ---

// --- START View / Layer System ---
bool32_t vkr_view_system_init(VkrRendererFrontendHandle renderer);
void vkr_view_system_shutdown(VkrRendererFrontendHandle renderer);
bool32_t vkr_view_system_register_layer(VkrRendererFrontendHandle renderer,
                                        const VkrLayerConfig *cfg,
                                        VkrLayerHandle *out_handle,
                                        VkrRendererError *out_error);
void vkr_view_system_unregister_layer(VkrRendererFrontendHandle renderer,
                                      VkrLayerHandle handle);
bool32_t vkr_view_system_set_layer_camera(VkrRendererFrontendHandle renderer,
                                          VkrLayerHandle handle,
                                          const Mat4 *view,
                                          const Mat4 *projection);
void vkr_view_system_on_resize(VkrRendererFrontendHandle renderer,
                               uint32_t width, uint32_t height);
void vkr_view_system_rebuild_targets(VkrRendererFrontendHandle renderer);
void vkr_view_system_update_all(VkrRendererFrontendHandle renderer,
                                float64_t delta_time);
void vkr_view_system_draw_all(VkrRendererFrontendHandle renderer,
                              float64_t delta_time, uint32_t image_index);

void vkr_view_system_set_layer_enabled(VkrRendererFrontendHandle renderer,
                                       VkrLayerHandle handle, bool8_t enabled);
bool8_t vkr_view_system_is_layer_enabled(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle handle);

void vkr_view_system_set_modal_focus(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle handle);
void vkr_view_system_clear_modal_focus(VkrRendererFrontendHandle renderer);
VkrLayerHandle
vkr_view_system_get_modal_focus(VkrRendererFrontendHandle renderer);

/**
 * @brief Send a typed message to a layer with optional response.
 *
 * Type-safe messaging API for inter-layer communication. Validates message
 * kind, version, and payload size in debug builds.
 *
 * @param renderer The renderer frontend handle.
 * @param target The target layer handle.
 * @param msg Pointer to message (header + payload).
 * @param out_rsp Buffer for typed response (NULL if none expected).
 * @param out_rsp_capacity Size of out_rsp buffer in bytes.
 * @param out_rsp_size Actual response size written (optional).
 * @return true on success, false on failure.
 */
bool32_t vkr_view_system_send_msg(VkrRendererFrontendHandle renderer,
                                  VkrLayerHandle target,
                                  const VkrLayerMsgHeader *msg, void *out_rsp,
                                  uint64_t out_rsp_capacity,
                                  uint64_t *out_rsp_size);

/**
 * @brief Send a typed message without expecting a response.
 *
 * Convenience wrapper for fire-and-forget messages.
 *
 * @param renderer The renderer frontend handle.
 * @param target The target layer handle.
 * @param msg Pointer to message (header + payload).
 * @return true on success, false on failure.
 */
bool32_t vkr_view_system_send_msg_no_rsp(VkrRendererFrontendHandle renderer,
                                         VkrLayerHandle target,
                                         const VkrLayerMsgHeader *msg);

/**
 * @brief Broadcast a typed message to all layers matching flags.
 *
 * @param renderer The renderer frontend handle.
 * @param msg Pointer to message (header + payload).
 * @param flags_filter Only layers with matching flags receive the message.
 */
void vkr_view_system_broadcast_msg(VkrRendererFrontendHandle renderer,
                                   const VkrLayerMsgHeader *msg,
                                   uint32_t flags_filter);

VkrLayerBehaviorHandle vkr_view_system_attach_behavior(
    VkrRendererFrontendHandle renderer, VkrLayerHandle layer_handle,
    const VkrLayerBehavior *behavior, VkrRendererError *out_error);
void vkr_view_system_detach_behavior(VkrRendererFrontendHandle renderer,
                                     VkrLayerHandle layer_handle,
                                     VkrLayerBehaviorHandle behavior_handle);
void *vkr_view_system_get_behavior_data(VkrRendererFrontendHandle renderer,
                                        VkrLayerHandle layer_handle,
                                        VkrLayerBehaviorHandle behavior_handle);
// --- END View / Layer System ---

// --- START Frame Lifecycle & Rendering Commands ---
VkrRendererError vkr_renderer_begin_frame(VkrRendererFrontendHandle renderer,
                                          float64_t delta_time);

void vkr_renderer_resize(VkrRendererFrontendHandle renderer, uint32_t width,
                         uint32_t height);

// Bind a vertex buffer (most common case)
void vkr_renderer_bind_vertex_buffer(VkrRendererFrontendHandle renderer,
                                     const VkrVertexBufferBinding *binding);

void vkr_renderer_bind_index_buffer(VkrRendererFrontendHandle renderer,
                                    const VkrIndexBufferBinding *binding);

void vkr_renderer_set_viewport(VkrRendererFrontendHandle renderer,
                               const VkrViewport *viewport);

void vkr_renderer_set_scissor(VkrRendererFrontendHandle renderer,
                              const VkrScissor *scissor);

/**
 * @brief Set Vulkan rasterization depth-bias parameters for subsequent draws.
 *
 * This is used by the shadow pass to reduce self-shadowing acne by biasing
 * shadow caster depth values. This is rasterization bias (vkCmdSetDepthBias),
 * not receiver-side bias (shadow_bias / normal_bias in shaders).
 */
void vkr_renderer_set_depth_bias(VkrRendererFrontendHandle renderer,
                                 float32_t constant_factor, float32_t clamp,
                                 float32_t slope_factor);

// High-level draw of current scene graph (uses configured systems)
void vkr_renderer_draw_frame(VkrRendererFrontendHandle renderer,
                             float64_t delta_time);

void vkr_renderer_draw(VkrRendererFrontendHandle renderer,
                       uint32_t vertex_count, uint32_t instance_count,
                       uint32_t first_vertex, uint32_t first_instance);

void vkr_renderer_draw_indexed(VkrRendererFrontendHandle renderer,
                               uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance);

void vkr_renderer_draw_indexed_indirect(VkrRendererFrontendHandle renderer,
                                        VkrBufferHandle indirect_buffer,
                                        uint64_t offset, uint32_t draw_count,
                                        uint32_t stride);

VkrRendererError
vkr_renderer_begin_render_pass(VkrRendererFrontendHandle renderer,
                               VkrRenderPassHandle pass,
                               VkrRenderTargetHandle target);

VkrRendererError
vkr_renderer_end_render_pass(VkrRendererFrontendHandle renderer);

VkrRendererError vkr_renderer_end_frame(VkrRendererFrontendHandle renderer,
                                        float64_t delta_time);
// --- END Frame Lifecycle & Rendering Commands ---

// Returns and resets backend descriptor writes avoided counter for the frame
uint64_t vkr_renderer_get_and_reset_descriptor_writes_avoided(
    VkrRendererFrontendHandle renderer);

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
 * @brief Request an asynchronous pixel readback from a texture.
 *
 * The readback is performed asynchronously with 1-frame latency.
 * Call vkr_renderer_get_pixel_readback_result() on the next frame
 * to retrieve the result.
 *
 * @param renderer The renderer frontend handle
 * @param texture The texture to read from (must have TRANSFER_SRC usage)
 * @param x X coordinate of the pixel to read
 * @param y Y coordinate of the pixel to read
 * @return VKR_RENDERER_ERROR_NONE on success
 */
VkrRendererError
vkr_renderer_request_pixel_readback(VkrRendererFrontendHandle renderer,
                                    VkrTextureOpaqueHandle texture, uint32_t x,
                                    uint32_t y);

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

/**
 * @brief Check and update the readback ring state.
 *
 * Called automatically during end_frame, but can be called manually
 * to poll for completed readbacks.
 *
 * @param renderer The renderer frontend handle
 */
void vkr_renderer_update_readback_ring(VkrRendererFrontendHandle renderer);

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

// ============================================================================
// Backend Interface (Implemented by each backend, e.g., Vulkan)
// ============================================================================

/*
    The frontend will hold a pointer to this struct, populated by the
    chosen backend implementation. `backend_state` is a pointer to the
    backend's internal context (e.g., Vulkan device, queues, etc.).
*/
typedef struct VkrRendererBackendInterface {
  // --- Lifecycle ---
  // `config` can be specific to the backend, or a generic struct
  // `window_handle` is platform specific (HWND, xcb_window_t, etc.)
  bool32_t (*initialize)(
      void **out_backend_state, // Backend allocates and returns its state
      VkrRendererBackendType type, VkrWindow *window, uint32_t initial_width,
      uint32_t initial_height, VkrDeviceRequirements *device_requirements,
      const VkrRendererBackendConfig *backend_config);
  void (*shutdown)(void *backend_state);
  void (*on_resize)(void *backend_state, uint32_t new_width,
                    uint32_t new_height);
  void (*get_device_information)(void *backend_state,
                                 VkrDeviceInformation *device_information,
                                 Arena *temp_arena);

  // --- Synchronization ---
  VkrRendererError (*wait_idle)(void *backend_state); // Wait for GPU to be idle

  // --- Frame Management ---
  VkrRendererError (*begin_frame)(void *backend_state, float64_t delta_time);
  VkrRendererError (*end_frame)(void *backend_state,
                                float64_t delta_time); // Includes present

  // --- Render Pass Management ---
  VkrRenderPassHandle (*renderpass_create_desc)(void *backend_state,
                                                const VkrRenderPassDesc *desc,
                                                VkrRendererError *out_error);
  void (*renderpass_destroy)(void *backend_state, VkrRenderPassHandle pass);
  VkrRenderPassHandle (*renderpass_get)(void *backend_state, const char *name);
  bool8_t (*domain_renderpass_set)(void *backend_state, VkrPipelineDomain domain,
                                   VkrRenderPassHandle pass,
                                   VkrDomainOverridePolicy policy,
                                   VkrRendererError *out_error);
  VkrRenderTargetHandle (*render_target_create)(void *backend_state,
                                                 const VkrRenderTargetDesc *desc,
                                                 VkrRenderPassHandle pass,
                                                 VkrRendererError *out_error);
  void (*render_target_destroy)(void *backend_state,
                                VkrRenderTargetHandle target);
  VkrRendererError (*begin_render_pass)(void *backend_state,
                                        VkrRenderPassHandle pass,
                                        VkrRenderTargetHandle target);
  VkrRendererError (*end_render_pass)(void *backend_state);
  VkrTextureOpaqueHandle (*window_attachment_get)(void *backend_state,
                                                  uint32_t image_index);
  VkrTextureOpaqueHandle (*depth_attachment_get)(void *backend_state);
  uint32_t (*window_attachment_count_get)(void *backend_state);
  uint32_t (*window_attachment_index_get)(void *backend_state);

  // --- Resource Management ---
  VkrBackendResourceHandle (*buffer_create)(void *backend_state,
                                            const VkrBufferDescription *desc,
                                            const void *initial_data);
  void (*buffer_destroy)(void *backend_state, VkrBackendResourceHandle handle);
  VkrRendererError (*buffer_update)(void *backend_state,
                                    VkrBackendResourceHandle handle,
                                    uint64_t offset, uint64_t size,
                                    const void *data);
  VkrRendererError (*buffer_upload)(void *backend_state,
                                    VkrBackendResourceHandle handle,
                                    uint64_t offset, uint64_t size,
                                    const void *data);
  void *(*buffer_get_mapped_ptr)(void *backend_state,
                                 VkrBackendResourceHandle handle);
  VkrRendererError (*buffer_flush)(void *backend_state,
                                   VkrBackendResourceHandle handle,
                                   uint64_t offset, uint64_t size);

  VkrBackendResourceHandle (*texture_create)(void *backend_state,
                                             const VkrTextureDescription *desc,
                                             const void *initial_data);
  VkrBackendResourceHandle (*render_target_texture_create)(
      void *backend_state, const VkrRenderTargetTextureDesc *desc);
  VkrBackendResourceHandle (*depth_attachment_create)(void *backend_state,
                                                      uint32_t width,
                                                      uint32_t height);
  VkrBackendResourceHandle (*sampled_depth_attachment_create)(
      void *backend_state, uint32_t width, uint32_t height);
  VkrBackendResourceHandle (*render_target_texture_msaa_create)(
      void *backend_state, uint32_t width, uint32_t height,
      VkrTextureFormat format, VkrSampleCount samples);
  VkrRendererError (*texture_transition_layout)(void *backend_state,
                                                VkrBackendResourceHandle handle,
                                                VkrTextureLayout old_layout,
                                                VkrTextureLayout new_layout);
  VkrRendererError (*texture_update)(void *backend_state,
                                     VkrBackendResourceHandle handle,
                                     const VkrTextureDescription *desc);
  VkrRendererError (*texture_write)(void *backend_state,
                                    VkrBackendResourceHandle handle,
                                    const VkrTextureWriteRegion *region,
                                    const void *data, uint64_t size);
  VkrRendererError (*texture_resize)(void *backend_state,
                                     VkrBackendResourceHandle handle,
                                     uint32_t new_width, uint32_t new_height,
                                     bool8_t preserve_contents);
  void (*texture_destroy)(void *backend_state, VkrBackendResourceHandle handle);

  // Pipeline creation uses VertexInputAttributeDescription and
  // VertexInputBindingDescription from GraphicsPipelineDescription to
  // configure the vertex input layout. Runtime vertex buffer bindings
  // must reference the binding numbers defined in these descriptions.
  VkrBackendResourceHandle (*graphics_pipeline_create)(
      void *backend_state, const VkrGraphicsPipelineDescription *description);
  VkrRendererError (*pipeline_update_state)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      const void *global_uniform_data, const VkrShaderStateObject *data,
      const VkrRendererMaterialState *material);
  void (*pipeline_destroy)(void *backend_state,
                           VkrBackendResourceHandle pipeline_handle);

  // Instance state management
  VkrRendererError (*instance_state_acquire)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      VkrRendererInstanceStateHandle *out_handle);
  VkrRendererError (*instance_state_release)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      VkrRendererInstanceStateHandle handle);

  void (*bind_buffer)(void *backend_state,
                      VkrBackendResourceHandle buffer_handle, uint64_t offset);

  void (*set_viewport)(void *backend_state, const VkrViewport *viewport);
  void (*set_scissor)(void *backend_state, const VkrScissor *scissor);
  void (*set_depth_bias)(void *backend_state, float32_t constant_factor,
                         float32_t clamp, float32_t slope_factor);

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);

  void (*draw_indexed)(void *backend_state, uint32_t index_count,
                       uint32_t instance_count, uint32_t first_index,
                       int32_t vertex_offset, uint32_t first_instance);

  void (*draw_indexed_indirect)(void *backend_state,
                                VkrBackendResourceHandle indirect_buffer,
                                uint64_t offset, uint32_t draw_count,
                                uint32_t stride);

  void (*set_instance_buffer)(void *backend_state,
                              VkrBackendResourceHandle buffer_handle);

  // Telemetry
  uint64_t (*get_and_reset_descriptor_writes_avoided)(void *backend_state);

  // --- Pixel Readback ---
  VkrRendererError (*readback_ring_init)(void *backend_state);
  void (*readback_ring_shutdown)(void *backend_state);
  VkrRendererError (*request_pixel_readback)(void *backend_state,
                                             VkrBackendResourceHandle texture,
                                             uint32_t x, uint32_t y);
  VkrRendererError (*get_pixel_readback_result)(void *backend_state,
                                                VkrPixelReadbackResult *result);
  void (*update_readback_ring)(void *backend_state);

  // Utility functions
  VkrAllocator *(*get_allocator)(void *backend_state);

  // Set the default 2D texture used as fallback for empty sampler slots
  void (*set_default_2d_texture)(void *backend_state,
                                 VkrTextureOpaqueHandle texture);
} VkrRendererBackendInterface;
