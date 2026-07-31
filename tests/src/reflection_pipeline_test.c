#include "reflection_pipeline_test.h"

#include "containers/str.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/shader_loader.h"
#include "renderer/vulkan/vulkan_shaders.h"
#include "renderer/vulkan/vulkan_spirv_reflection.h"

#include <assert.h>
#include <stdio.h>

vkr_internal void reflection_test_load_spirv(VkrAllocator *allocator,
                                             const char *relative_path,
                                             uint8_t **out_shader_data,
                                             uint64_t *out_shader_size) {
  assert(allocator != NULL);
  assert(relative_path != NULL);
  assert(out_shader_data != NULL);
  assert(out_shader_size != NULL);

  *out_shader_data = NULL;
  *out_shader_size = 0;

  FilePath shader_path =
      file_path_create(relative_path, allocator, FILE_PATH_TYPE_RELATIVE);
  FileError error = file_load_spirv_shader(&shader_path, allocator,
                                           out_shader_data, out_shader_size);
  assert(error == FILE_ERROR_NONE);
  assert(*out_shader_data != NULL);
  assert(*out_shader_size > 0);
}

vkr_internal uint32_t reflection_test_vk_format_size(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R32_SFLOAT:
  case VK_FORMAT_R32_SINT:
  case VK_FORMAT_R32_UINT:
    return 4;
  case VK_FORMAT_R32G32_SFLOAT:
  case VK_FORMAT_R32G32_SINT:
  case VK_FORMAT_R32G32_UINT:
    return 8;
  case VK_FORMAT_R32G32B32_SFLOAT:
  case VK_FORMAT_R32G32B32_SINT:
  case VK_FORMAT_R32G32B32_UINT:
    return 12;
  case VK_FORMAT_R32G32B32A32_SFLOAT:
  case VK_FORMAT_R32G32B32A32_SINT:
  case VK_FORMAT_R32G32B32A32_UINT:
    return 16;
  default:
    return 0;
  }
}

vkr_internal const VkrVertexInputBindingDesc *
reflection_test_find_binding(const VkrShaderReflection *reflection,
                             uint32_t binding_index) {
  if (!reflection) {
    return NULL;
  }

  for (uint32_t i = 0; i < reflection->vertex_binding_count; ++i) {
    if (reflection->vertex_bindings[i].binding == binding_index) {
      return &reflection->vertex_bindings[i];
    }
  }
  return NULL;
}

