#pragma once

#include "containers/array.h"
#include "containers/bitset.h"
#include "defines.h"
#include "math/vec.h"
#include "memory/arena.h"
#include "mesh.h"
#include "renderer/renderer.h"

// =============================================================================
// Interleaved Vertex Structures
// =============================================================================

/**
 * @brief Standard vertex format with position and color
 * This is the most basic vertex format for simple colored geometry.
 */
typedef struct {
  Vec3 position;
  Vec3 color;
} InterleavedVertex_PositionColor;

/**
 * @brief Standard vertex format with position, normal, and color
 * Common format for basic lit colored geometry.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec3 color;
} InterleavedVertex_PositionNormalColor;

/**
 * @brief Standard vertex format with position, normal, and texture coordinates
 * Common format for textured geometry with lighting.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec2 texcoord;
} InterleavedVertex_PositionNormalTexcoord;

/**
 * @brief Vertex format with position and texture coordinates only
 */
typedef struct {
  Vec3 position;
  Vec2 texcoord;
} InterleavedVertex_PositionTexcoord;

/**
 * @brief Full vertex format with all standard attributes
 * Complete vertex format for advanced rendering with all attributes.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec2 texcoord;
  Vec3 color;
} InterleavedVertex_Full;

// =============================================================================
// Generic Buffer Wrappers
// =============================================================================

/**
 * @brief Vertex buffer with metadata for rendering operations
 *
 * Wraps a BufferHandle with vertex-specific information needed for binding
 * and pipeline creation. Can be created from any vertex data source.
 */
typedef struct VertexBuffer {
  BufferHandle handle;
  uint32_t stride;            // Size of one vertex in bytes
  uint32_t vertex_count;      // Number of vertices in this buffer
  VertexInputRate input_rate; // Per-vertex or per-instance

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} VertexBuffer;

Array(VertexBuffer);

/**
 * @brief Index buffer with metadata for rendering operations
 */
typedef struct IndexBuffer {
  BufferHandle handle;
  IndexType type;       // uint16 or uint32
  uint32_t index_count; // Number of indices

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} IndexBuffer;

/**
 * @brief Uniform buffer for shader constants
 */
typedef struct UniformBuffer {
  BufferHandle handle;
  uint32_t binding;        // Descriptor set binding point
  ShaderStageFlags stages; // Which shader stages use this
  uint64_t size_bytes;     // Buffer size

  // Optional metadata
  String8 debug_name; // For debugging/profiling
  bool32_t dynamic;   // Whether this buffer is updated frequently
} UniformBuffer;

Array(UniformBuffer);

// =============================================================================
// Vertex Array - Complete Drawable Object
// =============================================================================

typedef enum VertexArrayStateFlags {
  VERTEX_ARRAY_STATE_UNINITIALIZED = 1 << 0,
  VERTEX_ARRAY_STATE_INITIALIZED = 1 << 1,
  VERTEX_ARRAY_STATE_HAS_INDEX_BUFFER = 1 << 2,
  VERTEX_ARRAY_STATE_PIPELINE_DATA_VALID =
      1 << 3, // True if attributes/bindings are computed
} VertexArrayStateFlags;
typedef Bitset8 VertexArrayState;

/**
 * @brief Complete vertex specification for rendering
 *
 * Represents a complete drawable object with vertex buffers, index buffer,
 * and all metadata needed for pipeline creation and rendering. This is
 * renderer-centric, not mesh-centric.
 */
typedef struct VertexArray {
  Arena *arena;

  // Vertex data
  Array_VertexBuffer vertex_buffers;
  IndexBuffer index_buffer;

  // Pre-computed pipeline descriptions (cached for efficiency)
  uint32_t attribute_count;
  VertexInputAttributeDescription *attributes;
  uint32_t binding_count;
  VertexInputBindingDescription *bindings;

  // Rendering metadata
  PrimitiveTopology topology;
  String8 debug_name;

  // State tracking
  VertexArrayState state;
} VertexArray;

Array(VertexArray);

// =============================================================================
// Buffer Creation Functions
// =============================================================================

/**
 * @brief Creates a vertex buffer from raw vertex data
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param data Vertex data
 * @param stride Size of one vertex
 * @param vertex_count Number of vertices
 * @param input_rate Per-vertex or per-instance
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created VertexBuffer
 */
VertexBuffer vertex_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                                  const void *data, uint32_t stride,
                                  uint32_t vertex_count,
                                  VertexInputRate input_rate,
                                  String8 debug_name, RendererError *out_error);

/**
 * @brief Creates an index buffer from index data
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param data Index data
 * @param type Index type (uint16/uint32)
 * @param index_count Number of indices
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created IndexBuffer
 */
IndexBuffer index_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                                const void *data, IndexType type,
                                uint32_t index_count, String8 debug_name,
                                RendererError *out_error);

/**
 * @brief Creates a uniform buffer
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param data Initial data (can be NULL)
 * @param size_bytes Buffer size
 * @param binding Descriptor binding point
 * @param stages Shader stages that use this buffer
 * @param dynamic Whether buffer is updated frequently
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created UniformBuffer
 */
