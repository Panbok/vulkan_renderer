#include "vulkan_shaders.h"
#include "filesystem/filesystem.h"
#include "vulkan_spirv_reflection.h"

typedef struct VulkanShaderReflectionInput {
  VkrShaderStageModuleDesc modules[VKR_SHADER_STAGE_COUNT];
  uint32_t module_count;
  uint8_t *owned_buffers[VKR_SHADER_STAGE_COUNT];
  uint64_t owned_buffer_sizes[VKR_SHADER_STAGE_COUNT];
  uint32_t owned_buffer_count;
  String8 program_name;
} VulkanShaderReflectionInput;

vkr_internal const uint32_t VKR_SHADER_REFLECTION_INDEX_INVALID = UINT32_MAX;
#define VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX 32u

typedef struct VulkanDescriptorPoolTypeCount {
  VkDescriptorType type;
  uint32_t count;
} VulkanDescriptorPoolTypeCount;

vkr_internal VkImageLayout
vulkan_shader_image_layout_for_texture(const struct s_TextureHandle *texture) {
  if (!texture) {
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  switch (texture->description.format) {
  case VKR_TEXTURE_FORMAT_D16_UNORM:
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  default:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

vkr_internal bool8_t
vulkan_shader_texture_has_image_view(const struct s_TextureHandle *texture) {
  return texture && texture->texture.image.handle != VK_NULL_HANDLE &&
         texture->texture.image.view != VK_NULL_HANDLE;
}

vkr_internal bool8_t
vulkan_shader_texture_has_sampler(const struct s_TextureHandle *texture) {
  return texture && texture->texture.sampler != VK_NULL_HANDLE;
}

vkr_internal bool8_t vulkan_shader_texture_ready_for_descriptors(
    const struct s_TextureHandle *texture, bool8_t needs_image_view,
    bool8_t needs_sampler) {
  if (!texture) {
    return false_v;
  }
  if (needs_image_view && !vulkan_shader_texture_has_image_view(texture)) {
    return false_v;
  }
  if (needs_sampler && !vulkan_shader_texture_has_sampler(texture)) {
    return false_v;
  }
  return true_v;
}

vkr_internal void
vulkan_shader_reflection_input_destroy(VkrAllocator *allocator,
                                       VulkanShaderReflectionInput *input) {
  if (!allocator || !input) {
    return;
  }

  for (uint32_t i = 0; i < input->owned_buffer_count; ++i) {
    if (!input->owned_buffers[i] || input->owned_buffer_sizes[i] == 0) {
      continue;
    }
    vkr_allocator_free(allocator, input->owned_buffers[i],
                       input->owned_buffer_sizes[i],
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
  }
  MemZero(input, sizeof(*input));
}

vkr_internal bool8_t vulkan_shader_collect_reflection_input(
    VulkanBackendState *state, const VkrShaderObjectDescription *desc,
    VulkanShaderReflectionInput *out_input) {
  if (!state || !desc || !out_input) {
    return false_v;
  }

  MemZero(out_input, sizeof(*out_input));

  if (desc->file_type == VKR_SHADER_FILE_TYPE_SINGLE) {
    uint32_t first_stage_index = VKR_SHADER_STAGE_COUNT;
    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; ++i) {
      if (desc->modules[i].stages.set != 0) {
        first_stage_index = i;
        break;
      }
    }
    if (first_stage_index == VKR_SHADER_STAGE_COUNT) {
      log_error(
          "Reflection input collection failed: no shader stages provided");
      return false_v;
    }

    const FilePath path =
        file_path_create(string8_cstr(&desc->modules[first_stage_index].path),
                         &state->alloc, FILE_PATH_TYPE_RELATIVE);
    if (!file_exists(&path)) {
      log_error(
          "Reflection input collection failed: shader file does not exist: "
          "%s",
          string8_cstr(&desc->modules[first_stage_index].path));
      return false_v;
    }

    uint8_t *spirv_bytes = NULL;
    uint64_t spirv_size = 0;
    const FileError file_error =
        file_load_spirv_shader(&path, &state->alloc, &spirv_bytes, &spirv_size);
    if (file_error != FILE_ERROR_NONE) {
      log_error("Reflection input collection failed: %s",
                file_get_error_string(file_error));
      return false_v;
    }

    out_input->owned_buffers[0] = spirv_bytes;
    out_input->owned_buffer_sizes[0] = spirv_size;
    out_input->owned_buffer_count = 1;

    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; ++i) {
      if (desc->modules[i].stages.set == 0) {
        continue;
      }

      const VulkanShaderStageFlagResult stage =
          vulkan_shader_stage_to_vk(desc->modules[i].stages);
      if (!stage.is_valid) {
        log_error("Reflection input collection failed: invalid stage mask");
        vulkan_shader_reflection_input_destroy(&state->alloc, out_input);
        return false_v;
      }

      out_input->modules[out_input->module_count++] =
          (VkrShaderStageModuleDesc){
              .stage = stage.flag,
              .path = desc->modules[i].path,
              .entry_point = desc->modules[i].entry_point,
              .spirv_bytes = spirv_bytes,
              .spirv_size = spirv_size,
          };
    }
  } else if (desc->file_type == VKR_SHADER_FILE_TYPE_MULTI) {
    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; ++i) {
      if (desc->modules[i].stages.set == 0) {
        continue;
      }

      const FilePath path =
          file_path_create(string8_cstr(&desc->modules[i].path), &state->alloc,
                           FILE_PATH_TYPE_RELATIVE);
      if (!file_exists(&path)) {
        log_error("Reflection input collection failed: shader file does not "
                  "exist: %s",
                  string8_cstr(&desc->modules[i].path));
        vulkan_shader_reflection_input_destroy(&state->alloc, out_input);
        return false_v;
      }

      uint8_t *spirv_bytes = NULL;
      uint64_t spirv_size = 0;
      const FileError file_error = file_load_spirv_shader(
          &path, &state->alloc, &spirv_bytes, &spirv_size);
      if (file_error != FILE_ERROR_NONE) {
        log_error("Reflection input collection failed: %s",
                  file_get_error_string(file_error));
        vulkan_shader_reflection_input_destroy(&state->alloc, out_input);
        return false_v;
      }

      const VulkanShaderStageFlagResult stage =
          vulkan_shader_stage_to_vk(desc->modules[i].stages);
      if (!stage.is_valid) {
        log_error("Reflection input collection failed: invalid stage mask");
        vkr_allocator_free(&state->alloc, spirv_bytes, spirv_size,
                           VKR_ALLOCATOR_MEMORY_TAG_FILE);
        vulkan_shader_reflection_input_destroy(&state->alloc, out_input);
        return false_v;
      }

      out_input->owned_buffers[out_input->owned_buffer_count] = spirv_bytes;
      out_input->owned_buffer_sizes[out_input->owned_buffer_count] = spirv_size;
      ++out_input->owned_buffer_count;

      out_input->modules[out_input->module_count++] =
          (VkrShaderStageModuleDesc){
              .stage = stage.flag,
              .path = desc->modules[i].path,
              .entry_point = desc->modules[i].entry_point,
              .spirv_bytes = spirv_bytes,
              .spirv_size = spirv_size,
          };
    }
  } else {
    log_error("Reflection input collection failed: unknown shader file type");
    return false_v;
  }

  if (out_input->module_count == 0) {
    log_error(
        "Reflection input collection failed: no shader modules collected");
    vulkan_shader_reflection_input_destroy(&state->alloc, out_input);
    return false_v;
  }

  out_input->program_name = out_input->modules[0].path;
  return true_v;
}

vkr_internal void
vulkan_shader_log_reflection_error(const VkrReflectionErrorContext *error) {
  if (!error) {
    return;
  }

  log_error("Shader reflection failed: code=%s program='%.*s' module='%.*s' "
            "entry='%.*s' stage=0x%x set=%u binding=%u location=%u backend=%d",
            vulkan_reflection_error_string(error->code),
            (int)error->program_name.length,
            error->program_name.str ? (const char *)error->program_name.str
                                    : "",
            (int)error->module_path.length,
            error->module_path.str ? (const char *)error->module_path.str : "",
            (int)error->entry_point.length,
            error->entry_point.str ? (const char *)error->entry_point.str : "",
            error->stage, error->set, error->binding, error->location,
            error->backend_result);
}

vkr_internal void vulkan_shader_log_reflection_layout_debug(
    String8 program_name, const VkrShaderReflection *reflection) {
#ifndef NDEBUG
  if (!reflection) {
    return;
  }

  log_debug("Reflected layout for '%.*s': sets=%u layout_sets=%u "
            "push_constants=%u vertex_bindings=%u vertex_attributes=%u",
            (int)program_name.length,
            program_name.str ? (const char *)program_name.str : "",
            reflection->set_count, reflection->layout_set_count,
            reflection->push_constant_range_count,
            reflection->vertex_binding_count,
            reflection->vertex_attribute_count);

  for (uint32_t set_index = 0; set_index < reflection->set_count; ++set_index) {
    const VkrDescriptorSetDesc *set_desc = &reflection->sets[set_index];
    log_debug("  set=%u role=%u bindings=%u", set_desc->set, set_desc->role,
              set_desc->binding_count);
    for (uint32_t binding_index = 0; binding_index < set_desc->binding_count;
         ++binding_index) {
      const VkrDescriptorBindingDesc *binding =
          &set_desc->bindings[binding_index];
      log_debug("    binding=%u type=%d count=%u stages=0x%x size=%u",
                binding->binding, binding->type, binding->count,
                binding->stages, binding->byte_size);
    }
  }

  for (uint32_t i = 0; i < reflection->push_constant_range_count; ++i) {
    const VkrPushConstantRangeDesc *range =
        &reflection->push_constant_ranges[i];
    log_debug("  push_constant[%u] offset=%u size=%u stages=0x%x", i,
              range->offset, range->size, range->stages);
  }
#else
  (void)program_name;
  (void)reflection;
#endif
}

vkr_internal void
vulkan_shader_destroy_modules(VulkanBackendState *state,
                              VulkanShaderObject *shader_object) {
  if (!state || !shader_object) {
    return;
  }
  for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; ++i) {
    vulkan_shader_module_destroy(state, shader_object->modules[i]);
    shader_object->modules[i] = VK_NULL_HANDLE;
  }
}

