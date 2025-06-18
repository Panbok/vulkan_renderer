#pragma once

#include "defines.h"
#include "vulkan_types.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VulkanRenderPass *out_render_pass, float32_t x,
                                 float32_t y, float32_t w, float32_t h,
                                 float32_t r, float32_t g, float32_t b,
                                 float32_t a, float32_t depth,
                                 uint32_t stencil);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass);

bool8_t vulkan_renderpass_begin(VulkanCommandBuffer *command_buffer,
                                VulkanRenderPass *render_pass,
                                VkFramebuffer framebuffer);

bool8_t vulkan_renderpass_end(VulkanCommandBuffer *command_buffer);