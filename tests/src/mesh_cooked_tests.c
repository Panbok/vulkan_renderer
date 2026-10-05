#include "mesh_cooked_tests.h"
#include "core/vkr_hash.h"

#include "assets/vkr_mesh_cook_source.h"
#include "assets/vkr_mesh_encode.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "vkr_geometry_data.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#define TEST_HEADER_SIZE 272u
#define TEST_RANGE_SIZE 200u
#define TEST_STREAM_OFFSET_FIELD 80u
#define TEST_HEADER_CRC_FIELD 184u
#define TEST_METADATA_CRC_FIELD 188u
#define TEST_HEADER_DECODE_BIAS_X_FIELD 192u
#define TEST_VERTEX_STREAM_OFFSET_FIELD 48u
#define TEST_VERTEX_CRC_FIELD 72u
#define TEST_CENTER_X_FIELD 104u
#define TEST_DECODE_FLAGS_FIELD 180u

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
      vkr_crc32(data + TEST_HEADER_SIZE, stream_offset - TEST_HEADER_SIZE));
  test_write_le32(data + TEST_HEADER_CRC_FIELD, 0u);
  test_write_le32(data + TEST_HEADER_CRC_FIELD,
                  vkr_crc32(data, TEST_HEADER_SIZE));
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

  /* A lightmap UV set lives in word 7 only under the decode flag. */
  VkrPackedStaticVertex lightmapped[3];
  MemCopy(lightmapped, packed, sizeof(packed));
  VkrGpuGeometryDecodeRecord lightmap_decode = decode;
  const float32_t uv2[6] = {0.0f, 0.0f, 1.0f, 0.25f, 0.5f, 1.0f};
  assert(vkr_packed_geometry_set_lightmap_uv(lightmapped, 3u, uv2,
                                             &lightmap_decode));
  assert(lightmap_decode.flags == (VKR_GPU_GEOMETRY_DECODE_STATIC_V1 |
                                   VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV));
  assert(vkr_packed_geometry_decode_is_valid(&lightmap_decode));
  assert(vkr_packed_geometry_vertices_are_valid(lightmapped, 3u,
                                                &lightmap_decode));
  assert(!vkr_packed_geometry_vertices_are_valid(&lightmapped[1], 1u, &decode));
  const Vec2 decoded_uv2 = vkr_packed_geometry_lightmap_uv(&lightmapped[1]);
  assert(fabsf(decoded_uv2.x - 1.0f) < 1.0e-5f &&
         fabsf(decoded_uv2.y - 0.25f) < 1.0e-4f);
  const float32_t outside_uv2[2] = {0.5f, 1.5f};
  VkrGpuGeometryDecodeRecord outside_decode = decode;
  assert(!vkr_packed_geometry_set_lightmap_uv(lightmapped, 1u, outside_uv2,
                                              &outside_decode));
  assert(outside_decode.flags == VKR_GPU_GEOMETRY_DECODE_STATIC_V1);

  VkrGpuGeometryDecodeRecord invalid_decode = decode;
  invalid_decode.flags = VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV;
  assert(!vkr_packed_geometry_decode_is_valid(&invalid_decode));
  invalid_decode = decode;
  invalid_decode.lod_record = 1u;
  assert(!vkr_packed_geometry_decode_is_valid(&invalid_decode));
  invalid_decode = decode;
  invalid_decode.position_scale[0] = INFINITY;
  assert(!vkr_packed_geometry_decode_is_valid(&invalid_decode));

  /* Metadata: decode records, then the LOD rows they reference in order,
     four records each (ADR-084). */
  VkrGpuGeometryDecodeRecord records[1u + VKR_GPU_GEOMETRY_LOD_RECORDS];
  records[0] = decode;
  records[0].lod_record = 1u;
  VkrGpuGeometryLodRow row = {
      .level_count = 2u,
      .flags = VKR_GPU_GEOMETRY_LOD_TERRAIN_GRID,
      .levels = {{.first_index = 0u, .index_count = 3u, .error = 0.0f},
                 {.first_index = 3u, .index_count = 3u, .error = 0.5f}},
  };
  MemCopy(&records[1], &row, sizeof(row));
  uint32_t decode_count = 0u;
  assert(vkr_packed_geometry_metadata_is_valid(records, ArrayCount(records),
                                               &decode_count));
  assert(decode_count == 1u &&
         vkr_packed_geometry_lod_row(records, 0u)->level_count == 2u);
  const uint32_t indices[6] = {0u, 1u, 2u, 2u, 1u, 0u};
  assert(vkr_packed_geometry_lod_ranges_are_valid(records, 0u, 0u, 3u, 0,
                                                  indices, 6u, 3u));
  /* Level 0 must be the range, and later levels stay inside the buffer. */
  assert(!vkr_packed_geometry_lod_ranges_are_valid(records, 0u, 0u, 6u, 0,
                                                   indices, 6u, 3u));
  assert(!vkr_packed_geometry_lod_ranges_are_valid(records, 0u, 0u, 3u, 0,
                                                   indices, 5u, 3u));
  assert(!vkr_packed_geometry_lod_ranges_are_valid(records, 0u, 0u, 3u, 0,
                                                   indices, 6u, 2u));
  /* A row nobody references, one before its decode record, a shrinking
     error or too many levels are rejected. */
  assert(vkr_packed_geometry_metadata_is_valid(&decode, 1u, NULL));
  VkrGpuGeometryDecodeRecord broken[ArrayCount(records)];
  MemCopy(broken, records, sizeof(records));
  broken[0].lod_record = 0u;
  assert(
      !vkr_packed_geometry_metadata_is_valid(broken, ArrayCount(broken), NULL));
  MemCopy(broken, records, sizeof(records));
  broken[0].lod_record = 2u;
  assert(
      !vkr_packed_geometry_metadata_is_valid(broken, ArrayCount(broken), NULL));
  VkrGpuGeometryLodRow bad = row;
  bad.levels[1].error = -1.0f;
  MemCopy(&broken[1], &bad, sizeof(bad));
  broken[0].lod_record = 1u;
  assert(
      !vkr_packed_geometry_metadata_is_valid(broken, ArrayCount(broken), NULL));
  bad = row;
  bad.level_count = VKR_GPU_GEOMETRY_LOD_LEVEL_MAX + 1u;
  MemCopy(&broken[1], &bad, sizeof(bad));
  assert(
      !vkr_packed_geometry_metadata_is_valid(broken, ArrayCount(broken), NULL));

  vertices[0].normal = (VkrPackedVec3){2.0f, 0.0f, 0.0f};
  vertices[0].tangent = vec4_new(2.0f, 0.0f, 0.0f, 1.0f);
  assert(vkr_packed_geometry_pack(
      vertices, ArrayCount(vertices), vec3_new(0.0f, 0.0f, 0.0f),
      vec3_new(1.0f, 1.0f, 0.0f), &budgets, packed, &decode, &metrics));
  VkrVertex3d repaired = {0};
  vkr_packed_geometry_unpack(&packed[0], 1u, &decode, &repaired);
  const Vec3 repaired_tangent =
      vec3_new(repaired.tangent.x, repaired.tangent.y, repaired.tangent.z);
  assert(vec3_length_squared(repaired_tangent) > 0.99f);
  assert(fabsf(vec3_dot(vkr_vertex_unpack_vec3(repaired.normal),
                        repaired_tangent)) < 1.0e-3f);

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

