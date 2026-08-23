#include "metal_packet_abi_test.h"

#include "renderer/metal/vkr_metal_packet_abi.h"
#include "renderer/metal/vkr_metal_packet_renderer.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void test_metal_packet_host_abi_manifest(void) {
  printf("  Running test_metal_packet_host_abi_manifest...\n");
  assert(vkr_metal_packet_abi_validate_host());
  for (uint32_t record_index = 0;
       record_index < VKR_METAL_PACKET_ABI_RECORD_COUNT; ++record_index) {
    const VkrMetalPacketAbiRecord *record =
        vkr_metal_packet_abi_record((VkrMetalPacketAbiRecordId)record_index);
    assert(record);
    assert(record->host_name && record->host_name[0] != '\0');
    assert(record->shader_name && record->shader_name[0] != '\0');
    assert(record->field_count > 0);
    for (uint32_t field_index = 0; field_index < record->field_count;
         ++field_index) {
      const VkrMetalPacketAbiField *field = &record->fields[field_index];
      assert(field->host_name && field->host_name[0] != '\0');
      assert(field->shader_name && field->shader_name[0] != '\0');
      assert(field->expected_offset < record->expected_size);
      for (uint32_t prior = 0; prior < field_index; ++prior)
        assert(strcmp(field->shader_name, record->fields[prior].shader_name) !=
               0);
    }
  }
  assert(!vkr_metal_packet_abi_record(VKR_METAL_PACKET_ABI_RECORD_COUNT));
  printf("  test_metal_packet_host_abi_manifest PASSED\n");
}

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

static void test_metal_packet_transmission_coverage_abi(void) {
  printf("  Running test_metal_packet_transmission_coverage_abi...\n");
  assert(sizeof(VkrGpuDrawCompactionState) == 80u);
  assert(sizeof(VkrMetalPacketTransmissionDiagnostics) == 112u);
  assert(offsetof(VkrMetalPacketTransmissionDiagnostics, covered_pixels) ==
         sizeof(VkrGpuDrawCompactionState));
  const VkrMetalPacketAbiRecord *record = vkr_metal_packet_abi_record(
      VKR_METAL_PACKET_ABI_TRANSMISSION_COVERAGE_ROOT);
  assert(record && record->expected_size == 32u && record->field_count == 5u);
  record = vkr_metal_packet_abi_record(
      VKR_METAL_PACKET_ABI_TRANSMISSION_COMPACT_ROOT);
  assert(record && record->expected_size == 80u && record->field_count == 11u);
  printf("  test_metal_packet_transmission_coverage_abi PASSED\n");
}

static void test_metal_packet_timing_collapse_detector(void) {
  printf("  Running test_metal_packet_timing_collapse_detector...\n");
  assert(vkr_metal_packet_pass_timing_collapse_detected(0u, 1100u, 1000u));
  assert(vkr_metal_packet_pass_timing_collapse_detected(100u, 100u, 1000u));
  assert(vkr_metal_packet_pass_timing_collapse_detected(100u, 1100u, 849u));
  assert(!vkr_metal_packet_pass_timing_collapse_detected(100u, 1100u, 850u));
  assert(!vkr_metal_packet_pass_timing_collapse_detected(100u, 1100u, 1100u));
  VkrMetalPacketResult result = {.pass_timing_count = 2u};
  result.pass_timings[0].valid = true_v;
  result.pass_timings[1].valid = true_v;
  vkr_metal_packet_pass_timings_invalidate(&result);
  assert(!result.pass_timings[0].valid && !result.pass_timings[1].valid);
  printf("  test_metal_packet_timing_collapse_detector PASSED\n");
}

bool32_t run_metal_packet_abi_tests(void) {
  printf("--- Running Metal packet ABI tests... ---\n");
  test_metal_packet_host_abi_manifest();
  test_metal_packet_slang_draw_matrix_conversion();
  test_metal_packet_shader_minimum_alignment();
  test_metal_packet_transmission_coverage_abi();
  test_metal_packet_timing_collapse_detector();
  printf("--- Metal packet ABI tests completed. ---\n");
  return true_v;
}
