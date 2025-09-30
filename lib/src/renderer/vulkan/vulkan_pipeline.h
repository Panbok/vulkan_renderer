#pragma once

#include "defines.h"
#include "renderer/vkr_renderer.h"
#include "vulkan_types.h"

bool8_t vulkan_graphics_graphics_pipeline_create(
    VulkanBackendState *state, const VkrGraphicsPipelineDescription *desc,
    struct s_GraphicsPipeline *out_pipeline);

VkrRendererError vulkan_graphics_pipeline_update_state(
    VulkanBackendState *state, struct s_GraphicsPipeline *pipeline,
    const VkrGlobalUniformObject *uniform, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material);

void vulkan_graphics_pipeline_bind(VulkanCommandBuffer *command_buffer,
                                   VkPipelineBindPoint bind_point,
                                   struct s_GraphicsPipeline *pipeline);

void vulkan_graphics_pipeline_destroy(VulkanBackendState *state,
                                      struct s_GraphicsPipeline *pipeline);