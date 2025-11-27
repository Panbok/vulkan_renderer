#pragma once

#include "containers/bitset.h"
#include "core/event.h"
#include "core/vkr_window.h"
#include "defines.h"
#include "math/mat.h"

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

typedef enum VkrBufferUsageBits {
  VKR_BUFFER_USAGE_NONE = 0,
  VKR_BUFFER_USAGE_VERTEX_BUFFER = 1 << 0,
  VKR_BUFFER_USAGE_INDEX_BUFFER = 1 << 1,
  VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER = 1 << 2,
  VKR_BUFFER_USAGE_UNIFORM = 1 << 3,
  VKR_BUFFER_USAGE_STORAGE = 1 << 4,      // For compute/more advanced
  VKR_BUFFER_USAGE_TRANSFER_SRC = 1 << 5, // Can be source of a copy
  VKR_BUFFER_USAGE_TRANSFER_DST = 1 << 6, // Can be destination of a copy
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
  VKR_TEXTURE_FORMAT_R8G8B8A8_UINT,
  VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM,
  VKR_TEXTURE_FORMAT_R8G8B8A8_SINT,
  // Single/dual channel formats
  VKR_TEXTURE_FORMAT_R8_UNORM,
  VKR_TEXTURE_FORMAT_R16_SFLOAT,
  VKR_TEXTURE_FORMAT_R32_SFLOAT,
  VKR_TEXTURE_FORMAT_R8G8_UNORM,
  // Depth/stencil formats
  VKR_TEXTURE_FORMAT_D32_SFLOAT,
  VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT,

  VKR_TEXTURE_FORMAT_COUNT,
} VkrTextureFormat;

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

typedef struct VkrTextureDescription {
  uint32_t id;
  uint32_t width;
  uint32_t height;
  uint32_t generation;
  uint32_t channels;

  VkrTextureType type;
  VkrTextureFormat format;
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
} VkrLocalMaterialState;

typedef struct VkrRendererInstanceStateHandle {
  uint32_t id;
} VkrRendererInstanceStateHandle;

/*
  Vulkan backend descriptor layout (current)
  - Descriptor set 0 (per-frame/global):
      binding 0 = uniform buffer (GlobalUniformObject: view, projection)
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

typedef enum VkrRenderPassClearFlags {
  VKR_RENDERPASS_CLEAR_NONE = 0,
  VKR_RENDERPASS_CLEAR_COLOR = 1 << 0,
  VKR_RENDERPASS_CLEAR_DEPTH = 1 << 1,
  VKR_RENDERPASS_CLEAR_STENCIL = 1 << 2
} VkrRenderPassClearFlags;

typedef struct VkrRenderPassConfig {
  String8 name;
  String8 prev_name;
  String8 next_name;
  Vec4 render_area;
  Vec4 clear_color;
  uint8_t clear_flags;
} VkrRenderPassConfig;

typedef struct VkrRenderTargetDesc {
  bool8_t sync_to_window_size;
  uint8_t attachment_count;
  VkrTextureOpaqueHandle *attachments;
  uint32_t width;
  uint32_t height;
} VkrRenderTargetDesc;

typedef enum VkrPipelineDomain {
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  VKR_PIPELINE_DOMAIN_SHADOW = 2,
  VKR_PIPELINE_DOMAIN_POST = 3,
  VKR_PIPELINE_DOMAIN_COMPUTE = 4,
  VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT = 5,

  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;

typedef struct VkrGraphicsPipelineDescription {
  VkrShaderObjectDescription shader_object_description;

  uint32_t attribute_count;
  VkrVertexInputAttributeDescription *attributes;
  uint32_t binding_count;
  VkrVertexInputBindingDescription *bindings;

  VkrPrimitiveTopology topology;

  VkrPolygonMode polygon_mode;

  VkrRenderPassHandle renderpass;
  VkrPipelineDomain domain;
} VkrGraphicsPipelineDescription;

typedef struct VkrRendererBackendConfig {
  const char *application_name;
  uint16_t renderpass_count;
  VkrRenderPassConfig *pass_configs;
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
  String8 renderpass_name; // Active renderpass name for this callback
} VkrLayerRenderInfo;

typedef struct VkrLayerCallbacks {
  bool32_t (*on_create)(
      VkrLayerContext *ctx);               // optional, return false on failure
  void (*on_attach)(VkrLayerContext *ctx); // Optional
  void (*on_resize)(VkrLayerContext *ctx, uint32_t width, uint32_t height);
  void (*on_render)(VkrLayerContext *ctx, const VkrLayerRenderInfo *info);
  void (*on_detach)(VkrLayerContext *ctx);  // Optional
  void (*on_destroy)(VkrLayerContext *ctx); // Optional
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
} VkrLayerConfig;

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
                                 VkrRendererError *out_error);

bool32_t vkr_renderer_systems_initialize(VkrRendererFrontendHandle renderer);

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

VkrTextureOpaqueHandle
vkr_renderer_create_texture(VkrRendererFrontendHandle renderer,
                            const VkrTextureDescription *description,
                            const void *initial_data,
                            VkrRendererError *out_error);
VkrTextureOpaqueHandle
vkr_renderer_create_writable_texture(VkrRendererFrontendHandle renderer,
                                     const VkrTextureDescription *desc,
                                     VkrRendererError *out_error);

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
VkrRenderPassHandle
vkr_renderer_renderpass_create(VkrRendererFrontendHandle renderer,
                               const VkrRenderPassConfig *cfg);
void vkr_renderer_renderpass_destroy(VkrRendererFrontendHandle renderer,
                                     VkrRenderPassHandle pass);
VkrRenderPassHandle
vkr_renderer_renderpass_get(VkrRendererFrontendHandle renderer, String8 name);

VkrRenderTargetHandle
vkr_renderer_render_target_create(VkrRendererFrontendHandle renderer,
                                  const VkrRenderTargetDesc *desc,
                                  VkrRenderPassHandle pass);
void vkr_renderer_render_target_destroy(VkrRendererFrontendHandle renderer,
                                        VkrRenderTargetHandle target,
                                        bool8_t free_internal_memory);
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
void vkr_view_system_draw_all(VkrRendererFrontendHandle renderer,
                              uint32_t image_index);
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

// High-level draw of current scene graph (uses configured systems)
void vkr_renderer_draw_frame(VkrRendererFrontendHandle renderer);

void vkr_renderer_draw(VkrRendererFrontendHandle renderer,
                       uint32_t vertex_count, uint32_t instance_count,
                       uint32_t first_vertex, uint32_t first_instance);

void vkr_renderer_draw_indexed(VkrRendererFrontendHandle renderer,
                               uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance);

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
  VkrRenderPassHandle (*renderpass_create)(void *backend_state,
                                           const VkrRenderPassConfig *cfg);
  void (*renderpass_destroy)(void *backend_state, VkrRenderPassHandle pass);
  VkrRenderPassHandle (*renderpass_get)(void *backend_state, const char *name);
  VkrRenderTargetHandle (*render_target_create)(void *backend_state,
                                                const VkrRenderTargetDesc *desc,
                                                VkrRenderPassHandle pass);
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

  VkrBackendResourceHandle (*texture_create)(void *backend_state,
                                             const VkrTextureDescription *desc,
                                             const void *initial_data);
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

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);

  void (*draw_indexed)(void *backend_state, uint32_t index_count,
                       uint32_t instance_count, uint32_t first_index,
                       int32_t vertex_offset, uint32_t first_instance);

  // Telemetry
  uint64_t (*get_and_reset_descriptor_writes_avoided)(void *backend_state);
} VkrRendererBackendInterface;
