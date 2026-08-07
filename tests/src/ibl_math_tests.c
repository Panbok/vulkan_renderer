#include "ibl_math_tests.h"

#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_ibl_math.h"

static bool32_t vkr_test_float_near(float32_t actual, float32_t expected,
                                    float32_t tolerance) {
  return fabsf(actual - expected) <= tolerance ? true_v : false_v;
}

static bool32_t test_float32_to_float16_boundaries(void) {
  printf("  Running test_float32_to_float16_boundaries...\n");
  assert(vkr_float32_to_float16(0.0f) == 0x0000u);
  assert(vkr_float32_to_float16(-0.0f) == 0x8000u);
  assert(vkr_float32_to_float16(ldexpf(1.0f, -24)) == 0x0001u);
  assert(vkr_float32_to_float16(ldexpf(1.0f, -14)) == 0x0400u);
  assert(vkr_float32_to_float16(65504.0f) == 0x7bffu);
  assert(vkr_float32_to_float16(70000.0f) == 0x7c00u);
  assert(vkr_float32_to_float16(-1.0f) == 0xbc00u);
  assert(vkr_float32_to_float16(INFINITY) == 0x7c00u);
  assert(vkr_float32_to_float16(-INFINITY) == 0xfc00u);
  assert((vkr_float32_to_float16(NAN) & 0x7c00u) == 0x7c00u);
  assert((vkr_float32_to_float16(NAN) & 0x03ffu) != 0u);
  assert(vkr_float16_to_float32(0x0000u) == 0.0f);
  assert(signbit(vkr_float16_to_float32(0x8000u)));
  assert(vkr_float16_to_float32(0x0001u) == ldexpf(1.0f, -24));
  assert(vkr_float16_to_float32(0x0400u) == ldexpf(1.0f, -14));
  assert(vkr_float16_to_float32(0x3c00u) == 1.0f);
  assert(vkr_float16_to_float32(0xbc00u) == -1.0f);
  assert(isinf(vkr_float16_to_float32(0x7c00u)));
  assert(isnan(vkr_float16_to_float32(0x7e00u)));
  printf("  test_float32_to_float16_boundaries PASSED\n");
  return true_v;
}

static bool32_t test_ibl_cubemap_size_derivation(void) {
  printf("  Running test_ibl_cubemap_size_derivation...\n");
  uint32_t face_size = 0u;
  uint32_t mip_count = 0u;
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 4096u, 16u, &face_size,
                                     &mip_count));
  assert(face_size == 1024u && mip_count == 11u);
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 600u, 16u, &face_size,
                                     &mip_count));
  assert(face_size == 512u && mip_count == 10u);
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 4096u, 5u, &face_size,
                                     &mip_count));
  assert(face_size == 1024u && mip_count == 5u);
  assert(!vkr_ibl_derive_cubemap_size(1024u, 1024u, 1024u, 16u, &face_size,
                                      &mip_count));
  assert(!vkr_ibl_derive_cubemap_size(UINT32_MAX, UINT32_MAX / 2u + 1u, 1024u,
                                      16u, &face_size, &mip_count));
  printf("  test_ibl_cubemap_size_derivation PASSED\n");
  return true_v;
}

static bool32_t test_ibl_equirect_cardinal_mapping(void) {
  printf("  Running test_ibl_equirect_cardinal_mapping...\n");
  VkrIblUv positive_y =
      vkr_ibl_direction_to_equirect_uv((VkrIblDirection){0.0f, 1.0f, 0.0f});
  VkrIblUv negative_y =
      vkr_ibl_direction_to_equirect_uv((VkrIblDirection){0.0f, -1.0f, 0.0f});
  VkrIblUv positive_x =
      vkr_ibl_direction_to_equirect_uv((VkrIblDirection){1.0f, 0.0f, 0.0f});
  VkrIblUv negative_x =
      vkr_ibl_direction_to_equirect_uv((VkrIblDirection){-1.0f, 0.0f, 0.0f});
  assert(vkr_test_float_near(positive_y.v, 0.0f, 1e-6f));
  assert(vkr_test_float_near(negative_y.v, 1.0f, 1e-6f));
  assert(vkr_test_float_near(positive_x.u, 0.5f, 1e-6f));
  assert(vkr_test_float_near(negative_x.u, 1.0f, 1e-6f) ||
         vkr_test_float_near(negative_x.u, 0.0f, 1e-6f));

  const VkrIblDirection original = {0.25f, -0.5f, 0.75f};
  VkrIblDirection round_trip = vkr_ibl_equirect_uv_to_direction(
      vkr_ibl_direction_to_equirect_uv(original));
  const float32_t inv_length =
      1.0f / sqrtf(original.x * original.x + original.y * original.y +
                   original.z * original.z);
  assert(vkr_test_float_near(round_trip.x, original.x * inv_length, 1e-5f));
  assert(vkr_test_float_near(round_trip.y, original.y * inv_length, 1e-5f));
  assert(vkr_test_float_near(round_trip.z, original.z * inv_length, 1e-5f));
  printf("  test_ibl_equirect_cardinal_mapping PASSED\n");
  return true_v;
}

