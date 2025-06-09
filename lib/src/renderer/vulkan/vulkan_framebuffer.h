#pragma once

#include "renderer/vulkan/vulkan_types.h"

bool8_t vulkan_framebuffer_create(VulkanBackendState *state,
                                  VkImageView *image_view,
                                  VkFramebuffer *out_framebuffer);

void vulkan_framebuffer_destroy(VulkanBackendState *state,
                                VkFramebuffer *framebuffer);