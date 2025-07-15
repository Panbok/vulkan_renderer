#pragma once

#include "defines.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool8_t vulkan_graphics_graphics_pipeline_create(
    VulkanBackendState *state, const GraphicsPipelineDescription *desc,
    struct s_GraphicsPipeline *out_pipeline);

void vulkan_graphics_pipeline_destroy(VulkanBackendState *state,
                                      struct s_GraphicsPipeline *pipeline);