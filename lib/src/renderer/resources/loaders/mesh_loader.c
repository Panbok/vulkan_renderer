#include "renderer/resources/loaders/mesh_loader.h"

#include "containers/str.h"
#include "containers/vector.h"
#include "core/logger.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

/**
 * @brief Result of loading a single mesh in a batch operation.
 */
typedef struct VkrMeshBatchResult {
  VkrMeshLoaderResult *result;
  VkrRendererError error;
  bool8_t success;
} VkrMeshBatchResult;

vkr_internal uint32_t vkr_host_to_little_u32(uint32_t value) {
  const union {
    uint32_t u32;
    uint8_t u8[4];
  } endian_check = {0x01020304};
  const bool8_t is_little_endian = (endian_check.u8[0] == 0x04);

  if (is_little_endian)
    return value;

  return ((value & 0xFF000000) >> 24) | ((value & 0x00FF0000) >> 8) |
         ((value & 0x0000FF00) << 8) | ((value & 0x000000FF) << 24);
}

Vector(Vec2);
Vector(Vec3);
Vector(VkrVertex3d);
Vector(VkrMeshLoaderSubset);
#define DEFAULT_SHADER string8_lit("shader.default.world")
#define VKR_MESH_CACHE_MAGIC 0x564B4D48u /* 'VKMH' */
#define VKR_MESH_CACHE_VERSION 1u
#define VKR_MESH_CACHE_EXT "vkb"

typedef struct VkrMeshLoaderMaterialDef {
  String8 name;
  String8 shader_name;
  Vec4 diffuse_color;
  Vec4 specular_color;
  Vec3 emission_color;
  float32_t shininess;
  String8 diffuse_map;
  String8 specular_map;
  String8 normal_map;
  String8 generated_path;
  VkrMaterialHandle material_handle;
  bool8_t generated;
} VkrMeshLoaderMaterialDef;
Vector(VkrMeshLoaderMaterialDef);

typedef struct VkrMeshLoaderSubsetBuilder {
  Vector_VkrVertex3d vertices;
  Vector_uint32_t indices;
  String8 name;
  String8 material_name;
  VkrPipelineDomain pipeline_domain;
  String8 shader_override;
} VkrMeshLoaderSubsetBuilder;

typedef struct VkrMeshLoaderState {
  VkrMeshLoaderContext *context;
  VkrAllocator *load_allocator;
  VkrAllocator *temp_allocator;
  VkrAllocator *scratch_allocator;

  Vector_Vec3 positions;
  Vector_Vec3 normals;
  Vector_Vec2 texcoords;
  Vector_VkrMeshLoaderSubset subsets;
  Vector_VkrMeshLoaderMaterialDef materials;
  VkrMeshLoaderSubsetBuilder builder;

  String8 obj_path;
  String8 obj_dir;
  String8 obj_stem;
  String8 material_dir;

  VkrRendererError *out_error;
} VkrMeshLoaderState;

typedef struct VkrMeshLoaderVertexRef {
  int32_t position;
  int32_t texcoord;
  int32_t normal;
} VkrMeshLoaderVertexRef;

typedef struct VkrMeshLoaderBinaryReader {
  uint8_t *ptr;
  uint8_t *end;
} VkrMeshLoaderBinaryReader;

typedef struct VkrMeshLoadJobPayload {
  String8 mesh_path;
  VkrMeshLoaderContext *context;
  VkrAllocator *result_allocator;
  VkrMeshLoaderResult *result;
  VkrRendererError *error;
  bool8_t *success;
} VkrMeshLoadJobPayload;

vkr_internal void
vkr_mesh_loader_builder_init(VkrMeshLoaderSubsetBuilder *builder,
                             VkrAllocator *allocator);
vkr_internal String8 vkr_mesh_loader_make_material_dir(VkrAllocator *allocator,
                                                       const String8 *stem);
vkr_internal bool8_t vkr_mesh_loader_read_file_to_string(
    VkrAllocator *allocator, String8 file_path, String8 *out_content,
    VkrRendererError *out_error);
vkr_internal void vkr_mesh_loader_parse_next_line(String8 *file_str,
                                                  uint64_t *offset,
                                                  String8 *out_line);
vkr_internal bool8_t vkr_mesh_loader_parse_vec3_line(String8 *line,
                                                     uint32_t prefix_len,
                                                     Vec3 *out_vec);
vkr_internal void vkr_mesh_loader_cleanup_arenas(
    VkrMeshLoaderResult **results, Arena **arenas, void **pool_chunks,
    uint32_t count, VkrArenaPool *arena_pool);
vkr_internal void vkr_mesh_loader_set_all_errors(VkrMeshBatchResult *results,
                                                 uint32_t count,
                                                 VkrRendererError error);

vkr_internal bool8_t vkr_mesh_loader_write_bytes(FileHandle *fh,
                                                 const void *data,
                                                 uint64_t size) {
  uint64_t written = 0;
  FileError err = file_write(fh, size, (const uint8_t *)data, &written);
  return err == FILE_ERROR_NONE && written == size;
}

vkr_internal bool8_t vkr_mesh_loader_write_u32(FileHandle *fh, uint32_t value) {
  uint32_t little_endian_value = vkr_host_to_little_u32(value);
  return vkr_mesh_loader_write_bytes(fh, &little_endian_value,
                                     sizeof(uint32_t));
}

vkr_internal bool8_t vkr_mesh_loader_write_f32(FileHandle *fh,
                                               float32_t value) {
  union {
    float32_t f32;
    uint32_t u32;
  } float_bits = {.f32 = value};
  uint32_t little_endian_bits = vkr_host_to_little_u32(float_bits.u32);
  return vkr_mesh_loader_write_bytes(fh, &little_endian_bits, sizeof(uint32_t));
}

vkr_internal bool8_t vkr_mesh_loader_write_vec3(FileHandle *fh, Vec3 value) {
  return vkr_mesh_loader_write_f32(fh, value.x) &&
         vkr_mesh_loader_write_f32(fh, value.y) &&
         vkr_mesh_loader_write_f32(fh, value.z);
}

vkr_internal bool8_t vkr_mesh_loader_write_string(FileHandle *fh,
                                                  String8 value) {
  if (value.length > UINT32_MAX ||
      !vkr_mesh_loader_write_u32(fh, (uint32_t)value.length))
    return false_v;

  if (value.length == 0 || !value.str)
    return true_v;

  return vkr_mesh_loader_write_bytes(fh, value.str, value.length);
}

