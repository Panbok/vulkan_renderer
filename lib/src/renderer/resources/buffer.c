#include "buffer.h"

// =============================================================================
// Buffer Creation Functions
// =============================================================================

VertexBuffer vertex_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                                  const void *data, uint32_t stride,
                                  uint32_t vertex_count,
                                  VertexInputRate input_rate,
                                  String8 debug_name,
                                  RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(stride > 0, "Stride must be > 0");
  assert_log(vertex_count > 0, "Vertex count must be > 0");

  log_debug("Creating vertex buffer");

  VertexBuffer vertex_buffer = {0};
  vertex_buffer.stride = stride;
  vertex_buffer.vertex_count = vertex_count;
  vertex_buffer.input_rate = input_rate;
  vertex_buffer.debug_name = debug_name;
  vertex_buffer.size_bytes = (uint64_t)stride * vertex_count;

  vertex_buffer.handle = renderer_create_vertex_buffer(
      renderer, vertex_buffer.size_bytes, data, out_error);

  if (*out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create vertex buffer: %s", string8_cstr(&debug_name));
    return vertex_buffer;
  }

  log_debug("Created vertex buffer '%s': %u vertices, stride %u, %llu bytes",
            string8_cstr(&debug_name), vertex_count, stride,
            vertex_buffer.size_bytes);

  return vertex_buffer;
}

IndexBuffer index_buffer_create(RendererFrontendHandle renderer, Arena *arena,
                                const void *data, IndexType type,
                                uint32_t index_count, String8 debug_name,
                                RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(index_count > 0, "Index count must be > 0");

  IndexBuffer index_buffer = {0};
  index_buffer.type = type;
  index_buffer.index_count = index_count;
  index_buffer.debug_name = debug_name;

  uint32_t index_size =
      (type == INDEX_TYPE_UINT16) ? sizeof(uint16_t) : sizeof(uint32_t);
  index_buffer.size_bytes = (uint64_t)index_size * index_count;

  index_buffer.handle = renderer_create_index_buffer(
      renderer, index_buffer.size_bytes, type, data, out_error);

  if (*out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to create index buffer: %s", string8_cstr(&debug_name));
    return index_buffer;
  }

  log_debug("Created index buffer '%s': %u indices, type %s, %llu bytes",
            string8_cstr(&debug_name), index_count,
            (type == INDEX_TYPE_UINT16) ? "uint16" : "uint32",
            index_buffer.size_bytes);

  return index_buffer;
}

