#include "renderer/renderer_frontend.h"

RendererFrontendHandle renderer_create(Arena *arena,
                                       RendererBackendType backend_type,
                                       Window *window,
                                       DeviceRequirements *device_requirements,
                                       RendererError *out_error) {
  assert_log(window != NULL, "Window is NULL");
  assert_log(out_error != NULL, "Out error is NULL");
  assert_log(device_requirements != NULL, "Device requirements is NULL");

  log_debug("Creating renderer");

  struct s_RendererFrontend *renderer = arena_alloc(
      arena, sizeof(struct s_RendererFrontend), ARENA_MEMORY_TAG_RENDERER);
  renderer->arena = arena;
  renderer->backend_type = backend_type;
  renderer->window = window;
  renderer->frame_active = false;
  renderer->backend_state = NULL;

  if (backend_type == RENDERER_BACKEND_TYPE_VULKAN) {
    renderer->backend = renderer_vulkan_get_interface();
  } else {
    *out_error = RENDERER_ERROR_BACKEND_NOT_SUPPORTED;
    return NULL;
  }

  uint32_t width = (uint32_t)window->width;
  uint32_t height = (uint32_t)window->height;
  if (!renderer->backend.initialize(&renderer->backend_state, backend_type,
                                    window, width, height,
                                    device_requirements)) {
    *out_error = RENDERER_ERROR_INITIALIZATION_FAILED;
    return NULL;
  }

  *out_error = RENDERER_ERROR_NONE;
  return renderer;
}

void renderer_destroy(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");

  log_debug("Destroying renderer");

  if (renderer->backend_state && renderer->backend.shutdown) {
    renderer->backend.shutdown(renderer->backend_state);
  }
}

String8 renderer_get_error_string(RendererError error) {
  switch (error) {
  case RENDERER_ERROR_NONE:
    return string8_lit("No error");
  case RENDERER_ERROR_UNKNOWN:
    return string8_lit("Unknown error");
  case RENDERER_ERROR_BACKEND_NOT_SUPPORTED:
    return string8_lit("Backend not supported");
  case RENDERER_ERROR_RESOURCE_CREATION_FAILED:
    return string8_lit("Resource creation failed");
  case RENDERER_ERROR_INVALID_HANDLE:
    return string8_lit("Invalid handle");
  case RENDERER_ERROR_INVALID_PARAMETER:
    return string8_lit("Invalid parameter");
  case RENDERER_ERROR_SHADER_COMPILATION_FAILED:
    return string8_lit("Shader compilation failed");
  case RENDERER_ERROR_OUT_OF_MEMORY:
    return string8_lit("Out of memory");
  case RENDERER_ERROR_COMMAND_RECORDING_FAILED:
    return string8_lit("Command recording failed");
  case RENDERER_ERROR_FRAME_PREPARATION_FAILED:
    return string8_lit("Frame preparation failed");
  case RENDERER_ERROR_PRESENTATION_FAILED:
    return string8_lit("Presentation failed");
  case RENDERER_ERROR_FRAME_IN_PROGRESS:
    return string8_lit("Frame in progress");
  default:
    return string8_lit("Unknown error");
  }
}

Window *renderer_get_window(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->window;
}

RendererBackendType renderer_get_backend_type(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->backend_type;
}

void renderer_get_device_information(RendererFrontendHandle renderer,
                                     DeviceInformation *device_information,
                                     Arena *temp_arena) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(device_information != NULL, "Device information is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  renderer->backend.get_device_information(renderer->backend_state,
                                           device_information, temp_arena);
}

bool32_t renderer_is_frame_active(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->frame_active;
}

RendererError renderer_wait_idle(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->backend.wait_idle(renderer->backend_state);
}

