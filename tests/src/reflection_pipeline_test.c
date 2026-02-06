#include "reflection_pipeline_test.h"

#include "containers/str.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
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
  FileError error =
      file_load_spirv_shader(&shader_path, allocator, out_shader_data,
                             out_shader_size);
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
    const uint32_t format_size = reflection_test_vk_format_size(attribute->format);
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

bool32_t run_reflection_pipeline_tests(void) {
  printf("--- Starting Reflection Pipeline Tests ---\n");

  test_reflection_world_program_success();
  test_reflection_duplicate_stage_rejected();
  test_reflection_missing_vertex_abi_rejected();
  test_reflection_repeated_create_destroy_cycle();
  test_reflection_missing_temp_allocator_rejected();

  printf("--- Reflection Pipeline Tests Completed ---\n");
  return true;
}