UniformBuffer uniform_buffer_create(RendererFrontendHandle renderer,
                                    Arena *arena, const void *data,
                                    uint64_t size_bytes, uint32_t binding,
                                    ShaderStageFlags stages, bool32_t dynamic,
                                    String8 debug_name,
                                    RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(size_bytes > 0, "Size must be > 0");

  UniformBuffer uniform_buffer = {0};
  uniform_buffer.binding = binding;
  uniform_buffer.stages = stages;
  uniform_buffer.size_bytes = size_bytes;
  uniform_buffer.dynamic = dynamic;
  uniform_buffer.debug_name = debug_name;

  BufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, BUFFER_TYPE_GRAPHICS);

  BufferDescription desc = {
      .size = size_bytes,
      .usage = buffer_usage_flags_from_bits(BUFFER_USAGE_UNIFORM |
                                            BUFFER_USAGE_TRANSFER_DST |
                                            BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties =
          dynamic
              ? memory_property_flags_from_bits(MEMORY_PROPERTY_HOST_VISIBLE |
                                                MEMORY_PROPERTY_HOST_COHERENT)
              : memory_property_flags_from_bits(MEMORY_PROPERTY_DEVICE_LOCAL),
      .buffer_type = buffer_type};

  uniform_buffer.handle =
      renderer_create_buffer(renderer, &desc, data, out_error);

  if (*out_error != RENDERER_ERROR_NONE) {
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

RendererError vertex_buffer_update(RendererFrontendHandle renderer,
                                   VertexBuffer *vertex_buffer,
                                   const void *data, uint32_t offset_vertices,
                                   uint32_t vertex_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_buffer != NULL, "Vertex buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_vertices + vertex_count > vertex_buffer->vertex_count) {
    log_error("Vertex buffer update out of bounds: offset %u + count %u > "
              "capacity %u",
              offset_vertices, vertex_count, vertex_buffer->vertex_count);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint64_t offset_bytes = (uint64_t)offset_vertices * vertex_buffer->stride;
  uint64_t size_bytes = (uint64_t)vertex_count * vertex_buffer->stride;

  RendererError result = renderer_update_buffer(renderer, vertex_buffer->handle,
                                                offset_bytes, size_bytes, data);

  if (result != RENDERER_ERROR_NONE) {
    log_error("Failed to update vertex buffer '%s'",
              string8_cstr(&vertex_buffer->debug_name));
  }

  return result;
}

RendererError index_buffer_update(RendererFrontendHandle renderer,
                                  IndexBuffer *index_buffer, const void *data,
                                  uint32_t offset_indices,
                                  uint32_t index_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(index_buffer != NULL, "Index buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_indices + index_count > index_buffer->index_count) {
    log_error(
        "Index buffer update out of bounds: offset %u + count %u > capacity %u",
        offset_indices, index_count, index_buffer->index_count);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  uint32_t index_size = (index_buffer->type == INDEX_TYPE_UINT16)
                            ? sizeof(uint16_t)
                            : sizeof(uint32_t);
  uint64_t offset_bytes = (uint64_t)offset_indices * index_size;
  uint64_t size_bytes = (uint64_t)index_count * index_size;

  RendererError result = renderer_update_buffer(renderer, index_buffer->handle,
                                                offset_bytes, size_bytes, data);

  if (result != RENDERER_ERROR_NONE) {
    log_error("Failed to update index buffer '%s'",
              string8_cstr(&index_buffer->debug_name));
  }

  return result;
}

RendererError uniform_buffer_update(RendererFrontendHandle renderer,
                                    UniformBuffer *uniform_buffer,
                                    const void *data, uint64_t offset_bytes,
                                    uint64_t size_bytes) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(uniform_buffer != NULL, "Uniform buffer is NULL");
  assert_log(data != NULL, "Data is NULL");

  if (offset_bytes + size_bytes > uniform_buffer->size_bytes) {
    log_error("Uniform buffer update out of bounds: offset %llu + size %llu > "
              "capacity %llu",
              offset_bytes, size_bytes, uniform_buffer->size_bytes);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  RendererError result = renderer_update_buffer(
      renderer, uniform_buffer->handle, offset_bytes, size_bytes, data);

  if (result != RENDERER_ERROR_NONE) {
    log_error("Failed to update uniform buffer '%s'",
              string8_cstr(&uniform_buffer->debug_name));
  }

  return result;
}

// =============================================================================
// Buffer Cleanup
// =============================================================================

void vertex_buffer_destroy(RendererFrontendHandle renderer,
                           VertexBuffer *vertex_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_buffer != NULL, "Vertex buffer is NULL");

  if (vertex_buffer->handle != NULL) {
    log_debug("Destroying vertex buffer '%s'",
              string8_cstr(&vertex_buffer->debug_name));
    renderer_destroy_buffer(renderer, vertex_buffer->handle);
    vertex_buffer->handle = NULL;
  }

  MemZero(vertex_buffer, sizeof(VertexBuffer));
}

void index_buffer_destroy(RendererFrontendHandle renderer,
                          IndexBuffer *index_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(index_buffer != NULL, "Index buffer is NULL");

  if (index_buffer->handle != NULL) {
    log_debug("Destroying index buffer '%s'",
              string8_cstr(&index_buffer->debug_name));
    renderer_destroy_buffer(renderer, index_buffer->handle);
    index_buffer->handle = NULL;
  }

  MemZero(index_buffer, sizeof(IndexBuffer));
}

void uniform_buffer_destroy(RendererFrontendHandle renderer,
                            UniformBuffer *uniform_buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(uniform_buffer != NULL, "Uniform buffer is NULL");

  if (uniform_buffer->handle != NULL) {
    log_debug("Destroying uniform buffer '%s'",
              string8_cstr(&uniform_buffer->debug_name));
    renderer_destroy_buffer(renderer, uniform_buffer->handle);
    uniform_buffer->handle = NULL;
  }

  MemZero(uniform_buffer, sizeof(UniformBuffer));
}

// =============================================================================
// Vertex Array Creation and Management
// =============================================================================

VertexArray vertex_array_create(Arena *arena, uint32_t max_vertex_buffers,
                                PrimitiveTopology topology,
                                String8 debug_name) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(max_vertex_buffers > 0, "Max vertex buffers must be > 0");

  VertexArray vertex_array = {0};
  vertex_array.arena = arena;
  vertex_array.topology = topology;
  vertex_array.debug_name = debug_name;
  vertex_array.state = bitset8_create();
  bitset8_set(&vertex_array.state, VERTEX_ARRAY_STATE_INITIALIZED);

  // Create vertex buffer array
  vertex_array.vertex_buffers =
      array_create_VertexBuffer(arena, max_vertex_buffers);

  // Initialize with empty arrays (will be allocated when needed)
  vertex_array.attributes = NULL;
  vertex_array.bindings = NULL;
  vertex_array.attribute_count = 0;
  vertex_array.binding_count = 0;

  log_debug("Created vertex array '%s' with capacity for %u vertex buffers",
            string8_cstr(&debug_name), max_vertex_buffers);

  return vertex_array;
}

void vertex_array_destroy(RendererFrontendHandle renderer,
                          VertexArray *vertex_array) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_array != NULL, "Vertex array is NULL");

  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    VertexBuffer *vb = array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    vertex_buffer_destroy(renderer, vb);
  }

  index_buffer_destroy(renderer, &vertex_array->index_buffer);

  array_destroy_VertexBuffer(&vertex_array->vertex_buffers);

  MemZero(vertex_array, sizeof(VertexArray));
}

