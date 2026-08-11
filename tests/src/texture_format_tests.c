#include "texture_format_tests.h"

static bool32_t test_hdr_format_metadata(void) {
  printf("  Running test_hdr_format_metadata...\\n");
  VkrTextureFormatInfo info = {0};
  assert(vkr_texture_format_get_info(VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
                                     &info));
  assert(info.channel_count == 4u);
  assert(info.block_width == 1u && info.block_height == 1u);
  assert(info.bytes_per_block == 8u);
  assert(!info.is_block_compressed && !info.is_depth_stencil);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
                                        4u, 2u) == 64u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
                                        UINT32_MAX, UINT32_MAX) == 0u);
  printf("  test_hdr_format_metadata PASSED\\n");
  return true_v;
}

static bool32_t test_compressed_format_region_sizes(void) {
  printf("  Running test_compressed_format_region_sizes...\\n");
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_BC7_UNORM, 4u, 4u) ==
         16u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_BC7_UNORM, 5u, 5u) ==
         64u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM, 1u,
                                        1u) == 16u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_COUNT, 1u, 1u) ==
         0u);
  printf("  test_compressed_format_region_sizes PASSED\\n");
  return true_v;
}

bool32_t run_texture_format_tests(void) {
  printf("--- Starting Texture Format Tests ---\\n");
  bool32_t ok = test_hdr_format_metadata();
  ok &= test_compressed_format_region_sizes();
  printf("--- Texture Format Tests Completed ---\\n");
  return ok;
}