BufferHandle renderer_create_buffer(RendererFrontendHandle renderer,
                                    const BufferDescription *description,
                                    const void *initial_data,
                                    RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating buffer");

  BackendResourceHandle handle = renderer->backend.buffer_create(
      renderer->backend_state, description, initial_data);
  if (handle.ptr == NULL) {
    *out_error = RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = RENDERER_ERROR_NONE;
  return (BufferHandle)handle.ptr;
}

BufferHandle renderer_create_vertex_buffer(RendererFrontendHandle renderer,
                                           uint64_t size,
                                           const void *initial_data,
                                           RendererError *out_error) {
  BufferDescription desc = {.size = size,
                            .usage = BUFFER_USAGE_VERTEX_BUFFER |
                                     BUFFER_USAGE_TRANSFER_DST,
                            .memory_properties = MEMORY_PROPERTY_DEVICE_LOCAL};

  return renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

BufferHandle renderer_create_index_buffer(RendererFrontendHandle renderer,
                                          uint64_t size, IndexType type,
                                          const void *initial_data,
                                          RendererError *out_error) {
  // Note: type parameter is for documentation/validation, the actual buffer
  // doesn't need to know the index type (that's specified at bind time)
  (void)type; // Suppress unused parameter warning

  BufferDescription desc = {.size = size,
                            .usage = BUFFER_USAGE_INDEX_BUFFER |
                                     BUFFER_USAGE_TRANSFER_DST,
                            .memory_properties = MEMORY_PROPERTY_DEVICE_LOCAL};

  return renderer_create_buffer(renderer, &desc, initial_data, out_error);
}

void renderer_destroy_buffer(RendererFrontendHandle renderer,
                             BufferHandle buffer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  log_debug("Destroying buffer");

  BackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.buffer_destroy(renderer->backend_state, handle);
}

ShaderHandle
renderer_create_shader_from_source(RendererFrontendHandle renderer,
                                   const ShaderModuleDescription *description,
                                   RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating shader from source");

  BackendResourceHandle handle = renderer->backend.shader_create_from_source(
      renderer->backend_state, description);
  if (handle.ptr == NULL) {
    *out_error = RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = RENDERER_ERROR_NONE;
  return (ShaderHandle)handle.ptr;
}

void renderer_destroy_shader(RendererFrontendHandle renderer,
                             ShaderHandle shader) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(shader != NULL, "Shader is NULL");

  log_debug("Destroying shader");

  BackendResourceHandle handle = {.ptr = (void *)shader};
  renderer->backend.shader_destroy(renderer->backend_state, handle);
}

PipelineHandle
renderer_create_pipeline(RendererFrontendHandle renderer,
                         const GraphicsPipelineDescription *description,
                         RendererError *out_error) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(description != NULL, "Description is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  log_debug("Creating pipeline");

  BackendResourceHandle handle =
      renderer->backend.pipeline_create(renderer->backend_state, description);
  if (handle.ptr == NULL) {
    *out_error = RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return NULL;
  }

  *out_error = RENDERER_ERROR_NONE;
  return (PipelineHandle)handle.ptr;
}

void renderer_destroy_pipeline(RendererFrontendHandle renderer,
                               PipelineHandle pipeline) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");

  log_debug("Destroying pipeline");

  // Wait for GPU to be idle to ensure no command buffers are still using this
  // pipeline
  renderer->backend.wait_idle(renderer->backend_state);

  BackendResourceHandle handle = {.ptr = (void *)pipeline};
  renderer->backend.pipeline_destroy(renderer->backend_state, handle);
}

RendererError renderer_update_buffer(RendererFrontendHandle renderer,
                                     BufferHandle buffer, uint64_t offset,
                                     uint64_t size, const void *data) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");

  log_debug("Updating buffer");

  BackendResourceHandle handle = {.ptr = (void *)buffer};
  return renderer->backend.buffer_update(renderer->backend_state, handle,
                                         offset, size, data);
}

RendererError renderer_begin_frame(RendererFrontendHandle renderer,
                                   float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Beginning frame");

  if (renderer->frame_active) {
    return RENDERER_ERROR_FRAME_IN_PROGRESS;
  }

  RendererError result =
      renderer->backend.begin_frame(renderer->backend_state, delta_time);
  if (result == RENDERER_ERROR_NONE) {
    renderer->frame_active = true;
  }

  return result;
}