RendererError vertex_array_add_vertex_buffer(VertexArray *vertex_array,
                                             const VertexBuffer *vertex_buffer,
                                             uint32_t binding_index) {
  assert_log(vertex_array != NULL, "Vertex array is NULL");
  assert_log(vertex_buffer != NULL, "Vertex buffer is NULL");

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    log_error("Vertex array not initialized");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // Check if we have space
  if (vertex_array->vertex_buffers.length == 0) {
    log_error("Vertex array buffer storage is full");
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  // Find an empty slot or check if binding already exists
  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    VertexBuffer *existing =
        array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (existing->handle == NULL) {
      // Found empty slot
      *existing = *vertex_buffer;
      log_debug("Added vertex buffer to vertex array '%s' at binding %u",
                string8_cstr(&vertex_array->debug_name), binding_index);
      return RENDERER_ERROR_NONE;
    }
  }

  log_error("No available slots in vertex array for new vertex buffer");
  return RENDERER_ERROR_OUT_OF_MEMORY;
}

RendererError vertex_array_set_index_buffer(VertexArray *vertex_array,
                                            const IndexBuffer *index_buffer) {
  assert_log(vertex_array != NULL, "Vertex array is NULL");
  assert_log(index_buffer != NULL, "Index buffer is NULL");

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    log_error("Vertex array not initialized");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  vertex_array->index_buffer = *index_buffer;
  bitset8_set(&vertex_array->state, VERTEX_ARRAY_STATE_HAS_INDEX_BUFFER);

  log_debug("Set index buffer for vertex array '%s'",
            string8_cstr(&vertex_array->debug_name));

  return RENDERER_ERROR_NONE;
}

RendererError vertex_array_add_attribute(VertexArray *vertex_array,
                                         uint32_t location, uint32_t binding,
                                         VertexFormat format, uint32_t offset) {
  assert_log(vertex_array != NULL, "Vertex array is NULL");

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    log_error("Vertex array not initialized");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // Invalidate computed pipeline data since we're adding new attributes
  bitset8_clear(&vertex_array->state, VERTEX_ARRAY_STATE_PIPELINE_DATA_VALID);

  // For now, we'll defer actual attribute storage until compute_pipeline_data
  // This is because we need to know the total count to allocate properly

  log_debug("Marked attribute %u (binding %u, format %d, offset %u) for vertex "
            "array '%s'",
            location, binding, format, offset,
            string8_cstr(&vertex_array->debug_name));

  return RENDERER_ERROR_NONE;
}

RendererError vertex_array_compute_pipeline_data(VertexArray *vertex_array) {
  assert_log(vertex_array != NULL, "Vertex array is NULL");

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    log_error("Vertex array not initialized");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // Count non-null vertex buffers to determine binding count
  uint32_t binding_count = 0;
  uint32_t attribute_count = 0;

  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    VertexBuffer *vb = array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      binding_count++;
      // For simplicity, assume each vertex buffer contributes one attribute
      // In a real implementation, this would be more sophisticated
      attribute_count++;
    }
  }

  if (binding_count == 0) {
    log_error("No vertex buffers in vertex array");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // Allocate space for bindings and attributes
  vertex_array->bindings =
      arena_alloc(vertex_array->arena,
                  binding_count * sizeof(VertexInputBindingDescription),
                  ARENA_MEMORY_TAG_RENDERER);
  vertex_array->attributes =
      arena_alloc(vertex_array->arena,
                  attribute_count * sizeof(VertexInputAttributeDescription),
                  ARENA_MEMORY_TAG_RENDERER);

  if (!vertex_array->bindings || !vertex_array->attributes) {
    log_error("Failed to allocate memory for vertex array pipeline data");
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  // Fill in binding descriptions
  uint32_t binding_index = 0;
  uint32_t attr_index = 0;

  uint32_t offset = 0;
  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    VertexBuffer *vb = array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      vertex_array->bindings[binding_index] =
          (VertexInputBindingDescription){.binding = binding_index,
                                          .stride = offset + vb->stride,
                                          .input_rate = vb->input_rate};

      // Create a simple attribute assuming the entire vertex buffer is one
      // attribute In a real implementation, this would be more sophisticated
      vertex_array->attributes[attr_index] = (VertexInputAttributeDescription){
          .location = attr_index,
          .binding = binding_index,
          .format = VERTEX_FORMAT_R32G32B32_SFLOAT, // Default format
          .offset = offset};

      binding_index++;
      attr_index++;
      offset += vb->stride;
    }
  }

  vertex_array->binding_count = binding_count;
  vertex_array->attribute_count = attribute_count;
  bitset8_set(&vertex_array->state, VERTEX_ARRAY_STATE_PIPELINE_DATA_VALID);

  log_debug("Computed pipeline data for vertex array '%s': %u bindings, %u "
            "attributes",
            string8_cstr(&vertex_array->debug_name), binding_count,
            attribute_count);

  return RENDERER_ERROR_NONE;
}

