#pragma once

#include "vulkan_instance.h"
#include "vulkan_types.h"

RendererBackendInterface renderer_vulkan_get_interface();

bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    RendererBackendType type, Window *window,
                                    uint32_t initial_width,
                                    uint32_t initial_height);

void renderer_vulkan_shutdown(void *backend_state);

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height);

RendererError renderer_vulkan_begin_frame(void *backend_state,
                                          float64_t delta_time);

RendererError renderer_vulkan_end_frame(void *backend_state,
                                        float64_t delta_time);

BackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const BufferDescription *desc,
                              const void *initial_data);

RendererError renderer_vulkan_update_buffer(void *backend_state,
                                            BackendResourceHandle handle,
                                            uint64_t offset, uint64_t size,
                                            const void *data);

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    BackendResourceHandle handle);

BackendResourceHandle
renderer_vulkan_create_shader(void *backend_state,
                              const ShaderModuleDescription *desc);

void renderer_vulkan_destroy_shader(void *backend_state,
                                    BackendResourceHandle handle);

BackendResourceHandle
renderer_vulkan_create_pipeline(void *backend_state,
                                const GraphicsPipelineDescription *desc);

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      BackendResourceHandle handle);

void renderer_vulkan_set_viewport(void *backend_state, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height,
                                  float32_t min_depth, float32_t max_depth);

void renderer_vulkan_set_scissor(void *backend_state, int32_t x, int32_t y,
                                 uint32_t width, uint32_t height);

void renderer_vulkan_clear_color(void *backend_state, float r, float g, float b,
                                 float a);

void renderer_vulkan_bind_pipeline(void *backend_state,
                                   BackendResourceHandle pipeline_handle);

void renderer_vulkan_bind_vertex_buffer(void *backend_state,
                                        BackendResourceHandle buffer_handle,
                                        uint32_t binding_index,
                                        uint64_t offset);

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance);
