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
    - Handles: Opaque pointers (e.g., `MeshHandle`, `TextureHandle`) are used
      in the frontend API to refer to resources. This hides internal details
      and backend-specific representations from the user.
    - Resource Descriptions: Structs (e.g., `BufferDescription`,
      `TextureDescription`) are used to specify parameters for resource
      creation.
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

typedef struct VertexInputAttributeDescription {
  uint32_t location;   // Shader input location
  uint32_t binding;    // Which vertex buffer binding this attribute uses
  VertexFormat format; // Format of the attribute data
  uint32_t offset;     // Offset within the vertex stride
} VertexInputAttributeDescription;

typedef struct VertexInputBindingDescription {
  uint32_t binding; // The binding number (matches attribute's binding)
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
// Frontend API (User-Facing)
// ============================================================================

// --- START Initialization and Shutdown ---
RendererFrontendHandle renderer_create(Arena *arena,
                                       RendererBackendType backend_type,
                                       Window *window,
                                       RendererError *out_error);

void renderer_destroy(RendererFrontendHandle renderer);
// --- END Initialization and Shutdown ---

// --- START Utility ---
String8 renderer_get_error_string(RendererError error);
Window *renderer_get_window(RendererFrontendHandle renderer);
RendererBackendType renderer_get_backend_type(RendererFrontendHandle renderer);
bool32_t renderer_is_frame_active(RendererFrontendHandle renderer);
// --- END Utility ---

// --- START Resource Management ---
BufferHandle renderer_create_buffer(RendererFrontendHandle renderer,
                                    const BufferDescription *description,
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

void renderer_set_viewport(RendererFrontendHandle renderer, int32_t x,
                           int32_t y, uint32_t width, uint32_t height,
                           float32_t min_depth, float32_t max_depth);

void renderer_set_scissor(RendererFrontendHandle renderer, int32_t x, int32_t y,
                          uint32_t width, uint32_t height);

void renderer_resize(RendererFrontendHandle renderer, uint32_t width,
                     uint32_t height);

void renderer_clear_color(RendererFrontendHandle renderer, float32_t r,
                          float32_t g, float32_t b, float32_t a);

void renderer_bind_graphics_pipeline(RendererFrontendHandle renderer,
                                     PipelineHandle pipeline);
void renderer_bind_vertex_buffer(RendererFrontendHandle renderer,
                                 BufferHandle buffer, uint32_t binding_index,
                                 uint64_t offset);

void renderer_draw(RendererFrontendHandle renderer, uint32_t vertex_count,
                   uint32_t instance_count, uint32_t first_vertex,
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
      uint32_t initial_height);
  void (*shutdown)(void *backend_state);
  void (*on_resize)(void *backend_state, uint32_t new_width,
                    uint32_t new_height);

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

  BackendResourceHandle (*pipeline_create)(
      void *backend_state, const GraphicsPipelineDescription *description);
  void (*pipeline_destroy)(void *backend_state,
                           BackendResourceHandle pipeline_handle);

  // --- Drawing Commands (Called by Frontend within a frame/render pass) ---
  // These are the "abstract commands" translated by the frontend.
  void (*set_viewport)(void *backend_state, int32_t x, int32_t y,
                       uint32_t width, uint32_t height, float32_t min_depth,
                       float32_t max_depth);
  void (*set_scissor)(void *backend_state, int32_t x, int32_t y, uint32_t width,
                      uint32_t height);

  void (*clear_color)(void *backend_state, float32_t r, float32_t g,
                      float32_t b, float32_t a);

  void (*bind_pipeline)(void *backend_state,
                        BackendResourceHandle pipeline_handle);
  void (*bind_vertex_buffer)(void *backend_state,
                             BackendResourceHandle buffer_handle,
                             uint32_t binding_index, uint64_t offset);

  void (*draw)(void *backend_state, uint32_t vertex_count,
               uint32_t instance_count, uint32_t first_vertex,
               uint32_t first_instance);
} RendererBackendInterface;