/**
 * @brief Computes pipeline data for interleaved vertex arrays
 * @param vertex_array Vertex array with interleaved data and pre-added
 * attributes
 * @return Error code
 */
static RendererError
vertex_array_compute_pipeline_data_interleaved(VertexArray *vertex_array) {
  assert_log(vertex_array != NULL, "Vertex array is NULL");

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    log_error("Vertex array not initialized");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // For interleaved layouts, we should have exactly one vertex buffer
  if (vertex_array->vertex_buffers.length != 1) {
    log_error("Interleaved vertex array should have exactly one vertex buffer, "
              "got %llu",
              vertex_array->vertex_buffers.length);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  VertexBuffer *vb = array_get_VertexBuffer(&vertex_array->vertex_buffers, 0);
  if (vb->handle == NULL) {
    log_error("No vertex buffer in interleaved vertex array");
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // We need to count how many attributes were added via
  // vertex_array_add_attribute calls For now, since we don't store them
  // dynamically, we'll deduce from the vertex format
  uint32_t attribute_count = 0;
  uint32_t binding_count = 1; // Always one binding for interleaved

  // Estimate attribute count based on stride
  if (vb->stride == sizeof(InterleavedVertex_PositionColor)) {
    attribute_count = 2; // position + color
  } else if (vb->stride == sizeof(InterleavedVertex_PositionNormalColor)) {
    attribute_count = 3; // position + normal + color
  } else if (vb->stride == sizeof(InterleavedVertex_PositionNormalTexcoord)) {
    attribute_count = 3; // position + normal + texcoord
  } else if (vb->stride == sizeof(InterleavedVertex_Full)) {
    attribute_count = 4; // position + normal + texcoord + color
  } else if (vb->stride == sizeof(Vec3)) {
    attribute_count = 1; // position only
  } else {
    log_error("Unsupported interleaved vertex stride: %u", vb->stride);
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  // Allocate space for bindings and attributes
  vertex_array->bindings =
      arena_alloc(vertex_array->arena,
                  binding_count * sizeof(VertexInputBindingDescription),
                  ARENA_MEMORY_TAG_RENDERER);
  vertex_array->attributes =
      arena_alloc(vertex_array->arena,
                  attribute_count * sizeof(VertexInputAttributeDescription),
                  ARENA_MEMORY_TAG_RENDERER);

  if (!vertex_array->bindings || !vertex_array->attributes) {
    log_error(
        "Failed to allocate memory for interleaved vertex array pipeline data");
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  // Set up single binding description
  vertex_array->bindings[0] = (VertexInputBindingDescription){
      .binding = 0, .stride = vb->stride, .input_rate = vb->input_rate};

  // Set up attribute descriptions based on vertex format
  if (vb->stride == sizeof(InterleavedVertex_PositionColor)) {
    vertex_array->attributes[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionColor, position)};
    vertex_array->attributes[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionColor, color)};
  } else if (vb->stride == sizeof(InterleavedVertex_PositionNormalColor)) {
    vertex_array->attributes[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, position)};
    vertex_array->attributes[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, normal)};
    vertex_array->attributes[2] = (VertexInputAttributeDescription){
        .location = 2,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_PositionNormalColor, color)};
  } else if (vb->stride == sizeof(InterleavedVertex_Full)) {
    vertex_array->attributes[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, position)};
    vertex_array->attributes[1] = (VertexInputAttributeDescription){
        .location = 1,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, normal)};
    vertex_array->attributes[2] = (VertexInputAttributeDescription){
        .location = 2,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, texcoord)};
    vertex_array->attributes[3] = (VertexInputAttributeDescription){
        .location = 3,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = offsetof(InterleavedVertex_Full, color)};
  } else if (vb->stride == sizeof(Vec3)) {
    vertex_array->attributes[0] = (VertexInputAttributeDescription){
        .location = 0,
        .binding = 0,
        .format = VERTEX_FORMAT_R32G32B32_SFLOAT,
        .offset = 0};
  }

  vertex_array->binding_count = binding_count;
  vertex_array->attribute_count = attribute_count;
  bitset8_set(&vertex_array->state, VERTEX_ARRAY_STATE_PIPELINE_DATA_VALID);

  log_debug("Computed interleaved pipeline data for vertex array '%s': %u "
            "bindings, %u attributes",
            string8_cstr(&vertex_array->debug_name), binding_count,
            attribute_count);

  return RENDERER_ERROR_NONE;
}

