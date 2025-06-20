#pragma once

#include "containers/bitset.h"
#include "defines.h"
#include "pch.h"
#include "platform/platform.h"
#include "platform/window.h"

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
typedef struct s_ShaderModule *ShaderHandle;
typedef struct s_Pipeline *PipelineHandle;

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

typedef enum BufferUsageBits {
  BUFFER_USAGE_NONE = 0,
  BUFFER_USAGE_VERTEX_BUFFER = 1 << 0,
  BUFFER_USAGE_INDEX_BUFFER = 1 << 1,
  BUFFER_USAGE_UNIFORM = 1 << 2,
  BUFFER_USAGE_STORAGE = 1 << 3,      // For compute/more advanced
  BUFFER_USAGE_TRANSFER_SRC = 1 << 4, // Can be source of a copy
  BUFFER_USAGE_TRANSFER_DST = 1 << 5, // Can be destination of a copy
} BufferUsageBits;
typedef Bitset8 BufferUsageFlags;

typedef enum MemoryPropertyBits {
  MEMORY_PROPERTY_DEVICE_LOCAL = 1 << 0,  // GPU optimal memory
  MEMORY_PROPERTY_HOST_VISIBLE = 1 << 1,  // CPU can map
  MEMORY_PROPERTY_HOST_COHERENT = 1 << 2, // No explicit flush needed
  MEMORY_PROPERTY_HOST_CACHED = 1 << 3,   // CPU cacheable
} MemoryPropertyBits;
typedef Bitset8 MemoryPropertyFlags;

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
} BufferDescription;

typedef struct ShaderModuleDescription {
  ShaderStageFlags stage;
  const uint64_t size; // Size of the shader bytecode in bytes (must be multiple
                       // of 4 for SPIR-V)
  const uint8_t *code; // Pointer to shader bytecode (SPIR-V format, must be
                       // 4-byte aligned)
  const String8 entry_point; // e.g., "main"
                             // Future: defines, include paths etc.
} ShaderModuleDescription;

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

typedef struct GraphicsPipelineDescription {
  ShaderHandle vertex_shader;
  ShaderHandle fragment_shader;
  // Future: geometry_shader, tess_control_shader, tess_eval_shader

  uint32_t attribute_count;
  VertexInputAttributeDescription *attributes;
  uint32_t binding_count;
  VertexInputBindingDescription *bindings;

  PrimitiveTopology topology;

  // Simplified for now. Add more states as needed:
  // RasterizationState rasterization_state;
  // DepthStencilState depth_stencil_state;
  // BlendState blend_state;
  // MultiSampleState multi_sample_state;
  // Dynamic states (e.g. viewport, scissor - already handled by separate
  // commands)
} GraphicsPipelineDescription;

// ============================================================================
// Buffer and Vertex/Index Data Structures
// ============================================================================

/* Usage Example:
 *
 * // 1. Create pipeline with vertex input layout
 * VertexInputAttributeDescription attrs[] = {
 *   {.location = 0, .binding = 0, .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
 * .offset = 0},  // position
 *   {.location = 1, .binding = 0, .format = VERTEX_FORMAT_R32G32_SFLOAT,
 * .offset = 12},   // uv
 * };
 * VertexInputBindingDescription bindings[] = {
 *   {.binding = 0, .stride = 20, .input_rate = VERTEX_INPUT_RATE_VERTEX}
 * };
 * GraphicsPipelineDescription pipeline_desc = {
 *   .vertex_shader = vs, .fragment_shader = fs,
 *   .attribute_count = 2, .attributes = attrs,
 *   .binding_count = 1, .bindings = bindings,
 *   // ... other pipeline state
 * };
 * PipelineHandle pipeline = renderer_create_pipeline(renderer, &pipeline_desc,
 * &err);
 *
 * // 2. Create buffers
 * BufferHandle vb = renderer_create_vertex_buffer(renderer, vertex_data_size,
 * vertex_data, &err);
 * BufferHandle ib = renderer_create_index_buffer(renderer,
 * index_data_size, INDEX_TYPE_UINT16, index_data, &err);
 *
 * // 3. Render
 * renderer_begin_frame(renderer, dt);
 * renderer_bind_graphics_pipeline(renderer, pipeline);
 *
 * VertexBufferBinding vb_binding = {.buffer = vb, .binding = 0, .offset = 0};
 * renderer_bind_vertex_buffer(renderer, &vb_binding);
 *
 * IndexBufferBinding ib_binding = {.buffer = ib, .type = INDEX_TYPE_UINT16,
 * .offset = 0}; renderer_bind_index_buffer(renderer, &ib_binding);
 *
 * renderer_draw_indexed(renderer, index_count, 1, 0, 0, 0);
 * renderer_end_frame(renderer, dt);
 */

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
                                       Window *window,
                                       DeviceRequirements *device_requirements,
                                       RendererError *out_error);

