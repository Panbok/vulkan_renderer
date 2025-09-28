#pragma once

#include "defines.h"
#include "vulkan_types.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VulkanRenderPass *out_render_pass,
                                 Vec2 position, Vec4 color, float32_t w,
                                 float32_t h, float32_t depth,
                                 uint32_t stencil);

bool8_t vulkan_renderpass_create_for_domain(VulkanBackendState *state,
                                            VkrPipelineDomain domain,
                                            VulkanRenderPass *out_render_pass);

bool8_t vulkan_renderpass_create_world(VulkanBackendState *state,
                                       VulkanRenderPass *out_render_pass);

bool8_t vulkan_renderpass_create_ui(VulkanBackendState *state,
                                    VulkanRenderPass *out_render_pass);

bool8_t vulkan_renderpass_create_shadow(VulkanBackendState *state,
                                        VulkanRenderPass *out_render_pass);

bool8_t vulkan_renderpass_create_post(VulkanBackendState *state,
                                      VulkanRenderPass *out_render_pass);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass);

bool8_t vulkan_renderpass_begin(VulkanCommandBuffer *command_buffer,
                                VulkanRenderPass *render_pass,
                                VkFramebuffer framebuffer);

bool8_t vulkan_renderpass_end(VulkanCommandBuffer *command_buffer);