vkr_internal const VkrDescriptorSetDesc *
reflection_test_find_descriptor_set(const VkrShaderReflection *reflection,
                                    uint32_t set_index) {
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

vkr_internal const VkrDescriptorBindingDesc *
reflection_test_find_descriptor_binding(const VkrDescriptorSetDesc *set_desc,
                                        uint32_t binding_index) {
  if (!set_desc) {
    return NULL;
  }

  for (uint32_t i = 0; i < set_desc->binding_count; ++i) {
    if (set_desc->bindings[i].binding == binding_index) {
      return &set_desc->bindings[i];
    }
  }

  return NULL;
}

vkr_internal void test_reflection_world_program_success(void) {
  printf("  Running test_reflection_world_program_success...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(2), MB(2));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/default.world.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/default.world.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = string8_lit("assets/shaders/default.world.spv"),
          .entry_point = string8_lit("fragmentMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.default.world"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_3D,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == true_v);
  assert(error.code == VKR_REFLECTION_OK);

  assert(reflection.set_count > 0);
  assert(reflection.layout_set_count >= reflection.set_count);
  assert(reflection.vertex_binding_count > 0);
  assert(reflection.vertex_attribute_count > 0);

  uint32_t previous_location = 0;
  for (uint32_t i = 0; i < reflection.vertex_attribute_count; ++i) {
    const VkrVertexInputAttributeDesc *attribute =
        &reflection.vertex_attributes[i];
    if (i > 0) {
      assert(attribute->location > previous_location);
    }
    previous_location = attribute->location;

    const VkrVertexInputBindingDesc *binding =
        reflection_test_find_binding(&reflection, attribute->binding);
    assert(binding != NULL);
    const uint32_t format_size =
        reflection_test_vk_format_size(attribute->format);
    assert(format_size > 0);
    assert(attribute->offset + format_size <= binding->stride);
  }

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_world_program_success PASSED\n");
}

vkr_internal void test_reflection_pbr_world_program_layout(void) {
  printf("  Running test_reflection_pbr_world_program_layout...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(2), MB(2));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/pbr.world.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/pbr.world.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = string8_lit("assets/shaders/pbr.world.spv"),
          .entry_point = string8_lit("fragmentMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.pbr.world"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_3D,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == true_v);
  assert(error.code == VKR_REFLECTION_OK);
  assert(reflection.vertex_binding_count > 0);
  assert(reflection.vertex_attribute_count > 0);

  const VkrDescriptorSetDesc *set0 =
      reflection_test_find_descriptor_set(&reflection, 0);
  const VkrDescriptorSetDesc *set1 =
      reflection_test_find_descriptor_set(&reflection, 1);
  assert(set0 != NULL);
  assert(set1 != NULL);

  const VkrDescriptorBindingDesc *set0_globals =
      reflection_test_find_descriptor_binding(set0, 0);
  const VkrDescriptorBindingDesc *set0_instances =
      reflection_test_find_descriptor_binding(set0, 1);
  assert(set0_globals != NULL);
  assert(set0_globals->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
  assert(set0_instances != NULL);
  assert(set0_instances->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

  const VkrDescriptorBindingDesc *set1_local =
      reflection_test_find_descriptor_binding(set1, 0);
  assert(set1_local != NULL);
  assert(set1_local->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

  for (uint32_t binding = 1; binding <= 11; ++binding) {
    const VkrDescriptorBindingDesc *desc =
        reflection_test_find_descriptor_binding(set1, binding);
    assert(desc != NULL);
    assert(desc->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  }

  for (uint32_t binding = 12; binding <= 22; ++binding) {
    const VkrDescriptorBindingDesc *desc =
        reflection_test_find_descriptor_binding(set1, binding);
    assert(desc != NULL);
    assert(desc->type == VK_DESCRIPTOR_TYPE_SAMPLER);
  }

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_pbr_world_program_layout PASSED\n");
}

vkr_internal void test_reflection_duplicate_stage_rejected(void) {
  printf("  Running test_reflection_duplicate_stage_rejected...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(1), MB(1));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/picking.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/picking.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/picking.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.duplicate.stage"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_UNKNOWN,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == false_v);
  assert(error.code == VKR_REFLECTION_ERROR_DUPLICATE_STAGE);

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_duplicate_stage_rejected PASSED\n");
}

vkr_internal void test_reflection_missing_vertex_abi_rejected(void) {
  printf("  Running test_reflection_missing_vertex_abi_rejected...\n");
  Arena *arena = arena_create(MB(4), MB(4));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(2), MB(2));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/default.world.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/default.world.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = string8_lit("assets/shaders/default.world.spv"),
          .entry_point = string8_lit("fragmentMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.missing.vertex.abi"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_UNKNOWN,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == false_v);
  assert(error.code == VKR_REFLECTION_ERROR_UNSUPPORTED_VERTEX_INPUT);

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_missing_vertex_abi_rejected PASSED\n");
}

vkr_internal void test_reflection_repeated_create_destroy_cycle(void) {
  printf("  Running test_reflection_repeated_create_destroy_cycle...\n");
  Arena *arena = arena_create(MB(8), MB(8));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(2), MB(2));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/default.text.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/default.text.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = string8_lit("assets/shaders/default.text.spv"),
          .entry_point = string8_lit("fragmentMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.repeated.reflection.cycle"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_TEXT_2D,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  for (uint32_t i = 0; i < 64; ++i) {
    VkrShaderReflection reflection = {0};
    VkrReflectionErrorContext error = {0};
    assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                                 &error) == true_v);
    assert(error.code == VKR_REFLECTION_OK);
    vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  }

  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_repeated_create_destroy_cycle PASSED\n");
}

vkr_internal void test_reflection_missing_temp_allocator_rejected(void) {
  printf("  Running test_reflection_missing_temp_allocator_rejected...\n");
  Arena *arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/picking.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[1] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/picking.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = NULL,
      .program_name = string8_lit("test.missing.temp.allocator"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_UNKNOWN,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };

  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == false_v);
  assert(error.code == VKR_REFLECTION_ERROR_PARSE_FAILED);

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(arena);
  printf("  test_reflection_missing_temp_allocator_rejected PASSED\n");
}

/**
 * The manifest's uniform offsets are what the frontend writes to; SPIR-V's are
 * what the GPU reads from. Nothing at runtime compares them, so a manifest that
 * drifts from its shader writes well-formed bytes to the wrong place. This
 * checks the real shipped pair, which is also what shader creation now enforces
 * -- so a regression fails here rather than at app startup.
 */
vkr_internal void test_reflection_shadercfg_matches_spirv(void) {
  printf("  Running test_reflection_shadercfg_matches_spirv...\n");
  Arena *arena = arena_create(MB(8), MB(8));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);
  Arena *temp_arena = arena_create(MB(4), MB(4));
  VkrAllocator temp_allocator = {.ctx = temp_arena};
  vkr_allocator_arena(&temp_allocator);

  VkrShaderConfig config = {0};
  VkrShaderConfigParseResult parse_result =
      vkr_shader_loader_parse(string8_lit("assets/shaders/pbr.world.shadercfg"),
                              &allocator, &temp_allocator, &config);
  assert(parse_result.is_valid);
  assert(config.uniform_count > 0);

  uint8_t *shader_data = NULL;
  uint64_t shader_size = 0;
  reflection_test_load_spirv(&allocator, "assets/shaders/pbr.world.spv",
                             &shader_data, &shader_size);

  VkrShaderStageModuleDesc modules[2] = {
      {
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .path = string8_lit("assets/shaders/pbr.world.spv"),
          .entry_point = string8_lit("vertexMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
      {
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = string8_lit("assets/shaders/pbr.world.spv"),
          .entry_point = string8_lit("fragmentMain"),
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      },
  };

  VkrShaderReflection reflection = {0};
  VkrReflectionErrorContext error = {0};
  VkrSpirvReflectionCreateInfo create_info = {
      .allocator = &allocator,
      .temp_allocator = &temp_allocator,
      .program_name = string8_lit("test.pbr.world"),
      .vertex_abi_profile = VKR_VERTEX_ABI_PROFILE_3D,
      .module_count = ArrayCount(modules),
      .modules = modules,
      .max_push_constant_size = 0,
  };
  assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                               &error) == true_v);

  // Uniform blocks are now actually reflected; this was hardcoded to zero.
  assert(reflection.uniform_block_count > 0);
  assert(reflection.uniform_blocks != NULL);

  // Exercise the shipped validator, not a copy of its rules: this is the exact
  // check shader creation performs, so a green assert here means the app will
  // not reject its own shader at startup.
  assert(vulkan_shader_validate_uniform_layout(
             &reflection, config.uniforms.data, config.uniform_count,
             string8_lit("test.pbr.world")) == true_v);

  uint32_t compared = 0;
  for (uint32_t i = 0; i < config.uniform_count; ++i) {
    const VkrShaderUniformDesc *declared =
        array_get_VkrShaderUniformDesc(&config.uniforms, i);
    if (declared->type != SHADER_UNIFORM_TYPE_SAMPLER &&
        declared->scope != VKR_SHADER_SCOPE_LOCAL) {
      compared++;
    }
  }
  assert(compared > 0);

  // Matching names and offsets are insufficient: a host vec4 cannot stand in
  // for the shader's mat4 even when the stale manifest size is left unchanged.
  for (uint32_t i = 0; i < config.uniform_count; ++i) {
    VkrShaderUniformDesc *declared =
        array_get_VkrShaderUniformDesc(&config.uniforms, i);
    if (declared->type != SHADER_UNIFORM_TYPE_MATRIX_4 ||
        declared->array_count != 1) {
      continue;
    }
    declared->type = SHADER_UNIFORM_TYPE_FLOAT32_4;
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, config.uniforms.data, config.uniform_count,
               string8_lit("test.pbr.world")) == false_v);
    declared->type = SHADER_UNIFORM_TYPE_MATRIX_4;
    break;
  }

  // Array cardinality is part of the ABI even when total block size still
  // happens to agree.
  for (uint32_t i = 0; i < config.uniform_count; ++i) {
    VkrShaderUniformDesc *declared =
        array_get_VkrShaderUniformDesc(&config.uniforms, i);
    if (declared->array_count <= 1) {
      continue;
    }
    const uint32_t original_count = declared->array_count;
    declared->array_count--;
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, config.uniforms.data, config.uniform_count,
               string8_lit("test.pbr.world")) == false_v);
    declared->array_count = original_count;
    break;
  }

  // A drifted offset must be caught. Perturb one entry and confirm rejection,
  // otherwise the validator could be vacuously passing.
  for (uint32_t i = 0; i < config.uniform_count; ++i) {
    VkrShaderUniformDesc *declared =
        array_get_VkrShaderUniformDesc(&config.uniforms, i);
    if (declared->type == SHADER_UNIFORM_TYPE_SAMPLER ||
        declared->scope == VKR_SHADER_SCOPE_LOCAL) {
      continue;
    }
    const uint32_t original_offset = declared->offset;
    declared->offset = original_offset + 4;
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, config.uniforms.data, config.uniform_count,
               string8_lit("test.pbr.world")) == false_v);
    declared->offset = original_offset;
    break;
  }

  // An undeclared name must be caught too.
  {
    VkrShaderUniformDesc bogus = {
        .type = SHADER_UNIFORM_TYPE_FLOAT32,
        .scope = VKR_SHADER_SCOPE_GLOBAL,
        .name = string8_lit("uniform_that_does_not_exist"),
        .offset = 0,
        .size = 4,
        .array_count = 1,
    };
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, &bogus, 1, string8_lit("test.pbr.world")) ==
           false_v);
  }

  // A same-named member in another descriptor scope must not satisfy the
  // declaration. Runtime binding maps global and instance UBOs independently.
  {
    VkrShaderUniformDesc wrong_scope = {
        .type = SHADER_UNIFORM_TYPE_MATRIX_4,
        .scope = VKR_SHADER_SCOPE_INSTANCE,
        .name = string8_lit("projection"),
        .offset = 0,
        .size = sizeof(Mat4),
        .array_count = 1,
    };
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, &wrong_scope, 1, string8_lit("test.pbr.world")) ==
           false_v);
  }

  vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
  vkr_allocator_free(&allocator, shader_data, shader_size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  arena_destroy(temp_arena);
  arena_destroy(arena);
  printf("  test_reflection_shadercfg_matches_spirv PASSED (%u uniforms)\n",
         compared);
}

