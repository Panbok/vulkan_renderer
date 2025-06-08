#pragma once

#include "vulkan_types.h"

bool8_t vulkan_renderpass_create(VulkanBackendState *state,
                                 struct s_GraphicsPipeline *pipeline);

void vulkan_renderpass_destroy(VulkanBackendState *state,
                               struct s_GraphicsPipeline *pipeline);