vkr_internal bool8_t vkr_mesh_loader_read_bytes(
    VkrMeshLoaderBinaryReader *reader, uint64_t size, void *out) {
  if (!reader || reader->ptr + size > reader->end)
    return false_v;

  if (out)
    MemCopy(out, reader->ptr, size);

  reader->ptr += size;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_u32(VkrMeshLoaderBinaryReader *reader,
                                              uint32_t *out) {
  uint32_t little_endian_value = 0;
  if (!vkr_mesh_loader_read_bytes(reader, sizeof(uint32_t),
                                  &little_endian_value)) {
    return false_v;
  }
  if (out) {
    *out = vkr_host_to_little_u32(little_endian_value);
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_f32(VkrMeshLoaderBinaryReader *reader,
                                              float32_t *out) {
  uint32_t little_endian_bits = 0;
  if (!vkr_mesh_loader_read_bytes(reader, sizeof(uint32_t),
                                  &little_endian_bits)) {
    return false_v;
  }
  if (out) {
    union {
      uint32_t u32;
      float32_t f32;
    } float_bits = {.u32 = vkr_host_to_little_u32(little_endian_bits)};
    *out = float_bits.f32;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_mesh_loader_read_vec3(VkrMeshLoaderBinaryReader *reader, Vec3 *out) {
  return vkr_mesh_loader_read_f32(reader, &out->x) &&
         vkr_mesh_loader_read_f32(reader, &out->y) &&
         vkr_mesh_loader_read_f32(reader, &out->z);
}

vkr_internal bool8_t vkr_mesh_loader_read_string(
    VkrMeshLoaderBinaryReader *reader, VkrAllocator *allocator, String8 *out) {
  uint32_t len = 0;
  if (!vkr_mesh_loader_read_u32(reader, &len) || reader->ptr + len > reader->end)
    return false_v;

  String8 view = {.str = reader->ptr, .length = len};
  reader->ptr += len;

  if (out)
    *out = string8_duplicate(allocator, &view);

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_file_to_string(
    VkrAllocator *allocator, String8 file_path, String8 *out_content,
    VkrRendererError *out_error) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(out_content != NULL, "Out content is NULL");

  FilePath fp = file_path_create((const char *)file_path.str, allocator,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("MeshLoader: failed to open '%s'", fp.path.str);
    return false_v;
  }

  FileError read_err = file_read_string(&fh, allocator, out_content);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE) {
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    log_error("MeshLoader: failed to read '%s'", fp.path.str);
    return false_v;
  }

  return true_v;
}

vkr_internal String8 vkr_mesh_loader_cache_path(VkrMeshLoaderState *state) {
  assert_log(state != NULL, "State is NULL");
  String8 cache_file = string8_create_formatted(
      state->load_allocator, "%.*s.%s", (int32_t)state->obj_stem.length,
      state->obj_stem.str, VKR_MESH_CACHE_EXT);
  return file_path_join(state->load_allocator, state->obj_dir, cache_file);
}

vkr_internal VkrMeshLoaderState vkr_mesh_loader_state_create(
    VkrMeshLoaderContext *context, VkrAllocator *load_allocator,
    VkrAllocator *temp_allocator, VkrAllocator *scratch_allocator, String8 name,
    VkrRendererError *out_error) {

  VkrMeshLoaderState state = {
      .context = context,
      .load_allocator = load_allocator,
      .temp_allocator = temp_allocator,
      .scratch_allocator = scratch_allocator,
      .positions = {0},
      .normals = {0},
      .texcoords = {0},
      .subsets = {0},
      .materials = {0},
      .obj_path = {0},
      .obj_dir = {0},
      .obj_stem = {0},
      .out_error = out_error,
  };

  state.positions = vector_create_Vec3(state.load_allocator);
  state.normals = vector_create_Vec3(state.load_allocator);
  state.texcoords = vector_create_Vec2(state.load_allocator);
  state.subsets = vector_create_VkrMeshLoaderSubset(state.load_allocator);
  state.materials =
      vector_create_VkrMeshLoaderMaterialDef(state.load_allocator);
  state.obj_path = string8_duplicate(state.load_allocator, &name);
  state.obj_dir = file_path_get_directory(state.load_allocator, name);
  state.obj_stem = string8_get_stem(state.load_allocator, name);
  state.material_dir =
      vkr_mesh_loader_make_material_dir(state.load_allocator, &state.obj_stem);
  vkr_mesh_loader_builder_init(&state.builder, state.load_allocator);
  return state;
}

vkr_internal bool8_t vkr_mesh_loader_write_binary(VkrMeshLoaderState *state,
                                                  String8 cache_path) {
  assert_log(state != NULL, "State is NULL");

  if (state->subsets.length == 0 || !cache_path.str)
    return false_v;

  String8 cache_dir =
      file_path_get_directory(state->load_allocator, cache_path);
  if (!file_ensure_directory(state->load_allocator, &cache_dir)) {
    log_warn("Failed to ensure cache directory '%.*s'",
             (int32_t)cache_dir.length, cache_dir.str);
    return false_v;
  }

  FilePath file_path =
      file_path_create((const char *)cache_path.str, state->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_warn("Failed to open cache '%s' for write: %s", file_path.path.str,
             file_get_error_string(ferr).str);
    return false_v;
  }

  bool8_t ok = true_v;
  ok = ok && vkr_mesh_loader_write_u32(&fh, VKR_MESH_CACHE_MAGIC);
  ok = ok && vkr_mesh_loader_write_u32(&fh, VKR_MESH_CACHE_VERSION);
  ok = ok && vkr_mesh_loader_write_string(&fh, state->obj_path);
  ok = ok && vkr_mesh_loader_write_u32(&fh, (uint32_t)state->subsets.length);

  for (uint64_t i = 0; ok && i < state->subsets.length; ++i) {
    VkrMeshLoaderSubset *subset =
        vector_get_VkrMeshLoaderSubset(&state->subsets, i);
    VkrGeometryConfig *cfg = &subset->geometry_config;

    uint64_t name_len = strnlen(cfg->name, GEOMETRY_NAME_MAX_LENGTH);
    String8 subset_name = {.str = (uint8_t *)cfg->name, .length = name_len};
    String8 material_str =
        subset->material_name.str
            ? subset->material_name
            : (String8){.str = (uint8_t *)cfg->material_name,
                        .length = strnlen(cfg->material_name,
                                          MATERIAL_NAME_MAX_LENGTH)};
    ok = ok && vkr_mesh_loader_write_string(&fh, subset_name);
    ok = ok && vkr_mesh_loader_write_string(&fh, material_str);
    ok = ok && vkr_mesh_loader_write_string(&fh, subset->shader_override);
    ok = ok && vkr_mesh_loader_write_u32(&fh, subset->pipeline_domain);
    ok = ok && vkr_mesh_loader_write_u32(&fh, cfg->vertex_size);
    ok = ok && vkr_mesh_loader_write_u32(&fh, cfg->vertex_count);
    ok = ok && vkr_mesh_loader_write_u32(&fh, cfg->index_size);
    ok = ok && vkr_mesh_loader_write_u32(&fh, cfg->index_count);
    ok = ok && vkr_mesh_loader_write_vec3(&fh, cfg->center);
    ok = ok && vkr_mesh_loader_write_vec3(&fh, cfg->min_extents);
    ok = ok && vkr_mesh_loader_write_vec3(&fh, cfg->max_extents);

    uint64_t vertex_bytes =
        (uint64_t)cfg->vertex_size * (uint64_t)cfg->vertex_count;
    uint64_t index_bytes =
        (uint64_t)cfg->index_size * (uint64_t)cfg->index_count;

    ok = ok && vkr_mesh_loader_write_bytes(&fh, (const uint8_t *)cfg->vertices,
                                           vertex_bytes);
    ok = ok && vkr_mesh_loader_write_bytes(&fh, (const uint8_t *)cfg->indices,
                                           index_bytes);
  }

  file_close(&fh);

  if (ok) {
    log_debug("Wrote cache '%s'", file_path.path.str);
  } else {
    log_warn("Failed writing cache '%s'", file_path.path.str);
  }

  return ok;
}

vkr_internal VkrMeshLoaderVertexRef
vkr_mesh_loader_parse_vertex_ref(const String8 *token) {
  VkrMeshLoaderVertexRef ref = {0, 0, 0};
  if (!token || !token->str || token->length == 0)
    return ref;

  int32_t values[3] = {0};
  uint32_t current = 0;
  int32_t sign = 1;
  int32_t value = 0;
  bool8_t has_digits = false_v;

  for (uint64_t i = 0; i < token->length; ++i) {
    uint8_t ch = token->str[i];
    if (ch == '-') {
      if (has_digits)
        continue;
      sign = -1;
    } else if (ch == '/') {
      if (current < 3)
        values[current++] = has_digits ? sign * value : 0;
      sign = 1;
      value = 0;
      has_digits = false_v;
    } else if (ch >= '0' && ch <= '9') {
      value = value * 10 + (int32_t)(ch - '0');
      has_digits = true_v;
    }
  }

  if (current < 3)
    values[current++] = has_digits ? sign * value : 0;

  ref.position = values[0];
  ref.texcoord = (current >= 2) ? values[1] : 0;
  ref.normal = (current >= 3) ? values[2] : 0;

  return ref;
}

vkr_internal uint32_t vkr_mesh_loader_fix_index(int32_t value, uint32_t count) {
  if (value > 0)
    return (uint32_t)(value - 1);

  if (value < 0) {
    int64_t resolved = (int64_t)count + value;
    return resolved < 0 ? 0 : (uint32_t)resolved;
  }

  return 0;
}

vkr_internal void
vkr_mesh_loader_builder_init(VkrMeshLoaderSubsetBuilder *builder,
                             VkrAllocator *allocator) {
  builder->vertices = vector_create_VkrVertex3d(allocator);
  builder->indices = vector_create_uint32_t(allocator);
  builder->name = vkr_string8_duplicate_cstr(allocator, "default");
  builder->material_name = (String8){0};
  builder->pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD;
  builder->shader_override = (String8){0};
}

vkr_internal VkrMeshLoaderMaterialDef *
vkr_mesh_loader_find_material(Vector_VkrMeshLoaderMaterialDef *materials,
                              const String8 *name) {
  assert_log(materials != NULL, "Materials is NULL");

  if (!name->str || name->length == 0)
    return NULL;

  for (uint64_t i = 0; i < materials->length; ++i) {
    VkrMeshLoaderMaterialDef *def =
        vector_get_VkrMeshLoaderMaterialDef(materials, i);
    if (string8_equalsi(&def->name, name))
      return def;
  }

  return NULL;
}

vkr_internal String8 vkr_mesh_loader_make_material_dir(VkrAllocator *allocator,
                                                       const String8 *stem) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(stem != NULL, "Stem is NULL");
  return string8_create_formatted(allocator, "assets/materials/%.*s",
                                  (int32_t)stem->length, stem->str);
}

vkr_internal String8 vkr_mesh_loader_make_material_path(
    VkrAllocator *allocator, const String8 *stem, const String8 *material) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(stem != NULL, "Stem is NULL");
  assert_log(material != NULL, "Material is NULL");

  return string8_create_formatted(allocator, "assets/materials/%.*s/%.*s.mt",
                                  (int32_t)stem->length, stem->str,
                                  (int32_t)material->length, material->str);
}

vkr_internal String8 vkr_mesh_loader_texture_basename(VkrAllocator *allocator,
                                                      const String8 *token) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token->str || token->length == 0)
    return (String8){0};

  uint64_t start = 0;
  for (uint64_t i = token->length; i > 0; --i) {
    uint8_t ch = token->str[i - 1];
    if (ch == '/' || ch == '\\') {
      start = i;
      break;
    }
  }

  String8 view = string8_substring(token, start, token->length);
  return string8_duplicate(allocator, &view);
}