static void test_tangent_generation_repairs_parallel_accumulation(void) {
  printf("  Running "
         "test_tangent_generation_repairs_parallel_accumulation...\n");
  Arena *arena = arena_create(KB(4), KB(4));
  assert(arena != NULL);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrVertex3d vertices[3] = {
      test_vertex(0.0f, 0.0f, 0.0f),
      test_vertex(1.0f, 0.0f, 0.0f),
      test_vertex(0.0f, 1.0f, 0.0f),
  };
  for (uint32_t i = 0u; i < ArrayCount(vertices); ++i) {
    vertices[i].normal = (VkrPackedVec3){1.0f, 0.0f, 0.0f};
  }
  const uint32_t indices[3] = {0u, 1u, 2u};
  vkr_geometry_generate_tangents(&allocator, vertices, ArrayCount(vertices),
                                 indices, ArrayCount(indices));
  for (uint32_t i = 0u; i < ArrayCount(vertices); ++i) {
    const Vec3 tangent = vec3_new(vertices[i].tangent.x, vertices[i].tangent.y,
                                  vertices[i].tangent.z);
    assert(isfinite(tangent.x) && isfinite(tangent.y) && isfinite(tangent.z));
    assert(fabsf(vec3_length_squared(tangent) - 1.0f) < 1.0e-5f);
    assert(fabsf(tangent.x) < 1.0e-5f);
    assert(fabsf(fabsf(vertices[i].tangent.w) - 1.0f) < 1.0e-5f);
  }
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  printf("  test_tangent_generation_repairs_parallel_accumulation PASSED\n");
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
  VkrGeometryUploadRange ranges[2] = {
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
      },
  };
  VkrMeshSourceNode source_nodes[3] = {
      {.name = string8_lit("group"),
       .local = mat4_identity(),
       .parent = UINT32_MAX,
       .mesh = UINT32_MAX,
       .mesh_variant = UINT32_MAX,
       .camera = UINT32_MAX,
       .skin = UINT32_MAX,
       .light = UINT32_MAX,
       .in_scene = true_v},
      {.name = string8_lit("shared one"),
       .local = mat4_identity(),
       .parent = 0,
       .mesh = 0,
       .mesh_variant = 0,
       .camera = UINT32_MAX,
       .skin = UINT32_MAX,
       .light = UINT32_MAX,
       .in_scene = true_v},
      {.name = string8_lit("shared two"),
       .local = mat4_identity(),
       .parent = 0,
       .mesh = 0,
       .mesh_variant = 0,
       .camera = 2,
       .skin = 3,
       .light = UINT32_MAX,
       .in_scene = true_v},
  };
  source_nodes[0].local.elements[12] = 10.0f;
  source_nodes[1].local.elements[12] = 2.0f;
  source_nodes[2].local.elements[12] = -2.0f;
  VkrMeshSourceMesh source_mesh = {
      .source_mesh_index = 0, .first_range = 0, .range_count = 2};
  String8 dependency_string = string8_lit(dependency_path);
  VkrMeshCookedEncodeInfo info = {
      .source = {.nodes = {.data = source_nodes, .length = 3},
                 .meshes = {.data = &source_mesh, .length = 1},
                 .fingerprint = UINT64_C(0x123456789abcdef0),
                 .animation_count = 2},
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
  assert(decoded.source.nodes.length == 3u &&
         decoded.source.meshes.length == 1u);
  assert(decoded.source.fingerprint == UINT64_C(0x123456789abcdef0));
  uint64_t identity_fingerprint = 0u;
  uint8_t identity_settings[32];
  assert(vkr_mesh_cooked_read_identity(first, first_size, &identity_fingerprint,
                                       identity_settings));
  assert(identity_fingerprint == decoded.source.fingerprint);
  assert(decoded.source.animation_count == 2u);
  assert(decoded.source.nodes.data[1].parent == 0u);
  assert(decoded.source.nodes.data[2].mesh_variant == 0u);
  assert(decoded.source.nodes.data[2].camera == 2u &&
         decoded.source.nodes.data[2].skin == 3u);
  assert(decoded.source.nodes.data[0].local.elements[12] == 10.0f);
  assert(decoded.source.nodes.data[2].local.elements[12] == -2.0f);
  assert(string8_equals(&decoded.source.nodes.data[1].name,
                        &source_nodes[1].name));
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
  assert(decoded.mesh_buffer.decode_count == 2u);
  assert(decoded.mesh_buffer.decodes != NULL);
  assert(decoded.ranges.data[0].decode_index == 0u);
  assert(decoded.ranges.data[1].decode_index == 1u);
  assert(decoded.mesh_buffer.decodes[0].position_bias[0] !=
         decoded.mesh_buffer.decodes[1].position_bias[0]);
  const uint32_t *decoded_indices = decoded.mesh_buffer.indices;
  for (uint32_t range_index = 0; range_index < 2u; ++range_index) {
    const VkrGeometryUploadRange *range = &decoded.ranges.data[range_index];
    for (uint32_t i = 0; i < range->index_count; ++i) {
      const uint32_t vertex_index = decoded_indices[range->first_index + i];
      assert(vertex_index < decoded.mesh_buffer.vertex_count);
      VkrVertex3d unpacked = {0};
      vkr_packed_geometry_unpack(
          (const VkrPackedStaticVertex *)decoded.mesh_buffer.vertices +
              vertex_index,
          1u, &decoded.mesh_buffer.decodes[range->decode_index], &unpacked);
      assert(test_contains_position(vertices + range_index * 3u, 3u,
                                    unpacked.position));
    }
  }

  static const char loader_artifact_path[] = "build/vkr_mesh_cooked_loader.vkb";
  VkrGeometryUploadRange loader_range = ranges[0];
  loader_range.material_name = (String8){0};
  loader_range.shader_override = (String8){0};
  VkrMeshCookedEncodeInfo loader_info = info;
  loader_info.mesh_buffer.vertex_count = 3;
  loader_info.mesh_buffer.index_count = 3;
  loader_info.ranges = &loader_range;
  loader_info.range_count = 1;
  uint8_t *loader_artifact = NULL;
  uint64_t loader_artifact_size = 0;
  // Source spans must describe the exact geometry range table being cooked.
  assert(!vkr_mesh_cooked_encode(&scratch, &loader_info, &loader_artifact,
                                 &loader_artifact_size));
  VkrMeshSourceMesh loader_source_mesh = source_mesh;
  loader_source_mesh.range_count = 1u;
  loader_info.source.meshes.data = &loader_source_mesh;
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
         3u * sizeof(VkrPackedStaticVertex) + 3u * sizeof(uint32_t) +
             sizeof(VkrGpuGeometryDecodeRecord));
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
  assert(!vkr_mesh_cooked_read_identity(
      mutated, first_size, &identity_fingerprint, identity_settings));

  MemCopy(mutated, first, first_size);
  test_write_f32(mutated + TEST_HEADER_DECODE_BIAS_X_FIELD, 0.25f);
  test_refresh_integrity(mutated);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, first_size,
                                 &decoded));

  MemCopy(mutated, first, first_size);
  test_write_le32(mutated + TEST_HEADER_SIZE + TEST_RANGE_SIZE +
                      TEST_DECODE_FLAGS_FIELD,
                  0u);
  test_refresh_integrity(mutated);
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
      vkr_crc32(mutated + vertex_stream_offset, vertex_stream_size));
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
test_mesh_result_position(const VkrMeshLoaderResult *result, uint32_t index,
                          uint32_t decode_index) {
  if (result->mesh_buffer.vertex_layout ==
      VKR_GPU_VERTEX_LAYOUT_STATIC_PACKED_V1) {
    VkrVertex3d unpacked = {0};
    const VkrPackedStaticVertex *vertices = result->mesh_buffer.vertices;
    vkr_packed_geometry_unpack(&vertices[index], 1u,
                               &result->mesh_buffer.decodes[decode_index],
                               &unpacked);
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
    const VkrGeometryUploadRange *range = &result->submeshes.data[range_index];
    for (uint32_t i = 0; i < range->index_count; i += 3u) {
      const VkrPackedVec3 a = test_mesh_result_position(
          result, indices[range->first_index + i], range->decode_index);
      const VkrPackedVec3 b = test_mesh_result_position(
          result, indices[range->first_index + i + 1u], range->decode_index);
      const VkrPackedVec3 c = test_mesh_result_position(
          result, indices[range->first_index + i + 2u], range->decode_index);
      out_centroids[triangle++] = (TestTriangleCentroid){
          .x = a.x + b.x + c.x,
          .y = a.y + b.y + c.y,
          .z = a.z + b.z + c.z,
      };
    }
  }
  qsort(out_centroids, triangle, sizeof(*out_centroids), test_centroid_compare);
}

