#include "renderer/renderer_frontend.h"

RendererFrontendHandle renderer_create(Arena *arena,
                                       RendererBackendType backend_type,
                                       Window *window,
                                       RendererError *out_error) {
  assert_log(window != NULL, "Window is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

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
                                    window, width, height)) {
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

bool32_t renderer_is_frame_active(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  return renderer->frame_active;
}

void renderer_wait_idle(RendererFrontendHandle renderer) {
  assert_log(renderer != NULL, "Renderer is NULL");
  renderer->backend.wait_idle(renderer->backend_state);
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

void renderer_bind_vertex_buffer(RendererFrontendHandle renderer,
                                 BufferHandle buffer, uint32_t binding_index,
                                 uint64_t offset) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(buffer != NULL, "Buffer is NULL");
  assert_log(renderer->frame_active,
             "Bind vertex buffer called outside of frame");

  // log_debug("Binding vertex buffer to %d with offset %d", binding_index,
  //           offset);

  BackendResourceHandle handle = {.ptr = (void *)buffer};
  renderer->backend.bind_vertex_buffer(renderer->backend_state, handle,
                                       binding_index, offset);
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
