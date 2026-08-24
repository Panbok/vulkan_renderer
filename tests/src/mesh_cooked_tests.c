#include "mesh_cooked_tests.h"

#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/resources/loaders/vkr_mesh_cooked.h"

#include <assert.h>
#include <stdio.h>

#define TEST_HEADER_SIZE 192u
#define TEST_RANGE_SIZE 144u
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
    if (vertices[i].position.x == position.x &&
        vertices[i].position.y == position.y &&
        vertices[i].position.z == position.z) {
      return true_v;
    }
  }
  return false_v;
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
  };

  uint8_t *first = NULL;
  uint64_t first_size = 0;
  uint8_t *second = NULL;
  uint64_t second_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &first, &first_size));
  assert(vkr_mesh_cooked_encode(&scratch, &info, &second, &second_size));
  assert(first_size == second_size);
  assert(MemCompare(first, second, first_size) == 0);

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
  assert(vkr_mesh_cooked_decode(&result, &scratch, first, first_size, true_v,
                                &decoded));
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
  const VkrVertex3d *decoded_vertices = decoded.mesh_buffer.vertices;
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
  loader.unload(&loader, &handle, string8_lit(loader_artifact_path));
  vkr_arena_pool_destroy(&scratch, &arena_pool);
  remove(loader_artifact_path);

  uint8_t *mutated =
      vkr_allocator_alloc(&scratch, first_size, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  assert(mutated != NULL);

  MemCopy(mutated, first, first_size);
  test_write_le64(mutated + TEST_HEADER_SIZE + TEST_VERTEX_STREAM_OFFSET_FIELD,
                  first_size);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 false_v, &decoded));

  MemCopy(mutated, first, first_size);
  test_write_le64(mutated + TEST_HEADER_SIZE + 64u, sizeof(VkrVertex3d) - 1u);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 false_v, &decoded));

  MemCopy(mutated, first, first_size);
  mutated[TEST_HEADER_SIZE + 32u] ^= 0x01u;
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 false_v, &decoded));

  MemCopy(mutated, first, first_size);
  test_write_f32(mutated + TEST_HEADER_SIZE + TEST_CENTER_X_FIELD, 0.25f);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 false_v, &decoded));

  MemCopy(mutated, first, first_size);
  uint64_t vertex_stream_offset = test_read_le64(
      mutated + TEST_HEADER_SIZE + TEST_VERTEX_STREAM_OFFSET_FIELD);
  mutated[vertex_stream_offset + 1u] ^= 0x80u;
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 false_v, &decoded));

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
                                 false_v, &decoded));

  dependency = fopen(dependency_path, "wb");
  assert(dependency != NULL);
  assert(fwrite("changed\n", 1u, 8u, dependency) == 8u);
  assert(fclose(dependency) == 0);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, first, first_size, true_v,
                                 &decoded));

  remove(dependency_path);
  vkr_allocator_release_global_accounting(&result);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(result_arena);
  arena_destroy(scratch_arena);
  printf("  test_mesh_cooked_round_trip_and_malformed_boundaries PASSED\n");
}

bool32_t run_mesh_cooked_tests(void) {
  printf("--- Starting Mesh Cooked Tests ---\n");
  test_mesh_cooked_round_trip_and_malformed_boundaries();
  printf("--- Mesh Cooked Tests Completed ---\n");
  return true_v;
}