void renderer_destroy(RendererFrontendHandle renderer);
// --- END Initialization and Shutdown ---

// --- START Utility ---
String8 renderer_get_error_string(RendererError error);
Window *renderer_get_window(RendererFrontendHandle renderer);
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

void renderer_destroy_buffer(RendererFrontendHandle renderer,
                             BufferHandle buffer);

ShaderHandle
renderer_create_shader_from_source(RendererFrontendHandle renderer,
                                   const ShaderModuleDescription *description,
                                   RendererError *out_error);

void renderer_destroy_shader(RendererFrontendHandle renderer,
                             ShaderHandle shader);

PipelineHandle
renderer_create_pipeline(RendererFrontendHandle renderer,
                         const GraphicsPipelineDescription *description,
                         RendererError *out_error);

void renderer_destroy_pipeline(RendererFrontendHandle renderer,
                               PipelineHandle pipeline);
// --- END Resource Management ---

// --- START Data Update ---
RendererError renderer_update_buffer(RendererFrontendHandle renderer,
                                     BufferHandle buffer, uint64_t offset,
                                     uint64_t size, const void *data);
// --- END Data Update ---

// --- START Frame Lifecycle & Rendering Commands ---
RendererError renderer_begin_frame(RendererFrontendHandle renderer,
                                   float64_t delta_time);

void renderer_resize(RendererFrontendHandle renderer, uint32_t width,
                     uint32_t height);

void renderer_bind_graphics_pipeline(RendererFrontendHandle renderer,
                                     PipelineHandle pipeline);

// Bind a vertex buffer (most common case)
void renderer_bind_vertex_buffer(RendererFrontendHandle renderer,
                                 const VertexBufferBinding *binding);

// Bind multiple vertex buffers at once - for advanced use cases
void renderer_bind_vertex_buffers(RendererFrontendHandle renderer,
                                  uint32_t first_binding,
                                  uint32_t binding_count,
                                  const VertexBufferBinding *bindings);

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
      RendererBackendType type, Window *window, uint32_t initial_width,
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

  // --- Shader & Pipeline Management (NEW) ---
  BackendResourceHandle (*shader_create_from_source)(
      void *backend_state, const ShaderModuleDescription *description);
  void (*shader_destroy)(void *backend_state,
                         BackendResourceHandle shader_handle);

  // Pipeline creation uses VertexInputAttributeDescription and
  // VertexInputBindingDescription from GraphicsPipelineDescription to
  // configure the vertex input layout. Runtime vertex buffer bindings
  // must reference the binding numbers defined in these descriptions.
  BackendResourceHandle (*pipeline_create)(
      void *backend_state, const GraphicsPipelineDescription *description);
  void (*pipeline_destroy)(void *backend_state,
                           BackendResourceHandle pipeline_handle);

  // --- Drawing Commands (Called by Frontend within a frame/render pass) ---
  // These are the "abstract commands" translated by the frontend.
  void (*bind_pipeline)(void *backend_state,
                        BackendResourceHandle pipeline_handle);

  void (*bind_vertex_buffer)(void *backend_state,
                             const VertexBufferBinding *binding);

  void (*bind_vertex_buffers)(void *backend_state, uint32_t first_binding,
                              uint32_t binding_count,
                              const VertexBufferBinding *bindings);

  void (*bind_index_buffer)(void *backend_state,
                            const IndexBufferBinding *binding);

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);

  void (*draw_indexed)(void *backend_state, uint32_t index_count,
                       uint32_t instance_count, uint32_t first_index,
                       int32_t vertex_offset, uint32_t first_instance);
} RendererBackendInterface;
