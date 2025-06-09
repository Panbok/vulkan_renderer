#pragma once

#include "defines.h"
#include "vulkan_types.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VulkanRenderPass *out_render_pass);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VulkanRenderPass *render_pass);

bool8_t vulkan_renderpass_begin(VulkanCommandBuffer *command_buffer,
                                VulkanRenderPass *render_pass,
                                VkFramebuffer framebuffer);

bool8_t vulkan_renderpass_end(VulkanCommandBuffer *command_buffer);