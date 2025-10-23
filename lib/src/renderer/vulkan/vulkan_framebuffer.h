#pragma once

#include "renderer/vulkan/vulkan_types.h"

bool8_t vulkan_framebuffer_create(VulkanBackendState *state,
                                  VulkanRenderPass *renderpass, uint32_t width,
                                  uint32_t height,
                                  Array_VkImageView *attachments,
                                  VulkanFramebuffer *out_framebuffer);

void vulkan_framebuffer_destroy(VulkanBackendState *state,
                                VulkanFramebuffer *framebuffer);

bool32_t vulkan_framebuffer_regenerate_for_domain(
    VulkanBackendState *state, VulkanSwapchain *swapchain,
    VulkanRenderPass *renderpass, VkrPipelineDomain domain,
    Array_VulkanFramebuffer *framebuffers);