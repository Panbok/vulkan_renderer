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

typedef struct s_RendererFrontend *RendererFrontendHandle;
typedef struct s_BufferResource *BufferHandle;
typedef struct s_Pipeline *PipelineHandle;
typedef struct s_TextureHandle *TextureHandle;

typedef union {
  void *ptr;
  uint64_t id;
  struct {
    uint32_t type;
    uint32_t index;
  } typed;
} BackendResourceHandle;

typedef enum RendererBackendType {
  RENDERER_BACKEND_TYPE_VULKAN,
  RENDERER_BACKEND_TYPE_DX12,  // Future
  RENDERER_BACKEND_TYPE_METAL, // Future
  RENDERER_BACKEND_TYPE_COUNT
} RendererBackendType;

typedef enum RendererError {
  RENDERER_ERROR_NONE = 0,
  RENDERER_ERROR_UNKNOWN,
  RENDERER_ERROR_INITIALIZATION_FAILED,
  RENDERER_ERROR_BACKEND_NOT_SUPPORTED,
  RENDERER_ERROR_RESOURCE_CREATION_FAILED,
  RENDERER_ERROR_INVALID_HANDLE,
  RENDERER_ERROR_INVALID_PARAMETER,
  RENDERER_ERROR_SHADER_COMPILATION_FAILED,
  RENDERER_ERROR_OUT_OF_MEMORY,
  RENDERER_ERROR_COMMAND_RECORDING_FAILED,
  RENDERER_ERROR_FRAME_PREPARATION_FAILED,
  RENDERER_ERROR_PRESENTATION_FAILED,
  RENDERER_ERROR_FRAME_IN_PROGRESS,
  RENDERER_ERROR_DEVICE_ERROR,
  RENDERER_ERROR_PIPELINE_STATE_UPDATE_FAILED,
  RENDERER_ERROR_FILE_NOT_FOUND,
  RENDERER_ERROR_RESOURCE_NOT_LOADED,

  RENDERER_ERROR_COUNT
} RendererError;

typedef enum ShaderStageBits {
  SHADER_STAGE_NONE = 0,
  SHADER_STAGE_VERTEX_BIT = 1 << 0,
  SHADER_STAGE_FRAGMENT_BIT = 1 << 1,
  SHADER_STAGE_COMPUTE_BIT = 1 << 2,                 // Future
  SHADER_STAGE_GEOMETRY_BIT = 1 << 3,                // Future
  SHADER_STAGE_TESSELLATION_CONTROL_BIT = 1 << 4,    // Future
  SHADER_STAGE_TESSELLATION_EVALUATION_BIT = 1 << 5, // Future
  SHADER_STAGE_ALL_GRAPHICS =
      SHADER_STAGE_VERTEX_BIT | SHADER_STAGE_FRAGMENT_BIT |
      SHADER_STAGE_GEOMETRY_BIT | SHADER_STAGE_TESSELLATION_CONTROL_BIT |
      SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
} ShaderStageBits;
typedef Bitset8 ShaderStageFlags; // Assuming Bitset8 is sufficient for now

// ShaderStageFlags helper functions
static inline ShaderStageFlags shader_stage_flags_create(void) {
  return bitset8_create();
}

