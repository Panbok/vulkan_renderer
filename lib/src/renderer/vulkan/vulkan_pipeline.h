#pragma once

#include "defines.h"
#include "vulkan_shaders.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool8_t vulkan_graphics_graphics_pipeline_create(
    VulkanBackendState *state, const GraphicsPipelineDescription *desc,
    struct s_GraphicsPipeline *out_pipeline);

RendererError vulkan_graphics_pipeline_update_state(
    VulkanBackendState *state, struct s_GraphicsPipeline *pipeline,
    const GlobalUniformObject *uniform, const ShaderStateObject *data);

void vulkan_graphics_pipeline_bind(VulkanCommandBuffer *command_buffer,
                                   VkPipelineBindPoint bind_point,
                                   struct s_GraphicsPipeline *pipeline);

void vulkan_graphics_pipeline_destroy(VulkanBackendState *state,
                                      struct s_GraphicsPipeline *pipeline);