vkr_internal const VkrDescriptorSetDesc *
vulkan_shader_reflection_find_set_by_index(
    const VkrShaderReflection *reflection, uint32_t set_index) {
  if (!reflection) {
    return NULL;
  }
  for (uint32_t i = 0; i < reflection->set_count; ++i) {
    if (reflection->sets[i].set == set_index) {
      return &reflection->sets[i];
    }
  }
  return NULL;
}

vkr_internal const VkrDescriptorSetDesc *
vulkan_shader_reflection_find_set_by_role(const VkrShaderReflection *reflection,
                                          VkrDescriptorSetRole role) {
  if (!reflection) {
    return NULL;
  }
  for (uint32_t i = 0; i < reflection->set_count; ++i) {
    if (reflection->sets[i].role == role) {
      return &reflection->sets[i];
    }
  }
  return NULL;
}

vkr_internal const VkrDescriptorBindingDesc *
vulkan_shader_reflection_find_binding(const VkrDescriptorSetDesc *set_desc,
                                      uint32_t binding) {
  if (!set_desc) {
    return NULL;
  }
  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    if (set_desc->bindings[i].binding == binding) {
      return &set_desc->bindings[i];
    }
  }
  return NULL;
}

vkr_internal bool8_t
vulkan_shader_descriptor_type_is_uniform(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
         type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

vkr_internal bool8_t
vulkan_shader_descriptor_type_is_storage(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
         type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

vkr_internal bool8_t
vulkan_shader_descriptor_type_is_dynamic(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
         type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

vkr_internal const VkrDescriptorBindingDesc *
vulkan_shader_reflection_find_first_binding_of_type(
    const VkrDescriptorSetDesc *set_desc,
    bool8_t (*predicate)(VkDescriptorType)) {
  if (!set_desc || !predicate) {
    return NULL;
  }
  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    if (predicate(set_desc->bindings[i].type)) {
      return &set_desc->bindings[i];
    }
  }
  return NULL;
}

vkr_internal uint32_t vulkan_shader_reflection_count_dynamic_descriptors(
    const VkrDescriptorSetDesc *set_desc) {
  if (!set_desc) {
    return 0;
  }
  uint32_t total = 0;
  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    if (vulkan_shader_descriptor_type_is_dynamic(set_desc->bindings[i].type)) {
      total += set_desc->bindings[i].count;
    }
  }
  return total;
}

vkr_internal uint32_t vulkan_shader_reflection_count_descriptors_of_type(
    const VkrDescriptorSetDesc *set_desc,
    bool8_t (*predicate)(VkDescriptorType)) {
  if (!set_desc || !predicate) {
    return 0;
  }

  uint32_t total = 0;
  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    if (predicate(set_desc->bindings[i].type)) {
      total += set_desc->bindings[i].count;
    }
  }
  return total;
}

