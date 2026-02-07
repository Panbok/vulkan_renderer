#include "vulkan_spirv_reflection.h"

#include "renderer/vkr_buffer.h"

vkr_internal const uint32_t VKR_REFLECTION_INDEX_INVALID = UINT32_MAX;

typedef struct VulkanReflectionBindingWork {
  uint32_t set;
  uint32_t binding;
  VkDescriptorType type;
  uint32_t count;
  uint32_t byte_size;
  VkShaderStageFlags stages;
  const char *name;
} VulkanReflectionBindingWork;

typedef struct VulkanReflectionPushRangeWork {
  uint32_t offset;
  uint32_t size;
  VkShaderStageFlags stages;
} VulkanReflectionPushRangeWork;

typedef struct VulkanReflectionVertexAttributeWork {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t size;
  const char *name;
} VulkanReflectionVertexAttributeWork;

typedef struct VulkanVertexAbiLocationDesc {
  uint32_t location;
  VkFormat format;
  uint32_t offset;
} VulkanVertexAbiLocationDesc;

typedef struct VulkanVertexAbiProfileDesc {
  const VulkanVertexAbiLocationDesc *locations;
  uint32_t location_count;
  uint32_t stride;
} VulkanVertexAbiProfileDesc;

vkr_internal void vulkan_reflection_set_error_ex(
    VkrReflectionErrorContext *context, VkrReflectionError code,
    VkShaderStageFlagBits stage, String8 entry_point, int32_t backend_result,
    String8 program_name, String8 module_path, uint32_t set, uint32_t binding,
    uint32_t location);

vkr_internal bool8_t vulkan_reflection_fail_vertex_rebuild(
    const VkrSpirvReflectionCreateInfo *create_info,
    VkrReflectionErrorContext *out_error, VkrReflectionError code,
    int32_t backend_result, uint32_t location) {
  vulkan_reflection_set_error_ex(
      out_error, code, VK_SHADER_STAGE_VERTEX_BIT, string8_lit(""),
      backend_result, create_info->program_name, string8_lit(""),
      VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID, location);
  return false_v;
}