static void vkr_test_assert_direction_near(VkrIblDirection actual,
                                           VkrIblDirection expected) {
  assert(vkr_test_float_near(actual.x, expected.x, 1e-6f));
  assert(vkr_test_float_near(actual.y, expected.y, 1e-6f));
  assert(vkr_test_float_near(actual.z, expected.z, 1e-6f));
}

typedef struct VkrTestCubeEdge {
  VkrIblCubeFace face_a;
  VkrIblUv edge_a_start;
  VkrIblUv edge_a_end;
  VkrIblCubeFace face_b;
  VkrIblUv edge_b_start;
  VkrIblUv edge_b_end;
} VkrTestCubeEdge;

static VkrIblUv vkr_test_uv_lerp(VkrIblUv a, VkrIblUv b, float32_t t) {
  return (VkrIblUv){a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}

static bool32_t test_ibl_cube_face_convention(void) {
  printf("  Running test_ibl_cube_face_convention...\n");
  static const VkrIblDirection k_face_centers[VKR_IBL_CUBE_FACE_COUNT] = {
      {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
  };
  for (uint32_t face = 0u; face < VKR_IBL_CUBE_FACE_COUNT; ++face) {
    vkr_test_assert_direction_near(
        vkr_ibl_cube_face_uv_to_direction((VkrIblCubeFace)face,
                                          (VkrIblUv){0.5f, 0.5f}),
        k_face_centers[face]);
  }

  // All twelve physical cube edges. Endpoint checks cover all eight corners;
  // interior samples catch a flipped edge parameter that corner-only tests do
  // not distinguish.
  static const VkrTestCubeEdge k_edges[12] = {
      {VKR_IBL_CUBE_FACE_POSITIVE_X,
       {0.0f, 0.0f},
       {0.0f, 1.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Z,
       {1.0f, 0.0f},
       {1.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_X,
       {1.0f, 0.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Z,
       {0.0f, 0.0f},
       {0.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_X,
       {0.0f, 0.0f},
       {0.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Z,
       {1.0f, 0.0f},
       {1.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_X,
       {1.0f, 0.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Z,
       {0.0f, 0.0f},
       {0.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_X,
       {0.0f, 0.0f},
       {1.0f, 0.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Y,
       {1.0f, 1.0f},
       {1.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_X,
       {0.0f, 1.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Y,
       {1.0f, 0.0f},
       {1.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_X,
       {0.0f, 0.0f},
       {1.0f, 0.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Y,
       {0.0f, 0.0f},
       {0.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_X,
       {0.0f, 1.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Y,
       {0.0f, 1.0f},
       {0.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_Z,
       {0.0f, 0.0f},
       {1.0f, 0.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Y,
       {0.0f, 1.0f},
       {1.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_Z,
       {0.0f, 1.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Y,
       {0.0f, 0.0f},
       {1.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_Z,
       {0.0f, 0.0f},
       {1.0f, 0.0f},
       VKR_IBL_CUBE_FACE_POSITIVE_Y,
       {1.0f, 0.0f},
       {0.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_Z,
       {0.0f, 1.0f},
       {1.0f, 1.0f},
       VKR_IBL_CUBE_FACE_NEGATIVE_Y,
       {1.0f, 1.0f},
       {0.0f, 1.0f}},
  };
  static const float32_t k_edge_samples[] = {0.0f, 0.125f, 0.5f, 0.875f, 1.0f};
  for (uint32_t edge = 0u; edge < sizeof(k_edges) / sizeof(k_edges[0]);
       ++edge) {
    for (uint32_t sample = 0u;
         sample < sizeof(k_edge_samples) / sizeof(k_edge_samples[0]);
         ++sample) {
      const float32_t t = k_edge_samples[sample];
      vkr_test_assert_direction_near(
          vkr_ibl_cube_face_uv_to_direction(
              k_edges[edge].face_a,
              vkr_test_uv_lerp(k_edges[edge].edge_a_start,
                               k_edges[edge].edge_a_end, t)),
          vkr_ibl_cube_face_uv_to_direction(
              k_edges[edge].face_b,
              vkr_test_uv_lerp(k_edges[edge].edge_b_start,
                               k_edges[edge].edge_b_end, t)));
    }
  }

  printf("  test_ibl_cube_face_convention PASSED\n");
  return true_v;
}

static bool32_t test_ibl_prefilter_source_lod(void) {
  printf("  Running test_ibl_prefilter_source_lod...\n");
  assert(vkr_ibl_prefilter_source_lod(1.0f, 1.0f, 0.0f, 1024u, 1024u, 11u) ==
         0.0f);
  const float32_t mid =
      vkr_ibl_prefilter_source_lod(0.8f, 0.9f, 0.5f, 1024u, 1024u, 11u);
  const float32_t grazing =
      vkr_ibl_prefilter_source_lod(0.01f, 0.01f, 0.9f, 1024u, 1024u, 11u);
  assert(isfinite(mid) && mid >= 0.0f && mid <= 10.0f);
  assert(isfinite(grazing) && grazing >= 0.0f && grazing <= 10.0f);
  assert(vkr_ibl_prefilter_source_lod(0.8f, 0.9f, 0.5f, 1024u, 1024u, 1u) ==
         0.0f);
  printf("  test_ibl_prefilter_source_lod PASSED\n");
  return true_v;
}

static bool32_t test_local_probe_fragment_influence_and_draw_visibility(void) {
  printf(
      "  Running test_local_probe_fragment_influence_and_draw_visibility...\n");
  Vec3 center = vec3_zero();
  Vec3 extents = vec3_new(2.0f, 2.0f, 2.0f);

  assert(vkr_test_float_near(vkr_world_resources_probe_fragment_influence(
                                 center, extents, 2.0f, vec3_zero()),
                             1.0f, 1e-6f));
  assert(vkr_test_float_near(
      vkr_world_resources_probe_fragment_influence(center, extents, 2.0f,
                                                   vec3_new(3.0f, 0.0f, 0.0f)),
      0.5f, 1e-6f));
  assert(vkr_test_float_near(
      vkr_world_resources_probe_fragment_influence(center, extents, 2.0f,
                                                   vec3_new(4.1f, 0.0f, 0.0f)),
      0.0f, 1e-6f));

  // A broad draw centered outside the room still binds the local candidate
  // when its world bounds overlap the authored influence volume.
  assert(vkr_world_resources_probe_intersects_sphere(
      center, extents, 2.0f, vec3_new(10.0f, 0.0f, 0.0f), 6.1f));
  assert(!vkr_world_resources_probe_intersects_sphere(
      center, extents, 2.0f, vec3_new(10.0f, 0.0f, 0.0f), 5.9f));
  printf("  test_local_probe_fragment_influence_and_draw_visibility PASSED\n");
  return true_v;
}

bool32_t run_ibl_math_tests(void) {
  printf("--- Starting HDR IBL Math Tests ---\n");
  bool32_t passed = true_v;
  passed &= test_float32_to_float16_boundaries();
  passed &= test_ibl_cubemap_size_derivation();
  passed &= test_ibl_equirect_cardinal_mapping();
  passed &= test_ibl_cube_face_convention();
  passed &= test_ibl_prefilter_source_lod();
  passed &= test_local_probe_fragment_influence_and_draw_visibility();
  printf("--- HDR IBL Math Tests Completed ---\n");
  return passed;
}
