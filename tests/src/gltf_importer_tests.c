#include "gltf_importer_tests.h"

#include "containers/str.h"
#include "filesystem/filesystem.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/mesh_loader_gltf.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define GLTF_TEST_FNV1A64_OFFSET_BASIS 0xcbf29ce484222325ull
#define GLTF_TEST_FNV1A64_PRIME 0x100000001b3ull

typedef struct GltfImporterTestCapture {
  VkrAllocator *allocator;
  uint32_t primitive_count;
  uint32_t total_vertices;
  uint32_t total_indices;
  String8 first_material_path;
} GltfImporterTestCapture;

static bool8_t gltf_test_make_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return false_v;
  }

#if defined(_WIN32)
  int result = _mkdir(path);
#else
  int result = mkdir(path, 0755);
#endif

  if (result == 0 || errno == EEXIST) {
    return true_v;
  }
  return false_v;
}

static void gltf_test_remove_file(const char *path) {
  if (!path || path[0] == '\0') {
    return;
  }
#if defined(_WIN32)
  _unlink(path);
#else
  unlink(path);
#endif
}

static void gltf_test_remove_dir(const char *path) {
  if (!path || path[0] == '\0') {
    return;
  }
#if defined(_WIN32)
  _rmdir(path);
#else
  rmdir(path);
#endif
}

static void gltf_test_ensure_dirs(void) {
  char tests_tmp[1024];
  snprintf(tests_tmp, sizeof(tests_tmp), "%stests/tmp", PROJECT_SOURCE_DIR);
  assert(gltf_test_make_dir(tests_tmp) == true_v);

  char importer_tmp[1024];
  snprintf(importer_tmp, sizeof(importer_tmp), "%stests/tmp/gltf_importer",
           PROJECT_SOURCE_DIR);
  assert(gltf_test_make_dir(importer_tmp) == true_v);

  char assets_materials[1024];
  snprintf(assets_materials, sizeof(assets_materials), "%sassets/materials",
           PROJECT_SOURCE_DIR);
  assert(gltf_test_make_dir(assets_materials) == true_v);
}

static bool8_t gltf_test_write_file_bytes(const char *path, const void *bytes,
                                          size_t byte_count) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    return false_v;
  }

  size_t written = fwrite(bytes, 1, byte_count, file);
  fclose(file);
  return written == byte_count ? true_v : false_v;
}

static bool8_t gltf_test_write_file_text(const char *path, const char *text) {
  if (!text) {
    return false_v;
  }

  return gltf_test_write_file_bytes(path, text, strlen(text));
}

static bool8_t gltf_test_read_file_text(VkrAllocator *allocator,
                                        const char *path, String8 *out_text) {
  assert(allocator != NULL);
  assert(path != NULL);
  assert(out_text != NULL);

  *out_text = (String8){0};

  FILE *file = fopen(path, "rb");
  if (!file) {
    return false_v;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false_v;
  }

  long file_size = ftell(file);
  if (file_size < 0) {
    fclose(file);
    return false_v;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false_v;
  }

  char *buffer = (char *)vkr_allocator_alloc(allocator, (uint64_t)file_size + 1,
                                             VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buffer) {
    fclose(file);
    return false_v;
  }

  size_t read = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);
  if (read != (size_t)file_size) {
    return false_v;
  }

  buffer[file_size] = '\0';
  *out_text =
      string8_create_from_cstr((const uint8_t *)buffer, (uint64_t)file_size);
  return true_v;
}

static bool8_t gltf_test_string8_equals_cstr(String8 value, const char *cstr) {
  if (!cstr) {
    return false_v;
  }

  uint64_t len = string_length(cstr);
  if (value.length != len) {
    return false_v;
  }

  return MemCompare(value.str, cstr, len) == 0;
}

static bool8_t gltf_test_file_exists(const char *path) {
  if (!path) {
    return false_v;
  }

  FILE *file = fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  fclose(file);
  return true_v;
}

