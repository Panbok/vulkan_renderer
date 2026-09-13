#include "collision_asset_test.h"
#include "assets/vkr_collision_cooked.h"
#include "assets/vkr_collision_hull.h"
#include "assets/vkr_collision_import.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "physics/vkr_collision_asset.h"
#include "platform/vkr_platform.h"

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct CollisionFixture {
  Arena *result_arena;
  Arena *scratch_arena;
  VkrAllocator result;
  VkrAllocator scratch;
  char directory[1024];
  char asset_path[1100];
  char gltf_path[1100];
  char binary_path[1100];
} CollisionFixture;

static int32_t fixture_mkdir(const char *path) {
#if defined(_WIN32)
  return _mkdir(path);
#else
  return mkdir(path, 0755);
#endif
}

static void fixture_begin(CollisionFixture *fixture) {
  *fixture = (CollisionFixture){0};
  char parent[1024];
  snprintf(parent, sizeof(parent), "%stests/tmp", PROJECT_SOURCE_DIR);
  int32_t status = fixture_mkdir(parent);
  assert(status == 0 || errno == EEXIST);
  static uint32_t sequence = 0;
  for (uint32_t attempt = 0; attempt < 100; ++attempt) {
    snprintf(fixture->directory, sizeof(fixture->directory),
             "%s/collision_%u_%u", parent, vkr_platform_get_process_id(),
             ++sequence);
    status = fixture_mkdir(fixture->directory);
    if (!status || errno != EEXIST) {
      break;
    }
  }
  assert(status == 0);
  snprintf(fixture->asset_path, sizeof(fixture->asset_path), "%s/fixture.vkc",
           fixture->directory);
  snprintf(fixture->gltf_path, sizeof(fixture->gltf_path), "%s/fixture.gltf",
           fixture->directory);
  snprintf(fixture->binary_path, sizeof(fixture->binary_path), "%s/fixture.bin",
           fixture->directory);
  fixture->result_arena = arena_create(MB(4), KB(64));
  fixture->scratch_arena = arena_create(MB(4), KB(64));
  assert(fixture->result_arena && fixture->scratch_arena);
  fixture->result.ctx = fixture->result_arena;
  fixture->scratch.ctx = fixture->scratch_arena;
  assert(vkr_allocator_arena(&fixture->result));
  assert(vkr_allocator_arena(&fixture->scratch));
}

static void fixture_end(CollisionFixture *fixture) {
  vkr_allocator_release_global_accounting(&fixture->scratch);
  vkr_allocator_release_global_accounting(&fixture->result);
  arena_destroy(fixture->scratch_arena);
  arena_destroy(fixture->result_arena);
  const char *files[] = {fixture->asset_path, fixture->gltf_path,
                         fixture->binary_path};
  for (uint32_t i = 0; i < ArrayCount(files); ++i) {
    const int32_t status = remove(files[i]);
    assert(status == 0 || errno == ENOENT);
  }
#if defined(_WIN32)
  assert(_rmdir(fixture->directory) == 0);
#else
  assert(rmdir(fixture->directory) == 0);
#endif
}

static void write_bytes(const char *path, const void *bytes, uint64_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  const size_t written = fwrite(bytes, 1, (size_t)size, file);
  const int32_t status = fclose(file);
  assert(written == size && status == 0);
}

static void put_u32(uint8_t *bytes, uint32_t value) {
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8);
  bytes[2] = (uint8_t)(value >> 16);
  bytes[3] = (uint8_t)(value >> 24);
}

/* Test fixture construction only: repair the documented FNV1a checksum so a
 * malformed numeric payload reaches its independent semantic validator. */
static void repair_checksum(uint8_t *bytes, uint64_t size) {
  MemZero(bytes + 24, 8);
  uint64_t checksum = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < size; ++i) {
    checksum ^= bytes[i];
    checksum *= UINT64_C(1099511628211);
  }
  put_u32(bytes + 24, (uint32_t)checksum);
  put_u32(bytes + 28, (uint32_t)(checksum >> 32));
}

static void expect_invalid(CollisionFixture *fixture, uint8_t *bytes,
                           uint64_t size) {
  VkrCollisionGeometry geometry = {.vertex_count = 99};
  const char *error = NULL;
  assert(!vkr_collision_cooked_decode(&fixture->result, bytes, size, &geometry,
                                      &error));
  assert(error && !geometry.positions && !geometry.indices &&
         !geometry.vertex_count);
}

