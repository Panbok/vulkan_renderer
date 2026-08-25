#include "mesh_cooked_tests.h"

#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/vkr_mesh_cooked.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#define TEST_HEADER_SIZE 272u
#define TEST_RANGE_SIZE 168u
#define TEST_STREAM_OFFSET_FIELD 80u
#define TEST_HEADER_CRC_FIELD 184u
#define TEST_METADATA_CRC_FIELD 188u
#define TEST_VERTEX_STREAM_OFFSET_FIELD 48u
#define TEST_VERTEX_CRC_FIELD 72u
#define TEST_CENTER_X_FIELD 104u

static uint32_t test_crc32(const uint8_t *data, uint64_t size) {
  uint32_t crc = 0xffffffffu;
  for (uint64_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static uint32_t test_read_le32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint64_t test_read_le64(const uint8_t *data) {
  return (uint64_t)test_read_le32(data) |
         ((uint64_t)test_read_le32(data + 4u) << 32u);
}

static void test_write_le32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8u);
  data[2] = (uint8_t)(value >> 16u);
  data[3] = (uint8_t)(value >> 24u);
}

static void test_write_le64(uint8_t *data, uint64_t value) {
  test_write_le32(data, (uint32_t)value);
  test_write_le32(data + 4u, (uint32_t)(value >> 32u));
}

static void test_write_f32(uint8_t *data, float32_t value) {
  uint32_t bits = 0;
  MemCopy(&bits, &value, sizeof(bits));
  test_write_le32(data, bits);
}

static void test_refresh_integrity(uint8_t *data) {
  uint64_t stream_offset = test_read_le64(data + TEST_STREAM_OFFSET_FIELD);
  assert(stream_offset >= TEST_HEADER_SIZE);
  test_write_le32(
      data + TEST_METADATA_CRC_FIELD,
      test_crc32(data + TEST_HEADER_SIZE, stream_offset - TEST_HEADER_SIZE));
  test_write_le32(data + TEST_HEADER_CRC_FIELD, 0u);
  test_write_le32(data + TEST_HEADER_CRC_FIELD,
                  test_crc32(data, TEST_HEADER_SIZE));
}

static VkrVertex3d test_vertex(float32_t x, float32_t y, float32_t z) {
  return (VkrVertex3d){
      .position = {x, y, z},
      .normal = {0.0f, 0.0f, 1.0f},
      .texcoord = {x, y},
      .colour = {1.0f, 1.0f, 1.0f, 1.0f},
      .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
  };
}

static bool8_t test_contains_position(const VkrVertex3d *vertices,
                                      uint32_t count, VkrPackedVec3 position) {
  for (uint32_t i = 0; i < count; ++i) {
    if (fabsf(vertices[i].position.x - position.x) <= 1.0e-4f &&
        fabsf(vertices[i].position.y - position.y) <= 1.0e-4f &&
        fabsf(vertices[i].position.z - position.z) <= 1.0e-4f) {
      return true_v;
    }
  }
  return false_v;
}

static void test_packed_geometry_validation_contract(void) {
  printf("  Running test_packed_geometry_validation_contract...\n");
  VkrVertex3d vertices[3] = {
      test_vertex(0.0f, 0.0f, 0.0f),
      test_vertex(1.0f, 0.0f, 0.0f),
      test_vertex(0.0f, 1.0f, 0.0f),
  };
  VkrPackedStaticVertex packed[3] = {0};
  VkrGpuGeometryDecodeRecord decode = {0};
  VkrGeometryQuantizationMetrics metrics = {0};
  VkrGeometryQuantizationBudgets budgets =
      vkr_packed_geometry_default_budgets();
  assert(vkr_packed_geometry_pack(
      vertices, ArrayCount(vertices), vec3_new(0.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 1.0f, 0.0f), &budgets, packed, &decode, &metrics));
  assert(vkr_packed_geometry_decode_is_valid(&decode));
  assert(vkr_packed_geometry_vertices_are_valid(packed, ArrayCount(packed),
                                                &decode));

  VkrPackedStaticVertex invalid = packed[0];
  invalid.words[7] = 1u;
  assert(!vkr_packed_geometry_vertices_are_valid(&invalid, 1u, &decode));
  invalid = packed[0];
  invalid.words[1] |= 1u << 17u;
  assert(!vkr_packed_geometry_vertices_are_valid(&invalid, 1u, &decode));
  invalid = packed[0];
  float32_t nan_value = NAN;
  MemCopy(&invalid.words[4], &nan_value, sizeof(nan_value));
  assert(!vkr_packed_geometry_vertices_are_valid(&invalid, 1u, &decode));

  VkrGpuGeometryDecodeRecord invalid_decode = decode;
  invalid_decode.reserved = 1u;
  assert(!vkr_packed_geometry_decode_is_valid(&invalid_decode));
  invalid_decode = decode;
  invalid_decode.position_scale[0] = INFINITY;
  assert(!vkr_packed_geometry_decode_is_valid(&invalid_decode));

  vertices[0].tangent.w = 0.0f;
  assert(!vkr_packed_geometry_pack(
      vertices, ArrayCount(vertices), vec3_new(0.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 1.0f, 0.0f), &budgets, packed, &decode, &metrics));
  vertices[0].tangent.w = 1.0f;
  budgets.uv_absolute = NAN;
  assert(!vkr_packed_geometry_pack(
      vertices, ArrayCount(vertices), vec3_new(0.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 1.0f, 0.0f), &budgets, packed, &decode, &metrics));
  printf("  test_packed_geometry_validation_contract PASSED\n");
}