static bool8_t gltf_test_vector_contains_path(Vector_String8 *paths,
                                              const char *path_cstr) {
  if (!paths || !path_cstr) {
    return false_v;
  }

  String8 expected = string8_create_from_cstr((const uint8_t *)path_cstr,
                                              string_length(path_cstr));
  for (uint64_t i = 0; i < paths->length; ++i) {
    String8 *value = vector_get_String8(paths, i);
    if (value && string8_equalsi(value, &expected)) {
      return true_v;
    }
  }

  return false_v;
}

static uint64_t gltf_test_hash_source_path(const char *source_path) {
  if (!source_path || source_path[0] == '\0') {
    return GLTF_TEST_FNV1A64_OFFSET_BASIS;
  }

  uint64_t hash = GLTF_TEST_FNV1A64_OFFSET_BASIS;
  for (const unsigned char *p = (const unsigned char *)source_path; *p; ++p) {
    hash ^= (uint64_t)(*p);
    hash *= GLTF_TEST_FNV1A64_PRIME;
  }

  return hash;
}

static void gltf_test_make_material_id(char *out_id, size_t out_id_size,
                                       const char *source_path,
                                       uint32_t material_index) {
  assert(out_id != NULL);
  assert(out_id_size > 0);
  assert(source_path != NULL);

  uint64_t source_hash = gltf_test_hash_source_path(source_path);
  snprintf(out_id, out_id_size, "gltf_mat_%016llx_%u",
           (unsigned long long)source_hash, material_index);
}

static void gltf_test_make_material_paths(const char *stem,
                                          const char *source_path,
                                          uint32_t material_index,
                                          char *out_absolute,
                                          size_t out_absolute_size,
                                          char *out_relative,
                                          size_t out_relative_size,
                                          char *out_material_id,
                                          size_t out_material_id_size) {
  assert(stem != NULL);
  assert(source_path != NULL);
  assert(out_absolute != NULL || out_relative != NULL || out_material_id != NULL);

  char material_id[128] = {0};
  gltf_test_make_material_id(material_id, sizeof(material_id), source_path,
                             material_index);

  if (out_absolute && out_absolute_size > 0) {
    snprintf(out_absolute, out_absolute_size, "%sassets/materials/%s/%s.mt",
             PROJECT_SOURCE_DIR, stem, material_id);
  }
  if (out_relative && out_relative_size > 0) {
    snprintf(out_relative, out_relative_size, "assets/materials/%s/%s.mt",
             stem, material_id);
  }

  if (out_material_id && out_material_id_size > 0) {
    snprintf(out_material_id, out_material_id_size, "%s", material_id);
  }
}

static bool8_t gltf_test_material_path_matches_pattern(String8 path,
                                                       const char *stem,
                                                       uint32_t material_index) {
  if (!path.str || !stem) {
    return false_v;
  }

  char prefix[256];
  snprintf(prefix, sizeof(prefix), "assets/materials/%s/gltf_mat_", stem);
  uint64_t prefix_len = string_length(prefix);
  if (path.length < prefix_len || MemCompare(path.str, prefix, prefix_len) != 0) {
    return false_v;
  }

  char suffix[32];
  snprintf(suffix, sizeof(suffix), "_%u.mt", material_index);
  uint64_t suffix_len = string_length(suffix);
  if (path.length < suffix_len) {
    return false_v;
  }

  uint64_t suffix_start = path.length - suffix_len;
  return MemCompare(path.str + suffix_start, suffix, suffix_len) == 0;
}

static bool8_t
gltf_test_capture_primitive(void *user_data,
                            const VkrMeshLoaderGltfPrimitive *primitive) {
  GltfImporterTestCapture *capture = (GltfImporterTestCapture *)user_data;
  if (!capture || !primitive || !primitive->vertices || !primitive->indices ||
      primitive->vertex_count == 0 || primitive->index_count == 0) {
    return false_v;
  }

  capture->primitive_count++;
  capture->total_vertices += primitive->vertex_count;
  capture->total_indices += primitive->index_count;
  if (capture->first_material_path.length == 0 &&
      primitive->material_path.str && primitive->material_path.length > 0) {
    capture->first_material_path =
        string8_duplicate(capture->allocator, &primitive->material_path);
  }

  return true_v;
}

