#pragma once

#include "vulkan_types.h"

// Internal function for swapchain recreation (used by swapchain module)
bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state);

VkrRendererBackendInterface renderer_vulkan_get_interface();

bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    VkrRendererBackendType type,
                                    VkrWindow *window, uint32_t initial_width,
                                    uint32_t initial_height,
                                    VkrDeviceRequirements *device_requirements);

void renderer_vulkan_shutdown(void *backend_state);

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height);

void renderer_vulkan_get_device_information(
    void *backend_state, VkrDeviceInformation *device_information,
    Arena *temp_arena);

VkrRendererError renderer_vulkan_wait_idle(void *backend_state);

VkrRendererError renderer_vulkan_begin_frame(void *backend_state,
                                             float64_t delta_time);

VkrRendererError renderer_vulkan_end_frame(void *backend_state,
                                           float64_t delta_time);

VkrBackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const VkrBufferDescription *desc,
                              const void *initial_data);

VkrRendererError renderer_vulkan_update_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data);

VkrRendererError renderer_vulkan_upload_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data);

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    VkrBackendResourceHandle handle);

VkrBackendResourceHandle
renderer_vulkan_create_texture(void *backend_state,
                               const VkrTextureDescription *desc,
                               const void *initial_data);
VkrRendererError renderer_vulkan_update_texture(
    void *backend_state, VkrBackendResourceHandle handle,
    const VkrTextureDescription *desc);
VkrRendererError renderer_vulkan_write_texture(
    void *backend_state, VkrBackendResourceHandle handle,
    const VkrTextureWriteRegion *region, const void *data, uint64_t size);
VkrRendererError renderer_vulkan_resize_texture(void *backend_state,
                                                VkrBackendResourceHandle handle,
                                                uint32_t new_width,
                                                uint32_t new_height,
                                                bool8_t preserve_contents);

void renderer_vulkan_destroy_texture(void *backend_state,
                                     VkrBackendResourceHandle handle);

VkrBackendResourceHandle renderer_vulkan_create_graphics_pipeline(
    void *backend_state, const VkrGraphicsPipelineDescription *desc);

VkrRendererError renderer_vulkan_update_pipeline_state(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    const void *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material);

VkrRendererError renderer_vulkan_instance_state_acquire(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrRendererInstanceStateHandle *out_handle);

VkrRendererError
renderer_vulkan_instance_state_release(void *backend_state,
                                       VkrBackendResourceHandle pipeline_handle,
                                       VkrRendererInstanceStateHandle handle);

void renderer_vulkan_destroy_pipeline(void *backend_state,
                                      VkrBackendResourceHandle handle);

void renderer_vulkan_bind_buffer(void *backend_state,
                                 VkrBackendResourceHandle buffer_handle,
                                 uint64_t offset);

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance);

void renderer_vulkan_draw_indexed(void *backend_state, uint32_t index_count,
                                  uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset,
                                  uint32_t first_instance);

VkrRendererError renderer_vulkan_begin_render_pass(void *backend_state,
                                                   VkrPipelineDomain domain);

VkrRendererError renderer_vulkan_end_render_pass(void *backend_state);

// Telemetry
uint64_t
renderer_vulkan_get_and_reset_descriptor_writes_avoided(void *backend_state);
