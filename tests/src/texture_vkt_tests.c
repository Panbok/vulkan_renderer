#include "texture_vkt_tests.h"

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

static void test_texture_transcode_target_policy(void) {
  printf("  Running test_texture_transcode_target_policy...\n");

  assert(vkr_texture_select_transcode_target_format(true_v, true_v, true_v,
                                                    true_v) ==
         VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB);
  assert(vkr_texture_select_transcode_target_format(true_v, false_v, false_v,
                                                    true_v) ==
         VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM);
  assert(vkr_texture_select_transcode_target_format(false_v, false_v, true_v,
                                                    true_v) ==
         VKR_TEXTURE_FORMAT_BC7_UNORM);
  assert(vkr_texture_select_transcode_target_format(false_v, true_v, true_v,
                                                    false_v) ==
         VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB);

  printf("  test_texture_transcode_target_policy PASSED\n");
}

bool32_t run_texture_vkt_tests() {
  printf("--- Starting Texture VKT Tests ---\n");

  test_texture_vkt_path_detection();
  test_texture_resolution_candidates_for_source_path();
  test_texture_resolution_candidates_for_direct_vkt();
  test_texture_vkt_container_detection();
  test_texture_query_colorspace_policy();
  test_texture_transcode_target_policy();

  printf("--- Texture VKT Tests Completed ---\n");
  return true_v;
}
