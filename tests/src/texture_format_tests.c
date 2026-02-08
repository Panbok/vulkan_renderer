#include "texture_format_tests.h"

static void test_vulkan_image_format_bc7_mapping(void) {
  printf("  Running test_vulkan_image_format_bc7_mapping...\n");
  assert(vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_BC7_UNORM) ==
         VK_FORMAT_BC7_UNORM_BLOCK);
  assert(vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_BC7_SRGB) ==
         VK_FORMAT_BC7_SRGB_BLOCK);
  printf("  test_vulkan_image_format_bc7_mapping PASSED\n");
}

static void test_vulkan_image_format_astc_mapping(void) {
  printf("  Running test_vulkan_image_format_astc_mapping...\n");
  assert(vulkan_image_format_from_texture_format(
             VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM) ==
         VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
  assert(vulkan_image_format_from_texture_format(
             VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB) ==
         VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
  printf("  test_vulkan_image_format_astc_mapping PASSED\n");
}

bool32_t run_texture_format_tests() {
  printf("--- Starting Texture Format Tests ---\n");

  test_vulkan_image_format_bc7_mapping();
  test_vulkan_image_format_astc_mapping();

  printf("--- Texture Format Tests Completed ---\n");
  return true;
}
