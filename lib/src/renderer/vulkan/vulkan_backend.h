#pragma once

#include "vulkan_buffer.h"
#include "vulkan_command.h"
#include "vulkan_device.h"
#include "vulkan_fence.h"
#include "vulkan_framebuffer.h"
#include "vulkan_instance.h"
#include "vulkan_pipeline.h"
#include "vulkan_renderpass.h"
#include "vulkan_shaders.h"
#include "vulkan_swapchain.h"
#include "vulkan_types.h"

#ifndef NDEBUG
#include "vulkan_debug.h"
#endif

// Internal function for swapchain recreation (used by swapchain module)
bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state);

RendererBackendInterface renderer_vulkan_get_interface();

bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    RendererBackendType type, Window *window,
                                    uint32_t initial_width,
                                    uint32_t initial_height,
                                    DeviceRequirements *device_requirements);

void renderer_vulkan_shutdown(void *backend_state);

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height);

void renderer_vulkan_get_device_information(
    void *backend_state, DeviceInformation *device_information,
    Arena *temp_arena);

RendererError renderer_vulkan_wait_idle(void *backend_state);

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

RendererError renderer_vulkan_upload_buffer(void *backend_state,
                                            BackendResourceHandle handle,
                                            uint64_t offset, uint64_t size,
                                            const void *data);

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    BackendResourceHandle handle);

BackendResourceHandle renderer_vulkan_create_graphics_pipeline(
    void *backend_state, const GraphicsPipelineDescription *desc);

RendererError renderer_vulkan_update_pipeline_state(
    void *backend_state, BackendResourceHandle pipeline_handle,
    const GlobalUniformObject *uniform, const ShaderStateObject *data);

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      BackendResourceHandle handle);

void renderer_vulkan_bind_buffer(void *backend_state,
                                 BackendResourceHandle buffer_handle,
                                 uint64_t offset);

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance);

void renderer_vulkan_draw_indexed(void *backend_state, uint32_t index_count,
                                  uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset,
                                  uint32_t first_instance);
