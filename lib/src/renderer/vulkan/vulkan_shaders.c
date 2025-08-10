#include "vulkan_shaders.h"

bool8_t vulkan_shader_module_create(
    VulkanBackendState *state, ShaderStageFlags stage, const uint64_t size,
    const uint8_t *code, const String8 entry_point, VkShaderModule *out_shader,
    VkPipelineShaderStageCreateInfo *out_stage) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(out_shader != NULL, "Output shader module is NULL");
  assert_log(out_stage != NULL, "Output shader stage is NULL");

  if (size == 0 || code == NULL) {
    log_error("Invalid shader code: size is 0 or code is NULL");
    return false;
  }

  if (size % 4 != 0) {
    log_error("Invalid SPIR-V: size (%lu) is not a multiple of 4 bytes", size);
    return false;
  }

  if ((uintptr_t)code % 4 != 0) {
    log_error("SPIR-V code is not 4-byte aligned. Consider using aligned "
              "allocation.");
    return false;
  }

  VkShaderModuleCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = size,
      .pCode = (const uint32_t *)code,
  };

  VkShaderModule shader_module;
  VkResult result =
      vkCreateShaderModule(state->device.logical_device, &create_info,
                           state->allocator, &shader_module);
  if (result != VK_SUCCESS) {
    log_error("Failed to create shader module");
    return false;
  }

  VulkanShaderStageFlagResult stage_result = vulkan_shader_stage_to_vk(stage);
  if (!stage_result.is_valid) {
    log_error("Invalid shader stage configuration: exactly one stage must be "
              "set");
    return false;
  }

  VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = stage_result.flag,
      .module = shader_module,
      .pName = string8_cstr(&entry_point),
  };

  *out_shader = shader_module;
  *out_stage = stage_info;

  log_debug("Shader module created: %p", out_shader);

  return true;
}

void vulkan_shader_module_destroy(VulkanBackendState *state,
                                  VkShaderModule shader) {
  if (shader != VK_NULL_HANDLE) {
    log_debug("Destroying shader module: %p", shader);

    vkDestroyShaderModule(state->device.logical_device, shader,
                          state->allocator);
    shader = VK_NULL_HANDLE;
  };
}