static void test_codec_and_asset_lifetime(void) {
  CollisionFixture fixture;
  fixture_begin(&fixture);
  const float32_t positions[] = {0, 0, 0, 2, 0, 0, 0, 3, 0};
  const uint32_t indices[] = {0, 1, 2};
  const VkrCollisionGeometry source = {
      .kind = VKR_COLLISION_TRIANGLE_MESH,
      .positions = positions,
      .vertex_count = 3,
      .indices = indices,
      .index_count = 3,
      .source_fingerprint = UINT64_C(0x123456789abcdef0),
  };
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  const char *error = NULL;
  assert(vkr_collision_cooked_encode(&fixture.result, &source, &bytes, &size,
                                     &error));
  assert(size == 96 && !error);
  VkrCollisionGeometry decoded;
  assert(vkr_collision_cooked_decode(&fixture.result, bytes, size, &decoded,
                                     &error));
  assert(decoded.kind == VKR_COLLISION_TRIANGLE_MESH);
  assert(decoded.vertex_count == 3 && decoded.index_count == 3);
  assert(decoded.source_fingerprint == UINT64_C(0x123456789abcdef0));
  assert(decoded.positions[3] == 2 && decoded.positions[7] == 3);
  assert(decoded.indices[0] == 0 && decoded.indices[1] == 1 &&
         decoded.indices[2] == 2);
  assert(decoded.positions != positions && decoded.indices != indices);
  write_bytes(fixture.asset_path, bytes, size);
  String8 path =
      string8_create((uint8_t *)fixture.asset_path, strlen(fixture.asset_path));
  VkrCollisionAsset *asset = vkr_collision_asset_open(path, &error);
  assert(asset && !error);
  const VkrCollisionGeometry *borrowed = vkr_collision_asset_geometry(asset);
  assert(borrowed && borrowed->positions[7] == 3);
  assert(vkr_collision_asset_retain(asset));
  vkr_collision_asset_close(asset);
  assert(vkr_collision_asset_geometry(asset) == borrowed);
  assert(borrowed->indices[2] == 2 &&
         borrowed->source_fingerprint == source.source_fingerprint);
  vkr_collision_asset_close(asset);
  assert(!vkr_collision_asset_retain(NULL));
  vkr_collision_asset_close(NULL);

  uint8_t corrupt[96];
  MemCopy(corrupt, bytes, size);
  corrupt[50] ^= 1;
  expect_invalid(&fixture, corrupt, size); // Checksum mismatch.
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 4, VKR_COLLISION_COOKED_VERSION + 1);
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 92, 3); // Vertex index equals vertex_count.
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 48, UINT32_C(0x7fc00000)); // Quiet NaN vertex coordinate.
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 92, 1); // Repeated triangle index.
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 76, 0); // Third vertex moves onto first: zero area.
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  MemCopy(corrupt, bytes, size);
  put_u32(corrupt + 36, UINT32_MAX);
  repair_checksum(corrupt, size);
  expect_invalid(&fixture, corrupt, size);
  expect_invalid(&fixture, bytes, size - 1);
  write_bytes(fixture.asset_path, corrupt, size);
  assert(!vkr_collision_asset_open(path, &error) && error);
  fixture_end(&fixture);
}

static float64_t triangle_normal_z(const VkrCollisionGeometry *g,
                                   uint32_t triangle) {
  const float32_t *a = g->positions + g->indices[triangle * 3] * 3;
  const float32_t *b = g->positions + g->indices[triangle * 3 + 1] * 3;
  const float32_t *c = g->positions + g->indices[triangle * 3 + 2] * 3;
  return ((float64_t)b[0] - a[0]) * ((float64_t)c[1] - a[1]) -
         ((float64_t)b[1] - a[1]) * ((float64_t)c[0] - a[0]);
}

static void test_import_transform_reflection_and_subtree(void) {
  CollisionFixture fixture;
  fixture_begin(&fixture);
  uint8_t binary[42] = {0};
  const float32_t positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  for (uint32_t i = 0; i < ArrayCount(positions); ++i) {
    uint32_t bits;
    MemCopy(&bits, positions + i, sizeof(bits));
    put_u32(binary + i * 4, bits);
  }
  binary[38] = 1;
  binary[40] = 2;
  write_bytes(fixture.binary_path, binary, sizeof(binary));
  const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0,3]}],"
      "\"nodes\":[{\"translation\":[10,0,0],\"children\":[1]},"
      "{\"mesh\":0,\"translation\":[2,0,0],\"scale\":[-2,3,1],\"children\":[2]}"
      ","
      "{\"mesh\":0,\"translation\":[0,4,0]},{\"mesh\":0,\"translation\":[100,0,"
      "0]}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
      "\"indices\":1}]}],"
      "\"buffers\":[{\"uri\":\"fixture.bin\",\"byteLength\":42}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC3\","
      "\"min\":[0,0,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":"
      "\"SCALAR\"}]}";
  write_bytes(fixture.gltf_path, json, sizeof(json) - 1);
  const String8 path =
      string8_create((uint8_t *)fixture.gltf_path, strlen(fixture.gltf_path));
  VkrCollisionGeometry geometry;
  const char *error = NULL;
  assert(vkr_collision_import_gltf(&fixture.result, &fixture.scratch, path,
                                   UINT32_MAX, VKR_COLLISION_TRIANGLE_MESH,
                                   &geometry, &error));
  assert(geometry.vertex_count == 9 && geometry.index_count == 9);
  assert(geometry.positions[0] == 12 && geometry.positions[3] == 10);
  assert(geometry.positions[9] == 12 && geometry.positions[10] == 12);
  assert(geometry.positions[18] == 100);
  assert(triangle_normal_z(&geometry, 0) > 0);
  assert(triangle_normal_z(&geometry, 1) > 0);
  assert(vkr_collision_import_gltf(&fixture.result, &fixture.scratch, path, 1,
                                   VKR_COLLISION_TRIANGLE_MESH, &geometry,
                                   &error));
  assert(geometry.vertex_count == 6 && geometry.index_count == 6);
  assert(fabsf(geometry.positions[0]) < 0.0001f && geometry.positions[3] == 1);
  assert(geometry.positions[9] == 0 && geometry.positions[10] == 4);
  assert(triangle_normal_z(&geometry, 0) > 0);
  assert(!vkr_collision_import_gltf(&fixture.result, &fixture.scratch, path, 99,
                                    VKR_COLLISION_TRIANGLE_MESH, &geometry,
                                    &error));
  assert(error && !geometry.positions);
  fixture_end(&fixture);
}

