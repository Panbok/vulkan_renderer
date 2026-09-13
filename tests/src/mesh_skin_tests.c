#include "mesh_skin_tests.h"

#include "assets/vkr_animation_import.h"
#include "assets/vkr_mesh_cook_source.h"
#include "assets/vkr_mesh_deduplicate.h"
#include "assets/vkr_mesh_encode.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* These offsets describe the wire format, independently of codec internals. */
static uint32_t skin_test_read_u32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8u) |
         ((uint32_t)bytes[2] << 16u) | ((uint32_t)bytes[3] << 24u);
}

static uint64_t skin_test_read_u64(const uint8_t *bytes) {
  return skin_test_read_u32(bytes) |
         ((uint64_t)skin_test_read_u32(bytes + 4u) << 32u);
}

static void skin_test_write_u32(uint8_t *bytes, uint32_t value) {
  for (uint32_t i = 0u; i < 4u; ++i) {
    bytes[i] = (uint8_t)(value >> (i * 8u));
  }
}

static uint32_t skin_test_crc(const uint8_t *bytes, uint64_t size) {
  uint32_t crc = UINT32_MAX;
  for (uint64_t i = 0u; i < size; ++i) {
    crc ^= bytes[i];
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static void skin_test_refresh_integrity(uint8_t *bytes) {
  uint64_t directory = skin_test_read_u64(bytes + 48u);
  uint64_t streams = skin_test_read_u64(bytes + 80u);
  skin_test_write_u32(bytes + 188u,
                      skin_test_crc(bytes + directory, streams - directory));
  skin_test_write_u32(bytes + 184u, 0u);
  skin_test_write_u32(bytes + 184u, skin_test_crc(bytes, 272u));
}

static VkrVertex3d skin_test_vertex(uint32_t corner) {
  return (VkrVertex3d){
      .position = {corner == 1u ? 1.0f : 0.0f, corner == 2u ? 1.0f : 0.0f,
                   0.0f},
      .normal = {0.0f, 0.0f, 1.0f},
      .colour = {1.0f, 1.0f, 1.0f, 1.0f},
      .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
  };
}

static VkrMeshSourceNode skin_test_node(uint32_t skin) {
  return (VkrMeshSourceNode){.local = mat4_identity(),
                             .parent = UINT32_MAX,
                             .mesh = 0u,
                             .mesh_variant = 0u,
                             .camera = UINT32_MAX,
                             .skin = skin,
                             .light = UINT32_MAX,
                             .in_scene = true_v};
}

/* Joint 0 translates x by 10, joint 1 by 20. Two otherwise identical
 * triangles must therefore have centroid x=10+1/3 and 20+1/3. This oracle
 * detects detached influence streams regardless of optimizer vertex order. */
static void skin_test_triangles(const VkrMeshCookedDecoded *decoded) {
  assert(decoded->skin.vertex_count == decoded->mesh_buffer.vertex_count);
  assert(decoded->mesh_buffer.index_count == 6u);
  const uint32_t *indices = decoded->mesh_buffer.indices;
  uint32_t seen = 0u;
  for (uint32_t triangle = 0u; triangle < 2u; ++triangle) {
    float32_t sum_x = 0.0f;
    float32_t sum_y = 0.0f;
    for (uint32_t corner = 0u; corner < 3u; ++corner) {
      uint32_t vertex = indices[triangle * 3u + corner];
      VkrVertex3d unpacked = {0};
      vkr_packed_geometry_unpack(
          (const VkrPackedStaticVertex *)decoded->mesh_buffer.vertices + vertex,
          1u,
          &decoded->mesh_buffer.decodes[decoded->ranges.data[0].decode_index],
          &unpacked);
      const VkrMeshSkinVertex *influence = &decoded->skin.vertices[vertex];
      assert(influence->weights[0] == 1.0f);
      assert(influence->joints[0] < 2u);
      sum_x += unpacked.position.x + 10.0f * (1u + influence->joints[0]);
      sum_y += unpacked.position.y;
    }
    assert(fabsf(sum_y - 1.0f) < 1.0e-4f);
    if (fabsf(sum_x - 31.0f) < 1.0e-4f) {
      assert(!(seen & 1u));
      seen |= 1u;
    } else {
      assert(fabsf(sum_x - 61.0f) < 1.0e-4f);
      assert(!(seen & 2u));
      seen |= 2u;
    }
  }
  assert(seen == 3u);
}

static void skin_test_write(const char *path, const void *data, size_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(data, 1u, size, file) == size);
  assert(fclose(file) == 0);
}

static void skin_test_dedup_codec(void) {
  printf("  Running skin deduplication, codec and binding tests...\n");
  Arena *scratch_arena = arena_create(MB(32), MB(1));
  Arena *result_arena = arena_create(MB(16), MB(1));
  assert(scratch_arena && result_arena);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch));
  assert(vkr_allocator_arena(&result));
  VkrVertex3d vertices[7];
  VkrMeshSkinVertex influences[7] = {0};
  for (uint32_t i = 0u; i < 7u; ++i) {
    vertices[i] = skin_test_vertex(i % 3u);
    influences[i].joints[0] = (i >= 3u && i < 6u) ? 1u : 0u;
    influences[i].weights[0] = 1.0f;
  }
  uint32_t indices[] = {6u, 1u, 2u, 3u, 4u, 5u};
  VkrVertex3d *unique = NULL;
  VkrMeshSkinVertex *unique_skin = NULL;
  uint32_t unique_count = 0u;
  assert(vkr_mesh_cook_deduplicate_vertices(
      &scratch, vertices, influences, ArrayCount(vertices), indices,
      ArrayCount(indices), &unique, &unique_skin, &unique_count));
  assert(unique_count == 6u);
  assert(indices[0] != indices[3]);
  VkrMeshSourceNode nodes[] = {skin_test_node(0u), skin_test_node(1u)};
  VkrMeshSourceMesh mesh = {.source_mesh_index = 0u, .range_count = 1u};
  uint32_t joint_counts[] = {2u, 2u};
  VkrMeshSource source = {
      .nodes = {.data = nodes, .length = ArrayCount(nodes)},
      .meshes = {.data = &mesh, .length = 1u},
      .fingerprint = 123u,
  };
  VkrGeometryUploadRange range = {
      .range_id = 0u,
      .index_count = 6u,
      .center = {0.5f, 0.5f, 0.0f},
      .min_extents = {0.0f, 0.0f, 0.0f},
      .max_extents = {1.0f, 1.0f, 0.0f},
      .pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD,
  };
  VkrMeshSkinData skin = {.animation_fingerprint = 456u,
                          .skin_count = 2u,
                          .joint_counts = joint_counts,
                          .vertex_count = unique_count,
                          .vertices = unique_skin};
  assert(vkr_mesh_skin_validate(&skin, &source, &range, 1u, indices, 6u,
                                unique_count));
  joint_counts[1] = 1u;
  assert(!vkr_mesh_skin_validate(&skin, &source, &range, 1u, indices, 6u,
                                 unique_count));
  joint_counts[1] = 2u;
  const float32_t invalid_weights[] = {-1.0f, 0.0f, 0.5f, NAN, INFINITY};
  for (uint32_t i = 0u; i < ArrayCount(invalid_weights); ++i) {
    unique_skin[0].weights[0] = invalid_weights[i];
    assert(!vkr_mesh_skin_validate(&skin, &source, &range, 1u, indices, 6u,
                                   unique_count));
  }
  unique_skin[0].weights[0] = 1.0f;
  nodes[1].skin = 2u;
  assert(!vkr_mesh_skin_validate(&skin, &source, &range, 1u, indices, 6u,
                                 unique_count));
  nodes[1].skin = 1u;

  static const char dependency_path[] = "build/vkr_skin_dependency.bin";
  skin_test_write(dependency_path, "skin", 4u);
  String8 dependency = string8_lit(dependency_path);
  VkrMeshCookedEncodeInfo info = {
      .source_path = dependency,
      .dependency_paths = &dependency,
      .dependency_count = 1u,
      .source = source,
      .skin = skin,
      .mesh_buffer = {.vertex_size = sizeof(VkrVertex3d),
                      .vertex_count = unique_count,
                      .vertices = unique,
                      .index_size = sizeof(uint32_t),
                      .index_count = 6u,
                      .indices = indices},
      .ranges = &range,
      .range_count = 1u,
      .budgets = vkr_packed_geometry_default_budgets(),
  };
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_mesh_cooked_encode(&scratch, &info, &bytes, &size));
  VkrMeshCookedDecoded decoded = {0};
  assert(vkr_mesh_cooked_decode(&result, &scratch, bytes, size, &decoded));
  assert(decoded.skin.animation_fingerprint == 456u);
  assert(decoded.skin.skin_count == 2u);
  assert(decoded.skin.joint_counts[0] == 2u);
  assert(decoded.skin.joint_counts[1] == 2u);
  skin_test_triangles(&decoded);
  uint8_t *variant = NULL;
  nodes[0].local.elements[12] = 7.0f;
  assert(
      vkr_mesh_cooked_source_variant(&scratch, bytes, size, &source, &variant));
  VkrMeshCookedDecoded variant_decoded = {0};
  assert(vkr_mesh_cooked_decode(&result, &scratch, variant, size,
                                &variant_decoded));
  assert(variant_decoded.source.nodes.data[0].local.elements[12] == 7.0f);
  assert(variant_decoded.skin.animation_fingerprint == 456u);
  assert(variant_decoded.skin.vertex_count == decoded.skin.vertex_count);
  assert(variant_decoded.skin.skin_count == decoded.skin.skin_count);
  assert(MemCompare(variant_decoded.skin.vertices, decoded.skin.vertices,
                    decoded.skin.vertex_count * sizeof(VkrMeshSkinVertex)) ==
         0);
  assert(MemCompare(variant_decoded.skin.joint_counts,
                    decoded.skin.joint_counts,
                    decoded.skin.skin_count * sizeof(uint32_t)) == 0);
  skin_test_triangles(&variant_decoded);
  nodes[0].skin = 1u;
  assert(!vkr_mesh_cooked_source_variant(&scratch, bytes, size, &source,
                                         &variant));
  nodes[0].skin = 0u;
  nodes[0].local.elements[12] = 0.0f;

  /* Repair both CRCs: these rejections must come from the new parser and
   * influence validation rather than transport integrity checks. */
  uint64_t strings = skin_test_read_u64(bytes + 64u);
  uint64_t extension_size = 16u + 4u * skin.skin_count + 32u * unique_count;
  assert(strings >= extension_size);
  uint8_t *extension = bytes + strings - extension_size;
  const uint32_t corrupt_offsets[] = {8u, 12u, 16u, 24u, 40u};
  const uint32_t corrupt_values[] = {UINT32_MAX, UINT32_MAX, 1u, 99u,
                                     0x7fc00000u};
  for (uint32_t i = 0u; i < ArrayCount(corrupt_offsets); ++i) {
    uint8_t *field = extension + corrupt_offsets[i];
    uint32_t saved = skin_test_read_u32(field);
    skin_test_write_u32(field, corrupt_values[i]);
    skin_test_refresh_integrity(bytes);
    assert(!vkr_mesh_cooked_decode(&result, &scratch, bytes, size, &decoded));
    skin_test_write_u32(field, saved);
    skin_test_refresh_integrity(bytes);
  }
  assert(vkr_mesh_cooked_decode(&result, &scratch, bytes, size, &decoded));

  assert(
      !vkr_mesh_cooked_decode(&result, &scratch, bytes, size - 1u, &decoded));
  joint_counts[1] = 1u;
  assert(!vkr_mesh_cooked_encode(&scratch, &info, &bytes, &size));
  assert(remove(dependency_path) == 0);
  arena_destroy(result_arena);
  arena_destroy(scratch_arena);
}