vkr_internal void
vulkan_reflection_release_vertex_bindings(VkrAllocator *allocator,
                                          VkrShaderReflection *reflection) {
  if (!allocator || !reflection || !reflection->vertex_bindings ||
      reflection->vertex_binding_count == 0) {
    return;
  }

  vkr_allocator_free(allocator, reflection->vertex_bindings,
                     sizeof(VkrVertexInputBindingDesc) *
                         (uint64_t)reflection->vertex_binding_count,
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  reflection->vertex_bindings = NULL;
  reflection->vertex_binding_count = 0;
}

vkr_internal void
vulkan_reflection_destroy_modules(VulkanSpirvReflectionModule *modules,
                                  uint32_t module_count) {
  if (!modules) {
    return;
  }
  for (uint32_t i = 0; i < module_count; ++i) {
    vulkan_spirv_reflection_module_destroy(&modules[i]);
  }
}

vkr_internal void *
vulkan_reflection_temp_alloc(const VkrSpirvReflectionCreateInfo *create_info,
                             uint64_t size) {
  return vkr_allocator_alloc(create_info->temp_allocator, size,
                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

vkr_internal void *
vulkan_reflection_temp_realloc(const VkrSpirvReflectionCreateInfo *create_info,
                               void *ptr, uint64_t old_size,
                               uint64_t new_size) {
  return vkr_allocator_realloc(create_info->temp_allocator, ptr, old_size,
                               new_size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
}

vkr_internal void *vulkan_reflection_temp_alloc_zeroed(
    const VkrSpirvReflectionCreateInfo *create_info, uint64_t size) {
  void *ptr = vulkan_reflection_temp_alloc(create_info, size);
  if (ptr) {
    MemZero(ptr, size);
  }
  return ptr;
}

vkr_internal String8 vulkan_reflection_name_duplicate(VkrAllocator *allocator,
                                                      const char *name) {
  if (!allocator || !name || name[0] == '\0') {
    return (String8){0};
  }
  return vkr_string8_duplicate_cstr(allocator, name);
}

vkr_internal bool8_t vulkan_reflection_vertex_format_size(VkFormat format,
                                                          uint32_t *out_size) {
  if (!out_size) {
    return false_v;
  }
  switch (format) {
  case VK_FORMAT_R32_SFLOAT:
  case VK_FORMAT_R32_SINT:
  case VK_FORMAT_R32_UINT:
    *out_size = 4;
    return true_v;
  case VK_FORMAT_R32G32_SFLOAT:
  case VK_FORMAT_R32G32_SINT:
  case VK_FORMAT_R32G32_UINT:
    *out_size = 8;
    return true_v;
  case VK_FORMAT_R32G32B32_SFLOAT:
  case VK_FORMAT_R32G32B32_SINT:
  case VK_FORMAT_R32G32B32_UINT:
    *out_size = 12;
    return true_v;
  case VK_FORMAT_R32G32B32A32_SFLOAT:
  case VK_FORMAT_R32G32B32A32_SINT:
  case VK_FORMAT_R32G32B32A32_UINT:
    *out_size = 16;
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vulkan_reflection_get_vertex_abi_profile_desc(
    VkrVertexAbiProfile profile, VulkanVertexAbiProfileDesc *out_desc) {
  if (!out_desc) {
    return false_v;
  }

  static const VulkanVertexAbiLocationDesc abi_3d_locations[] = {
      {.location = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(VkrVertex3d, position)},
      {.location = 1,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(VkrVertex3d, normal)},
      {.location = 2,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(VkrVertex3d, texcoord)},
      {.location = 3,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = offsetof(VkrVertex3d, colour)},
      {.location = 4,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = offsetof(VkrVertex3d, tangent)},
  };
  static const VulkanVertexAbiLocationDesc abi_2d_locations[] = {
      {.location = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(VkrVertex2d, position)},
      {.location = 1,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(VkrVertex2d, texcoord)},
  };
  static const VulkanVertexAbiLocationDesc abi_text_locations[] = {
      {.location = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(VkrTextVertex, position)},
      {.location = 1,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(VkrTextVertex, texcoord)},
      {.location = 2,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = offsetof(VkrTextVertex, color)},
  };

  switch (profile) {
  case VKR_VERTEX_ABI_PROFILE_3D:
    *out_desc = (VulkanVertexAbiProfileDesc){
        .locations = abi_3d_locations,
        .location_count = ArrayCount(abi_3d_locations),
        .stride = sizeof(VkrVertex3d),
    };
    return true_v;
  case VKR_VERTEX_ABI_PROFILE_2D:
    *out_desc = (VulkanVertexAbiProfileDesc){
        .locations = abi_2d_locations,
        .location_count = ArrayCount(abi_2d_locations),
        .stride = sizeof(VkrVertex2d),
    };
    return true_v;
  case VKR_VERTEX_ABI_PROFILE_TEXT_2D:
    *out_desc = (VulkanVertexAbiProfileDesc){
        .locations = abi_text_locations,
        .location_count = ArrayCount(abi_text_locations),
        .stride = sizeof(VkrTextVertex),
    };
    return true_v;
  case VKR_VERTEX_ABI_PROFILE_UNKNOWN:
  case VKR_VERTEX_ABI_PROFILE_NONE:
  default:
    break;
  }

  return false_v;
}

vkr_internal const VulkanVertexAbiLocationDesc *
vulkan_reflection_find_vertex_abi_location(
    const VulkanVertexAbiProfileDesc *profile_desc, uint32_t location) {
  if (!profile_desc || !profile_desc->locations) {
    return NULL;
  }

  for (uint32_t i = 0; i < profile_desc->location_count; ++i) {
    if (profile_desc->locations[i].location == location) {
      return &profile_desc->locations[i];
    }
  }

  return NULL;
}

vkr_internal bool8_t vulkan_reflection_rebuild_vertex_bindings(
    const VkrSpirvReflectionCreateInfo *create_info,
    VkrShaderReflection *reflection, VkrReflectionErrorContext *out_error) {
  if (reflection->vertex_attribute_count == 0) {
    vulkan_reflection_release_vertex_bindings(create_info->allocator,
                                              reflection);
    return true_v;
  }

  VulkanVertexAbiProfileDesc abi_profile = {0};
  const bool8_t has_vertex_abi = vulkan_reflection_get_vertex_abi_profile_desc(
      create_info->vertex_abi_profile, &abi_profile);
  if (!has_vertex_abi) {
    return vulkan_reflection_fail_vertex_rebuild(
        create_info, out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
        SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND,
        reflection->vertex_attributes[0].location);
  }

  uint32_t offsets[2] = {0, 0};
  bool8_t binding_used[2] = {false_v, false_v};

  for (uint32_t i = 0; i < reflection->vertex_attribute_count; ++i) {
    VkrVertexInputAttributeDesc *attribute = &reflection->vertex_attributes[i];
    if (attribute->binding > 1) {
      return vulkan_reflection_fail_vertex_rebuild(
          create_info, out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
          SPV_REFLECT_RESULT_ERROR_RANGE_EXCEEDED, attribute->location);
    }

    if (attribute->binding == 0) {
      const VulkanVertexAbiLocationDesc *abi_location =
          vulkan_reflection_find_vertex_abi_location(&abi_profile,
                                                     attribute->location);
      if (!abi_location) {
        return vulkan_reflection_fail_vertex_rebuild(
            create_info, out_error,
            VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
            SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND, attribute->location);
      }
      if (abi_location->format != attribute->format) {
        return vulkan_reflection_fail_vertex_rebuild(
            create_info, out_error,
            VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
            SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH, attribute->location);
      }

      attribute->offset = abi_location->offset;
      binding_used[0] = true_v;
      continue;
    }

    uint32_t attribute_size = 0;
    if (!vulkan_reflection_vertex_format_size(attribute->format,
                                              &attribute_size)) {
      return vulkan_reflection_fail_vertex_rebuild(
          create_info, out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION,
          attribute->location);
    }

    const uint32_t binding_index = attribute->binding;
    const uint32_t aligned_offset = AlignPow2(offsets[binding_index], 4);
    attribute->offset = aligned_offset;
    offsets[binding_index] = aligned_offset + attribute_size;
    binding_used[binding_index] = true_v;
  }

  vulkan_reflection_release_vertex_bindings(create_info->allocator, reflection);

  uint32_t binding_count =
      (binding_used[0] ? 1u : 0u) + (binding_used[1] ? 1u : 0u);
  if (binding_count == 0) {
    return true_v;
  }

  reflection->vertex_bindings = vkr_allocator_alloc(
      create_info->allocator,
      sizeof(VkrVertexInputBindingDesc) * (uint64_t)binding_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!reflection->vertex_bindings) {
    return vulkan_reflection_fail_vertex_rebuild(
        create_info, out_error, VKR_REFLECTION_ERROR_PARSE_FAILED,
        SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED, VKR_REFLECTION_INDEX_INVALID);
  }

  uint32_t write_index = 0;
  if (binding_used[0]) {
    reflection->vertex_bindings[write_index++] = (VkrVertexInputBindingDesc){
        .binding = 0,
        .stride = abi_profile.stride,
        .rate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
  }
  if (binding_used[1]) {
    reflection->vertex_bindings[write_index++] = (VkrVertexInputBindingDesc){
        .binding = 1,
        .stride = offsets[1],
        .rate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
  }
  reflection->vertex_binding_count = binding_count;
  return true_v;
}

vkr_internal void vulkan_reflection_string_destroy(VkrAllocator *allocator,
                                                   String8 *name) {
  if (!allocator || !name || !name->str || name->length == 0) {
    return;
  }
  vkr_allocator_free(allocator, name->str, name->length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  name->str = NULL;
  name->length = 0;
}

vkr_internal const SpvReflectEntryPoint *
vulkan_spirv_reflection_find_entry_point(const SpvReflectShaderModule *module,
                                         const String8 *entry_point_name) {
  if (!module || !entry_point_name) {
    return NULL;
  }

  for (uint32_t i = 0; i < module->entry_point_count; ++i) {
    const SpvReflectEntryPoint *entry = &module->entry_points[i];
    if (!entry->name) {
      continue;
    }

    if (vkr_string8_equals_cstr(entry_point_name, entry->name)) {
      return entry;
    }
  }

  return NULL;
}

vkr_internal void
vulkan_reflection_error_context_set_string(String8 source, uint8_t *storage,
                                           uint32_t storage_capacity,
                                           String8 *out_value) {
  if (!storage || storage_capacity == 0 || !out_value) {
    return;
  }

  if (!source.str || source.length == 0) {
    storage[0] = '\0';
    out_value->str = NULL;
    out_value->length = 0;
    return;
  }

  const uint64_t max_copy = (uint64_t)storage_capacity - 1;
  const uint64_t copy_length = Min(source.length, max_copy);
  MemCopy(storage, source.str, copy_length);
  storage[copy_length] = '\0';

  out_value->str = storage;
  out_value->length = copy_length;
}

vkr_internal void vulkan_reflection_set_error(
    VkrReflectionErrorContext *context, VkrReflectionError code,
    VkShaderStageFlagBits stage, String8 entry_point, int32_t backend_result) {
  if (!context) {
    return;
  }

  context->code = code;
  context->stage = stage;
  vulkan_reflection_error_context_set_string(
      entry_point, context->entry_point_storage,
      VKR_REFLECTION_ERROR_ENTRY_POINT_MAX, &context->entry_point);
  context->set = VKR_REFLECTION_INDEX_INVALID;
  context->binding = VKR_REFLECTION_INDEX_INVALID;
  context->location = VKR_REFLECTION_INDEX_INVALID;
  context->backend_result = backend_result;
}

vkr_internal void vulkan_reflection_set_error_ex(
    VkrReflectionErrorContext *context, VkrReflectionError code,
    VkShaderStageFlagBits stage, String8 entry_point, int32_t backend_result,
    String8 program_name, String8 module_path, uint32_t set, uint32_t binding,
    uint32_t location) {
  vulkan_reflection_set_error(context, code, stage, entry_point,
                              backend_result);
  if (!context) {
    return;
  }
  vulkan_reflection_error_context_set_string(
      program_name, context->program_name_storage,
      VKR_REFLECTION_ERROR_PROGRAM_NAME_MAX, &context->program_name);
  vulkan_reflection_error_context_set_string(
      module_path, context->module_path_storage,
      VKR_REFLECTION_ERROR_MODULE_PATH_MAX, &context->module_path);
  context->set = set;
  context->binding = binding;
  context->location = location;
}

vkr_internal bool8_t
vulkan_reflection_is_single_stage_flag(VkShaderStageFlagBits stage) {
  uint32_t bits = (uint32_t)stage;
  return bits != 0 && (bits & (bits - 1)) == 0;
}

vkr_internal bool8_t vulkan_reflection_map_descriptor_type(
    SpvReflectDescriptorType source_type, VkDescriptorType *out_type) {
  if (!out_type) {
    return false_v;
  }

  switch (source_type) {
  case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
    *out_type = VK_DESCRIPTOR_TYPE_SAMPLER;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    *out_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    *out_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    *out_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    *out_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    *out_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    *out_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
    *out_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    return true_v;
  case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
    *out_type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal bool8_t vulkan_reflection_descriptor_count(
    const SpvReflectDescriptorBinding *binding, uint32_t *out_count) {
  if (!binding || !out_count) {
    return false_v;
  }

  uint64_t count = binding->count;
  if (binding->array.dims_count > 0) {
    count = 1;
    for (uint32_t i = 0; i < binding->array.dims_count; ++i) {
      const uint32_t dim = binding->array.dims[i];
      if (dim == SPV_REFLECT_ARRAY_DIM_RUNTIME || dim == 0) {
        return false_v;
      }
      count *= dim;
      if (count > UINT32_MAX) {
        return false_v;
      }
    }
  }

  if (count == 0 || count > UINT32_MAX) {
    return false_v;
  }

  *out_count = (uint32_t)count;
  return true_v;
}

vkr_internal uint32_t vulkan_reflection_descriptor_byte_size(
    const SpvReflectDescriptorBinding *binding,
    VkDescriptorType descriptor_type) {
  if (!binding) {
    return 0;
  }

  const bool8_t is_buffer_desc =
      descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
      descriptor_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
      descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
      descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
  if (!is_buffer_desc) {
    return 0;
  }

  uint32_t size = binding->block.padded_size;
  if (size == 0) {
    size = binding->block.size;
  }
  return size;
}

vkr_internal bool8_t vulkan_reflection_map_vertex_format(
    SpvReflectFormat source_format, VkFormat *out_format, uint32_t *out_size) {
  if (!out_format || !out_size) {
    return false_v;
  }

  switch (source_format) {
  case SPV_REFLECT_FORMAT_R32_SFLOAT:
    *out_format = VK_FORMAT_R32_SFLOAT;
    *out_size = 4;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
    *out_format = VK_FORMAT_R32G32_SFLOAT;
    *out_size = 8;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
    *out_format = VK_FORMAT_R32G32B32_SFLOAT;
    *out_size = 12;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
    *out_format = VK_FORMAT_R32G32B32A32_SFLOAT;
    *out_size = 16;
    return true_v;
  case SPV_REFLECT_FORMAT_R32_SINT:
    *out_format = VK_FORMAT_R32_SINT;
    *out_size = 4;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32_SINT:
    *out_format = VK_FORMAT_R32G32_SINT;
    *out_size = 8;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32_SINT:
    *out_format = VK_FORMAT_R32G32B32_SINT;
    *out_size = 12;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32A32_SINT:
    *out_format = VK_FORMAT_R32G32B32A32_SINT;
    *out_size = 16;
    return true_v;
  case SPV_REFLECT_FORMAT_R32_UINT:
    *out_format = VK_FORMAT_R32_UINT;
    *out_size = 4;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32_UINT:
    *out_format = VK_FORMAT_R32G32_UINT;
    *out_size = 8;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32_UINT:
    *out_format = VK_FORMAT_R32G32B32_UINT;
    *out_size = 12;
    return true_v;
  case SPV_REFLECT_FORMAT_R32G32B32A32_UINT:
    *out_format = VK_FORMAT_R32G32B32A32_UINT;
    *out_size = 16;
    return true_v;
  default:
    return false_v;
  }
}

vkr_internal int vulkan_reflection_binding_work_compare(const void *lhs,
                                                        const void *rhs) {
  const VulkanReflectionBindingWork *a = lhs;
  const VulkanReflectionBindingWork *b = rhs;
  if (a->set < b->set)
    return -1;
  if (a->set > b->set)
    return 1;
  if (a->binding < b->binding)
    return -1;
  if (a->binding > b->binding)
    return 1;
  return 0;
}

vkr_internal int vulkan_reflection_u32_compare(const void *lhs,
                                               const void *rhs) {
  const uint32_t a = *(const uint32_t *)lhs;
  const uint32_t b = *(const uint32_t *)rhs;
  if (a < b)
    return -1;
  if (a > b)
    return 1;
  return 0;
}

vkr_internal int vulkan_reflection_push_range_compare(const void *lhs,
                                                      const void *rhs) {
  const VulkanReflectionPushRangeWork *a = lhs;
  const VulkanReflectionPushRangeWork *b = rhs;
  if (a->offset < b->offset)
    return -1;
  if (a->offset > b->offset)
    return 1;
  if (a->size < b->size)
    return -1;
  if (a->size > b->size)
    return 1;
  if (a->stages < b->stages)
    return -1;
  if (a->stages > b->stages)
    return 1;
  return 0;
}

vkr_internal int vulkan_reflection_vertex_attr_compare(const void *lhs,
                                                       const void *rhs) {
  const VulkanReflectionVertexAttributeWork *a = lhs;
  const VulkanReflectionVertexAttributeWork *b = rhs;
  if (a->location < b->location)
    return -1;
  if (a->location > b->location)
    return 1;
  return 0;
}

vkr_internal int32_t vulkan_reflection_find_binding_index(
    const VulkanReflectionBindingWork *bindings, uint32_t count, uint32_t set,
    uint32_t binding) {
  for (uint32_t i = 0; i < count; ++i) {
    if (bindings[i].set == set && bindings[i].binding == binding) {
      return (int32_t)i;
    }
  }
  return -1;
}

vkr_internal bool8_t vulkan_reflection_collect_descriptor_bindings(
    const VkrSpirvReflectionCreateInfo *create_info,
    const VulkanSpirvReflectionModule *modules, VkrShaderReflection *reflection,
    VkrReflectionErrorContext *out_error) {
  VulkanReflectionBindingWork *work = NULL;
  uint32_t work_count = 0;

  for (uint32_t module_index = 0; module_index < create_info->module_count;
       ++module_index) {
    const VulkanSpirvReflectionModule *module = &modules[module_index];
    for (uint32_t set_index = 0;
         set_index < module->entry_point->descriptor_set_count; ++set_index) {
      const SpvReflectDescriptorSet *set =
          &module->entry_point->descriptor_sets[set_index];
      for (uint32_t binding_index = 0; binding_index < set->binding_count;
           ++binding_index) {
        const SpvReflectDescriptorBinding *binding =
            set->bindings[binding_index];
        if (!binding) {
          continue;
        }

        VkDescriptorType mapped_type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (!vulkan_reflection_map_descriptor_type(binding->descriptor_type,
                                                   &mapped_type)) {
          vulkan_reflection_set_error_ex(
              out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_DESCRIPTOR,
              module->stage, module->entry_point_name, binding->descriptor_type,
              create_info->program_name,
              create_info->modules[module_index].path, set->set,
              binding->binding, VKR_REFLECTION_INDEX_INVALID);
          return false_v;
        }

        uint32_t descriptor_count = 0;
        if (!vulkan_reflection_descriptor_count(binding, &descriptor_count)) {
          vulkan_reflection_set_error_ex(
              out_error, VKR_REFLECTION_ERROR_RUNTIME_ARRAY, module->stage,
              module->entry_point_name, SPV_REFLECT_RESULT_ERROR_RANGE_EXCEEDED,
              create_info->program_name,
              create_info->modules[module_index].path, set->set,
              binding->binding, VKR_REFLECTION_INDEX_INVALID);
          return false_v;
        }

        const uint32_t descriptor_byte_size =
            vulkan_reflection_descriptor_byte_size(binding, mapped_type);

        const int32_t existing_index = vulkan_reflection_find_binding_index(
            work, work_count, set->set, binding->binding);
        if (existing_index >= 0) {
          VulkanReflectionBindingWork *existing = &work[existing_index];
          if (existing->type != mapped_type) {
            vulkan_reflection_set_error_ex(
                out_error, VKR_REFLECTION_ERROR_BINDING_TYPE_MISMATCH,
                module->stage, module->entry_point_name,
                SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH,
                create_info->program_name,
                create_info->modules[module_index].path, set->set,
                binding->binding, VKR_REFLECTION_INDEX_INVALID);
            return false_v;
          }
          if (existing->count != descriptor_count) {
            vulkan_reflection_set_error_ex(
                out_error, VKR_REFLECTION_ERROR_BINDING_COUNT_MISMATCH,
                module->stage, module->entry_point_name,
                SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH,
                create_info->program_name,
                create_info->modules[module_index].path, set->set,
                binding->binding, VKR_REFLECTION_INDEX_INVALID);
            return false_v;
          }
          if (existing->byte_size != descriptor_byte_size) {
            vulkan_reflection_set_error_ex(
                out_error, VKR_REFLECTION_ERROR_BINDING_SIZE_MISMATCH,
                module->stage, module->entry_point_name,
                SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH,
                create_info->program_name,
                create_info->modules[module_index].path, set->set,
                binding->binding, VKR_REFLECTION_INDEX_INVALID);
            return false_v;
          }
          existing->stages |= module->stage;
          if ((!existing->name || existing->name[0] == '\0') && binding->name &&
              binding->name[0] != '\0') {
            existing->name = binding->name;
          }
          continue;
        }

        VulkanReflectionBindingWork *grown = vulkan_reflection_temp_realloc(
            create_info, work, sizeof(*grown) * (uint64_t)work_count,
            sizeof(*grown) * (uint64_t)(work_count + 1));
        if (!grown) {
          vulkan_reflection_set_error_ex(
              out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, module->stage,
              module->entry_point_name, SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
              create_info->program_name,
              create_info->modules[module_index].path, set->set,
              binding->binding, VKR_REFLECTION_INDEX_INVALID);
          return false_v;
        }
        work = grown;

        work[work_count] = (VulkanReflectionBindingWork){
            .set = set->set,
            .binding = binding->binding,
            .type = mapped_type,
            .count = descriptor_count,
            .byte_size = descriptor_byte_size,
            .stages = module->stage,
            .name = binding->name,
        };
        ++work_count;
      }
    }
  }

  if (work_count == 0) {
    reflection->set_count = 0;
    reflection->sets = NULL;
    reflection->layout_set_count = 0;
    return true_v;
  }

  qsort(work, work_count, sizeof(*work),
        vulkan_reflection_binding_work_compare);

  uint32_t set_count = 1;
  uint32_t max_set = work[0].set;
  for (uint32_t i = 1; i < work_count; ++i) {
    if (work[i].set != work[i - 1].set) {
      ++set_count;
    }
    if (work[i].set > max_set) {
      max_set = work[i].set;
    }
  }

  reflection->sets =
      vkr_allocator_alloc(create_info->allocator,
                          sizeof(VkrDescriptorSetDesc) * (uint64_t)set_count,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!reflection->sets) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }
  MemZero(reflection->sets, sizeof(VkrDescriptorSetDesc) * (uint64_t)set_count);
  reflection->set_count = set_count;

  uint32_t set_write_index = 0;
  uint32_t i = 0;
  while (i < work_count) {
    const uint32_t set_index = work[i].set;
    uint32_t start = i;
    while (i < work_count && work[i].set == set_index) {
      ++i;
    }
    const uint32_t binding_count = i - start;

    VkrDescriptorBindingDesc *bindings = vkr_allocator_alloc(
        create_info->allocator,
        sizeof(VkrDescriptorBindingDesc) * (uint64_t)binding_count,
        VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!bindings) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
          string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
          create_info->program_name, string8_lit(""), set_index,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
      return false_v;
    }
    MemZero(bindings,
            sizeof(VkrDescriptorBindingDesc) * (uint64_t)binding_count);

    for (uint32_t j = 0; j < binding_count; ++j) {
      const VulkanReflectionBindingWork *src = &work[start + j];
      bindings[j] = (VkrDescriptorBindingDesc){
          .binding = src->binding,
          .type = src->type,
          .count = src->count,
          .byte_size = src->byte_size,
          .stages = src->stages,
          .name = vulkan_reflection_name_duplicate(create_info->allocator,
                                                   src->name),
      };
    }

    reflection->sets[set_write_index++] = (VkrDescriptorSetDesc){
        .set = set_index,
        .role = VKR_DESCRIPTOR_SET_ROLE_NONE,
        .binding_count = binding_count,
        .bindings = bindings,
    };
  }

  reflection->layout_set_count = max_set + 1;
  return true_v;
}

vkr_internal bool8_t vulkan_reflection_collect_push_constants(
    const VkrSpirvReflectionCreateInfo *create_info,
    const VulkanSpirvReflectionModule *modules, VkrShaderReflection *reflection,
    VkrReflectionErrorContext *out_error) {
  VulkanReflectionPushRangeWork *source_ranges = NULL;
  uint32_t source_range_count = 0;

  for (uint32_t module_index = 0; module_index < create_info->module_count;
       ++module_index) {
    const VulkanSpirvReflectionModule *module = &modules[module_index];
    uint32_t block_count = 0;
    SpvReflectResult reflect_result =
        spvReflectEnumerateEntryPointPushConstantBlocks(
            &module->module, module->entry_point->name, &block_count, NULL);
    if (reflect_result != SPV_REFLECT_RESULT_SUCCESS) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, module->stage,
          module->entry_point_name, reflect_result, create_info->program_name,
          create_info->modules[module_index].path, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
      return false_v;
    }

    if (block_count == 0) {
      continue;
    }

    SpvReflectBlockVariable **blocks = vulkan_reflection_temp_alloc(
        create_info, sizeof(*blocks) * (uint64_t)block_count);
    if (!blocks) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, module->stage,
          module->entry_point_name, SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
          create_info->program_name, create_info->modules[module_index].path,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID);
      return false_v;
    }

    reflect_result = spvReflectEnumerateEntryPointPushConstantBlocks(
        &module->module, module->entry_point->name, &block_count, blocks);
    if (reflect_result != SPV_REFLECT_RESULT_SUCCESS) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, module->stage,
          module->entry_point_name, reflect_result, create_info->program_name,
          create_info->modules[module_index].path, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
      return false_v;
    }

    for (uint32_t block_index = 0; block_index < block_count; ++block_index) {
      const SpvReflectBlockVariable *block = blocks[block_index];
      if (!block || block->size == 0) {
        continue;
      }

      if ((block->offset % 4) != 0 || (block->size % 4) != 0) {
        vulkan_reflection_set_error_ex(
            out_error, VKR_REFLECTION_ERROR_PUSH_CONSTANT_ALIGNMENT,
            module->stage, module->entry_point_name,
            SPV_REFLECT_RESULT_ERROR_RANGE_EXCEEDED, create_info->program_name,
            create_info->modules[module_index].path,
            VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
            VKR_REFLECTION_INDEX_INVALID);
        return false_v;
      }

      const uint64_t range_end =
          (uint64_t)block->offset + (uint64_t)block->size;
      if (create_info->max_push_constant_size > 0 &&
          range_end > create_info->max_push_constant_size) {
        vulkan_reflection_set_error_ex(
            out_error, VKR_REFLECTION_ERROR_PUSH_CONSTANT_LIMIT, module->stage,
            module->entry_point_name, SPV_REFLECT_RESULT_ERROR_RANGE_EXCEEDED,
            create_info->program_name, create_info->modules[module_index].path,
            VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
            VKR_REFLECTION_INDEX_INVALID);
        return false_v;
      }

      VulkanReflectionPushRangeWork *grown = vulkan_reflection_temp_realloc(
          create_info, source_ranges,
          sizeof(*grown) * (uint64_t)source_range_count,
          sizeof(*grown) * (uint64_t)(source_range_count + 1));
      if (!grown) {
        vulkan_reflection_set_error_ex(
            out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, module->stage,
            module->entry_point_name, SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
            create_info->program_name, create_info->modules[module_index].path,
            VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
            VKR_REFLECTION_INDEX_INVALID);
        return false_v;
      }
      source_ranges = grown;
      source_ranges[source_range_count++] = (VulkanReflectionPushRangeWork){
          .offset = block->offset,
          .size = block->size,
          .stages = module->stage,
      };
    }
  }

  if (source_range_count == 0) {
    reflection->push_constant_range_count = 0;
    reflection->push_constant_ranges = NULL;
    return true_v;
  }

  uint32_t *boundaries = vulkan_reflection_temp_alloc(
      create_info, sizeof(*boundaries) * (uint64_t)source_range_count * 2u);
  if (!boundaries) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }

  uint32_t boundary_count = 0;
  for (uint32_t i = 0; i < source_range_count; ++i) {
    boundaries[boundary_count++] = source_ranges[i].offset;
    boundaries[boundary_count++] =
        source_ranges[i].offset + source_ranges[i].size;
  }

  qsort(boundaries, boundary_count, sizeof(*boundaries),
        vulkan_reflection_u32_compare);

  uint32_t unique_count = 0;
  for (uint32_t i = 0; i < boundary_count; ++i) {
    if (i == 0 || boundaries[i] != boundaries[i - 1]) {
      boundaries[unique_count++] = boundaries[i];
    }
  }

  VulkanReflectionPushRangeWork *normalized = vulkan_reflection_temp_alloc(
      create_info, sizeof(*normalized) * (uint64_t)Max(unique_count, 1));
  if (!normalized) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }

  uint32_t normalized_count = 0;
  for (uint32_t i = 0; i + 1 < unique_count; ++i) {
    const uint32_t begin = boundaries[i];
    const uint32_t end = boundaries[i + 1];
    if (end <= begin) {
      continue;
    }

    VkShaderStageFlags stages = 0;
    for (uint32_t range_index = 0; range_index < source_range_count;
         ++range_index) {
      const uint32_t range_begin = source_ranges[range_index].offset;
      const uint32_t range_end =
          source_ranges[range_index].offset + source_ranges[range_index].size;
      if (begin >= range_begin && end <= range_end) {
        stages |= source_ranges[range_index].stages;
      }
    }

    if (stages == 0) {
      continue;
    }

    normalized[normalized_count++] = (VulkanReflectionPushRangeWork){
        .offset = begin,
        .size = end - begin,
        .stages = stages,
    };
  }

  qsort(normalized, normalized_count, sizeof(*normalized),
        vulkan_reflection_push_range_compare);

  uint32_t merged_count = 0;
  for (uint32_t i = 0; i < normalized_count; ++i) {
    if (merged_count == 0) {
      normalized[merged_count++] = normalized[i];
      continue;
    }

    VulkanReflectionPushRangeWork *last = &normalized[merged_count - 1];
    const VulkanReflectionPushRangeWork *current = &normalized[i];
    const uint32_t last_end = last->offset + last->size;
    if (last->stages == current->stages && last_end == current->offset) {
      last->size += current->size;
    } else {
      normalized[merged_count++] = *current;
    }
  }

  reflection->push_constant_ranges = vkr_allocator_alloc(
      create_info->allocator,
      sizeof(VkrPushConstantRangeDesc) * (uint64_t)merged_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!reflection->push_constant_ranges) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }

  for (uint32_t i = 0; i < merged_count; ++i) {
    reflection->push_constant_ranges[i] = (VkrPushConstantRangeDesc){
        .offset = normalized[i].offset,
        .size = normalized[i].size,
        .stages = normalized[i].stages,
    };
  }
  reflection->push_constant_range_count = merged_count;

  return true_v;
}

vkr_internal bool8_t vulkan_reflection_collect_vertex_inputs(
    const VkrSpirvReflectionCreateInfo *create_info,
    const VulkanSpirvReflectionModule *modules, VkrShaderReflection *reflection,
    VkrReflectionErrorContext *out_error) {
  const VulkanSpirvReflectionModule *vertex_module = NULL;
  String8 vertex_module_path = {0};

  for (uint32_t i = 0; i < create_info->module_count; ++i) {
    if (modules[i].stage == VK_SHADER_STAGE_VERTEX_BIT) {
      vertex_module = &modules[i];
      vertex_module_path = create_info->modules[i].path;
      break;
    }
  }

  if (!vertex_module) {
    reflection->vertex_binding_count = 0;
    reflection->vertex_bindings = NULL;
    reflection->vertex_attribute_count = 0;
    reflection->vertex_attributes = NULL;
    return true_v;
  }

  VulkanReflectionVertexAttributeWork *attrs = NULL;
  uint32_t attr_count = 0;

  for (uint32_t i = 0; i < vertex_module->entry_point->input_variable_count;
       ++i) {
    const SpvReflectInterfaceVariable *input =
        vertex_module->entry_point->input_variables[i];
    if (!input || input->built_in != -1) {
      continue;
    }

    if (input->location == VKR_REFLECTION_INDEX_INVALID) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_MISSING_LOCATION,
          vertex_module->stage, vertex_module->entry_point_name,
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION,
          create_info->program_name, vertex_module_path,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
          input ? input->location : VKR_REFLECTION_INDEX_INVALID);
      return false_v;
    }

    // SPIRV-Reflect uses UINT32_MAX when Component decoration is absent.
    // Only reject explicitly decorated non-zero components.
    if (input->component != 0 && input->component != UINT32_MAX) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_VERTEX_COMPONENT_DECORATION,
          vertex_module->stage, vertex_module->entry_point_name,
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION,
          create_info->program_name, vertex_module_path,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
          input->location);
      return false_v;
    }

    if (input->array.dims_count > 0 || input->numeric.matrix.column_count > 1 ||
        input->numeric.matrix.row_count > 1 ||
        input->numeric.scalar.width == 64) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
          vertex_module->stage, vertex_module->entry_point_name,
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION,
          create_info->program_name, vertex_module_path,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
          input->location);
      return false_v;
    }

    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t format_size = 0;
    if (!vulkan_reflection_map_vertex_format(input->format, &format,
                                             &format_size)) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT,
          vertex_module->stage, vertex_module->entry_point_name,
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_INSTRUCTION,
          create_info->program_name, vertex_module_path,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
          input->location);
      return false_v;
    }

    for (uint32_t existing = 0; existing < attr_count; ++existing) {
      if (attrs[existing].location == input->location) {
        vulkan_reflection_set_error_ex(
            out_error, VKR_REFLECTION_ERROR_DUPLICATE_VERTEX_LOCATION,
            vertex_module->stage, vertex_module->entry_point_name,
            SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH, create_info->program_name,
            vertex_module_path, VKR_REFLECTION_INDEX_INVALID,
            VKR_REFLECTION_INDEX_INVALID, input->location);
        return false_v;
      }
    }

    VulkanReflectionVertexAttributeWork *grown = vulkan_reflection_temp_realloc(
        create_info, attrs, sizeof(*grown) * (uint64_t)attr_count,
        sizeof(*grown) * (uint64_t)(attr_count + 1));
    if (!grown) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, vertex_module->stage,
          vertex_module->entry_point_name,
          SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED, create_info->program_name,
          vertex_module_path, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID, input->location);
      return false_v;
    }
    attrs = grown;
    attrs[attr_count++] = (VulkanReflectionVertexAttributeWork){
        .location = input->location,
        .binding = 0, // Default vertex-rate binding.
        .format = format,
        .size = format_size,
        .name = input->name,
    };
  }

  if (attr_count == 0) {
    reflection->vertex_binding_count = 0;
    reflection->vertex_bindings = NULL;
    reflection->vertex_attribute_count = 0;
    reflection->vertex_attributes = NULL;
    return true_v;
  }

  qsort(attrs, attr_count, sizeof(*attrs),
        vulkan_reflection_vertex_attr_compare);

  reflection->vertex_attributes = vkr_allocator_alloc(
      create_info->allocator,
      sizeof(VkrVertexInputAttributeDesc) * (uint64_t)attr_count,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!reflection->vertex_attributes) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, vertex_module->stage,
        vertex_module->entry_point_name, SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, vertex_module_path,
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }
  MemZero(reflection->vertex_attributes,
          sizeof(VkrVertexInputAttributeDesc) * (uint64_t)attr_count);

  uint32_t offset = 0;
  for (uint32_t i = 0; i < attr_count; ++i) {
    const uint32_t aligned_offset = AlignPow2(offset, 4);
    reflection->vertex_attributes[i] = (VkrVertexInputAttributeDesc){
        .location = attrs[i].location,
        .binding = attrs[i].binding,
        .format = attrs[i].format,
        .offset = aligned_offset,
        .name = vulkan_reflection_name_duplicate(create_info->allocator,
                                                 attrs[i].name),
    };
    offset = aligned_offset + attrs[i].size;
  }

  reflection->vertex_bindings = vkr_allocator_alloc(
      create_info->allocator, sizeof(VkrVertexInputBindingDesc),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!reflection->vertex_bindings) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, vertex_module->stage,
        vertex_module->entry_point_name, SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, vertex_module_path,
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }
  reflection->vertex_bindings[0] = (VkrVertexInputBindingDesc){
      .binding = 0,
      .stride = offset,
      .rate = VK_VERTEX_INPUT_RATE_VERTEX,
  };
  reflection->vertex_binding_count = 1;
  reflection->vertex_attribute_count = attr_count;

  return true_v;
}

