#include "vulkan_shaders.h"
#include "filesystem/filesystem.h"

bool8_t vulkan_shader_module_create(
    VulkanBackendState *state, VkrShaderStageFlags stage, const uint64_t size,
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
    log_error(
        "Invalid shader stage configuration: exactly one stage must be set");
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
                                    const VkrShaderObjectDescription *desc,
                                    VulkanShaderObject *out_shader_object) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(desc != NULL, "Shader object description is NULL");

  if (desc->file_format != VKR_SHADER_FILE_FORMAT_SPIR_V) {
    log_error("Only SPIR-V shader file format is supported");
    return false_v;
  }

  const VkPhysicalDeviceLimits *limits = &state->device.properties.limits;
  if (desc->global_ubo_stride < desc->global_ubo_size) {
    log_fatal("global_ubo_stride (%llu) < global_ubo_size (%llu)",
              (uint64_t)desc->global_ubo_stride,
              (uint64_t)desc->global_ubo_size);
    return false_v;
  }

  if (desc->instance_ubo_stride < desc->instance_ubo_size) {
    log_fatal("instance_ubo_stride (%llu) < instance_ubo_size (%llu)",
              (uint64_t)desc->instance_ubo_stride,
              (uint64_t)desc->instance_ubo_size);
    return false_v;
  }

  if (desc->global_ubo_size > limits->maxUniformBufferRange) {
    log_fatal(
        "global_ubo_size (%llu) exceeds device maxUniformBufferRange (%u)",
        (uint64_t)desc->global_ubo_size, limits->maxUniformBufferRange);
    return false_v;
  }

  if (desc->instance_ubo_size > limits->maxUniformBufferRange) {
    log_fatal(
        "instance_ubo_size (%llu) exceeds device maxUniformBufferRange (%u)",
        (uint64_t)desc->instance_ubo_size, limits->maxUniformBufferRange);
    return false_v;
  }

  if (desc->push_constant_size > limits->maxPushConstantsSize) {
    log_fatal(
        "push_constant_size (%llu) exceeds device maxPushConstantsSize (%u)",
        (uint64_t)desc->push_constant_size, limits->maxPushConstantsSize);
    return false_v;
  }

  if (desc->file_type == VKR_SHADER_FILE_TYPE_SINGLE) {
    const FilePath path =
        file_path_create(string8_cstr(&desc->modules[0].path), state->arena,
                         FILE_PATH_TYPE_RELATIVE);
    if (!file_exists(&path)) {
      log_fatal("Shader file does not exist: %s",
                string8_cstr(&desc->modules[0].path));
      return false_v;
    }

    uint8_t *shader_data = NULL;
    uint64_t shader_size = 0;
    FileError file_error =
        file_load_spirv_shader(&path, state->arena, &shader_data, &shader_size);
    if (file_error != FILE_ERROR_NONE) {
      log_fatal("Failed to load shader: %s", file_get_error_string(file_error));
      return false_v;
    }

    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; i++) {
      if (desc->modules[i].stages.set == 0)
        continue;
      if (!vulkan_shader_module_create(
              state, desc->modules[i].stages, shader_size, shader_data,
              desc->modules[i].entry_point, &out_shader_object->modules[i],
              &out_shader_object->stages[i])) {
        log_error("Failed to create shader module: %s",
                  string8_cstr(&desc->modules[i].path));
        for (uint32_t j = 0; j < i; j++) {
          if (out_shader_object->modules[j] != VK_NULL_HANDLE) {
            vulkan_shader_module_destroy(state, out_shader_object->modules[j]);
            out_shader_object->modules[j] = VK_NULL_HANDLE;
          }
        }
        return false_v;
      }
    }
  } else if (desc->file_type == VKR_SHADER_FILE_TYPE_MULTI) {
    // Load per-stage files
    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; i++) {
      if (desc->modules[i].stages.set == 0)
        continue; // stage not provided

      const FilePath path =
          file_path_create(string8_cstr(&desc->modules[i].path), state->arena,
                           FILE_PATH_TYPE_RELATIVE);
      if (!file_exists(&path)) {
        log_fatal("Shader file does not exist: %s",
                  string8_cstr(&desc->modules[i].path));
        return false_v;
      }

      uint8_t *shader_data = NULL;
      uint64_t shader_size = 0;
      FileError file_error = file_load_spirv_shader(&path, state->arena,
                                                    &shader_data, &shader_size);
      if (file_error != FILE_ERROR_NONE) {
        log_fatal("Failed to load shader: %s",
                  file_get_error_string(file_error));
        return false_v;
      }

      if (!vulkan_shader_module_create(
              state, desc->modules[i].stages, shader_size, shader_data,
              desc->modules[i].entry_point, &out_shader_object->modules[i],
              &out_shader_object->stages[i])) {
        return false_v;
      }
    }
  } else {
    log_error("Unknown shader file type");
    return false_v;
  }

  // Global descriptors

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

  out_shader_object->frame_count = state->swapchain.image_count;

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

  // Allocate per-frame global descriptor sets
  Scratch scratch_global = scratch_create(state->temp_arena);
  VkDescriptorSetLayout *global_descriptor_layouts =
      (VkDescriptorSetLayout *)arena_alloc(scratch_global.arena,
                                           sizeof(VkDescriptorSetLayout) *
                                               out_shader_object->frame_count,
                                           ARENA_MEMORY_TAG_RENDERER);
  for (uint32_t i = 0; i < out_shader_object->frame_count; i++) {
    global_descriptor_layouts[i] =
        out_shader_object->global_descriptor_set_layout;
  }

  VkDescriptorSetAllocateInfo global_descriptor_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = out_shader_object->global_descriptor_pool,
      .descriptorSetCount = out_shader_object->frame_count,
      .pSetLayouts = global_descriptor_layouts,
  };

  out_shader_object->global_descriptor_sets = (VkDescriptorSet *)arena_alloc(
      state->arena, sizeof(VkDescriptorSet) * out_shader_object->frame_count,
      ARENA_MEMORY_TAG_RENDERER);

  if (vkAllocateDescriptorSets(
          state->device.logical_device, &global_descriptor_set_allocate_info,
          (VkDescriptorSet *)out_shader_object->global_descriptor_sets) !=
      VK_SUCCESS) {
    log_fatal("Failed to allocate Vulkan global descriptor sets");
    scratch_destroy(scratch_global, ARENA_MEMORY_TAG_ARRAY);
    return false;
  }

  scratch_destroy(scratch_global, ARENA_MEMORY_TAG_ARRAY);

  log_debug("Created Vulkan global descriptor pool: %p",
            out_shader_object->global_descriptor_pool);

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  out_shader_object->global_ubo_size = desc->global_ubo_size;
  out_shader_object->global_ubo_stride = desc->global_ubo_stride;
  out_shader_object->instance_ubo_size = desc->instance_ubo_size;
  out_shader_object->instance_ubo_stride = desc->instance_ubo_stride;
  out_shader_object->push_constant_size = desc->push_constant_size;

  VkrBufferDescription global_uniform_buffer_desc = {
      .size =
          out_shader_object->global_ubo_stride * out_shader_object->frame_count,
      .usage = vkr_buffer_usage_flags_from_bits(
          VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER |
          VKR_BUFFER_USAGE_TRANSFER_DST | VKR_BUFFER_USAGE_TRANSFER_SRC),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_DEVICE_LOCAL | VKR_MEMORY_PROPERTY_HOST_VISIBLE |
          VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v};

  if (!vulkan_buffer_create(state, &global_uniform_buffer_desc,
                            &out_shader_object->global_uniform_buffer)) {
    log_fatal("Failed to create Vulkan global uniform buffer");
    return false;
  }

  // Instance descriptors: binding 0 = uniform buffer, 1 = sampled image, 2 =
  // sampler
  const uint32_t instance_sampler_count = 1;
  VkDescriptorSetLayoutBinding instance_descriptor_set_layout_bindings
      [VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];

  instance_descriptor_set_layout_bindings[0] = (VkDescriptorSetLayoutBinding){
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .pImmutableSamplers = NULL,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };

  instance_descriptor_set_layout_bindings[1] = (VkDescriptorSetLayoutBinding){
      .binding = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .descriptorCount = 1,
      .pImmutableSamplers = NULL,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };

  instance_descriptor_set_layout_bindings[2] = (VkDescriptorSetLayoutBinding){
      .binding = 2,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = 1,
      .pImmutableSamplers = NULL,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
  };

  VkDescriptorSetLayoutCreateInfo instance_descriptor_set_layout_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT,
      .pBindings = instance_descriptor_set_layout_bindings,
  };

  if (vkCreateDescriptorSetLayout(
          state->device.logical_device,
          &instance_descriptor_set_layout_create_info, state->allocator,
          &out_shader_object->instance_descriptor_set_layout) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan instance descriptor set layout");
    return false;
  }

  VkDescriptorPoolSize instance_pool_size[3] = {
      {
          .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
      },
      {
          .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .descriptorCount = VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
      },
      {
          .type = VK_DESCRIPTOR_TYPE_SAMPLER,
          .descriptorCount = VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
      },
  };

  VkDescriptorPoolCreateInfo instance_descriptor_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
      .poolSizeCount = 3,
      .pPoolSizes = instance_pool_size,
  };

  if (vkCreateDescriptorPool(
          state->device.logical_device, &instance_descriptor_pool_create_info,
          state->allocator,
          &out_shader_object->instance_descriptor_pool) != VK_SUCCESS) {
    log_fatal("Failed to create Vulkan instance descriptor pool");
    return false;
  }

  VkDescriptorSetLayout layouts[2] = {
      out_shader_object->instance_descriptor_set_layout,
      out_shader_object->instance_descriptor_set_layout,
  };

  VkrBufferDescription instance_uniform_buffer_desc = {
      .size = out_shader_object->instance_ubo_stride *
              VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
      .usage = vkr_buffer_usage_flags_from_bits(VKR_BUFFER_USAGE_TRANSFER_DST |
                                                VKR_BUFFER_USAGE_TRANSFER_SRC |
                                                VKR_BUFFER_USAGE_UNIFORM),
      .memory_properties = vkr_memory_property_flags_from_bits(
          VKR_MEMORY_PROPERTY_DEVICE_LOCAL | VKR_MEMORY_PROPERTY_HOST_VISIBLE |
          VKR_MEMORY_PROPERTY_HOST_COHERENT),
      .buffer_type = buffer_type,
      .bind_on_create = true_v};

  if (!vulkan_buffer_create(state, &instance_uniform_buffer_desc,
                            &out_shader_object->instance_uniform_buffer)) {
    log_fatal("Failed to create Vulkan instance uniform buffer");
    return false;
  }

  // Initialize free list for instance states
  out_shader_object->instance_uniform_buffer_count = 0;
  out_shader_object->instance_state_free_count = 0;

  return true;
}