// =============================================================================
// Rendering Functions
// =============================================================================

void vertex_array_bind(RendererFrontendHandle renderer,
                       const VertexArray *vertex_array) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_array != NULL, "Vertex array is NULL");
  assert_log(renderer_is_frame_active(renderer), "No active frame");

  if (!vertex_array_is_valid(vertex_array)) {
    log_error("Attempting to bind invalid vertex array");
    return;
  }

  // Bind all vertex buffers
  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    const VertexBuffer *vb =
        array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      VertexBufferBinding binding = {
          .buffer = vb->handle, .binding = (uint32_t)i, .offset = 0};
      renderer_bind_vertex_buffer(renderer, &binding);
    }
  }

  // Bind index buffer if present
  if (bitset8_is_set(&vertex_array->state,
                     VERTEX_ARRAY_STATE_HAS_INDEX_BUFFER)) {
    IndexBufferBinding index_binding = {.buffer =
                                            vertex_array->index_buffer.handle,
                                        .type = vertex_array->index_buffer.type,
                                        .offset = 0};
    renderer_bind_index_buffer(renderer, &index_binding);
  }
}

void vertex_array_draw(RendererFrontendHandle renderer,
                       const VertexArray *vertex_array,
                       uint32_t instance_count) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_array != NULL, "Vertex array is NULL");
  assert_log(renderer_is_frame_active(renderer), "No active frame");

  if (!vertex_array_is_valid(vertex_array)) {
    log_error("Attempting to draw invalid vertex array");
    return;
  }

  if (bitset8_is_set(&vertex_array->state,
                     VERTEX_ARRAY_STATE_HAS_INDEX_BUFFER)) {
    renderer_draw_indexed(renderer, vertex_array->index_buffer.index_count,
                          instance_count, 0, 0, 0);
  } else {
    uint32_t vertex_count = vertex_array_get_vertex_count(vertex_array);
    renderer_draw(renderer, vertex_count, instance_count, 0, 0);
  }
}

void vertex_array_render(RendererFrontendHandle renderer,
                         const VertexArray *vertex_array,
                         uint32_t instance_count) {
  vertex_array_bind(renderer, vertex_array);
  vertex_array_draw(renderer, vertex_array, instance_count);
}

// =============================================================================
// Mesh Conversion Functions
// =============================================================================

VertexArrayFromMeshOptions vertex_array_from_mesh_options_create(void) {
  return bitset8_create();
}

VertexArrayFromMeshOptions
vertex_array_from_mesh_options_from_flags(uint8_t flags) {
  VertexArrayFromMeshOptions options = bitset8_create();

  // Set individual flags using bitset operations
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED);
  }
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS);
  }
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TANGENTS) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TANGENTS);
  }
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_BITANGENTS) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_BITANGENTS);
  }
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS);
  }
  if (flags & VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS) {
    bitset8_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS);
  }

  return options;
}

void vertex_array_from_mesh_options_add_flag(
    VertexArrayFromMeshOptions *options, VertexArrayFromMeshOptionFlags flag) {
  assert_log(options != NULL, "Options is NULL");
  bitset8_set(options, flag);
}

// =============================================================================
// Internal Helper Functions
// =============================================================================

/**
 * @brief Creates an interleaved vertex array from a mesh
 * @param renderer Renderer instance
 * @param arena Memory allocator
 * @param mesh Source mesh
 * @param options Conversion options
 * @param debug_name Debug name for the vertex array
 * @param out_error Error output
 * @return Created VertexArray with interleaved data
 */