vkr_internal String8 vkr_mesh_loader_texture_path(VkrAllocator *allocator,
                                                  const String8 *token) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token->str || token->length == 0)
    return (String8){0};

  String8 file = vkr_mesh_loader_texture_basename(allocator, token);
  if (!file.str)
    return (String8){0};

  return string8_create_formatted(allocator, "assets/textures/%.*s",
                                  (int32_t)file.length, file.str);
}

vkr_internal bool8_t vkr_mesh_loader_write_material_file(
    VkrMeshLoaderState *state, VkrMeshLoaderMaterialDef *material,
    String8 relative_path) {
  assert_log(state != NULL, "State is NULL");
  assert_log(material != NULL, "Material is NULL");
  assert_log(relative_path.str != NULL, "Relative path is NULL");

  FilePath dir_path =
      file_path_create(string8_cstr(&state->material_dir),
                       state->load_allocator, FILE_PATH_TYPE_RELATIVE);
  if (!file_ensure_directory(state->load_allocator, &dir_path.path)) {
    log_error("MeshLoader: failed to create material directory '%s'",
              string8_cstr(&state->material_dir));

    return false_v;
  }

  FilePath file_path =
      file_path_create((const char *)relative_path.str, state->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to open material file '%s': %s",
              file_path.path.str, file_get_error_string(ferr).str);
    return false_v;
  }

  String8 shader_value = (material->shader_name.length > 0)
                             ? material->shader_name
                             : DEFAULT_SHADER;

  String8 lines[] = {
      string8_create_formatted(state->load_allocator, "name=%.*s",
                               (int32_t)material->name.length,
                               material->name.str),
      string8_create_formatted(state->load_allocator, "diffuse_texture=%.*s",
                               (int32_t)material->diffuse_map.length,
                               material->diffuse_map.str),
      string8_create_formatted(state->load_allocator,
                               "diffuse_colorspace=srgb"),
      string8_create_formatted(
          state->load_allocator, "diffuse_color=%f,%f,%f,%f",
          material->diffuse_color.x, material->diffuse_color.y,
          material->diffuse_color.z, material->diffuse_color.w),
      string8_create_formatted(state->load_allocator, "specular_texture=%.*s",
                               (int32_t)material->specular_map.length,
                               material->specular_map.str),
      string8_create_formatted(state->load_allocator,
                               "specular_colorspace=linear"),
      string8_create_formatted(
          state->load_allocator, "specular_color=%f,%f,%f,%f",
          material->specular_color.x, material->specular_color.y,
          material->specular_color.z, material->specular_color.w),
      string8_create_formatted(state->load_allocator, "norm_texture=%.*s",
                               (int32_t)material->normal_map.length,
                               material->normal_map.str),
      string8_create_formatted(state->load_allocator,
                               "normal_colorspace=linear"),
      string8_create_formatted(state->load_allocator, "shininess=%f",
                               material->shininess),
      string8_create_formatted(state->load_allocator, "emission_color=%f,%f,%f",
                               material->emission_color.x,
                               material->emission_color.y,
                               material->emission_color.z),
      string8_create_formatted(state->load_allocator, "shader=%.*s",
                               (int32_t)shader_value.length,
                               (const char *)shader_value.str),
      string8_create_formatted(state->load_allocator, "pipeline=%s", "world"),
  };

  for (uint32_t i = 0; i < ArrayCount(lines); ++i) {
    if (lines[i].length == 0)
      continue;
    FileError werr = file_write_line(&fh, &lines[i]);
    if (werr != FILE_ERROR_NONE) {
      log_error("MeshLoader: failed to write material '%s': %s",
                file_path.path.str, file_get_error_string(werr).str);
      file_close(&fh);
      return false_v;
    }
  }

  file_close(&fh);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_resolve_material(
    VkrMeshLoaderState *state, const String8 *material_name, String8 *out_path,
    VkrMaterialHandle *out_handle) {
  assert_log(state != NULL, "State is NULL");

  if (!material_name->str || material_name->length == 0)
    return false_v;

  VkrMeshLoaderMaterialDef *mat =
      vkr_mesh_loader_find_material(&state->materials, material_name);
  if (!mat) {
    log_warn("MeshLoader: unknown material '%.*s'",
             (int32_t)material_name->length, material_name->str);
    return false_v;
  }

  if (!mat->generated_path.str) {
    mat->generated_path = vkr_mesh_loader_make_material_path(
        state->load_allocator, &state->obj_stem, material_name);
  }

  if (!mat->generated) {
    if (!vkr_mesh_loader_write_material_file(state, mat, mat->generated_path))
      return false_v;
    mat->generated = true_v;
  }

  if (out_path)
    *out_path = mat->generated_path;
  if (out_handle)
    *out_handle = VKR_MATERIAL_HANDLE_INVALID;

  return true_v;
}