/**
 * Shader creation now rejects a manifest that disagrees with its SPIR-V, so a
 * drifted `.shadercfg` is an app that will not start. Sweep every shipped
 * shader here rather than discovering it at launch.
 */
vkr_internal void test_reflection_all_shadercfg_match_spirv(void) {
  printf("  Running test_reflection_all_shadercfg_match_spirv...\n");

  static const char *manifests[] = {
      "assets/shaders/default.skybox.shadercfg",
      "assets/shaders/default.text.shadercfg",
      "assets/shaders/default.ui.shadercfg",
      "assets/shaders/default.viewport_display.shadercfg",
      "assets/shaders/default.world_text.shadercfg",
      "assets/shaders/default.world.shadercfg",
      "assets/shaders/ibl.diffuse_convolution.shadercfg",
      "assets/shaders/ibl.specular_prefilter.shadercfg",
      "assets/shaders/pbr.world.shadercfg",
      "assets/shaders/picking_text.shadercfg",
      "assets/shaders/picking.shadercfg",
      "assets/shaders/shadow_opaque.shadercfg",
      "assets/shaders/shadow.shadercfg",
  };

  for (uint32_t i = 0; i < ArrayCount(manifests); ++i) {
    Arena *arena = arena_create(MB(8), MB(8));
    VkrAllocator allocator = {.ctx = arena};
    vkr_allocator_arena(&allocator);
    Arena *temp_arena = arena_create(MB(4), MB(4));
    VkrAllocator temp_allocator = {.ctx = temp_arena};
    vkr_allocator_arena(&temp_allocator);

    VkrShaderConfig config = {0};
    String8 manifest_path = string8_create_from_cstr(
        (const uint8_t *)manifests[i], string_length(manifests[i]));
    VkrShaderConfigParseResult parse_result = vkr_shader_loader_parse(
        manifest_path, &allocator, &temp_allocator, &config);
    assert(parse_result.is_valid);
    assert(config.stage_count > 0);

    // Every shipped manifest uses a single multi-entry SPIR-V module.
    VkrShaderStageFile first_stage =
        *array_get_VkrShaderStageFile(&config.stages, 0);
    char spv_path[512] = {0};
    assert(first_stage.filename.length < sizeof(spv_path));
    MemCopy(spv_path, first_stage.filename.str, first_stage.filename.length);

    uint8_t *shader_data = NULL;
    uint64_t shader_size = 0;
    reflection_test_load_spirv(&allocator, spv_path, &shader_data,
                               &shader_size);

    VkrShaderStageModuleDesc modules[VKR_SHADER_STAGE_COUNT] = {0};
    uint32_t module_count = 0;
    for (uint32_t stage_index = 0; stage_index < config.stage_count;
         ++stage_index) {
      VkrShaderStageFile stage_file =
          *array_get_VkrShaderStageFile(&config.stages, stage_index);
      modules[module_count] = (VkrShaderStageModuleDesc){
          .stage = stage_file.stage == VKR_SHADER_STAGE_VERTEX
                       ? VK_SHADER_STAGE_VERTEX_BIT
                       : VK_SHADER_STAGE_FRAGMENT_BIT,
          .path = stage_file.filename,
          .entry_point = stage_file.entry_point,
          .spirv_bytes = shader_data,
          .spirv_size = shader_size,
      };
      module_count++;
    }

    VkrShaderReflection reflection = {0};
    VkrReflectionErrorContext error = {0};
    VkrSpirvReflectionCreateInfo create_info = {
        .allocator = &allocator,
        .temp_allocator = &temp_allocator,
        .program_name = config.name,
        .vertex_abi_profile = config.vertex_abi_profile,
        .module_count = module_count,
        .modules = modules,
        .max_push_constant_size = 0,
    };
    assert(vulkan_spirv_shader_reflection_create(&create_info, &reflection,
                                                 &error) == true_v);
    assert(vulkan_shader_validate_uniform_layout(
               &reflection, config.uniforms.data, config.uniform_count,
               config.name) == true_v);

    vulkan_spirv_shader_reflection_destroy(&allocator, &reflection);
    vkr_allocator_free(&allocator, shader_data, shader_size,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
    arena_destroy(temp_arena);
    arena_destroy(arena);
  }

  printf("  test_reflection_all_shadercfg_match_spirv PASSED (%u shaders)\n",
         (uint32_t)ArrayCount(manifests));
}

bool32_t run_reflection_pipeline_tests(void) {
  printf("--- Starting Reflection Pipeline Tests ---\n");

  test_reflection_world_program_success();
  test_reflection_pbr_world_program_layout();
  test_reflection_duplicate_stage_rejected();
  test_reflection_missing_vertex_abi_rejected();
  test_reflection_repeated_create_destroy_cycle();
  test_reflection_missing_temp_allocator_rejected();
  test_reflection_shadercfg_matches_spirv();
  test_reflection_all_shadercfg_match_spirv();

  printf("--- Reflection Pipeline Tests Completed ---\n");
  return true;
}