static void gltf_test_remove_generated_material(const char *stem) {
  char source_path[1024] = {0};
  snprintf(source_path, sizeof(source_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);

  char material_file[1024] = {0};
  gltf_test_make_material_paths(stem, source_path, 0, material_file,
                                sizeof(material_file), NULL, 0, NULL, 0);
  gltf_test_remove_file(material_file);

  /* Cleanup legacy pre-hash naming used before material identity fix. */
  snprintf(material_file, sizeof(material_file),
           "%sassets/materials/%s/gltf_mat_0.mt", PROJECT_SOURCE_DIR, stem);
  gltf_test_remove_file(material_file);

  char material_dir[1024];
  snprintf(material_dir, sizeof(material_dir), "%sassets/materials/%s",
           PROJECT_SOURCE_DIR, stem);
  gltf_test_remove_dir(material_dir);
}

static void gltf_test_remove_source_files(const char *stem) {
  char gltf_file[1024];
  snprintf(gltf_file, sizeof(gltf_file), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  gltf_test_remove_file(gltf_file);

  char bin_file[1024];
  snprintf(bin_file, sizeof(bin_file), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);
  gltf_test_remove_file(bin_file);
}

static void gltf_test_write_basic_triangle_bin(const char *path) {
  float positions[9] = {
      0.0f, 0.0f, 0.0f, //
      1.0f, 0.0f, 0.0f, //
      0.0f, 1.0f, 0.0f, //
  };
  uint16_t indices[3] = {0u, 1u, 2u};

  uint8_t bytes[42] = {0};
  MemCopy(bytes, positions, sizeof(positions));
  MemCopy(bytes + sizeof(positions), indices, sizeof(indices));
  assert(gltf_test_write_file_bytes(path, bytes, sizeof(bytes)) == true_v);
}

static VkrMeshLoaderGltfParseInfo gltf_test_make_parse_info(
    VkrAllocator *allocator, VkrAllocator *scratch_allocator,
    const char *source_path_cstr, VkrRendererError *out_error,
    GltfImporterTestCapture *capture) {
  String8 source_path = string8_create_from_cstr(
      (const uint8_t *)source_path_cstr, string_length(source_path_cstr));
  String8 source_dir = file_path_get_directory(allocator, source_path);
  String8 source_stem = string8_get_stem(allocator, source_path);

  return (VkrMeshLoaderGltfParseInfo){
      .source_path = source_path,
      .source_dir = source_dir,
      .source_stem = source_stem,
      .load_allocator = allocator,
      .scratch_allocator = scratch_allocator,
      .out_error = out_error,
      .on_primitive = gltf_test_capture_primitive,
      .user_data = capture,
  };
}

static void test_gltf_import_basic_and_deterministic_mt(void) {
  printf("  Running test_gltf_import_basic_and_deterministic_mt...\n");

  const char *stem = "gltf_import_basic";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);
  char mt_path[1024] = {0};
  char mt_relative_path[256] = {0};
  char material_id[128] = {0};
  gltf_test_make_material_paths(stem, gltf_path, 0, mt_path, sizeof(mt_path),
                                mt_relative_path, sizeof(mt_relative_path),
                                material_id, sizeof(material_id));

  gltf_test_write_basic_triangle_bin(bin_path);

  char gltf_json[8192];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,"
           "0.7,0.6,0.5],\"metallicFactor\":0.7,\"roughnessFactor\":0.3,"
           "\"baseColorTexture\":{\"index\":0}},\"normalTexture\":{\"index\":1,"
           "\"scale\":0.9},\"occlusionTexture\":{\"index\":2,\"strength\":0.4},"
           "\"emissiveTexture\":{\"index\":3},\"emissiveFactor\":[0.1,0.2,0.3],"
           "\"alphaMode\":\"BLEND\"}],"
           "\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2},{"
           "\"source\":3}],"
           "\"images\":[{\"uri\":\"base.png\"},{\"uri\":\"normal.png\"},{"
           "\"uri\":\"occ.png\"},{\"uri\":\"emit.png\"}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, &capture);
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(capture.primitive_count == 1u);
  assert(capture.total_vertices == 3u);
  assert(capture.total_indices == 3u);
  assert(gltf_test_material_path_matches_pattern(capture.first_material_path,
                                                 stem, 0) == true_v);

  String8 expected_rel = string8_create_formatted(
      &allocator, "%s", mt_relative_path);
  assert(gltf_test_string8_equals_cstr(capture.first_material_path,
                                       (const char *)expected_rel.str) ==
         true_v);

  String8 first_contents = {0};
  assert(gltf_test_read_file_text(&allocator, mt_path, &first_contents) ==
         true_v);
  assert(strstr((const char *)first_contents.str, "type=pbr") != NULL);
  assert(strstr((const char *)first_contents.str, "alpha_mode=blend") != NULL);
  assert(strstr((const char *)first_contents.str, "base_color_texture=") !=
         NULL);
  assert(strstr((const char *)first_contents.str, "cs=srgb") != NULL);
  assert(strstr((const char *)first_contents.str, "tc=color_srgb") != NULL);
  char expected_name_line[192] = {0};
  snprintf(expected_name_line, sizeof(expected_name_line), "name=%s",
           material_id);
  assert(strstr((const char *)first_contents.str, expected_name_line) != NULL);

  GltfImporterTestCapture capture_second = {.allocator = &allocator};
  error = VKR_RENDERER_ERROR_NONE;
  parse_info.user_data = &capture_second;
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);

  String8 second_contents = {0};
  assert(gltf_test_read_file_text(&allocator, mt_path, &second_contents) ==
         true_v);
  assert(first_contents.length == second_contents.length);
  assert(MemCompare(first_contents.str, second_contents.str,
                    first_contents.length) == 0);

  arena_destroy(scratch_arena);
  arena_destroy(arena);

  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  printf("  test_gltf_import_basic_and_deterministic_mt PASSED\n");
}

