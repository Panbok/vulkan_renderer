#include "texture_vkt_tests.h"
#include "renderer/systems/vkr_texture_transcode_cache.h"
#include "vkr_vkt_pack_contract.h"

#include <math.h>

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

static void test_texture_vkt_container_detection(void) {
  printf("  Running test_texture_vkt_container_detection...\n");

  const uint8_t legacy_magic[4] = {0x48, 0x54, 0x4B, 0x56};
  assert(vkr_texture_detect_vkt_container(legacy_magic, sizeof(legacy_magic)) ==
         VKR_TEXTURE_VKT_CONTAINER_LEGACY_RAW);

  const uint8_t ktx2_sig[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  assert(vkr_texture_detect_vkt_container(ktx2_sig, sizeof(ktx2_sig)) ==
         VKR_TEXTURE_VKT_CONTAINER_KTX2);

  const uint8_t unknown[4] = {0x00, 0x11, 0x22, 0x33};
  assert(vkr_texture_detect_vkt_container(unknown, sizeof(unknown)) ==
         VKR_TEXTURE_VKT_CONTAINER_UNKNOWN);

  printf("  test_texture_vkt_container_detection PASSED\n");
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

static void test_texture_source_only_bypasses_strict_vkt_policy(void) {
  printf("  Running test_texture_source_only_bypasses_strict_vkt_policy...\n");

  Arena *arena = arena_create(MB(8), MB(8));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  VkrTextureSystem system = {0};
  system.strict_vkt_only_mode = true_v;
  system.allow_source_fallback = false_v;

  VkrTexturePreparedLoad prepared = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_UNKNOWN;
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
  printf("  test_texture_source_only_bypasses_strict_vkt_policy PASSED\n");
}

static void test_texture_transcode_target_policy(void) {
  printf("  Running test_texture_transcode_target_policy...\n");

  VkrDeviceTypeFlags integrated = bitset8_create();
  bitset8_set(&integrated, VKR_DEVICE_TYPE_INTEGRATED_BIT);
  VkrDeviceTypeFlags discrete = bitset8_create();
  bitset8_set(&discrete, VKR_DEVICE_TYPE_DISCRETE_BIT);

  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_COLOR_SRGB, true_v, integrated, true_v, true_v,
             true_v, true_v, true_v) == VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB);
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_COLOR_LINEAR, false_v, discrete, true_v, true_v,
             true_v, true_v, true_v) == VKR_TEXTURE_FORMAT_BC7_UNORM);
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_COLOR_LINEAR, true_v, discrete, true_v, true_v,
             true_v, true_v, true_v) == VKR_TEXTURE_FORMAT_BC7_UNORM);
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_COLOR_SRGB, true_v, integrated, false_v, false_v,
             true_v, false_v,
             false_v) == VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB);
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_NORMAL_RG, true_v, discrete, true_v, true_v,
             true_v, true_v, true_v) == VKR_TEXTURE_FORMAT_BC5_UNORM);
  // Neither BC5 nor ASTC, but EAC RG11 available: the compressed two-channel
  // target, not a widened RGBA one.
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_NORMAL_RG, false_v, integrated, false_v, false_v,
             true_v, false_v, true_v) == VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM);
  // No two-channel compressed target at all. This used to return R8G8_UNORM,
  // which libktx cannot transcode to, so every strict .vkt normal map failed.
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_NORMAL_RG, false_v, integrated, false_v, false_v,
             false_v, false_v, false_v) == VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM);
  assert(vkr_texture_select_transcode_target_format(
             VKR_TEXTURE_CLASS_DATA_MASK, true_v, integrated, false_v, false_v,
             true_v, false_v,
             false_v) == VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM);

  printf("  test_texture_transcode_target_policy PASSED\n");
}

static Vec3 test_decode_normal_rg(float red, float green, float blue,
                                  float strength) {
  (void)blue;
  const float x = (red * 2.0f - 1.0f) * strength;
  const float y = -(green * 2.0f - 1.0f) * strength;
  return vec3_new(x, y, sqrtf(fmaxf(0.0f, 1.0f - x * x - y * y)));
}

