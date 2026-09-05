#include "metal_packet_abi_test.h"

#include "renderer/metal/vkr_metal_packet_abi.h"
#include "renderer/metal/vkr_metal_packet_renderer.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void test_metal_packet_slang_draw_matrix_conversion(void) {
  printf("  Running test_metal_packet_slang_draw_matrix_conversion...\n");
  Mat4 source = {0};
  for (uint32_t i = 0; i < 16u; ++i)
    source.elements[i] = (float32_t)(i + 1u);

  const Mat4 converted = vkr_metal_packet_slang_draw_matrix(source);
  for (uint32_t column = 0; column < 4u; ++column) {
    for (uint32_t row = 0; row < 4u; ++row) {
      assert(converted.elements[column * 4u + row] ==
             source.elements[row * 4u + column]);
    }
  }
  printf("  test_metal_packet_slang_draw_matrix_conversion PASSED\n");
}

static Vec3 prepared_normal(const VkrPreparedInstanceGPU *instance, Vec3 normal) {
  return (Vec3){
      instance->normal_column0.x * normal.x +
          instance->normal_column1.x * normal.y +
          instance->normal_column2.x * normal.z,
      instance->normal_column0.y * normal.x +
          instance->normal_column1.y * normal.y +
          instance->normal_column2.y * normal.z,
      instance->normal_column0.z * normal.x +
          instance->normal_column1.z * normal.y +
          instance->normal_column2.z * normal.z,
  };
}

static void test_prepared_instance_normal_orthogonality(void) {
  printf("  Running test_prepared_instance_normal_orthogonality...\n");
  VkrInstanceDataGPU source = {.model = mat4_identity()};
  source.model.m00 = 2.0f;
  source.model.m01 = 1.0f;
  source.model.m12 = 1.0f;
  source.model.m22 = 0.5f;
  const VkrPreparedInstanceGPU prepared = vkr_gpu_prepare_instance(&source);
  const Vec3 normal = prepared_normal(&prepared, (Vec3){1.0f, 1.0f, 1.0f});
  /* The original plane's tangents (1,-1,0) and (0,1,-1) become
     (1,-1,0) and (1,0,-.5). Its transported normal must annihilate both. */
  assert(fabsf(normal.x - normal.y) < 1e-6f);
  assert(fabsf(normal.x - 0.5f * normal.z) < 1e-6f);
  assert(normal.z > 0.0f);

  source.model = mat4_identity();
  source.model.m00 = -2.0f;
  source.model.m22 = 0.5f;
  const VkrPreparedInstanceGPU mirrored = vkr_gpu_prepare_instance(&source);
  const Vec3 mirror_normal =
      prepared_normal(&mirrored, (Vec3){1.0f, 1.0f, 0.0f});
  assert(fabsf(-2.0f * mirror_normal.x - mirror_normal.y) < 1e-6f);
  assert(mirror_normal.x < 0.0f && mirror_normal.y > 0.0f);
  assert(mirrored.normal_column0.w == -1.0f);

  source.model = mat4_identity();
  source.model.m00 = source.model.m11 = source.model.m22 = 1e-12f;
  const VkrPreparedInstanceGPU tiny = vkr_gpu_prepare_instance(&source);
  const Vec3 tiny_normal = prepared_normal(&tiny, (Vec3){0.0f, 0.0f, 1.0f});
  assert(tiny_normal.x == 0.0f && tiny_normal.y == 0.0f && tiny_normal.z == 1.0f);
  printf("  test_prepared_instance_normal_orthogonality PASSED\n");
}

static void test_metal_packet_shader_minimum_alignment(void) {
  printf("  Running test_metal_packet_shader_minimum_alignment...\n");
  assert(vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_DRAW_ROOT, 16u));
  assert(vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_DRAW_ROOT, 8u));
  assert(vkr_metal_packet_abi_alignment_compatible(VKR_METAL_PACKET_ABI_VERTEX,
                                                   4u));
  assert(!vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_DRAW_ROOT, 32u));
  assert(!vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_DRAW_ROOT, 3u));
  assert(!vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_DRAW_ROOT, 0u));
  assert(!vkr_metal_packet_abi_alignment_compatible(
      VKR_METAL_PACKET_ABI_RECORD_COUNT, 16u));
  printf("  test_metal_packet_shader_minimum_alignment PASSED\n");
}

bool32_t run_metal_packet_abi_tests(void) {
  printf("--- Running Metal packet ABI tests... ---\n");
  test_metal_packet_slang_draw_matrix_conversion();
  test_prepared_instance_normal_orthogonality();
  test_metal_packet_shader_minimum_alignment();
  printf("--- Metal packet ABI tests completed. ---\n");
  return true_v;
}
