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
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM),
                             VK_FORMAT_ASTC_4x4_UNORM_BLOCK,
                             "ASTC_4x4_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
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
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM),
                             VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                             "ETC2_RGBA_UNORM")) {
    return false_v;
  }
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB),
                             VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK,
                             "ETC2_RGBA_SRGB")) {
    return false_v;
  }
  // The compressed two-channel normal-map target on ETC2-class devices.
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM),
                             VK_FORMAT_EAC_R11G11_UNORM_BLOCK,
                             "EAC_R11G11_UNORM")) {
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
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_D32_SFLOAT),
                             VK_FORMAT_D32_SFLOAT, "D32_SFLOAT")) {
    return false_v;
  }
  if (!test_expect_vk_format(vulkan_image_format_from_texture_format(
                                 VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT),
                             VK_FORMAT_D24_UNORM_S8_UINT,
                             "D24_UNORM_S8_UINT")) {
    return false_v;
  }
  printf("  test_vulkan_image_format_depth_mapping PASSED\n");
  return true_v;
}

static bool32_t test_hdr_format_metadata_and_mapping(void) {
  printf("  Running test_hdr_format_metadata_and_mapping...\n");
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
  assert(vulkan_image_format_from_texture_format(
             VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT) ==
         VK_FORMAT_R16G16B16A16_SFLOAT);
  assert(
      vulkan_texture_format_from_image_format(VK_FORMAT_R16G16B16A16_SFLOAT) ==
      VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT);
  printf("  test_hdr_format_metadata_and_mapping PASSED\n");
  return true_v;
}

static bool32_t test_compressed_format_region_sizes(void) {
  printf("  Running test_compressed_format_region_sizes...\n");
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_BC7_UNORM, 4u, 4u) ==
         16u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_BC7_UNORM, 5u, 5u) ==
         64u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM, 1u,
                                        1u) == 16u);
  assert(vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_COUNT, 1u, 1u) ==
         0u);
  printf("  test_compressed_format_region_sizes PASSED\n");
  return true_v;
}

static bool32_t test_vulkan_storage_image_usage_mapping(void) {
  printf("  Running test_vulkan_storage_image_usage_mapping...\n");
  VkrTextureUsageFlags usage = vkr_texture_usage_flags_from_bits(
      VKR_TEXTURE_USAGE_SAMPLED | VKR_TEXTURE_USAGE_STORAGE);
  VkImageUsageFlags vk_usage = vulkan_image_usage_from_texture_usage(usage);
  if ((vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 ||
      (vk_usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0) {
    printf(
        "  FAIL: sampled/storage texture usage lost during Vulkan mapping\n");
    return false_v;
  }
  printf("  test_vulkan_storage_image_usage_mapping PASSED\n");
  return true_v;
}

static bool32_t test_vulkan_attachment_subresource_view_selection(void) {
  printf("  Running test_vulkan_attachment_subresource_view_selection...\n");

  if (vulkan_attachment_needs_subresource_view(1, 1, 0, 0, 1)) {
    printf(
        "  FAIL: whole single-layer attachment should use its default view\n");
    return false_v;
  }
  if (!vulkan_attachment_needs_subresource_view(1, 4, 0, 0, 1)) {
    printf("  FAIL: array layer 0 must use a one-layer subresource view\n");
    return false_v;
  }
  if (!vulkan_attachment_needs_subresource_view(1, 4, 0, 1, 1)) {
    printf("  FAIL: nonzero array layer must use a subresource view\n");
    return false_v;
  }
  if (vulkan_attachment_needs_subresource_view(1, 4, 0, 0, 4)) {
    printf("  FAIL: whole array attachment should use its default view\n");
    return false_v;
  }
  if (!vulkan_attachment_needs_subresource_view(4, 1, 0, 0, 1)) {
    printf("  FAIL: a multi-mip image needs a single-mip attachment view\n");
    return false_v;
  }

  printf("  test_vulkan_attachment_subresource_view_selection PASSED\n");
  return true_v;
}

bool32_t run_texture_format_tests(void) {
  printf("--- Starting Texture Format Tests ---\n");

  bool32_t ok = true_v;
  ok &= test_vulkan_image_format_bc7_mapping();
  ok &= test_vulkan_image_format_astc_mapping();
  ok &= test_vulkan_image_format_bc5_etc2_mapping();
  ok &= test_vulkan_image_format_depth_mapping();
  ok &= test_hdr_format_metadata_and_mapping();
  ok &= test_compressed_format_region_sizes();
  ok &= test_vulkan_storage_image_usage_mapping();
  ok &= test_vulkan_attachment_subresource_view_selection();

  printf("--- Texture Format Tests Completed ---\n");
  return ok;
}
