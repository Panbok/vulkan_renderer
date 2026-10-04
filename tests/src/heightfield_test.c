#include "heightfield_test.h"

#include "filesystem/filesystem.h"
#include "level/vkr_heightfield.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_terrain.h"
#include "renderer/systems/vkr_scene_types.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Terrain samples (ADR-084). Oracles are hand-computed heights at named
 * points, a byte-exact file round trip, and the samples undo restores. */

typedef struct HeightfieldTest {
  VkrDMemory memory;
  VkrAllocator allocator;
} HeightfieldTest;

static void heightfield_test_begin(HeightfieldTest *test) {
  assert(vkr_dmemory_create(MB(8), MB(64), &test->memory));
  test->allocator = (VkrAllocator){.ctx = &test->memory};
  vkr_dmemory_allocator_create(&test->allocator);
}

static void heightfield_test_end(HeightfieldTest *test) {
  vkr_dmemory_allocator_destroy(&test->allocator);
}

static bool8_t near(float32_t a, float32_t b, float32_t tolerance) {
  return fabsf(a - b) <= tolerance;
}

/* A file of a field whose last tile is partial reads back sample for
 * sample. */
static void heightfield_test_file(void) {
  printf("  Running heightfield_test_file...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  VkrHeightfield field;
  assert(vkr_heightfield_create(&field, 128u, 0.5f, -10.0f, 90.0f, 1.0f,
                                &test.allocator));
  const uint32_t samples = vkr_heightfield_samples(&field);
  for (uint32_t i = 0; i < samples * samples; ++i) {
    field.heights[i] = (uint16_t)(i * 7u);
    field.weights[i] = i * 2654435761u;
  }
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/heightfield_%u.vkrhf",
           vkr_platform_get_process_id());
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char error[160];
  assert(vkr_heightfield_save(&field, path, error, sizeof(error)));
  VkrHeightfield read;
  assert(
      vkr_heightfield_load(&read, path, &test.allocator, error, sizeof(error)));
  assert(read.cells == 128u && read.spacing == 0.5f &&
         read.height_min == -10.0f && read.height_max == 90.0f);
  assert(MemCompare(read.heights, field.heights,
                    sizeof(uint16_t) * samples * samples) == 0);
  assert(MemCompare(read.weights, field.weights,
                    sizeof(uint32_t) * samples * samples) == 0);
  FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&file) == FILE_ERROR_NONE);
  assert(!vkr_heightfield_load(&read, path, &test.allocator, error,
                               sizeof(error)));
  vkr_heightfield_destroy(&read, &test.allocator);
  vkr_heightfield_destroy(&field, &test.allocator);
  heightfield_test_end(&test);
  printf("  heightfield_test_file PASSED\n");
}

