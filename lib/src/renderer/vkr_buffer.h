#pragma once

#include "containers/array.h"
#include "defines.h"
#include "renderer/vkr_renderer.h"

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
  VkrBufferHandle handle;
  uint32_t stride;               // Size of one vertex in bytes
  uint32_t vertex_count;         // Number of vertices in this buffer
  VkrVertexInputRate input_rate; // Per-vertex or per-instance

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} VkrVertexBuffer;

Array(VkrVertexBuffer);

/**
 * @brief Index buffer with metadata for rendering operations
 */
typedef struct VkrIndexBuffer {
  VkrBufferHandle handle;
  VkrIndexType type;    // uint16 or uint32
  uint32_t index_count; // Number of indices

  // Optional metadata
  String8 debug_name;  // For debugging/profiling
  uint64_t size_bytes; // Total buffer size
} VkrIndexBuffer;

/**
 * @brief Uniform buffer for shader constants
 */
typedef struct VkrUniformBuffer {
  VkrBufferHandle handle;
  uint32_t binding;           // Descriptor set binding point
  VkrShaderStageFlags stages; // Which shader stages use this
  uint64_t size_bytes;        // Buffer size

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
 * @param data Vertex data
 * @param stride Size of one vertex
 * @param vertex_count Number of vertices
 * @param input_rate Per-vertex or per-instance
 * @param debug_name Optional debug name
 * @param out_error Error output
 * @return Created VkrVertexBuffer
 */
VkrVertexBuffer vkr_vertex_buffer_create(VkrRendererFrontendHandle renderer,
                                         const void *data, uint32_t stride,
                                         uint32_t vertex_count,
                                         VkrVertexInputRate input_rate,
                                         String8 debug_name,
                                         VkrRendererError *out_error);

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
VkrIndexBuffer vkr_index_buffer_create(VkrRendererFrontendHandle renderer,
                                       const void *data, VkrIndexType type,
                                       uint32_t index_count, String8 debug_name,
                                       VkrRendererError *out_error);

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
VkrUniformBuffer
vkr_uniform_buffer_create(VkrRendererFrontendHandle renderer, const void *data,
                          uint64_t size_bytes, uint32_t binding,
                          VkrShaderStageFlags stages, bool32_t dynamic,
                          String8 debug_name, VkrRendererError *out_error);

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
VkrRendererError vkr_vertex_buffer_update(VkrRendererFrontendHandle renderer,
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
VkrRendererError vkr_index_buffer_update(VkrRendererFrontendHandle renderer,
                                         VkrIndexBuffer *index_buffer,
                                         const void *data,
                                         uint32_t offset_indices,
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
VkrRendererError vkr_uniform_buffer_update(VkrRendererFrontendHandle renderer,
                                           VkrUniformBuffer *uniform_buffer,
                                           const void *data,
                                           uint64_t offset_bytes,
                                           uint64_t size_bytes);

// =============================================================================
// Buffer Cleanup
// =============================================================================

/**
 * @brief Destroys a vertex buffer
 * @param renderer Renderer instance
 * @param vertex_buffer Vertex buffer to destroy
 */
void vkr_vertex_buffer_destroy(VkrRendererFrontendHandle renderer,
                               VkrVertexBuffer *vertex_buffer);

/**
 * @brief Destroys an index buffer
 * @param renderer Renderer instance
 * @param index_buffer Index buffer to destroy
 */
void vkr_index_buffer_destroy(VkrRendererFrontendHandle renderer,
                              VkrIndexBuffer *index_buffer);

/**
 * @brief Destroys a uniform buffer
 * @param renderer Renderer instance
 * @param uniform_buffer Uniform buffer to destroy
 */
void vkr_uniform_buffer_destroy(VkrRendererFrontendHandle renderer,
                                VkrUniformBuffer *uniform_buffer);