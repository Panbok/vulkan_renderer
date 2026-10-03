#include "texture_vkt_tests.h"
#include "vkr_vkt_mips.h"
#include "vkr_vkt_normal_roughness.h"
#include "vkr_vkt_pack_contract.h"

#include <ktx.h>
#include <vulkan/vulkan_core.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static bool8_t string8_equals_cstr(String8 value, const char *cstr) {
  if (!cstr) {
    return false_v;
  }
  const uint64_t len = string_length(cstr);
  if (value.length != len) {
    return false_v;
  }
  return MemCompare(value.str, cstr, len) == 0;
}

static bool8_t texture_vkt_test_make_dir(const char *path) {
#if defined(_WIN32)
  return _mkdir(path) == 0 || errno == EEXIST ? true_v : false_v;
#else
  return mkdir(path, 0755) == 0 || errno == EEXIST ? true_v : false_v;
#endif
}

static bool8_t texture_vkt_test_write_rgba16f_cube(const char *path) {
  const ktxTextureCreateInfo create_info = {
      .vkFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
      .baseWidth = 2u,
      .baseHeight = 2u,
      .baseDepth = 1u,
      .numDimensions = 2u,
      .numLevels = 1u,
      .numLayers = 1u,
      .numFaces = 6u,
      .isArray = KTX_FALSE,
      .generateMipmaps = KTX_FALSE,
  };
  ktxTexture2 *texture = NULL;
  ktxResult result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    return false_v;
  }

  uint16_t pixels[2u * 2u * VKR_TEXTURE_RGBA_CHANNELS];
  for (uint32_t face = 0u; face < 6u; ++face) {
    for (uint32_t pixel = 0u; pixel < 4u; ++pixel) {
      pixels[pixel * VKR_TEXTURE_RGBA_CHANNELS + 0u] =
          (uint16_t)(0x3c00u + face);
      pixels[pixel * VKR_TEXTURE_RGBA_CHANNELS + 1u] =
          (uint16_t)(0x4000u + face);
      pixels[pixel * VKR_TEXTURE_RGBA_CHANNELS + 2u] =
          (uint16_t)(0x4200u + face);
      pixels[pixel * VKR_TEXTURE_RGBA_CHANNELS + 3u] = 0x3c00u;
    }
    result =
        ktxTexture_SetImageFromMemory(ktxTexture(texture), 0u, 0u, face,
                                      (const uint8_t *)pixels, sizeof(pixels));
    if (result != KTX_SUCCESS) {
      ktxTexture_Destroy(ktxTexture(texture));
      return false_v;
    }
  }
  result = ktxTexture_WriteToNamedFile(ktxTexture(texture), path);
  ktxTexture_Destroy(ktxTexture(texture));
  return result == KTX_SUCCESS ? true_v : false_v;
}

static bool8_t texture_vkt_test_write_truncated_copy(const char *source,
                                                     const char *destination) {
  FILE *input = fopen(source, "rb");
  if (!input || fseek(input, 0, SEEK_END) != 0) {
    if (input) {
      fclose(input);
    }
    return false_v;
  }
  const long size = ftell(input);
  if (size <= 1 || fseek(input, 0, SEEK_SET) != 0) {
    fclose(input);
    return false_v;
  }
  uint8_t *bytes = malloc((size_t)size);
  if (!bytes || fread(bytes, 1u, (size_t)size, input) != (size_t)size) {
    free(bytes);
    fclose(input);
    return false_v;
  }
  fclose(input);

  FILE *output = fopen(destination, "wb");
  const bool8_t written = output && fwrite(bytes, 1u, (size_t)size - 1u,
                                           output) == (size_t)size - 1u;
  if (output) {
    fclose(output);
  }
  free(bytes);
  return written;
}

/* Face markers make a cubemap layer permutation visible; a white HDR
 * environment would not reveal a wrong KTX face-to-array-layer mapping. */
