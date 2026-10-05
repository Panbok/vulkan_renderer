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
#include <stdlib.h>
#include <string.h>

#if !defined(PLATFORM_WINDOWS)
#include <sys/stat.h>
#endif

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

static bool8_t heightfield_test_near(float32_t a, float32_t b,
                                     float32_t tolerance) {
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
  const VkrHeightfieldRect all = {0u, 0u, samples - 1u, samples - 1u};
  uint16_t *heights =
      vkr_allocator_alloc(&test.allocator, sizeof(uint16_t) * samples * samples,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *weights =
      vkr_allocator_alloc(&test.allocator, sizeof(uint32_t) * samples * samples,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint16_t *read_heights =
      vkr_allocator_alloc(&test.allocator, sizeof(uint16_t) * samples * samples,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *read_weights =
      vkr_allocator_alloc(&test.allocator, sizeof(uint32_t) * samples * samples,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  for (uint32_t i = 0; i < samples * samples; ++i) {
    heights[i] = (uint16_t)(i * 7u);
    weights[i] = i * 2654435761u;
  }
  vkr_heightfield_write_rect(&field, all, heights, weights);
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
  vkr_heightfield_read_rect(&read, all, read_heights, read_weights);
  assert(MemCompare(read_heights, heights,
                    sizeof(uint16_t) * samples * samples) == 0);
  assert(MemCompare(read_weights, weights,
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
  assert(heightfield_test_near(vkr_heightfield_at(&field, 32u, 32u), 2.0f,
                               2.0f * step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 36u, 32u), 0.0f, step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 10u, 10u), 0.0f, step));
  float32_t h = 0.0f;
  assert(vkr_heightfield_sample(&field, 0.0f, 0.0f, &h) &&
         heightfield_test_near(h, 2.0f, 2.0f * step));
  assert(!vkr_heightfield_sample(&field, 40.0f, 0.0f, &h));

  /* A flattened rectangle is level inside and untouched past its
     falloff. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_FLATTEN,
                          .min = vec2_new(-20.0f, -20.0f),
                          .max = vec2_new(-10.0f, -10.0f),
                          .height = 5.0f,
                          .falloff = 2.0f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 17u, 17u), 5.0f, step));
  assert(heightfield_test_near(vkr_heightfield_at(&field, 7u, 7u), 0.0f, step));

  /* A ramp's centreline follows its ends. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_RAMP,
                          .a = vec3_new(10.0f, 0.0f, 0.0f),
                          .b = vec3_new(20.0f, 10.0f, 0.0f),
                          .width = 2.0f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 42u, 32u), 0.0f, step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 47u, 32u), 5.0f, step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 52u, 32u), 10.0f, step));

  /* A road follows its centreline's heights inside its width and leaves
     ground past its falloff alone. */
  const Vec3 path[3] = {vec3_new(-20.0f, 1.0f, 20.0f),
                        vec3_new(0.0f, 3.0f, 20.0f),
                        vec3_new(20.0f, 3.0f, 20.0f)};
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_ROAD,
                          .width = 4.0f,
                          .falloff = 2.0f,
                          .path = path,
                          .path_count = 3u};
  assert(vkr_heightfield_op_rect(&field, &op, &predicted));
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(MemCompare(&predicted, &touched, sizeof(touched)) == 0);
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 22u, 52u), 2.0f, step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 42u, 51u), 3.0f, step));
  assert(
      heightfield_test_near(vkr_heightfield_at(&field, 42u, 59u), 0.0f, step));
  op.path_count = 1u;
  assert(!vkr_heightfield_op_rect(&field, &op, &predicted));

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
      const uint32_t w = vkr_heightfield_weights_at(&field, x, z);
      assert((w & 0xFFu) + ((w >> 8u) & 0xFFu) + ((w >> 16u) & 0xFFu) +
                 (w >> 24u) ==
             255u);
    }
  }
  assert(((vkr_heightfield_weights_at(&field, 32u, 32u) >> 16u) & 0xFFu) ==
         153u);
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

  /* A stroke across a 1 km terrain continues in a second entry once its
     rectangle would pass 512 x 512 samples, and still undoes as one; a
     step inside the new entry's rectangle folds into it. */
  assert(vkr_heightfield_create(&field, 1024u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  assert(vkr_heightfield_save(&field, relative, error, sizeof(error)));
  vkr_heightfield_destroy(&field, &test.allocator);
  assert(vkr_scene_init(&scene, &test.allocator, 0, 8, NULL));
  entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_zero(),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update_transforms(&scene);
  assert(
      vkr_scene_set_typed(&scene, entity, &vkr_scene_terrain_type, &terrain));
  live = vkr_scene_terrain_field(&scene, entity);
  assert(live && live->cells == 1024u);
  vkr_scene_edit_reset(&state, &test.allocator, 1);
  op.a = vec3_new(-500.0f, 0.0f, -500.0f);
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 9u));
  op.a = vec3_new(500.0f, 0.0f, 500.0f);
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 9u));
  op.a = vec3_new(501.0f, 0.0f, 500.0f);
  assert(vkr_scene_edit_terrain(&state, &scene, entity, &op, 9u));
  assert(state.undo_count == 2u && state.undo[0].group &&
         state.undo[0].group == state.undo[1].group);
  assert(vkr_heightfield_at(live, 12u, 12u) > 0.9f &&
         vkr_heightfield_at(live, 1012u, 1012u) > 0.9f);
  assert(vkr_scene_edit_undo(&state, &scene, false_v));
  assert(fabsf(vkr_heightfield_at(live, 12u, 12u)) < 0.01f &&
         fabsf(vkr_heightfield_at(live, 1012u, 1012u)) < 0.01f &&
         fabsf(vkr_heightfield_at(live, 1013u, 1012u)) < 0.01f);
  vkr_scene_edit_reset(&state, &test.allocator, 0);
  vkr_scene_shutdown(&scene, NULL);
  assert(file_remove(&file) == FILE_ERROR_NONE);
  heightfield_test_end(&test);
  printf("  heightfield_test_journal PASSED\n");
}