static void test_hull_volume_and_support(void) {
  const float32_t points[] = {-1, -1, -1, 1,  -1, -1, -1, 1,     -1,    1,
                              1,  -1, -1, -1, 1,  1,  -1, 1,     -1,    1,
                              1,  1,  1,  1,  0,  0,  0,  0.25f, -0.5f, 0.75f};
  float32_t positions[256 * 3];
  uint32_t indices[1536];
  uint32_t vertex_count, index_count;
  const char *error = NULL;
  assert(vkr_collision_build_hull(points, ArrayCount(points) / 3, positions,
                                  indices, &vertex_count, &index_count,
                                  &error));
  assert(vertex_count == 8 && index_count == 36);
  float64_t volume = 0;
  for (uint32_t t = 0; t < index_count; t += 3) {
    assert(indices[t] < vertex_count && indices[t + 1] < vertex_count &&
           indices[t + 2] < vertex_count);
    const float32_t *a = positions + indices[t] * 3;
    const float32_t *b = positions + indices[t + 1] * 3;
    const float32_t *c = positions + indices[t + 2] * 3;
    const float64_t cross_bc[3] = {b[1] * c[2] - b[2] * c[1],
                                   b[2] * c[0] - b[0] * c[2],
                                   b[0] * c[1] - b[1] * c[0]};
    volume +=
        (a[0] * cross_bc[0] + a[1] * cross_bc[1] + a[2] * cross_bc[2]) / 6;
    float64_t ab[3], ac[3];
    for (uint32_t k = 0; k < 3; ++k) {
      ab[k] = (float64_t)b[k] - a[k];
      ac[k] = (float64_t)c[k] - a[k];
    }
    const float64_t normal[3] = {ab[1] * ac[2] - ab[2] * ac[1],
                                 ab[2] * ac[0] - ab[0] * ac[2],
                                 ab[0] * ac[1] - ab[1] * ac[0]};
    for (uint32_t v = 0; v < ArrayCount(points) / 3; ++v) {
      float64_t side = 0;
      for (uint32_t k = 0; k < 3; ++k) {
        side += normal[k] * ((float64_t)points[v * 3 + k] - a[k]);
      }
      assert(side <=
             0.00001); // Every input point lies inside every face plane.
    }
  }
  assert(fabs(volume - 8) < 0.00001);
  VkrCollisionGeometry hull = {
      .kind = VKR_COLLISION_CONVEX_HULL,
      .positions = positions,
      .vertex_count = vertex_count,
      .indices = indices,
      .index_count = index_count,
  };
  assert(vkr_collision_geometry_validate(&hull, &error));
  hull.index_count -= 3;
  assert(!vkr_collision_geometry_validate(&hull, &error)); // Open hull surface.
  hull.index_count = index_count;
  uint32_t saved = indices[1];
  indices[1] = indices[2];
  indices[2] = saved;
  assert(
      !vkr_collision_geometry_validate(&hull, &error)); // Inward-facing face.
  indices[2] = indices[1];
  indices[1] = saved;
  assert(vkr_collision_geometry_validate(&hull, &error));
  assert(!vkr_collision_build_hull(NULL, 4, positions, indices, &vertex_count,
                                   &index_count, &error));
  assert(vertex_count == 0 && index_count == 0 && error);
  assert(!vkr_collision_build_hull(points, 10, positions, indices, NULL,
                                   &index_count, &error));

  const float32_t flat[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
  assert(!vkr_collision_build_hull(flat, 4, positions, indices, &vertex_count,
                                   &index_count, &error));
  assert(vertex_count == 0 && index_count == 0 && error);
}

bool32_t run_collision_asset_tests(void) {
  printf("Running collision asset tests...\n");
  test_codec_and_asset_lifetime();
  test_import_transform_reflection_and_subtree();
  test_hull_volume_and_support();
  printf("Collision asset tests PASSED\n");
  return true_v;
}
