#pragma once

#include "containers/bitset.h"
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
typedef struct s_Pipeline *VkrPipelineHandle;
typedef struct s_TextureHandle *VkrTextureHandle;

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
  VKR_TEXTURE_PROPERTY_FILTER_LINEAR_BIT = 1 << 0,
  VKR_TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT = 1 << 1,
  VKR_TEXTURE_PROPERTY_FILTER_MIPMAP_BIT = 1 << 2,
  VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT = 1 << 3,
} VkrTexturePropertyBits;
typedef Bitset8 VkrTexturePropertyFlags;

vkr_internal INLINE VkrTexturePropertyFlags
vkr_texture_property_flags_create(void) {
  return bitset8_create();
}

vkr_internal INLINE VkrTexturePropertyFlags
vkr_texture_property_flags_from_bits(uint8_t bits) {
  VkrTexturePropertyFlags flags = bitset8_create();
  if (bits & VKR_TEXTURE_PROPERTY_FILTER_LINEAR_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_FILTER_LINEAR_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_FILTER_MIPMAP_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_FILTER_MIPMAP_BIT);
  if (bits & VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT)
    bitset8_set(&flags, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
  return flags;
}

typedef struct VkrTextureDescription {
  uint32_t id;
  uint32_t width;
  uint32_t height;
  uint32_t generation;
  uint32_t channels;

  VkrTextureType type;
  VkrTextureFormat format;
  VkrTexturePropertyFlags properties;
} VkrTextureDescription;

// ----------------------------------------------------------------------------
// Local state & material state
// ----------------------------------------------------------------------------

typedef struct VkrRendererLocalStateHandle {
  uint32_t id;
} VkrRendererLocalStateHandle;

// Used to create a single global uniform object for the entire scene
// This is used to pass the MVP matrix to the shader
typedef struct VkrGlobalUniformObject {
  Mat4 view;
  Mat4 projection;
  // Padding to align to 256 bytes (required by Nvidia GPUs)
  uint8_t padding[128];
} VkrGlobalUniformObject;

// Used to pass the object's properties to the shader
typedef struct VkrLocalUniformObject {
  Vec4 diffuse_color;
  // Padding to align to 256 bytes (required by Nvidia GPUs)
  uint8_t padding[256 - sizeof(Vec4)];
} VkrLocalUniformObject;

/*
  Vulkan backend descriptor layout (current)
  - Descriptor set 0 (per-frame/global):
      binding 0 = uniform buffer (GlobalUniformObject: view, projection)
  - Descriptor set 1 (per-object/local):
      binding 0 = uniform buffer (LocalUniformObject: material uniforms)
      binding 1 = sampled image (combined image sampler slot 0)
      binding 2 = sampler (slot 0)

  Notes:
  - Materials currently bind exactly 1 texture (base color) via slot 0.
  - Additional textures (normal/metallic/emissive) are not yet exposed; future
    work may extend set 1 or use descriptor arrays.
*/

typedef struct VkrShaderStateObject {
  Mat4 model;
  // Local state management: hidden behind a typed handle.
  VkrRendererLocalStateHandle local_state;
} VkrShaderStateObject;

typedef struct VkrRendererMaterialState {
  // Per-material uniforms for the local UBO
  VkrLocalUniformObject uniforms;
  // Current base color texture for slot 0 (may be NULL when disabled)
  VkrTextureHandle texture0;
  bool8_t texture0_enabled;
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

  VkrGlobalUniformObject global_uniform_object;
  VkrShaderStateObject shader_state_object;
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

  VkrPipelineDomain domain;
} VkrGraphicsPipelineDescription;

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
VkrRendererFrontendHandle vkr_renderer_create(
    Arena *arena, VkrRendererBackendType backend_type, VkrWindow *window,
    VkrDeviceRequirements *device_requirements, VkrRendererError *out_error);

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

VkrTextureHandle
vkr_renderer_create_texture(VkrRendererFrontendHandle renderer,
                            const VkrTextureDescription *description,
                            const void *initial_data,
                            VkrRendererError *out_error);

void vkr_renderer_destroy_texture(VkrRendererFrontendHandle renderer,
                                  VkrTextureHandle texture);

void vkr_renderer_destroy_buffer(VkrRendererFrontendHandle renderer,
                                 VkrBufferHandle buffer);

VkrPipelineHandle vkr_renderer_create_graphics_pipeline(
    VkrRendererFrontendHandle renderer,
    const VkrGraphicsPipelineDescription *description,
    VkrRendererError *out_error);

void vkr_renderer_destroy_pipeline(VkrRendererFrontendHandle renderer,
                                   VkrPipelineHandle pipeline);
// --- END Resource Management ---

// --- START Data Update ---
VkrRendererError vkr_renderer_update_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data);