/* Every level of a tile covers its 64 x 64 cells once, counter-clockwise
   from above, and reports its largest height difference from the samples:
   a spike at an odd sample is that tall at level 1 and flat ground costs
   only the cell-size floor. */
static void heightfield_test_tile_levels(void) {
  printf("  Running heightfield_test_tile_levels...\n");
  const uint32_t side = VKR_GPU_TERRAIN_TILE_SIDE;
  const uint32_t vertex_count = side * side + 4u * side;
  static VkrVertex3d
      vertices[VKR_GPU_TERRAIN_TILE_SIDE * VKR_GPU_TERRAIN_TILE_SIDE +
               4u * VKR_GPU_TERRAIN_TILE_SIDE];
  for (uint32_t z = 0; z < side; ++z) {
    for (uint32_t x = 0; x < side; ++x) {
      vertices[z * side + x] = (VkrVertex3d){
          .position = {(float32_t)x * 2.0f, 0.0f, (float32_t)z * 2.0f}};
    }
  }
  /* Skirt vertices hang below their edge samples. */
  for (uint32_t i = side * side; i < vertex_count; ++i) {
    vertices[i] = (VkrVertex3d){.position = {0.0f, -4.0f, 0.0f}};
  }
  vertices[1u * side + 1u].position.y = 3.0f;
  const uint32_t capacity = vkr_scene_terrain_tile_index_count();
  uint32_t *indices = malloc(sizeof(uint32_t) * capacity);
  assert(indices);
  VkrGpuGeometryLodRow lod;
  const uint32_t count =
      vkr_scene_terrain_tile_indices(vertices, 2.0f, indices, capacity, &lod);
  assert(count == capacity);
  assert(lod.level_count == VKR_SCENE_TERRAIN_LOD_LEVELS &&
         lod.flags == VKR_GPU_GEOMETRY_LOD_TERRAIN_GRID);
  assert(lod.levels[0].first_index == 0u && lod.levels[0].error == 0.0f);
  for (uint32_t level = 0; level < lod.level_count; ++level) {
    const VkrGpuGeometryLodLevel range = lod.levels[level];
    float32_t area = 0.0f;
    for (uint32_t t = 0; t < range.index_count; t += 3u) {
      const uint32_t *tri = &indices[range.first_index + t];
      assert(tri[0] < vertex_count && tri[1] < vertex_count &&
             tri[2] < vertex_count);
      if (tri[0] >= side * side || tri[1] >= side * side ||
          tri[2] >= side * side) {
        continue;
      }
      const Vec3 a = vkr_vertex_unpack_vec3(vertices[tri[0]].position);
      const Vec3 b = vkr_vertex_unpack_vec3(vertices[tri[1]].position);
      const Vec3 c = vkr_vertex_unpack_vec3(vertices[tri[2]].position);
      /* Seen from +Y, counter-clockwise has a negative x-z cross. */
      const float32_t cross =
          (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
      assert(cross < 0.0f);
      area -= 0.5f * cross;
    }
    assert(heightfield_test_near(area, 128.0f * 128.0f, 1.0e-2f));
    if (level > 0u) {
      assert(range.error >= lod.levels[level - 1u].error);
    }
  }
  /* Level 1 drops the spike: 3 m against a 0.4 m floor. */
  assert(heightfield_test_near(lod.levels[1].error, 3.0f, 1.0e-5f));
  /* Flat elsewhere: level 6's floor is a tenth of its 128 m cells, above
     the spike. */
  assert(heightfield_test_near(lod.levels[6].error, 12.8f, 1.0e-4f));
  free(indices);
  printf("  heightfield_test_tile_levels PASSED\n");
}

/* A field larger than the resident limit loads only its overview; a tile
   edited, saved in place and released reads back, the overview follows the
   edit, and an unsaved tile cannot be released. */
static void heightfield_test_streamed(void) {
  printf("  Running heightfield_test_streamed...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/heightfield_streamed_%u.vkrhf",
           vkr_platform_get_process_id());
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char error[160];
  const uint32_t cells = 2u * VKR_HEIGHTFIELD_STREAMED_CELLS;
  assert(vkr_heightfield_create_file(path, cells, 1.0f, -100.0f, 100.0f, 5.0f,
                                     error, sizeof(error)));
  VkrHeightfield field;
  assert(vkr_heightfield_load(&field, path, &test.allocator, error,
                              sizeof(error)));
  assert(vkr_heightfield_streamed(&field) && field.resident_tiles == 0u);
  float32_t height = 0.0f;
  assert(vkr_heightfield_sample(&field, 0.0f, 0.0f, &height));
  assert(heightfield_test_near(height, 5.0f, 0.01f));

  /* Samples (192..207, 256..271) lie in tile (3, 4); (192, 256) is an
     overview sample. */
  const VkrHeightfieldRect rect = {192u, 256u, 207u, 271u};
  assert(!vkr_heightfield_resident(&field, rect));
  assert(vkr_heightfield_load_rect(&field, rect));
  assert(field.resident_tiles == 1u && vkr_heightfield_resident(&field, rect));
  uint16_t heights[16u * 16u];
  uint32_t weights[16u * 16u];
  const uint16_t raised = vkr_heightfield_quantize(&field, 40.0f);
  for (uint32_t i = 0; i < ArrayCount(heights); ++i) {
    heights[i] = raised;
    weights[i] = 0xFF00u;
  }
  vkr_heightfield_write_rect(&field, rect, heights, weights);
  assert(heightfield_test_near(vkr_heightfield_overview_at(&field, 12u, 16u),
                               40.0f, 0.01f));
  /* Unsaved samples stay resident. */
  assert(!vkr_heightfield_tile_release(&field, 3u, 4u));
  assert(vkr_heightfield_save(&field, path, error, sizeof(error)));
  assert(vkr_heightfield_tile_release(&field, 3u, 4u));
  assert(field.resident_tiles == 0u);
  vkr_heightfield_destroy(&field, &test.allocator);

  assert(vkr_heightfield_load(&field, path, &test.allocator, error,
                              sizeof(error)));
  assert(heightfield_test_near(vkr_heightfield_overview_at(&field, 12u, 16u),
                               40.0f, 0.01f));
  assert(heightfield_test_near(vkr_heightfield_overview_at(&field, 13u, 16u),
                               5.0f, 0.01f));
  assert(vkr_heightfield_tile_load(&field, 3u, 4u));
  assert(heightfield_test_near(vkr_heightfield_at(&field, 200u, 260u), 40.0f,
                               0.01f));
  assert(vkr_heightfield_weights_at(&field, 200u, 260u) == 0xFF00u);
  assert(heightfield_test_near(vkr_heightfield_at(&field, 210u, 260u), 5.0f,
                               0.01f));
  /* Span-wise heights, across tile edges, match vkr_heightfield_at bit for
     bit. */
  const VkrHeightfieldRect span = {150u, 250u, 270u, 262u};
  assert(vkr_heightfield_load_rect(&field, span));
  static float32_t metres[121u * 13u];
  vkr_heightfield_read_metres(&field, span, metres, 121u);
  for (uint32_t z = span.z0; z <= span.z1; ++z) {
    for (uint32_t x = span.x0; x <= span.x1; ++x) {
      const float32_t expected = vkr_heightfield_at(&field, x, z);
      assert(MemCompare(&metres[(z - span.z0) * 121u + (x - span.x0)],
                        &expected, sizeof(expected)) == 0);
    }
  }
  vkr_heightfield_destroy(&field, &test.allocator);

  /* A save cut short after its journal finishes when the file opens again:
     samples only the journal holds come back, and the journal goes. */
  char journal[1100];
  snprintf(journal, sizeof(journal), "%s.journal", path);
  const FilePath journal_file = {.path = string8_create_from_cstr(
                                     (const uint8_t *)journal, strlen(journal)),
                                 .type = FILE_PATH_TYPE_ABSOLUTE};
  const float32_t written[2] = {60.0f, 80.0f};
  for (uint32_t pass = 0; pass < 2u; ++pass) {
    assert(vkr_heightfield_load(&field, path, &test.allocator, error,
                                sizeof(error)));
    assert(vkr_heightfield_load_rect(&field, rect));
    const uint16_t value = vkr_heightfield_quantize(&field, written[pass]);
    for (uint32_t i = 0; i < ArrayCount(heights); ++i) {
      heights[i] = value;
    }
    vkr_heightfield_write_rect(&field, rect, heights, weights);
    assert(vkr_heightfield_journal_write(&field, path, error, sizeof(error)));
    vkr_heightfield_destroy(&field, &test.allocator);
    if (pass == 1u) {
      /* A torn journal, cut before its footer, leaves the file as it was. */
      FILE *torn = file_fopen(journal, "rb");
      assert(torn && fseek(torn, 0, SEEK_END) == 0);
      const long size = ftell(torn);
      char *bytes = malloc((size_t)size);
      assert(bytes && fseek(torn, 0, SEEK_SET) == 0 &&
             fread(bytes, 1, (size_t)size, torn) == (size_t)size);
      fclose(torn);
      torn = file_fopen(journal, "wb");
      assert(torn &&
             fwrite(bytes, 1, (size_t)size - 1u, torn) == (size_t)size - 1u);
      fclose(torn);
      free(bytes);
    }
    assert(vkr_heightfield_load(&field, path, &test.allocator, error,
                                sizeof(error)));
    assert(!file_exists(&journal_file));
    assert(vkr_heightfield_tile_load(&field, 3u, 4u));
    assert(heightfield_test_near(vkr_heightfield_at(&field, 200u, 260u), 60.0f,
                                 0.01f));
    assert(heightfield_test_near(vkr_heightfield_overview_at(&field, 12u, 16u),
                                 60.0f, 0.01f));
    vkr_heightfield_destroy(&field, &test.allocator);
  }

#if !defined(PLATFORM_WINDOWS)
  /* A file it may not write still streams, and saving it fails. */
  assert(chmod(path, 0444) == 0);
  assert(vkr_heightfield_load(&field, path, &test.allocator, error,
                              sizeof(error)));
  assert(field.read_only && vkr_heightfield_tile_load(&field, 3u, 4u));
  assert(heightfield_test_near(vkr_heightfield_at(&field, 200u, 260u), 60.0f,
                               0.01f));
  vkr_heightfield_write_rect(&field, rect, heights, weights);
  assert(!vkr_heightfield_save(&field, path, error, sizeof(error)));
  vkr_heightfield_destroy(&field, &test.allocator);
  assert(chmod(path, 0644) == 0);
#endif

  /* The ground under any point of a streamed terrain comes from its
     samples, loaded on demand, where no physics body reaches: sample
     (200, 260) lies at local (-824, -764). */
  VkrScene scene;
  assert(vkr_scene_init(&scene, &test.allocator, 0, 8, NULL));
  const VkrEntityId entity = vkr_scene_create_entity(&scene, NULL);
  assert(vkr_scene_set_transform(&scene, entity, vec3_new(0.0f, 10.0f, 0.0f),
                                 vkr_quat_identity(), vec3_one()));
  vkr_scene_update_transforms(&scene);
  SceneTerrain terrain;
  vkr_type_defaults(&vkr_scene_terrain_type, &terrain);
  snprintf(terrain.heightfield, sizeof(terrain.heightfield), "%s", path);
  assert(
      vkr_scene_set_typed(&scene, entity, &vkr_scene_terrain_type, &terrain));
  assert(vkr_scene_terrain_field(&scene, entity)->resident_tiles == 0u);
  Vec3 ground = vec3_zero();
  Vec3 normal = vec3_zero();
  assert(vkr_scene_terrain_ground(&scene, vec3_new(-824.0f, 500.0f, -764.0f),
                                  -500.0f, &ground, &normal));
  assert(heightfield_test_near(ground.y, 70.0f, 0.01f) && normal.y > 0.999f);
  assert(!vkr_scene_terrain_ground(&scene, vec3_new(-824.0f, 60.0f, -764.0f),
                                   -500.0f, &ground, &normal));
  vkr_scene_shutdown(&scene, NULL);

  FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&file) == FILE_ERROR_NONE);
  heightfield_test_end(&test);
  printf("  heightfield_test_streamed PASSED\n");
}

bool32_t run_heightfield_tests(void) {
  printf("--- Starting Heightfield Tests ---\n");
  heightfield_test_file();
  heightfield_test_streamed();
  heightfield_test_operations();
  heightfield_test_journal();
  heightfield_test_tile_levels();
  printf("--- Heightfield Tests Completed ---\n");
  return true_v;
}
