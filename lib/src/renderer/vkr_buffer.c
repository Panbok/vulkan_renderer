#include "vkr_buffer.h"

// =============================================================================
// Buffer Creation Functions
// =============================================================================

VkrVertexBuffer vkr_vertex_buffer_create(VkrRendererFrontendHandle renderer,
                                         const void *data, uint32_t stride,
                                         uint32_t vertex_count,
                                         VkrVertexInputRate input_rate,
                                         String8 debug_name,
                                         VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(stride > 0, "Stride must be > 0");
  assert_log(vertex_count > 0, "Vertex count must be > 0");

  log_debug("Creating vertex buffer");

  VkrVertexBuffer vertex_buffer = {0};
  vertex_buffer.stride = stride;
  vertex_buffer.vertex_count = vertex_count;
  vertex_buffer.input_rate = input_rate;
  vertex_buffer.debug_name = debug_name;
  vertex_buffer.size_bytes = (uint64_t)stride * vertex_count;

  vertex_buffer.handle = vkr_renderer_create_vertex_buffer(
      renderer, vertex_buffer.size_bytes, data, out_error);

  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to create vertex buffer: %s", string8_cstr(&debug_name));
    return vertex_buffer;
  }

  log_debug("Created vertex buffer '%s': %u vertices, stride %u, %llu bytes",
            string8_cstr(&debug_name), vertex_count, stride,
            vertex_buffer.size_bytes);

  return vertex_buffer;
}

VkrIndexBuffer vkr_index_buffer_create(VkrRendererFrontendHandle renderer,
                                       const void *data, VkrIndexType type,
                                       uint32_t index_count, String8 debug_name,
                                       VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(index_count > 0, "Index count must be > 0");

  VkrIndexBuffer index_buffer = {0};
  index_buffer.type = type;
  index_buffer.index_count = index_count;
  index_buffer.debug_name = debug_name;

  uint32_t index_size =
      (type == VKR_INDEX_TYPE_UINT16) ? sizeof(uint16_t) : sizeof(uint32_t);
  index_buffer.size_bytes = (uint64_t)index_size * index_count;

  index_buffer.handle = vkr_renderer_create_index_buffer(
      renderer, index_buffer.size_bytes, type, data, out_error);

  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to create index buffer: %s", string8_cstr(&debug_name));
    return index_buffer;
  }

  log_debug("Created index buffer '%s': %u indices, type %s, %llu bytes",
            string8_cstr(&debug_name), index_count,
            (type == VKR_INDEX_TYPE_UINT16) ? "uint16" : "uint32",
            index_buffer.size_bytes);

  return index_buffer;
}

VkrUniformBuffer
vkr_uniform_buffer_create(VkrRendererFrontendHandle renderer, const void *data,
                          uint64_t size_bytes, uint32_t binding,
                          VkrShaderStageFlags stages, bool32_t dynamic,
                          String8 debug_name, VkrRendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(size_bytes > 0, "Size must be > 0");

  VkrUniformBuffer uniform_buffer = {0};
  uniform_buffer.binding = binding;
  uniform_buffer.stages = stages;
  uniform_buffer.size_bytes = size_bytes;
  uniform_buffer.dynamic = dynamic;
  uniform_buffer.debug_name = debug_name;

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  VkrBufferDescription desc = {
      .size = size_bytes,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_UNIFORM |
                                                VKR_BUFFER_USAGE_TRANSFER_DST |
                                                VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = dynamic ? vkr_memory_property_flags_from_bits(
                                         VKR_MEMORY_PROPERTY_HOST_VISIBLE |
                                         VKR_MEMORY_PROPERTY_HOST_COHERENT)
                                   : vkr_memory_property_flags_from_bits(
                                         VKR_MEMORY_PROPERTY_DEVICE_LOCAL),
      .buffer_type = buffer_type};

  uniform_buffer.handle =
      vkr_renderer_create_buffer(renderer, &desc, data, out_error);

  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to create uniform buffer: %s", string8_cstr(&debug_name));
    return uniform_buffer;
  }

  log_debug("Created uniform buffer '%s': binding %u, %llu bytes, %s",
            string8_cstr(&debug_name), binding, size_bytes,
            dynamic ? "dynamic" : "static");

  return uniform_buffer;
}

// =============================================================================
// Buffer Update Functions
// =============================================================================