static void test_cooked_optimization_preserves_triangles(void) {
  printf("  Running test_cooked_optimization_preserves_triangles...\n");
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
  Arena *source_arena = arena_create(MB(16), MB(2));
  assert(scratch_arena != NULL && source_arena != NULL);
  VkrAllocator source_allocator = {.ctx = source_arena};
  assert(vkr_allocator_arena(&source_allocator));
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
  VkrMeshCookStats stats = {0};
  assert(vkr_mesh_cook_source(string8_lit(source_path),
                              string8_lit(sidecar_path), &source_allocator,
                              &scratch, &stats, &error));
  assert(!loader.load(&loader, string8_lit(source_path), &scratch,
                      &first_handle, &error));
  assert(arena_pool.pool.allocated == 0u);
  error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(sidecar_path), &scratch,
                     &first_handle, &error));
  error = VKR_RENDERER_ERROR_NONE;
  assert(loader.load(&loader, string8_lit(sidecar_path), &scratch,
                     &second_handle, &error));
  assert(arena_pool.pool.allocated == 2u);

  const VkrMeshLoaderResult *first = first_handle.as.mesh;
  const VkrMeshLoaderResult *second = second_handle.as.mesh;
  assert(first->mesh_buffer.index_count == second->mesh_buffer.index_count);
  assert(first->submeshes.length == second->submeshes.length);
  assert(!first->load_metrics.runtime_optimized);
  assert(!second->load_metrics.runtime_optimized);
  assert(first->load_metrics.preparation == VKR_MESH_PREPARATION_COOKED);
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
  assert(file_exists(&sidecar));

  loader.unload(&loader, &first_handle, string8_lit(sidecar_path));
  loader.unload(&loader, &second_handle, string8_lit(sidecar_path));
  assert(arena_pool.pool.allocated == 0u);
  vkr_arena_pool_destroy(&scratch, &arena_pool);
  remove(source_path);
  remove(sidecar_path);
  vkr_allocator_release_global_accounting(&source_allocator);
  arena_destroy(source_arena);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(scratch_arena);
  printf("  test_cooked_optimization_preserves_triangles PASSED\n");
}