static void test_gltf_import_fails_without_position(void) {
  printf("  Running test_gltf_import_fails_without_position...\n");

  const char *stem = "gltf_import_missing_position";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);

  gltf_test_write_basic_triangle_bin(bin_path);

  char gltf_json[4096];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"NORMAL\":0},"
           "\"indices\":1}]}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, &capture);
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == false_v);
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);
  assert(capture.primitive_count == 0u);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_source_files(stem);

  printf("  test_gltf_import_fails_without_position PASSED\n");
}

static void test_gltf_import_rejects_data_uri_images(void) {
  printf("  Running test_gltf_import_rejects_data_uri_images...\n");

  const char *stem = "gltf_import_data_uri";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);

  gltf_test_write_basic_triangle_bin(bin_path);

  char gltf_json[4096];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{"
           "\"index\":0}}}],"
           "\"textures\":[{\"source\":0}],"
           "\"images\":[{\"uri\":\"data:image/png;base64,AA==\"}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, &capture);
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == false_v);
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  printf("  test_gltf_import_rejects_data_uri_images PASSED\n");
}

static void test_gltf_import_rejects_buffer_view_images(void) {
  printf("  Running test_gltf_import_rejects_buffer_view_images...\n");

  const char *stem = "gltf_import_buffer_view_image";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);

  float positions[9] = {
      0.0f, 0.0f, 0.0f, //
      1.0f, 0.0f, 0.0f, //
      0.0f, 1.0f, 0.0f, //
  };
  uint16_t indices[3] = {0u, 1u, 2u};
  uint8_t image_bytes[4] = {0x89, 0x50, 0x4E, 0x47};
  uint8_t bytes[46] = {0};
  MemCopy(bytes, positions, sizeof(positions));
  MemCopy(bytes + sizeof(positions), indices, sizeof(indices));
  MemCopy(bytes + sizeof(positions) + sizeof(indices), image_bytes,
          sizeof(image_bytes));
  assert(gltf_test_write_file_bytes(bin_path, bytes, sizeof(bytes)) == true_v);

  char gltf_json[4096];
  snprintf(
      gltf_json, sizeof(gltf_json),
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"scene\":0,"
      "\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
      "\"indices\":1,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{"
      "\"index\":0}}}],"
      "\"textures\":[{\"source\":0}],"
      "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/png\"}],"
      "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":46}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
      "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
      "\"target\":34963},{\"buffer\":0,\"byteOffset\":42,\"byteLength\":4}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
      "\"type\":\"SCALAR\"}]"
      "}",
      stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, &capture);
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == false_v);
  assert(error == VKR_RENDERER_ERROR_INVALID_PARAMETER);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  printf("  test_gltf_import_rejects_buffer_view_images PASSED\n");
}