VkrRendererError vkr_vertex_buffer_update(VkrRendererFrontendHandle renderer,
                                          VkrVertexBuffer *vertex_buffer,
                                          const void *data,
                                          uint32_t offset_vertices,
                                          uint32_t vertex_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_buffer != NULL, "Vertex buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_vertices > vertex_buffer->vertex_count ||
      vertex_count > (vertex_buffer->vertex_count - offset_vertices)) {
    log_error("Vertex buffer update out of bounds: offset %u + count %u > "
              "capacity %u",
              offset_vertices, vertex_count, vertex_buffer->vertex_count);
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t offset_bytes = (uint64_t)offset_vertices * vertex_buffer->stride;
  uint64_t size_bytes = (uint64_t)vertex_count * vertex_buffer->stride;

  VkrRendererError result = vkr_renderer_update_buffer(
      renderer, vertex_buffer->handle, offset_bytes, size_bytes, data);

  if (result != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to update vertex buffer '%s'",
              string8_cstr(&vertex_buffer->debug_name));
  }

  return result;
}

VkrRendererError vkr_index_buffer_update(VkrRendererFrontendHandle renderer,
                                         VkrIndexBuffer *index_buffer,
                                         const void *data,
                                         uint32_t offset_indices,
                                         uint32_t index_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(index_buffer != NULL, "Index buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_indices > index_buffer->index_count ||
      index_count > (index_buffer->index_count - offset_indices)) {
    log_error(
        "Index buffer update out of bounds: offset %u + count %u > capacity %u",
        offset_indices, index_count, index_buffer->index_count);
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t index_size = (index_buffer->type == VKR_INDEX_TYPE_UINT16)
                            ? sizeof(uint16_t)
                            : sizeof(uint32_t);
  uint64_t offset_bytes = (uint64_t)offset_indices * index_size;
  uint64_t size_bytes = (uint64_t)index_count * index_size;

  VkrRendererError result = vkr_renderer_update_buffer(
      renderer, index_buffer->handle, offset_bytes, size_bytes, data);

  if (result != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to update index buffer '%s'",
              string8_cstr(&index_buffer->debug_name));
  }

  return result;
}

VkrRendererError vkr_uniform_buffer_update(VkrRendererFrontendHandle renderer,
                                           VkrUniformBuffer *uniform_buffer,
                                           const void *data,
                                           uint64_t offset_bytes,
                                           uint64_t size_bytes) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(uniform_buffer != NULL, "Uniform buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_bytes > uniform_buffer->size_bytes ||
      size_bytes > (uniform_buffer->size_bytes - offset_bytes)) {
    log_error("Uniform buffer update out of bounds: offset %llu + size %llu > "
              "capacity %llu",
              offset_bytes, size_bytes, uniform_buffer->size_bytes);
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }

  VkrRendererError result = vkr_renderer_update_buffer(
      renderer, uniform_buffer->handle, offset_bytes, size_bytes, data);

  if (result != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to update uniform buffer '%s'",
              string8_cstr(&uniform_buffer->debug_name));
  }

  return result;
}

// =============================================================================
// Buffer Cleanup
// =============================================================================

void vkr_vertex_buffer_destroy(VkrRendererFrontendHandle renderer,
                               VkrVertexBuffer *vertex_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_buffer != NULL, "Vertex buffer is NULL");

  if (vertex_buffer->handle != NULL) {
    log_debug("Destroying vertex buffer '%s'",
              string8_cstr(&vertex_buffer->debug_name));
    vkr_renderer_destroy_buffer(renderer, vertex_buffer->handle);
    vertex_buffer->handle = NULL;
  }

  MemZero(vertex_buffer, sizeof(VkrVertexBuffer));
}

void vkr_index_buffer_destroy(VkrRendererFrontendHandle renderer,
                              VkrIndexBuffer *index_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(index_buffer != NULL, "Index buffer is NULL");

  if (index_buffer->handle != NULL) {
    log_debug("Destroying index buffer '%s'",
              string8_cstr(&index_buffer->debug_name));
    vkr_renderer_destroy_buffer(renderer, index_buffer->handle);
    index_buffer->handle = NULL;
  }

  MemZero(index_buffer, sizeof(VkrIndexBuffer));
}

void vkr_uniform_buffer_destroy(VkrRendererFrontendHandle renderer,
                                VkrUniformBuffer *uniform_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(uniform_buffer != NULL, "Uniform buffer is NULL");

  if (uniform_buffer->handle != NULL) {
    log_debug("Destroying uniform buffer '%s'",
              string8_cstr(&uniform_buffer->debug_name));
    vkr_renderer_destroy_buffer(renderer, uniform_buffer->handle);
    uniform_buffer->handle = NULL;
  }

  MemZero(uniform_buffer, sizeof(VkrUniformBuffer));
}