/* An OBJ's faces append to per-material builders held in arenas, which keep a
   reallocation's old storage. Reserving each face exactly made that storage
   quadratic: 16384 triangles need about 26 GB that way and a few MiB with
   geometric growth, so the cook must fit 64 MiB arenas. */
static void test_obj_face_storage_grows_geometrically(void) {
  printf("  Running test_obj_face_storage_grows_geometrically...\n");
  static const char source_path[] = "build/vkr_mesh_face_growth.obj";
  static const char cooked_path[] = "build/vkr_mesh_face_growth.vkb";
  enum { columns = 128, rows = 64 };
  FILE *file = fopen(source_path, "wb");
  assert(file != NULL);
  fprintf(file, "o grid\nvn 0 0 1\n");
  for (uint32_t y = 0; y <= rows; ++y) {
    for (uint32_t x = 0; x <= columns; ++x) {
      fprintf(file, "v %u %u 0\n", x, y);
    }
  }
  for (uint32_t y = 0; y < rows; ++y) {
    for (uint32_t x = 0; x < columns; ++x) {
      const uint32_t corner = y * (columns + 1u) + x + 1u;
      const uint32_t above = corner + columns + 1u;
      fprintf(file, "f %u//1 %u//1 %u//1\n", corner, corner + 1u, above + 1u);
      fprintf(file, "f %u//1 %u//1 %u//1\n", corner, above + 1u, above);
    }
  }
  assert(fclose(file) == 0);

  Arena *source_arena = arena_create(MB(64), MB(2));
  Arena *scratch_arena = arena_create(MB(64), MB(2));
  assert(source_arena != NULL && scratch_arena != NULL);
  VkrAllocator source = {.ctx = source_arena};
  assert(vkr_allocator_arena(&source));
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&scratch));
  VkrMeshCookStats stats = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_mesh_cook_source(string8_lit(source_path),
                              string8_lit(cooked_path), &source, &scratch,
                              &stats, &error));
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(stats.index_count == columns * rows * 6u);
  assert(stats.vertex_count == (columns + 1u) * (rows + 1u));

  assert(remove(source_path) == 0);
  assert(remove(cooked_path) == 0);
  vkr_allocator_release_global_accounting(&source);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(source_arena);
  arena_destroy(scratch_arena);
  printf("  test_obj_face_storage_grows_geometrically PASSED\n");
}