static void test_texture_ktx2_rgba16f_cube_decode(void) {
  printf("  Running test_texture_ktx2_rgba16f_cube_decode...\n");

  char tmp_dir[1024];
  snprintf(tmp_dir, sizeof(tmp_dir), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(texture_vkt_test_make_dir(tmp_dir));
  char source_path[1024];
  char truncated_path[1024];
  snprintf(source_path, sizeof(source_path), "%s/rgba16f-cube.vkt", tmp_dir);
  snprintf(truncated_path, sizeof(truncated_path),
           "%s/rgba16f-cube-truncated.vkt", tmp_dir);
  remove(source_path);
  remove(truncated_path);
  assert(texture_vkt_test_write_rgba16f_cube(source_path));
  assert(texture_vkt_test_write_truncated_copy(source_path, truncated_path));

  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrTextureSystem system = {0};
  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
  const String8 source = string8_lit("tests/tmp/rgba16f-cube.vkt");
  assert(vkr_texture_system_prepare_load_from_file(
      &system, source, VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared,
      &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(prepared.description.type == VKR_TEXTURE_TYPE_CUBE_MAP);
  assert(prepared.description.format == VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT);
  assert(prepared.description.width == 2u && prepared.description.height == 2u);
  assert(prepared.description.mip_levels == 1u);
  assert(prepared.description.array_layers == 6u);
  assert(prepared.upload_region_count == 6u);
  assert(prepared.upload_data_size == 6u * 2u * 2u * 8u);
  assert(!prepared.upload_is_compressed);
  for (uint32_t face = 0u; face < 6u; ++face) {
    const VkrTextureUploadRegion *region = &prepared.upload_regions[face];
    assert(region->mip_level == 0u && region->array_layer == face);
    assert(region->width == 2u && region->height == 2u &&
           region->byte_size == 2u * 2u * 8u);
    uint16_t marker = 0u;
    MemCopy(&marker, prepared.upload_data + region->byte_offset,
            sizeof(marker));
    assert(marker == 0x3c00u + face);
  }
  vkr_texture_system_release_prepared_load(&prepared);

  const String8 truncated = string8_lit("tests/tmp/rgba16f-cube-truncated.vkt");
  assert(!vkr_texture_system_prepare_load_from_file(
      &system, truncated, VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared,
      &error));
  vkr_texture_system_release_prepared_load(&prepared);
  arena_destroy(arena);
  remove(source_path);
  remove(truncated_path);
  printf("  test_texture_ktx2_rgba16f_cube_decode PASSED\n");
}

/* Writes a square texture of 16-byte 4x4 blocks in `vk_format` with
 * `level_count` levels from `base_extent` (at most 16). Every block starts
 * with its level and block index, so a wrong region offset or a transcode of
 * the payload changes the markers read back. */
static bool8_t texture_vkt_test_write_blocks(const char *path,
                                             uint32_t vk_format,
                                             uint32_t base_extent,
                                             uint32_t level_count) {
  const ktxTextureCreateInfo create_info = {
      .vkFormat = vk_format,
      .baseWidth = base_extent,
      .baseHeight = base_extent,
      .baseDepth = 1u,
      .numDimensions = 2u,
      .numLevels = level_count,
      .numLayers = 1u,
      .numFaces = 1u,
      .isArray = KTX_FALSE,
      .generateMipmaps = KTX_FALSE,
  };
  ktxTexture2 *texture = NULL;
  ktxResult result = ktxTexture2_Create(
      &create_info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
  if (result != KTX_SUCCESS || !texture) {
    return false_v;
  }
  for (uint32_t level = 0u; level < level_count; ++level) {
    const uint32_t blocks_per_side = Max(1u, (base_extent >> level) / 4u);
    const uint32_t block_count = blocks_per_side * blocks_per_side;
    uint8_t blocks[16u * 16u] = {0};
    for (uint32_t block = 0u; block < block_count; ++block) {
      blocks[block * 16u] = (uint8_t)(0xa0u + level);
      blocks[block * 16u + 1u] = (uint8_t)block;
    }
    result = ktxTexture_SetImageFromMemory(ktxTexture(texture), level, 0u, 0u,
                                           blocks, block_count * 16u);
    if (result != KTX_SUCCESS) {
      ktxTexture_Destroy(ktxTexture(texture));
      return false_v;
    }
  }
  result = ktxTexture_WriteToNamedFile(ktxTexture(texture), path);
  ktxTexture_Destroy(ktxTexture(texture));
  return result == KTX_SUCCESS ? true_v : false_v;
}

/* A workspace import stores its host's native blocks: ASTC 4x4 on ASTC hosts,
 * BC7 and BC5 on BC hosts. They upload untouched, and a device that cannot
 * sample the format refuses them instead of misreading. */
static void test_texture_ktx2_native_block_decode(void) {
  printf("  Running test_texture_ktx2_native_block_decode...\n");

  char tmp_dir[1024];
  snprintf(tmp_dir, sizeof(tmp_dir), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(texture_vkt_test_make_dir(tmp_dir));
  char source_path[1024];
  snprintf(source_path, sizeof(source_path), "%s/native-blocks.vkt", tmp_dir);

  const struct {
    uint32_t vk_format;
    VkrTextureFormat format;
    VkrTextureSystem supported;
  } cases[] = {
      {VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
       VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB,
       {.supports_texture_astc_4x4 = true_v}},
      /* At 8x8 and 4x4, 6x6 blocks number the same as 4x4 blocks. */
      {VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
       VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB,
       {.supports_texture_astc_4x4 = true_v}},
      {VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
       VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM,
       {.supports_texture_astc_4x4 = true_v}},
      {VK_FORMAT_BC7_SRGB_BLOCK,
       VKR_TEXTURE_FORMAT_BC7_SRGB,
       {.supports_texture_bc7 = true_v}},
      {VK_FORMAT_BC7_UNORM_BLOCK,
       VKR_TEXTURE_FORMAT_BC7_UNORM,
       {.supports_texture_bc7 = true_v}},
      {VK_FORMAT_BC5_UNORM_BLOCK,
       VKR_TEXTURE_FORMAT_BC5_UNORM,
       {.supports_texture_bc5 = true_v}},
  };
  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  const String8 source = string8_lit("tests/tmp/native-blocks.vkt");
  for (uint32_t i = 0u; i < ArrayCount(cases); ++i) {
    remove(source_path);
    assert(
        texture_vkt_test_write_blocks(source_path, cases[i].vk_format, 8u, 2u));

    VkrTextureSystem system = cases[i].supported;
    VkrTexturePreparedLoad prepared = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
    assert(vkr_texture_system_prepare_load_from_file(
        &system, source, VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared,
        &error));
    assert(error == VKR_RENDERER_ERROR_NONE);
    assert(prepared.description.format == cases[i].format);
    assert(prepared.description.width == 8u &&
           prepared.description.height == 8u);
    assert(prepared.description.mip_levels == 2u);
    assert(prepared.upload_is_compressed);
    assert(prepared.upload_region_count == 2u);
    assert(prepared.upload_data_size == 5u * 16u);
    for (uint32_t level = 0u; level < 2u; ++level) {
      const VkrTextureUploadRegion *region = &prepared.upload_regions[level];
      assert(region->mip_level == level && region->array_layer == 0u);
      assert(region->width == (level == 0u ? 8u : 4u));
      assert(region->byte_size == (level == 0u ? 4u : 1u) * 16u);
      const uint8_t *blocks = prepared.upload_data + region->byte_offset;
      for (uint32_t block = 0u; block < region->byte_size / 16u; ++block) {
        assert(blocks[block * 16u] == 0xa0u + level);
        assert(blocks[block * 16u + 1u] == block);
      }
    }
    vkr_texture_system_release_prepared_load(&prepared);

    /* Every other native family is supported, this one is not. */
    system = (VkrTextureSystem){.supports_texture_astc_4x4 = true_v,
                                .supports_texture_bc7 = true_v,
                                .supports_texture_bc5 = true_v};
    if (cases[i].supported.supports_texture_astc_4x4) {
      system.supports_texture_astc_4x4 = false_v;
    } else if (cases[i].supported.supports_texture_bc7) {
      system.supports_texture_bc7 = false_v;
    } else {
      system.supports_texture_bc5 = false_v;
    }
    assert(!vkr_texture_system_prepare_load_from_file(
        &system, source, VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared,
        &error));
    vkr_texture_system_release_prepared_load(&prepared);
  }
  arena_destroy(arena);
  remove(source_path);
  printf("  test_texture_ktx2_native_block_decode PASSED\n");
}

/* A load limit drops the mips above it: the texture loads as its first mip
 * within the limit, with the kept levels' own blocks packed at aligned
 * offsets. A limit at or above the base extent leaves the chain unchanged,
 * and the smallest mip always remains. The system's limit and a request's
 * `max_extent` combine to the tighter one, and every load reports the stored
 * chain a later limit would shorten. */
static void test_texture_ktx2_load_dimension_cap(void) {
  printf("  Running test_texture_ktx2_load_dimension_cap...\n");

  char tmp_dir[1024];
  snprintf(tmp_dir, sizeof(tmp_dir), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(texture_vkt_test_make_dir(tmp_dir));
  char source_path[1024];
  snprintf(source_path, sizeof(source_path), "%s/capped-blocks.vkt", tmp_dir);
  remove(source_path);
  assert(texture_vkt_test_write_blocks(source_path, VK_FORMAT_BC7_UNORM_BLOCK,
                                       16u, 3u));

  const struct {
    uint32_t limit;
    uint32_t request_limit;
    uint32_t extent;
    uint32_t first_level;
  } cases[] = {
      {8u, 0u, 8u, 1u},   {4u, 0u, 4u, 2u},  {2u, 0u, 4u, 2u},
      {16u, 0u, 16u, 0u}, {0u, 0u, 16u, 0u}, {0u, 8u, 8u, 1u},
      {8u, 4u, 4u, 2u},   {4u, 8u, 4u, 2u},  {0u, 16u, 16u, 0u},
  };
  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  for (uint32_t i = 0u; i < ArrayCount(cases); ++i) {
    VkrTextureSystem system = {
        .supports_texture_bc7 = true_v,
        .config = {.max_load_dimension = cases[i].limit},
    };
    char request[128];
    if (cases[i].request_limit) {
      snprintf(request, sizeof(request),
               "tests/tmp/capped-blocks.vkt?cs=linear&max_extent=%u",
               cases[i].request_limit);
    } else {
      snprintf(request, sizeof(request), "tests/tmp/capped-blocks.vkt");
    }
    const String8 source =
        string8_create_from_cstr((const uint8_t *)request, strlen(request));
    VkrTexturePreparedLoad prepared = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
    assert(vkr_texture_system_prepare_load_from_file(
        &system, source, VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared,
        &error));
    assert(error == VKR_RENDERER_ERROR_NONE);
    const uint32_t mip_count = 3u - cases[i].first_level;
    assert(prepared.description.width == cases[i].extent &&
           prepared.description.height == cases[i].extent);
    assert(prepared.description.mip_levels == mip_count);
    assert(prepared.upload_region_count == mip_count);
    assert(prepared.limit_source_extent == 16u);
    assert(prepared.limit_source_mip_levels == 3u);

    uint64_t expected_size = 0u;
    for (uint32_t mip = 0u; mip < mip_count; ++mip) {
      const uint32_t level = cases[i].first_level + mip;
      const uint32_t blocks_per_side = Max(1u, (16u >> level) / 4u);
      const VkrTextureUploadRegion *region = NULL;
      for (uint32_t r = 0u; r < prepared.upload_region_count; ++r) {
        if (prepared.upload_regions[r].mip_level == mip) {
          region = &prepared.upload_regions[r];
        }
      }
      assert(region);
      assert(region->width == Max(1u, cases[i].extent >> mip));
      assert(region->byte_size == blocks_per_side * blocks_per_side * 16u);
      assert(region->byte_offset % 16u == 0u);
      assert(region->byte_offset + region->byte_size <=
             prepared.upload_data_size);
      const uint8_t *blocks = prepared.upload_data + region->byte_offset;
      for (uint32_t block = 0u; block < region->byte_size / 16u; ++block) {
        assert(blocks[block * 16u] == 0xa0u + level);
        assert(blocks[block * 16u + 1u] == block);
      }
      expected_size += region->byte_size;
    }
    assert(prepared.upload_data_size == expected_size);
    vkr_texture_system_release_prepared_load(&prepared);
  }
  arena_destroy(arena);
  remove(source_path);
  printf("  test_texture_ktx2_load_dimension_cap PASSED\n");
}

/* A request's explicit `cs=` and `tc=` choose the sRGB or UNORM view of the
 * stored native blocks, as they chose the transcode target before; without
 * them the stored format stands. */
static void test_texture_native_request_view(void) {
  printf("  Running test_texture_native_request_view...\n");

  char tmp_dir[1024];
  snprintf(tmp_dir, sizeof(tmp_dir), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(texture_vkt_test_make_dir(tmp_dir));
  char source_path[1024];
  snprintf(source_path, sizeof(source_path), "%s/native-view.vkt", tmp_dir);

  const struct {
    uint32_t vk_format;
    const char *request;
    VkrTextureFormat expected;
  } cases[] = {
      {VK_FORMAT_BC7_SRGB_BLOCK, "tests/tmp/native-view.vkt",
       VKR_TEXTURE_FORMAT_BC7_SRGB},
      {VK_FORMAT_BC7_SRGB_BLOCK, "tests/tmp/native-view.vkt?tc=data_mask",
       VKR_TEXTURE_FORMAT_BC7_UNORM},
      {VK_FORMAT_BC7_SRGB_BLOCK,
       "tests/tmp/native-view.vkt?cs=linear&tc=color_srgb",
       VKR_TEXTURE_FORMAT_BC7_UNORM},
      {VK_FORMAT_BC7_UNORM_BLOCK, "tests/tmp/native-view.vkt?tc=color_srgb",
       VKR_TEXTURE_FORMAT_BC7_UNORM},
      {VK_FORMAT_BC7_UNORM_BLOCK,
       "tests/tmp/native-view.vkt?cs=srgb&tc=color_srgb",
       VKR_TEXTURE_FORMAT_BC7_SRGB},
      /* Without `tc=` the stored UNORM file is a linear class. */
      {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, "tests/tmp/native-view.vkt?cs=srgb",
       VKR_TEXTURE_FORMAT_ASTC_6x6_UNORM},
      {VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
       "tests/tmp/native-view.vkt?cs=srgb&tc=color_srgb",
       VKR_TEXTURE_FORMAT_ASTC_6x6_SRGB},
      {VK_FORMAT_BC5_UNORM_BLOCK, "tests/tmp/native-view.vkt?cs=srgb",
       VKR_TEXTURE_FORMAT_BC5_UNORM},
  };
  Arena *arena = arena_create(KB(64), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  for (uint32_t i = 0u; i < ArrayCount(cases); ++i) {
    remove(source_path);
    assert(
        texture_vkt_test_write_blocks(source_path, cases[i].vk_format, 8u, 2u));
    VkrTextureSystem system = {.supports_texture_astc_4x4 = true_v,
                               .supports_texture_bc7 = true_v,
                               .supports_texture_bc5 = true_v};
    VkrTexturePreparedLoad prepared = {0};
    VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
    assert(vkr_texture_system_prepare_load_from_file(
        &system,
        string8_create_from_cstr((const uint8_t *)cases[i].request,
                                 strlen(cases[i].request)),
        VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
    assert(prepared.description.format == cases[i].expected);
    vkr_texture_system_release_prepared_load(&prepared);
  }
  arena_destroy(arena);
  remove(source_path);
  printf("  test_texture_native_request_view PASSED\n");
}

static void test_texture_vkt_path_detection(void) {
  printf("  Running test_texture_vkt_path_detection...\n");
  assert(vkr_texture_is_vkt_path(
      string8_lit("assets/textures/albedo.vkt?cs=srgb")));
  assert(!vkr_texture_is_vkt_path(string8_lit("assets/textures/albedo.png")));
  printf("  test_texture_vkt_path_detection PASSED\n");
}

static void test_texture_resolution_candidates_for_source_path(void) {
  printf("  Running test_texture_resolution_candidates_for_source_path...\n");
  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  String8 direct_vkt = {0};
  String8 sidecar_vkt = {0};
  String8 source_path = {0};

  vkr_texture_build_resolution_candidates(
      &allocator, string8_lit("assets/textures/albedo.png?cs=srgb"),
      &direct_vkt, &sidecar_vkt, &source_path);

  assert(direct_vkt.length == 0);
  assert(string8_equals_cstr(source_path, "assets/textures/albedo.png"));
  assert(string8_equals_cstr(sidecar_vkt, "assets/textures/albedo.png.vkt"));

  arena_destroy(arena);
  printf("  test_texture_resolution_candidates_for_source_path PASSED\n");
}

static void test_texture_resolution_candidates_for_direct_vkt(void) {
  printf("  Running test_texture_resolution_candidates_for_direct_vkt...\n");
  Arena *arena = arena_create(KB(64), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  String8 direct_vkt = {0};
  String8 sidecar_vkt = {0};
  String8 source_path = {0};

  vkr_texture_build_resolution_candidates(
      &allocator, string8_lit("assets/textures/albedo.vkt?cs=linear"),
      &direct_vkt, &sidecar_vkt, &source_path);

  assert(string8_equals_cstr(direct_vkt, "assets/textures/albedo.vkt"));
  assert(sidecar_vkt.length == 0);
  assert(string8_equals_cstr(source_path, "assets/textures/albedo.vkt"));

  arena_destroy(arena);
  printf("  test_texture_resolution_candidates_for_direct_vkt PASSED\n");
}

static void test_texture_query_colorspace_policy(void) {
  printf("  Running test_texture_query_colorspace_policy...\n");

  assert(vkr_texture_request_prefers_srgb(
             string8_lit("assets/textures/albedo.png?cs=srgb"), false_v) ==
         true_v);
  assert(vkr_texture_request_prefers_srgb(
             string8_lit("assets/textures/albedo.png?cs=linear"), true_v) ==
         false_v);
  assert(vkr_texture_request_prefers_srgb(
             string8_lit("assets/textures/albedo.png?cs=invalid"), true_v) ==
         true_v);

  printf("  test_texture_query_colorspace_policy PASSED\n");
}

/* Only an explicit `source=only` request decodes a source image; the same
 * path without it names no cooked `.vkt` and fails. */
static void test_texture_source_only_request(void) {
  printf("  Running test_texture_source_only_request...\n");

  Arena *arena = arena_create(MB(8), MB(8));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrTextureSystem system = {0};
  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(!vkr_texture_system_prepare_load_from_file(
      &system, string8_lit("assets/fonts/Ubuntu-2d.png?cs=linear&tc=data_mask"),
      VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
  assert(error == VKR_RENDERER_ERROR_FILE_NOT_FOUND);
  vkr_texture_system_release_prepared_load(&prepared);

  error = VKR_RENDERER_ERROR_UNKNOWN;
  assert(vkr_texture_system_prepare_load_from_file(
      &system,
      string8_lit("assets/fonts/Ubuntu-2d.png?cs=linear&tc=data_mask&source="
                  "only"),
      VKR_TEXTURE_RGBA_CHANNELS, &allocator, &prepared, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(prepared.description.width == 1024u);
  assert(prepared.description.height == 1024u);
  assert(prepared.description.channels == VKR_TEXTURE_RGBA_CHANNELS);
  assert(prepared.description.format == VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM);
  assert(prepared.description.mip_levels == 1u);
  assert(prepared.description.array_layers == 1u);
  assert(prepared.upload_mip_levels == 1u);
  assert(prepared.upload_array_layers == 1u);
  assert(prepared.upload_region_count == 1u);
  assert(!prepared.upload_is_compressed);
  vkr_texture_system_release_prepared_load(&prepared);

  arena_destroy(arena);
  printf("  test_texture_source_only_request PASSED\n");
}

static void assert_normal_rg_material_output(const uint8_t *normal) {
  assert(normal[2] == 0u);
  assert(normal[3] == normal[1]);
}

/**
 * These fixed moment cases cover the offline normal/roughness boundary before
 * mip integration. They catch a changed decode domain, a normalized reduction,
 * or an RG output contract regression without requiring a KTX artifact.
 */
static void test_normal_roughness_material_moments(void) {
  printf("  Running test_normal_roughness_material_moments...\n");

  // Source bytes represent (0.6, 0.2, sqrt(0.6)). Independent analytic
  // directions pin Z-before-strength; strength 1 alone cannot expose the bug.
  const uint8_t tilted[] = {204u, 153u, 0u, 0u};
  const struct {
    float32_t strength;
    float64_t x;
    float64_t y;
    float64_t z;
  } cases[] = {
      {0.0f, 0.0, 0.0, 1.0},
      {0.5f, 0.35856858280031806, 0.11952286093343936, 0.9258200997725515},
      {1.0f, 0.6, 0.2, 0.7745966692414834},
      {2.0f, 0.8090398349558905, 0.26967994498529685, 0.5222329678670935},
  };
  for (uint32_t i = 0u; i < ArrayCount(cases); ++i) {
    const VkrVktMaterialMoment decoded =
        vkr_vkt_material_moment(tilted, 0u, cases[i].strength, 1.0f);
    assert(fabs(decoded.x - cases[i].x) < 1e-12);
    assert(fabs(decoded.y - cases[i].y) < 1e-12);
    assert(fabs(decoded.z - cases[i].z) < 1e-12);
    uint8_t encoded[4];
    uint8_t roughness;
    vkr_vkt_encode_material_moment(decoded, encoded, &roughness);
    const VkrVktMaterialMoment recooked =
        vkr_vkt_material_moment(encoded, 0u, 1.0f, 1.0f);
    // Two-channel byte output introduces quantization, bounded here to 0.01
    // in each component for these oblique directions.
    assert(fabs(recooked.x - cases[i].x) < 0.01);
    assert(fabs(recooked.y - cases[i].y) < 0.01);
    assert(fabs(recooked.z - cases[i].z) < 0.01);
    assert(roughness == 0u);
  }

  // Compression can put XY outside the unit disk. Zero strength must remain
  // a finite flat normal even when the reconstructed source Z is zero.
  const uint8_t outside_disk[] = {255u, 255u, 0u, 0u};
  const VkrVktMaterialMoment flattened =
      vkr_vkt_material_moment(outside_disk, 0u, 0.0f, 1.0f);
  assert(flattened.x == 0.0 && flattened.y == 0.0 && flattened.z == 1.0);

  // A constant normal has zero directional variance, so every source
  // roughness byte survives the fourth-power moment conversion. Scale 0
  // instead reconstructs the flat normal while retaining the same roughness.
  const uint8_t constant[] = {128u, 128u, 0u, 0u};
  for (uint32_t roughness = 0u; roughness < 256u; ++roughness) {
    uint8_t normal[4];
    uint8_t encoded_roughness = 0u;
    VkrVktMaterialMoment moment =
        vkr_vkt_material_moment(constant, (uint8_t)roughness, 1.0f, 1.0f);
    vkr_vkt_encode_material_moment(moment, normal, &encoded_roughness);
    assert(normal[0] == 128u && normal[1] == 128u);
    assert_normal_rg_material_output(normal);
    assert(encoded_roughness == roughness);

    moment = vkr_vkt_material_moment(constant, (uint8_t)roughness, 0.0f, 1.0f);
    vkr_vkt_encode_material_moment(moment, normal, &encoded_roughness);
    assert(normal[0] == 128u && normal[1] == 128u);
    assert_normal_rg_material_output(normal);
    assert(encoded_roughness == roughness);
  }

  // Two equally weighted tilted normals (+/- 0.6, 0, 0.8) average to a flat
  // XY direction with length 0.8. Their analytic variance exceeds the 0.25
  // cap, so zero input roughness encodes to round(255 * sqrt(sqrt(.25))) = 180.
  VkrVktMaterialMoment opposing_mean = {0.0, 0.0, 0.8, 0.0};
  uint8_t normal[4];
  uint8_t encoded_roughness = 0u;
  vkr_vkt_encode_material_moment(opposing_mean, normal, &encoded_roughness);
  assert(normal[0] == 128u && normal[1] == 128u);
  assert_normal_rg_material_output(normal);
  assert(encoded_roughness == 180u);

  // Average r^4, not r: half roughness 0 and half roughness 1 produces a
  // fourth moment of 0.5 and encodes to 0.5^(1/4) = .8408964, byte 214.
  const VkrVktMaterialMoment half_roughness = {0.0, 0.0, 1.0, 0.5};
  vkr_vkt_encode_material_moment(half_roughness, normal, &encoded_roughness);
  assert(normal[0] == 128u && normal[1] == 128u);
  assert_normal_rg_material_output(normal);
  assert(encoded_roughness == 214u);

  // A cancellation to zero mean uses the finite flat-normal fallback and the
  // same capped variance instead of dividing by zero.
  const VkrVktMaterialMoment zero_mean = {0.0, 0.0, 0.0, 0.0};
  vkr_vkt_encode_material_moment(zero_mean, normal, &encoded_roughness);
  assert(normal[0] == 128u && normal[1] == 128u);
  assert_normal_rg_material_output(normal);
  assert(encoded_roughness == 180u);

  printf("  test_normal_roughness_material_moments PASSED\n");
}

static void test_texture_mip_color_transfer(void) {
  printf("  Running test_texture_mip_color_transfer...\n");
  const uint8_t black_white[] = {0, 0, 0, 0, 255, 255, 255, 255};
  uint8_t mip[4];
  vkr_vkt_downsample_rgba8(black_white, 2u, 1u, mip, 1u, 1u, true_v, false_v);
  // Half linear radiance encodes to 188 in sRGB; alpha has no transfer.
  assert(mip[0] == 188u && mip[1] == 188u && mip[2] == 188u);
  assert(mip[3] == 128u);
  vkr_vkt_downsample_rgba8(black_white, 1u, 2u, mip, 1u, 1u, false_v, false_v);
  assert(mip[0] == 128u && mip[1] == 128u && mip[2] == 128u);
  assert(mip[3] == 128u);

  // Constant images must remain constant across the full byte domain,
  // including the linear segment near black and odd rectangular footprints.
  uint8_t source[5u * 3u * 4u];
  uint8_t intermediate[2u * 4u];
  for (uint32_t value = 0u; value < 256u; ++value) {
    MemSet(source, value, sizeof(source));
    vkr_vkt_downsample_rgba8(source, 5u, 3u, intermediate, 2u, 1u, true_v,
                             false_v);
    for (uint32_t i = 0u; i < sizeof(intermediate); ++i) {
      assert(intermediate[i] == value);
    }
    vkr_vkt_downsample_rgba8(intermediate, 2u, 1u, mip, 1u, 1u, true_v,
                             false_v);
    for (uint32_t i = 0u; i < sizeof(mip); ++i) {
      assert(mip[i] == value);
    }
  }
  printf("  test_texture_mip_color_transfer PASSED\n");
}

static void test_texture_mip_area_footprint(void) {
  printf("  Running test_texture_mip_area_footprint...\n");
  const uint8_t last_edge[] = {0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255};
  uint8_t mip[4];
  vkr_vkt_downsample_rgba8(last_edge, 3u, 1u, mip, 1u, 1u, false_v, false_v);
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(mip[i] == 85u);
  }
  vkr_vkt_downsample_rgba8(last_edge, 1u, 3u, mip, 1u, 1u, false_v, false_v);
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(mip[i] == 85u);
  }

  uint8_t source[5u * 5u * 4u] = {0};
  uint8_t square[2u * 2u * 4u];
  // The center texel contributes one quarter of its area to each output.
  MemSet(source + (2u * 5u + 2u) * 4u, 255, 4u);
  vkr_vkt_downsample_rgba8(source, 5u, 5u, square, 2u, 2u, false_v, false_v);
  for (uint32_t i = 0u; i < sizeof(square); ++i) {
    assert(square[i] == 10u); // round(255 * 0.25 / 6.25)
  }
  MemZero(source, sizeof(source));
  MemSet(source + (4u * 5u + 4u) * 4u, 255, 4u);
  vkr_vkt_downsample_rgba8(source, 5u, 5u, square, 2u, 2u, false_v, false_v);
  for (uint32_t i = 0u; i < sizeof(square); ++i) {
    assert(square[i] == (i >= 12u ? 41u : 0u));
  }
  printf("  test_texture_mip_area_footprint PASSED\n");
}

static void test_texture_cutout_color_and_coverage(void) {
  printf("  Running test_texture_cutout_color_and_coverage...\n");
  const uint8_t hidden_blue[] = {255, 0, 0, 255, 0, 0, 255, 0};
  uint8_t mip[4];
  vkr_vkt_downsample_rgba8(hidden_blue, 2u, 1u, mip, 1u, 1u, true_v, true_v);
  assert(mip[0] == 255u && mip[1] == 0u && mip[2] == 0u && mip[3] == 128u);
  vkr_vkt_downsample_rgba8(hidden_blue, 2u, 1u, mip, 1u, 1u, true_v, false_v);
  assert(mip[0] == 188u && mip[2] == 188u && mip[3] == 128u);

  const uint8_t clear[] = {255, 0, 0, 0, 0, 0, 255, 0};
  vkr_vkt_downsample_rgba8(clear, 2u, 1u, mip, 1u, 1u, true_v, true_v);
  assert(mip[0] == 0u && mip[1] == 0u && mip[2] == 0u && mip[3] == 0u);

  const uint32_t threshold = vkr_vkt_alpha_pass_byte(0.5f, 1.0f);
  assert(threshold == 128u);
  assert(vkr_vkt_alpha_pass_byte(0.5f, 0.5f) == 255u);
  assert(vkr_vkt_alpha_pass_byte(0.75f, 0.5f) == 256u);
  assert(vkr_vkt_alpha_pass_byte(0.0f, 0.0f) == 0u);
  assert(vkr_vkt_alpha_pass_byte(0.5f, 0.0f) == 256u);

  // Half the base texels pass, but none of these four unadjusted mip texels
  // pass. A single scale can recover exactly two without promoting alpha 0.
  uint8_t reduced[] = {90, 80, 70, 0,  90, 80, 70, 60,
                       90, 80, 70, 80, 90, 80, 70, 120};
  vkr_vkt_preserve_alpha_coverage(reduced, 4u, threshold, 8u, 16u);
  assert(vkr_vkt_alpha_covered(reduced, 4u, threshold) == 2u);
  assert(reduced[3] == 0u && reduced[7] < threshold);
  assert(reduced[11] >= threshold && reduced[15] >= threshold);
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(reduced[i * 4u] == 90u && reduced[i * 4u + 1u] == 80u &&
           reduced[i * 4u + 2u] == 70u);
  }

  // A one-texel mip with 3/4 base coverage should stay occupied. At 1/4 it
  // should stay empty. These are the closest representable coverages.
  uint8_t single[] = {30, 40, 50, 80};
  vkr_vkt_preserve_alpha_coverage(single, 1u, threshold, 3u, 4u);
  assert(single[3] >= threshold);
  single[3] = 180u;
  vkr_vkt_preserve_alpha_coverage(single, 1u, threshold, 1u, 4u);
  assert(single[3] < threshold);

  single[3] = 80u;
  vkr_vkt_preserve_alpha_coverage(single, 1u, 0u, 4u, 4u);
  assert(single[3] == 80u);
  vkr_vkt_preserve_alpha_coverage(single, 1u, 256u, 0u, 4u);
  assert(single[3] == 80u);
  printf("  test_texture_cutout_color_and_coverage PASSED\n");
}

bool32_t run_texture_vkt_tests() {
  printf("--- Starting Texture VKT Tests ---\n");

  test_texture_vkt_path_detection();
  test_texture_resolution_candidates_for_source_path();
  test_texture_resolution_candidates_for_direct_vkt();
  test_texture_query_colorspace_policy();
  test_texture_ktx2_rgba16f_cube_decode();
  test_texture_ktx2_native_block_decode();
  test_texture_ktx2_load_dimension_cap();
  test_texture_native_request_view();
  test_texture_source_only_request();
  test_normal_roughness_material_moments();
  test_texture_mip_color_transfer();
  test_texture_mip_area_footprint();
  test_texture_cutout_color_and_coverage();

  printf("--- Texture VKT Tests Completed ---\n");
  return true_v;
}