/* Operations change what they name and nothing beyond it. */
static void heightfield_test_operations(void) {
  printf("  Running heightfield_test_operations...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  VkrHeightfield field;
  /* 64 cells of 1 m: local x and z run from -32 to 32. */
  assert(vkr_heightfield_create(&field, 64u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  const float32_t step = 100.0f / 65535.0f;
  VkrHeightfieldRect touched;
  VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                         .brush = VKR_HEIGHTFIELD_RAISE,
                         .a = vec3_new(0.0f, 0.0f, 0.0f),
                         .radius = 4.0f,
                         .strength = 2.0f};
  VkrHeightfieldRect predicted;
  assert(vkr_heightfield_op_rect(&field, &op, &predicted));
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(MemCompare(&predicted, &touched, sizeof(touched)) == 0);
  /* Full strength at the centre, nothing at the radius. */
  assert(near(vkr_heightfield_at(&field, 32u, 32u), 2.0f, 2.0f * step));
  assert(near(vkr_heightfield_at(&field, 36u, 32u), 0.0f, step));
  assert(near(vkr_heightfield_at(&field, 10u, 10u), 0.0f, step));
  float32_t h = 0.0f;
  assert(vkr_heightfield_sample(&field, 0.0f, 0.0f, &h) &&
         near(h, 2.0f, 2.0f * step));
  assert(!vkr_heightfield_sample(&field, 40.0f, 0.0f, &h));

  /* A flattened rectangle is level inside and untouched past its
     falloff. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_FLATTEN,
                          .min = vec2_new(-20.0f, -20.0f),
                          .max = vec2_new(-10.0f, -10.0f),
                          .height = 5.0f,
                          .falloff = 2.0f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(near(vkr_heightfield_at(&field, 17u, 17u), 5.0f, step));
  assert(near(vkr_heightfield_at(&field, 7u, 7u), 0.0f, step));

  /* A ramp's centreline follows its ends. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_RAMP,
                          .a = vec3_new(10.0f, 0.0f, 0.0f),
                          .b = vec3_new(20.0f, 10.0f, 0.0f),
                          .width = 2.0f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(near(vkr_heightfield_at(&field, 42u, 32u), 0.0f, step));
  assert(near(vkr_heightfield_at(&field, 47u, 32u), 5.0f, step));
  assert(near(vkr_heightfield_at(&field, 52u, 32u), 10.0f, step));

  /* Painting keeps the four weights summing to one. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                          .brush = VKR_HEIGHTFIELD_PAINT,
                          .layer = 2u,
                          .a = vec3_new(0.0f, 0.0f, 0.0f),
                          .radius = 3.0f,
                          .strength = 0.6f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  for (uint32_t z = touched.z0; z <= touched.z1; ++z) {
    for (uint32_t x = touched.x0; x <= touched.x1; ++x) {
      const uint32_t w = field.weights[z * 65u + x];
      assert((w & 0xFFu) + ((w >> 8u) & 0xFFu) + ((w >> 16u) & 0xFFu) +
                 (w >> 24u) ==
             255u);
    }
  }
  assert(((field.weights[32u * 65u + 32u] >> 16u) & 0xFFu) == 153u);
  vkr_heightfield_destroy(&field, &test.allocator);
  heightfield_test_end(&test);
  printf("  heightfield_test_operations PASSED\n");
}

/* A terrain edit is one undo entry; a stroke's steps fold into one, and
   undo restores the samples from before the stroke. */
static void heightfield_test_journal(void) {
  printf("  Running heightfield_test_journal...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  char relative[256];
  snprintf(relative, sizeof(relative),
           PROJECT_SOURCE_DIR "tests/tmp/terrain_%u.vkrhf",
           vkr_platform_get_process_id());
  VkrHeightfield field;
  char error[160];
  assert(vkr_heightfield_create(&field, 64u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  assert(vkr_heightfield_save(&field, relative, error, sizeof(error)));
  vkr_heightfield_destroy(&field, &test.allocator);

  VkrScene scene;
  assert(vkr_scene_init(&scene, &test.allocator, 0, 8, NULL));
  VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(100.0f, 2.0f, 0.0f),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update_transforms(&scene);
  SceneTerrain terrain;
  vkr_type_defaults(&vkr_scene_terrain_type, &terrain);
  snprintf(terrain.heightfield, sizeof(terrain.heightfield), "%s", relative);
  assert(
      vkr_scene_set_typed(&scene, entity, &vkr_scene_terrain_type, &terrain));
  const VkrHeightfield *live = vkr_scene_terrain_field(&scene, entity);
  assert(live);
  VkrSceneEditState state = {0};
  vkr_scene_edit_reset(&state, &test.allocator, 1);
  /* World (100, _, 0) is the terrain's centre, its ground at y 2. */
  VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                         .brush = VKR_HEIGHTFIELD_RAISE,
                         .a = vec3_new(100.0f, 0.0f, 0.0f),
                         .radius = 3.0f,
                         .strength = 1.0f};
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 7u));
  op.a = vec3_new(110.0f, 0.0f, 0.0f);
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 7u));
  assert(state.undo_count == 1u);
  assert(vkr_heightfield_at(live, 32u, 32u) > 0.9f &&
         vkr_heightfield_at(live, 42u, 32u) > 0.9f);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(fabsf(vkr_heightfield_at(live, 32u, 32u)) < 0.01f &&
         fabsf(vkr_heightfield_at(live, 42u, 32u)) < 0.01f);
  assert(vkr_scene_edit_undo(&state, &scene, true_v));
  assert(vkr_heightfield_at(live, 42u, 32u) > 0.9f);
  /* Another gesture is another entry. */
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 8u));
  assert(state.undo_count == 2u);
  vkr_scene_edit_reset(&state, &test.allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  FilePath file = {.path = string8_create_from_cstr((const uint8_t *)relative,
                                                    strlen(relative)),
                   .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&file) == FILE_ERROR_NONE);
  heightfield_test_end(&test);
  printf("  heightfield_test_journal PASSED\n");
}

bool32_t run_heightfield_tests(void) {
  printf("--- Starting Heightfield Tests ---\n");
  heightfield_test_file();
  heightfield_test_operations();
  heightfield_test_journal();
  printf("--- Heightfield Tests Completed ---\n");
  return true_v;
}