static void test_metadata_only_gltf_cooked_load_without_source(void) {
  static const char source_path[] = "build/vkr_metadata_nodes.gltf";
  static const char cooked_path[] = "build/vkr_metadata_nodes.vkb";
  static const char json[] =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}"
      "],"
      "\"cameras\":[{\"type\":\"perspective\",\"perspective\":{\"yfov\":1,"
      "\"znear\":0.1}}],"
      "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":"
      "\"point\",\"intensity\":3}]}},"
      "\"nodes\":[{\"name\":\"Group\",\"translation\":[2,0,0],\"children\":[1,"
      "2]},"
      "{\"name\":\"Camera\",\"camera\":0},"
      "{\"name\":\"Lamp\",\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}"
      "}}]}";
  FILE *file = fopen(source_path, "wb");
  assert(file &&
         fwrite(json, 1u, sizeof(json) - 1u, file) == sizeof(json) - 1u);
  assert(fclose(file) == 0);
  Arena *scratch_arena = arena_create(MB(16), MB(2));
  Arena *source_arena = arena_create(MB(16), MB(2));
  assert(scratch_arena && source_arena);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator source = {.ctx = source_arena};
  assert(vkr_allocator_arena(&scratch) && vkr_allocator_arena(&source));
  VkrArenaPool pool = {0};
  assert(vkr_arena_pool_create(MB(1), 1, &scratch, &pool));
  VkrGeometrySystem geometry = {0};
  VkrMeshLoaderContext context = {.arena_pool = &pool,
                                  .geometry_system = &geometry};
  VkrResourceLoader loader = vkr_mesh_loader_create(&context);
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrMeshCookStats stats = {0};
  assert(vkr_mesh_cook_source(string8_lit(source_path),
                              string8_lit(cooked_path), &source, &scratch,
                              &stats, &error));
  assert(stats.range_count == 0 && stats.vertex_count == 0 &&
         stats.decoded_bytes == 0);
  for (uint32_t pass = 0; pass < 2; ++pass) {
    const String8 path = string8_lit(cooked_path);
    VkrResourceHandleInfo handle = {0};
    assert(loader.load(&loader, path, &scratch, &handle, &error));
    const VkrMeshLoaderResult *mesh = handle.as.mesh;
    assert(mesh && !mesh->has_mesh_buffer && mesh->submeshes.length == 0);
    assert(mesh->load_metrics.upload_bytes == 0);
    assert(mesh->source.nodes.length == 3 && mesh->source.meshes.length == 0);
    assert(mesh->source.nodes.data[0].local.elements[12] == 2.0f);
    assert(mesh->source.nodes.data[1].parent == 0 &&
           mesh->source.nodes.data[1].camera == 0);
    assert(mesh->source.nodes.data[2].punctual.kind == 2 &&
           mesh->source.nodes.data[2].punctual.intensity == 3.0f);
    assert(mesh->source.nodes.data[2].mesh == UINT32_MAX);
    loader.unload(&loader, &handle, path);
    assert(pool.pool.allocated == 0);
    if (!pass)
      assert(remove(source_path) == 0);
  }
  assert(remove(cooked_path) == 0);
  vkr_arena_pool_destroy(&scratch, &pool);
  vkr_allocator_release_global_accounting(&source);
  vkr_allocator_release_global_accounting(&scratch);
  arena_destroy(source_arena);
  arena_destroy(scratch_arena);
  printf("  test_metadata_only_gltf_cooked_load_without_source PASSED\n");
}

