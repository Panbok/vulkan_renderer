#include "texture_hdr_tests.h"

#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <stb_image_write.h>
#include <stdio.h>

static void test_hdr_prepared_load_content_probe_orientation_and_cleanup(void) {
  printf("  Running "
         "test_hdr_prepared_load_content_probe_orientation_and_cleanup...\n");

  const char *path = "build/test_hdr_content_probe.bin";
  float32_t pixels[4u * 2u * 3u] = {0};
  for (uint32_t x = 0u; x < 4u; ++x) {
    pixels[x * 3u + 0u] = 1.0f;
    pixels[(4u + x) * 3u + 2u] = 4.0f;
  }
  assert(stbi_write_hdr(path, 4, 2, 3, pixels) != 0);

  Arena *arena = arena_create(MB(1), MB(1));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrTextureSystem system = {0};
  system.strict_vkt_only_mode = true_v;
  system.allow_source_fallback = false_v;
  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(vkr_texture_system_prepare_load_from_file(
      &system, string8_lit("build/test_hdr_content_probe.bin"),
      VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(prepared.description.width == 4u);
  assert(prepared.description.height == 2u);
  assert(prepared.description.channels == VKR_TEXTURE_RGBA_CHANNELS);
  assert(prepared.description.format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT);
  assert(prepared.description.u_repeat_mode == VKR_TEXTURE_REPEAT_MODE_REPEAT);
  assert(prepared.description.v_repeat_mode ==
         VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE);
  assert(prepared.description.mip_filter == VKR_MIP_FILTER_NONE);
  assert(prepared.upload_data_size == 4u * 2u * 8u);
  assert(prepared.upload_region_count == 1u);
  assert(prepared.upload_mip_levels == 1u);

  const uint16_t *rgba16 = (const uint16_t *)prepared.upload_data;
  assert(rgba16[0u] == 0x3c00u);
  assert(rgba16[1u] == 0x0000u);
  assert(rgba16[2u] == 0x0000u);
  assert(rgba16[3u] == 0x3c00u);
  const uint32_t bottom_left = 4u * 4u;
  assert(rgba16[bottom_left + 0u] == 0x0000u);
  assert(rgba16[bottom_left + 1u] == 0x0000u);
  assert(rgba16[bottom_left + 2u] == 0x4400u);
  assert(rgba16[bottom_left + 3u] == 0x3c00u);

  vkr_texture_system_release_prepared_load(&prepared);
  assert(prepared.upload_data == NULL);
  assert(prepared.upload_regions == NULL);
  assert(prepared.upload_data_size == 0u);
  assert(prepared.description.width == 0u);

  arena_destroy(arena);
  const FilePath file_path = {
      .path = string8_lit("build/test_hdr_content_probe.bin"),
      .type = FILE_PATH_TYPE_RELATIVE,
  };
  assert(file_remove(&file_path) == FILE_ERROR_NONE);
  printf("  test_hdr_prepared_load_content_probe_orientation_and_cleanup "
         "PASSED\n");
}

static void test_hdr_prepared_load_rejects_non_2_to_1_extent(void) {
  printf("  Running test_hdr_prepared_load_rejects_non_2_to_1_extent...\n");
  const char *path = "build/test_hdr_invalid_aspect.hdr";
  float32_t pixels[3u * 2u * 3u] = {0};
  assert(stbi_write_hdr(path, 3, 2, 3, pixels) != 0);

  Arena *arena = arena_create(MB(1), MB(1));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrTextureSystem system = {0};
  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(!vkr_texture_system_prepare_load_from_file(
      &system, string8_lit("build/test_hdr_invalid_aspect.hdr"),
      VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);
  assert(prepared.upload_data == NULL);
  assert(prepared.upload_regions == NULL);

  arena_destroy(arena);
  const FilePath file_path = {
      .path = string8_lit("build/test_hdr_invalid_aspect.hdr"),
      .type = FILE_PATH_TYPE_RELATIVE,
  };
  assert(file_remove(&file_path) == FILE_ERROR_NONE);
  printf("  test_hdr_prepared_load_rejects_non_2_to_1_extent PASSED\n");
}

bool32_t run_texture_hdr_tests(void) {
  printf("--- Starting HDR Texture Tests ---\n");
  test_hdr_prepared_load_content_probe_orientation_and_cleanup();
  test_hdr_prepared_load_rejects_non_2_to_1_extent();
  printf("--- HDR Texture Tests Completed ---\n");
  return true_v;
}
