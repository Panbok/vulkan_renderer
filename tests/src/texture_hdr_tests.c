#include "texture_hdr_tests.h"

#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_texture_system.h"

#include <assert.h>
#include <stb_image_write.h>
#include <stdio.h>

/* Radiance sources only fed the removed image skies. Without an explicit
   rejection, source fallback would decode them as 8-bit sRGB-range texels. */
static void test_hdr_source_is_rejected(void) {
  printf("  Running test_hdr_source_is_rejected...\n");

  const char *path = "build/test_hdr_source_rejected.bin";
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
  system.allow_source_fallback = true_v;
  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(!vkr_texture_system_prepare_load_from_file(
      &system, string8_lit("build/test_hdr_source_rejected.bin"),
      VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);
  assert(prepared.upload_data == NULL);
  assert(prepared.upload_regions == NULL);

  arena_destroy(arena);
  const FilePath file_path = {
      .path = string8_lit("build/test_hdr_source_rejected.bin"),
      .type = FILE_PATH_TYPE_RELATIVE,
  };
  assert(file_remove(&file_path) == FILE_ERROR_NONE);
  printf("  test_hdr_source_is_rejected PASSED\n");
}

bool32_t run_texture_hdr_tests(void) {
  printf("--- Starting HDR Texture Tests ---\n");
  test_hdr_source_is_rejected();
  printf("--- HDR Texture Tests Completed ---\n");
  return true_v;
}