#define TEST_HEADER_FLAGS_FIELD 108u

static float32_t test_uv_edge(Vec2 a, Vec2 b, Vec2 p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

/* A unit cube split into two source meshes of three faces, one range each.
 * The encode info points into the record, which the caller keeps alive. */
typedef struct TestLightmapCube {
  VkrVertex3d vertices[24];
  uint32_t indices[36];
  VkrGeometryUploadRange ranges[2];
  VkrMeshSourceMesh source_meshes[2];
  VkrMeshSourceNode nodes[2];
  VkrMeshCookedEncodeInfo info;
} TestLightmapCube;

static void test_lightmap_cube_build(TestLightmapCube *cube,
                                     String8 dependency_string) {
  const float32_t normals[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
  for (uint32_t face = 0u; face < 6u; ++face) {
    const float32_t *n = normals[face];
    const float32_t u[3] = {n[1], n[2], n[0]};
    const float32_t v[3] = {n[2], n[0], n[1]};
    for (uint32_t corner = 0u; corner < 4u; ++corner) {
      const float32_t su = (corner == 1u || corner == 2u) ? 0.5f : -0.5f;
      const float32_t sv = corner >= 2u ? 0.5f : -0.5f;
      VkrVertex3d *vertex = &cube->vertices[face * 4u + corner];
      *vertex = test_vertex(0.5f * n[0] + su * u[0] + sv * v[0],
                            0.5f * n[1] + su * u[1] + sv * v[1],
                            0.5f * n[2] + su * u[2] + sv * v[2]);
      vertex->normal = (VkrPackedVec3){n[0], n[1], n[2]};
      vertex->tangent = (Vec4){u[0], u[1], u[2], 1.0f};
    }
    const uint32_t base = face * 4u;
    const uint32_t face_indices[6] = {base, base + 1u, base + 2u,
                                      base, base + 2u, base + 3u};
    MemCopy(&cube->indices[face * 6u], face_indices, sizeof(face_indices));
  }
  for (uint32_t i = 0u; i < 2u; ++i) {
    cube->ranges[i] = (VkrGeometryUploadRange){
        .range_id = i,
        .first_index = 18u * i,
        .index_count = 18,
        .min_extents = {-0.5f, -0.5f, -0.5f},
        .max_extents = {0.5f, 0.5f, 0.5f},
        .material_name = i == 0u ? string8_lit("material.first")
                                 : string8_lit("material.second"),
        .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD};
    cube->source_meshes[i] = (VkrMeshSourceMesh){
        .source_mesh_index = i, .first_range = i, .range_count = 1};
    cube->nodes[i] = (VkrMeshSourceNode){.name = string8_lit("half"),
                                         .local = mat4_identity(),
                                         .parent = UINT32_MAX,
                                         .mesh = i,
                                         .mesh_variant = i,
                                         .camera = UINT32_MAX,
                                         .skin = UINT32_MAX,
                                         .light = UINT32_MAX,
                                         .in_scene = true_v};
  }
  cube->info = (VkrMeshCookedEncodeInfo){
      .source = {.nodes = {.data = cube->nodes, .length = 2},
                 .meshes = {.data = cube->source_meshes, .length = 2}},
      .source_path = dependency_string,
      .dependency_paths = &cube->info.source_path,
      .dependency_count = 1,
      .mesh_buffer = {.vertex_size = sizeof(VkrVertex3d),
                      .vertex_count = 24,
                      .vertices = cube->vertices,
                      .index_size = sizeof(uint32_t),
                      .index_count = 36,
                      .indices = cube->indices},
      .ranges = cube->ranges,
      .range_count = 2,
      .budgets = vkr_packed_geometry_default_budgets(),
  };
  cube->info.dependency_paths = &cube->info.source_path;
}

/* A unit cube split into two source meshes of three faces, one range each,
 * cooked with a lightmap UV set: the artifact carries the header flag, one
 * atlas per source mesh and flagged ranges; every source triangle survives;
 * within each mesh's atlas lightmap triangles keep area and never share a
 * texel; a small size limit lowers the density; the header flag must agree
 * with the ranges. */
static void test_mesh_cooked_lightmap_uv_round_trip(void) {
  printf("  Running test_mesh_cooked_lightmap_uv_round_trip...\n");
  static const char dependency_path[] = "build/vkr_mesh_cooked_lightmap.bin";
  FILE *dependency = fopen(dependency_path, "wb");
  assert(dependency != NULL);
  assert(fwrite("lightmap\n", 1u, 9u, dependency) == 9u);
  assert(fclose(dependency) == 0);

  Arena *scratch_arena = arena_create(MB(64), MB(4));
  Arena *result_arena = arena_create(MB(8), MB(1));
  assert(scratch_arena != NULL && result_arena != NULL);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch));
  assert(vkr_allocator_arena(&result));

  static TestLightmapCube cube;
  test_lightmap_cube_build(&cube, string8_lit(dependency_path));
  VkrMeshCookedEncodeInfo info = cube.info;
  uint8_t *plain = NULL;
  uint64_t plain_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &plain, &plain_size));
  assert(test_read_le32(plain + TEST_HEADER_FLAGS_FIELD) == 0u);

  info.lightmap = (VkrMeshCookedLightmapOptions){
      .texels_per_unit = 32.0f, .padding = 2u, .max_size = 4096u};
  uint8_t *cooked = NULL;
  uint64_t cooked_size = 0;
  uint8_t *again = NULL;
  uint64_t again_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &cooked, &cooked_size));
  assert(vkr_mesh_cooked_encode(&scratch, &info, &again, &again_size));
  assert(cooked_size == again_size &&
         MemCompare(cooked, again, cooked_size) == 0);
  assert(test_read_le32(cooked + TEST_HEADER_FLAGS_FIELD) ==
         VKR_MESH_COOKED_FLAG_LIGHTMAP_UV);

  VkrMeshCookedDecoded decoded = {0};
  assert(
      vkr_mesh_cooked_decode(&result, &scratch, cooked, cooked_size, &decoded));
  const VkrMeshCookedLightmap *lightmap = &decoded.lightmap;
  assert(lightmap->atlas_count == 2u && lightmap->padding == 2u);
  assert(decoded.ranges.length == 2u);

  /* Each source face's two triangles come back, and in its source mesh's
     atlas every triangle has area and no texel center lies inside two
     triangles. Source mesh r owns range r. */
  const VkrPackedStaticVertex *packed = decoded.mesh_buffer.vertices;
  const uint32_t *decoded_indices = decoded.mesh_buffer.indices;
  uint32_t triangles = 0u;
  for (uint32_t r = 0u; r < 2u; ++r) {
    const VkrMeshCookedLightmapAtlas *atlas = &lightmap->atlases[r];
    assert(atlas->width > 0u && atlas->width % 4u == 0u);
    assert(atlas->height > 0u && atlas->height % 4u == 0u);
    assert(atlas->texels_per_unit == 32.0f);
    uint8_t *coverage =
        calloc((size_t)atlas->width * atlas->height, sizeof(uint8_t));
    assert(coverage != NULL);
    const VkrGeometryUploadRange *range = &decoded.ranges.data[r];
    const VkrGpuGeometryDecodeRecord *decode =
        &decoded.mesh_buffer.decodes[range->decode_index];
    assert((decode->flags & VKR_GPU_GEOMETRY_DECODE_LIGHTMAP_UV) != 0u);
    for (uint32_t t = 0u; t < range->index_count; t += 3u) {
      Vec2 uv[3];
      VkrPackedVec3 centroid = {0};
      for (uint32_t c = 0u; c < 3u; ++c) {
        const uint32_t index = decoded_indices[range->first_index + t + c];
        assert(
            vkr_packed_geometry_vertices_are_valid(&packed[index], 1u, decode));
        VkrVertex3d unpacked = {0};
        vkr_packed_geometry_unpack(&packed[index], 1u, decode, &unpacked);
        centroid.x += unpacked.position.x / 3.0f;
        centroid.y += unpacked.position.y / 3.0f;
        centroid.z += unpacked.position.z / 3.0f;
        uv[c] = vkr_packed_geometry_lightmap_uv(&packed[index]);
        uv[c].x *= (float32_t)atlas->width;
        uv[c].y *= (float32_t)atlas->height;
      }
      /* A face's triangle centroids lie at a third of the way across it. */
      const float32_t extent =
          fmaxf(fabsf(centroid.x), fmaxf(fabsf(centroid.y), fabsf(centroid.z)));
      assert(fabsf(extent - 0.5f) < 1.0e-3f);
      const float32_t area = test_uv_edge(uv[0], uv[1], uv[2]);
      assert(fabsf(area) > 1.0f);
      const float32_t sign = area > 0.0f ? 1.0f : -1.0f;
      for (uint32_t y = 0u; y < atlas->height; ++y) {
        for (uint32_t x = 0u; x < atlas->width; ++x) {
          const Vec2 p = vec2_new((float32_t)x + 0.5f, (float32_t)y + 0.5f);
          if (sign * test_uv_edge(uv[0], uv[1], p) > 1.0e-3f &&
              sign * test_uv_edge(uv[1], uv[2], p) > 1.0e-3f &&
              sign * test_uv_edge(uv[2], uv[0], p) > 1.0e-3f) {
            uint8_t *cell = &coverage[(size_t)y * atlas->width + x];
            assert(*cell == 0u);
            *cell = 1u;
          }
        }
      }
      ++triangles;
    }
    free(coverage);
  }
  assert(triangles == 12u);

  /* A size limit below the requested density's atlas halves the density. */
  VkrMeshCookedEncodeInfo small_info = info;
  small_info.lightmap.max_size = 64u;
  uint8_t *small = NULL;
  uint64_t small_size = 0;
  assert(vkr_mesh_cooked_encode(&scratch, &small_info, &small, &small_size));
  VkrMeshCookedDecoded small_decoded = {0};
  assert(vkr_mesh_cooked_decode(&result, &scratch, small, small_size,
                                &small_decoded));
  for (uint32_t i = 0u; i < 2u; ++i) {
    const VkrMeshCookedLightmapAtlas *atlas =
        &small_decoded.lightmap.atlases[i];
    assert(atlas->texels_per_unit < 32.0f && atlas->texels_per_unit > 0.0f);
    assert(atlas->width <= 64u && atlas->height <= 64u);
  }

  /* A source-metadata patch keeps the lightmap block. */
  VkrMeshSource patched_source = decoded.source;
  VkrMeshSourceNode patched_nodes[2] = {decoded.source.nodes.data[0],
                                        decoded.source.nodes.data[1]};
  patched_nodes[0].local.elements[12] = 3.0f;
  patched_source.nodes.data = patched_nodes;
  uint8_t *variant = NULL;
  assert(vkr_mesh_cooked_source_variant(&scratch, cooked, cooked_size,
                                        &patched_source, &variant));
  VkrMeshCookedDecoded variant_decoded = {0};
  assert(vkr_mesh_cooked_decode(&result, &scratch, variant, cooked_size,
                                &variant_decoded));
  assert(variant_decoded.source.nodes.data[0].local.elements[12] == 3.0f);
  assert(variant_decoded.lightmap.atlas_count == lightmap->atlas_count &&
         variant_decoded.lightmap.padding == lightmap->padding);
  assert(MemCompare(variant_decoded.lightmap.atlases, lightmap->atlases,
                    lightmap->atlas_count * sizeof(*lightmap->atlases)) == 0);

  /* The header flag must agree with the ranges' decode records. */
  uint8_t *mutated = vkr_allocator_alloc(&scratch, cooked_size,
                                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  assert(mutated != NULL);
  MemCopy(mutated, cooked, cooked_size);
  test_write_le32(mutated + TEST_HEADER_FLAGS_FIELD, 0u);
  test_refresh_integrity(mutated);
  VkrMeshCookedDecoded rejected = {0};
  assert(!vkr_mesh_cooked_decode(&result, &scratch, mutated, cooked_size,
                                 &rejected));
  uint8_t *flagged = vkr_allocator_alloc(&scratch, plain_size,
                                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  assert(flagged != NULL);
  MemCopy(flagged, plain, plain_size);
  test_write_le32(flagged + TEST_HEADER_FLAGS_FIELD,
                  VKR_MESH_COOKED_FLAG_LIGHTMAP_UV);
  test_refresh_integrity(flagged);
  assert(!vkr_mesh_cooked_decode(&result, &scratch, flagged, plain_size,
                                 &rejected));

  arena_destroy(result_arena);
  arena_destroy(scratch_arena);
  printf("  test_mesh_cooked_lightmap_uv_round_trip PASSED\n");
}

uint8_t *test_cook_lightmap_cube(float32_t texels_per_unit,
                                 uint64_t *out_size) {
  static const char dependency_path[] = "build/vkr_mesh_cooked_lightmap.bin";
  FILE *dependency = fopen(dependency_path, "wb");
  assert(dependency != NULL);
  assert(fwrite("lightmap\n", 1u, 9u, dependency) == 9u);
  assert(fclose(dependency) == 0);
  Arena *scratch_arena = arena_create(MB(64), MB(4));
  assert(scratch_arena != NULL);
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&scratch));
  static TestLightmapCube cube;
  test_lightmap_cube_build(&cube, string8_lit(dependency_path));
  cube.info.lightmap = (VkrMeshCookedLightmapOptions){
      .texels_per_unit = texels_per_unit, .padding = 2u, .max_size = 4096u};
  uint8_t *cooked = NULL;
  assert(vkr_mesh_cooked_encode(&scratch, &cube.info, &cooked, out_size));
  uint8_t *copy = malloc(*out_size);
  assert(copy != NULL);
  MemCopy(copy, cooked, *out_size);
  arena_destroy(scratch_arena);
  return copy;
}

bool32_t run_mesh_cooked_tests(void) {
  printf("--- Starting Mesh Cooked Tests ---\n");
  test_packed_geometry_validation_contract();
  test_tangent_generation_repairs_parallel_accumulation();
  test_mesh_cooked_round_trip_and_malformed_boundaries();
  test_mesh_cooked_lightmap_uv_round_trip();
  test_cooked_optimization_preserves_triangles();
  test_obj_face_storage_grows_geometrically();
  test_metadata_only_gltf_cooked_load_without_source();
  printf("--- Mesh Cooked Tests Completed ---\n");
  return true_v;
}