static void skin_test_gltf_normalized_weights(void) {
  printf("  Running normalized glTF skin cooking test...\n");
  static const char source_path[] = "build/vkr_skin_source.gltf";
  static const char binary_path[] = "build/vkr_skin_source.bin";
  static const char cooked_path[] = "build/vkr_skin_source.vkb";
  static const char source[] =
      "{\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"vkr_skin_source.bin\",\"byteLength\":120}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteLength\":72},"
      "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":6,"
      "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
      "{\"bufferView\":1,\"componentType\":5121,\"count\":6,\"type\":\"VEC4\"},"
      "{\"bufferView\":2,\"componentType\":5121,\"normalized\":true,"
      "\"count\":6,\"type\":\"VEC4\"}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"JOINTS_0\":1,\"WEIGHTS_0\":2}}]}],"
      "\"nodes\":[{\"mesh\":0,\"skin\":0},{\"children\":[2]},"
      "{\"translation\":[1,0,0]}],"
      "\"skins\":[{\"joints\":[1,2]}],"
      "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0}";
  uint8_t binary[120] = {0};
  for (uint32_t i = 0u; i < 6u; ++i) {
    VkrVertex3d vertex = skin_test_vertex(i % 3u);
    MemCopy(binary + i * 12u, &vertex.position, 12u);
    binary[72u + i * 4u] = i >= 3u ? 1u : 0u;
    binary[96u + i * 4u] = 255u;
  }
  skin_test_write(source_path, source, sizeof(source) - 1u);
  skin_test_write(binary_path, binary, sizeof(binary));
  Arena *scratch_arena = arena_create(MB(32), MB(1));
  Arena *result_arena = arena_create(MB(32), MB(1));
  assert(scratch_arena && result_arena);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch));
  assert(vkr_allocator_arena(&result));
  VkrMeshCookStats stats = {0};
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_mesh_cook_source(string8_lit(source_path),
                              string8_lit(cooked_path), &result, &scratch,
                              &stats, &error));
  FILE *file = fopen(cooked_path, "rb");
  assert(file);
  uint8_t bytes[16384];
  size_t size = fread(bytes, 1u, sizeof(bytes), file);
  assert(feof(file));
  assert(fclose(file) == 0);
  VkrMeshCookedDecoded decoded = {0};
  assert(vkr_mesh_cooked_decode(&result, &scratch, bytes, size, &decoded));
  assert(decoded.skin.skin_count == 1u);
  assert(decoded.skin.joint_counts[0] == 2u);
  assert(decoded.skin.animation_fingerprint != 0u);
  VkrAnimationAsset animation = {0};
  const char *import_error = NULL;
  assert(vkr_animation_import_gltf(&result, &scratch, string8_lit(source_path),
                                   &animation, &import_error));
  assert(decoded.skin.animation_fingerprint == animation.source_fingerprint);

  skin_test_triangles(&decoded);
  arena_destroy(result_arena);
  arena_destroy(scratch_arena);
  assert(remove(cooked_path) == 0);
  assert(remove(binary_path) == 0);
  assert(remove(source_path) == 0);
}

bool32_t run_mesh_skin_tests(void) {
  printf("Running mesh skin tests...\n");
  skin_test_dedup_codec();
  skin_test_gltf_normalized_weights();
  return true_v;
}