static VertexArray vertex_array_from_mesh_interleaved(
    RendererFrontendHandle renderer, Arena *arena, const Mesh *mesh,
    VertexArrayFromMeshOptions options, String8 debug_name,
    RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(mesh != NULL, "Mesh is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VertexArray vertex_array = {0};
  *out_error = RENDERER_ERROR_NONE;

  // Determine which attributes to include
  bool32_t include_normals =
      bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS);
  bool32_t include_texcoords =
      bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS);
  bool32_t include_colors =
      bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS);

  // Create vertex array with single buffer capacity
  vertex_array = vertex_array_create(arena, 1, PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                     debug_name);

  // Determine vertex format and create interleaved data
  if (include_normals && include_texcoords && include_colors) {
    // Full vertex format: position + normal + texcoord + color
    InterleavedVertex_Full *vertex_data =
        arena_alloc(arena, mesh->vertex_count * sizeof(InterleavedVertex_Full),
                    ARENA_MEMORY_TAG_RENDERER);

    if (!vertex_data) {
      log_error("Failed to allocate memory for interleaved vertex data");
      *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
      return vertex_array;
    }

    // Convert from SoA to AoS
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
      vertex_data[i].position = *array_get_Vec3(&mesh->positions, i);
      vertex_data[i].normal = *array_get_Vec3(&mesh->normals, i);
      vertex_data[i].texcoord = *array_get_Vec2(&mesh->texcoords, i);
      vertex_data[i].color = *array_get_Vec3(&mesh->colors, i);
    }

    // Create single vertex buffer
    VertexBuffer interleaved_buffer = vertex_buffer_create(
        renderer, arena, vertex_data, sizeof(InterleavedVertex_Full),
        mesh->vertex_count, VERTEX_INPUT_RATE_VERTEX, debug_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create interleaved vertex buffer (full)");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &interleaved_buffer, 0);

    // Set up vertex attributes for interleaved layout
    vertex_array_add_attribute(&vertex_array, 0, 0,
                               VERTEX_FORMAT_R32G32B32_SFLOAT,
                               offsetof(InterleavedVertex_Full, position));
    vertex_array_add_attribute(&vertex_array, 1, 0,
                               VERTEX_FORMAT_R32G32B32_SFLOAT,
                               offsetof(InterleavedVertex_Full, normal));
    vertex_array_add_attribute(&vertex_array, 2, 0, VERTEX_FORMAT_R32G32_SFLOAT,
                               offsetof(InterleavedVertex_Full, texcoord));
    vertex_array_add_attribute(&vertex_array, 3, 0,
                               VERTEX_FORMAT_R32G32B32_SFLOAT,
                               offsetof(InterleavedVertex_Full, color));

  } else if (include_colors && !include_normals && !include_texcoords) {
    // Position + Color format
    InterleavedVertex_PositionColor *vertex_data = arena_alloc(
        arena, mesh->vertex_count * sizeof(InterleavedVertex_PositionColor),
        ARENA_MEMORY_TAG_RENDERER);

    if (!vertex_data) {
      log_error("Failed to allocate memory for position-color vertex data");
      *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
      return vertex_array;
    }

    // Convert from SoA to AoS
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
      vertex_data[i].position = *array_get_Vec3(&mesh->positions, i);
      vertex_data[i].color = *array_get_Vec3(&mesh->colors, i);
    }

    // Create single vertex buffer
    VertexBuffer interleaved_buffer = vertex_buffer_create(
        renderer, arena, vertex_data, sizeof(InterleavedVertex_PositionColor),
        mesh->vertex_count, VERTEX_INPUT_RATE_VERTEX, debug_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create interleaved vertex buffer (position-color)");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &interleaved_buffer, 0);

    // Set up vertex attributes for interleaved layout
    vertex_array_add_attribute(
        &vertex_array, 0, 0, VERTEX_FORMAT_R32G32B32_SFLOAT,
        offsetof(InterleavedVertex_PositionColor, position));
    vertex_array_add_attribute(
        &vertex_array, 1, 0, VERTEX_FORMAT_R32G32B32_SFLOAT,
        offsetof(InterleavedVertex_PositionColor, color));

  } else if (include_normals && include_colors && !include_texcoords) {
    // Position + Normal + Color format
    InterleavedVertex_PositionNormalColor *vertex_data = arena_alloc(
        arena,
        mesh->vertex_count * sizeof(InterleavedVertex_PositionNormalColor),
        ARENA_MEMORY_TAG_RENDERER);

    if (!vertex_data) {
      log_error(
          "Failed to allocate memory for position-normal-color vertex data");
      *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
      return vertex_array;
    }

    // Convert from SoA to AoS
    for (uint32_t i = 0; i < mesh->vertex_count; i++) {
      vertex_data[i].position = *array_get_Vec3(&mesh->positions, i);
      vertex_data[i].normal = *array_get_Vec3(&mesh->normals, i);
      vertex_data[i].color = *array_get_Vec3(&mesh->colors, i);
    }

    // Create single vertex buffer
    VertexBuffer interleaved_buffer = vertex_buffer_create(
        renderer, arena, vertex_data,
        sizeof(InterleavedVertex_PositionNormalColor), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, debug_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error(
          "Failed to create interleaved vertex buffer (position-normal-color)");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &interleaved_buffer, 0);

    // Set up vertex attributes for interleaved layout
    vertex_array_add_attribute(
        &vertex_array, 0, 0, VERTEX_FORMAT_R32G32B32_SFLOAT,
        offsetof(InterleavedVertex_PositionNormalColor, position));
    vertex_array_add_attribute(
        &vertex_array, 1, 0, VERTEX_FORMAT_R32G32B32_SFLOAT,
        offsetof(InterleavedVertex_PositionNormalColor, normal));
    vertex_array_add_attribute(
        &vertex_array, 2, 0, VERTEX_FORMAT_R32G32B32_SFLOAT,
        offsetof(InterleavedVertex_PositionNormalColor, color));

  } else {
    // Default to position-only for unsupported combinations
    log_warn("Unsupported attribute combination for interleaved layout, using "
             "position-only");

    VertexBuffer pos_buffer = vertex_buffer_create(
        renderer, arena, mesh->positions.data, sizeof(Vec3), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, debug_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create position-only vertex buffer");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &pos_buffer, 0);
    vertex_array_add_attribute(&vertex_array, 0, 0,
                               VERTEX_FORMAT_R32G32B32_SFLOAT, 0);
  }

  // Create index buffer if available
  if (mesh->indices.data != NULL && mesh->indices.length > 0) {
    String8 idx_name = string8_create_formatted(arena, "%s_indices",
                                                string8_cstr(&debug_name));
    IndexBuffer index_buffer = index_buffer_create(
        renderer, arena, mesh->indices.data, INDEX_TYPE_UINT32,
        mesh->index_count, idx_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error(
          "Failed to create index buffer for interleaved mesh conversion");
      return vertex_array;
    }

    vertex_array_set_index_buffer(&vertex_array, &index_buffer);
  }

  // Compute pipeline data for interleaved layout
  *out_error = vertex_array_compute_pipeline_data_interleaved(&vertex_array);
  if (*out_error != RENDERER_ERROR_NONE) {
    log_error(
        "Failed to compute interleaved pipeline data for mesh conversion");
    return vertex_array;
  }

  log_debug("Successfully converted mesh to interleaved vertex array '%s'",
            string8_cstr(&debug_name));

  return vertex_array;
}