bool8_t vulkan_shader_update_global_state(
    VulkanBackendState *state, VulkanShaderObject *shader_object,
    VkPipelineLayout pipeline_layout, const VkrGlobalUniformObject *uniform) {
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

  uint64_t global_offset =
      shader_object->global_ubo_stride * (uint64_t)image_index;
  if (!vulkan_buffer_load_data(
          state, &shader_object->global_uniform_buffer.buffer, global_offset,
          shader_object->global_ubo_size, 0, uniform)) {
    log_error("Failed to load global uniform buffer data");
    return false;
  }

  VkDescriptorBufferInfo buffer_info = {
      .buffer = shader_object->global_uniform_buffer.buffer.handle,
      .offset = global_offset,
      .range = shader_object->global_ubo_size,
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

  return true_v;
}

bool8_t vulkan_shader_update_instance(
    VulkanBackendState *state, VulkanShaderObject *shader_object,
    VkPipelineLayout pipeline_layout, const VkrShaderStateObject *data,
    const VkrRendererMaterialState *material) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(pipeline_layout != VK_NULL_HANDLE, "Pipeline layout is NULL");
  assert_log(data != NULL, "Data is NULL");

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);

  // Push constants (config-driven size). Use minimum of configured size and
  // available data size (Mat4) to avoid overruns.
  if (shader_object->push_constant_size > 0) {
    uint32_t pc_size = (uint32_t)shader_object->push_constant_size;
    if (pc_size > sizeof(Mat4))
      pc_size = (uint32_t)sizeof(Mat4);
    vkCmdPushConstants(command_buffer->handle, pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, pc_size, &data->model);
  }

  VulkanShaderObjectInstanceState *instance_state =
      &shader_object->instance_states[data->instance_state.id];
  if (instance_state->descriptor_sets == NULL ||
      instance_state->descriptor_sets[image_index] == VK_NULL_HANDLE) {
    log_warn("Instance descriptor set not created yet, skipping update");
    return false;
  }

  VkDescriptorSet local_descriptor =
      instance_state->descriptor_sets[image_index];

  VkWriteDescriptorSet
      descriptor_writes[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  VkDescriptorBufferInfo
      buffer_infos[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  VkDescriptorImageInfo
      image_infos[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  MemZero(descriptor_writes, sizeof(descriptor_writes));
  MemZero(buffer_infos, sizeof(buffer_infos));
  MemZero(image_infos, sizeof(image_infos));

  uint32_t descriptor_index = 0;
  uint32_t descriptor_count = 0;
  VkrInstanceUniformObject local_uniform_object;

  uint32_t range = (uint32_t)shader_object->instance_ubo_size;
  uint64_t offset =
      shader_object->instance_ubo_stride * (uint64_t)data->instance_state.id;

  // Material uniforms provided explicitly
  if (material) {
    local_uniform_object = material->uniforms;
  } else {
    // Fallback: ensure zeroed
    MemZero(&local_uniform_object, sizeof(VkrInstanceUniformObject));
  }

  if (!vulkan_buffer_load_data(state,
                               &shader_object->instance_uniform_buffer.buffer,
                               offset, range, 0, &local_uniform_object)) {
    log_error("Failed to load instance uniform buffer data");
    return false;
  }

  if (instance_state->descriptor_states[descriptor_index]
          .generations[image_index] == VKR_INVALID_ID) {
    buffer_infos[descriptor_count] = (VkDescriptorBufferInfo){
        .buffer = shader_object->instance_uniform_buffer.buffer.handle,
        .offset = offset,
        .range = range,
    };

    descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = local_descriptor,
        .dstBinding = descriptor_index,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &buffer_infos[descriptor_count],
        .pImageInfo = NULL,
        .pTexelBufferView = NULL,
    };

    descriptor_count++;

    instance_state->descriptor_states[descriptor_index]
        .generations[image_index] = 1;
  }
  descriptor_index++;

  const uint32_t sampler_count = 1;
  for (uint32_t sampler_index = 0; sampler_index < sampler_count;
       sampler_index++) {
    struct s_TextureHandle *texture = NULL;
    if (material && material->texture0_enabled) {
      texture = (struct s_TextureHandle *)material->texture0;
    }
    if (texture == NULL || texture->texture.image.handle == VK_NULL_HANDLE) {
      // No texture bound; skip image/sampler updates
      continue;
    }

    VulkanTexture *texture_object = &texture->texture;

    // Binding 1: SAMPLED_IMAGE (image view + layout)
    {
      uint32_t *image_desc_generation =
          &instance_state->descriptor_states[1].generations[image_index];

      if (*image_desc_generation != texture->description.generation ||
          *image_desc_generation == VKR_INVALID_ID) {
        image_infos[descriptor_count] = (VkDescriptorImageInfo){
            .sampler = VK_NULL_HANDLE,
            .imageView = texture_object->image.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = local_descriptor,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[descriptor_count],
        };

        descriptor_count++;
        *image_desc_generation = texture->description.generation;
      }
    }

    // Binding 2: SAMPLER (sampler only)
    {
      uint32_t *sampler_desc_generation =
          &instance_state->descriptor_states[2].generations[image_index];

      if (*sampler_desc_generation != texture->description.generation ||
          *sampler_desc_generation == VKR_INVALID_ID) {
        image_infos[descriptor_count] = (VkDescriptorImageInfo){
            .sampler = texture_object->sampler,
            .imageView = VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = local_descriptor,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[descriptor_count],
        };

        descriptor_count++;
        *sampler_desc_generation = texture->description.generation;
      }
    }
  }

  if (descriptor_count > 0) {
    vkUpdateDescriptorSets(state->device.logical_device, descriptor_count,
                           descriptor_writes, 0, NULL);
  } else {
    // No descriptor writes needed this frame for this instance
    state->descriptor_writes_avoided++;
  }

  vkCmdBindDescriptorSets(command_buffer->handle,
                          VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1,
                          1, &local_descriptor, 0, 0);

  return true_v;
}

bool8_t vulkan_shader_acquire_instance(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t *out_object_id) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");

  if (shader_object->instance_state_free_count > 0) {
    shader_object->instance_state_free_count--;
    *out_object_id =
        shader_object
            ->instance_state_free_ids[shader_object->instance_state_free_count];
  } else {
    *out_object_id = shader_object->instance_uniform_buffer_count;
    shader_object->instance_uniform_buffer_count++;
  }

  uint32_t object_id = *out_object_id;
  VulkanShaderObjectInstanceState *local_state =
      &shader_object->instance_states[object_id];
  for (uint32_t descriptor_state_idx = 0;
       descriptor_state_idx < VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT;
       descriptor_state_idx++) {
    // Allocate per-frame generations
    local_state->descriptor_states[descriptor_state_idx].generations =
        arena_alloc(state->arena, sizeof(uint32_t) * shader_object->frame_count,
                    ARENA_MEMORY_TAG_RENDERER);
    for (uint32_t descriptor_generation_idx = 0;
         descriptor_generation_idx < shader_object->frame_count;
         descriptor_generation_idx++) {
      local_state->descriptor_states[descriptor_state_idx]
          .generations[descriptor_generation_idx] = VKR_INVALID_ID;
    }
  }

  // Allocate per-frame instance descriptor sets
  local_state->descriptor_sets = (VkDescriptorSet *)arena_alloc(
      state->arena, sizeof(VkDescriptorSet) * shader_object->frame_count,
      ARENA_MEMORY_TAG_RENDERER);

  Scratch scratch_local = scratch_create(state->temp_arena);
  VkDescriptorSetLayout *layouts = (VkDescriptorSetLayout *)arena_alloc(
      scratch_local.arena,
      sizeof(VkDescriptorSetLayout) * shader_object->frame_count,
      ARENA_MEMORY_TAG_RENDERER);
  for (uint32_t i = 0; i < shader_object->frame_count; i++) {
    layouts[i] = shader_object->instance_descriptor_set_layout;
  }

  VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = shader_object->instance_descriptor_pool,
      .descriptorSetCount = shader_object->frame_count,
      .pSetLayouts = layouts,
  };

  if (vkAllocateDescriptorSets(
          state->device.logical_device, &descriptor_set_allocate_info,
          (VkDescriptorSet *)local_state->descriptor_sets) != VK_SUCCESS) {
    log_error("Failed to allocate descriptor set");
    scratch_destroy(scratch_local, ARENA_MEMORY_TAG_ARRAY);
    return false;
  }

  scratch_destroy(scratch_local, ARENA_MEMORY_TAG_ARRAY);
  return true_v;
}