UniformBuffer uniform_buffer_create(RendererFrontendHandle renderer,
                                    Arena *arena, const void *data,
                                    uint64_t size_bytes, uint32_t binding,
                                    ShaderStageFlags stages, bool32_t dynamic,
                                    String8 debug_name,
                                    RendererError *out_error);

/**
 * @brief Creates a global uniform buffer
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param global_uniform_object Global uniform object
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created UniformBuffer
 */
UniformBuffer
global_uniform_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                             GlobalUniformObject *global_uniform_object,
                             String8 debug_name, RendererError *out_error);

// =============================================================================
// Buffer Update Functions
// =============================================================================

/**
 * @brief Updates vertex buffer data
 */
RendererError vertex_buffer_update(RendererFrontendHandle renderer,
                                   VertexBuffer *vertex_buffer,
                                   const void *data, uint32_t offset_vertices,
                                   uint32_t vertex_count);

/**
 * @brief Updates index buffer data
 */
RendererError index_buffer_update(RendererFrontendHandle renderer,
                                  IndexBuffer *index_buffer, const void *data,
                                  uint32_t offset_indices,
                                  uint32_t index_count);

/**
 * @brief Updates uniform buffer data
 */
RendererError uniform_buffer_update(RendererFrontendHandle renderer,
                                    UniformBuffer *uniform_buffer,
                                    const void *data, uint64_t offset_bytes,
                                    uint64_t size_bytes);

// =============================================================================
// Buffer Cleanup
// =============================================================================

void vertex_buffer_destroy(RendererFrontendHandle renderer,
                           VertexBuffer *vertex_buffer);
void index_buffer_destroy(RendererFrontendHandle renderer,
                          IndexBuffer *index_buffer);
void uniform_buffer_destroy(RendererFrontendHandle renderer,
                            UniformBuffer *uniform_buffer);

// =============================================================================
// Vertex Array Creation and Management
// =============================================================================

/**
 * @brief Creates an empty vertex array
 * @param arena Memory allocator
 * @param max_vertex_buffers Maximum number of vertex buffers
 * @param topology Primitive topology
 * @param debug_name Optional debug name
 * @return Created VertexArray
 */
VertexArray vertex_array_create(Arena *arena, uint32_t max_vertex_buffers,
                                PrimitiveTopology topology, String8 debug_name);

/**
 * @brief Destroys a vertex array
 * @param renderer Renderer instance
 * @param vertex_array Vertex array to destroy
 */
void vertex_array_destroy(RendererFrontendHandle renderer,
                          VertexArray *vertex_array);

/**
 * @brief Adds a vertex buffer to the vertex array
 * @param vertex_array Target vertex array
 * @param vertex_buffer Vertex buffer to add
 * @param binding_index Which binding point this buffer uses
 * @return Error code
 */
RendererError vertex_array_add_vertex_buffer(VertexArray *vertex_array,
                                             const VertexBuffer *vertex_buffer,
                                             uint32_t binding_index);

/**
 * @brief Sets the index buffer for the vertex array
 * @param vertex_array Target vertex array
 * @param index_buffer Index buffer to set
 * @return Error code
 */
RendererError vertex_array_set_index_buffer(VertexArray *vertex_array,
                                            const IndexBuffer *index_buffer);

/**
 * @brief Adds a vertex attribute to the vertex array
 * @param vertex_array Target vertex array
 * @param location Shader attribute location
 * @param binding Which vertex buffer binding this attribute uses
 * @param format Vertex format
 * @param offset Offset within the vertex
 * @return Error code
 */
RendererError vertex_array_add_attribute(VertexArray *vertex_array,
                                         uint32_t location, uint32_t binding,
                                         VertexFormat format, uint32_t offset);

/**
 * @brief Computes and caches pipeline vertex input descriptions
 * @param vertex_array Vertex array to process
 * @return Error code
 */
RendererError vertex_array_compute_pipeline_data(VertexArray *vertex_array);

// =============================================================================
// Rendering Functions
// =============================================================================

/**
 * @brief Binds a vertex array for rendering
 * @param renderer Renderer instance
 * @param vertex_array Vertex array to bind
 *
 * Must be called between renderer_begin_frame() and renderer_end_frame()
 * Must be called after binding a compatible pipeline
 */
void vertex_array_bind(RendererFrontendHandle renderer,
                       const VertexArray *vertex_array);

/**
 * @brief Draws a bound vertex array
 * @param renderer Renderer instance
 * @param vertex_array Vertex array to draw (must be bound)
 * @param instance_count Number of instances
 */
void vertex_array_draw(RendererFrontendHandle renderer,
                       const VertexArray *vertex_array,
                       uint32_t instance_count);

/**
 * @brief Convenience: bind and draw vertex array
 * @param renderer Renderer instance
 * @param vertex_array Vertex array to render
 * @param instance_count Number of instances
 */
