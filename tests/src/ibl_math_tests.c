#include "ibl_math_tests.h"

#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_ibl_sh_pool.h"

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

// --- ADR-038 second-order spherical-harmonic diffuse response ---------------

#define VKR_TEST_CUBE_LEVEL0 128u
#define VKR_TEST_CUBE_LEVEL1 64u
#define VKR_TEST_CUBE_LEVEL2 32u

typedef struct VkrTestCubeImage {
  uint32_t face_size;
  float32_t *texels;
} VkrTestCubeImage;

static float32_t
    s_cube_level0[VKR_TEST_CUBE_LEVEL0 * VKR_TEST_CUBE_LEVEL0 * 6u * 3u];
static float32_t
    s_cube_level1[VKR_TEST_CUBE_LEVEL1 * VKR_TEST_CUBE_LEVEL1 * 6u * 3u];
static float32_t
    s_cube_level2[VKR_TEST_CUBE_LEVEL2 * VKR_TEST_CUBE_LEVEL2 * 6u * 3u];

static float32_t *vkr_test_cube_texel(const VkrTestCubeImage *image,
                                      uint32_t face, uint32_t x, uint32_t y) {
  return image->texels +
         (((size_t)face * image->face_size + y) * image->face_size + x) * 3u;
}

static void vkr_test_cube_sample(void *user, VkrIblCubeFace face, uint32_t x,
                                 uint32_t y, float32_t out_rgb[3]) {
  const VkrTestCubeImage *image = (const VkrTestCubeImage *)user;
  const float32_t *texel = vkr_test_cube_texel(image, (uint32_t)face, x, y);
  out_rgb[0] = texel[0];
  out_rgb[1] = texel[1];
  out_rgb[2] = texel[2];
}

/** Matches the 2x2 box average that VK_FILTER_LINEAR blit mip generation and
 *  Metal mip generation perform between power-of-two levels. */
static void vkr_test_cube_downsample(const VkrTestCubeImage *source,
                                     VkrTestCubeImage *destination) {
  for (uint32_t face = 0u; face < VKR_IBL_CUBE_FACE_COUNT; ++face) {
    for (uint32_t y = 0u; y < destination->face_size; ++y) {
      for (uint32_t x = 0u; x < destination->face_size; ++x) {
        float32_t *out = vkr_test_cube_texel(destination, face, x, y);
        for (uint32_t channel = 0u; channel < 3u; ++channel) {
          out[channel] =
              0.25f *
              (vkr_test_cube_texel(source, face, x * 2u, y * 2u)[channel] +
               vkr_test_cube_texel(source, face, x * 2u + 1u, y * 2u)[channel] +
               vkr_test_cube_texel(source, face, x * 2u, y * 2u + 1u)[channel] +
               vkr_test_cube_texel(source, face, x * 2u + 1u,
                                   y * 2u + 1u)[channel]);
        }
      }
    }
  }
}

static void vkr_test_cube_fill_constant(VkrTestCubeImage *image,
                                        const float32_t rgb[3]) {
  const size_t texel_count =
      (size_t)image->face_size * image->face_size * VKR_IBL_CUBE_FACE_COUNT;
  for (size_t i = 0u; i < texel_count; ++i) {
    image->texels[i * 3u + 0u] = rgb[0];
    image->texels[i * 3u + 1u] = rgb[1];
    image->texels[i * 3u + 2u] = rgb[2];
  }
}

/** Radiance `magnitude` over one face, zero elsewhere. */
static void vkr_test_cube_fill_face(VkrTestCubeImage *image,
                                    VkrIblCubeFace emitting_face,
                                    float32_t magnitude) {
  const float32_t black[3] = {0.0f, 0.0f, 0.0f};
  vkr_test_cube_fill_constant(image, black);
  for (uint32_t y = 0u; y < image->face_size; ++y) {
    for (uint32_t x = 0u; x < image->face_size; ++x) {
      float32_t *texel =
          vkr_test_cube_texel(image, (uint32_t)emitting_face, x, y);
      texel[0] = magnitude;
      texel[1] = magnitude;
      texel[2] = magnitude;
    }
  }
}