static void test_normal_rg_decode_contract(void) {
  printf("  Running test_normal_rg_decode_contract...\n");

  const Vec3 missing_blue = test_decode_normal_rg(0.5f, 0.5f, 0.0f, 1.0f);
  const Vec3 stored_blue = test_decode_normal_rg(0.5f, 0.5f, 1.0f, 1.0f);
  assert(vec3_equal(missing_blue, stored_blue, 0.000001f));
  assert(vec3_equal(missing_blue, vec3_new(0.0f, 0.0f, 1.0f), 0.000001f));

  const Vec3 tilted = test_decode_normal_rg(0.75f, 0.25f, 0.0f, 1.0f);
  assert(tilted.z > 0.0f);
  assert(fabsf(vec3_length(tilted) - 1.0f) < 0.000001f);

  printf("  test_normal_rg_decode_contract PASSED\n");
}

static void test_normal_rg_basis_channel_contract(void) {
  printf("  Running test_normal_rg_basis_channel_contract...\n");

  assert(vkr_vkt_filename_is_normal_rg("surface_ddna.png", 16u));
  assert(vkr_vkt_filename_is_normal_rg("surface_ddn.tga", 15u));
  assert(vkr_vkt_filename_is_normal_rg("Surface_Bump.PNG", 16u));
  assert(vkr_vkt_filename_is_normal_rg("surface_Normal.png", 18u));
  assert(!vkr_vkt_filename_is_normal_rg("surface_base.png", 16u));

  uint8_t pixels[] = {
      10u, 20u, 30u, 40u, 50u, 60u, 70u, 80u,
  };
  vkr_vkt_prepare_normal_rg_for_basis(pixels, 2u);

  assert(pixels[0] == 10u && pixels[1] == 20u && pixels[2] == 30u &&
         pixels[3] == 20u);
  assert(pixels[4] == 50u && pixels[5] == 60u && pixels[6] == 70u &&
         pixels[7] == 60u);

  printf("  test_normal_rg_basis_channel_contract PASSED\n");
}

/**
 * The selector and the transcode mapper are separate switches that must agree.
 * They silently disagreed for one capability combination, so pin the invariant
 * itself rather than another handful of examples: every format the selector can
 * ever return must be transcodable.
 */
static void test_transcode_target_always_transcodable(void) {
  printf("  Running test_transcode_target_always_transcodable...\n");

  static const VkrTextureClass classes[] = {
      VKR_TEXTURE_CLASS_COLOR_SRGB,
      VKR_TEXTURE_CLASS_COLOR_LINEAR,
      VKR_TEXTURE_CLASS_NORMAL_RG,
      VKR_TEXTURE_CLASS_DATA_MASK,
  };
  static const uint8_t device_bits[] = {
      VKR_DEVICE_TYPE_DISCRETE_BIT,
      VKR_DEVICE_TYPE_INTEGRATED_BIT,
      VKR_DEVICE_TYPE_VIRTUAL_BIT,
      VKR_DEVICE_TYPE_CPU_BIT,
  };

  uint32_t checked = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    for (uint32_t d = 0; d < 4; ++d) {
      VkrDeviceTypeFlags device_types = bitset8_create();
      bitset8_set(&device_types, device_bits[d]);
      for (uint32_t srgb = 0; srgb < 2; ++srgb) {
        // Sweep all 32 capability combinations: astc, bc7, etc2, bc5, eac.
        for (uint32_t caps = 0; caps < 32; ++caps) {
          VkrTextureFormat format = vkr_texture_select_transcode_target_format(
              classes[c], srgb ? true_v : false_v, device_types,
              (caps & 1) ? true_v : false_v, (caps & 2) ? true_v : false_v,
              (caps & 4) ? true_v : false_v, (caps & 8) ? true_v : false_v,
              (caps & 16) ? true_v : false_v);
          assert(vkr_texture_format_has_ktx_transcode_target(format));
          checked++;
        }
      }
    }
  }
  assert(checked == 4 * 4 * 2 * 32);

  printf("  test_transcode_target_always_transcodable PASSED (%u "
         "combinations)\n",
         checked);
}

