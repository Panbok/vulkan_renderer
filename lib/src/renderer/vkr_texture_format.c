#include "renderer/vkr_renderer.h"

static const VkrTextureFormatInfo
    s_texture_format_info[VKR_TEXTURE_FORMAT_COUNT] = {
        [VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R8G8B8A8_UINT] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R8G8B8A8_SINT] = {4, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_BC7_UNORM] = {4, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_BC7_SRGB] = {4, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_BC5_UNORM] = {2, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM] = {4, 4, 4, 16, true_v,
                                                    false_v},
        [VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB] = {4, 4, 4, 16, true_v,
                                                   false_v},
        [VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM] = {4, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB] = {4, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM] = {2, 4, 4, 16, true_v, false_v},
        [VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT] = {4, 1, 1, 8, false_v,
                                                    false_v},
        [VKR_TEXTURE_FORMAT_R8_UNORM] = {1, 1, 1, 1, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R16_SFLOAT] = {1, 1, 1, 2, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R32_SFLOAT] = {1, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R32_UINT] = {1, 1, 1, 4, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R8G8_UNORM] = {2, 1, 1, 2, false_v, false_v},
        [VKR_TEXTURE_FORMAT_D16_UNORM] = {1, 1, 1, 2, false_v, true_v},
        [VKR_TEXTURE_FORMAT_D32_SFLOAT] = {1, 1, 1, 4, false_v, true_v},
        [VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT] = {2, 1, 1, 4, false_v, true_v},
        [VKR_TEXTURE_FORMAT_R32G32_UINT] = {2, 1, 1, 8, false_v, false_v},
        [VKR_TEXTURE_FORMAT_R16G16_SNORM] = {2, 1, 1, 4, false_v, false_v},
};

bool8_t vkr_texture_format_get_info(VkrTextureFormat format,
                                    VkrTextureFormatInfo *out_info) {
  if (!out_info || format < 0 || format >= VKR_TEXTURE_FORMAT_COUNT) {
    return false_v;
  }

  const VkrTextureFormatInfo info = s_texture_format_info[format];
  if (info.block_width == 0 || info.block_height == 0 ||
      info.bytes_per_block == 0 || info.channel_count == 0) {
    return false_v;
  }

  *out_info = info;
  return true_v;
}

uint64_t vkr_texture_format_region_size(VkrTextureFormat format, uint32_t width,
                                        uint32_t height) {
  VkrTextureFormatInfo info = {0};
  if (width == 0 || height == 0 ||
      !vkr_texture_format_get_info(format, &info)) {
    return 0;
  }

  const uint64_t blocks_x =
      ((uint64_t)width + info.block_width - 1u) / info.block_width;
  const uint64_t blocks_y =
      ((uint64_t)height + info.block_height - 1u) / info.block_height;
  if (blocks_x > UINT64_MAX / blocks_y) {
    return 0u;
  }
  const uint64_t block_count = blocks_x * blocks_y;
  if (block_count > UINT64_MAX / info.bytes_per_block) {
    return 0u;
  }
  return block_count * info.bytes_per_block;
}