/** A high-contrast emitter a few texels wide: the firefly case §2.1 targets. */
static void vkr_test_cube_fill_narrow_sun(VkrTestCubeImage *image,
                                          float32_t magnitude) {
  const float32_t black[3] = {0.0f, 0.0f, 0.0f};
  vkr_test_cube_fill_constant(image, black);
  const uint32_t center = image->face_size / 2u;
  const uint32_t radius = Max(1u, image->face_size / 32u);
  for (uint32_t y = center - radius; y < center + radius; ++y) {
    for (uint32_t x = center - radius; x < center + radius; ++x) {
      float32_t *texel =
          vkr_test_cube_texel(image, VKR_IBL_CUBE_FACE_POSITIVE_Y, x, y);
      texel[0] = magnitude;
      texel[1] = magnitude;
      texel[2] = magnitude;
    }
  }
}

/** Deterministic direction set covering all octants plus the six axes. */
static VkrIblDirection vkr_test_probe_direction(uint32_t index) {
  static const VkrIblDirection k_directions[14] = {
      {1.0f, 0.0f, 0.0f},
      {-1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, -1.0f},
      {0.57735f, 0.57735f, 0.57735f},
      {-0.57735f, 0.57735f, 0.57735f},
      {0.57735f, -0.57735f, 0.57735f},
      {0.57735f, 0.57735f, -0.57735f},
      {-0.57735f, -0.57735f, 0.57735f},
      {-0.57735f, 0.57735f, -0.57735f},
      {0.57735f, -0.57735f, -0.57735f},
      {-0.57735f, -0.57735f, -0.57735f},
  };
  return k_directions[index % 14u];
}