vkr_internal bool8_t
vulkan_shader_descriptor_type_is_sampled_image(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

vkr_internal bool8_t
vulkan_shader_descriptor_type_is_sampler(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_SAMPLER;
}

vkr_internal uint64_t vulkan_shader_reflection_uniform_binding_size(
    const VkrDescriptorSetDesc *set_desc, uint32_t binding_index) {
  if (!set_desc || binding_index == VKR_SHADER_REFLECTION_INDEX_INVALID) {
    return 0;
  }

  const VkrDescriptorBindingDesc *binding =
      vulkan_shader_reflection_find_binding(set_desc, binding_index);
  if (!binding) {
    return 0;
  }

  return (uint64_t)binding->byte_size;
}

vkr_internal bool8_t vulkan_shader_validate_linear_binding_slots(
    const VkrDescriptorSetDesc *set_desc, uint32_t base_binding,
    VkDescriptorType descriptor_type, uint32_t slot_count) {
  if (!set_desc || slot_count == 0 ||
      base_binding == VKR_SHADER_REFLECTION_INDEX_INVALID) {
    return false_v;
  }

  for (uint32_t i = 0; i < slot_count; ++i) {
    const uint32_t binding_index = base_binding + i;
    const VkrDescriptorBindingDesc *binding_desc =
        vulkan_shader_reflection_find_binding(set_desc, binding_index);
    if (!binding_desc) {
      return false_v;
    }
    if (binding_desc->type != descriptor_type || binding_desc->count != 1) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal uint64_t
vulkan_shader_max_push_constant_end(const VkrShaderReflection *reflection) {
  if (!reflection || reflection->push_constant_range_count == 0) {
    return 0;
  }

  uint64_t max_end = 0;
  for (uint32_t i = 0; i < reflection->push_constant_range_count; ++i) {
    const VkrPushConstantRangeDesc *range =
        &reflection->push_constant_ranges[i];
    uint64_t end = (uint64_t)range->offset + (uint64_t)range->size;
    if (end > max_end) {
      max_end = end;
    }
  }
  return max_end;
}

vkr_internal uint64_t vulkan_shader_align_up_u64(uint64_t value,
                                                 uint64_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

vkr_internal bool8_t vulkan_shader_pool_type_count_add(
    VulkanDescriptorPoolTypeCount *entries, uint32_t *entry_count,
    uint32_t entry_capacity, VkDescriptorType type, uint32_t count) {
  if (!entries || !entry_count || count == 0) {
    return false_v;
  }
  for (uint32_t i = 0; i < *entry_count; ++i) {
    if (entries[i].type == type) {
      entries[i].count += count;
      return true_v;
    }
  }
  if (*entry_count >= entry_capacity) {
    return false_v;
  }
  entries[*entry_count] = (VulkanDescriptorPoolTypeCount){
      .type = type,
      .count = count,
  };
  (*entry_count)++;
  return true_v;
}

vkr_internal bool8_t vulkan_shader_create_set_layout_from_reflection(
    VulkanBackendState *state, const VkrDescriptorSetDesc *set_desc,
    VkDescriptorSetLayout *out_layout) {
  if (!state || !set_desc || !out_layout) {
    return false_v;
  }

  VkDescriptorSetLayoutBinding
      bindings[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  if (set_desc->binding_count > ArrayCount(bindings)) {
    log_error("Reflected set %u has too many bindings (%u > %u)", set_desc->set,
              set_desc->binding_count, ArrayCount(bindings));
    return false_v;
  }

  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    const VkrDescriptorBindingDesc *src = &set_desc->bindings[i];
    bindings[i] = (VkDescriptorSetLayoutBinding){
        .binding = src->binding,
        .descriptorType = src->type,
        .descriptorCount = src->count,
        .stageFlags = src->stages,
        .pImmutableSamplers = NULL,
    };
  }

  const VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = set_desc->binding_count,
      .pBindings = set_desc->binding_count > 0 ? bindings : NULL,
  };

  if (vkCreateDescriptorSetLayout(state->device.logical_device, &layout_info,
                                  state->allocator, out_layout) != VK_SUCCESS) {
    log_error("Failed to create descriptor set layout for reflected set %u",
              set_desc->set);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vulkan_shader_resolve_runtime_set_contract(
    const VkrShaderReflection *reflection, VulkanShaderObject *shader_object) {
  if (!reflection || !shader_object) {
    return false_v;
  }

  shader_object->frame_set_index = VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->draw_set_index = VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->frame_uniform_binding = VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->frame_instance_buffer_binding =
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->draw_uniform_binding = VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->draw_sampled_image_binding_base =
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->draw_sampler_binding_base =
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  shader_object->frame_dynamic_offset_count = 0;
  shader_object->draw_dynamic_offset_count = 0;

  const VkrDescriptorSetDesc *frame_set =
      vulkan_shader_reflection_find_set_by_role(reflection,
                                                VKR_DESCRIPTOR_SET_ROLE_FRAME);
  const VkrDescriptorSetDesc *draw_set =
      vulkan_shader_reflection_find_set_by_role(reflection,
                                                VKR_DESCRIPTOR_SET_ROLE_DRAW);

  if (!frame_set && reflection->set_count > 0) {
    frame_set = &reflection->sets[0];
  }
  if (!draw_set && reflection->set_count > 1) {
    draw_set = &reflection->sets[1];
  }
  if (draw_set && frame_set && draw_set->set == frame_set->set) {
    draw_set = NULL;
  }

  if (frame_set) {
    shader_object->frame_set_index = frame_set->set;
    const VkrDescriptorBindingDesc *frame_uniform =
        vulkan_shader_reflection_find_first_binding_of_type(
            frame_set, vulkan_shader_descriptor_type_is_uniform);
    const VkrDescriptorBindingDesc *frame_storage =
        vulkan_shader_reflection_find_first_binding_of_type(
            frame_set, vulkan_shader_descriptor_type_is_storage);
    if (frame_uniform) {
      shader_object->frame_uniform_binding = frame_uniform->binding;
    }
    if (frame_storage) {
      shader_object->frame_instance_buffer_binding = frame_storage->binding;
    }
    shader_object->frame_dynamic_offset_count =
        vulkan_shader_reflection_count_dynamic_descriptors(frame_set);
    if (shader_object->frame_dynamic_offset_count >
        VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT) {
      log_error("Frame set %u dynamic descriptor count (%u) exceeds max %u",
                frame_set->set, shader_object->frame_dynamic_offset_count,
                VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT);
      return false_v;
    }
  }

  if (draw_set) {
    shader_object->draw_set_index = draw_set->set;
    shader_object->draw_dynamic_offset_count =
        vulkan_shader_reflection_count_dynamic_descriptors(draw_set);
    if (shader_object->draw_dynamic_offset_count >
        VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT) {
      log_error("Draw set %u dynamic descriptor count (%u) exceeds max %u",
                draw_set->set, shader_object->draw_dynamic_offset_count,
                VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT);
      return false_v;
    }

    const VkrDescriptorBindingDesc *draw_uniform =
        vulkan_shader_reflection_find_first_binding_of_type(
            draw_set, vulkan_shader_descriptor_type_is_uniform);
    if (draw_uniform) {
      shader_object->draw_uniform_binding = draw_uniform->binding;
    }

    for (uint32_t i = 0; i < draw_set->binding_count; ++i) {
      const VkrDescriptorBindingDesc *binding = &draw_set->bindings[i];
      if (binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
          shader_object->draw_sampled_image_binding_base ==
              VKR_SHADER_REFLECTION_INDEX_INVALID) {
        shader_object->draw_sampled_image_binding_base = binding->binding;
      }
      if (binding->type == VK_DESCRIPTOR_TYPE_SAMPLER &&
          shader_object->draw_sampler_binding_base ==
              VKR_SHADER_REFLECTION_INDEX_INVALID) {
        shader_object->draw_sampler_binding_base = binding->binding;
      }
    }
  }

  return true_v;
}

vkr_internal bool8_t vulkan_shader_validate_descriptor_write(
    const VkrShaderReflection *reflection, uint32_t set_index, uint32_t binding,
    VkDescriptorType type, uint32_t array_element, uint32_t count) {
  const VkrDescriptorSetDesc *set_desc =
      vulkan_shader_reflection_find_set_by_index(reflection, set_index);
  if (!set_desc) {
    log_error("Descriptor write rejected: set %u not reflected", set_index);
    return false_v;
  }
  const VkrDescriptorBindingDesc *binding_desc =
      vulkan_shader_reflection_find_binding(set_desc, binding);
  if (!binding_desc) {
    log_error("Descriptor write rejected: set %u binding %u not reflected",
              set_index, binding);
    return false_v;
  }
  if (binding_desc->type != type) {
    log_error("Descriptor write rejected: set %u binding %u type mismatch "
              "(write=%d reflected=%d)",
              set_index, binding, type, binding_desc->type);
    return false_v;
  }
  if (count == 0 || array_element + count > binding_desc->count) {
    log_error(
        "Descriptor write rejected: set %u binding %u range out of bounds "
        "(array=%u count=%u reflected_count=%u)",
        set_index, binding, array_element, count, binding_desc->count);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vulkan_shader_bind_descriptor_set_checked(
    VkCommandBuffer command_buffer, VkPipelineLayout pipeline_layout,
    uint32_t set_index, VkDescriptorSet descriptor_set,
    uint32_t expected_dynamic_offset_count,
    uint32_t supplied_dynamic_offset_count, const uint32_t *dynamic_offsets) {
  if (expected_dynamic_offset_count != supplied_dynamic_offset_count) {
    log_error("Descriptor bind rejected for set %u: expected %u dynamic "
              "offsets, supplied %u",
              set_index, expected_dynamic_offset_count,
              supplied_dynamic_offset_count);
    return false_v;
  }

  if (supplied_dynamic_offset_count > 0 && !dynamic_offsets) {
    log_error("Descriptor bind rejected for set %u: %u dynamic offsets "
              "expected but no offset array supplied",
              set_index, supplied_dynamic_offset_count);
    return false_v;
  }

  vkCmdBindDescriptorSets(
      command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
      set_index, 1, &descriptor_set, supplied_dynamic_offset_count,
      supplied_dynamic_offset_count > 0 ? dynamic_offsets : NULL);
  return true_v;
}

vkr_internal bool8_t vulkan_shader_descriptor_state_index_from_binding(
    uint32_t binding, uint32_t *out_index) {
  if (!out_index) {
    return false_v;
  }
  if (binding >= VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT) {
    return false_v;
  }
  *out_index = binding;
  return true_v;
}

vkr_internal uint32_t vulkan_shader_default_set_allocation_count(
    VkrDescriptorSetRole role, uint32_t swapchain_image_count) {
  switch (role) {
  case VKR_DESCRIPTOR_SET_ROLE_FRAME:
    return Max(swapchain_image_count, 1u);
  case VKR_DESCRIPTOR_SET_ROLE_MATERIAL:
    return 256u;
  case VKR_DESCRIPTOR_SET_ROLE_DRAW:
    return 1024u;
  case VKR_DESCRIPTOR_SET_ROLE_FEATURE:
  case VKR_DESCRIPTOR_SET_ROLE_NONE:
  default:
    return 64u;
  }
}

vkr_internal bool8_t vulkan_shader_create_instance_descriptor_pool(
    VulkanBackendState *state, const VkrDescriptorSetDesc *draw_set_desc,
    uint32_t frame_count, uint32_t instance_capacity,
    VkDescriptorPool *out_pool) {
  if (!state || !draw_set_desc || !out_pool || frame_count == 0 ||
      instance_capacity == 0) {
    return false_v;
  }

  const uint64_t max_sets_u64 =
      (uint64_t)frame_count * (uint64_t)instance_capacity;
  if (max_sets_u64 > UINT32_MAX) {
    log_error("Instance descriptor pool maxSets overflow");
    return false_v;
  }
  const uint32_t max_sets = (uint32_t)max_sets_u64;

  VulkanDescriptorPoolTypeCount
      type_counts[VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX];
  MemZero(type_counts, sizeof(type_counts));
  uint32_t type_count = 0;
  for (uint32_t i = 0; i < draw_set_desc->binding_count; ++i) {
    const VkrDescriptorBindingDesc *binding = &draw_set_desc->bindings[i];
    if (!vulkan_shader_pool_type_count_add(
            type_counts, &type_count, VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX,
            binding->type, binding->count * max_sets)) {
      log_error("Descriptor pool type table overflow for draw set %u",
                draw_set_desc->set);
      return false_v;
    }
  }

  VkDescriptorPoolSize pool_sizes[VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX];
  for (uint32_t i = 0; i < type_count; ++i) {
    pool_sizes[i] = (VkDescriptorPoolSize){
        .type = type_counts[i].type,
        .descriptorCount = type_counts[i].count,
    };
  }

  const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = max_sets,
      .poolSizeCount = type_count,
      .pPoolSizes = type_count > 0 ? pool_sizes : NULL,
  };

  if (vkCreateDescriptorPool(state->device.logical_device, &pool_info,
                             state->allocator, out_pool) != VK_SUCCESS) {
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vulkan_shader_allocate_instance_sets_from_pool(
    VulkanBackendState *state, VulkanShaderObject *shader_object,
    VkDescriptorPool pool, VkDescriptorSet *out_sets, VkResult *out_result) {
  if (!state || !shader_object || !out_sets) {
    return false_v;
  }

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(&state->temp_scope);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    log_error("Failed to acquire temporary allocator for descriptor sets");
    return false_v;
  }
  VkDescriptorSetLayout *layouts = (VkDescriptorSetLayout *)vkr_allocator_alloc(
      &state->temp_scope,
      sizeof(VkDescriptorSetLayout) * shader_object->frame_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!layouts) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  for (uint32_t i = 0; i < shader_object->frame_count; ++i) {
    layouts[i] = shader_object->instance_descriptor_set_layout;
  }

  const VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = shader_object->frame_count,
      .pSetLayouts = layouts,
  };

  const VkResult result = vkAllocateDescriptorSets(
      state->device.logical_device, &descriptor_set_allocate_info, out_sets);
  if (out_result) {
    *out_result = result;
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return result == VK_SUCCESS;
}

vkr_internal bool8_t vulkan_shader_allocate_instance_descriptor_sets(
    VulkanBackendState *state, VulkanShaderObject *shader_object,
    VkDescriptorSet *out_sets, VkDescriptorPool *out_pool) {
  if (!state || !shader_object || !out_sets || !out_pool) {
    return false_v;
  }
  if (shader_object->instance_descriptor_pool_count == 0) {
    log_error("No instance descriptor pools available for allocation");
    return false_v;
  }

  for (uint32_t i = 0; i < shader_object->instance_descriptor_pool_count; ++i) {
    VkResult result = VK_SUCCESS;
    if (vulkan_shader_allocate_instance_sets_from_pool(
            state, shader_object, shader_object->instance_descriptor_pools[i],
            out_sets, &result)) {
      *out_pool = shader_object->instance_descriptor_pools[i];
      return true_v;
    }
    if (result != VK_ERROR_OUT_OF_POOL_MEMORY &&
        result != VK_ERROR_FRAGMENTED_POOL) {
      log_error("Descriptor set allocation failed with VkResult=%d", result);
      return false_v;
    }
  }

  if (shader_object->instance_descriptor_pool_count >=
      VULKAN_SHADER_OBJECT_MAX_INSTANCE_POOLS) {
    log_error("Descriptor pool overflow limit reached (%u)",
              VULKAN_SHADER_OBJECT_MAX_INSTANCE_POOLS);
    return false_v;
  }

  const VkrDescriptorSetDesc *draw_set_desc =
      vulkan_shader_reflection_find_set_by_index(&shader_object->reflection,
                                                 shader_object->draw_set_index);
  if (!draw_set_desc) {
    log_error("Draw set %u missing in reflection during overflow allocation",
              shader_object->draw_set_index);
    return false_v;
  }

  const uint32_t current_index =
      shader_object->instance_descriptor_pool_count - 1;
  const uint32_t current_capacity =
      shader_object->instance_pool_instance_capacities[current_index];
  const uint32_t new_capacity =
      Min(current_capacity * 2u, VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT);
  if (new_capacity <= current_capacity) {
    log_error("Cannot grow instance descriptor pool beyond %u instances",
              current_capacity);
    return false_v;
  }

  VkDescriptorPool overflow_pool = VK_NULL_HANDLE;
  if (!vulkan_shader_create_instance_descriptor_pool(
          state, draw_set_desc, shader_object->frame_count, new_capacity,
          &overflow_pool)) {
    log_error("Failed to create overflow descriptor pool (capacity=%u)",
              new_capacity);
    return false_v;
  }

  const uint32_t new_pool_index =
      shader_object->instance_descriptor_pool_count++;
  shader_object->instance_descriptor_pools[new_pool_index] = overflow_pool;
  shader_object->instance_pool_instance_capacities[new_pool_index] =
      new_capacity;
  shader_object->instance_pool_overflow_creations++;

  VkResult retry_result = VK_SUCCESS;
  if (!vulkan_shader_allocate_instance_sets_from_pool(
          state, shader_object, overflow_pool, out_sets, &retry_result)) {
    log_error("Overflow descriptor pool allocation failed with VkResult=%d",
              retry_result);
    return false_v;
  }

  shader_object->instance_pool_fallback_allocations++;
  *out_pool = overflow_pool;
  log_warn("Instance descriptor pool overflow fallback used (new capacity=%u, "
           "pools=%u)",
           new_capacity, shader_object->instance_descriptor_pool_count);
  return true_v;
}

bool8_t vulkan_shader_module_create(
    VulkanBackendState *state, VkrShaderStageFlags stage, const uint64_t size,
    const uint8_t *code, const String8 entry_point, VkShaderModule *out_shader,
    VkPipelineShaderStageCreateInfo *out_stage) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(out_shader != NULL, "Output shader module is NULL");
  assert_log(out_stage != NULL, "Output shader stage is NULL");

  if (size == 0 || code == NULL) {
    log_error("Invalid shader code: size is 0 or code is NULL");
    return false_v;
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

  // log_debug("Shader module created: %p", out_shader);

  return true;
}

void vulkan_shader_module_destroy(VulkanBackendState *state,
                                  VkShaderModule shader) {
  if (shader != VK_NULL_HANDLE) {
    // log_debug("Destroying shader module: %p", shader);

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
  assert_log(out_shader_object != NULL, "Output shader object is NULL");

  MemZero(out_shader_object, sizeof(*out_shader_object));
  VkrAllocator *arena_alloc = &state->alloc;

  if (desc->file_format != VKR_SHADER_FILE_FORMAT_SPIR_V) {
    log_error("Only SPIR-V shader file format is supported");
    return false_v;
  }

  if (desc->file_type == VKR_SHADER_FILE_TYPE_SINGLE) {
    const FilePath path =
        file_path_create(string8_cstr(&desc->modules[0].path), arena_alloc,
                         FILE_PATH_TYPE_RELATIVE);
    if (!file_exists(&path)) {
      log_fatal("Shader file does not exist: %s",
                string8_cstr(&desc->modules[0].path));
      return false_v;
    }

    uint8_t *shader_data = NULL;
    uint64_t shader_size = 0;
    FileError file_error =
        file_load_spirv_shader(&path, arena_alloc, &shader_data, &shader_size);
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
        vkr_allocator_free(arena_alloc, shader_data, shader_size,
                           VKR_ALLOCATOR_MEMORY_TAG_FILE);
        return false_v;
      }
    }
    // Free shader bytecode after all modules are created (Vulkan copies it)
    vkr_allocator_free(arena_alloc, shader_data, shader_size,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
  } else if (desc->file_type == VKR_SHADER_FILE_TYPE_MULTI) {
    // Load per-stage files
    for (uint32_t i = 0; i < VKR_SHADER_STAGE_COUNT; i++) {
      if (desc->modules[i].stages.set == 0)
        continue; // stage not provided

      const FilePath path =
          file_path_create(string8_cstr(&desc->modules[i].path), arena_alloc,
                           FILE_PATH_TYPE_RELATIVE);
      if (!file_exists(&path)) {
        log_fatal("Shader file does not exist: %s",
                  string8_cstr(&desc->modules[i].path));
        return false_v;
      }

      uint8_t *shader_data = NULL;
      uint64_t shader_size = 0;
      FileError file_error = file_load_spirv_shader(&path, arena_alloc,
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
        vkr_allocator_free(arena_alloc, shader_data, shader_size,
                           VKR_ALLOCATOR_MEMORY_TAG_FILE);
        return false_v;
      }
      // Free shader bytecode after module is created (Vulkan copies it)
      vkr_allocator_free(arena_alloc, shader_data, shader_size,
                         VKR_ALLOCATOR_MEMORY_TAG_FILE);
    }
  } else {
    log_error("Unknown shader file type");
    return false_v;
  }

  VulkanShaderReflectionInput reflection_input = {0};
  if (!vulkan_shader_collect_reflection_input(state, desc, &reflection_input)) {
    vulkan_shader_destroy_modules(state, out_shader_object);
    return false_v;
  }

  VkrReflectionErrorContext reflection_error = {0};
  const VkrSpirvReflectionCreateInfo reflection_create_info = {
      .allocator = arena_alloc,
      .temp_allocator = &state->temp_scope,
      .program_name = reflection_input.program_name,
      .vertex_abi_profile = desc->vertex_abi_profile,
      .module_count = reflection_input.module_count,
      .modules = reflection_input.modules,
      .max_push_constant_size =
          state->device.properties.limits.maxPushConstantsSize,
  };
  if (!vulkan_spirv_shader_reflection_create(&reflection_create_info,
                                             &out_shader_object->reflection,
                                             &reflection_error)) {
    vulkan_shader_log_reflection_error(&reflection_error);
    vulkan_shader_reflection_input_destroy(arena_alloc, &reflection_input);
    vulkan_shader_destroy_modules(state, out_shader_object);
    return false_v;
  }
  out_shader_object->has_reflection = true_v;
  vulkan_shader_log_reflection_layout_debug(reflection_input.program_name,
                                            &out_shader_object->reflection);
  vulkan_shader_reflection_input_destroy(arena_alloc, &reflection_input);

  if (!vulkan_shader_resolve_runtime_set_contract(
          &out_shader_object->reflection, out_shader_object)) {
    log_fatal("Failed to resolve reflected descriptor set contract");
    goto cleanup_reflection;
  }

  out_shader_object->frame_count = state->swapchain.image_count;

  const VkrDescriptorSetDesc *frame_set_desc =
      out_shader_object->frame_set_index == VKR_SHADER_REFLECTION_INDEX_INVALID
          ? NULL
          : vulkan_shader_reflection_find_set_by_index(
                &out_shader_object->reflection,
                out_shader_object->frame_set_index);
  const VkrDescriptorSetDesc *draw_set_desc =
      out_shader_object->draw_set_index == VKR_SHADER_REFLECTION_INDEX_INVALID
          ? NULL
          : vulkan_shader_reflection_find_set_by_index(
                &out_shader_object->reflection,
                out_shader_object->draw_set_index);
  const uint64_t min_uniform_alignment = Max(
      (uint64_t)state->device.properties.limits.minUniformBufferOffsetAlignment,
      1u);
  const uint64_t reflected_global_ubo_size =
      vulkan_shader_reflection_uniform_binding_size(
          frame_set_desc, out_shader_object->frame_uniform_binding);
  const uint64_t reflected_instance_ubo_size =
      vulkan_shader_reflection_uniform_binding_size(
          draw_set_desc, out_shader_object->draw_uniform_binding);
  const uint64_t reflected_push_constant_size =
      vulkan_shader_max_push_constant_end(&out_shader_object->reflection);
  uint32_t reflected_global_texture_count = 0;
  uint32_t reflected_instance_texture_count = 0;
  if (frame_set_desc) {
    const uint32_t frame_sampled_images =
        vulkan_shader_reflection_count_descriptors_of_type(
            frame_set_desc, vulkan_shader_descriptor_type_is_sampled_image);
    const uint32_t frame_samplers =
        vulkan_shader_reflection_count_descriptors_of_type(
            frame_set_desc, vulkan_shader_descriptor_type_is_sampler);
    reflected_global_texture_count = Min(frame_sampled_images, frame_samplers);
  }
  if (draw_set_desc) {
    const uint32_t draw_sampled_images =
        vulkan_shader_reflection_count_descriptors_of_type(
            draw_set_desc, vulkan_shader_descriptor_type_is_sampled_image);
    const uint32_t draw_samplers =
        vulkan_shader_reflection_count_descriptors_of_type(
            draw_set_desc, vulkan_shader_descriptor_type_is_sampler);
    reflected_instance_texture_count = Min(draw_sampled_images, draw_samplers);
  }

  if (reflected_global_ubo_size >
      state->device.properties.limits.maxUniformBufferRange) {
    log_fatal("Reflected frame UBO size exceeds maxUniformBufferRange (%llu > "
              "%u)",
              (unsigned long long)reflected_global_ubo_size,
              state->device.properties.limits.maxUniformBufferRange);
    goto cleanup_reflection;
  }
  if (reflected_instance_ubo_size >
      state->device.properties.limits.maxUniformBufferRange) {
    log_fatal("Reflected draw UBO size exceeds maxUniformBufferRange (%llu > "
              "%u)",
              (unsigned long long)reflected_instance_ubo_size,
              state->device.properties.limits.maxUniformBufferRange);
    goto cleanup_reflection;
  }
  if (reflected_push_constant_size >
      state->device.properties.limits.maxPushConstantsSize) {
    log_fatal("Reflected push constant size exceeds device limit (%llu > %u)",
              (unsigned long long)reflected_push_constant_size,
              state->device.properties.limits.maxPushConstantsSize);
    goto cleanup_reflection;
  }
  if (reflected_instance_texture_count > VKR_MAX_INSTANCE_TEXTURES) {
    log_fatal("Reflected draw texture slot count exceeds engine capacity (%u > "
              "%u)",
              reflected_instance_texture_count, VKR_MAX_INSTANCE_TEXTURES);
    goto cleanup_reflection;
  }
  if (reflected_instance_texture_count > 0) {
    if (!vulkan_shader_validate_linear_binding_slots(
            draw_set_desc, out_shader_object->draw_sampled_image_binding_base,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            reflected_instance_texture_count)) {
      log_fatal("Draw sampled-image bindings must be contiguous, single-slot "
                "bindings for %u slots",
                reflected_instance_texture_count);
      goto cleanup_reflection;
    }
    if (!vulkan_shader_validate_linear_binding_slots(
            draw_set_desc, out_shader_object->draw_sampler_binding_base,
            VK_DESCRIPTOR_TYPE_SAMPLER, reflected_instance_texture_count)) {
      log_fatal("Draw sampler bindings must be contiguous, single-slot "
                "bindings for %u slots",
                reflected_instance_texture_count);
      goto cleanup_reflection;
    }
  }
  if (out_shader_object->frame_uniform_binding !=
          VKR_SHADER_REFLECTION_INDEX_INVALID &&
      reflected_global_ubo_size == 0) {
    log_fatal("Reflected frame UBO binding %u has zero byte size",
              out_shader_object->frame_uniform_binding);
    goto cleanup_reflection;
  }
  if (out_shader_object->draw_uniform_binding !=
          VKR_SHADER_REFLECTION_INDEX_INVALID &&
      reflected_instance_ubo_size == 0) {
    log_fatal("Reflected draw UBO binding %u has zero byte size",
              out_shader_object->draw_uniform_binding);
    goto cleanup_reflection;
  }

  out_shader_object->global_ubo_size = reflected_global_ubo_size;
  out_shader_object->global_ubo_stride =
      reflected_global_ubo_size > 0
          ? vulkan_shader_align_up_u64(reflected_global_ubo_size,
                                       min_uniform_alignment)
          : 0;
  out_shader_object->instance_ubo_size = reflected_instance_ubo_size;
  out_shader_object->instance_ubo_stride =
      reflected_instance_ubo_size > 0
          ? vulkan_shader_align_up_u64(reflected_instance_ubo_size,
                                       min_uniform_alignment)
          : 0;
  out_shader_object->push_constant_size = reflected_push_constant_size;
  out_shader_object->global_texture_count = reflected_global_texture_count;
  out_shader_object->instance_texture_count = reflected_instance_texture_count;
  out_shader_object->instance_descriptor_pool_count = 0;
  out_shader_object->instance_pool_fallback_allocations = 0;
  out_shader_object->instance_pool_overflow_creations = 0;

  if (frame_set_desc) {
    if (!vulkan_shader_create_set_layout_from_reflection(
            state, frame_set_desc,
            &out_shader_object->global_descriptor_set_layout)) {
      log_fatal("Failed to create reflected frame descriptor set layout");
      goto cleanup_reflection;
    }

    VulkanDescriptorPoolTypeCount
        frame_pool_counts[VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX];
    MemZero(frame_pool_counts, sizeof(frame_pool_counts));
    uint32_t frame_pool_count = 0;
    for (uint32_t i = 0; i < frame_set_desc->binding_count; ++i) {
      const VkrDescriptorBindingDesc *binding = &frame_set_desc->bindings[i];
      if (!vulkan_shader_pool_type_count_add(
              frame_pool_counts, &frame_pool_count,
              VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX, binding->type,
              binding->count * out_shader_object->frame_count)) {
        log_fatal("Frame descriptor pool type table overflow");
        goto cleanup_reflection;
      }
    }

    VkDescriptorPoolSize pool_sizes[VKR_SHADER_DESCRIPTOR_TYPE_BUCKET_MAX];
    for (uint32_t i = 0; i < frame_pool_count; ++i) {
      pool_sizes[i] = (VkDescriptorPoolSize){
          .type = frame_pool_counts[i].type,
          .descriptorCount = frame_pool_counts[i].count,
      };
    }

    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = out_shader_object->frame_count,
        .poolSizeCount = frame_pool_count,
        .pPoolSizes = frame_pool_count > 0 ? pool_sizes : NULL,
    };
    if (vkCreateDescriptorPool(
            state->device.logical_device, &pool_info, state->allocator,
            &out_shader_object->global_descriptor_pool) != VK_SUCCESS) {
      log_fatal("Failed to create frame descriptor pool");
      goto cleanup_reflection;
    }

    VkrAllocatorScope temp_scope =
        vkr_allocator_begin_scope(&state->temp_scope);
    if (!vkr_allocator_scope_is_valid(&temp_scope)) {
      log_fatal("Failed to acquire temporary allocator for frame descriptors");
      goto cleanup_reflection;
    }

    VkDescriptorSetLayout *layouts =
        (VkDescriptorSetLayout *)vkr_allocator_alloc(
            &state->temp_scope,
            sizeof(VkDescriptorSetLayout) * out_shader_object->frame_count,
            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!layouts) {
      log_fatal("Failed to allocate temporary frame descriptor layouts");
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      goto cleanup_reflection;
    }
    for (uint32_t i = 0; i < out_shader_object->frame_count; ++i) {
      layouts[i] = out_shader_object->global_descriptor_set_layout;
    }

    out_shader_object->global_descriptor_sets =
        (VkDescriptorSet *)vkr_allocator_alloc(
            arena_alloc,
            sizeof(VkDescriptorSet) * out_shader_object->frame_count,
            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    out_shader_object->global_descriptor_generations = vkr_allocator_alloc(
        arena_alloc, sizeof(uint32_t) * out_shader_object->frame_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    out_shader_object->global_descriptor_instance_buffers = vkr_allocator_alloc(
        arena_alloc, sizeof(VkBuffer) * out_shader_object->frame_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!out_shader_object->global_descriptor_sets ||
        !out_shader_object->global_descriptor_generations ||
        !out_shader_object->global_descriptor_instance_buffers) {
      log_fatal("Failed to allocate frame descriptor tracking buffers");
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      goto cleanup_reflection;
    }

    const VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = out_shader_object->global_descriptor_pool,
        .descriptorSetCount = out_shader_object->frame_count,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(state->device.logical_device, &allocate_info,
                                 out_shader_object->global_descriptor_sets) !=
        VK_SUCCESS) {
      log_fatal("Failed to allocate frame descriptor sets");
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      goto cleanup_reflection;
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    for (uint32_t i = 0; i < out_shader_object->frame_count; ++i) {
      out_shader_object->global_descriptor_generations[i] = VKR_INVALID_ID;
      out_shader_object->global_descriptor_instance_buffers[i] = VK_NULL_HANDLE;
    }
  }

  VkrBufferTypeFlags buffer_type = bitset8_create();
  bitset8_set(&buffer_type, VKR_BUFFER_TYPE_GRAPHICS);

  const bool8_t has_global_ubo = out_shader_object->frame_uniform_binding !=
                                     VKR_SHADER_REFLECTION_INDEX_INVALID &&
                                 out_shader_object->global_ubo_stride > 0;
  if (has_global_ubo) {
    VkrBufferDescription global_uniform_buffer_desc = {
        .size = out_shader_object->global_ubo_stride *
                out_shader_object->frame_count,
        .usage = vkr_buffer_usage_flags_from_bits(
            VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER |
            VKR_BUFFER_USAGE_TRANSFER_DST | VKR_BUFFER_USAGE_TRANSFER_SRC),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_DEVICE_LOCAL |
            VKR_MEMORY_PROPERTY_HOST_VISIBLE |
            VKR_MEMORY_PROPERTY_HOST_COHERENT),
        .buffer_type = buffer_type,
        .bind_on_create = true_v};

    if (!vulkan_buffer_create(state, &global_uniform_buffer_desc,
                              &out_shader_object->global_uniform_buffer)) {
      log_fatal("Failed to create Vulkan global uniform buffer");
      goto cleanup_reflection;
    }
  } else {
    MemZero(&out_shader_object->global_uniform_buffer,
            sizeof(out_shader_object->global_uniform_buffer));
  }

  if (draw_set_desc) {
    if (draw_set_desc->binding_count >
        VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT) {
      log_fatal("Draw set %u exceeds descriptor state capacity (%u > %u)",
                draw_set_desc->set, draw_set_desc->binding_count,
                VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT);
      goto cleanup_reflection;
    }
    for (uint32_t i = 0; i < draw_set_desc->binding_count; ++i) {
      if (draw_set_desc->bindings[i].binding >=
          VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT) {
        log_fatal(
            "Draw set %u binding index %u exceeds descriptor state capacity %u",
            draw_set_desc->set, draw_set_desc->bindings[i].binding,
            VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT);
        goto cleanup_reflection;
      }
    }

    if (!vulkan_shader_create_set_layout_from_reflection(
            state, draw_set_desc,
            &out_shader_object->instance_descriptor_set_layout)) {
      log_fatal("Failed to create reflected draw descriptor set layout");
      goto cleanup_reflection;
    }

    uint32_t initial_instance_capacity =
        vulkan_shader_default_set_allocation_count(
            draw_set_desc->role, state->swapchain.image_count);
    initial_instance_capacity = Min(initial_instance_capacity,
                                    VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT);
    if (initial_instance_capacity == 0) {
      initial_instance_capacity = 1;
    }

    VkDescriptorPool primary_pool = VK_NULL_HANDLE;
    if (!vulkan_shader_create_instance_descriptor_pool(
            state, draw_set_desc, out_shader_object->frame_count,
            initial_instance_capacity, &primary_pool)) {
      log_fatal("Failed to create initial draw descriptor pool");
      goto cleanup_reflection;
    }

    out_shader_object->instance_descriptor_pool = primary_pool;
    out_shader_object->instance_descriptor_pools[0] = primary_pool;
    out_shader_object->instance_pool_instance_capacities[0] =
        initial_instance_capacity;
    out_shader_object->instance_descriptor_pool_count = 1;
  }

  const bool8_t has_instance_ubo = out_shader_object->draw_uniform_binding !=
                                       VKR_SHADER_REFLECTION_INDEX_INVALID &&
                                   out_shader_object->instance_ubo_stride > 0;
  if (has_instance_ubo) {
    VkrBufferDescription instance_uniform_buffer_desc = {
        .size = out_shader_object->instance_ubo_stride *
                VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
        .usage = vkr_buffer_usage_flags_from_bits(
            VKR_BUFFER_USAGE_TRANSFER_DST | VKR_BUFFER_USAGE_TRANSFER_SRC |
            VKR_BUFFER_USAGE_UNIFORM),
        .memory_properties = vkr_memory_property_flags_from_bits(
            VKR_MEMORY_PROPERTY_DEVICE_LOCAL |
            VKR_MEMORY_PROPERTY_HOST_VISIBLE |
            VKR_MEMORY_PROPERTY_HOST_COHERENT),
        .buffer_type = buffer_type,
        .bind_on_create = true_v};

    if (!vulkan_buffer_create(state, &instance_uniform_buffer_desc,
                              &out_shader_object->instance_uniform_buffer)) {
      log_fatal("Failed to create Vulkan instance uniform buffer");
      goto cleanup_reflection;
    }
  } else {
    MemZero(&out_shader_object->instance_uniform_buffer,
            sizeof(out_shader_object->instance_uniform_buffer));
  }

  // Initialize free list for instance states
  out_shader_object->instance_uniform_buffer_count = 0;
  out_shader_object->instance_state_free_count = 0;

  return true;

cleanup_reflection:
  if (out_shader_object->has_reflection) {
    vulkan_spirv_shader_reflection_destroy(&state->alloc,
                                           &out_shader_object->reflection);
    out_shader_object->has_reflection = false_v;
  }
  vulkan_shader_destroy_modules(state, out_shader_object);
  return false_v;
}

bool8_t vulkan_shader_update_global_state(VulkanBackendState *state,
                                          VulkanShaderObject *shader_object,
                                          VkPipelineLayout pipeline_layout,
                                          const void *uniform) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(pipeline_layout != VK_NULL_HANDLE, "Pipeline layout is NULL");
  if (!shader_object->has_reflection ||
      shader_object->frame_set_index == VKR_SHADER_REFLECTION_INDEX_INVALID) {
    return true_v;
  }

  const VkrDescriptorSetDesc *frame_set_desc =
      vulkan_shader_reflection_find_set_by_index(
          &shader_object->reflection, shader_object->frame_set_index);
  if (!frame_set_desc) {
    log_error("Frame set index %u is not present in reflection",
              shader_object->frame_set_index);
    return false_v;
  }

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);
  if (!shader_object->global_descriptor_sets ||
      image_index >= shader_object->frame_count) {
    log_error("Frame descriptor set is unavailable for image index %u",
              image_index);
    return false_v;
  }

  const bool8_t has_global_uniform_binding =
      shader_object->frame_uniform_binding !=
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  const VkrDescriptorBindingDesc *frame_uniform_desc =
      has_global_uniform_binding
          ? vulkan_shader_reflection_find_binding(
                frame_set_desc, shader_object->frame_uniform_binding)
          : NULL;
  const bool8_t has_global_uniform = uniform != NULL && frame_uniform_desc &&
                                     shader_object->global_ubo_size > 0;
  if (has_global_uniform && shader_object->global_ubo_size > 0) {
    if (shader_object->global_uniform_buffer.buffer.handle == VK_NULL_HANDLE) {
      log_warn("Global uniform buffer not created yet, skipping descriptor set "
               "update");
      return false_v;
    }
    uint64_t global_offset =
        shader_object->global_ubo_stride * (uint64_t)image_index;
    if (!vulkan_buffer_load_data(
            state, &shader_object->global_uniform_buffer.buffer, global_offset,
            shader_object->global_ubo_size, 0, uniform)) {
      log_error("Failed to load global uniform buffer data");
      return false_v;
    }
  }

  const bool8_t has_instance_storage_binding =
      shader_object->frame_instance_buffer_binding !=
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  const VkrDescriptorBindingDesc *frame_storage_desc =
      has_instance_storage_binding
          ? vulkan_shader_reflection_find_binding(
                frame_set_desc, shader_object->frame_instance_buffer_binding)
          : NULL;

  struct s_BufferHandle *instance_buffer = state->instance_buffer;
  if (has_instance_storage_binding &&
      (!instance_buffer || instance_buffer->buffer.handle == VK_NULL_HANDLE)) {
    log_error("Instance buffer not set for global descriptor binding");
    return false_v;
  }

  bool8_t needs_descriptor_update =
      shader_object->global_descriptor_generations &&
      shader_object->global_descriptor_generations[image_index] ==
          VKR_INVALID_ID;
  if (has_instance_storage_binding &&
      shader_object->global_descriptor_instance_buffers &&
      shader_object->global_descriptor_instance_buffers[image_index] !=
          instance_buffer->buffer.handle) {
    needs_descriptor_update = true_v;
  }

  if (needs_descriptor_update) {
    VkWriteDescriptorSet descriptor_writes[2];
    VkDescriptorBufferInfo buffer_infos[2];
    MemZero(descriptor_writes, sizeof(descriptor_writes));
    MemZero(buffer_infos, sizeof(buffer_infos));

    uint32_t write_count = 0;
    if (has_global_uniform) {
      if (!vulkan_shader_validate_descriptor_write(
              &shader_object->reflection, shader_object->frame_set_index,
              shader_object->frame_uniform_binding, frame_uniform_desc->type, 0,
              1)) {
        return false_v;
      }
      uint64_t global_offset =
          shader_object->global_ubo_stride * (uint64_t)image_index;
      buffer_infos[write_count] = (VkDescriptorBufferInfo){
          .buffer = shader_object->global_uniform_buffer.buffer.handle,
          .offset = global_offset,
          .range = shader_object->global_ubo_size,
      };
      descriptor_writes[write_count] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = shader_object->global_descriptor_sets[image_index],
          .dstBinding = shader_object->frame_uniform_binding,
          .dstArrayElement = 0,
          .descriptorType = frame_uniform_desc->type,
          .descriptorCount = 1,
          .pBufferInfo = &buffer_infos[write_count],
      };
      write_count++;
    }

    if (has_instance_storage_binding && frame_storage_desc) {
      if (!vulkan_shader_validate_descriptor_write(
              &shader_object->reflection, shader_object->frame_set_index,
              shader_object->frame_instance_buffer_binding,
              frame_storage_desc->type, 0, 1)) {
        return false_v;
      }
      buffer_infos[write_count] = (VkDescriptorBufferInfo){
          .buffer = instance_buffer->buffer.handle,
          .offset = 0,
          .range = instance_buffer->description.size,
      };
      descriptor_writes[write_count] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = shader_object->global_descriptor_sets[image_index],
          .dstBinding = shader_object->frame_instance_buffer_binding,
          .dstArrayElement = 0,
          .descriptorType = frame_storage_desc->type,
          .descriptorCount = 1,
          .pBufferInfo = &buffer_infos[write_count],
      };
      write_count++;
    }

    if (write_count > 0) {
      vkUpdateDescriptorSets(state->device.logical_device, write_count,
                             descriptor_writes, 0, NULL);
    }
    shader_object->global_descriptor_generations[image_index] = 1;
    if (has_instance_storage_binding &&
        shader_object->global_descriptor_instance_buffers) {
      shader_object->global_descriptor_instance_buffers[image_index] =
          instance_buffer->buffer.handle;
    }
  }

  VkDescriptorSet global_descriptor =
      shader_object->global_descriptor_sets[image_index];
  if (command_buffer->bound_global_descriptor_set != global_descriptor ||
      command_buffer->bound_global_pipeline_layout != pipeline_layout) {
    uint32_t dynamic_offsets[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
    MemZero(dynamic_offsets, sizeof(dynamic_offsets));
    if (!vulkan_shader_bind_descriptor_set_checked(
            command_buffer->handle, pipeline_layout,
            shader_object->frame_set_index, global_descriptor,
            shader_object->frame_dynamic_offset_count,
            shader_object->frame_dynamic_offset_count, dynamic_offsets)) {
      return false_v;
    }
    command_buffer->bound_global_descriptor_set = global_descriptor;
    command_buffer->bound_global_pipeline_layout = pipeline_layout;
  }

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

  if (!shader_object->has_reflection) {
    log_error("Shader object has no reflection data");
    return false_v;
  }

  uint32_t image_index = state->image_index;
  VulkanCommandBuffer *command_buffer = array_get_VulkanCommandBuffer(
      &state->graphics_command_buffers, image_index);

  if (shader_object->reflection.push_constant_range_count > 0) {
    if (!data->push_constants_data || data->push_constants_size == 0) {
      log_warn("Push constants required but no data provided");
    } else {
      for (uint32_t i = 0;
           i < shader_object->reflection.push_constant_range_count; ++i) {
        const VkrPushConstantRangeDesc *range =
            &shader_object->reflection.push_constant_ranges[i];
        if (data->push_constants_size <= range->offset) {
          continue;
        }
        uint32_t remaining = data->push_constants_size - range->offset;
        uint32_t write_size = Min(range->size, remaining);
        if (write_size == 0) {
          continue;
        }
        vkCmdPushConstants(command_buffer->handle, pipeline_layout,
                           range->stages, range->offset, write_size,
                           ((const uint8_t *)data->push_constants_data) +
                               range->offset);
      }
    }
  }

  const bool8_t has_instance_descriptors =
      shader_object->draw_set_index != VKR_SHADER_REFLECTION_INDEX_INVALID &&
      shader_object->instance_descriptor_set_layout != VK_NULL_HANDLE;
  if (!has_instance_descriptors) {
    return true_v;
  }
  const VkrDescriptorSetDesc *draw_set_desc =
      vulkan_shader_reflection_find_set_by_index(&shader_object->reflection,
                                                 shader_object->draw_set_index);
  if (!draw_set_desc) {
    log_error("Draw set index %u missing from reflection",
              shader_object->draw_set_index);
    return false_v;
  }

  // If no valid instance state, push constants were sufficient
  if (data->instance_state.id == VKR_INVALID_ID) {
    return true_v;
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

  uint32_t descriptor_count = 0;

  const bool8_t has_instance_ubo_binding =
      shader_object->draw_uniform_binding !=
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  const VkrDescriptorBindingDesc *draw_uniform_desc =
      has_instance_ubo_binding
          ? vulkan_shader_reflection_find_binding(
                draw_set_desc, shader_object->draw_uniform_binding)
          : NULL;
  uint32_t range = (uint32_t)shader_object->instance_ubo_size;
  uint64_t offset =
      shader_object->instance_ubo_stride * (uint64_t)data->instance_state.id;
  const bool8_t has_instance_ubo_buffer =
      has_instance_ubo_binding && draw_uniform_desc &&
      shader_object->instance_uniform_buffer.buffer.handle != VK_NULL_HANDLE &&
      shader_object->instance_ubo_stride > 0;

  if (has_instance_ubo_buffer) {
    if (!data->instance_ubo_data || data->instance_ubo_size == 0) {
      log_warn("Instance UBO required but no data provided");
    } else {
      uint32_t use = data->instance_ubo_size;
      if (use > range)
        use = range;
      if (!vulkan_buffer_load_data(
              state, &shader_object->instance_uniform_buffer.buffer, offset,
              use, 0, data->instance_ubo_data)) {
        log_error("Failed to load instance uniform buffer data (raw)");
        return false;
      }
    }

    uint32_t state_slot = 0;
    if (!vulkan_shader_descriptor_state_index_from_binding(
            shader_object->draw_uniform_binding, &state_slot)) {
      log_error("Draw uniform binding %u exceeds descriptor state capacity",
                shader_object->draw_uniform_binding);
      return false_v;
    }

    if (instance_state->descriptor_states[state_slot]
            .generations[image_index] == VKR_INVALID_ID) {
      if (!vulkan_shader_validate_descriptor_write(
              &shader_object->reflection, shader_object->draw_set_index,
              shader_object->draw_uniform_binding, draw_uniform_desc->type, 0,
              1)) {
        return false_v;
      }
      buffer_infos[descriptor_count] = (VkDescriptorBufferInfo){
          .buffer = shader_object->instance_uniform_buffer.buffer.handle,
          .offset = offset,
          .range = range,
      };

      descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = local_descriptor,
          .dstBinding = shader_object->draw_uniform_binding,
          .descriptorType = draw_uniform_desc->type,
          .descriptorCount = 1,
          .pBufferInfo = &buffer_infos[descriptor_count],
          .pImageInfo = NULL,
          .pTexelBufferView = NULL,
      };

      descriptor_count++;

      instance_state->descriptor_states[state_slot].generations[image_index] =
          1;
    }
  }

  const uint32_t sampler_count = shader_object->instance_texture_count;
  const bool8_t has_sampled_images =
      shader_object->draw_sampled_image_binding_base !=
      VKR_SHADER_REFLECTION_INDEX_INVALID;
  const bool8_t has_samplers = shader_object->draw_sampler_binding_base !=
                               VKR_SHADER_REFLECTION_INDEX_INVALID;
  if ((sampler_count > 0) && (!has_sampled_images || !has_samplers)) {
    log_warn("Material textures requested (%u) but draw set lacks sampled "
             "image/sampler "
             "bindings",
             sampler_count);
  }

  for (uint32_t sampler_index = 0; sampler_index < sampler_count;
       sampler_index++) {
    const bool8_t needs_image_view = has_sampled_images;
    const bool8_t needs_sampler = has_samplers;
    struct s_TextureHandle *texture = NULL;
    if (material && sampler_index < material->texture_count &&
        material->textures_enabled[sampler_index]) {
      texture = (struct s_TextureHandle *)material->textures[sampler_index];
    }
    if (!vulkan_shader_texture_ready_for_descriptors(texture, needs_image_view,
                                                     needs_sampler)) {
      // No texture bound. Use default placeholder to avoid stale cubemap
      // bindings in descriptor sets after scene reload.
      texture = state->default_2d_texture;
    }
    if (!vulkan_shader_texture_ready_for_descriptors(texture, needs_image_view,
                                                     needs_sampler)) {
      log_error(
          "Descriptor update failed: no valid fallback texture is available");
      return false_v;
    }

    VulkanTexture *texture_object = &texture->texture;

    if (has_sampled_images) {
      uint32_t binding_image =
          shader_object->draw_sampled_image_binding_base + sampler_index;
      uint32_t image_state_slot = 0;
      if (!vulkan_shader_descriptor_state_index_from_binding(
              binding_image, &image_state_slot)) {
        log_error("Image binding %u exceeds descriptor state capacity",
                  binding_image);
        return false_v;
      }
      uint32_t *image_desc_generation =
          &instance_state->descriptor_states[image_state_slot]
               .generations[image_index];
      VkImageView *image_desc_view =
          &instance_state->descriptor_states[image_state_slot]
               .image_views[image_index];

      if (*image_desc_generation != texture->description.generation ||
          *image_desc_generation == VKR_INVALID_ID ||
          *image_desc_view != texture_object->image.view) {
        if (!vulkan_shader_validate_descriptor_write(
                &shader_object->reflection, shader_object->draw_set_index,
                binding_image, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0, 1)) {
          return false_v;
        }
        VkImageLayout image_layout =
            vulkan_shader_image_layout_for_texture(texture);
        image_infos[descriptor_count] = (VkDescriptorImageInfo){
            .sampler = VK_NULL_HANDLE,
            .imageView = texture_object->image.view,
            .imageLayout = image_layout,
        };

        descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = local_descriptor,
            .dstBinding = binding_image,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[descriptor_count],
        };

        descriptor_count++;
        *image_desc_generation = texture->description.generation;
        *image_desc_view = texture_object->image.view;
      }
    }

    if (has_samplers) {
      uint32_t binding_sampler =
          shader_object->draw_sampler_binding_base + sampler_index;
      uint32_t sampler_state_slot = 0;
      if (!vulkan_shader_descriptor_state_index_from_binding(
              binding_sampler, &sampler_state_slot)) {
        log_error("Sampler binding %u exceeds descriptor state capacity",
                  binding_sampler);
        return false_v;
      }
      uint32_t *sampler_desc_generation =
          &instance_state->descriptor_states[sampler_state_slot]
               .generations[image_index];
      VkSampler *sampler_desc_handle =
          &instance_state->descriptor_states[sampler_state_slot]
               .samplers[image_index];

      if (*sampler_desc_generation != texture->description.generation ||
          *sampler_desc_generation == VKR_INVALID_ID ||
          *sampler_desc_handle != texture_object->sampler) {
        if (!vulkan_shader_validate_descriptor_write(
                &shader_object->reflection, shader_object->draw_set_index,
                binding_sampler, VK_DESCRIPTOR_TYPE_SAMPLER, 0, 1)) {
          return false_v;
        }
        image_infos[descriptor_count] = (VkDescriptorImageInfo){
            .sampler = texture_object->sampler,
            .imageView = VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        descriptor_writes[descriptor_count] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = local_descriptor,
            .dstBinding = binding_sampler,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_infos[descriptor_count],
        };

        descriptor_count++;
        *sampler_desc_generation = texture->description.generation;
        *sampler_desc_handle = texture_object->sampler;
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

  uint32_t dynamic_offsets[VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT];
  MemZero(dynamic_offsets, sizeof(dynamic_offsets));
  if (!vulkan_shader_bind_descriptor_set_checked(
          command_buffer->handle, pipeline_layout,
          shader_object->draw_set_index, local_descriptor,
          shader_object->draw_dynamic_offset_count,
          shader_object->draw_dynamic_offset_count, dynamic_offsets)) {
    return false_v;
  }

  return true_v;
}

/**
 * @brief Finish releasing a shader instance once GPU work is complete.
 *
 * This is called only when the instance is guaranteed not to be referenced
 * by in-flight command buffers.
 */
vkr_internal bool8_t vulkan_shader_release_instance_immediate(
    VulkanBackendState *state, VulkanShaderObject *shader_object,
    uint32_t object_id) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");

  VulkanShaderObjectInstanceState *local_state =
      &shader_object->instance_states[object_id];

  const VkDescriptorPool descriptor_pool = local_state->descriptor_pool;
  if (local_state->descriptor_sets && descriptor_pool != VK_NULL_HANDLE) {
    if (vkFreeDescriptorSets(state->device.logical_device, descriptor_pool,
                             shader_object->frame_count,
                             local_state->descriptor_sets) != VK_SUCCESS) {
      log_error("Failed to free descriptor sets");
      return false_v;
    }
  }

  // Reset generation tracking back to invalid without freeing memory
  for (uint32_t descriptor_state_idx = 0;
       descriptor_state_idx < VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT;
       descriptor_state_idx++) {
    if (!local_state->descriptor_states[descriptor_state_idx].generations) {
      continue;
    }
    for (uint32_t descriptor_generation_idx = 0;
         descriptor_generation_idx < shader_object->frame_count;
         descriptor_generation_idx++) {
      local_state->descriptor_states[descriptor_state_idx]
          .generations[descriptor_generation_idx] = VKR_INVALID_ID;
      if (local_state->descriptor_states[descriptor_state_idx].image_views) {
        local_state->descriptor_states[descriptor_state_idx]
            .image_views[descriptor_generation_idx] = VK_NULL_HANDLE;
      }
      if (local_state->descriptor_states[descriptor_state_idx].samplers) {
        local_state->descriptor_states[descriptor_state_idx]
            .samplers[descriptor_generation_idx] = VK_NULL_HANDLE;
      }
    }
  }

  local_state->release_pending = false_v;
  local_state->release_serial = 0;
  local_state->descriptor_pool = VK_NULL_HANDLE;

  // Push id to free list for reuse
  assert_log(shader_object->instance_state_free_count <
                 VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT,
             "instance_state_free_ids overflow");
  shader_object
      ->instance_state_free_ids[shader_object->instance_state_free_count++] =
      object_id;

  return true_v;
}

/**
 * @brief Process deferred instance releases whose GPU work has completed.
 */
vkr_internal void
vulkan_shader_process_pending_releases(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object) {
  if (!state || !shader_object || shader_object->pending_release_count == 0) {
    return;
  }

  // Descriptor sets must not be freed while command buffers are recording.
  if (state->frame_active) {
    return;
  }

  uint64_t safe_serial = state->submit_serial >= BUFFERING_FRAMES
                             ? state->submit_serial - BUFFERING_FRAMES
                             : 0;

  uint32_t i = 0;
  while (i < shader_object->pending_release_count) {
    uint32_t object_id = shader_object->pending_release_ids[i];
    if (object_id >= shader_object->instance_uniform_buffer_count) {
      shader_object->pending_release_ids[i] =
          shader_object
              ->pending_release_ids[--shader_object->pending_release_count];
      continue;
    }

    VulkanShaderObjectInstanceState *local_state =
        &shader_object->instance_states[object_id];
    if (!local_state->release_pending) {
      shader_object->pending_release_ids[i] =
          shader_object
              ->pending_release_ids[--shader_object->pending_release_count];
      continue;
    }

    if (local_state->release_serial > safe_serial) {
      i++;
      continue;
    }

    if (vulkan_shader_release_instance_immediate(state, shader_object,
                                                 object_id)) {
      shader_object->pending_release_ids[i] =
          shader_object
              ->pending_release_ids[--shader_object->pending_release_count];
      continue;
    }

    i++;
  }
}

bool8_t vulkan_shader_acquire_instance(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t *out_object_id) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  assert_log(out_object_id != NULL, "Out object id is NULL");

  vulkan_shader_process_pending_releases(state, shader_object);

  if (shader_object->instance_state_free_count > 0) {
    shader_object->instance_state_free_count--;
    *out_object_id =
        shader_object
            ->instance_state_free_ids[shader_object->instance_state_free_count];
  } else {
    if (shader_object->instance_uniform_buffer_count >=
        VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT) {
      log_error(
          "Failed to acquire shader instance: limit (%u) exceeded for shader",
          VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT);
      return false_v;
    }
    *out_object_id = shader_object->instance_uniform_buffer_count;
    shader_object->instance_uniform_buffer_count++;
  }

  uint32_t object_id = *out_object_id;
  VulkanShaderObjectInstanceState *local_state =
      &shader_object->instance_states[object_id];
  local_state->descriptor_pool = VK_NULL_HANDLE;
  VkrAllocator *arena_alloc = &state->alloc;
  for (uint32_t descriptor_state_idx = 0;
       descriptor_state_idx < VULKAN_SHADER_OBJECT_DESCRIPTOR_STATE_COUNT;
       descriptor_state_idx++) {
    // Allocate per-frame generations once; instance state ids are reused via a
    // free list, so re-allocating here would leak arena-backed memory.
    if (!local_state->descriptor_states[descriptor_state_idx].generations) {
      local_state->descriptor_states[descriptor_state_idx].generations =
          vkr_allocator_alloc(arena_alloc,
                              sizeof(uint32_t) * shader_object->frame_count,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!local_state->descriptor_states[descriptor_state_idx].generations) {
        log_error("Failed to allocate descriptor generation tracking");
        return false_v;
      }
      for (uint32_t descriptor_generation_idx = 0;
           descriptor_generation_idx < shader_object->frame_count;
           descriptor_generation_idx++) {
        local_state->descriptor_states[descriptor_state_idx]
            .generations[descriptor_generation_idx] = VKR_INVALID_ID;
      }
    }

    if (!local_state->descriptor_states[descriptor_state_idx].image_views) {
      local_state->descriptor_states[descriptor_state_idx].image_views =
          vkr_allocator_alloc(arena_alloc,
                              sizeof(VkImageView) * shader_object->frame_count,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!local_state->descriptor_states[descriptor_state_idx].image_views) {
        log_error("Failed to allocate descriptor image view tracking");
        return false_v;
      }
      for (uint32_t i = 0; i < shader_object->frame_count; ++i) {
        local_state->descriptor_states[descriptor_state_idx].image_views[i] =
            VK_NULL_HANDLE;
      }
    }

    if (!local_state->descriptor_states[descriptor_state_idx].samplers) {
      local_state->descriptor_states[descriptor_state_idx].samplers =
          vkr_allocator_alloc(arena_alloc,
                              sizeof(VkSampler) * shader_object->frame_count,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!local_state->descriptor_states[descriptor_state_idx].samplers) {
        log_error("Failed to allocate descriptor sampler tracking");
        return false_v;
      }
      for (uint32_t i = 0; i < shader_object->frame_count; ++i) {
        local_state->descriptor_states[descriptor_state_idx].samplers[i] =
            VK_NULL_HANDLE;
      }
    }
  }

  // Allocate per-frame instance descriptor sets buffer once; Vulkan descriptor
  // sets themselves are allocated/freed per acquire/release.
  if (!local_state->descriptor_sets) {
    local_state->descriptor_sets = (VkDescriptorSet *)vkr_allocator_alloc(
        arena_alloc, sizeof(VkDescriptorSet) * shader_object->frame_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!local_state->descriptor_sets) {
      log_error("Failed to allocate instance descriptor set handles");
      return false_v;
    }
    MemZero(local_state->descriptor_sets,
            sizeof(VkDescriptorSet) * shader_object->frame_count);
  }

  if (shader_object->instance_descriptor_set_layout == VK_NULL_HANDLE ||
      shader_object->instance_descriptor_pool == VK_NULL_HANDLE) {
    local_state->descriptor_pool = VK_NULL_HANDLE;
    return true_v;
  }

  VkDescriptorPool allocated_pool = VK_NULL_HANDLE;
  if (!vulkan_shader_allocate_instance_descriptor_sets(
          state, shader_object, local_state->descriptor_sets,
          &allocated_pool)) {
    log_error("Failed to allocate descriptor sets for instance");
    return false_v;
  }
  local_state->descriptor_pool = allocated_pool;
  return true_v;
}

bool8_t vulkan_shader_release_instance(VulkanBackendState *state,
                                       VulkanShaderObject *shader_object,
                                       uint32_t object_id) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(shader_object != NULL, "Shader object is NULL");
  if (object_id >= shader_object->instance_uniform_buffer_count) {
    log_error("Shader instance release out of bounds: id=%u (count=%u)",
              object_id, shader_object->instance_uniform_buffer_count);
    return false_v;
  }

  VulkanShaderObjectInstanceState *local_state =
      &shader_object->instance_states[object_id];

  if (local_state->release_pending) {
    return true_v;
  }

  if (shader_object->pending_release_count >=
      VULKAN_SHADER_OBJECT_INSTANCE_STATE_COUNT) {
    log_error("Shader instance pending release queue overflow");
    return false_v;
  }

  local_state->release_pending = true_v;
  local_state->release_serial =
      state->submit_serial + (state->frame_active ? 1u : 0u);
  shader_object->pending_release_ids[shader_object->pending_release_count++] =
      object_id;

  return true_v;
}

void vulkan_shader_object_destroy(VulkanBackendState *state,
                                  VulkanShaderObject *out_shader_object) {
  assert_log(state != NULL, "Backend state is NULL");
  assert_log(out_shader_object != NULL, "Shader object is NULL");

  if (out_shader_object->instance_pool_overflow_creations > 0 ||
      out_shader_object->instance_pool_fallback_allocations > 0) {
    log_debug(
        "Descriptor pool telemetry: overflow_pools=%u fallback_allocations=%u",
        out_shader_object->instance_pool_overflow_creations,
        out_shader_object->instance_pool_fallback_allocations);
  }

  for (uint32_t i = 0; i < out_shader_object->instance_descriptor_pool_count;
       ++i) {
    if (out_shader_object->instance_descriptor_pools[i] != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(state->device.logical_device,
                              out_shader_object->instance_descriptor_pools[i],
                              state->allocator);
      out_shader_object->instance_descriptor_pools[i] = VK_NULL_HANDLE;
    }
  }
  out_shader_object->instance_descriptor_pool_count = 0;
  out_shader_object->instance_descriptor_pool = VK_NULL_HANDLE;
  if (out_shader_object->instance_descriptor_set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(
        state->device.logical_device,
        out_shader_object->instance_descriptor_set_layout, state->allocator);
    out_shader_object->instance_descriptor_set_layout = VK_NULL_HANDLE;
  }

  if (out_shader_object->global_descriptor_pool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(state->device.logical_device,
                            out_shader_object->global_descriptor_pool,
                            state->allocator);
    out_shader_object->global_descriptor_pool = VK_NULL_HANDLE;
  }

  if (out_shader_object->global_descriptor_set_layout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(
        state->device.logical_device,
        out_shader_object->global_descriptor_set_layout, state->allocator);
    out_shader_object->global_descriptor_set_layout = VK_NULL_HANDLE;
  }

  vulkan_buffer_destroy(state,
                        &out_shader_object->instance_uniform_buffer.buffer);
  vulkan_buffer_destroy(state,
                        &out_shader_object->global_uniform_buffer.buffer);

  if (out_shader_object->has_reflection) {
    vulkan_spirv_shader_reflection_destroy(&state->alloc,
                                           &out_shader_object->reflection);
    out_shader_object->has_reflection = false_v;
  }

  vulkan_shader_destroy_modules(state, out_shader_object);
}
