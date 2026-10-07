#include "decal_tests.h"

#include "math/mat.h"
#include "math/vkr_math.h"
#include "vkr_decal.h"

/* A box from its three edge vectors and centre: column i is the image of the
   box's unit edge along axis i. */
static Mat4 decal_test_box(Vec3 x, Vec3 y, Vec3 z, Vec3 centre) {
  Mat4 box = mat4_identity();
  box.cols[0] = vec4_new(x.x, x.y, x.z, 0.0f);
  box.cols[1] = vec4_new(y.x, y.y, y.z, 0.0f);
  box.cols[2] = vec4_new(z.x, z.y, z.z, 0.0f);
  box.cols[3] = vec4_new(centre.x, centre.y, centre.z, 1.0f);
  return box;
}

static VkrDecal decal_test_decal(Mat4 box_to_world) {
  VkrDecal decal = {
      .opacity = 1.0f,
      .fade_cos_start = 0.5f,
      .fade_cos_end = 0.2f,
      .depth_fade = 0.25f,
  };
  assert(vkr_decal_set_box(&decal, box_to_world));
  assert(vkr_decal_valid(&decal));
  return decal;
}

static bool8_t decal_test_mask_contains(VkrDecalMask mask, uint32_t index) {
  return (mask.words[index / 32u] & (1u << (index % 32u))) != 0u;
}

static bool8_t decal_test_inside(Vec3 point, Vec3 minimum, Vec3 maximum) {
  const float32_t slack = 1e-4f;
  return point.x >= minimum.x - slack && point.x <= maximum.x + slack &&
         point.y >= minimum.y - slack && point.y <= maximum.y + slack &&
         point.z >= minimum.z - slack && point.z <= maximum.z + slack;
}

/* Points placed inside a rotated, scaled or sheared box by its forward map
   must return to the same box coordinates, lie within its bounds and find
   it in their grid cell, up to the box's corners. */
static bool32_t test_decal_box_maps_back_and_is_found(void) {
  printf("  Running test_decal_box_maps_back_and_is_found...\n");
  const Mat4 rotation = vkr_quat_to_mat4(vkr_quat_from_axis_angle(
      vec3_normalize(vec3_new(0.3f, 1.0f, 0.2f)), vkr_to_radians(37.0f)));
  const Mat4 boxes[] = {
      decal_test_box(vec3_new(2.0f, 0.0f, 0.0f), vec3_new(0.0f, 0.5f, 0.0f),
                     vec3_new(0.0f, 0.0f, 3.0f), vec3_new(4.0f, 1.0f, -6.0f)),
      mat4_mul(mat4_mul(mat4_translate(vec3_new(-8.0f, 0.2f, 5.0f)), rotation),
               mat4_scale(vec3_new(4.0f, 0.25f, 1.5f))),
      decal_test_box(vec3_new(1.0f, 0.0f, 0.0f), vec3_new(0.5f, 1.0f, 0.0f),
                     vec3_new(0.0f, 0.3f, 2.0f), vec3_new(0.0f, 3.0f, 12.0f)),
  };
  VkrDecal decals[ArrayCount(boxes)];
  for (uint32_t i = 0u; i < ArrayCount(boxes); ++i) {
    decals[i] = decal_test_decal(boxes[i]);
  }
  VkrDecalGrid grid;
  vkr_decal_grid_build(decals, ArrayCount(decals), &grid);
  assert(grid.cell_count > 0u && grid.cell_count <= VKR_DECAL_GRID_MAX_CELLS);

  const float32_t steps[] = {-0.5f, -0.3f, 0.0f, 0.3f, 0.5f};
  for (uint32_t i = 0u; i < ArrayCount(boxes); ++i) {
    for (uint32_t a = 0u; a < ArrayCount(steps); ++a) {
      for (uint32_t b = 0u; b < ArrayCount(steps); ++b) {
        for (uint32_t c = 0u; c < ArrayCount(steps); ++c) {
          const Vec3 local = vec3_new(steps[a], steps[b], steps[c]);
          const Vec3 world = mat4_mul_vec3(boxes[i], local);
          const Vec3 back = mat4_mul_vec3(decals[i].world_to_box, world);
          assert(fabsf(back.x - local.x) < 1e-4f &&
                 fabsf(back.y - local.y) < 1e-4f &&
                 fabsf(back.z - local.z) < 1e-4f);
          assert(decal_test_inside(world, decals[i].bounds_min,
                                   decals[i].bounds_max));
          /* The grid's far faces close each cell, so pull the box's
             corners a hair inside before the lookup. */
          const Vec3 probe = mat4_mul_vec3(boxes[i], vec3_scale(local, 0.999f));
          assert(decal_test_mask_contains(vkr_decal_grid_mask_at(&grid, probe),
                                          i));
        }
      }
    }
  }

  const VkrDecalMask outside =
      vkr_decal_grid_mask_at(&grid, vec3_new(100.0f, 100.0f, 100.0f));
  assert(outside.words[0] == 0u && outside.words[1] == 0u);

  VkrDecal flat = decals[0];
  const VkrDecal before = flat;
  assert(!vkr_decal_set_box(&flat, mat4_scale(vec3_new(1.0f, 0.0f, 1.0f))));
  assert(MemCompare(&flat, &before, sizeof(flat)) == 0);
  printf("  test_decal_box_maps_back_and_is_found PASSED\n");
  return true_v;
}

/* Small decals far apart keep cells of their own, and the grid represents
   the last row of a full frame table. */
static bool32_t test_decal_grid_separates_decals_and_holds_full_table(void) {
  printf(
      "  Running test_decal_grid_separates_decals_and_holds_full_table...\n");
  VkrDecal decals[VKR_MAX_FRAME_DECALS];
  for (uint32_t i = 0u; i < VKR_MAX_FRAME_DECALS; ++i) {
    decals[i] = decal_test_decal(mat4_translate(vec3_new(
        (float32_t)(i % 8u) * 40.0f, 0.0f, (float32_t)(i / 8u) * 40.0f)));
  }
  VkrDecalGrid grid;
  vkr_decal_grid_build(decals, VKR_MAX_FRAME_DECALS, &grid);
  assert(grid.cell_count > 0u && grid.cell_count <= VKR_DECAL_GRID_MAX_CELLS);
  for (uint32_t i = 0u; i < VKR_MAX_FRAME_DECALS; ++i) {
    const VkrDecalMask mask = vkr_decal_grid_mask_at(
        &grid, mat4_position(mat4_inverse_affine(decals[i].world_to_box)));
    assert(decal_test_mask_contains(mask, i));
    const uint32_t neighbour = (i + 1u) % VKR_MAX_FRAME_DECALS;
    assert(!decal_test_mask_contains(mask, neighbour));
  }
  assert(grid.max_decals_per_cell == 1u);
  printf("  test_decal_grid_separates_decals_and_holds_full_table PASSED\n");
  return true_v;
}

bool32_t run_decal_tests(void) {
  printf("--- Running Decal tests... ---\n");
  bool32_t passed = true_v;
  passed &= test_decal_box_maps_back_and_is_found();
  passed &= test_decal_grid_separates_decals_and_holds_full_table();
  printf("--- Decal tests completed. ---\n");
  return passed;
}
