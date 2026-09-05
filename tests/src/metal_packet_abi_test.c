#include "metal_packet_abi_test.h"

#include "renderer/metal/vkr_metal_packet_abi.h"
#include "renderer/metal/vkr_metal_packet_renderer.h"

#include <assert.h>
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
  test_metal_packet_shader_minimum_alignment();
  printf("--- Metal packet ABI tests completed. ---\n");
  return true_v;
}
