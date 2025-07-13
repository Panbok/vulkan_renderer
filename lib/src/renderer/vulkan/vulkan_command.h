#pragma once

#include "defines.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool8_t vulkan_command_buffer_allocate(VulkanBackendState *state,
                                       VulkanCommandBuffer *out_command_buffer);

void vulkan_command_buffer_free(VulkanBackendState *state,
                                VulkanCommandBuffer *command_buffer);

bool8_t vulkan_command_buffer_begin(VulkanCommandBuffer *command_buffer);

bool8_t vulkan_command_buffer_end(VulkanCommandBuffer *command_buffer);

void vulkan_command_buffer_update_submitted(
    VulkanCommandBuffer *command_buffer);

void vulkan_command_buffer_reset(VulkanCommandBuffer *command_buffer);

bool8_t vulkan_command_buffer_allocate_and_begin_single_use(
    VulkanBackendState *state, VulkanCommandBuffer *command_buffer);

bool8_t vulkan_command_buffer_end_single_use(
    VulkanBackendState *state, VkCommandPool pool,
    VulkanCommandBuffer *command_buffer, VkQueue queue);