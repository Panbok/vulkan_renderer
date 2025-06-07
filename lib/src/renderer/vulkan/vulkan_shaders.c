#include "vulkan_shaders.h"
#include "containers/bitset.h"
#include "containers/str.h"
#include "core/logger.h"
#include "defines.h"
#include "renderer/renderer.h"

static VkShaderStageFlagBits vulkan_shader_stage_to_vk(ShaderStageFlags stage) {
  if (bitset8_is_set(&stage, SHADER_STAGE_VERTEX_BIT)) {
    return VK_SHADER_STAGE_VERTEX_BIT;
  } else if (bitset8_is_set(&stage, SHADER_STAGE_FRAGMENT_BIT)) {
    return VK_SHADER_STAGE_FRAGMENT_BIT;
  } else if (bitset8_is_set(&stage, SHADER_STAGE_COMPUTE_BIT)) {
    return VK_SHADER_STAGE_COMPUTE_BIT;
  } else if (bitset8_is_set(&stage, SHADER_STAGE_GEOMETRY_BIT)) {
    return VK_SHADER_STAGE_GEOMETRY_BIT;
  } else if (bitset8_is_set(&stage, SHADER_STAGE_TESSELLATION_CONTROL_BIT)) {
    return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  } else if (bitset8_is_set(&stage, SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) {
    return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }

  return VK_SHADER_STAGE_ALL_GRAPHICS;
}

bool8_t vulkan_shader_module_create(VulkanBackendState *state,
                                    const ShaderModuleDescription *desc,
                                    struct s_ShaderModule *out_shader) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Shader module description is NULL");
  assert_log(out_shader != NULL, "Output shader module is NULL");

  if (desc->size == 0 || desc->code == NULL) {
    log_error("Invalid shader code: size is 0 or code is NULL");
    return false;
  }

  if (desc->size % 4 != 0) {
    log_error("Invalid SPIR-V: size (%lu) is not a multiple of 4 bytes",
              desc->size);
    return false;
  }

  if ((uintptr_t)desc->code % 4 != 0) {
    log_error("SPIR-V code is not 4-byte aligned. Consider using aligned "
              "allocation.");
    return false;
  }

  VkShaderModuleCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = desc->size,
      .pCode = (const uint32_t *)desc->code,
  };

  VkShaderModule shader_module;
  VkResult result =
      vkCreateShaderModule(state->device, &create_info, NULL, &shader_module);
  if (result != VK_SUCCESS) {
    log_error("Failed to create shader module: %s", string_VkResult(result));
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = vulkan_shader_stage_to_vk(desc->stage),
      .module = shader_module,
      .pName = string8_cstr(&desc->entry_point),
  };

  out_shader->module = shader_module;
  out_shader->stage = desc->stage;
  out_shader->size = desc->size;
  out_shader->entry_point = &desc->entry_point;
  out_shader->code = desc->code;
  out_shader->stage_info = stage_info;

  log_debug("Shader module created: %p", out_shader);

  return true;
}

void vulkan_shader_module_destroy(VulkanBackendState *state,
                                  struct s_ShaderModule *shader) {
  if (shader && shader->module != VK_NULL_HANDLE) {
    log_debug("Destroying shader module: %p", shader);

    vkDestroyShaderModule(state->device, shader->module, NULL);
    shader->module = VK_NULL_HANDLE;
  };
}