void vulkan_reflection_error_context_reset(VkrReflectionErrorContext *context) {
  assert_log(context != NULL, "Context must not be NULL");

  MemZero(context, sizeof(*context));
  context->code = VKR_REFLECTION_OK;
  context->program_name = (String8){0};
  context->module_path = (String8){0};
  context->entry_point = (String8){0};
  context->set = VKR_REFLECTION_INDEX_INVALID;
  context->binding = VKR_REFLECTION_INDEX_INVALID;
  context->location = VKR_REFLECTION_INDEX_INVALID;
}

String8 vulkan_spirv_reflection_resolve_entry_point(String8 entry_point) {
  if (!entry_point.str || entry_point.length == 0) {
    return string8_lit("main");
  }
  return entry_point;
}

bool8_t vulkan_spirv_reflection_module_create(
    const uint8_t *spirv_bytes, uint64_t spirv_size,
    VkShaderStageFlagBits expected_stage, String8 entry_point,
    VulkanSpirvReflectionModule *out_module,
    VkrReflectionErrorContext *out_error) {
  assert_log(out_module != NULL, "Output module must not be NULL");
  MemZero(out_module, sizeof(*out_module));
  if (out_error != NULL) {
    vulkan_reflection_error_context_reset(out_error);
  }

  const String8 resolved_entry_point =
      vulkan_spirv_reflection_resolve_entry_point(entry_point);

  if (!spirv_bytes || spirv_size == 0 || (spirv_size % sizeof(uint32_t)) != 0) {
    vulkan_reflection_set_error(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, expected_stage,
        resolved_entry_point, SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_CODE_SIZE);
    return false_v;
  }

  SpvReflectShaderModule module = {0};
  const SpvReflectResult reflect_result = spvReflectCreateShaderModule2(
      SPV_REFLECT_MODULE_FLAG_NONE, spirv_size, spirv_bytes, &module);
  if (reflect_result != SPV_REFLECT_RESULT_SUCCESS) {
    vulkan_reflection_set_error(out_error, VKR_REFLECTION_ERROR_PARSE_FAILED,
                                expected_stage, resolved_entry_point,
                                reflect_result);
    return false_v;
  }

  const SpvReflectEntryPoint *resolved_entry =
      vulkan_spirv_reflection_find_entry_point(&module, &resolved_entry_point);
  if (!resolved_entry) {
    vulkan_reflection_set_error(
        out_error, VKR_REFLECTION_ERROR_ENTRY_POINT_NOT_FOUND, expected_stage,
        resolved_entry_point, SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND);
    spvReflectDestroyShaderModule(&module);
    return false_v;
  }

  const VkShaderStageFlagBits reflected_stage =
      (VkShaderStageFlagBits)resolved_entry->shader_stage;
  if (expected_stage != 0 && reflected_stage != expected_stage) {
    vulkan_reflection_set_error(
        out_error, VKR_REFLECTION_ERROR_STAGE_MISMATCH, expected_stage,
        resolved_entry_point,
        SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_ENTRY_POINT);
    spvReflectDestroyShaderModule(&module);
    return false_v;
  }

  out_module->module = module;
  out_module->entry_point = resolved_entry;
  out_module->stage = reflected_stage;
  if (resolved_entry->name) {
    const uint64_t reflected_entry_name_length =
        string_length(resolved_entry->name);
    out_module->entry_point_name =
        (reflected_entry_name_length > 0)
            ? string8_create_from_cstr((const uint8_t *)resolved_entry->name,
                                       reflected_entry_name_length)
            : resolved_entry_point;
  } else {
    out_module->entry_point_name = resolved_entry_point;
  }
  out_module->is_initialized = true_v;

  return true_v;
}