static void test_gltf_import_collects_external_dependencies(void) {
  printf("  Running test_gltf_import_collects_external_dependencies...\n");

  const char *stem = "gltf_import_dependencies";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);
  char base_texture_name[128];
  snprintf(base_texture_name, sizeof(base_texture_name), "%s_base.png", stem);
  char missing_texture_name[128];
  snprintf(missing_texture_name, sizeof(missing_texture_name), "%s_missing.png",
           stem);
  char base_texture_path[1024];
  snprintf(base_texture_path, sizeof(base_texture_path),
           "%stests/tmp/gltf_importer/%s", PROJECT_SOURCE_DIR,
           base_texture_name);
  char missing_texture_path[1024];
  snprintf(missing_texture_path, sizeof(missing_texture_path),
           "%stests/tmp/gltf_importer/%s", PROJECT_SOURCE_DIR,
           missing_texture_name);
  char mt_path[1024] = {0};
  char mt_relative_path[256] = {0};
  gltf_test_make_material_paths(stem, gltf_path, 0, mt_path, sizeof(mt_path),
                                mt_relative_path, sizeof(mt_relative_path),
                                NULL, 0);

  gltf_test_write_basic_triangle_bin(bin_path);
  const uint8_t texture_stub[4] = {0x89, 0x50, 0x4E, 0x47};
  assert(gltf_test_write_file_bytes(base_texture_path, texture_stub,
                                    sizeof(texture_stub)) == true_v);

  char gltf_json[8192];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{"
           "\"index\":0}},\"normalTexture\":{\"index\":1}}],"
           "\"textures\":[{\"source\":0},{\"source\":1}],"
           "\"images\":[{\"uri\":\"%s\"},{\"uri\":\"%s\"}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           base_texture_name, missing_texture_name, stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  Vector_String8 dependency_paths = vector_create_String8(&allocator);
  Vector_String8 generated_material_paths = vector_create_String8(&allocator);
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, &capture);
  parse_info.out_dependency_paths = &dependency_paths;
  parse_info.out_generated_material_paths = &generated_material_paths;
  assert(vkr_mesh_loader_gltf_parse(&parse_info) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);

  assert(gltf_test_vector_contains_path(&dependency_paths, gltf_path) ==
         true_v);
  assert(gltf_test_vector_contains_path(&dependency_paths, bin_path) == true_v);
  assert(gltf_test_vector_contains_path(&dependency_paths, base_texture_path) ==
         true_v);
  assert(gltf_test_vector_contains_path(&dependency_paths,
                                        missing_texture_path) == false_v);
  assert(gltf_test_vector_contains_path(&generated_material_paths,
                                        mt_relative_path) == true_v);
  assert(gltf_test_file_exists(mt_path) == true_v);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_file(base_texture_path);
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  printf("  test_gltf_import_collects_external_dependencies PASSED\n");
}