static void test_persistent_transcode_cache_contract(void) {
  printf("  Running test_persistent_transcode_cache_contract...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  const String8 source_path =
      string8_lit("tests/fixtures/persistent-cache-source.vkt");
  const uint8_t source_data[] = {1u, 2u, 3u, 4u, 5u, 6u};
  const uint8_t changed_source[] = {1u, 2u, 3u, 4u, 5u, 7u};
  const uint8_t payload[] = {
      0u,  1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u,  10u,
      11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u,
      22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u,
  };
  const VkrTextureUploadRegion regions[] = {
      {.mip_level = 0u,
       .array_layer = 0u,
       .width = 4u,
       .height = 4u,
       .depth = 1u,
       .byte_offset = 0u,
       .byte_size = 16u},
      {.mip_level = 1u,
       .array_layer = 0u,
       .width = 2u,
       .height = 2u,
       .depth = 1u,
       .byte_offset = 16u,
       .byte_size = 16u},
  };
  const VkrTextureTranscodeCacheRecord source = {
      .width = 4u,
      .height = 4u,
      .channels = 4u,
      .format = VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB,
      .mip_levels = 2u,
      .array_layers = 1u,
      .is_compressed = true_v,
      .has_transparency = true_v,
      .alpha_mask = true_v,
      .data = (uint8_t *)payload,
      .data_size = sizeof(payload),
      .regions = (VkrTextureUploadRegion *)regions,
      .region_count = ArrayCount(regions),
  };
  String8 cache_path = {0};
  assert(vkr_texture_transcode_cache_path(&allocator, source_path,
                                          source.format, &cache_path));
  remove((const char *)cache_path.str);
  assert(vkr_texture_transcode_cache_store(&allocator, source_path, source_data,
                                           sizeof(source_data), &source));

  VkrTextureTranscodeCacheRecord loaded = {0};
  assert(vkr_texture_transcode_cache_load(
      &allocator, source_path, source_data, sizeof(source_data), source.format,
      source.width, source.height, source.mip_levels, source.array_layers,
      &loaded));
  assert(loaded.data_size == sizeof(payload));
  assert(loaded.region_count == ArrayCount(regions));
  assert(loaded.has_transparency && loaded.alpha_mask && loaded.is_compressed);
  assert(MemCompare(loaded.data, payload, sizeof(payload)) == 0);
  assert(loaded.regions[1].byte_offset == 16u);
  vkr_texture_transcode_cache_release(&loaded);

  VkrTextureUploadRegion invalid_regions[ArrayCount(regions)];
  MemCopy(invalid_regions, regions, sizeof(regions));
  invalid_regions[1].mip_level = 0u;
  VkrTextureTranscodeCacheRecord invalid_source = source;
  invalid_source.regions = invalid_regions;
  assert(!vkr_texture_transcode_cache_store(&allocator, source_path,
                                            source_data, sizeof(source_data),
                                            &invalid_source));
  MemCopy(invalid_regions, regions, sizeof(regions));
  invalid_regions[1].byte_size--;
  assert(!vkr_texture_transcode_cache_store(&allocator, source_path,
                                            source_data, sizeof(source_data),
                                            &invalid_source));
  invalid_source = source;
  invalid_source.is_compressed = false_v;
  assert(!vkr_texture_transcode_cache_store(&allocator, source_path,
                                            source_data, sizeof(source_data),
                                            &invalid_source));

  assert(!vkr_texture_transcode_cache_load(
      &allocator, source_path, changed_source, sizeof(changed_source),
      source.format, source.width, source.height, source.mip_levels,
      source.array_layers, &loaded));
  assert(vkr_texture_transcode_cache_store(&allocator, source_path, source_data,
                                           sizeof(source_data), &source));
  FILE *file = fopen((const char *)cache_path.str, "r+b");
  assert(file != NULL);
  assert(fseek(file, -1L, SEEK_END) == 0);
  const int value = fgetc(file);
  assert(value != EOF);
  assert(fseek(file, -1L, SEEK_END) == 0);
  assert(fputc(value ^ 0xff, file) != EOF);
  assert(fclose(file) == 0);
  assert(!vkr_texture_transcode_cache_load(
      &allocator, source_path, source_data, sizeof(source_data), source.format,
      source.width, source.height, source.mip_levels, source.array_layers,
      &loaded));
  remove((const char *)cache_path.str);
  arena_destroy(arena);
  printf("  test_persistent_transcode_cache_contract PASSED\n");
}

bool32_t run_texture_vkt_tests() {
  printf("--- Starting Texture VKT Tests ---\n");

  test_texture_vkt_path_detection();
  test_texture_resolution_candidates_for_source_path();
  test_texture_resolution_candidates_for_direct_vkt();
  test_texture_vkt_container_detection();
  test_texture_query_colorspace_policy();
  test_texture_source_only_bypasses_strict_vkt_policy();
  test_texture_transcode_target_policy();
  test_normal_rg_decode_contract();
  test_normal_rg_basis_channel_contract();
  test_transcode_target_always_transcodable();
  test_persistent_transcode_cache_contract();

  printf("--- Texture VKT Tests Completed ---\n");
  return true_v;
}