static bool32_t test_ibl_sh_solid_angle_closure(void) {
  printf("  Running test_ibl_sh_solid_angle_closure...\n");
  // Includes face_size 1: a source smaller than the projection extent still
  // projects mip 0, and its four-corner weights must close on the full sphere.
  static const uint32_t k_extents[] = {1u, 2u, 8u, 32u, 64u};
  for (uint32_t i = 0u; i < sizeof(k_extents) / sizeof(k_extents[0]); ++i) {
    const uint32_t extent = k_extents[i];
    const float32_t step = 2.0f / (float32_t)extent;
    float64_t total = 0.0;
    for (uint32_t face = 0u; face < VKR_IBL_CUBE_FACE_COUNT; ++face) {
      for (uint32_t y = 0u; y < extent; ++y) {
        for (uint32_t x = 0u; x < extent; ++x) {
          const float32_t s0 = (float32_t)x * step - 1.0f;
          const float32_t t0 = (float32_t)y * step - 1.0f;
          total += (float64_t)vkr_ibl_cube_texel_solid_angle(s0, s0 + step, t0,
                                                             t0 + step);
        }
      }
    }
    printf("    extent %2u: sum(dOmega) = %.9f (4pi = %.9f)\n", extent, total,
           4.0 * (float64_t)VKR_PI);
    assert(fabs(total - 4.0 * (float64_t)VKR_PI) < 1e-5);
  }
  printf("  test_ibl_sh_solid_angle_closure PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_projection_mip_selection(void) {
  printf("  Running test_ibl_sh_projection_mip_selection...\n");
  uint32_t mip = 0u;
  uint32_t extent = 0u;

  // Greatest available extent not larger than 32.
  assert(vkr_ibl_sh_projection_mip(1024u, 11u, &mip, &extent));
  assert(mip == 5u && extent == 32u);
  assert(vkr_ibl_sh_projection_mip(256u, 9u, &mip, &extent));
  assert(mip == 3u && extent == 32u);
  assert(vkr_ibl_sh_projection_mip(32u, 6u, &mip, &extent));
  assert(mip == 0u && extent == 32u);

  // A source already below the target extent uses mip 0.
  assert(vkr_ibl_sh_projection_mip(16u, 5u, &mip, &extent));
  assert(mip == 0u && extent == 16u);
  assert(vkr_ibl_sh_projection_mip(1u, 1u, &mip, &extent));
  assert(mip == 0u && extent == 1u);

  // A truncated mip chain clamps to the last available level rather than
  // addressing a level the image does not have.
  assert(vkr_ibl_sh_projection_mip(1024u, 3u, &mip, &extent));
  assert(mip == 2u && extent == 256u);

  assert(!vkr_ibl_sh_projection_mip(0u, 4u, &mip, &extent));
  assert(!vkr_ibl_sh_projection_mip(64u, 0u, &mip, &extent));
  assert(!vkr_ibl_sh_projection_mip(64u, 4u, NULL, &extent));
  printf("  test_ibl_sh_projection_mip_selection PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_constant_radiance_identity(void) {
  printf("  Running test_ibl_sh_constant_radiance_identity...\n");
  VkrTestCubeImage image = {VKR_TEST_CUBE_LEVEL2, s_cube_level2};
  const float32_t radiance[3] = {0.25f, 1.5f, 4.0f};
  vkr_test_cube_fill_constant(&image, radiance);

  VkrShL2Rgb coefficients;
  float32_t total_solid_angle = 0.0f;
  assert(vkr_ibl_sh_project_cube(image.face_size, vkr_test_cube_sample, &image,
                                 &coefficients, &total_solid_angle));
  assert(vkr_test_float_near(total_solid_angle, 4.0f * VKR_PI, 1e-4f));

  // Every band above l = 0 has to vanish for an isotropic field.
  for (uint32_t i = 1u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      assert(fabsf(coefficients.c[i][channel]) < 1e-4f);
    }
  }

  vkr_ibl_sh_apply_diffuse_transfer(&coefficients, 0.0f);
  VkrShL2Packed packed;
  vkr_ibl_sh_pack(&coefficients, &packed);

  // The stored signal is D = E / pi, so a constant source radiance L must
  // reconstruct to L and not pi * L.
  for (uint32_t i = 0u; i < 14u; ++i) {
    float32_t response[3];
    vkr_ibl_sh_evaluate_packed(&packed, vkr_test_probe_direction(i), response);
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      assert(vkr_test_float_near(response[channel], radiance[channel], 1e-4f));
    }
  }
  printf("  test_ibl_sh_constant_radiance_identity PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_axis_emitter_basis_signs(void) {
  printf("  Running test_ibl_sh_axis_emitter_basis_signs...\n");
  // Coefficient index of the linear band aligned with each face's axis, and the
  // expected sign. This is the normative check on basis order, basis signs, and
  // the cubemap face-to-direction mapping together.
  static const struct {
    VkrIblCubeFace face;
    uint32_t linear_index;
    float32_t sign;
    VkrIblDirection peak;
  } k_cases[VKR_IBL_CUBE_FACE_COUNT] = {
      {VKR_IBL_CUBE_FACE_POSITIVE_X, 3u, 1.0f, {1.0f, 0.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_X, 3u, -1.0f, {-1.0f, 0.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_Y, 1u, 1.0f, {0.0f, 1.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_Y, 1u, -1.0f, {0.0f, -1.0f, 0.0f}},
      {VKR_IBL_CUBE_FACE_POSITIVE_Z, 2u, 1.0f, {0.0f, 0.0f, 1.0f}},
      {VKR_IBL_CUBE_FACE_NEGATIVE_Z, 2u, -1.0f, {0.0f, 0.0f, -1.0f}},
  };

  VkrTestCubeImage image = {VKR_TEST_CUBE_LEVEL2, s_cube_level2};
  for (uint32_t i = 0u; i < VKR_IBL_CUBE_FACE_COUNT; ++i) {
    vkr_test_cube_fill_face(&image, k_cases[i].face, 1.0f);

    VkrShL2Rgb coefficients;
    assert(vkr_ibl_sh_project_cube(image.face_size, vkr_test_cube_sample,
                                   &image, &coefficients, NULL));

    // Exactly one linear coefficient is excited, with the expected sign.
    for (uint32_t index = 1u; index <= 3u; ++index) {
      const float32_t value = coefficients.c[index][0];
      if (index == k_cases[i].linear_index) {
        assert(value * k_cases[i].sign > 0.5f);
      } else {
        assert(fabsf(value) < 1e-4f);
      }
    }

    vkr_ibl_sh_apply_diffuse_transfer(&coefficients, 0.0f);
    VkrShL2Packed packed;
    vkr_ibl_sh_pack(&coefficients, &packed);

    // The reconstructed response peaks toward the emitting face and is lowest
    // opposite it. A transposed face mapping or a flipped basis sign breaks
    // this even when the coefficient magnitudes look plausible.
    float32_t toward[3];
    float32_t away[3];
    vkr_ibl_sh_evaluate_packed(&packed, k_cases[i].peak, toward);
    const VkrIblDirection opposite = {-k_cases[i].peak.x, -k_cases[i].peak.y,
                                      -k_cases[i].peak.z};
    vkr_ibl_sh_evaluate_packed(&packed, opposite, away);
    assert(toward[0] > away[0] + 0.25f);
  }
  printf("  test_ibl_sh_axis_emitter_basis_signs PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_directional_delta_response(void) {
  printf("  Running test_ibl_sh_directional_delta_response...\n");
  // A unit-flux delta in direction d has radiance coefficients Y_i(d). The
  // addition theorem then gives the exact reconstructed response:
  //   D(n) = (1 / 4pi) * [1 + 2 * cos(g) + (5/8) * (3 cos(g)^2 - 1)]
  // for the normalized transfer factors 1, 2/3, 1/4.
  const VkrIblDirection delta = {0.267261f, 0.534522f, 0.801784f};
  VkrShL2Rgb coefficients = {{{0.0f}}};
  float32_t basis[VKR_SH_L2_COEFFICIENT_COUNT];
  vkr_ibl_sh_basis(delta, basis);
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    coefficients.c[i][0] = basis[i];
    coefficients.c[i][1] = basis[i];
    coefficients.c[i][2] = basis[i];
  }

  vkr_ibl_sh_apply_diffuse_transfer(&coefficients, 0.0f);
  VkrShL2Packed packed;
  vkr_ibl_sh_pack(&coefficients, &packed);

  for (uint32_t i = 0u; i < 14u; ++i) {
    const VkrIblDirection n = vkr_test_probe_direction(i);
    const float32_t cosine = n.x * delta.x + n.y * delta.y + n.z * delta.z;
    const float32_t expected =
        (1.0f + 2.0f * cosine + 0.625f * (3.0f * cosine * cosine - 1.0f)) /
        (4.0f * VKR_PI);
    float32_t response[3];
    vkr_ibl_sh_evaluate_packed(&packed, n, response);
    assert(vkr_test_float_near(response[0], expected, 1e-5f));
  }
  printf("  test_ibl_sh_directional_delta_response PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_packed_matches_direct(void) {
  printf("  Running test_ibl_sh_packed_matches_direct...\n");
  VkrShL2Rgb coefficients;
  // Deterministic asymmetric coefficients: every basis slot carries a distinct
  // value per channel so a transposed pack term cannot cancel out.
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      coefficients.c[i][channel] =
          0.37f * (float32_t)(i + 1u) - 0.11f * (float32_t)channel * (i % 3u);
    }
  }

  VkrShL2Packed packed;
  vkr_ibl_sh_pack(&coefficients, &packed);
  assert(packed.v[6][3] == 0.0f);

  for (uint32_t i = 0u; i < 14u; ++i) {
    const VkrIblDirection n = vkr_test_probe_direction(i);
    float32_t direct[3];
    float32_t optimized[3];
    vkr_ibl_sh_evaluate(&coefficients, n, direct);
    vkr_ibl_sh_evaluate_packed(&packed, n, optimized);
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      assert(vkr_test_float_near(direct[channel], optimized[channel], 1e-5f));
    }
  }
  printf("  test_ibl_sh_packed_matches_direct PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_deringing_window(void) {
  printf("  Running test_ibl_sh_deringing_window...\n");
  // sh_deringing = 0 is the identity for every band.
  for (uint32_t band = 0u; band < 3u; ++band) {
    assert(vkr_ibl_sh_window_factor(band, 0.0f) == 1.0f);
  }

  // Finite positive values follow pow(sinc_pi(l / 3), sh_deringing) exactly.
  static const float32_t k_deringing[] = {0.5f, 1.0f, 2.0f, 4.0f};
  for (uint32_t i = 0u; i < sizeof(k_deringing) / sizeof(k_deringing[0]); ++i) {
    const float32_t d = k_deringing[i];
    assert(vkr_test_float_near(vkr_ibl_sh_window_factor(0u, d), 1.0f, 1e-6f));
    const float32_t band1 = powf(sinf(VKR_PI / 3.0f) / (VKR_PI / 3.0f), d);
    const float32_t band2 =
        powf(sinf(2.0f * VKR_PI / 3.0f) / (2.0f * VKR_PI / 3.0f), d);
    assert(vkr_test_float_near(vkr_ibl_sh_window_factor(1u, d), band1, 1e-6f));
    assert(vkr_test_float_near(vkr_ibl_sh_window_factor(2u, d), band2, 1e-6f));
    assert(band1 < 1.0f && band2 < band1);
  }

  // The window scales bands, not individual coefficients: l = 0 is untouched
  // and the two higher bands shrink by their own factor.
  VkrShL2Rgb windowed;
  for (uint32_t i = 0u; i < VKR_SH_L2_COEFFICIENT_COUNT; ++i) {
    windowed.c[i][0] = 1.0f;
    windowed.c[i][1] = 1.0f;
    windowed.c[i][2] = 1.0f;
  }
  vkr_ibl_sh_apply_diffuse_transfer(&windowed, 1.0f);
  assert(vkr_test_float_near(windowed.c[0][0], 1.0f, 1e-6f));
  assert(vkr_test_float_near(windowed.c[1][0],
                             (2.0f / 3.0f) * vkr_ibl_sh_window_factor(1u, 1.0f),
                             1e-6f));
  assert(vkr_test_float_near(
      windowed.c[4][0], 0.25f * vkr_ibl_sh_window_factor(2u, 1.0f), 1e-6f));
  printf("  test_ibl_sh_deringing_window PASSED\n");
  return true_v;
}

/** Greatest absolute reconstruction difference over the probe direction set. */
static float32_t vkr_test_sh_max_response_delta(const VkrShL2Packed *a,
                                                const VkrShL2Packed *b) {
  float32_t maximum = 0.0f;
  for (uint32_t i = 0u; i < 14u; ++i) {
    const VkrIblDirection n = vkr_test_probe_direction(i);
    float32_t response_a[3];
    float32_t response_b[3];
    vkr_ibl_sh_evaluate_packed(a, n, response_a);
    vkr_ibl_sh_evaluate_packed(b, n, response_b);
    for (uint32_t channel = 0u; channel < 3u; ++channel) {
      maximum = Max(maximum, fabsf(response_a[channel] - response_b[channel]));
    }
  }
  return maximum;
}

static void vkr_test_sh_project_packed(const VkrTestCubeImage *image,
                                       VkrShL2Packed *out_packed) {
  VkrShL2Rgb coefficients;
  assert(vkr_ibl_sh_project_cube(image->face_size, vkr_test_cube_sample,
                                 (void *)image, &coefficients, NULL));
  vkr_ibl_sh_apply_diffuse_transfer(&coefficients, 0.0f);
  vkr_ibl_sh_pack(&coefficients, out_packed);
}

static bool32_t test_ibl_sh_selected_mip_against_full_resolution(void) {
  printf("  Running test_ibl_sh_selected_mip_against_full_resolution...\n");
  VkrTestCubeImage level0 = {VKR_TEST_CUBE_LEVEL0, s_cube_level0};
  VkrTestCubeImage level1 = {VKR_TEST_CUBE_LEVEL1, s_cube_level1};
  VkrTestCubeImage level2 = {VKR_TEST_CUBE_LEVEL2, s_cube_level2};

  uint32_t mip = 0u;
  uint32_t extent = 0u;
  assert(vkr_ibl_sh_projection_mip(VKR_TEST_CUBE_LEVEL0, 8u, &mip, &extent));
  assert(mip == 2u && extent == VKR_TEST_CUBE_LEVEL2);

  // Peak reference response per fixture, so the recorded absolute error can be
  // read as a fraction of the signal it is measured against.
  static const char *k_names[3] = {"constant", "axis-emitter", "narrow-sun"};
  float32_t deltas[3];
  float32_t references[3];

  for (uint32_t fixture = 0u; fixture < 3u; ++fixture) {
    const float32_t constant_radiance[3] = {1.0f, 1.0f, 1.0f};
    if (fixture == 0u) {
      vkr_test_cube_fill_constant(&level0, constant_radiance);
    } else if (fixture == 1u) {
      vkr_test_cube_fill_face(&level0, VKR_IBL_CUBE_FACE_POSITIVE_X, 1.0f);
    } else {
      vkr_test_cube_fill_narrow_sun(&level0, 100.0f);
    }

    vkr_test_cube_downsample(&level0, &level1);
    vkr_test_cube_downsample(&level1, &level2);

    VkrShL2Packed reference;
    VkrShL2Packed selected;
    vkr_test_sh_project_packed(&level0, &reference);
    vkr_test_sh_project_packed(&level2, &selected);

    float32_t peak = 0.0f;
    for (uint32_t i = 0u; i < 14u; ++i) {
      float32_t response[3];
      vkr_ibl_sh_evaluate_packed(&reference, vkr_test_probe_direction(i),
                                 response);
      peak = Max(peak, fabsf(response[0]));
    }

    deltas[fixture] = vkr_test_sh_max_response_delta(&reference, &selected);
    references[fixture] = peak;
    printf("    %-13s mip 0 (%u) vs mip %u (%u): max |dD| = %.6f, "
           "peak |D| = %.6f, relative = %.6f\n",
           k_names[fixture], VKR_TEST_CUBE_LEVEL0, mip, extent, deltas[fixture],
           peak, peak > 0.0f ? deltas[fixture] / peak : 0.0f);
  }

  // Tolerances set from the recorded run above, not chosen ahead of it.
  // Measured relative error was 0.000000 (constant), 0.000032 (axis emitter),
  // and 0.000408 (narrow sun); each bound keeps roughly five times that
  // headroom. The narrow-sun fixture is the one that actually exercises the 2x2
  // box approximation of a solid-angle-preserving filter, and it stays three
  // orders of magnitude inside the signal it is measured against, so the
  // extent-32 projection target holds.
  static const float32_t k_tolerance[3] = {1e-5f, 2e-4f, 2e-3f};
  for (uint32_t fixture = 0u; fixture < 3u; ++fixture) {
    assert(deltas[fixture] < k_tolerance[fixture] * references[fixture]);
  }
  printf("  test_ibl_sh_selected_mip_against_full_resolution PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_clamp_diagnostics(void) {
  printf("  Running test_ibl_sh_clamp_diagnostics...\n");
  VkrTestCubeImage image = {VKR_TEST_CUBE_LEVEL2, s_cube_level2};

  // An isotropic field cannot ring, so the clamp must be a no-op.
  const float32_t constant_radiance[3] = {1.0f, 1.0f, 1.0f};
  vkr_test_cube_fill_constant(&image, constant_radiance);
  VkrShL2Packed packed;
  vkr_test_sh_project_packed(&image, &packed);

  VkrShL2ClampDiagnostics diagnostics;
  vkr_ibl_sh_clamp_diagnostics(&packed, 32u, &diagnostics);
  assert(diagnostics.sample_count == 32u * 64u);
  assert(diagnostics.negative_sample_count == 0u);
  assert(diagnostics.min_value >= 0.0f);
  assert(vkr_test_float_near(diagnostics.mean_unclamped[0],
                             diagnostics.mean_clamped[0], 1e-6f));

  // A single-face emitter is the truncation case: L2 rings negative opposite
  // the source, and max(D, 0) then adds integrated response.
  vkr_test_cube_fill_face(&image, VKR_IBL_CUBE_FACE_POSITIVE_X, 1.0f);
  vkr_test_sh_project_packed(&image, &packed);
  vkr_ibl_sh_clamp_diagnostics(&packed, 32u, &diagnostics);
  printf("    axis emitter: negative %u/%u samples, min = %.6f, "
         "mean %.6f -> %.6f (drift %+.6f)\n",
         diagnostics.negative_sample_count, diagnostics.sample_count,
         diagnostics.min_value, diagnostics.mean_unclamped[0],
         diagnostics.mean_clamped[0],
         diagnostics.mean_clamped[0] - diagnostics.mean_unclamped[0]);
  assert(diagnostics.negative_sample_count > 0u);
  assert(diagnostics.min_value < 0.0f);
  assert(diagnostics.mean_clamped[0] > diagnostics.mean_unclamped[0]);

  // Deringing suppresses the higher bands, so it must reduce the negative lobe
  // rather than merely move it.
  VkrShL2Rgb windowed;
  assert(vkr_ibl_sh_project_cube(image.face_size, vkr_test_cube_sample, &image,
                                 &windowed, NULL));
  vkr_ibl_sh_apply_diffuse_transfer(&windowed, 4.0f);
  VkrShL2Packed windowed_packed;
  vkr_ibl_sh_pack(&windowed, &windowed_packed);
  VkrShL2ClampDiagnostics windowed_diagnostics;
  vkr_ibl_sh_clamp_diagnostics(&windowed_packed, 32u, &windowed_diagnostics);
  printf("    axis emitter, sh_deringing = 4: negative %u/%u samples, "
         "min = %.6f\n",
         windowed_diagnostics.negative_sample_count,
         windowed_diagnostics.sample_count, windowed_diagnostics.min_value);
  assert(windowed_diagnostics.min_value > diagnostics.min_value);
  assert(windowed_diagnostics.negative_sample_count <=
         diagnostics.negative_sample_count);
  printf("  test_ibl_sh_clamp_diagnostics PASSED\n");
  return true_v;
}

// --- ADR-038 coefficient slot pool lifetime -------------------------------

static bool32_t test_ibl_sh_pool_publication_cycle(void) {
  printf("  Running test_ibl_sh_pool_publication_cycle...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.published_count == 0u && metrics.retired_count == 0u);

  // The black sentinel is never handed out.
  uint32_t slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &slot) == VKR_SH_POOL_STATUS_OK);
  assert(slot != VKR_SH_SLOT_BLACK && slot < VKR_SH_SLOT_CAPACITY);

  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.published_count == 1u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS - 1u);

  // Publication order is enforced: a published slot cannot be re-published and
  // a free slot cannot skip straight to published.
  assert(vkr_ibl_sh_pool_publish(&pool, slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  vkr_ibl_sh_pool_reference(&pool, slot, 7u);
  assert(vkr_ibl_sh_pool_retire(&pool, slot) == VKR_SH_POOL_STATUS_OK);

  // A retired slot stays out of circulation until its last reader completes.
  // Anything less than the recorded serial must not release it.
  assert(vkr_ibl_sh_pool_collect(&pool, 6u) == 0u);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.retired_count == 1u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS - 1u);

  assert(vkr_ibl_sh_pool_collect(&pool, 7u) == 1u);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.retired_count == 0u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  printf("  test_ibl_sh_pool_publication_cycle PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_abandon_and_exhaustion(void) {
  printf("  Running test_ibl_sh_pool_abandon_and_exhaustion...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // A pre-submit failure releases the candidate immediately: nothing was
  // submitted, so no reader can exist.
  uint32_t abandoned = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &abandoned) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_abandon(&pool, abandoned) == VKR_SH_POOL_STATUS_OK);
  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.abandon_count == 1u);
  assert(vkr_ibl_sh_pool_abandon(&pool, abandoned) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  // Capacity covers two generations of every logical source that can be live.
  uint32_t slots[VKR_SH_REUSABLE_SLOTS];
  for (uint32_t i = 0u; i < VKR_SH_REUSABLE_SLOTS; ++i) {
    assert(vkr_ibl_sh_pool_reserve(&pool, &slots[i]) == VKR_SH_POOL_STATUS_OK);
  }
  assert(VKR_SH_REUSABLE_SLOTS == VKR_SH_LOGICAL_MAX * VKR_SH_GENERATION_COUNT);

  // Exhaustion reports an error rather than waiting or overwriting.
  uint32_t overflow = 0xffffffffu;
  assert(vkr_ibl_sh_pool_reserve(&pool, &overflow) ==
         VKR_SH_POOL_STATUS_EXHAUSTED);
  assert(overflow == 0xffffffffu);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.exhaustion_count == 1u);
  assert(metrics.free_count == 0u);

  // Every reserved slot is distinct: no two logical sources can be handed the
  // same storage.
  for (uint32_t i = 0u; i < VKR_SH_REUSABLE_SLOTS; ++i) {
    assert(slots[i] != VKR_SH_SLOT_BLACK);
    for (uint32_t j = i + 1u; j < VKR_SH_REUSABLE_SLOTS; ++j) {
      assert(slots[i] != slots[j]);
    }
  }
  printf("  test_ibl_sh_pool_abandon_and_exhaustion PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_replacement_generation(void) {
  printf("  Running test_ibl_sh_pool_replacement_generation...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // Publish a generation and let frame 10 read it.
  uint32_t old_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &old_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, old_slot) ==
         VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, old_slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, old_slot, 10u);

  // A replacement generation must land on different storage while the old one
  // is still readable by an outstanding frame.
  uint32_t new_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(new_slot != old_slot);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, new_slot) ==
         VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_retire(&pool, old_slot) == VKR_SH_POOL_STATUS_OK);

  // Frame 10 is still outstanding, so the old storage must not be reusable.
  assert(vkr_ibl_sh_pool_collect(&pool, 9u) == 0u);
  uint32_t probe_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &probe_slot) == VKR_SH_POOL_STATUS_OK);
  assert(probe_slot != old_slot);
  assert(vkr_ibl_sh_pool_abandon(&pool, probe_slot) == VKR_SH_POOL_STATUS_OK);

  assert(vkr_ibl_sh_pool_collect(&pool, 10u) == 1u);

  // A reference from a later frame must not resurrect a collected slot's
  // retirement serial; slot 0 and free slots ignore references entirely.
  vkr_ibl_sh_pool_reference(&pool, VKR_SH_SLOT_BLACK, 99u);
  vkr_ibl_sh_pool_reference(&pool, old_slot, 99u);
  assert(pool.last_reader_serial[old_slot] == 0u);
  assert(vkr_ibl_sh_pool_retire(&pool, old_slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  // Scene reset retires the live publication; the pool itself survives.
  vkr_ibl_sh_pool_reference(&pool, new_slot, 12u);
  assert(vkr_ibl_sh_pool_retire(&pool, new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_collect(&pool, 11u) == 0u);
  assert(vkr_ibl_sh_pool_collect(&pool, 12u) == 1u);

  // Back to the black-sentinel-only baseline, with no leaked retirements.
  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.published_count == 0u && metrics.retired_count == 0u &&
         metrics.reserved_count == 0u && metrics.recorded_count == 0u);
  assert(metrics.exhaustion_count == 0u);
  printf("  test_ibl_sh_pool_replacement_generation PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_same_submission_bake_and_read(void) {
  printf("  Running test_ibl_sh_pool_same_submission_bake_and_read...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // A frame may consume the candidate it bakes, so a RECORDED slot accrues
  // readers before publication. Retiring it must still respect that reader.
  uint32_t slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, slot, 42u);

  // Publication failed after submission succeeded: the slot follows normal
  // retirement rather than immediate reuse, because the GPU accepted the work.
  assert(vkr_ibl_sh_pool_retire(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_collect(&pool, 41u) == 0u);
  assert(vkr_ibl_sh_pool_collect(&pool, 42u) == 1u);

  // A reserved-but-never-recorded slot has no reader and never gained one.
  uint32_t reserved = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &reserved) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, reserved, 50u);
  assert(pool.last_reader_serial[reserved] == 0u);
  assert(vkr_ibl_sh_pool_abandon(&pool, reserved) == VKR_SH_POOL_STATUS_OK);

  assert(vkr_ibl_sh_pool_reserve(NULL, &reserved) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  assert(vkr_ibl_sh_pool_retire(&pool, VKR_SH_SLOT_BLACK) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  assert(vkr_ibl_sh_pool_retire(&pool, VKR_SH_SLOT_CAPACITY) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  printf("  test_ibl_sh_pool_same_submission_bake_and_read PASSED\n");
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
  passed &= test_ibl_sh_solid_angle_closure();
  passed &= test_ibl_sh_projection_mip_selection();
  passed &= test_ibl_sh_constant_radiance_identity();
  passed &= test_ibl_sh_axis_emitter_basis_signs();
  passed &= test_ibl_sh_directional_delta_response();
  passed &= test_ibl_sh_packed_matches_direct();
  passed &= test_ibl_sh_deringing_window();
  passed &= test_ibl_sh_selected_mip_against_full_resolution();
  passed &= test_ibl_sh_clamp_diagnostics();
  passed &= test_ibl_sh_pool_publication_cycle();
  passed &= test_ibl_sh_pool_abandon_and_exhaustion();
  passed &= test_ibl_sh_pool_replacement_generation();
  passed &= test_ibl_sh_pool_same_submission_bake_and_read();
  printf("--- HDR IBL Math Tests Completed ---\n");
  return passed;
}