static void
test_gltf_import_generate_materials_regenerates_missing_files(void) {
  printf(
      "  Running test_gltf_import_generate_materials_regenerates_missing_files"
      "...\n");

  const char *stem = "gltf_import_material_regen";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  char gltf_path[1024];
  snprintf(gltf_path, sizeof(gltf_path), "%stests/tmp/gltf_importer/%s.gltf",
           PROJECT_SOURCE_DIR, stem);
  char bin_path[1024];
  snprintf(bin_path, sizeof(bin_path), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem);
  char mt_path[1024] = {0};
  char mt_relative_path[256] = {0};
  gltf_test_make_material_paths(stem, gltf_path, 0, mt_path, sizeof(mt_path),
                                mt_relative_path, sizeof(mt_relative_path),
                                NULL, 0);

  gltf_test_write_basic_triangle_bin(bin_path);

  char gltf_json[8192];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.7,"
           "0.8,0.9,1.0],\"metallicFactor\":0.2,\"roughnessFactor\":0.6}}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem);
  assert(gltf_test_write_file_text(gltf_path, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  Vector_String8 dependency_paths = vector_create_String8(&allocator);
  Vector_String8 generated_material_paths = vector_create_String8(&allocator);
  VkrRendererError error = VKR_RENDERER_ERROR_NONE;
  VkrMeshLoaderGltfParseInfo parse_info = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path, &error, NULL);
  parse_info.on_primitive = NULL;
  parse_info.user_data = NULL;
  parse_info.out_dependency_paths = &dependency_paths;
  parse_info.out_generated_material_paths = &generated_material_paths;

  assert(vkr_mesh_loader_gltf_generate_materials(&parse_info) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(gltf_test_file_exists(mt_path) == true_v);
  assert(gltf_test_vector_contains_path(&generated_material_paths,
                                        mt_relative_path) == true_v);
  assert(gltf_test_vector_contains_path(&dependency_paths, gltf_path) ==
         true_v);
  assert(gltf_test_vector_contains_path(&dependency_paths, bin_path) == true_v);

  gltf_test_remove_file(mt_path);
  assert(gltf_test_file_exists(mt_path) == false_v);

  vector_clear_String8(&dependency_paths);
  vector_clear_String8(&generated_material_paths);
  error = VKR_RENDERER_ERROR_NONE;
  assert(vkr_mesh_loader_gltf_generate_materials(&parse_info) == true_v);
  assert(error == VKR_RENDERER_ERROR_NONE);
  assert(gltf_test_file_exists(mt_path) == true_v);
  assert(gltf_test_vector_contains_path(&generated_material_paths,
                                        mt_relative_path) == true_v);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_source_files(stem);
  gltf_test_remove_generated_material(stem);

  printf("  test_gltf_import_generate_materials_regenerates_missing_files "
         "PASSED\n");
}

static void test_gltf_import_material_ids_are_unique_per_source(void) {
  printf("  Running test_gltf_import_material_ids_are_unique_per_source...\n");

  const char *stem_a = "gltf_import_collision_a";
  const char *stem_b = "gltf_import_collision_b";
  gltf_test_ensure_dirs();
  gltf_test_remove_source_files(stem_a);
  gltf_test_remove_generated_material(stem_a);
  gltf_test_remove_source_files(stem_b);
  gltf_test_remove_generated_material(stem_b);

  char gltf_path_a[1024];
  snprintf(gltf_path_a, sizeof(gltf_path_a),
           "%stests/tmp/gltf_importer/%s.gltf", PROJECT_SOURCE_DIR, stem_a);
  char bin_path_a[1024];
  snprintf(bin_path_a, sizeof(bin_path_a), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem_a);
  char gltf_path_b[1024];
  snprintf(gltf_path_b, sizeof(gltf_path_b),
           "%stests/tmp/gltf_importer/%s.gltf", PROJECT_SOURCE_DIR, stem_b);
  char bin_path_b[1024];
  snprintf(bin_path_b, sizeof(bin_path_b), "%stests/tmp/gltf_importer/%s.bin",
           PROJECT_SOURCE_DIR, stem_b);

  gltf_test_write_basic_triangle_bin(bin_path_a);
  gltf_test_write_basic_triangle_bin(bin_path_b);

  char gltf_json[4096];
  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,"
           "1.0,1.0,1.0]}}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem_a);
  assert(gltf_test_write_file_text(gltf_path_a, gltf_json) == true_v);

  snprintf(gltf_json, sizeof(gltf_json),
           "{"
           "\"asset\":{\"version\":\"2.0\"},"
           "\"scene\":0,"
           "\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[{\"mesh\":0}],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},"
           "\"indices\":1,\"material\":0}]}],"
           "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,"
           "1.0,1.0,1.0]}}],"
           "\"buffers\":[{\"uri\":\"%s.bin\",\"byteLength\":42}],"
           "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,"
           "\"target\":34962},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,"
           "\"target\":34963}],"
           "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":"
           "3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,"
           "\"count\":3,\"type\":\"SCALAR\"}]"
           "}",
           stem_b);
  assert(gltf_test_write_file_text(gltf_path_b, gltf_json) == true_v);

  Arena *arena = arena_create(MB(2), MB(2));
  Arena *scratch_arena = arena_create(MB(2), MB(2));
  VkrAllocator allocator = {.ctx = arena};
  VkrAllocator scratch_allocator = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&allocator));
  assert(vkr_allocator_arena(&scratch_allocator));

  VkrRendererError error_a = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture_a = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info_a = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path_a, &error_a, &capture_a);
  assert(vkr_mesh_loader_gltf_parse(&parse_info_a) == true_v);
  assert(error_a == VKR_RENDERER_ERROR_NONE);

  VkrRendererError error_b = VKR_RENDERER_ERROR_NONE;
  GltfImporterTestCapture capture_b = {.allocator = &allocator};
  VkrMeshLoaderGltfParseInfo parse_info_b = gltf_test_make_parse_info(
      &allocator, &scratch_allocator, gltf_path_b, &error_b, &capture_b);
  assert(vkr_mesh_loader_gltf_parse(&parse_info_b) == true_v);
  assert(error_b == VKR_RENDERER_ERROR_NONE);

  assert(gltf_test_material_path_matches_pattern(capture_a.first_material_path,
                                                 stem_a, 0) == true_v);
  assert(gltf_test_material_path_matches_pattern(capture_b.first_material_path,
                                                 stem_b, 0) == true_v);
  assert(string8_equalsi(&capture_a.first_material_path,
                         &capture_b.first_material_path) == false_v);

  char mt_path_a[1024] = {0};
  char mt_relative_path_a[256] = {0};
  char material_id_a[128] = {0};
  gltf_test_make_material_paths(stem_a, gltf_path_a, 0, mt_path_a,
                                sizeof(mt_path_a), mt_relative_path_a,
                                sizeof(mt_relative_path_a), material_id_a,
                                sizeof(material_id_a));

  char mt_path_b[1024] = {0};
  char mt_relative_path_b[256] = {0};
  char material_id_b[128] = {0};
  gltf_test_make_material_paths(stem_b, gltf_path_b, 0, mt_path_b,
                                sizeof(mt_path_b), mt_relative_path_b,
                                sizeof(mt_relative_path_b), material_id_b,
                                sizeof(material_id_b));

  assert(strcmp(material_id_a, material_id_b) != 0);
  assert(gltf_test_string8_equals_cstr(capture_a.first_material_path,
                                       mt_relative_path_a) == true_v);
  assert(gltf_test_string8_equals_cstr(capture_b.first_material_path,
                                       mt_relative_path_b) == true_v);

  String8 contents_a = {0};
  String8 contents_b = {0};
  assert(gltf_test_read_file_text(&allocator, mt_path_a, &contents_a) == true_v);
  assert(gltf_test_read_file_text(&allocator, mt_path_b, &contents_b) == true_v);

  char expected_name_line_a[192] = {0};
  char expected_name_line_b[192] = {0};
  snprintf(expected_name_line_a, sizeof(expected_name_line_a), "name=%s",
           material_id_a);
  snprintf(expected_name_line_b, sizeof(expected_name_line_b), "name=%s",
           material_id_b);
  assert(strstr((const char *)contents_a.str, expected_name_line_a) != NULL);
  assert(strstr((const char *)contents_b.str, expected_name_line_b) != NULL);

  arena_destroy(scratch_arena);
  arena_destroy(arena);
  gltf_test_remove_source_files(stem_a);
  gltf_test_remove_generated_material(stem_a);
  gltf_test_remove_source_files(stem_b);
  gltf_test_remove_generated_material(stem_b);

  printf("  test_gltf_import_material_ids_are_unique_per_source PASSED\n");
}

bool32_t run_gltf_importer_tests(void) {
  printf("--- Starting glTF Importer Tests ---\n");

  test_gltf_import_basic_and_deterministic_mt();
  test_gltf_import_fails_without_position();
  test_gltf_import_rejects_data_uri_images();
  test_gltf_import_rejects_buffer_view_images();
  test_gltf_import_collects_external_dependencies();
  test_gltf_import_generate_materials_regenerates_missing_files();
  test_gltf_import_material_ids_are_unique_per_source();

  printf("--- glTF Importer Tests Completed ---\n");
  return true_v;
}
