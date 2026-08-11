#include "metal_packet_abi_test.h"

#include "renderer/metal/vkr_metal_packet_abi.h"

#include <assert.h>
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

bool32_t run_metal_packet_abi_tests(void) {
  printf("--- Running Metal packet ABI tests... ---\n");
  test_metal_packet_host_abi_manifest();
  test_metal_packet_slang_draw_matrix_conversion();
  printf("--- Metal packet ABI tests completed. ---\n");
  return true_v;
}
