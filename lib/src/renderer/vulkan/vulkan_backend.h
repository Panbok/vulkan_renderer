#pragma once

#include "vulkan_types.h"

// Internal function for swapchain recreation (used by swapchain module)
bool32_t vulkan_backend_recreate_swapchain(VulkanBackendState *state);
VkResult vulkan_backend_queue_submit_locked(VulkanBackendState *state,
                                            VkQueue queue,
                                            uint32_t submit_count,
                                            const VkSubmitInfo *submit_infos,
                                            VkFence fence);
VkResult vulkan_backend_queue_present_locked(
    VulkanBackendState *state, VkQueue queue,
    const VkPresentInfoKHR *present_info);
VkResult vulkan_backend_queue_wait_idle_locked(VulkanBackendState *state,
                                               VkQueue queue);

VkrRendererBackendInterface renderer_vulkan_get_interface();

bool32_t renderer_vulkan_initialize(void **out_backend_state,
                                    VkrRendererBackendType type,
                                    VkrWindow *window, uint32_t initial_width,
                                    uint32_t initial_height,
                                    VkrDeviceRequirements *device_requirements,
                                    const VkrRendererBackendConfig *config);

void renderer_vulkan_shutdown(void *backend_state);

void renderer_vulkan_on_resize(void *backend_state, uint32_t new_width,
                               uint32_t new_height);

void renderer_vulkan_get_device_information(
    void *backend_state, VkrDeviceInformation *device_information,
    Arena *temp_arena);

VkrRendererError renderer_vulkan_wait_idle(void *backend_state);
void renderer_vulkan_set_job_system(void *backend_state,
                                    VkrJobSystem *job_system);

VkrRendererError renderer_vulkan_begin_frame(void *backend_state,
                                             float64_t delta_time);

VkrRendererError renderer_vulkan_end_frame(void *backend_state,
                                           float64_t delta_time);

void renderer_vulkan_renderpass_destroy(void *backend_state,
                                        VkrRenderPassHandle pass);
VkrRenderPassHandle renderer_vulkan_renderpass_get(void *backend_state,
                                                   const char *name);

void renderer_vulkan_render_target_destroy(void *backend_state,
                                           VkrRenderTargetHandle target);

VkrRendererError
renderer_vulkan_begin_render_pass(void *backend_state, VkrRenderPassHandle pass,
                                  VkrRenderTargetHandle target);
VkrRendererError renderer_vulkan_end_render_pass(void *backend_state);

VkrBackendResourceHandle
renderer_vulkan_create_buffer(void *backend_state,
                              const VkrBufferDescription *desc,
                              const void *initial_data);
uint32_t renderer_vulkan_create_buffer_batch(
    void *backend_state, const VkrBufferBatchCreateRequest *requests,
    uint32_t count, VkrBackendResourceHandle *out_handles,
    VkrRendererError *out_errors);

VkrRendererError renderer_vulkan_update_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data);

VkrRendererError renderer_vulkan_upload_buffer(void *backend_state,
                                               VkrBackendResourceHandle handle,
                                               uint64_t offset, uint64_t size,
                                               const void *data);

void *renderer_vulkan_buffer_get_mapped_ptr(void *backend_state,
                                            VkrBackendResourceHandle handle);

VkrRendererError renderer_vulkan_flush_buffer(void *backend_state,
                                              VkrBackendResourceHandle handle,
                                              uint64_t offset, uint64_t size);

void renderer_vulkan_destroy_buffer(void *backend_state,
                                    VkrBackendResourceHandle handle);

VkrBackendResourceHandle
renderer_vulkan_create_texture(void *backend_state,
                               const VkrTextureDescription *desc,
                               const void *initial_data);
VkrBackendResourceHandle renderer_vulkan_create_texture_with_payload(
    void *backend_state, const VkrTextureDescription *desc,
    const VkrTextureUploadPayload *payload);
uint32_t renderer_vulkan_create_texture_with_payload_batch(
    void *backend_state, const VkrTextureBatchCreateRequest *requests,
    uint32_t count, VkrBackendResourceHandle *out_handles,
    VkrRendererError *out_errors);
