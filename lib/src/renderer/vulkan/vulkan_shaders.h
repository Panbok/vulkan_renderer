#pragma once

#include "defines.h"
#include "vulkan_types.h"

bool8_t vulkan_shader_create(VulkanBackendState *state,
                             const ShaderModuleDescription *desc,
                             struct s_ShaderModule *out_shader);

void vulkan_shader_destroy(VulkanBackendState *state,
                           struct s_ShaderModule *shader);