#pragma once

#include "vulkan_types.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 VkRenderPass *out_render_pass);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               VkRenderPass render_pass);