VkrRendererError vkr_renderer_update_pipeline_state(
    VkrRendererFrontendHandle renderer, VkrPipelineHandle pipeline,
    const VkrGlobalUniformObject *uniform, const VkrShaderStateObject *data,
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
                                 VkrPipelineHandle pipeline,
                                 const VkrGlobalUniformObject *uniform);

/**
 * @brief Update only the per-object local state (e.g., model matrix, material
 * uniforms, textures). Call per renderable.
 *
 * @param renderer
 * @param pipeline
 * @param data
 * @return VkrRendererError
 */
VkrRendererError vkr_renderer_update_local_state(
    VkrRendererFrontendHandle renderer, VkrPipelineHandle pipeline,
    const VkrShaderStateObject *data, const VkrRendererMaterialState *material);

// Local state lifetime
VkrRendererError
vkr_renderer_acquire_local_state(VkrRendererFrontendHandle renderer,
                                 VkrPipelineHandle pipeline,
                                 VkrRendererLocalStateHandle *out_handle);

VkrRendererError
vkr_renderer_release_local_state(VkrRendererFrontendHandle renderer,
                                 VkrPipelineHandle pipeline,
                                 VkrRendererLocalStateHandle handle);

VkrRendererError vkr_renderer_upload_buffer(VkrRendererFrontendHandle renderer,
                                            VkrBufferHandle buffer,
                                            uint64_t offset, uint64_t size,
                                            const void *data);
// --- END Data Update ---

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

void vkr_renderer_draw(VkrRendererFrontendHandle renderer,
                       uint32_t vertex_count, uint32_t instance_count,
                       uint32_t first_vertex, uint32_t first_instance);

void vkr_renderer_draw_indexed(VkrRendererFrontendHandle renderer,
                               uint32_t index_count, uint32_t instance_count,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance);

VkrRendererError vkr_renderer_end_frame(VkrRendererFrontendHandle renderer,
                                        float64_t delta_time);
// --- END Frame Lifecycle & Rendering Commands ---

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
      uint32_t initial_height, VkrDeviceRequirements *device_requirements);
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
  void (*texture_destroy)(void *backend_state, VkrBackendResourceHandle handle);

  // Pipeline creation uses VertexInputAttributeDescription and
  // VertexInputBindingDescription from GraphicsPipelineDescription to
  // configure the vertex input layout. Runtime vertex buffer bindings
  // must reference the binding numbers defined in these descriptions.
  VkrBackendResourceHandle (*graphics_pipeline_create)(
      void *backend_state, const VkrGraphicsPipelineDescription *description);
  VkrRendererError (*pipeline_update_state)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      const VkrGlobalUniformObject *uniform, const VkrShaderStateObject *data,
      const VkrRendererMaterialState *material);
  void (*pipeline_destroy)(void *backend_state,
                           VkrBackendResourceHandle pipeline_handle);

  // Local state management
  VkrRendererError (*local_state_acquire)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      VkrRendererLocalStateHandle *out_handle);
  VkrRendererError (*local_state_release)(
      void *backend_state, VkrBackendResourceHandle pipeline_handle,
      VkrRendererLocalStateHandle handle);

  void (*bind_buffer)(void *backend_state,
                      VkrBackendResourceHandle buffer_handle, uint64_t offset);

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);

  void (*draw_indexed)(void *backend_state, uint32_t index_count,
                       uint32_t instance_count, uint32_t first_index,
                       int32_t vertex_offset, uint32_t first_instance);
} VkrRendererBackendInterface;