VertexArray vertex_array_from_mesh(RendererFrontendHandle renderer,
                                   Arena *arena, const Mesh *mesh,
                                   VertexArrayFromMeshOptions options,
                                   String8 debug_name,
                                   RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(mesh != NULL, "Mesh is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VertexArray vertex_array = {0};
  *out_error = RENDERER_ERROR_NONE;

  if (!mesh_validate(mesh)) {
    log_error("Invalid mesh provided for vertex array conversion");
    *out_error = RENDERER_ERROR_INVALID_PARAMETER;
    return vertex_array;
  }

  // Check if interleaved mode is requested
  if (bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INTERLEAVED)) {
    return vertex_array_from_mesh_interleaved(renderer, arena, mesh, options,
                                              debug_name, out_error);
  }

  // Legacy separate buffer mode (keeping for compatibility)
  // Create vertex array
  uint32_t max_buffers = 4; // positions, normals, texcoords, colors
  vertex_array = vertex_array_create(
      arena, max_buffers, PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, debug_name);

  // Create position buffer (always required)
  if (mesh->positions.data != NULL && mesh->positions.length > 0) {
    String8 pos_name = string8_create_formatted(arena, "%s_positions",
                                                string8_cstr(&debug_name));
    VertexBuffer pos_buffer = vertex_buffer_create(
        renderer, arena, mesh->positions.data, sizeof(Vec3), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, pos_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create position buffer for mesh conversion");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &pos_buffer, 0);
  }

  // Create normal buffer if requested and available
  if (bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_NORMALS) &&
      mesh->normals.data != NULL && mesh->normals.length > 0) {
    String8 norm_name = string8_create_formatted(arena, "%s_normals",
                                                 string8_cstr(&debug_name));
    VertexBuffer norm_buffer = vertex_buffer_create(
        renderer, arena, mesh->normals.data, sizeof(Vec3), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, norm_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create normal buffer for mesh conversion");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &norm_buffer, 1);
  }

  // Create texcoord buffer if requested and available
  if (bitset8_is_set(&options,
                     VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_TEXCOORDS) &&
      mesh->texcoords.data != NULL && mesh->texcoords.length > 0) {
    String8 uv_name = string8_create_formatted(arena, "%s_texcoords",
                                               string8_cstr(&debug_name));
    VertexBuffer uv_buffer = vertex_buffer_create(
        renderer, arena, mesh->texcoords.data, sizeof(Vec2), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, uv_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create texcoord buffer for mesh conversion");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &uv_buffer, 2);
  }

  // Create color buffer if requested and available
  if (bitset8_is_set(&options, VERTEX_ARRAY_FROM_MESH_OPTION_INCLUDE_COLORS) &&
      mesh->colors.data != NULL && mesh->colors.length > 0) {
    String8 color_name =
        string8_create_formatted(arena, "%s_colors", string8_cstr(&debug_name));
    VertexBuffer color_buffer = vertex_buffer_create(
        renderer, arena, mesh->colors.data, sizeof(Vec3), mesh->vertex_count,
        VERTEX_INPUT_RATE_VERTEX, color_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create color buffer for mesh conversion");
      return vertex_array;
    }

    vertex_array_add_vertex_buffer(&vertex_array, &color_buffer, 3);
  }

  // Create index buffer if available
  if (mesh->indices.data != NULL && mesh->indices.length > 0) {
    String8 idx_name = string8_create_formatted(arena, "%s_indices",
                                                string8_cstr(&debug_name));
    IndexBuffer index_buffer = index_buffer_create(
        renderer, arena, mesh->indices.data, INDEX_TYPE_UINT32,
        mesh->index_count, idx_name, out_error);

    if (*out_error != RENDERER_ERROR_NONE) {
      log_error("Failed to create index buffer for mesh conversion");
      return vertex_array;
    }

    vertex_array_set_index_buffer(&vertex_array, &index_buffer);
  }

  // Compute pipeline data
  *out_error = vertex_array_compute_pipeline_data(&vertex_array);
  if (*out_error != RENDERER_ERROR_NONE) {
    log_error("Failed to compute pipeline data for mesh conversion");
    return vertex_array;
  }

  log_debug("Successfully converted mesh to vertex array '%s'",
            string8_cstr(&debug_name));

  return vertex_array;
}

// =============================================================================
// Utility Functions
// =============================================================================

bool32_t vertex_array_is_valid(const VertexArray *vertex_array) {
  if (vertex_array == NULL)
    return false_v;

  if (!bitset8_is_set(&vertex_array->state, VERTEX_ARRAY_STATE_INITIALIZED)) {
    return false_v;
  }

  bool32_t has_vertex_buffer = false_v;
  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    const VertexBuffer *vb =
        array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      has_vertex_buffer = true_v;
      break;
    }
  }

  return has_vertex_buffer;
}