vkr_internal void vkr_mesh_loader_compute_bounds(const VkrVertex3d *vertices,
                                                 uint32_t count, Vec3 *out_min,
                                                 Vec3 *out_max,
                                                 Vec3 *out_center) {
  Vec3 min = vec3_new(FLT_MAX, FLT_MAX, FLT_MAX);
  Vec3 max = vec3_new(-FLT_MAX, -FLT_MAX, -FLT_MAX);

  for (uint32_t i = 0; i < count; ++i) {
    Vec3 pos = vertices[i].position;
    min.x = vkr_min_f32(min.x, pos.x);
    min.y = vkr_min_f32(min.y, pos.y);
    min.z = vkr_min_f32(min.z, pos.z);
    max.x = vkr_max_f32(max.x, pos.x);
    max.y = vkr_max_f32(max.y, pos.y);
    max.z = vkr_max_f32(max.z, pos.z);
  }

  *out_min = min;
  *out_max = max;
  *out_center = vec3_scale(vec3_add(min, max), 0.5f);
}

vkr_internal bool8_t
vkr_mesh_loader_finalize_subset(VkrMeshLoaderState *state) {
  assert_log(state != NULL, "State is NULL");

  VkrMeshLoaderSubsetBuilder *builder = &state->builder;
  if (builder->indices.length == 0 || builder->vertices.length == 0) {
    vector_clear_VkrVertex3d(&builder->vertices);
    vector_clear_uint32_t(&builder->indices);
    return true_v;
  }

  VkrAllocatorScope temp_scope =
      vkr_allocator_begin_scope(state->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    log_error("MeshLoader: failed to acquire temporary scope");
    return false_v;
  }
  uint32_t index_count = (uint32_t)builder->indices.length;
  uint32_t vertex_count = (uint32_t)builder->vertices.length;

  VkrVertex3d *dedup_vertices = NULL;
  uint32_t dedup_vertex_count = 0;
  uint32_t *indices_copy = vkr_allocator_alloc(
      state->scratch_allocator, (uint64_t)index_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(indices_copy != NULL, "MeshLoader: failed to allocate indices");
  MemCopy(indices_copy, builder->indices.data,
          (uint64_t)builder->indices.length * sizeof(uint32_t));

  if (!vkr_geometry_system_deduplicate_vertices(
          state->context->geometry_system, state->scratch_allocator,
          builder->vertices.data, vertex_count, indices_copy, index_count,
          &dedup_vertices, &dedup_vertex_count)) {
    vkr_allocator_free(state->scratch_allocator, indices_copy,
                       (uint64_t)index_count * sizeof(uint32_t),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    log_error("MeshLoader: deduplication failed for subset");
    return false_v;
  }

  uint64_t vertex_bytes = (uint64_t)dedup_vertex_count * sizeof(VkrVertex3d);
  uint64_t index_bytes = (uint64_t)index_count * sizeof(uint32_t);
  VkrVertex3d *vertex_copy = vkr_allocator_alloc(
      state->load_allocator, vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *index_copy = vkr_allocator_alloc(state->load_allocator, index_bytes,
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(vertex_copy && index_copy,
             "MeshLoader: failed to allocate geometry buffers");

  MemCopy(vertex_copy, dedup_vertices, vertex_bytes);
  MemCopy(index_copy, indices_copy, index_bytes);

  vkr_geometry_system_generate_tangents(state->load_allocator, vertex_copy,
                                        dedup_vertex_count, index_copy,
                                        index_count);

  Vec3 min, max, center;
  vkr_mesh_loader_compute_bounds(vertex_copy, dedup_vertex_count, &min, &max,
                                 &center);

  VkrMeshLoaderSubset subset = {0};
  subset.geometry_config.vertex_size = sizeof(VkrVertex3d);
  subset.geometry_config.vertex_count = dedup_vertex_count;
  subset.geometry_config.vertices = vertex_copy;
  subset.geometry_config.index_size = sizeof(uint32_t);
  subset.geometry_config.index_count = index_count;
  subset.geometry_config.indices = index_copy;
  subset.geometry_config.center = center;
  subset.geometry_config.min_extents = min;
  subset.geometry_config.max_extents = max;

  if (builder->name.str) {
    uint64_t copy_len =
        Min((uint64_t)GEOMETRY_NAME_MAX_LENGTH - 1, builder->name.length);
    MemZero(subset.geometry_config.name, GEOMETRY_NAME_MAX_LENGTH);
    MemCopy(subset.geometry_config.name, builder->name.str, copy_len);
  }

  String8 material_path = {0};
  VkrMaterialHandle mat_handle = VKR_MATERIAL_HANDLE_INVALID;
  if (builder->material_name.str) {
    vkr_mesh_loader_resolve_material(state, &builder->material_name,
                                     &material_path, &mat_handle);
    if (material_path.str) {
      uint64_t copy_len =
          Min((uint64_t)MATERIAL_NAME_MAX_LENGTH - 1, material_path.length);
      MemZero(subset.geometry_config.material_name, MATERIAL_NAME_MAX_LENGTH);
      MemCopy(subset.geometry_config.material_name, material_path.str,
              copy_len);
    }
  }

  subset.material_name =
      string8_duplicate(state->load_allocator, &material_path);
  subset.material_handle = mat_handle;
  subset.pipeline_domain = builder->pipeline_domain;
  subset.shader_override =
      string8_duplicate(state->load_allocator, &builder->shader_override);

  vector_push_VkrMeshLoaderSubset(&state->subsets, subset);

  vkr_allocator_free(state->scratch_allocator, indices_copy,
                     (uint64_t)index_count * sizeof(uint32_t),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  vector_clear_VkrVertex3d(&builder->vertices);
  vector_clear_uint32_t(&builder->indices);
  return true_v;
}

vkr_internal void vkr_mesh_loader_push_face(VkrMeshLoaderState *state,
                                            uint32_t token_count,
                                            String8 *tokens) {
  assert_log(state != NULL, "State is NULL");
  assert_log(tokens != NULL, "Tokens is NULL");

  if (token_count < 3)
    return;

  uint32_t first_index = (uint32_t)state->builder.vertices.length;

  for (uint32_t i = 0; i < token_count; ++i) {
    VkrMeshLoaderVertexRef ref = vkr_mesh_loader_parse_vertex_ref(&tokens[i]);
    uint32_t pos_idx = vkr_mesh_loader_fix_index(
        ref.position, (uint32_t)state->positions.length);
    uint32_t tex_idx = vkr_mesh_loader_fix_index(
        ref.texcoord, (uint32_t)state->texcoords.length);
    uint32_t norm_idx =
        vkr_mesh_loader_fix_index(ref.normal, (uint32_t)state->normals.length);

    VkrVertex3d vert = {0};
    vert.position = (pos_idx < state->positions.length)
                        ? *vector_get_Vec3(&state->positions, pos_idx)
                        : vec3_zero();
    vert.texcoord = (tex_idx < state->texcoords.length)
                        ? *vector_get_Vec2(&state->texcoords, tex_idx)
                        : vec2_zero();
    vert.normal = (norm_idx < state->normals.length)
                      ? *vector_get_Vec3(&state->normals, norm_idx)
                      : vec3_new(0.0f, 1.0f, 0.0f);
    vert.colour = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
    vert.tangent = vec4_zero();

    vector_push_VkrVertex3d(&state->builder.vertices, vert);
  }

  for (uint32_t tri = 0; tri < token_count - 2; ++tri) {
    vector_push_uint32_t(&state->builder.indices, first_index);
    vector_push_uint32_t(&state->builder.indices, first_index + tri + 1);
    vector_push_uint32_t(&state->builder.indices, first_index + tri + 2);
  }
}

vkr_internal void vkr_mesh_loader_parse_next_line(String8 *file_str,
                                                  uint64_t *offset,
                                                  String8 *out_line) {
  uint64_t line_end = *offset;
  while (line_end < file_str->length && file_str->str[line_end] != '\n' &&
         file_str->str[line_end] != '\r') {
    line_end++;
  }
  *out_line = string8_substring(file_str, *offset, line_end);
  *offset = line_end;
  while (*offset < file_str->length &&
         (file_str->str[*offset] == '\n' || file_str->str[*offset] == '\r')) {
    (*offset)++;
  }
  string8_trim(out_line);
}

vkr_internal bool8_t vkr_mesh_loader_parse_vec3_line(String8 *line,
                                                     uint32_t prefix_len,
                                                     Vec3 *out_vec) {
  String8 coords = vkr_string8_trimmed_suffix(line, prefix_len);
  String8 tokens[3];
  uint32_t count = string8_split_whitespace(&coords, tokens, 3);
  if (count < 3)
    return false_v;

  float32_t x = 0, y = 0, z = 0;
  string8_to_f32(&tokens[0], &x);
  string8_to_f32(&tokens[1], &y);
  string8_to_f32(&tokens[2], &z);
  *out_vec = vec3_new(x, y, z);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_mtl(VkrMeshLoaderState *state,
                                               String8 rel_path) {
  assert_log(state != NULL, "State is NULL");

  if (!rel_path.str || rel_path.length == 0)
    return false_v;

  String8 full_path =
      file_path_join(state->load_allocator, state->obj_dir, rel_path);
  String8 file_str = {0};
  if (!vkr_mesh_loader_read_file_to_string(state->load_allocator, full_path,
                                           &file_str, NULL))
    return false_v;

  VkrMeshLoaderMaterialDef *current = NULL;
  uint64_t offset = 0;
  while (offset < file_str.length) {
    String8 line = {0};
    vkr_mesh_loader_parse_next_line(&file_str, &offset, &line);

    if (line.length == 0 || line.str[0] == '#')
      continue;

    if (vkr_string8_starts_with(&line, "newmtl")) {
      String8 mat_name = vkr_string8_trimmed_suffix(&line, 6);
      VkrMeshLoaderMaterialDef def = {
          .name = string8_duplicate(state->load_allocator, &mat_name),
          .shader_name = DEFAULT_SHADER,
          .diffuse_color = vec4_new(1, 1, 1, 1),
          .specular_color = vec4_new(1, 1, 1, 1),
          .emission_color = vec3_new(0, 0, 0),
          .shininess = 8.0f,
      };
      vector_push_VkrMeshLoaderMaterialDef(&state->materials, def);
      current = vector_get_VkrMeshLoaderMaterialDef(
          &state->materials, state->materials.length - 1);
      continue;
    }

    if (!current)
      continue;

    Vec3 vec3_val;
    if (vkr_string8_starts_with(&line, "Kd")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->diffuse_color.x = vec3_val.x;
        current->diffuse_color.y = vec3_val.y;
        current->diffuse_color.z = vec3_val.z;
      }
    } else if (vkr_string8_starts_with(&line, "Ke")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->emission_color = vec3_val;
      }
    } else if (vkr_string8_starts_with(&line, "Ks")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val)) {
        current->specular_color.x = vec3_val.x;
        current->specular_color.y = vec3_val.y;
        current->specular_color.z = vec3_val.z;
      }
    } else if (vkr_string8_starts_with(&line, "Ns")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 2);
      float32_t shininess = 0.0f;
      string8_to_f32(&value, &shininess);
      if (shininess > 0.0f)
        current->shininess = shininess;
    } else if (vkr_string8_starts_with(&line, "map_Kd")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->diffuse_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "map_Ks")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->specular_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "map_bump") ||
               vkr_string8_starts_with(&line, "bump")) {
      uint64_t idx = vkr_string8_starts_with(&line, "map_bump") ? 8 : 4;
      String8 value = vkr_string8_trimmed_suffix(&line, idx);
      current->normal_map =
          vkr_mesh_loader_texture_path(state->load_allocator, &value);
    } else if (vkr_string8_starts_with(&line, "shader")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->shader_name = string8_duplicate(state->load_allocator, &value);
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_obj(VkrMeshLoaderState *state) {
  String8 file_str = {0};
  if (!vkr_mesh_loader_read_file_to_string(state->load_allocator,
                                           state->obj_path, &file_str,
                                           state->out_error))
    return false_v;

  uint64_t offset = 0;
  while (offset < file_str.length) {
    String8 line = {0};
    vkr_mesh_loader_parse_next_line(&file_str, &offset, &line);

    if (line.length == 0 || line.str[0] == '#')
      continue;

    Vec3 vec3_val;
    if (vkr_string8_starts_with(&line, "v ")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 1, &vec3_val))
        vector_push_Vec3(&state->positions, vec3_val);
      continue;
    }

    if (vkr_string8_starts_with(&line, "vn")) {
      if (vkr_mesh_loader_parse_vec3_line(&line, 2, &vec3_val))
        vector_push_Vec3(&state->normals, vec3_val);
      continue;
    }

    if (vkr_string8_starts_with(&line, "vt")) {
      String8 coords = vkr_string8_trimmed_suffix(&line, 2);
      String8 tokens[2];
      if (string8_split_whitespace(&coords, tokens, 2) >= 2) {
        float32_t u = 0, v = 0;
        string8_to_f32(&tokens[0], &u);
        string8_to_f32(&tokens[1], &v);
        vector_push_Vec2(&state->texcoords, vec2_new(u, v));
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "mtllib")) {
      String8 path = vkr_string8_trimmed_suffix(&line, 6);
      vkr_mesh_loader_parse_mtl(state, path);
      continue;
    }

    if (vkr_string8_starts_with(&line, "usemtl")) {
      vkr_mesh_loader_finalize_subset(state);
      String8 material_name = vkr_string8_trimmed_suffix(&line, 6);
      state->builder.material_name =
          string8_duplicate(state->load_allocator, &material_name);
      continue;
    }

    if (vkr_string8_starts_with(&line, "o ") ||
        vkr_string8_starts_with(&line, "g ")) {
      vkr_mesh_loader_finalize_subset(state);
      String8 name = vkr_string8_trimmed_suffix(&line, 1);
      state->builder.name = string8_duplicate(state->load_allocator, &name);
      continue;
    }

    if (vkr_string8_starts_with(&line, "f ")) {
      String8 face = vkr_string8_trimmed_suffix(&line, 1);
      String8 tokens[64];
      uint32_t count = string8_split_whitespace(&face, tokens, 64);
      if (count >= 3)
        vkr_mesh_loader_push_face(state, count, tokens);
      continue;
    }
  }

  return vkr_mesh_loader_finalize_subset(state);
}

