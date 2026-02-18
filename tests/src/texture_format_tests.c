#include "texture_format_tests.h"

static bool32_t test_expect_vk_format(VkFormat actual, VkFormat expected,
                                      const char *label) {
  if (actual != expected) {
    printf("  FAIL: %s: expected 0x%x got 0x%x\n", label,
           (unsigned int)expected, (unsigned int)actual);
    return false_v;
  }
  return true_v;
}

static bool32_t test_vulkan_image_format_bc7_mapping(void) {
  printf("  Running test_vulkan_image_format_bc7_mapping...\n");
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_BC7_UNORM),
          VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_BC7_SRGB),
          VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB")) {
    return false_v;
  }
  printf("  test_vulkan_image_format_bc7_mapping PASSED\n");
  return true_v;
}

static bool32_t test_vulkan_image_format_astc_mapping(void) {
  printf("  Running test_vulkan_image_format_astc_mapping...\n");
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM),
          VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB),
          VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC_4x4_SRGB")) {
    return false_v;
  }
  printf("  test_vulkan_image_format_astc_mapping PASSED\n");
  return true_v;
}

static bool32_t test_vulkan_image_format_bc5_etc2_mapping(void) {
  printf("  Running test_vulkan_image_format_bc5_etc2_mapping...\n");
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_BC5_UNORM),
          VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM),
          VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB),
          VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, "ETC2_RGBA_SRGB")) {
    return false_v;
  }
  printf("  test_vulkan_image_format_bc5_etc2_mapping PASSED\n");
  return true_v;
}

static bool32_t test_vulkan_image_format_depth_mapping(void) {
  printf("  Running test_vulkan_image_format_depth_mapping...\n");
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(VKR_TEXTURE_FORMAT_D16_UNORM),
          VK_FORMAT_D16_UNORM, "D16_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_D32_SFLOAT),
          VK_FORMAT_D32_SFLOAT, "D32_SFLOAT")) {
    return false_v;
  }
  if (!test_expect_vk_format(
          vulkan_image_format_from_texture_format(
              VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT),
          VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT")) {
    return false_v;
  }
  printf("  test_vulkan_image_format_depth_mapping PASSED\n");
  return true_v;
}

bool32_t run_texture_format_tests(void) {
  printf("--- Starting Texture Format Tests ---\n");

  bool32_t ok = true_v;
  ok &= test_vulkan_image_format_bc7_mapping();
  ok &= test_vulkan_image_format_astc_mapping();
  ok &= test_vulkan_image_format_bc5_etc2_mapping();
  ok &= test_vulkan_image_format_depth_mapping();

  printf("--- Texture Format Tests Completed ---\n");
  return ok;
}