uint64_t vertex_array_estimate_memory_usage(const VertexArray *vertex_array) {
  if (vertex_array == NULL)
    return 0;

  uint64_t total_size = 0;

  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    const VertexBuffer *vb =
        array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      total_size += vb->size_bytes;
    }
  }

  if (bitset8_is_set(&vertex_array->state,
                     VERTEX_ARRAY_STATE_HAS_INDEX_BUFFER)) {
    total_size += vertex_array->index_buffer.size_bytes;
  }

  return total_size;
}

uint32_t vertex_array_get_vertex_count(const VertexArray *vertex_array) {
  if (vertex_array == NULL)
    return 0;

  for (uint64_t i = 0; i < vertex_array->vertex_buffers.length; i++) {
    const VertexBuffer *vb =
        array_get_VertexBuffer(&vertex_array->vertex_buffers, i);
    if (vb->handle != NULL) {
      return vb->vertex_count;
    }
  }

  return 0;
}

// =============================================================================
// Batch Rendering Functions
// =============================================================================

void vertex_array_render_batch(RendererFrontendHandle renderer,
                               const VertexArray *vertex_arrays,
                               uint32_t array_count,
                               const uint32_t *instance_counts) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(vertex_arrays != NULL, "Vertex arrays is NULL");
  assert_log(instance_counts != NULL, "Instance counts is NULL");
  assert_log(renderer_is_frame_active(renderer), "No active frame");

  for (uint32_t i = 0; i < array_count; i++) {
    if (vertex_array_is_valid(&vertex_arrays[i])) {
      vertex_array_render(renderer, &vertex_arrays[i], instance_counts[i]);
    } else {
      log_warn("Skipping invalid vertex array %u in batch render", i);
    }
  }
}