void vulkan_spirv_reflection_module_destroy(
    VulkanSpirvReflectionModule *module) {
  if (!module || !module->is_initialized) {
    return;
  }

  spvReflectDestroyShaderModule(&module->module);
  MemZero(module, sizeof(*module));
}

void vulkan_spirv_shader_reflection_destroy(VkrAllocator *allocator,
                                            VkrShaderReflection *reflection) {
  if (!allocator || !reflection) {
    return;
  }

  for (uint32_t set_index = 0; set_index < reflection->set_count; ++set_index) {
    VkrDescriptorSetDesc *set = &reflection->sets[set_index];
    for (uint32_t binding_index = 0; binding_index < set->binding_count;
         ++binding_index) {
      vulkan_reflection_string_destroy(allocator,
                                       &set->bindings[binding_index].name);
    }
    if (set->bindings && set->binding_count > 0) {
      vkr_allocator_free(allocator, set->bindings,
                         sizeof(VkrDescriptorBindingDesc) *
                             (uint64_t)set->binding_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }
  }

  if (reflection->sets && reflection->set_count > 0) {
    vkr_allocator_free(allocator, reflection->sets,
                       sizeof(VkrDescriptorSetDesc) *
                           (uint64_t)reflection->set_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  if (reflection->push_constant_ranges &&
      reflection->push_constant_range_count > 0) {
    vkr_allocator_free(allocator, reflection->push_constant_ranges,
                       sizeof(VkrPushConstantRangeDesc) *
                           (uint64_t)reflection->push_constant_range_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  if (reflection->vertex_attributes && reflection->vertex_attribute_count > 0) {
    for (uint32_t i = 0; i < reflection->vertex_attribute_count; ++i) {
      vulkan_reflection_string_destroy(allocator,
                                       &reflection->vertex_attributes[i].name);
    }
    vkr_allocator_free(allocator, reflection->vertex_attributes,
                       sizeof(VkrVertexInputAttributeDesc) *
                           (uint64_t)reflection->vertex_attribute_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  if (reflection->vertex_bindings && reflection->vertex_binding_count > 0) {
    vkr_allocator_free(allocator, reflection->vertex_bindings,
                       sizeof(VkrVertexInputBindingDesc) *
                           (uint64_t)reflection->vertex_binding_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  for (uint32_t i = 0; i < reflection->uniform_block_count; ++i) {
    VkrUniformBlockDesc *block = &reflection->uniform_blocks[i];
    vulkan_reflection_string_destroy(allocator, &block->name);
    for (uint32_t m = 0; m < block->member_count; ++m) {
      vulkan_reflection_string_destroy(allocator, &block->members[m].name);
    }
    if (block->members && block->member_count > 0) {
      vkr_allocator_free(allocator, block->members,
                         sizeof(VkrUniformMemberDesc) *
                             (uint64_t)block->member_count,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    }
  }

  if (reflection->uniform_blocks && reflection->uniform_block_count > 0) {
    vkr_allocator_free(allocator, reflection->uniform_blocks,
                       sizeof(VkrUniformBlockDesc) *
                           (uint64_t)reflection->uniform_block_count,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }

  MemZero(reflection, sizeof(*reflection));
}

bool8_t vulkan_spirv_shader_reflection_create(
    const VkrSpirvReflectionCreateInfo *create_info,
    VkrShaderReflection *out_reflection, VkrReflectionErrorContext *out_error) {
  if (!create_info || !create_info->allocator || !create_info->temp_allocator ||
      !create_info->modules || create_info->module_count == 0 ||
      !out_reflection ||
      create_info->temp_allocator == create_info->allocator) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_NULL_POINTER, string8_lit(""),
        string8_lit(""), VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }

  MemZero(out_reflection, sizeof(*out_reflection));
  vulkan_reflection_error_context_reset(out_error);
  if (out_error) {
    vulkan_reflection_error_context_set_string(
        create_info->program_name, out_error->program_name_storage,
        VKR_REFLECTION_ERROR_PROGRAM_NAME_MAX, &out_error->program_name);
  }

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(create_info->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    return false_v;
  }

  bool8_t is_success = false_v;
  VulkanSpirvReflectionModule *modules = vulkan_reflection_temp_alloc_zeroed(
      create_info, sizeof(*modules) * (uint64_t)create_info->module_count);
  if (!modules) {
    vulkan_reflection_set_error_ex(
        out_error, VKR_REFLECTION_ERROR_PARSE_FAILED, VK_SHADER_STAGE_ALL,
        string8_lit(""), SPV_REFLECT_RESULT_ERROR_ALLOC_FAILED,
        create_info->program_name, string8_lit(""),
        VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID,
        VKR_REFLECTION_INDEX_INVALID);
    goto cleanup;
  }

  VkShaderStageFlags seen_stages = 0;
  for (uint32_t i = 0; i < create_info->module_count; ++i) {
    const VkrShaderStageModuleDesc *desc = &create_info->modules[i];
    if (!vulkan_reflection_is_single_stage_flag(desc->stage)) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_STAGE_MISMATCH, desc->stage,
          vulkan_spirv_reflection_resolve_entry_point(desc->entry_point),
          SPV_REFLECT_RESULT_ERROR_SPIRV_INVALID_ENTRY_POINT,
          create_info->program_name, desc->path, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
      goto fail;
    }

    if ((seen_stages & desc->stage) != 0) {
      vulkan_reflection_set_error_ex(
          out_error, VKR_REFLECTION_ERROR_DUPLICATE_STAGE, desc->stage,
          vulkan_spirv_reflection_resolve_entry_point(desc->entry_point),
          SPV_REFLECT_RESULT_ERROR_COUNT_MISMATCH, create_info->program_name,
          desc->path, VKR_REFLECTION_INDEX_INVALID,
          VKR_REFLECTION_INDEX_INVALID, VKR_REFLECTION_INDEX_INVALID);
      goto fail;
    }

    if (!vulkan_spirv_reflection_module_create(
            desc->spirv_bytes, desc->spirv_size, desc->stage, desc->entry_point,
            &modules[i], out_error)) {
      if (out_error) {
        vulkan_reflection_error_context_set_string(
            create_info->program_name, out_error->program_name_storage,
            VKR_REFLECTION_ERROR_PROGRAM_NAME_MAX, &out_error->program_name);
        vulkan_reflection_error_context_set_string(
            desc->path, out_error->module_path_storage,
            VKR_REFLECTION_ERROR_MODULE_PATH_MAX, &out_error->module_path);
      }
      goto fail;
    }

    seen_stages |= desc->stage;
  }

  if (!vulkan_reflection_collect_descriptor_bindings(
          create_info, modules, out_reflection, out_error)) {
    goto fail;
  }

  if (!vulkan_reflection_collect_push_constants(create_info, modules,
                                                out_reflection, out_error)) {
    goto fail;
  }

  if (!vulkan_reflection_collect_vertex_inputs(create_info, modules,
                                               out_reflection, out_error)) {
    goto fail;
  }

  if (!vulkan_reflection_rebuild_vertex_bindings(create_info, out_reflection,
                                                 out_error)) {
    goto fail;
  }

  out_reflection->uniform_block_count = 0;
  out_reflection->uniform_blocks = NULL;
  is_success = true_v;

  goto cleanup;

fail:
  is_success = false_v;

cleanup:
  vulkan_reflection_destroy_modules(modules, create_info->module_count);
  if (!is_success) {
    vulkan_spirv_shader_reflection_destroy(create_info->allocator,
                                           out_reflection);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return is_success;
}

const char *vulkan_reflection_error_string(VkrReflectionError error) {
  switch (error) {
  case VKR_REFLECTION_OK:
    return "ok";
  case VKR_REFLECTION_ERROR_PARSE_FAILED:
    return "parse_failed";
  case VKR_REFLECTION_ERROR_DUPLICATE_STAGE:
    return "duplicate_stage";
  case VKR_REFLECTION_ERROR_ENTRY_POINT_NOT_FOUND:
    return "entry_point_not_found";
  case VKR_REFLECTION_ERROR_STAGE_MISMATCH:
    return "stage_mismatch";
  case VKR_REFLECTION_ERROR_BINDING_TYPE_MISMATCH:
    return "binding_type_mismatch";
  case VKR_REFLECTION_ERROR_BINDING_COUNT_MISMATCH:
    return "binding_count_mismatch";
  case VKR_REFLECTION_ERROR_BINDING_SIZE_MISMATCH:
    return "binding_size_mismatch";
  case VKR_REFLECTION_ERROR_UNSUPPORTED_DESCRIPTOR:
    return "unsupported_descriptor";
  case VKR_REFLECTION_ERROR_RUNTIME_ARRAY:
    return "runtime_array";
  case VKR_REFLECTION_ERROR_MISSING_LOCATION:
    return "missing_location";
  case VKR_REFLECTION_ERROR_VERTEX_COMPONENT_DECORATION:
    return "vertex_component_decoration";
  case VKR_REFLECTION_ERROR_DUPLICATE_VERTEX_LOCATION:
    return "duplicate_vertex_location";
  case VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT:
    return "unsupported_vertex_input";
  case VKR_REFLECTION_ERROR_PUSH_CONSTANT_ALIGNMENT:
    return "push_constant_alignment";
  case VKR_REFLECTION_ERROR_PUSH_CONSTANT_LIMIT:
    return "push_constant_limit";
  }

  return "unknown_reflection_error";
}
