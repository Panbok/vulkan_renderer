#include "heightfield_test.h"

#include "filesystem/filesystem.h"
#include "level/vkr_heightfield.h"
#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "physics/vkr_physics.h"
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
  /* Every sample is ground on layer 0; zero weights would make it a
     hole. */
  const Vec4 ground = vec4_new(1.0f, 0.0f, 0.0f, 0.0f);
  for (uint32_t z = 0; z < side; ++z) {
    for (uint32_t x = 0; x < side; ++x) {
      vertices[z * side + x] = (VkrVertex3d){
          .position = {(float32_t)x * 2.0f, 0.0f, (float32_t)z * 2.0f},
          .colour = ground};
    }
  }
  /* Skirt vertices hang below their edge samples. */
  for (uint32_t i = side * side; i < vertex_count; ++i) {
    vertices[i] =
        (VkrVertex3d){.position = {0.0f, -4.0f, 0.0f}, .colour = ground};
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
  vkr_heightfield_read_metres(&field, span, metres, 121u, 0.0f);
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

#define HOLES_SIDE (VKR_HEIGHTFIELD_TILE_CELLS + 1u)
#define HOLES_GRID (HOLES_SIDE * HOLES_SIDE)
#define HOLES_VERTICES (HOLES_GRID + 4u * HOLES_SIDE)
/* The physics grid: the tile's samples padded to a multiple of four. */
#define HOLES_PADDED 68u

/* The one tile of a 64-cell field as the terrain system builds it: a
   vertex per sample whose color holds its four weights, then a skirt
   vertex 2 m below each edge sample, edges -Z, +X, +Z, -X. */
static void heightfield_test_tile_vertices(const VkrHeightfield *field,
                                           VkrVertex3d *vertices) {
  const float32_t half = vkr_heightfield_half_size(field);
  for (uint32_t z = 0; z < HOLES_SIDE; ++z) {
    for (uint32_t x = 0; x < HOLES_SIDE; ++x) {
      const uint32_t w = vkr_heightfield_weights_at(field, x, z);
      vertices[z * HOLES_SIDE + x] = (VkrVertex3d){
          .position = {(float32_t)x * field->spacing - half,
                       vkr_heightfield_at(field, x, z),
                       (float32_t)z * field->spacing - half},
          .colour = vec4_new((float32_t)(w & 0xFFu) / 255.0f,
                             (float32_t)((w >> 8u) & 0xFFu) / 255.0f,
                             (float32_t)((w >> 16u) & 0xFFu) / 255.0f,
                             (float32_t)(w >> 24u) / 255.0f),
      };
    }
  }
  const uint32_t last = VKR_HEIGHTFIELD_TILE_CELLS;
  for (uint32_t edge = 0; edge < 4u; ++edge) {
    for (uint32_t i = 0; i < HOLES_SIDE; ++i) {
      const uint32_t x = edge == 0u   ? i
                         : edge == 1u ? last
                         : edge == 2u ? last - i
                                      : 0u;
      const uint32_t z = edge == 0u   ? 0u
                         : edge == 1u ? i
                         : edge == 2u ? last
                                      : last - i;
      VkrVertex3d skirt = vertices[z * HOLES_SIDE + x];
      skirt.position.y -= 2.0f;
      vertices[HOLES_GRID + edge * HOLES_SIDE + i] = skirt;
    }
  }
}

static bool8_t heightfield_test_vertex_open(const VkrVertex3d *vertices,
                                            uint32_t index) {
  const Vec4 w = vertices[index].colour;
  return w.x == 0.0f && w.y == 0.0f && w.z == 0.0f && w.w == 0.0f;
}

/* Holes open the same ground in a tile's mesh as in Jolt's height field
   built from the same samples. A probe in each quarter the two diagonals
   cut from every cell lies on one side of both, so it is covered by a
   drawn triangle exactly when a ray down hits the collision, and
   vkr_heightfield_open answers the same. No skirt hangs from a hole; paint
   keeps a hole, a file keeps it, and fill closes it at its old height. */
static void heightfield_test_holes(void) {
  printf("  Running heightfield_test_holes...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  VkrHeightfield field;
  /* One tile of 1 m cells: local x and z run from -32 to 32. */
  assert(vkr_heightfield_create(&field, 64u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  const VkrHeightfieldRect all = {0u, 0u, HOLES_SIDE - 1u, HOLES_SIDE - 1u};
  static uint16_t heights[HOLES_GRID];
  static uint32_t weights[HOLES_GRID];
  for (uint32_t z = 0; z < HOLES_SIDE; ++z) {
    for (uint32_t x = 0; x < HOLES_SIDE; ++x) {
      const float32_t h = 3.0f * sinf(0.37f * (float32_t)x) +
                          2.0f * cosf(0.23f * (float32_t)z) +
                          0.05f * (float32_t)x;
      heights[z * HOLES_SIDE + x] = vkr_heightfield_quantize(&field, h);
      weights[z * HOLES_SIDE + x] = 0xFFu;
    }
  }
  vkr_heightfield_write_rect(&field, all, heights, weights);

  /* A hole brush over the -Z edge, so skirts must open, and a box with
     off-grid corners inside the tile. */
  VkrHeightfieldRect touched;
  VkrHeightfieldOp op = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                         .brush = VKR_HEIGHTFIELD_HOLE,
                         .a = vec3_new(-20.0f, 0.0f, -31.6f),
                         .radius = 2.5f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  /* The brush centre is sample (12, 0.4). */
  assert(vkr_heightfield_hole(&field, 12u, 0u) &&
         vkr_heightfield_hole(&field, 14u, 0u) &&
         vkr_heightfield_hole(&field, 12u, 2u));
  assert(!vkr_heightfield_hole(&field, 15u, 0u) &&
         !vkr_heightfield_hole(&field, 12u, 3u));
  const VkrHeightfieldOp box = {.kind = VKR_HEIGHTFIELD_OP_HOLE,
                                .min = vec2_new(5.3f, 4.7f),
                                .max = vec2_new(11.6f, 9.2f),
                                .add = true_v};
  assert(vkr_heightfield_op_apply(&field, &box, &test.allocator, &touched));
  /* Local x 6 to 11 and z 5 to 9 are samples 38 to 43 and 37 to 41. */
  assert(vkr_heightfield_hole(&field, 38u, 37u) &&
         vkr_heightfield_hole(&field, 43u, 41u));
  assert(!vkr_heightfield_hole(&field, 37u, 37u) &&
         !vkr_heightfield_hole(&field, 44u, 41u) &&
         !vkr_heightfield_hole(&field, 38u, 36u) &&
         !vkr_heightfield_hole(&field, 38u, 42u));

  /* Paint over the box keeps its holes and paints the ground beside it. */
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                          .brush = VKR_HEIGHTFIELD_PAINT,
                          .layer = 2u,
                          .a = vec3_new(6.0f, 0.0f, 7.0f),
                          .radius = 2.0f,
                          .strength = 1.0f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  assert(vkr_heightfield_hole(&field, 38u, 39u));
  assert(((vkr_heightfield_weights_at(&field, 37u, 39u) >> 16u) & 0xFFu) > 0u);

  /* The tile's mesh. */
  static VkrVertex3d vertices[HOLES_VERTICES];
  heightfield_test_tile_vertices(&field, vertices);
  const uint32_t capacity = vkr_scene_terrain_tile_index_count();
  uint32_t *indices = malloc(sizeof(uint32_t) * capacity);
  assert(indices);
  VkrGpuGeometryLodRow lod;
  assert(vkr_scene_terrain_tile_indices(vertices, field.spacing, indices,
                                        capacity, &lod));
  assert(lod.level_count == 1u);

  /* Jolt's height field of the same samples, holes padding it. */
  static float32_t metres[HOLES_PADDED * HOLES_PADDED];
  for (uint32_t i = 0; i < ArrayCount(metres); ++i) {
    metres[i] = VKR_PHYSICS_HEIGHT_HOLE;
  }
  vkr_heightfield_read_metres(&field, all, metres, HOLES_PADDED,
                              VKR_PHYSICS_HEIGHT_HOLE);
  VkrPhysicsWorld *world = vkr_physics_world_create(4);
  assert(world);
  const float32_t half = vkr_heightfield_half_size(&field);
  const VkrPhysicsColliderDesc collider = {
      .entity_id = 70,
      .shape = VKR_PHYSICS_HEIGHT_FIELD,
      .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
      .scale = {1.0f, 1.0f, 1.0f},
      .geometry = {.positions = metres,
                   .vertex_count = HOLES_PADDED * HOLES_PADDED,
                   .height_samples = HOLES_PADDED,
                   .height_spacing = field.spacing},
      .enabled = true_v,
  };
  const VkrPhysicsBodyDesc desc = {.entity_id = 7,
                                   .motion = VKR_PHYSICS_STATIC,
                                   .position = {-half, 0.0f, -half},
                                   .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
                                   .mass = 1.0f,
                                   .enabled = true_v,
                                   .collision_layer = 1,
                                   .collision_mask = UINT16_MAX,
                                   .colliders = &collider,
                                   .collider_count = 1};
  VkrPhysicsBody body;
  assert(vkr_physics_body_create(world, &desc, &body));

  /* Each probe, in cell units: the middle of one quarter of the cell. */
  static const float32_t s_probe[4][2] = {
      {0.5f, 0.2f}, {0.8f, 0.5f}, {0.5f, 0.8f}, {0.2f, 0.5f}};
  const uint32_t cells = VKR_HEIGHTFIELD_TILE_CELLS;
  static uint8_t
      covered[VKR_HEIGHTFIELD_TILE_CELLS * VKR_HEIGHTFIELD_TILE_CELLS * 4u];
  for (uint32_t level = 0; level < lod.level_count; ++level) {
    const VkrGpuGeometryLodLevel range = lod.levels[level];
    MemZero(covered, sizeof(covered));
    uint32_t skirts = 0u;
    for (uint32_t t = 0; t < range.index_count; t += 3u) {
      const uint32_t *tri = &indices[range.first_index + t];
      if (tri[0] >= HOLES_GRID || tri[1] >= HOLES_GRID ||
          tri[2] >= HOLES_GRID) {
        /* A skirt hangs from drawn ground only. */
        for (uint32_t k = 0; k < 3u; ++k) {
          assert(!heightfield_test_vertex_open(vertices, tri[k]));
        }
        skirts++;
        continue;
      }
      Vec2 p[3];
      for (uint32_t k = 0; k < 3u; ++k) {
        assert(!heightfield_test_vertex_open(vertices, tri[k]));
        p[k] = vec2_new(vertices[tri[k]].position.x + half,
                        vertices[tri[k]].position.z + half);
      }
      /* Counter-clockwise from +Y: a negative x-z cross. */
      assert((p[1].x - p[0].x) * (p[2].y - p[0].y) -
                 (p[1].y - p[0].y) * (p[2].x - p[0].x) <
             0.0f);
      const uint32_t x0 = (uint32_t)Min(p[0].x, Min(p[1].x, p[2].x));
      const uint32_t z0 = (uint32_t)Min(p[0].y, Min(p[1].y, p[2].y));
      const uint32_t x1 = (uint32_t)Max(p[0].x, Max(p[1].x, p[2].x));
      const uint32_t z1 = (uint32_t)Max(p[0].y, Max(p[1].y, p[2].y));
      for (uint32_t cz = z0; cz < z1; ++cz) {
        for (uint32_t cx = x0; cx < x1; ++cx) {
          for (uint32_t k = 0; k < 4u; ++k) {
            const Vec2 q = vec2_new((float32_t)cx + s_probe[k][0],
                                    (float32_t)cz + s_probe[k][1]);
            bool8_t inside = true_v;
            for (uint32_t e = 0; e < 3u; ++e) {
              const Vec2 a = p[e];
              const Vec2 b = p[(e + 1u) % 3u];
              inside =
                  inside &&
                  (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x) < 0.0f;
            }
            covered[(cz * cells + cx) * 4u + k] += inside;
          }
        }
      }
    }
    /* The brush opened part of the -Z edge's skirt. */
    assert(skirts > 0u && skirts < 4u * cells * 2u);
    uint32_t open = 0u;
    uint32_t ground = 0u;
    for (uint32_t cz = 0; cz < cells; ++cz) {
      for (uint32_t cx = 0; cx < cells; ++cx) {
        for (uint32_t k = 0; k < 4u; ++k) {
          const uint8_t count = covered[(cz * cells + cx) * 4u + k];
          const float32_t x = (float32_t)cx + s_probe[k][0] - half;
          const float32_t z = (float32_t)cz + s_probe[k][1] - half;
          const float32_t from[3] = {x, 100.0f, z};
          const float32_t down[3] = {0.0f, -200.0f, 0.0f};
          VkrPhysicsRayHit hit;
          const bool8_t struck = vkr_physics_raycast(world, from, down, &hit);
          assert(count <= 1u);
          assert((count == 1u) == struck);
          assert(vkr_heightfield_open(&field, x, z) == !struck);
          open += !struck;
          ground += struck;
        }
      }
    }
    assert(open > 0u && ground > open);
  }
  /* The box's middle is open, ground far from both holes is not. */
  assert(vkr_heightfield_open(&field, 8.5f, 7.0f));
  assert(!vkr_heightfield_open(&field, 20.3f, 20.6f));
  vkr_physics_world_destroy(world);

  /* A file keeps the holes. */
  char path[1024];
  snprintf(path, sizeof(path),
           PROJECT_SOURCE_DIR "tests/tmp/heightfield_holes_%u.vkrhf",
           vkr_platform_get_process_id());
  FilePath directory = {.path = string8_lit(PROJECT_SOURCE_DIR "tests/tmp"),
                        .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&directory));
  char error[160];
  assert(vkr_heightfield_save(&field, path, error, sizeof(error)));
  VkrHeightfield read;
  assert(
      vkr_heightfield_load(&read, path, &test.allocator, error, sizeof(error)));
  static uint16_t read_heights[HOLES_GRID];
  static uint32_t read_weights[HOLES_GRID];
  vkr_heightfield_read_rect(&field, all, heights, weights);
  vkr_heightfield_read_rect(&read, all, read_heights, read_weights);
  assert(MemCompare(read_weights, weights, sizeof(weights)) == 0);
  assert(vkr_heightfield_hole(&read, 40u, 39u));
  vkr_heightfield_destroy(&read, &test.allocator);
  FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_remove(&file) == FILE_ERROR_NONE);

  /* Fill closes every hole on layer 0 at the height it kept, and the tile
     takes all its levels again. */
  VkrHeightfieldOp fill = box;
  fill.add = false_v;
  assert(vkr_heightfield_op_apply(&field, &fill, &test.allocator, &touched));
  op = (VkrHeightfieldOp){.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                          .brush = VKR_HEIGHTFIELD_FILL,
                          .a = vec3_new(-20.0f, 0.0f, -31.6f),
                          .radius = 2.5f};
  assert(vkr_heightfield_op_apply(&field, &op, &test.allocator, &touched));
  for (uint32_t z = 0; z < HOLES_SIDE; ++z) {
    for (uint32_t x = 0; x < HOLES_SIDE; ++x) {
      assert(!vkr_heightfield_hole(&field, x, z));
      assert(vkr_heightfield_raw(&field, x, z) == heights[z * HOLES_SIDE + x]);
    }
  }
  assert(vkr_heightfield_weights_at(&field, 40u, 39u) == 0xFFu);
  heightfield_test_tile_vertices(&field, vertices);
  assert(vkr_scene_terrain_tile_indices(vertices, field.spacing, indices,
                                        capacity, &lod) == capacity);
  assert(lod.level_count == VKR_SCENE_TERRAIN_LOD_LEVELS);
  free(indices);
  vkr_heightfield_destroy(&field, &test.allocator);
  heightfield_test_end(&test);
  printf("  heightfield_test_holes PASSED\n");
}

/* Fails when the samples op does not copy another op's result exactly:
   heights and paint, inside the rectangle only. */
static void heightfield_test_samples(void) {
  printf("  Running heightfield_test_samples...\n");
  HeightfieldTest test;
  heightfield_test_begin(&test);
  VkrHeightfield source;
  VkrHeightfield copy;
  assert(vkr_heightfield_create(&source, 64u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  assert(vkr_heightfield_create(&copy, 64u, 1.0f, -50.0f, 50.0f, 0.0f,
                                &test.allocator));
  VkrHeightfieldRect touched;
  const VkrHeightfieldOp raise = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                                  .brush = VKR_HEIGHTFIELD_RAISE,
                                  .a = vec3_new(3.0f, 0.0f, -2.0f),
                                  .radius = 5.0f,
                                  .strength = 1.7f};
  assert(vkr_heightfield_op_apply(&source, &raise, &test.allocator, &touched));
  const VkrHeightfieldOp paint = {.kind = VKR_HEIGHTFIELD_OP_BRUSH,
                                  .brush = VKR_HEIGHTFIELD_PAINT,
                                  .a = vec3_new(3.0f, 0.0f, -2.0f),
                                  .radius = 5.0f,
                                  .strength = 0.6f,
                                  .layer = 2u};
  VkrHeightfieldRect painted;
  assert(vkr_heightfield_op_apply(&source, &paint, &test.allocator, &painted));
  (void)vkr_heightfield_rect_union(&source, touched, painted, &touched);
  const uint32_t count = vkr_heightfield_rect_count(touched);
  uint16_t *heights = malloc(count * sizeof(uint16_t));
  uint32_t *weights = malloc(count * sizeof(uint32_t));
  assert(heights && weights);
  vkr_heightfield_read_rect(&source, touched, heights, weights);
  const VkrHeightfieldOp samples = {.kind = VKR_HEIGHTFIELD_OP_SAMPLES,
                                    .rect = touched,
                                    .heights = heights,
                                    .weights = weights};
  VkrHeightfieldRect written;
  assert(vkr_heightfield_op_apply(&copy, &samples, &test.allocator, &written));
  assert(MemCompare(&written, &touched, sizeof(written)) == 0);
  const VkrHeightfieldRect whole = {0u, 0u, 64u, 64u};
  const uint32_t all = vkr_heightfield_rect_count(whole);
  uint16_t *a_heights = malloc(all * sizeof(uint16_t));
  uint16_t *b_heights = malloc(all * sizeof(uint16_t));
  uint32_t *a_weights = malloc(all * sizeof(uint32_t));
  uint32_t *b_weights = malloc(all * sizeof(uint32_t));
  assert(a_heights && b_heights && a_weights && b_weights);
  vkr_heightfield_read_rect(&source, whole, a_heights, a_weights);
  vkr_heightfield_read_rect(&copy, whole, b_heights, b_weights);
  assert(MemCompare(a_heights, b_heights, all * sizeof(uint16_t)) == 0);
  assert(MemCompare(a_weights, b_weights, all * sizeof(uint32_t)) == 0);
  /* A rectangle past the field is refused. */
  VkrHeightfieldOp outside = samples;
  outside.rect.x1 = 65u;
  assert(!vkr_heightfield_op_rect(&copy, &outside, &written));
  free(a_heights);
  free(b_heights);
  free(a_weights);
  free(b_weights);
  free(heights);
  free(weights);
  vkr_heightfield_destroy(&source, &test.allocator);
  vkr_heightfield_destroy(&copy, &test.allocator);
  heightfield_test_end(&test);
  printf("  heightfield_test_samples PASSED\n");
}

bool32_t run_heightfield_tests(void) {
  printf("--- Starting Heightfield Tests ---\n");
  heightfield_test_file();
  heightfield_test_streamed();
  heightfield_test_operations();
  heightfield_test_journal();
  heightfield_test_tile_levels();
  heightfield_test_holes();
  heightfield_test_samples();
  printf("--- Heightfield Tests Completed ---\n");
  return true_v;
}
