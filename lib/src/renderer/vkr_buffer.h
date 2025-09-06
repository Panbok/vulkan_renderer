#pragma once

#include "containers/array.h"
#include "containers/bitset.h"
#include "defines.h"
#include "math/vec.h"
#include "memory/arena.h"
#include "renderer/renderer.h"
#include "renderer/resources/vkr_resources.h"

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
} VkrInterleavedVertex_PositionColor;

/**
 * @brief Standard vertex format with position, normal, and color
 * Common format for basic lit colored geometry.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec3 color;
} VkrInterleavedVertex_PositionNormalColor;

/**
 * @brief Standard vertex format with position, normal, and texture coordinates
 * Common format for textured geometry with lighting.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec2 texcoord;
} VkrInterleavedVertex_PositionNormalTexcoord;

/**
 * @brief Vertex format with position and texture coordinates only
 */
typedef struct {
  Vec3 position;
  Vec2 texcoord;
} VkrInterleavedVertex_PositionTexcoord;

/**
 * @brief Full vertex format with all standard attributes
 * Complete vertex format for advanced rendering with all attributes.
 */
typedef struct {
  Vec3 position;
  Vec3 normal;
  Vec2 texcoord;
  Vec3 color;
} VkrInterleavedVertex_Full;

// =============================================================================
// Generic Buffer Wrappers
// =============================================================================

/**
 * @brief Vertex buffer with metadata for rendering operations
 *
 * Wraps a BufferHandle with vertex-specific information needed for binding
 * and pipeline creation. Can be created from any vertex data source.
 */
typedef struct VkrVertexBuffer {
  BufferHandle handle;
  uint32_t stride;            // Size of one vertex in bytes
  uint32_t vertex_count;      // Number of vertices in this buffer
  VertexInputRate input_rate; // Per-vertex or per-instance

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} VkrVertexBuffer;

Array(VkrVertexBuffer);

/**
 * @brief Index buffer with metadata for rendering operations
 */
typedef struct VkrIndexBuffer {
  BufferHandle handle;
  IndexType type;       // uint16 or uint32
  uint32_t index_count; // Number of indices

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} VkrIndexBuffer;

/**
 * @brief Uniform buffer for shader constants
 */
typedef struct VkrUniformBuffer {
  BufferHandle handle;
  uint32_t binding;        // Descriptor set binding point
  ShaderStageFlags stages; // Which shader stages use this
  uint64_t size_bytes;     // Buffer size

  // Optional metadata
  String8 debug_name; // For debugging/profiling
  bool32_t dynamic;   // Whether this buffer is updated frequently
} VkrUniformBuffer;

Array(VkrUniformBuffer);

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
 * @return Created VkrVertexBuffer
 */
VkrVertexBuffer vkr_vertex_buffer_create(RendererFrontendHandle renderer,
                                         Arena *arena, const void *data,
                                         uint32_t stride, uint32_t vertex_count,
                                         VertexInputRate input_rate,
                                         String8 debug_name,
                                         RendererError *out_error);

/**
 * @brief Creates an index buffer from index data
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param data Index data
 * @param type Index type (uint16/uint32)
 * @param index_count Number of indices
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created VkrIndexBuffer
 */
VkrIndexBuffer vkr_index_buffer_create(RendererFrontendHandle renderer,
                                       Arena *arena, const void *data,
                                       IndexType type, uint32_t index_count,
                                       String8 debug_name,
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
 * @return Created VkrUniformBuffer
 */
VkrUniformBuffer vkr_uniform_buffer_create(
    RendererFrontendHandle renderer, Arena *arena, const void *data,
    uint64_t size_bytes, uint32_t binding, ShaderStageFlags stages,
    bool32_t dynamic, String8 debug_name, RendererError *out_error);

/**
 * @brief Creates a global uniform buffer
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param global_uniform_object Global uniform object
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created VkrUniformBuffer
 */
VkrUniformBuffer
vkr_global_uniform_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                                 GlobalUniformObject *global_uniform_object,
                                 String8 debug_name, RendererError *out_error);

// =============================================================================
// Buffer Update Functions
// =============================================================================

/**
 * @brief Updates vertex buffer data
 * @param renderer Renderer instance
 * @param vertex_buffer Vertex buffer to update
 * @param data New vertex data
 * @param offset_vertices Offset in vertices
 * @param vertex_count Number of vertices to update
 * @return RendererError indicating success or failure
 */
RendererError vkr_vertex_buffer_update(RendererFrontendHandle renderer,
                                       VkrVertexBuffer *vertex_buffer,
                                       const void *data,
                                       uint32_t offset_vertices,
                                       uint32_t vertex_count);

/**
 * @brief Updates index buffer data
 * @param renderer Renderer instance
 * @param index_buffer Index buffer to update
 * @param data New index data
 * @param offset_indices Offset in indices
 * @param index_count Number of indices to update
 * @return RendererError indicating success or failure
 */
RendererError vkr_index_buffer_update(RendererFrontendHandle renderer,
                                      VkrIndexBuffer *index_buffer,
                                      const void *data, uint32_t offset_indices,
                                      uint32_t index_count);

/**
 * @brief Updates uniform buffer data
 * @param renderer Renderer instance
 * @param uniform_buffer Uniform buffer to update
 * @param data New uniform data
 * @param offset_bytes Offset in bytes
 * @param size_bytes Size of the data to update
 * @return RendererError indicating success or failure
 */
RendererError vkr_uniform_buffer_update(RendererFrontendHandle renderer,
                                        VkrUniformBuffer *uniform_buffer,
                                        const void *data, uint64_t offset_bytes,
                                        uint64_t size_bytes);

// =============================================================================
// Buffer Cleanup
// =============================================================================

/**
 * @brief Destroys a vertex buffer
 * @param renderer Renderer instance
 * @param vertex_buffer Vertex buffer to destroy
 */
void vkr_vertex_buffer_destroy(RendererFrontendHandle renderer,
                               VkrVertexBuffer *vertex_buffer);

/**
 * @brief Destroys an index buffer
 * @param renderer Renderer instance
 * @param index_buffer Index buffer to destroy
 */
void vkr_index_buffer_destroy(RendererFrontendHandle renderer,
                              VkrIndexBuffer *index_buffer);

/**
 * @brief Destroys a uniform buffer
 * @param renderer Renderer instance
 * @param uniform_buffer Uniform buffer to destroy
 */
void vkr_uniform_buffer_destroy(RendererFrontendHandle renderer,
                                VkrUniformBuffer *uniform_buffer);