bool8_t vulkan_shader_release_instance(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t object_id) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(object_id < shader_object->instance_uniform_buffer_count,
             "Object ID is out of bounds");

  VulkanShaderObjectInstanceState *local_state =
      &shader_object->instance_states[object_id];

  if (vkFreeDescriptorSets(state->device.logical_device,
                           shader_object->instance_descriptor_pool,
                           shader_object->frame_count,
                           local_state->descriptor_sets) != VK_SUCCESS) {
    log_error("Failed to free descriptor sets");
    return false;
  }

  // Reset generation tracking back to invalid without freeing memory
  for (uint32_t descriptor_state_idx = 0;
       descriptor_state_idx < VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT;
       descriptor_state_idx++) {
    for (uint32_t descriptor_generation_idx = 0;
         descriptor_generation_idx < shader_object->frame_count;
         descriptor_generation_idx++) {
      local_state->descriptor_states[descriptor_state_idx]
          .generations[descriptor_generation_idx] = VKR_INVALID_ID;
    }
  }

  // Push id to free list for reuse
  assert_log(shader_object->instance_state_free_count <
                 VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
             "instance_state_free_ids overflow");
  shader_object
      ->instance_state_free_ids[shader_object->instance_state_free_count++] =
      object_id;

  return true_v;
}

void vulkan_shader_object_destroy(VulkanBackendState *state,
                                  VulkanShaderObject *out_shader_object) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(out_shader_object != NULL, "Shader object is NULL");

  vkDestroyDescriptorPool(state->device.logical_device,
                          out_shader_object->instance_descriptor_pool,
                          state->allocator);
  vkDestroyDescriptorSetLayout(
      state->device.logical_device,
      out_shader_object->instance_descriptor_set_layout, state->allocator);

  if (out_shader_object->instance_descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(state->device.logical_device,
                            out_shader_object->global_descriptor_pool,
                            state->allocator);
  }
  vkDestroyDescriptorSetLayout(state->device.logical_device,
                               out_shader_object->global_descriptor_set_layout,
                               state->allocator);

  vulkan_buffer_destroy(state,
                        &out_shader_object->instance_uniform_buffer.buffer);
  vulkan_buffer_destroy(state,
                        &out_shader_object->global_uniform_buffer.buffer);

  for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; i++) {
    vulkan_shader_module_destroy(state, out_shader_object->modules[i]);
  }
}