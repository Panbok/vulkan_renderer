#include "vulkan_backend.h"

RendererBackendInterface renderer_vulkan_get_interface() {
  return (RendererBackendInterface){
      .initialize = renderer_vulkan_initialize,
      .shutdown = renderer_vulkan_shutdown,
      .on_resize = renderer_vulkan_on_resize,
      .buffer_create = renderer_vulkan_create_buffer,
      .buffer_destroy = renderer_vulkan_destroy_buffer,
      .buffer_update = renderer_vulkan_update_buffer,
      .shader_create_from_source = renderer_vulkan_create_shader,
      .shader_destroy = renderer_vulkan_destroy_shader,
      .pipeline_create = renderer_vulkan_create_pipeline,
      .pipeline_destroy = renderer_vulkan_destroy_pipeline,
      .begin_frame = renderer_vulkan_begin_frame,
      .end_frame = renderer_vulkan_end_frame,
      .set_viewport = renderer_vulkan_set_viewport,
      .set_scissor = renderer_vulkan_set_scissor,
      .clear_color = renderer_vulkan_clear_color,
      .bind_pipeline = renderer_vulkan_bind_pipeline,
      .bind_vertex_buffer = renderer_vulkan_bind_vertex_buffer,
      .draw = renderer_vulkan_draw,
  };
}

bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    RendererBackendType type, Window *window,
                                    uint32_t initial_width,
                                    uint32_t initial_height) {
  return true;
}

void renderer_vulkan_shutdown(void *backend_state) { return; }

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height) {
  return;
}

RendererError renderer_vulkan_begin_frame(void *backend_state,
                                          float64_t delta_time) {
  return RENDERER_ERROR_NONE;
}

RendererError renderer_vulkan_end_frame(void *backend_state,
                                        float64_t delta_time) {
  return RENDERER_ERROR_NONE;
}

BackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const BufferDescription *desc,
                              const void *initial_data) {
  return (BackendResourceHandle){.ptr = NULL};
}

RendererError renderer_vulkan_update_buffer(void *backend_state,
                                            BackendResourceHandle handle,
                                            uint64_t offset, uint64_t size,
                                            const void *data) {
  return RENDERER_ERROR_NONE;
}

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    BackendResourceHandle handle) {
  return;
}

BackendResourceHandle
renderer_vulkan_create_shader(void *backend_state,
                              const ShaderModuleDescription *desc) {
  return (BackendResourceHandle){.ptr = NULL};
}

void renderer_vulkan_destroy_shader(void *backend_state,
                                    BackendResourceHandle handle) {
  return;
}

BackendResourceHandle
renderer_vulkan_create_pipeline(void *backend_state,
                                const GraphicsPipelineDescription *desc) {
  return (BackendResourceHandle){.ptr = NULL};
}

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      BackendResourceHandle handle) {
  return;
}

void renderer_vulkan_set_viewport(void *backend_state, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height,
                                  float32_t min_depth, float32_t max_depth) {
  return;
}

void renderer_vulkan_set_scissor(void *backend_state, int32_t x, int32_t y,
                                 uint32_t width, uint32_t height) {
  return;
}

void renderer_vulkan_clear_color(void *backend_state, float r, float g, float b,
                                 float a) {
  return;
}

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   BackendResourceHandle pipeline_handle) {
  return;
}

void renderer_vulkan_bind_vertex_buffer(void *backend_state,
                                        BackendResourceHandle buffer_handle,
                                        uint32_t binding_index,
                                        uint64_t offset) {
  return;
}

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance) {
  return;
}