VkrBackendResourceHandle renderer_vulkan_create_render_target_texture(
    void *backend_state, const VkrRenderTargetTextureDesc *desc);
VkrBackendResourceHandle
renderer_vulkan_create_depth_attachment(void *backend_state, uint32_t width,
                                        uint32_t height);
VkrBackendResourceHandle
renderer_vulkan_create_sampled_depth_attachment(void *backend_state,
                                                uint32_t width,
                                                uint32_t height);
VkrBackendResourceHandle
renderer_vulkan_create_sampled_depth_attachment_array(void *backend_state,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t layers);
VkrRendererError renderer_vulkan_transition_texture_layout(
    void *backend_state, VkrBackendResourceHandle handle,
    VkrTextureLayout old_layout, VkrTextureLayout new_layout);
VkrRendererError
renderer_vulkan_update_texture(void *backend_state,
                               VkrBackendResourceHandle handle,
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
bool8_t renderer_vulkan_pipeline_get_shader_runtime_layout(
    void *backend_state, VkrBackendResourceHandle pipeline_handle,
    VkrShaderRuntimeLayout *out_layout);

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

void renderer_vulkan_set_instance_buffer(void *backend_state,
                                         VkrBackendResourceHandle buffer);

void renderer_vulkan_set_viewport(void *backend_state,
                                  const VkrViewport *viewport);

void renderer_vulkan_set_scissor(void *backend_state,
                                 const VkrScissor *scissor);

void renderer_vulkan_set_depth_bias(void *backend_state,
                                    float32_t constant_factor, float32_t clamp,
                                    float32_t slope_factor);

void renderer_vulkan_draw(void *backend_state, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t first_vertex,
                          uint32_t first_instance);

void renderer_vulkan_draw_indexed(void *backend_state, uint32_t index_count,
                                  uint32_t instance_count, uint32_t first_index,
                                  int32_t vertex_offset,
                                  uint32_t first_instance);

void renderer_vulkan_draw_indexed_indirect(
    void *backend_state, VkrBackendResourceHandle indirect_buffer,
    uint64_t offset, uint32_t draw_count, uint32_t stride);

VkrTextureOpaqueHandle
renderer_vulkan_window_attachment_get(void *backend_state,
                                      uint32_t image_index);
VkrTextureOpaqueHandle
renderer_vulkan_depth_attachment_get(void *backend_state);
uint32_t renderer_vulkan_window_attachment_count(void *backend_state);
uint32_t renderer_vulkan_window_attachment_index(void *backend_state);
VkrTextureFormat renderer_vulkan_shadow_depth_format_get(void *backend_state);

// Telemetry
uint64_t
renderer_vulkan_get_and_reset_descriptor_writes_avoided(void *backend_state);

// RenderGraph GPU timing (timestamps)
bool8_t renderer_vulkan_rg_timing_begin_frame(void *backend_state,
                                              uint32_t pass_count);
void renderer_vulkan_rg_timing_begin_pass(void *backend_state,
                                          uint32_t pass_index);
void renderer_vulkan_rg_timing_end_pass(void *backend_state,
                                        uint32_t pass_index);
bool8_t renderer_vulkan_rg_timing_get_results(void *backend_state,
                                              uint32_t *out_pass_count,
                                              const float64_t **out_pass_ms,
                                              const bool8_t **out_pass_valid);

// --- Pixel Readback API ---
VkrRendererError renderer_vulkan_readback_ring_init(void *backend_state);
void renderer_vulkan_readback_ring_shutdown(void *backend_state);
VkrRendererError
renderer_vulkan_request_pixel_readback(void *backend_state,
                                       VkrBackendResourceHandle texture,
                                       uint32_t x, uint32_t y);
VkrRendererError
renderer_vulkan_get_pixel_readback_result(void *backend_state,
                                          VkrPixelReadbackResult *result);
void renderer_vulkan_update_readback_ring(void *backend_state);

// Utility functions

VkrAllocator *renderer_vulkan_get_allocator(void *backend_state);
