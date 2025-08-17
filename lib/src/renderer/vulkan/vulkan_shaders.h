#pragma once

#include "defines.h"
#include "vulkan_buffer.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool8_t vulkan_shader_object_create(VulkanBackendState *state,
                                    const ShaderObjectDescription *desc,
                                    VulkanShaderObject *out_shader_object);

bool8_t vulkan_shader_update_global_state(VulkanBackendState *state,
                                          VulkanShaderObject *shader_object,
                                          VkPipelineLayout pipeline_layout,
                                          const GlobalUniformObject *uniform);

bool8_t vulkan_shader_update_state(VulkanBackendState *state,
                                   VulkanShaderObject *shader_object,
                                   VkPipelineLayout pipeline_layout,
                                   const ShaderStateObject *data);

bool8_t vulkan_shader_acquire_resource(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t *out_object_id);

bool8_t vulkan_shader_release_resource(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t object_id);

void vulkan_shader_object_destroy(VulkanBackendState *state,
                                  VulkanShaderObject *out_shader_object);

bool8_t vulkan_shader_module_create(VulkanBackendState *state,
                                    ShaderStageFlags stage, const uint64_t size,
                                    const uint8_t *code,
                                    const String8 entry_point,
                                    VkShaderModule *out_shader,
                                    VkPipelineShaderStageCreateInfo *out_stage);

void vulkan_shader_module_destroy(VulkanBackendState *state,
                                  VkShaderModule shader);