vkr_internal bool8_t vkr_mesh_loader_can_load(VkrResourceLoader *self,
                                              String8 name) {
  (void)self;
  if (!name.str || name.length == 0)
    return false_v;

  for (uint64_t i = name.length; i > 0; --i) {
    if (name.str[i - 1] == '.') {
      String8 ext = string8_substring(&name, i, name.length);
      String8 obj_ext = string8_lit("obj");
      return string8_equalsi(&ext, &obj_ext);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_mesh_loader_read_binary_no_materials(
    VkrMeshLoaderState *state, String8 cache_path) {
  assert_log(state != NULL, "State is NULL");

  if (!cache_path.str || cache_path.length == 0)
    return false_v;

  FilePath file_path =
      file_path_create((const char *)cache_path.str, state->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  if (!file_exists(&file_path))
    return false_v;

  uint8_t *data = NULL;
  uint64_t size = 0;

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE)
    return false_v;

  FileError read_err = file_read_all(&fh, state->load_allocator, &data, &size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !data || size == 0)
    return false_v;

  VkrMeshLoaderBinaryReader reader = {.ptr = data, .end = data + size};

  uint32_t magic = 0, version = 0;
  if (!vkr_mesh_loader_read_u32(&reader, &magic) ||
      !vkr_mesh_loader_read_u32(&reader, &version)) {
    return false_v;
  }

  if (magic != VKR_MESH_CACHE_MAGIC || version != VKR_MESH_CACHE_VERSION) {
    return false_v;
  }

  String8 cached_name = {0};
  if (!vkr_mesh_loader_read_string(&reader, state->load_allocator,
                                   &cached_name)) {
    return false_v;
  }

  if (!string8_equalsi(&cached_name, &state->obj_path)) {
    return false_v;
  }

  uint32_t subset_count = 0;
  if (!vkr_mesh_loader_read_u32(&reader, &subset_count) || subset_count == 0) {
    return false_v;
  }

  for (uint32_t i = 0; i < subset_count; ++i) {
    String8 subset_name = {0};
    String8 material_path = {0};
    String8 shader_override = {0};
    uint32_t pipeline_domain = 0;
    uint32_t vertex_stride = 0;
    uint32_t vertex_count = 0;
    uint32_t index_size = 0;
    uint32_t index_count = 0;
    Vec3 center = vec3_zero();
    Vec3 min_extents = vec3_zero();
    Vec3 max_extents = vec3_zero();

    if (!vkr_mesh_loader_read_string(&reader, state->load_allocator,
                                     &subset_name) ||
        !vkr_mesh_loader_read_string(&reader, state->load_allocator,
                                     &material_path) ||
        !vkr_mesh_loader_read_string(&reader, state->load_allocator,
                                     &shader_override) ||
        !vkr_mesh_loader_read_u32(&reader, &pipeline_domain) ||
        !vkr_mesh_loader_read_u32(&reader, &vertex_stride) ||
        !vkr_mesh_loader_read_u32(&reader, &vertex_count) ||
        !vkr_mesh_loader_read_u32(&reader, &index_size) ||
        !vkr_mesh_loader_read_u32(&reader, &index_count) ||
        !vkr_mesh_loader_read_vec3(&reader, &center) ||
        !vkr_mesh_loader_read_vec3(&reader, &min_extents) ||
        !vkr_mesh_loader_read_vec3(&reader, &max_extents)) {
      return false_v;
    }

    if (vertex_stride != sizeof(VkrVertex3d) ||
        index_size != sizeof(uint32_t) || vertex_count == 0 ||
        index_count == 0) {
      return false_v;
    }

    uint64_t vertex_bytes = (uint64_t)vertex_stride * (uint64_t)vertex_count;
    uint64_t index_bytes = (uint64_t)index_size * (uint64_t)index_count;

    if (reader.ptr + vertex_bytes + index_bytes > reader.end) {
      return false_v;
    }

    VkrVertex3d *vertices = vkr_allocator_alloc(
        state->load_allocator, vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *indices = vkr_allocator_alloc(state->load_allocator, index_bytes,
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!vertices || !indices) {
      return false_v;
    }

    if (!vkr_mesh_loader_read_bytes(&reader, vertex_bytes, vertices) ||
        !vkr_mesh_loader_read_bytes(&reader, index_bytes, indices)) {
      return false_v;
    }

    VkrMeshLoaderSubset subset = {0};
    subset.geometry_config.vertex_size = vertex_stride;
    subset.geometry_config.vertex_count = vertex_count;
    subset.geometry_config.vertices = vertices;
    subset.geometry_config.index_size = index_size;
    subset.geometry_config.index_count = index_count;
    subset.geometry_config.indices = indices;
    subset.geometry_config.center = center;
    subset.geometry_config.min_extents = min_extents;
    subset.geometry_config.max_extents = max_extents;

    if (subset_name.str && subset_name.length > 0) {
      uint64_t copy_len =
          vkr_min_u64(subset_name.length, GEOMETRY_NAME_MAX_LENGTH - 1);
      MemZero(subset.geometry_config.name, GEOMETRY_NAME_MAX_LENGTH);
      MemCopy(subset.geometry_config.name, subset_name.str, copy_len);
    }

    if (material_path.str && material_path.length > 0) {
      uint64_t copy_len =
          vkr_min_u64(material_path.length, MATERIAL_NAME_MAX_LENGTH - 1);
      MemZero(subset.geometry_config.material_name, MATERIAL_NAME_MAX_LENGTH);
      MemCopy(subset.geometry_config.material_name, material_path.str,
              copy_len);
    }

    subset.material_name = material_path;
    subset.pipeline_domain = (VkrPipelineDomain)pipeline_domain;
    subset.shader_override =
        shader_override.length > 0
            ? string8_duplicate(state->load_allocator, &shader_override)
            : (String8){0};
    subset.material_handle = VKR_MATERIAL_HANDLE_INVALID;

    vector_push_VkrMeshLoaderSubset(&state->subsets, subset);
  }

  log_debug("Read cache '%s' (%u subsets)", file_path.path.str, subset_count);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_load_job_run(VkrJobContext *ctx, void *payload) {
  VkrMeshLoadJobPayload *job = (VkrMeshLoadJobPayload *)payload;

  *job->success = false_v;
  *job->error = VKR_RENDERER_ERROR_NONE;

  VkrAllocator *job_scratch = ctx->allocator;
  if (!job_scratch) {
    log_error("MeshLoader: job context allocator is NULL");
    *job->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrMeshLoaderState state = vkr_mesh_loader_state_create(
      job->context, job->result_allocator, job_scratch, job_scratch,
      job->mesh_path, job->error);

  String8 cache_path = vkr_mesh_loader_cache_path(&state);
  bool8_t loaded_from_cache = false_v;

  if (cache_path.str)
    loaded_from_cache =
        vkr_mesh_loader_read_binary_no_materials(&state, cache_path);

  if (!loaded_from_cache) {
    if (!vkr_mesh_loader_parse_obj(&state))
      return false_v;

    if (cache_path.str)
      vkr_mesh_loader_write_binary(&state, cache_path);
  }

  if (state.subsets.length == 0) {
    *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  Array_VkrMeshLoaderSubset subset_array = array_create_VkrMeshLoaderSubset(
      job->result_allocator, state.subsets.length);
  for (uint64_t i = 0; i < state.subsets.length; ++i) {
    array_set_VkrMeshLoaderSubset(&subset_array, i, state.subsets.data[i]);
  }

  job->result->source_path =
      string8_duplicate(job->result_allocator, &job->mesh_path);
  job->result->root_transform = vkr_transform_identity();
  job->result->subsets = subset_array;

  *job->success = true_v;
  return true_v;
}

vkr_internal void vkr_mesh_loader_cleanup_arenas(
    VkrMeshLoaderResult **results, Arena **arenas, void **pool_chunks,
    uint32_t count, VkrArenaPool *arena_pool) {
  for (uint32_t i = 0; i < count; i++) {
    if (results[i])
      vkr_allocator_release_global_accounting(&results[i]->allocator);
    if (arenas[i])
      arena_destroy(arenas[i]);
    if (pool_chunks[i] && arena_pool)
      vkr_arena_pool_release(arena_pool, pool_chunks[i]);
  }
}

vkr_internal void vkr_mesh_loader_set_all_errors(VkrMeshBatchResult *results,
                                                 uint32_t count,
                                                 VkrRendererError error) {
  for (uint32_t i = 0; i < count; i++)
    results[i].error = error;
}

vkr_internal uint32_t vkr_mesh_loader_load_batch(
    VkrMeshLoaderContext *context, const String8 *mesh_paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrMeshBatchResult *out_results) {
  assert_log(context != NULL, "Context is NULL");
  assert_log(mesh_paths != NULL, "Mesh paths is NULL");
  assert_log(out_results != NULL, "Out results is NULL");

  if (count == 0)
    return 0;

  for (uint32_t i = 0; i < count; i++) {
    out_results[i].result = NULL;
    out_results[i].error = VKR_RENDERER_ERROR_NONE;
    out_results[i].success = false_v;
  }

  VkrJobSystem *job_sys = context->job_system;
  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_results[i].error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // Allocate per-mesh structures
  VkrMeshLoaderResult **results = (VkrMeshLoaderResult **)vkr_allocator_alloc(
      temp_alloc, sizeof(VkrMeshLoaderResult *) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  Arena **result_arenas = (Arena **)vkr_allocator_alloc(
      temp_alloc, sizeof(Arena *) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  void **pool_chunks = (void **)vkr_allocator_alloc(
      temp_alloc, sizeof(void *) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrAllocator *result_allocators = vkr_allocator_alloc(
      temp_alloc, sizeof(VkrAllocator) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrRendererError *errors =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrRendererError) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *success = vkr_allocator_alloc(temp_alloc, sizeof(bool8_t) * count,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrJobHandle *job_handles = vkr_allocator_alloc(
      temp_alloc, sizeof(VkrJobHandle) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshLoadJobPayload *payloads =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMeshLoadJobPayload) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *job_submitted = vkr_allocator_alloc(
      temp_alloc, sizeof(bool8_t) * count, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!results || !result_arenas || !pool_chunks || !result_allocators ||
      !errors || !success || !job_handles || !payloads || !job_submitted) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_set_all_errors(out_results, count,
                                   VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return 0;
  }

  VkrArenaPool *arena_pool = context->arena_pool;
  if (!arena_pool || !arena_pool->initialized) {
    log_error("Mesh loader requires arena_pool to be initialized");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_set_all_errors(out_results, count,
                                   VKR_RENDERER_ERROR_INITIALIZATION_FAILED);
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    pool_chunks[i] = vkr_arena_pool_acquire(arena_pool);
    if (!pool_chunks[i]) {
      log_error("Arena pool exhausted at mesh %u/%u", i, count);
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    result_arenas[i] =
        arena_create_from_buffer(pool_chunks[i], arena_pool->chunk_size);
    if (!result_arenas[i]) {
      vkr_arena_pool_release(arena_pool, pool_chunks[i]);
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    result_allocators[i].ctx = result_arenas[i];
    vkr_allocator_arena(&result_allocators[i]);

    results[i] =
        vkr_allocator_alloc(&result_allocators[i], sizeof(VkrMeshLoaderResult),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!results[i]) {
      vkr_mesh_loader_cleanup_arenas(results, result_arenas, pool_chunks, i + 1,
                                     arena_pool);
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    MemZero(results[i], sizeof(VkrMeshLoaderResult));
    results[i]->arena = result_arenas[i];
    results[i]->pool_chunk = pool_chunks[i];
    results[i]->allocator = result_allocators[i];

    errors[i] = VKR_RENDERER_ERROR_NONE;
    success[i] = false_v;
    job_submitted[i] = false_v;
  }

  if (job_sys) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);

    for (uint32_t i = 0; i < count; i++) {
      if (!mesh_paths[i].str || mesh_paths[i].length == 0)
        continue;

      payloads[i] = (VkrMeshLoadJobPayload){
          .mesh_path = mesh_paths[i],
          .context = context,
          .result_allocator = &results[i]->allocator,
          .result = results[i],
          .error = &errors[i],
          .success = &success[i],
      };

      VkrJobDesc job_desc = {
          .priority = VKR_JOB_PRIORITY_NORMAL,
          .type_mask = type_mask,
          .run = vkr_mesh_load_job_run,
          .on_success = NULL,
          .on_failure = NULL,
          .payload = &payloads[i],
          .payload_size = sizeof(VkrMeshLoadJobPayload),
          .dependencies = NULL,
          .dependency_count = 0,
          .defer_enqueue = false_v,
      };

      if (vkr_job_submit(job_sys, &job_desc, &job_handles[i]))
        job_submitted[i] = true_v;
    }

    for (uint32_t i = 0; i < count; i++) {
      if (job_submitted[i])
        vkr_job_wait(job_sys, job_handles[i]);
    }
  } else {
    for (uint32_t i = 0; i < count; i++) {
      if (!mesh_paths[i].str || mesh_paths[i].length == 0)
        continue;

      payloads[i] = (VkrMeshLoadJobPayload){
          .mesh_path = mesh_paths[i],
          .context = context,
          .result_allocator = &results[i]->allocator,
          .result = results[i],
          .error = &errors[i],
          .success = &success[i],
      };

      VkrJobContext fake_ctx = {.system = NULL,
                                .worker_index = 0,
                                .thread_id = 0,
                                .allocator = temp_alloc,
                                .scope = temp_scope};
      vkr_mesh_load_job_run(&fake_ctx, &payloads[i]);
    }
  }

  uint32_t total_materials = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!success[i] || !results[i])
      continue;

    for (uint64_t j = 0; j < results[i]->subsets.length; j++) {
      VkrMeshLoaderSubset *subset = &results[i]->subsets.data[j];
      if (subset->material_name.str && subset->material_name.length > 0)
        total_materials++;
    }
  }

  log_debug("Mesh batch: %u meshes loaded, %u total materials to load", count,
            total_materials);

  if (total_materials > 0) {
    String8 *all_material_paths =
        vkr_allocator_alloc(temp_alloc, sizeof(String8) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrMaterialHandle *all_material_handles = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrMaterialHandle) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    VkrRendererError *all_material_errors = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrRendererError) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *material_mesh_index =
        vkr_allocator_alloc(temp_alloc, sizeof(uint32_t) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *material_subset_index =
        vkr_allocator_alloc(temp_alloc, sizeof(uint32_t) * total_materials,
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    if (!all_material_paths || !all_material_handles || !all_material_errors ||
        !material_mesh_index || !material_subset_index) {
      for (uint32_t i = 0; i < count; i++) {
        if (success[i] && results[i]) {
          out_results[i].result = results[i];
          out_results[i].error = VKR_RENDERER_ERROR_NONE;
          out_results[i].success = true_v;
        } else {
          out_results[i].error = errors[i];
        }
      }
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return count;
    }

    uint32_t mat_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (!success[i] || !results[i])
        continue;

      for (uint64_t j = 0; j < results[i]->subsets.length; j++) {
        VkrMeshLoaderSubset *subset = &results[i]->subsets.data[j];
        if (subset->material_name.str && subset->material_name.length > 0) {
          all_material_paths[mat_idx] = subset->material_name;
          material_mesh_index[mat_idx] = i;
          material_subset_index[mat_idx] = (uint32_t)j;
          mat_idx++;
        }
      }
    }

    VkrResourceHandleInfo *material_handle_infos = vkr_allocator_alloc(
        temp_alloc, sizeof(VkrResourceHandleInfo) * total_materials,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!material_handle_infos) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_set_all_errors(out_results, count,
                                     VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return 0;
    }

    uint32_t materials_loaded = vkr_resource_system_load_batch(
        VKR_RESOURCE_TYPE_MATERIAL, all_material_paths, total_materials,
        temp_alloc, material_handle_infos, all_material_errors);

    log_debug("Mesh batch: %u/%u materials loaded", materials_loaded,
              total_materials);

    for (uint32_t m = 0; m < total_materials; m++) {
      VkrMaterialHandle mat_handle = VKR_MATERIAL_HANDLE_INVALID;
      if (material_handle_infos[m].type == VKR_RESOURCE_TYPE_MATERIAL) {
        mat_handle = material_handle_infos[m].as.material;
        all_material_handles[m] = mat_handle;
      } else {
        all_material_handles[m] = VKR_MATERIAL_HANDLE_INVALID;
      }

      if (mat_handle.id != 0) {
        uint32_t mesh_idx = material_mesh_index[m];
        uint32_t subset_idx = material_subset_index[m];
        VkrMeshLoaderSubset *subset =
            &results[mesh_idx]->subsets.data[subset_idx];
        subset->material_handle = mat_handle;
        vkr_material_system_add_ref(context->material_system, mat_handle);
      }
    }
  }

  uint32_t loaded_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (success[i] && results[i]) {
      out_results[i].result = results[i];
      out_results[i].error = VKR_RENDERER_ERROR_NONE;
      out_results[i].success = true_v;
      loaded_count++;
    } else {
      out_results[i].error = errors[i];
      out_results[i].success = false_v;
    }
  }

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  log_debug("Mesh batch complete: %u/%u meshes loaded successfully",
            loaded_count, count);
  return loaded_count;
}

vkr_internal bool8_t vkr_mesh_loader_load(VkrResourceLoader *self, String8 name,
                                          VkrAllocator *temp_alloc,
                                          VkrResourceHandleInfo *out_handle,
                                          VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;

  VkrMeshBatchResult batch_result = {0};
  uint32_t loaded =
      vkr_mesh_loader_load_batch(context, &name, 1, temp_alloc, &batch_result);

  if (loaded == 0 || !batch_result.success) {
    *out_error = batch_result.error != VKR_RENDERER_ERROR_NONE
                     ? batch_result.error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = batch_result.result;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal void vkr_mesh_loader_unload(VkrResourceLoader *self,
                                         const VkrResourceHandleInfo *handle,
                                         String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  (void)name;

  if (handle->type != VKR_RESOURCE_TYPE_MESH) {
    log_warn("MeshLoader: attempted to unload non-mesh resource");
    return;
  }

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  VkrMeshLoaderResult *result = handle->as.mesh;

  if (!result)
    return;

  if (result->subsets.data && context->material_system) {
    for (uint64_t i = 0; i < result->subsets.length; i++) {
      VkrMeshLoaderSubset *subset = &result->subsets.data[i];
      if (subset->material_handle.id != 0)
        vkr_material_system_release(context->material_system,
                                    subset->material_handle);
    }
  }

  if (result->arena) {
    vkr_allocator_release_global_accounting(&result->allocator);
    arena_destroy(result->arena);
  }

  if (result->pool_chunk)
    vkr_arena_pool_release(context->arena_pool, result->pool_chunk);
}

vkr_internal uint32_t vkr_mesh_loader_batch_load(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;

  VkrMeshBatchResult *batch_results =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMeshBatchResult) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!batch_results) {
    for (uint32_t i = 0; i < count; i++)
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return 0;
  }

  uint32_t loaded = vkr_mesh_loader_load_batch(context, paths, count,
                                               temp_alloc, batch_results);

  for (uint32_t i = 0; i < count; i++) {
    if (batch_results[i].success && batch_results[i].result) {
      out_handles[i].type = VKR_RESOURCE_TYPE_MESH;
      out_handles[i].loader_id = self->id;
      out_handles[i].as.mesh = batch_results[i].result;
      out_errors[i] = VKR_RENDERER_ERROR_NONE;
    } else {
      out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
      out_handles[i].loader_id = VKR_INVALID_ID;
      out_errors[i] = batch_results[i].error;
    }
  }

  return loaded;
}

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MESH;
  loader.resource_system = context;
  loader.can_load = vkr_mesh_loader_can_load;
  loader.load = vkr_mesh_loader_load;
  loader.unload = vkr_mesh_loader_unload;
  loader.batch_load = vkr_mesh_loader_batch_load;
  return loader;
}