static inline ShaderStageFlags shader_stage_flags_from_bits(uint8_t bits) {
  ShaderStageFlags flags = bitset8_create();
  if (bits & SHADER_STAGE_VERTEX_BIT)
    bitset8_set(&flags, SHADER_STAGE_VERTEX_BIT);
  if (bits & SHADER_STAGE_FRAGMENT_BIT)
    bitset8_set(&flags, SHADER_STAGE_FRAGMENT_BIT);
  if (bits & SHADER_STAGE_COMPUTE_BIT)
    bitset8_set(&flags, SHADER_STAGE_COMPUTE_BIT);
  if (bits & SHADER_STAGE_GEOMETRY_BIT)
    bitset8_set(&flags, SHADER_STAGE_GEOMETRY_BIT);
  if (bits & SHADER_STAGE_TESSELLATION_CONTROL_BIT)
    bitset8_set(&flags, SHADER_STAGE_TESSELLATION_CONTROL_BIT);
  if (bits & SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
    bitset8_set(&flags, SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
  return flags;
}

#define SHADER_STAGE_FLAGS_VERTEX_FRAGMENT()                                   \
  shader_stage_flags_from_bits(SHADER_STAGE_VERTEX_BIT |                       \
                               SHADER_STAGE_FRAGMENT_BIT)

#define SHADER_STAGE_FLAGS_ALL_GRAPHICS()                                      \
  shader_stage_flags_from_bits(SHADER_STAGE_ALL_GRAPHICS)

typedef enum PrimitiveTopology {
  PRIMITIVE_TOPOLOGY_POINT_LIST,
  PRIMITIVE_TOPOLOGY_LINE_LIST,
  PRIMITIVE_TOPOLOGY_LINE_STRIP,
  PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
  PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, // Often not recommended
} PrimitiveTopology;

typedef enum VertexFormat {
  VERTEX_FORMAT_UNDEFINED = 0,
  VERTEX_FORMAT_R32_SFLOAT,
  VERTEX_FORMAT_R32G32_SFLOAT,
  VERTEX_FORMAT_R32G32B32_SFLOAT,
  VERTEX_FORMAT_R32G32B32A32_SFLOAT,
  VERTEX_FORMAT_R8G8B8A8_UNORM,
} VertexFormat;

typedef enum VertexInputRate {
  VERTEX_INPUT_RATE_VERTEX,
  VERTEX_INPUT_RATE_INSTANCE
} VertexInputRate;

typedef enum IndexType {
  INDEX_TYPE_UINT16,
  INDEX_TYPE_UINT32,
} IndexType;

typedef enum PolygonMode {
  POLYGON_MODE_FILL = 0,
  POLYGON_MODE_LINE,
  POLYGON_MODE_POINT,
} PolygonMode;

typedef enum BufferUsageBits {
  BUFFER_USAGE_NONE = 0,
  BUFFER_USAGE_VERTEX_BUFFER = 1 << 0,
  BUFFER_USAGE_INDEX_BUFFER = 1 << 1,
  BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER = 1 << 2,
  BUFFER_USAGE_UNIFORM = 1 << 3,
  BUFFER_USAGE_STORAGE = 1 << 4,      // For compute/more advanced
  BUFFER_USAGE_TRANSFER_SRC = 1 << 5, // Can be source of a copy
  BUFFER_USAGE_TRANSFER_DST = 1 << 6, // Can be destination of a copy
} BufferUsageBits;
typedef Bitset8 BufferUsageFlags;

// BufferUsageFlags helper functions
static inline BufferUsageFlags buffer_usage_flags_create(void) {
  return bitset8_create();
}

static inline BufferUsageFlags buffer_usage_flags_from_bits(uint8_t bits) {
  BufferUsageFlags flags = bitset8_create();
  if (bits & BUFFER_USAGE_VERTEX_BUFFER)
    bitset8_set(&flags, BUFFER_USAGE_VERTEX_BUFFER);
  if (bits & BUFFER_USAGE_INDEX_BUFFER)
    bitset8_set(&flags, BUFFER_USAGE_INDEX_BUFFER);
  if (bits & BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER)
    bitset8_set(&flags, BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER);
  if (bits & BUFFER_USAGE_UNIFORM)
    bitset8_set(&flags, BUFFER_USAGE_UNIFORM);
  if (bits & BUFFER_USAGE_STORAGE)
    bitset8_set(&flags, BUFFER_USAGE_STORAGE);
  if (bits & BUFFER_USAGE_TRANSFER_SRC)
    bitset8_set(&flags, BUFFER_USAGE_TRANSFER_SRC);
  if (bits & BUFFER_USAGE_TRANSFER_DST)
    bitset8_set(&flags, BUFFER_USAGE_TRANSFER_DST);
  return flags;
}

#define BUFFER_USAGE_FLAGS_VERTEX()                                            \
  buffer_usage_flags_from_bits(BUFFER_USAGE_VERTEX_BUFFER |                    \
                               BUFFER_USAGE_TRANSFER_DST)

#define BUFFER_USAGE_FLAGS_INDEX()                                             \
  buffer_usage_flags_from_bits(BUFFER_USAGE_INDEX_BUFFER |                     \
                               BUFFER_USAGE_TRANSFER_DST)

#define BUFFER_USAGE_FLAGS_UNIFORM()                                           \
  buffer_usage_flags_from_bits(BUFFER_USAGE_UNIFORM | BUFFER_USAGE_TRANSFER_DST)

typedef enum BufferTypeBits {
  BUFFER_TYPE_GRAPHICS = 1 << 0,
  BUFFER_TYPE_COMPUTE = 1 << 1,
  BUFFER_TYPE_TRANSFER = 1 << 2,
} BufferTypeBits;
typedef Bitset8 BufferTypeFlags;

typedef enum MemoryPropertyBits {
  MEMORY_PROPERTY_DEVICE_LOCAL = 1 << 0,  // GPU optimal memory
  MEMORY_PROPERTY_HOST_VISIBLE = 1 << 1,  // CPU can map
  MEMORY_PROPERTY_HOST_COHERENT = 1 << 2, // No explicit flush needed
  MEMORY_PROPERTY_HOST_CACHED = 1 << 3,   // CPU cacheable
} MemoryPropertyBits;
typedef Bitset8 MemoryPropertyFlags;

// MemoryPropertyFlags helper functions
static inline MemoryPropertyFlags memory_property_flags_create(void) {
  return bitset8_create();
}

static inline MemoryPropertyFlags
memory_property_flags_from_bits(uint8_t bits) {
  MemoryPropertyFlags flags = bitset8_create();
  if (bits & MEMORY_PROPERTY_DEVICE_LOCAL)
    bitset8_set(&flags, MEMORY_PROPERTY_DEVICE_LOCAL);
  if (bits & MEMORY_PROPERTY_HOST_VISIBLE)
    bitset8_set(&flags, MEMORY_PROPERTY_HOST_VISIBLE);
  if (bits & MEMORY_PROPERTY_HOST_COHERENT)
    bitset8_set(&flags, MEMORY_PROPERTY_HOST_COHERENT);
  if (bits & MEMORY_PROPERTY_HOST_CACHED)
    bitset8_set(&flags, MEMORY_PROPERTY_HOST_CACHED);
  return flags;
}

#define MEMORY_PROPERTY_FLAGS_DEVICE_LOCAL()                                   \
  memory_property_flags_from_bits(MEMORY_PROPERTY_DEVICE_LOCAL)

#define MEMORY_PROPERTY_FLAGS_HOST_VISIBLE()                                   \
  memory_property_flags_from_bits(MEMORY_PROPERTY_HOST_VISIBLE |               \
                                  MEMORY_PROPERTY_HOST_COHERENT)

// ============================================================================
// Device Resources
// ============================================================================
typedef enum DeviceTypeBits {
  DEVICE_TYPE_DISCRETE_BIT = 1 << 0,
  DEVICE_TYPE_INTEGRATED_BIT = 1 << 1,
  DEVICE_TYPE_VIRTUAL_BIT = 1 << 2,
  DEVICE_TYPE_CPU_BIT = 1 << 3,
} DeviceTypeBits;
typedef Bitset8 DeviceTypeFlags;

typedef enum DeviceQueueBits {
  DEVICE_QUEUE_GRAPHICS_BIT = 1 << 0,
  DEVICE_QUEUE_COMPUTE_BIT = 1 << 1,
  DEVICE_QUEUE_TRANSFER_BIT = 1 << 2,
  DEVICE_QUEUE_SPARSE_BINDING_BIT = 1 << 3,
  DEVICE_QUEUE_PROTECTED_BIT = 1 << 4,
  DEVICE_QUEUE_PRESENT_BIT = 1 << 5,
} DeviceQueueBits;
typedef Bitset8 DeviceQueueFlags;

typedef enum SamplerFilterBits {
  SAMPLER_FILTER_ANISOTROPIC_BIT = 1 << 0,
  SAMPLER_FILTER_LINEAR_BIT = 1 << 1,
} SamplerFilterBits;
typedef Bitset8 SamplerFilterFlags;

typedef struct DeviceRequirements {
  ShaderStageFlags supported_stages;
  DeviceQueueFlags supported_queues;
  DeviceTypeFlags allowed_device_types;
  SamplerFilterFlags supported_sampler_filters;
} DeviceRequirements;

typedef struct DeviceInformation {
  String8 device_name;
  String8 vendor_name;
  String8 driver_version;
  String8 api_version;
  uint64_t vram_size;
  uint64_t vram_local_size;
  uint64_t vram_shared_size;
  DeviceTypeFlags device_types;
  DeviceQueueFlags device_queues;
  SamplerFilterFlags sampler_filters;
} DeviceInformation;

// ============================================================================
// Resource Descriptions
// ============================================================================

typedef struct BufferDescription {
  uint64_t size;
  BufferUsageFlags usage;
  MemoryPropertyFlags memory_properties; // Hint for memory type
  // For staging, the frontend might create two buffers:
  // one HOST_VISIBLE for upload, one DEVICE_LOCAL for rendering.
  // Or the backend abstracts this.

  BufferTypeFlags buffer_type;

  bool8_t bind_on_create;
} BufferDescription;

typedef enum ShaderStage {
  SHADER_STAGE_VERTEX = 0,
  SHADER_STAGE_FRAGMENT = 1,
  // Future: geometry_shader, tess_control_shader, tess_eval_shader
  SHADER_STAGE_COUNT,
} ShaderStage;

typedef enum ShaderFileFormat {
  SHADER_FILE_FORMAT_SPIR_V = 0,
  SHADER_FILE_FORMAT_HLSL,
  SHADER_FILE_FORMAT_GLSL,
} ShaderFileFormat;

typedef enum ShaderFileType {
  SHADER_FILE_TYPE_SINGLE = 0,
  SHADER_FILE_TYPE_MULTI,
} ShaderFileType;

typedef enum TextureType {
  TEXTURE_TYPE_2D,
  TEXTURE_TYPE_CUBE_MAP,
  TEXTURE_TYPE_COUNT,
} TextureType;

typedef enum TextureFormat {
  // RGBA formats
  TEXTURE_FORMAT_R8G8B8A8_UNORM,
  TEXTURE_FORMAT_R8G8B8A8_SRGB,
  TEXTURE_FORMAT_R8G8B8A8_UINT,
  TEXTURE_FORMAT_R8G8B8A8_SNORM,
  TEXTURE_FORMAT_R8G8B8A8_SINT,
  // Single/dual channel formats
  TEXTURE_FORMAT_R8_UNORM,
  TEXTURE_FORMAT_R16_SFLOAT,
  TEXTURE_FORMAT_R32_SFLOAT,
  TEXTURE_FORMAT_R8G8_UNORM,
  // Depth/stencil formats
  TEXTURE_FORMAT_D32_SFLOAT,
  TEXTURE_FORMAT_D24_UNORM_S8_UINT,

  TEXTURE_FORMAT_COUNT,
} TextureFormat;

typedef enum TexturePropertyBits {
  TEXTURE_PROPERTY_FILTER_LINEAR_BIT = 1 << 0,
  TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT = 1 << 1,
  TEXTURE_PROPERTY_FILTER_MIPMAP_BIT = 1 << 2,
  TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT = 1 << 3,
} TexturePropertyBits;
typedef Bitset8 TexturePropertyFlags;

static inline TexturePropertyFlags texture_property_flags_create(void) {
  return bitset8_create();
}

static inline TexturePropertyFlags
texture_property_flags_from_bits(uint8_t bits) {
  TexturePropertyFlags flags = bitset8_create();
  if (bits & TEXTURE_PROPERTY_FILTER_LINEAR_BIT)
    bitset8_set(&flags, TEXTURE_PROPERTY_FILTER_LINEAR_BIT);
  if (bits & TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT)
    bitset8_set(&flags, TEXTURE_PROPERTY_FILTER_ANISOTROPIC_BIT);
  if (bits & TEXTURE_PROPERTY_FILTER_MIPMAP_BIT)
    bitset8_set(&flags, TEXTURE_PROPERTY_FILTER_MIPMAP_BIT);
  if (bits & TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT)
    bitset8_set(&flags, TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);
  return flags;
}

typedef struct TextureDescription {
  uint32_t id;
  uint32_t width;
  uint32_t height;
  uint32_t generation;
  uint32_t channels;

  TextureType type;
  TextureFormat format;
  TexturePropertyFlags properties;
} TextureDescription;

// ----------------------------------------------------------------------------
// Local state & material state
// ----------------------------------------------------------------------------

typedef struct RendererLocalStateHandle {
  uint32_t id;
} RendererLocalStateHandle;

// Used to create a single global uniform object for the entire scene
// This is used to pass the MVP matrix to the shader
typedef struct GlobalUniformObject {
  Mat4 view;
  Mat4 projection;
  // Padding to align to 256 bytes (required by Nvidia GPUs)
  uint8_t padding[128];
} GlobalUniformObject;

// Used to pass the object's properties to the shader
typedef struct LocalUniformObject {
  Vec4 diffuse_color;
  // Padding to align to 256 bytes (required by Nvidia GPUs)
  uint8_t padding[256 - sizeof(Vec4)];
} LocalUniformObject;

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

typedef struct ShaderStateObject {
  Mat4 model;
  // Local state management: hidden behind a typed handle.
  RendererLocalStateHandle local_state;
} ShaderStateObject;

typedef struct RendererMaterialState {
  // Per-material uniforms for the local UBO
  LocalUniformObject uniforms;
  // Current base color texture for slot 0 (may be NULL when disabled)
  TextureHandle texture0;
  bool8_t texture0_enabled;
} RendererMaterialState;

typedef struct ShaderModuleDescription {
  ShaderStageFlags stages;
  /* Path to the shader file (same path for single file, different paths for
   * multi-file) */
  String8 path;
  /* Entry point for the shader (e.g., "main") */
  String8 entry_point;
  // Future: defines, include paths etc.
} ShaderModuleDescription;

typedef struct ShaderObjectDescription {
  /* Format of the shader file (e.g., SPIR-V, HLSL, GLSL) */
  ShaderFileFormat file_format;
  /* Determines if the shader is a single file or a multi-file shader (e.g.,
   * single, multi) */
  ShaderFileType file_type;

  ShaderModuleDescription modules[SHADER_STAGE_COUNT];

  GlobalUniformObject global_uniform_object;
  ShaderStateObject shader_state_object;
} ShaderObjectDescription;

// Used at PIPELINE CREATION time to define vertex layout
typedef struct VertexInputAttributeDescription {
  uint32_t location;   // Shader input location (layout(location = X) in shader)
  uint32_t binding;    // Which vertex buffer binding this attribute uses
  VertexFormat format; // Format of the attribute data
  uint32_t offset;     // Offset within the vertex stride
} VertexInputAttributeDescription;

// Used at PIPELINE CREATION time to define vertex buffer bindings
typedef struct VertexInputBindingDescription {
  uint32_t binding; // The binding number (referenced by attributes and runtime
                    // bindings)
  uint32_t stride;  // Distance between consecutive elements for this binding
  VertexInputRate input_rate; // Per-vertex or per-instance
} VertexInputBindingDescription;

typedef enum VkrPipelineDomain {
  VKR_PIPELINE_DOMAIN_WORLD = 0,
  VKR_PIPELINE_DOMAIN_UI = 1,
  VKR_PIPELINE_DOMAIN_SHADOW = 2,
  VKR_PIPELINE_DOMAIN_POST = 3,
  VKR_PIPELINE_DOMAIN_COMPUTE = 4,

  VKR_PIPELINE_DOMAIN_COUNT
} VkrPipelineDomain;

typedef struct GraphicsPipelineDescription {
  ShaderObjectDescription shader_object_description;

  uint32_t attribute_count;
  VertexInputAttributeDescription *attributes;
  uint32_t binding_count;
  VertexInputBindingDescription *bindings;

  PrimitiveTopology topology;

  PolygonMode polygon_mode;

  VkrPipelineDomain domain;
} GraphicsPipelineDescription;

// ============================================================================
// Buffer and Vertex/Index Data Structures
// ============================================================================

// Used at RUNTIME to bind actual buffers to the vertex input bindings defined
// in the pipeline
typedef struct VertexBufferBinding {
  BufferHandle buffer;
  uint32_t binding; // Must match a binding number from
                    // VertexInputBindingDescription in the current pipeline
  uint64_t offset;  // Offset into the buffer
} VertexBufferBinding;

typedef struct IndexBufferBinding {
  BufferHandle buffer;
  IndexType type;  // uint16 or uint32
  uint64_t offset; // Offset into the buffer
} IndexBufferBinding;

// ============================================================================
// Frontend API (User-Facing)
// ============================================================================

// --- START Initialization and Shutdown ---
RendererFrontendHandle renderer_create(Arena *arena,
                                       RendererBackendType backend_type,
                                       VkrWindow *window,
                                       DeviceRequirements *device_requirements,
                                       RendererError *out_error);

void renderer_destroy(RendererFrontendHandle renderer);
// --- END Initialization and Shutdown ---

// --- START Utility ---
String8 renderer_get_error_string(RendererError error);
VkrWindow *renderer_get_window(RendererFrontendHandle renderer);
RendererBackendType renderer_get_backend_type(RendererFrontendHandle renderer);
bool32_t renderer_is_frame_active(RendererFrontendHandle renderer);
RendererError renderer_wait_idle(RendererFrontendHandle renderer);
void renderer_get_device_information(RendererFrontendHandle renderer,
                                     DeviceInformation *device_information,
                                     Arena *temp_arena);
// --- END Utility ---

// --- START Resource Management ---
BufferHandle renderer_create_buffer(RendererFrontendHandle renderer,
                                    const BufferDescription *description,
                                    const void *initial_data,
                                    RendererError *out_error);

// Convenience functions for common buffer types
BufferHandle renderer_create_vertex_buffer(RendererFrontendHandle renderer,
                                           uint64_t size,
                                           const void *initial_data,
                                           RendererError *out_error);

BufferHandle renderer_create_index_buffer(RendererFrontendHandle renderer,
                                          uint64_t size, IndexType type,
                                          const void *initial_data,
                                          RendererError *out_error);

TextureHandle renderer_create_texture(RendererFrontendHandle renderer,
                                      const TextureDescription *description,
                                      const void *initial_data,
                                      RendererError *out_error);

void renderer_destroy_texture(RendererFrontendHandle renderer,
                              TextureHandle texture);

void renderer_destroy_buffer(RendererFrontendHandle renderer,
                             BufferHandle buffer);

PipelineHandle renderer_create_graphics_pipeline(
    RendererFrontendHandle renderer,
    const GraphicsPipelineDescription *description, RendererError *out_error);

void renderer_destroy_pipeline(RendererFrontendHandle renderer,
                               PipelineHandle pipeline);
// --- END Resource Management ---

// --- START Data Update ---
RendererError renderer_update_buffer(RendererFrontendHandle renderer,
                                     BufferHandle buffer, uint64_t offset,
                                     uint64_t size, const void *data);

RendererError renderer_update_pipeline_state(
    RendererFrontendHandle renderer, PipelineHandle pipeline,
    const GlobalUniformObject *uniform, const ShaderStateObject *data,
    const RendererMaterialState *material);

/**
 * @brief Update only the per-frame global state (e.g., view/projection). Call
 * once per frame before drawing renderables.
 *
 * @param renderer
 * @param pipeline
 * @param uniform
 * @return RendererError
 */
RendererError renderer_update_global_state(RendererFrontendHandle renderer,
                                           PipelineHandle pipeline,
                                           const GlobalUniformObject *uniform);

/**
 * @brief Update only the per-object local state (e.g., model matrix, material
 * uniforms, textures). Call per renderable.
 *
 * @param renderer
 * @param pipeline
 * @param data
 * @return RendererError
 */
RendererError renderer_update_local_state(
    RendererFrontendHandle renderer, PipelineHandle pipeline,
    const ShaderStateObject *data, const RendererMaterialState *material);

// Local state lifetime
RendererError
renderer_acquire_local_state(RendererFrontendHandle renderer,
                             PipelineHandle pipeline,
                             RendererLocalStateHandle *out_handle);

RendererError renderer_release_local_state(RendererFrontendHandle renderer,
                                           PipelineHandle pipeline,
                                           RendererLocalStateHandle handle);

RendererError renderer_upload_buffer(RendererFrontendHandle renderer,
                                     BufferHandle buffer, uint64_t offset,
                                     uint64_t size, const void *data);
// --- END Data Update ---

// --- START Frame Lifecycle & Rendering Commands ---
RendererError renderer_begin_frame(RendererFrontendHandle renderer,
                                   float64_t delta_time);

void renderer_resize(RendererFrontendHandle renderer, uint32_t width,
                     uint32_t height);

// Bind a vertex buffer (most common case)
void renderer_bind_vertex_buffer(RendererFrontendHandle renderer,
                                 const VertexBufferBinding *binding);

void renderer_bind_index_buffer(RendererFrontendHandle renderer,
                                const IndexBufferBinding *binding);

void renderer_draw(RendererFrontendHandle renderer, uint32_t vertex_count,
                   uint32_t instance_count, uint32_t first_vertex,
                   uint32_t first_instance);

void renderer_draw_indexed(RendererFrontendHandle renderer,
                           uint32_t index_count, uint32_t instance_count,
                           uint32_t first_index, int32_t vertex_offset,
                           uint32_t first_instance);

RendererError renderer_end_frame(RendererFrontendHandle renderer,
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
typedef struct RendererBackendInterface {
  // --- Lifecycle ---
  // `config` can be specific to the backend, or a generic struct
  // `window_handle` is platform specific (HWND, xcb_window_t, etc.)
  bool32_t (*initialize)(
      void **out_backend_state, // Backend allocates and returns its state
      RendererBackendType type, VkrWindow *window, uint32_t initial_width,
      uint32_t initial_height, DeviceRequirements *device_requirements);
  void (*shutdown)(void *backend_state);
  void (*on_resize)(void *backend_state, uint32_t new_width,
                    uint32_t new_height);
  void (*get_device_information)(void *backend_state,
                                 DeviceInformation *device_information,
                                 Arena *temp_arena);

  // --- Synchronization ---
  RendererError (*wait_idle)(void *backend_state); // Wait for GPU to be idle

  // --- Frame Management ---
  RendererError (*begin_frame)(void *backend_state, float64_t delta_time);
  RendererError (*end_frame)(void *backend_state,
                             float64_t delta_time); // Includes present

  // --- Resource Management ---
  BackendResourceHandle (*buffer_create)(void *backend_state,
                                         const BufferDescription *desc,
                                         const void *initial_data);
  void (*buffer_destroy)(void *backend_state, BackendResourceHandle handle);
  RendererError (*buffer_update)(void *backend_state,
                                 BackendResourceHandle handle, uint64_t offset,
                                 uint64_t size, const void *data);
  RendererError (*buffer_upload)(void *backend_state,
                                 BackendResourceHandle handle, uint64_t offset,
                                 uint64_t size, const void *data);

  BackendResourceHandle (*texture_create)(void *backend_state,
                                          const TextureDescription *desc,
                                          const void *initial_data);
  void (*texture_destroy)(void *backend_state, BackendResourceHandle handle);

  // Pipeline creation uses VertexInputAttributeDescription and
  // VertexInputBindingDescription from GraphicsPipelineDescription to
  // configure the vertex input layout. Runtime vertex buffer bindings
  // must reference the binding numbers defined in these descriptions.
  BackendResourceHandle (*graphics_pipeline_create)(
      void *backend_state, const GraphicsPipelineDescription *description);
  RendererError (*pipeline_update_state)(void *backend_state,
                                         BackendResourceHandle pipeline_handle,
                                         const GlobalUniformObject *uniform,
                                         const ShaderStateObject *data,
                                         const RendererMaterialState *material);
  void (*pipeline_destroy)(void *backend_state,
                           BackendResourceHandle pipeline_handle);

  // Local state management
  RendererError (*local_state_acquire)(void *backend_state,
                                       BackendResourceHandle pipeline_handle,
                                       RendererLocalStateHandle *out_handle);
  RendererError (*local_state_release)(void *backend_state,
                                       BackendResourceHandle pipeline_handle,
                                       RendererLocalStateHandle handle);

  void (*bind_buffer)(void *backend_state, BackendResourceHandle buffer_handle,
                      uint64_t offset);

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);

  void (*draw_indexed)(void *backend_state, uint32_t index_count,
                       uint32_t instance_count, uint32_t first_index,
                       int32_t vertex_offset, uint32_t first_instance);
} RendererBackendInterface;