bool8_t vulkan_shader_object_create(VulkanBackendState *state,
                                    const ShaderObjectDescription *desc,
                                    VulkanShaderObject *out_shader_object) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Shader object description is NULL");

  if (desc->file_format != SHADER_FILE_FORMAT_SPIR_V) {
    log_error("Only SPIR-V shader file format is supported");
    return false_v;
  }

  if (desc->file_type == SHADER_FILE_TYPE_SINGLE) {
    const FilePath path =
        file_path_create(string8_cstr(&desc->modules[0].path), state->arena,
                         FILE_PATH_TYPE_RELATIVE);
    if (!file_exists(&path)) {
      log_fatal("Cube shader file does not exist: %s",
                string8_cstr(&desc->modules[0].path));
      return false_v;
    }

    // Load shaders
    uint8_t *shader_data = NULL;
    uint64_t shader_size = 0;
    FileError file_error =
        file_load_spirv_shader(&path, state->arena, &shader_data, &shader_size);
    if (file_error != FILE_ERROR_NONE) {
      log_fatal("Failed to load shader: %s", file_get_error_string(file_error));
      return false_v;
    }

    for (uint32_t i = 0; i < SHADER_STAGE_COUNT; i++) {
      vulkan_shader_module_create(state, desc->modules[i].stages, shader_size,
                                  shader_data, desc->modules[i].entry_point,
                                  &out_shader_object->modules[i],
                                  &out_shader_object->stages[i]);
    }
  } else {
    log_error("Multi-file shaders are not supported yet");
    return false_v;
  }

  VkDescriptorSetLayoutBinding global_descriptor_set_layout_binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .pImmutableSamplers = NULL,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
  };

  VkDescriptorSetLayoutBinding global_descriptor_set_layout_bindings[] = {
      global_descriptor_set_layout_binding,
  };

  VkDescriptorSetLayoutCreateInfo global_descriptor_set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = global_descriptor_set_layout_bindings,
  };

  // Global descriptors set layout
  if (vkCreateDescriptorSetLayout(
          state->device.logical_device,
          &global_descriptor_set_layout_create_info, state->allocator,
          &out_shader_object->global_descriptor_set_layout) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan global descriptor set layout");
    return false;
  }

  VkDescriptorPoolSize global_pool_size = {
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = state->swapchain.image_count,
  };

  VkDescriptorPoolCreateInfo descriptor_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = state->swapchain.image_count,
      .poolSizeCount = 1,
      .pPoolSizes = &global_pool_size,
  };

  if (vkCreateDescriptorPool(state->device.logical_device,
                             &descriptor_pool_create_info, state->allocator,
                             &out_shader_object->global_descriptor_pool) !=
      VK_SUCCESS) {
    log_fatal("Failed to create Vulkan global descriptor pool");
    return false;
  }

  VkDescriptorSetLayout global_descriptor_layouts[3] = {
      out_shader_object->global_descriptor_set_layout,
      out_shader_object->global_descriptor_set_layout,
      out_shader_object->global_descriptor_set_layout,
  };

  VkDescriptorSetAllocateInfo global_descriptor_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = out_shader_object->global_descriptor_pool,
      .descriptorSetCount = 3,
      .pSetLayouts = global_descriptor_layouts,
  };

  if (vkAllocateDescriptorSets(
          state->device.logical_device, &global_descriptor_set_allocate_info,
          out_shader_object->global_descriptor_sets) != VK_SUCCESS) {
    log_fatal("Failed to allocate Vulkan global descriptor sets");
    return false;
  }

  log_debug("Created Vulkan global descriptor pool: %p",
            out_shader_object->global_descriptor_pool);

  BufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, BUFFER_TYPE_GRAPHICS);
  BufferDescription global_uniform_buffer_desc = {
      .size = sizeof(GlobalUniformObject),
      .usage = buffer_usage_flags_from_bits(BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER |
                                            BUFFER_USAGE_TRANSFER_DST |
                                            BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = memory_property_flags_from_bits(
          MEMORY_PROPERTY_DEVICE_LOCAL | MEMORY_PROPERTY_HOST_VISIBLE |
          MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v};

  if (!vulkan_buffer_create(state, &global_uniform_buffer_desc,
                            &out_shader_object->global_uniform_buffer)) {
    log_fatal("Failed to create Vulkan global uniform buffer");
    return false;
  }

  return true;
}

bool8_t vulkan_shader_update_global_state(VulkanBackendState *state,
                                          VulkanShaderObject *shader_object,
                                          VkPipelineLayout pipeline_layout,
                                          const GlobalUniformObject *uniform) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(pipeline_layout != VK_NULL_HANDLE, "Pipeline layout is NULL");
  assert_log(uniform != NULL, "Uniform is NULL");

  if (shader_object->global_uniform_buffer.buffer.handle == VK_NULL_HANDLE) {
    log_warn("Global uniform buffer not created yet, skipping descriptor set "
             "update");
    return false;
  }

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);
  VkDescriptorSet global_descriptor =
      shader_object->global_descriptor_sets[image_index];

  vkCmdBindDescriptorSets(command_buffer->handle,
                          VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                          1, &global_descriptor, 0, 0);

  if (!vulkan_buffer_load_data(state,
                               &shader_object->global_uniform_buffer.buffer, 0,
                               sizeof(GlobalUniformObject), 0, uniform)) {
    log_error("Failed to load global uniform buffer data");
    return false;
  }

  VkDescriptorBufferInfo buffer_info = {
      .buffer = shader_object->global_uniform_buffer.buffer.handle,
      .offset = 0,
      .range = sizeof(GlobalUniformObject),
  };

  VkWriteDescriptorSet descriptor_write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = shader_object->global_descriptor_sets[image_index],
      .dstBinding = 0,
      .dstArrayElement = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .pBufferInfo = &buffer_info,
      .pImageInfo = NULL,
      .pTexelBufferView = NULL,
  };

  vkUpdateDescriptorSets(state->device.logical_device, 1, &descriptor_write, 0,
                         NULL);

  // log_debug("Updated global descriptor sets with uniform buffer");

  return true;
}

bool8_t vulkan_shader_update_state(VulkanBackendState *state,
                                   VulkanShaderObject *shader_object,
                                   VkPipelineLayout pipeline_layout,
                                   const ShaderStateObject *data) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(pipeline_layout != VK_NULL_HANDLE, "Pipeline layout is NULL");
  assert_log(data != NULL, "Data is NULL");

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);

  vkCmdPushConstants(command_buffer->handle, pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShaderStateObject),
                     data);

  // log_debug("Updated shader state");

  return true;
}

void vulkan_shader_object_destroy(VulkanBackendState *state,
                                  VulkanShaderObject *out_shader_object) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(out_shader_object != NULL, "Shader object is NULL");

  vkDestroyDescriptorPool(state->device.logical_device,
                          out_shader_object->global_descriptor_pool,
                          state->allocator);
  vkDestroyDescriptorSetLayout(state->device.logical_device,
                               out_shader_object->global_descriptor_set_layout,
                               state->allocator);

  vulkan_buffer_destroy(state,
                        &out_shader_object->global_uniform_buffer.buffer);

  for (uint32_t i = 0; i < SHADER_STAGE_COUNT; i++) {
    vulkan_shader_module_destroy(state, out_shader_object->modules[i]);
  }
}