static void test_mesh_cooked_round_trip_and_malformed_boundaries(void) {
  printf("  Running test_mesh_cooked_round_trip_and_malformed_boundaries...\n");
  static const char dependency_path[] = "build/vkr_mesh_cooked_dependency.bin";
  static const char dependency_bytes[] = "mesh-cooked-dependency-v1\n";
  FILE *dependency = fopen(dependency_path, "wb");
  assert(dependency != NULL);
  assert(fwrite(dependency_bytes, 1u, sizeof(dependency_bytes) - 1u,
                dependency) == sizeof(dependency_bytes) - 1u);
  assert(fclose(dependency) == 0);

  Arena *scratch_arena = arena_create(MB(32), MB(4));
  Arena *result_arena = arena_create(MB(4), MB(1));
  assert(scratch_arena != NULL && result_arena != NULL);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch));
  assert(vkr_allocator_arena(&result));

  VkrVertex3d vertices[6] = {
      test_vertex(0.0f, 0.0f, 0.0f), test_vertex(1.0f, 0.0f, 0.0f),
      test_vertex(0.0f, 1.0f, 0.0f), test_vertex(2.0f, 0.0f, 0.0f),
      test_vertex(3.0f, 0.0f, 0.0f), test_vertex(2.0f, 1.0f, 0.0f),
  };
  uint32_t indices[6] = {0, 1, 2, 3, 4, 5};
  VkrMeshLoaderSubmeshRange ranges[2] = {
      {
          .range_id = 0,
          .first_index = 0,
          .index_count = 3,
          .center = {0.5f, 0.5f, 0.0f},
          .min_extents = {0.0f, 0.0f, 0.0f},
          .max_extents = {1.0f, 1.0f, 0.0f},
          .material_name = string8_lit("material.first"),
          .shader_override = string8_lit("shader.first"),
          .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
          .material_handle = VKR_MATERIAL_HANDLE_INVALID,
      },
      {
          .range_id = 1,
          .first_index = 3,
          .index_count = 3,
          .center = {2.5f, 0.5f, 0.0f},
          .min_extents = {2.0f, 0.0f, 0.0f},
          .max_extents = {3.0f, 1.0f, 0.0f},
          .material_name = string8_lit("material.second"),
          .shader_override = string8_lit("shader.second"),
          .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
          .material_handle = VKR_MATERIAL_HANDLE_INVALID,
      },
  };
  String8 dependency_string = string8_lit(dependency_path);
  VkrMeshCookedEncodeInfo info = {
      .source_path = dependency_string,
      .dependency_paths = &dependency_string,
      .dependency_count = 1,
      .mesh_buffer =
          {
              .vertex_size = sizeof(VkrVertex3d),
              .vertex_count = 6,
              .vertices = vertices,
              .index_size = sizeof(uint32_t),
              .index_count = 6,
              .indices = indices,
          },
      .ranges = ranges,
      .range_count = 2,
      .budgets = vkr_packed_geometry_default_budgets(),
  };

  uint8_t *first = NULL;
  uint64_t first_size = 0;
  uint8_t *second = NULL;
  uint64_t second_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &first, &first_size));
  assert(vkr_mesh_cooked_encode(&scratch, &info, &second, &second_size));
  assert(first_size == second_size);
  assert(MemCompare(first, second, first_size) == 0);

  VkrVertex3d strict_vertices[6];
  MemCopy(strict_vertices, vertices, sizeof(vertices));
  strict_vertices[1].colour.x = 0.3f;
  VkrMeshCookedEncodeInfo strict_info = info;
  strict_info.mesh_buffer.vertices = strict_vertices;
  strict_info.budgets.color_absolute = 1e-6f;
  uint8_t *rejected = NULL;
  uint64_t rejected_size = 0;
  assert(!vkr_mesh_cooked_encode(&scratch, &strict_info, &rejected,
                                 &rejected_size));

  static const uint8_t expected_dependency_sha256[32] = {
      0x03, 0x7f, 0xbd, 0x1f, 0x18, 0x02, 0x5b, 0x4e, 0x31, 0xdc, 0x40,
      0xab, 0xe6, 0x64, 0x12, 0x62, 0x0e, 0x20, 0xe0, 0x92, 0xdf, 0xaf,
      0x4f, 0xbf, 0xcc, 0xc9, 0x70, 0x6a, 0x02, 0xfd, 0x40, 0xd6,
  };
  const uint64_t dependency_hash_offset =
      TEST_HEADER_SIZE + 2u * TEST_RANGE_SIZE + 24u;
  assert(MemCompare(first + dependency_hash_offset, expected_dependency_sha256,
                    sizeof(expected_dependency_sha256)) == 0);

  VkrMeshCookedDecoded decoded = {0};
  assert(
      vkr_mesh_cooked_decode(&result, &scratch, first, first_size, &decoded));
  assert(decoded.mesh_buffer.vertex_count == 6);
  assert(decoded.mesh_buffer.index_count == 6);
  assert(decoded.ranges.length == 2);
  assert(decoded.ranges.data[0].range_id == 0);
  assert(decoded.ranges.data[0].first_index == 0);
  assert(decoded.ranges.data[1].range_id == 1);
  assert(decoded.ranges.data[1].first_index == 3);
  assert(string8_equals(&decoded.ranges.data[0].material_name,
                        &ranges[0].material_name));
  assert(string8_equals(&decoded.ranges.data[1].material_name,
                        &ranges[1].material_name));
  assert(decoded.mesh_buffer.vertex_layout ==
         VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1);
  assert(decoded.mesh_buffer.vertex_size == sizeof(VkrPackedStaticVertex));
  VkrVertex3d decoded_vertices[6] = {0};
  vkr_packed_geometry_unpack(decoded.mesh_buffer.vertices, 6u,
                             &decoded.mesh_buffer.decode, decoded_vertices);
  const uint32_t *decoded_indices = decoded.mesh_buffer.indices;
  for (uint32_t i = 0; i < 6; ++i) {
    assert(decoded_indices[i] < decoded.mesh_buffer.vertex_count);
    assert(test_contains_position(decoded_vertices, 6, vertices[i].position));
  }

  static const char loader_artifact_path[] = "build/vkr_mesh_cooked_loader.vkb";
  VkrMeshLoaderSubmeshRange loader_range = ranges[0];
  loader_range.material_name = (String8){0};
  loader_range.shader_override = (String8){0};
  VkrMeshCookedEncodeInfo loader_info = info;
  loader_info.mesh_buffer.vertex_count = 3;
  loader_info.mesh_buffer.index_count = 3;
  loader_info.ranges = &loader_range;
  loader_info.range_count = 1;
  uint8_t *loader_artifact = NULL;
  uint64_t loader_artifact_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &loader_info, &loader_artifact,
                                &loader_artifact_size));
  assert(vkr_mesh_cooked_write_atomic(&scratch,
                                      string8_lit(loader_artifact_path),
                                      loader_artifact, loader_artifact_size));
  assert(remove(dependency_path) == 0);

  VkrArenaPool arena_pool = {0};
  assert(vkr_arena_pool_create(MB(1), 1, &scratch, &arena_pool));
  VkrMeshLoaderContext loader_context = {.arena_pool = &arena_pool};
  VkrResourceLoader loader = vkr_mesh_loader_create(&loader_context);
  assert(loader.can_load(&loader, string8_lit(loader_artifact_path)));
  VkrResourceHandleInfo handle = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(loader_artifact_path), &scratch,
                     &handle, &load_error));
  assert(load_error == VKR_RENDERER_ERROR_NONE);
  assert(handle.type == VKR_RESOURCE_TYPE_MESH);
  assert(handle.as.mesh->has_mesh_buffer);
  assert(handle.as.mesh->mesh_buffer.vertex_count == 3);
  assert(handle.as.mesh->mesh_buffer.index_count == 3);
  assert(handle.as.mesh->submeshes.length == 1);
  assert(handle.as.mesh->load_metrics.preparation ==
         VKR_MESH_PREPARATION_COOKED);
  assert(handle.as.mesh->load_metrics.cooked_bytes == loader_artifact_size);
  assert(handle.as.mesh->load_metrics.decoded_bytes ==
         3u * sizeof(VkrPackedStaticVertex) + 3u * sizeof(uint32_t));
  assert(handle.as.mesh->load_metrics.vertices_transformed_before > 0);
  assert(handle.as.mesh->load_metrics.vertices_transformed_after ==
         handle.as.mesh->load_metrics.vertices_transformed_before);
  assert(arena_pool.pool.allocated == 1u);
  loader.unload(&loader, &handle, string8_lit(loader_artifact_path));
  assert(arena_pool.pool.allocated == 0u);
  for (uint32_t iteration = 0; iteration < 16u; ++iteration) {
    handle = (VkrResourceHandleInfo){0};
    load_error = VKR_RENDERER_ERROR_NONE;
    assert(loader.load(&loader, string8_lit(loader_artifact_path), &scratch,
                       &handle, &load_error));
    assert(arena_pool.pool.allocated == 1u);
    loader.unload(&loader, &handle, string8_lit(loader_artifact_path));
    assert(arena_pool.pool.allocated == 0u);
  }

  loader_context.allocator = scratch;
  assert(vkr_dmemory_create(KB(64), MB(2), &loader_context.async_memory));
  loader_context.async_allocator =
      (VkrAllocator){.ctx = &loader_context.async_memory};
  vkr_dmemory_allocator_create(&loader_context.async_allocator);
  assert(vkr_mutex_create(&scratch, &loader_context.async_mutex));
  for (uint32_t iteration = 0; iteration < 16u; ++iteration) {
    void *payload = NULL;
    load_error = VKR_RENDERER_ERROR_NONE;
    assert(loader.prepare_async(&loader, string8_lit(loader_artifact_path),
                                &scratch, &payload, &load_error));
    assert(payload != NULL);
    assert(arena_pool.pool.allocated == 1u);
    loader.release_async_payload(&loader, payload);
    assert(arena_pool.pool.allocated == 0u);
  }

  static const char invalid_loader_path[] =
      "build/vkr_mesh_cooked_loader_invalid.vkb";
  uint8_t *invalid_loader_artifact = vkr_allocator_alloc(
      &scratch, loader_artifact_size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  assert(invalid_loader_artifact != NULL);
  MemCopy(invalid_loader_artifact, loader_artifact, loader_artifact_size);
  const uint64_t loader_vertex_stream_offset =
      test_read_le64(invalid_loader_artifact + TEST_HEADER_SIZE +
                     TEST_VERTEX_STREAM_OFFSET_FIELD);
  invalid_loader_artifact[loader_vertex_stream_offset + 1u] ^= 0x80u;
  assert(vkr_mesh_cooked_write_atomic(
      &scratch, string8_lit(invalid_loader_path), invalid_loader_artifact,
      loader_artifact_size));
  void *failed_payload = NULL;
  load_error = VKR_RENDERER_ERROR_NONE;
  assert(!loader.prepare_async(&loader, string8_lit(invalid_loader_path),
                               &scratch, &failed_payload, &load_error));
  assert(failed_payload == NULL);
  assert(arena_pool.pool.allocated == 0u);
  remove(invalid_loader_path);

  vkr_mutex_destroy(&scratch, &loader_context.async_mutex);
  vkr_dmemory_allocator_destroy(&loader_context.async_allocator);
  vkr_arena_pool_destroy(&scratch, &arena_pool);
  assert(arena_pool.pool.allocated == 0u);
  remove(loader_artifact_path);

  uint8_t *mutated =
      vkr_allocator_alloc(&scratch, first_size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  assert(mutated != NULL);

  MemCopy(mutated, first, first_size);
  test_write_le64(mutated + TEST_HEADER_SIZE + TEST_VERTEX_STREAM_OFFSET_FIELD,
                  first_size);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  test_write_le64(mutated + TEST_HEADER_SIZE + 64u,
                  sizeof(VkrPackedStaticVertex) - 1u);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  mutated[TEST_HEADER_SIZE + 32u] ^= 0x01u;
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  test_write_f32(mutated + TEST_HEADER_SIZE + TEST_CENTER_X_FIELD, 0.25f);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  uint64_t vertex_stream_offset = test_read_le64(
      mutated + TEST_HEADER_SIZE + TEST_VERTEX_STREAM_OFFSET_FIELD);
  mutated[vertex_stream_offset + 1u] ^= 0x80u;
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  vertex_stream_offset = test_read_le64(mutated + TEST_HEADER_SIZE +
                                        TEST_VERTEX_STREAM_OFFSET_FIELD);
  uint64_t vertex_stream_size =
      test_read_le64(mutated + TEST_HEADER_SIZE + 56u);
  mutated[vertex_stream_offset] ^= 0xffu;
  test_write_le32(
      mutated + TEST_HEADER_SIZE + TEST_VERTEX_CRC_FIELD,
      test_crc32(mutated + vertex_stream_offset, vertex_stream_size));
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  assert(
      vkr_mesh_cooked_decode(&result, &scratch, first, first_size, &decoded));
  vkr_allocator_release_global_accounting(&result);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(result_arena);
  arena_destroy(scratch_arena);
  printf("  test_mesh_cooked_round_trip_and_malformed_boundaries PASSED\n");
}

typedef struct TestTriangleCentroid {
  float32_t x;
  float32_t y;
  float32_t z;
} TestTriangleCentroid;

static int test_centroid_compare(const void *lhs, const void *rhs) {
  const TestTriangleCentroid *a = lhs;
  const TestTriangleCentroid *b = rhs;
  if (a->x != b->x) {
    return a->x < b->x ? -1 : 1;
  }
  if (a->y != b->y) {
    return a->y < b->y ? -1 : 1;
  }
  if (a->z != b->z) {
    return a->z < b->z ? -1 : 1;
  }
  return 0;
}

static VkrPackedVec3
test_mesh_result_position(const VkrMeshLoaderResult *result, uint32_t index) {
  if (result->mesh_buffer.vertex_layout ==
      VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1) {
    VkrVertex3d unpacked = {0};
    const VkrPackedStaticVertex *vertices = result->mesh_buffer.vertices;
    vkr_packed_geometry_unpack(&vertices[index], 1u,
                               &result->mesh_buffer.decode, &unpacked);
    return unpacked.position;
  }
  const VkrVertex3d *vertices = result->mesh_buffer.vertices;
  return vertices[index].position;
}

static void
test_collect_triangle_centroids(const VkrMeshLoaderResult *result,
                                TestTriangleCentroid *out_centroids) {
  assert(result->mesh_buffer.vertex_layout ==
         VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1);
  const uint32_t *indices = result->mesh_buffer.indices;
  uint32_t triangle = 0;
  for (uint64_t range_index = 0; range_index < result->submeshes.length;
       ++range_index) {
    const VkrMeshLoaderSubmeshRange *range =
        &result->submeshes.data[range_index];
    for (uint32_t i = 0; i < range->index_count; i += 3u) {
      const VkrPackedVec3 a =
          test_mesh_result_position(result, indices[range->first_index + i]);
      const VkrPackedVec3 b = test_mesh_result_position(
          result, indices[range->first_index + i + 1u]);
      const VkrPackedVec3 c = test_mesh_result_position(
          result, indices[range->first_index + i + 2u]);
      out_centroids[triangle++] = (TestTriangleCentroid){
          .x = a.x + b.x + c.x,
          .y = a.y + b.y + c.y,
          .z = a.z + b.z + c.z,
      };
    }
  }
  qsort(out_centroids, triangle, sizeof(*out_centroids), test_centroid_compare);
}

static void test_mesh_source_optimization_is_mandatory(void) {
  printf("  Running test_mesh_source_optimization_is_mandatory...\n");
  static const char source_path[] = "build/vkr_mesh_runtime_opt.obj";
  static const char sidecar_path[] = "build/vkr_mesh_runtime_opt.vkb";
  static const char source[] = "o grid\n"
                               "v 0 0 0\nv 1 0 0\nv 2 0 0\n"
                               "v 0 1 0\nv 1 1 0\nv 2 1 0\n"
                               "v 0 2 0\nv 1 2 0\nv 2 2 0\n"
                               "vn 0 0 1\n"
                               "f 1//1 2//1 5//1\n"
                               "f 5//1 8//1 9//1\n"
                               "f 1//1 5//1 4//1\n"
                               "f 2//1 3//1 6//1\n"
                               "f 4//1 5//1 8//1\n"
                               "f 5//1 9//1 8//1\n"
                               "f 2//1 6//1 5//1\n"
                               "f 5//1 6//1 9//1\n";
  remove(sidecar_path);
  FILE *file = fopen(source_path, "wb");
  assert(file != NULL);
  assert(fwrite(source, 1u, sizeof(source) - 1u, file) == sizeof(source) - 1u);
  assert(fclose(file) == 0);

  Arena *scratch_arena = arena_create(MB(16), MB(2));
  assert(scratch_arena != NULL);
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&scratch));
  VkrArenaPool arena_pool = {0};
  assert(vkr_arena_pool_create(MB(2), 2, &scratch, &arena_pool));
  VkrGeometrySystem geometry = {0};
  VkrMeshLoaderContext context = {
      .geometry_system = &geometry,
      .arena_pool = &arena_pool,
  };
  VkrResourceLoader loader = vkr_mesh_loader_create(&context);
  VkrResourceHandleInfo first_handle = {0};
  VkrResourceHandleInfo second_handle = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(source_path), &scratch, &first_handle,
                     &error));
  error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(source_path), &scratch,
                     &second_handle, &error));
  assert(arena_pool.pool.allocated == 2u);

  const VkrMeshLoaderResult *first = first_handle.as.mesh;
  const VkrMeshLoaderResult *second = second_handle.as.mesh;
  assert(first->mesh_buffer.index_count == second->mesh_buffer.index_count);
  assert(first->submeshes.length == second->submeshes.length);
  assert(first->load_metrics.runtime_optimized);
  assert(second->load_metrics.runtime_optimized);
  assert(first->load_metrics.preparation == VKR_MESH_PREPARATION_SOURCE);
  assert(first->load_metrics.vertices_transformed_after <=
         first->load_metrics.vertices_transformed_before);
  assert(first->load_metrics.bytes_fetched_after <=
         first->load_metrics.bytes_fetched_before);
  assert((uint8_t *)first->mesh_buffer.vertices >=
         (uint8_t *)first->pool_chunk);
  assert((uint8_t *)first->mesh_buffer.vertices <
         (uint8_t *)first->pool_chunk + arena_pool.chunk_size);

  TestTriangleCentroid first_centroids[8] = {0};
  TestTriangleCentroid second_centroids[8] = {0};
  test_collect_triangle_centroids(first, first_centroids);
  test_collect_triangle_centroids(second, second_centroids);
  static const TestTriangleCentroid expected_centroids[8] = {
      {1.0f, 2.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, {2.0f, 4.0f, 0.0f},
      {4.0f, 2.0f, 0.0f}, {4.0f, 5.0f, 0.0f}, {4.0f, 5.0f, 0.0f},
      {5.0f, 1.0f, 0.0f}, {5.0f, 4.0f, 0.0f},
  };
  for (uint32_t i = 0; i < 8u; ++i) {
    assert(first_centroids[i].x == second_centroids[i].x);
    assert(first_centroids[i].y == second_centroids[i].y);
    assert(first_centroids[i].z == second_centroids[i].z);
    assert(fabsf(first_centroids[i].x - expected_centroids[i].x) < 1e-3f);
    assert(fabsf(first_centroids[i].y - expected_centroids[i].y) < 1e-3f);
    assert(fabsf(first_centroids[i].z - expected_centroids[i].z) < 1e-3f);
  }
  FilePath sidecar =
      file_path_create(sidecar_path, &scratch, FILE_PATH_TYPE_RELATIVE);
  assert(!file_exists(&sidecar));

  loader.unload(&loader, &first_handle, string8_lit(source_path));
  loader.unload(&loader, &second_handle, string8_lit(source_path));
  assert(arena_pool.pool.allocated == 0u);
  vkr_arena_pool_destroy(&scratch, &arena_pool);
  remove(source_path);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(scratch_arena);
  printf("  test_mesh_source_optimization_is_mandatory PASSED\n");
}

bool32_t run_mesh_cooked_tests(void) {
  printf("--- Starting Mesh Cooked Tests ---\n");
  test_packed_geometry_validation_contract();
  test_mesh_cooked_round_trip_and_malformed_boundaries();
  test_mesh_source_optimization_is_mandatory();
  printf("--- Mesh Cooked Tests Completed ---\n");
  return true_v;
}