void vertex_array_render(RendererFrontendHandle renderer,
                         const VertexArray *vertex_array,
                         uint32_t instance_count);

// =============================================================================
// Mesh Conversion Functions
// =============================================================================

typedef enum VertexArrayFromMeshOptionFlags {
  VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED = 1 << 0,
  VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS = 1 << 1,
  VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TANGENTS = 1 << 2,
  VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_BITANGENTS = 1 << 3,
  VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS = 1 << 4,
  VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS = 1 << 5,
} VertexArrayFromMeshOptionFlags;
typedef Bitset8 VertexArrayFromMeshOptions;

/**
 * @brief IMPORTANT: VertexArrayFromMeshOptions is a Bitset8 STRUCTURE, not a
 * raw integer!
 *
 * You MUST use the helper functions below to create and manipulate options.
 * Do NOT pass raw integer flags directly to vertex_array_from_mesh().
 *
 * ❌ WRONG:
 *   vertex_array_from_mesh(renderer, arena, mesh,
 *                          VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS |
 * VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS, debug_name, &error);
 *
 * ✅ CORRECT:
 *   VertexArrayFromMeshOptions options =
 * vertex_array_from_mesh_options_from_flags(
 *       VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS |
 * VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS);
 *   vertex_array_from_mesh(renderer, arena, mesh, options, debug_name, &error);
 */

/**
 * @brief Creates empty VertexArrayFromMeshOptions bitset
 * @return Empty options bitset
 */
VertexArrayFromMeshOptions vertex_array_from_mesh_options_create(void);

/**
 * @brief Creates VertexArrayFromMeshOptions with specified flags
 * @param flags OR'd combination of VertexArrayFromMeshOptionFlags
 * @return Options bitset with specified flags set
 */
VertexArrayFromMeshOptions
vertex_array_from_mesh_options_from_flags(uint8_t flags);

/**
 * @brief Adds a flag to existing VertexArrayFromMeshOptions
 * @param options Existing options to modify
 * @param flag Flag to add
 */
void vertex_array_from_mesh_options_add_flag(
    VertexArrayFromMeshOptions *options, VertexArrayFromMeshOptionFlags flag);

// =============================================================================
// Convenience Macros for Common Option Combinations
// =============================================================================

/**
 * @brief Creates options with only position data (minimal)
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_POSITIONS_ONLY()                        \
  vertex_array_from_mesh_options_create()

/**
 * @brief Creates options with positions and normals
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_BASIC()                                 \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS)

/**
 * @brief Creates options with positions, normals, and texture coordinates
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_TEXTURED()                              \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS |                          \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS)

/**
 * @brief Creates options with all attributes (positions, normals, texcoords,
 * colors)
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_FULL()                                  \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS |                          \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS |                        \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS)

/**
 * @brief Creates options with interleaved position and color (single buffer)
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_INTERLEAVED_POSITION_COLOR()            \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED |                              \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS)

/**
 * @brief Creates options with interleaved position and texture coordinates
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_INTERLEAVED_POSITION_TEXCOORD()         \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED |                              \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS)

/**
 * @brief Creates options with interleaved full attributes (single buffer)
 */
#define VERTEX_ARRAY_FROM_MESH_OPTIONS_INTERLEAVED_FULL()                      \
  vertex_array_from_mesh_options_from_flags(                                   \
      VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED |                              \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS |                          \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS |                        \
      VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS)

/**
 * @brief Creates a vertex array from a mesh (one possible source)
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param mesh Source mesh
 * @param options Bitset options for which attributes to include
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created VertexArray
 */
VertexArray vertex_array_from_mesh(RendererFrontendHandle renderer,
                                   Arena *arena, const Mesh *mesh,
                                   VertexArrayFromMeshOptions options,
                                   String8 debug_name,
                                   RendererError *out_error);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Validates a vertex array for rendering
 * @param vertex_array Vertex array to validate
 * @return True if valid and ready for rendering
 */
bool32_t vertex_array_is_valid(const VertexArray *vertex_array);

/**
 * @brief Estimates GPU memory usage for a vertex array
 * @param vertex_array Vertex array to analyze
 * @return Estimated memory usage in bytes
 */
uint64_t vertex_array_estimate_memory_usage(const VertexArray *vertex_array);

/**
 * @brief Gets the total vertex count across all vertex buffers
 * @param vertex_array Vertex array to analyze
 * @return Total vertex count (should be same for all buffers)
 */
uint32_t vertex_array_get_vertex_count(const VertexArray *vertex_array);

// =============================================================================
// Batch Rendering Functions
// =============================================================================

/**
 * @brief Render multiple vertex arrays efficiently
 * @param renderer Renderer instance
 * @param vertex_arrays Array of vertex arrays to render
 * @param array_count Number of vertex arrays
 * @param instance_counts Array of instance counts (one per vertex array)
 */
void vertex_array_render_batch(RendererFrontendHandle renderer,
                               const VertexArray *vertex_arrays,
                               uint32_t array_count,
                               const uint32_t *instance_counts);