void renderer_resize(RendererFrontendHandle renderer, uint32_t width,
                     uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");

  log_debug("Resizing renderer to %d %d", width, height);

  renderer->backend.on_resize(renderer->backend_state, width, height);
}

void renderer_bind_graphics_pipeline(RendererFrontendHandle renderer,
                                     PipelineHandle pipeline) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(pipeline != NULL, "Pipeline is NULL");
  assert_log(renderer->frame_active, "Bind pipeline called outside of frame");

  // log_debug("Binding pipeline");

  BackendResourceHandle handle = {.ptr = (void *)pipeline};
  renderer->backend.bind_pipeline(renderer->backend_state, handle);
}

void renderer_bind_vertex_buffers(RendererFrontendHandle renderer,
                                  uint32_t first_binding,
                                  uint32_t binding_count,
                                  const VertexBufferBinding *bindings) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(bindings != NULL, "Bindings is NULL");
  assert_log(binding_count > 0, "Binding count must be > 0");
  assert_log(renderer->frame_active,
             "Bind vertex buffers called outside of frame");

  // log_debug("Binding %d vertex buffers starting at binding %d",
  // binding_count, first_binding);

  renderer->backend.bind_vertex_buffers(renderer->backend_state, first_binding,
                                        binding_count, bindings);
}

void renderer_bind_vertex_buffer(RendererFrontendHandle renderer,
                                 const VertexBufferBinding *binding) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(binding != NULL, "Binding is NULL");
  assert_log(binding->buffer != NULL, "Buffer is NULL");
  assert_log(renderer->frame_active,
             "Bind vertex buffer called outside of frame");

  // log_debug("Binding vertex buffer to binding %d with offset %llu",
  //           binding->binding, binding->offset);

  renderer->backend.bind_vertex_buffer(renderer->backend_state, binding);
}

void renderer_bind_index_buffer(RendererFrontendHandle renderer,
                                const IndexBufferBinding *binding) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(binding != NULL, "Binding is NULL");
  assert_log(binding->buffer != NULL, "Buffer is NULL");
  assert_log(renderer->frame_active,
             "Bind index buffer called outside of frame");

  // log_debug("Binding index buffer with type %d and offset %llu",
  //           binding->type, binding->offset);

  renderer->backend.bind_index_buffer(renderer->backend_state, binding);
}

void renderer_draw(RendererFrontendHandle renderer, uint32_t vertex_count,
                   uint32_t instance_count, uint32_t first_vertex,
                   uint32_t first_instance) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "Draw called outside of frame");

  // log_debug("Drawing %d vertices with %d instances starting at %d with %d",
  //           vertex_count, instance_count, first_vertex, first_instance);

  renderer->backend.draw(renderer->backend_state, vertex_count, instance_count,
                         first_vertex, first_instance);
}

void renderer_draw_indexed(RendererFrontendHandle renderer,
                           uint32_t index_count, uint32_t instance_count,
                           uint32_t first_index, int32_t vertex_offset,
                           uint32_t first_instance) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(renderer->frame_active, "Draw indexed called outside of frame");

  // log_debug("Drawing %d indices with %d instances starting at %d with vertex
  // offset %d",
  //           index_count, instance_count, first_index, vertex_offset);

  renderer->backend.draw_indexed(renderer->backend_state, index_count,
                                 instance_count, first_index, vertex_offset,
                                 first_instance);
}

RendererError renderer_end_frame(RendererFrontendHandle renderer,
                                 float64_t delta_time) {
  assert_log(renderer != NULL, "Renderer is NULL");

  // log_debug("Ending frame");

  if (!renderer->frame_active) {
    return RENDERER_ERROR_INVALID_PARAMETER;
  }

  RendererError result =
      renderer->backend.end_frame(renderer->backend_state, delta_time);
  renderer->frame_active = false;

  return result;
}
