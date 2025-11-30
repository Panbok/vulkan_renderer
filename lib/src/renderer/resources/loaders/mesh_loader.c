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
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/systems/vkr_resource_system.h"

// Portable little-endian conversion functions
vkr_internal uint32_t vkr_host_to_little_u32(uint32_t value) {
  // Check if system is little-endian
  const union {
    uint32_t u32;
    uint8_t u8[4];
  } endian_check = {0x01020304};
  const bool8_t is_little_endian = (endian_check.u8[0] == 0x04);

  if (is_little_endian) {
    return value;
  } else {
    // Byte swap for big-endian systems
    return ((value & 0xFF000000) >> 24) | ((value & 0x00FF0000) >> 8) |
           ((value & 0x0000FF00) << 8) | ((value & 0x000000FF) << 24);
  }
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
  Arena *load_arena;
  Arena *temp_arena;
  Arena *scratch_arena;
  VkrAllocator allocator;

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

// Job payload for async file reading
typedef struct VkrMeshFileReadResult {
  uint8_t *data;
  uint64_t size;
  bool8_t success;
} VkrMeshFileReadResult;

typedef struct VkrMeshFileReadJobPayload {
  String8 file_path;
  Arena *arena;
  VkrMeshFileReadResult *result;
} VkrMeshFileReadJobPayload;

// Batch material loading structures
typedef struct VkrMeshPendingMaterial {
  String8 material_path;
  VkrMaterialHandle handle;
} VkrMeshPendingMaterial;
Vector(VkrMeshPendingMaterial);

vkr_internal void
vkr_mesh_loader_builder_init(VkrMeshLoaderSubsetBuilder *builder, Arena *arena);
vkr_internal String8 vkr_mesh_loader_make_material_dir(Arena *arena,
                                                       const String8 *stem);

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
  if (value.length > UINT32_MAX) {
    return false_v;
  }
  if (!vkr_mesh_loader_write_u32(fh, (uint32_t)value.length)) {
    return false_v;
  }
  if (value.length == 0 || !value.str) {
    return true_v;
  }
  return vkr_mesh_loader_write_bytes(fh, value.str, value.length);
}

vkr_internal bool8_t vkr_mesh_loader_read_bytes(
    VkrMeshLoaderBinaryReader *reader, uint64_t size, void *out) {
  if (!reader || reader->ptr + size > reader->end)
    return false_v;
  if (out) {
    MemCopy(out, reader->ptr, size);
  }
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
    VkrMeshLoaderBinaryReader *reader, Arena *arena, String8 *out) {
  uint32_t len = 0;
  if (!vkr_mesh_loader_read_u32(reader, &len))
    return false_v;
  if (reader->ptr + len > reader->end)
    return false_v;
  String8 view = {
      .str = reader->ptr,
      .length = len,
  };
  reader->ptr += len;
  if (out) {
    *out = string8_duplicate(arena, &view);
  }
  return true_v;
}

vkr_internal String8 vkr_mesh_loader_cache_path(VkrMeshLoaderState *state) {
  assert_log(state != NULL, "State is NULL");
  String8 cache_file = string8_create_formatted(
      state->load_arena, "%.*s.%s", (int32_t)state->obj_stem.length,
      state->obj_stem.str, VKR_MESH_CACHE_EXT);
  return file_path_join(state->load_arena, state->obj_dir, cache_file);
}

vkr_internal VkrMeshLoaderState vkr_mesh_loader_state_create(
    VkrMeshLoaderContext *context, Arena *load_arena, Arena *temp_arena,
    Arena *scratch_arena, String8 name, VkrRendererError *out_error) {
  // Create a per-state allocator using the load_arena for thread-safety
  // (each job has its own load_arena, avoiding race conditions)
  VkrAllocator state_allocator = {.ctx = load_arena};
  vkr_allocator_arena(&state_allocator);

  VkrMeshLoaderState state = {
      .context = context,
      .load_arena = load_arena,
      .temp_arena = temp_arena,
      .scratch_arena = scratch_arena,
      .allocator = state_allocator,
      .positions = vector_create_Vec3(load_arena),
      .normals = vector_create_Vec3(load_arena),
      .texcoords = vector_create_Vec2(load_arena),
      .subsets = vector_create_VkrMeshLoaderSubset(load_arena),
      .materials = vector_create_VkrMeshLoaderMaterialDef(load_arena),
      .obj_path = string8_duplicate(load_arena, &name),
      .obj_dir = file_path_get_directory(load_arena, name),
      .obj_stem = string8_get_stem(load_arena, name),
      .out_error = out_error,
  };
  state.material_dir =
      vkr_mesh_loader_make_material_dir(load_arena, &state.obj_stem);
  vkr_mesh_loader_builder_init(&state.builder, load_arena);
  return state;
}

// Job function for async file reading
vkr_internal bool8_t vkr_mesh_file_read_job_run(VkrJobContext *ctx,
                                                void *payload) {
  (void)ctx;
  VkrMeshFileReadJobPayload *job = (VkrMeshFileReadJobPayload *)payload;

  job->result->data = NULL;
  job->result->size = 0;
  job->result->success = false_v;

  FilePath fp = file_path_create((const char *)job->file_path.str, job->arena,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  if (!file_exists(&fp)) {
    return false_v;
  }

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return false_v;
  }

  uint8_t *data = NULL;
  uint64_t size = 0;
  FileError err = file_read_all(&fh, job->arena, &data, &size);
  file_close(&fh);

  if (err == FILE_ERROR_NONE && data && size > 0) {
    job->result->data = data;
    job->result->size = size;
    job->result->success = true_v;
    return true_v;
  }

  return false_v;
}

// Batch load materials - uses parallel file parsing and batch texture loading
vkr_internal void vkr_mesh_loader_batch_load_materials(
    VkrMeshLoaderState *state,
    Vector_VkrMeshPendingMaterial *pending_materials) {
  if (!pending_materials || pending_materials->length == 0) {
    return;
  }

  uint32_t count = (uint32_t)pending_materials->length;
  VkrMaterialSystem *mat_sys = state->context->material_system;

  // Allocate arrays for batch loading
  Scratch scratch = scratch_create(state->scratch_arena);

  String8 *material_paths = arena_alloc(scratch.arena, sizeof(String8) * count,
                                        ARENA_MEMORY_TAG_ARRAY);
  VkrMaterialHandle *handles = arena_alloc(
      scratch.arena, sizeof(VkrMaterialHandle) * count, ARENA_MEMORY_TAG_ARRAY);
  VkrRendererError *errors = arena_alloc(
      scratch.arena, sizeof(VkrRendererError) * count, ARENA_MEMORY_TAG_ARRAY);

  if (!material_paths || !handles || !errors) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_warn("Failed to allocate batch material arrays");
    return;
  }

  // Collect all material paths
  for (uint32_t i = 0; i < count; i++) {
    VkrMeshPendingMaterial *pending =
        vector_get_VkrMeshPendingMaterial(pending_materials, i);
    material_paths[i] = pending->material_path;
    handles[i] = VKR_MATERIAL_HANDLE_INVALID;
    errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  // Setup batch context
  VkrMaterialBatchContext batch_ctx = {
      .material_system = mat_sys,
      .job_system = state->context->job_system,
      .arena = state->load_arena,
      .temp_arena = state->temp_arena,
  };

  // Batch load all materials with parallel parsing and texture loading
  uint32_t loaded = vkr_material_loader_load_batch(&batch_ctx, material_paths,
                                                   count, handles, errors);

  // Assign handles back to pending materials and add refs
  for (uint32_t i = 0; i < count; i++) {
    VkrMeshPendingMaterial *pending =
        vector_get_VkrMeshPendingMaterial(pending_materials, i);
    if (handles[i].id != 0) {
      pending->handle = handles[i];
      vkr_material_system_add_ref(mat_sys, pending->handle);
    }
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  log_debug("Mesh batch loaded %u/%u materials", loaded, count);
}

vkr_internal bool8_t vkr_mesh_loader_write_binary(VkrMeshLoaderState *state,
                                                  String8 cache_path) {
  assert_log(state != NULL, "State is NULL");

  if (state->subsets.length == 0 || !cache_path.str)
    return false_v;

  String8 cache_dir = file_path_get_directory(state->load_arena, cache_path);
  if (!file_ensure_directory(state->load_arena, &cache_dir)) {
    log_warn("Failed to ensure cache directory '%.*s'",
             (int32_t)cache_dir.length, cache_dir.str);
    return false_v;
  }

  FilePath file_path = file_path_create(
      (const char *)cache_path.str, state->load_arena, FILE_PATH_TYPE_RELATIVE);
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
      if (has_digits) {
        continue;
      }
      sign = -1;
    } else if (ch == '/') {
      if (current < 3) {
        values[current++] = has_digits ? sign * value : 0;
      }
      sign = 1;
      value = 0;
      has_digits = false_v;
    } else if (ch >= '0' && ch <= '9') {
      value = value * 10 + (int32_t)(ch - '0');
      has_digits = true_v;
    }
  }

  if (current < 3) {
    values[current++] = has_digits ? sign * value : 0;
  }

  ref.position = values[0];
  ref.texcoord = (current >= 2) ? values[1] : 0;
  ref.normal = (current >= 3) ? values[2] : 0;

  return ref;
}

vkr_internal uint32_t vkr_mesh_loader_fix_index(int32_t value, uint32_t count) {
  if (value > 0) {
    return (uint32_t)(value - 1);
  }

  if (value < 0) {
    int64_t resolved = (int64_t)count + value;
    if (resolved < 0)
      resolved = 0;
    return (uint32_t)resolved;
  }

  return 0;
}

vkr_internal void
vkr_mesh_loader_builder_init(VkrMeshLoaderSubsetBuilder *builder,
                             Arena *arena) {
  builder->vertices = vector_create_VkrVertex3d(arena);
  builder->indices = vector_create_uint32_t(arena);
  builder->name = vkr_string8_duplicate_cstr(arena, "default");
  builder->material_name = (String8){0};
  builder->pipeline_domain = VKR_PIPELINE_DOMAIN_WORLD;
  builder->shader_override = (String8){0};
}

vkr_internal VkrMeshLoaderMaterialDef *
vkr_mesh_loader_find_material(Vector_VkrMeshLoaderMaterialDef *materials,
                              const String8 *name) {
  assert_log(materials != NULL, "Materials is NULL");

  if (!name || !name->str || name->length == 0)
    return NULL;

  for (uint64_t i = 0; i < materials->length; ++i) {
    VkrMeshLoaderMaterialDef *def =
        vector_get_VkrMeshLoaderMaterialDef(materials, i);
    if (string8_equalsi(&def->name, name)) {
      return def;
    }
  }

  return NULL;
}

vkr_internal String8 vkr_mesh_loader_make_material_dir(Arena *arena,
                                                       const String8 *stem) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(stem != NULL, "Stem is NULL");

  return string8_create_formatted(arena, "assets/materials/%.*s",
                                  (int32_t)stem->length, stem->str);
}

vkr_internal String8 vkr_mesh_loader_make_material_path(
    Arena *arena, const String8 *stem, const String8 *material) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(stem != NULL, "Stem is NULL");
  assert_log(material != NULL, "Material is NULL");

  return string8_create_formatted(arena, "assets/materials/%.*s/%.*s.mt",
                                  (int32_t)stem->length, stem->str,
                                  (int32_t)material->length, material->str);
}

vkr_internal String8 vkr_mesh_loader_texture_basename(Arena *arena,
                                                      const String8 *token) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token || !token->str || token->length == 0)
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
  return string8_duplicate(arena, &view);
}

vkr_internal String8 vkr_mesh_loader_texture_path(Arena *arena,
                                                  const String8 *token) {
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(token != NULL, "Token is NULL");

  if (!token || !token->str || token->length == 0)
    return (String8){0};

  String8 file = vkr_mesh_loader_texture_basename(arena, token);
  if (!file.str)
    return (String8){0};

  return string8_create_formatted(arena, "assets/textures/%.*s",
                                  (int32_t)file.length, file.str);
}

vkr_internal bool8_t vkr_mesh_loader_write_material_file(
    VkrMeshLoaderState *state, VkrMeshLoaderMaterialDef *material,
    String8 relative_path) {
  assert_log(state != NULL, "State is NULL");
  assert_log(material != NULL, "Material is NULL");
  assert_log(relative_path.str != NULL, "Relative path is NULL");

  FilePath dir_path =
      file_path_create(string8_cstr(&state->material_dir), state->load_arena,
                       FILE_PATH_TYPE_RELATIVE);
  if (!file_ensure_directory(state->load_arena, &dir_path.path)) {
    log_error("MeshLoader: failed to create material directory '%s'",
              string8_cstr(&state->material_dir));

    return false_v;
  }

  FilePath file_path =
      file_path_create((const char *)relative_path.str, state->load_arena,
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
      string8_create_formatted(state->load_arena, "name=%.*s",
                               (int32_t)material->name.length,
                               material->name.str),
      string8_create_formatted(state->load_arena, "diffuse_texture=%.*s",
                               (int32_t)material->diffuse_map.length,
                               material->diffuse_map.str),
      string8_create_formatted(
          state->load_arena, "diffuse_color=%f,%f,%f,%f",
          material->diffuse_color.x, material->diffuse_color.y,
          material->diffuse_color.z, material->diffuse_color.w),
      string8_create_formatted(state->load_arena, "specular_texture=%.*s",
                               (int32_t)material->specular_map.length,
                               material->specular_map.str),
      string8_create_formatted(
          state->load_arena, "specular_color=%f,%f,%f,%f",
          material->specular_color.x, material->specular_color.y,
          material->specular_color.z, material->specular_color.w),
      string8_create_formatted(state->load_arena, "norm_texture=%.*s",
                               (int32_t)material->normal_map.length,
                               material->normal_map.str),
      string8_create_formatted(state->load_arena, "shininess=%f",
                               material->shininess),
      string8_create_formatted(state->load_arena, "emission_color=%f,%f,%f",
                               material->emission_color.x,
                               material->emission_color.y,
                               material->emission_color.z),
      string8_create_formatted(state->load_arena, "shader=%.*s",
                               (int32_t)shader_value.length,
                               (const char *)shader_value.str),
      string8_create_formatted(state->load_arena, "pipeline=%s", "world"),
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

// Resolves material name to path, generates .mt file if needed
// Materials are loaded later via batch loading on the main thread
vkr_internal bool8_t vkr_mesh_loader_resolve_material(
    VkrMeshLoaderState *state, const String8 *material_name, String8 *out_path,
    VkrMaterialHandle *out_handle) {
  assert_log(state != NULL, "State is NULL");

  if (!material_name || !material_name->str || material_name->length == 0)
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
        state->load_arena, &state->obj_stem, material_name);
  }

  if (!mat->generated) {
    if (!vkr_mesh_loader_write_material_file(state, mat, mat->generated_path)) {
      return false_v;
    }
    mat->generated = true_v;
  }

  if (out_path) {
    *out_path = mat->generated_path;
  }
  if (out_handle) {
    *out_handle = VKR_MATERIAL_HANDLE_INVALID;
  }
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

  Scratch scratch = scratch_create(state->scratch_arena);
  VkrAllocator scratch_allocator = {.ctx = scratch.arena};
  vkr_allocator_arena(&scratch_allocator);
  uint32_t index_count = (uint32_t)builder->indices.length;
  uint32_t vertex_count = (uint32_t)builder->vertices.length;

  VkrVertex3d *dedup_vertices = NULL;
  uint32_t dedup_vertex_count = 0;
  uint32_t *indices_copy = vkr_allocator_alloc(
      &scratch_allocator, (uint64_t)index_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(indices_copy != NULL, "MeshLoader: failed to allocate indices");
  MemCopy(indices_copy, builder->indices.data,
          (uint64_t)builder->indices.length * sizeof(uint32_t));

  if (!vkr_geometry_system_deduplicate_vertices(
          state->context->geometry_system, scratch.arena,
          builder->vertices.data, vertex_count, indices_copy, index_count,
          &dedup_vertices, &dedup_vertex_count)) {
    vkr_allocator_free(&scratch_allocator, indices_copy,
                       (uint64_t)index_count * sizeof(uint32_t),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    log_error("MeshLoader: deduplication failed for subset");
    return false_v;
  }

  uint64_t vertex_bytes = (uint64_t)dedup_vertex_count * sizeof(VkrVertex3d);
  uint64_t index_bytes = (uint64_t)index_count * sizeof(uint32_t);
  VkrVertex3d *vertex_copy = vkr_allocator_alloc(
      &state->allocator, vertex_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *index_copy = vkr_allocator_alloc(&state->allocator, index_bytes,
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  assert_log(vertex_copy && index_copy,
             "MeshLoader: failed to allocate geometry buffers");

  MemCopy(vertex_copy, dedup_vertices, vertex_bytes);
  MemCopy(index_copy, indices_copy, index_bytes);

  vkr_geometry_system_generate_tangents(&state->allocator, vertex_copy,
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

  subset.material_name = string8_duplicate(state->load_arena, &material_path);
  subset.material_handle = mat_handle;
  subset.pipeline_domain = builder->pipeline_domain;
  subset.shader_override =
      string8_duplicate(state->load_arena, &builder->shader_override);

  vector_push_VkrMeshLoaderSubset(&state->subsets, subset);

  vkr_allocator_free(&scratch_allocator, indices_copy,
                     (uint64_t)index_count * sizeof(uint32_t),
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
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

  uint32_t face_vert_count = token_count;
  for (uint32_t tri = 0; tri < face_vert_count - 2; ++tri) {
    vector_push_uint32_t(&state->builder.indices, first_index);
    vector_push_uint32_t(&state->builder.indices, first_index + tri + 1);
    vector_push_uint32_t(&state->builder.indices, first_index + tri + 2);
  }
}

vkr_internal bool8_t vkr_mesh_loader_parse_mtl(VkrMeshLoaderState *state,
                                               String8 rel_path) {
  assert_log(state != NULL, "State is NULL");

  if (!rel_path.str || rel_path.length == 0)
    return false_v;

  String8 full_path =
      file_path_join(state->load_arena, state->obj_dir, rel_path);
  FilePath file_path = file_path_create(
      (const char *)full_path.str, state->load_arena, FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to open mtl '%s'", file_path.path.str);
    return false_v;
  }

  String8 file_str = {0};
  FileError read_err = file_read_string(&fh, state->load_arena, &file_str);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to read mtl '%s'", file_path.path.str);
    return false_v;
  }

  VkrMeshLoaderMaterialDef *current = NULL;
  uint64_t offset = 0;
  while (offset < file_str.length) {
    uint64_t line_end = offset;
    while (line_end < file_str.length && file_str.str[line_end] != '\n' &&
           file_str.str[line_end] != '\r') {
      line_end++;
    }
    String8 line = string8_substring(&file_str, offset, line_end);
    offset = line_end;
    while (offset < file_str.length &&
           (file_str.str[offset] == '\n' || file_str.str[offset] == '\r')) {
      offset++;
    }

    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#')
      continue;

    if (vkr_string8_starts_with(&line, "newmtl")) {
      String8 mat_name = vkr_string8_trimmed_suffix(&line, 6);
      VkrMeshLoaderMaterialDef def = {
          .name = string8_duplicate(state->load_arena, &mat_name),
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

    if (vkr_string8_starts_with(&line, "Kd")) {
      String8 tokens[3];
      String8 values = vkr_string8_trimmed_suffix(&line, 2);
      uint32_t count = string8_split_whitespace(&values, tokens, 3);
      if (count >= 3) {
        string8_to_f32(&tokens[0], &current->diffuse_color.x);
        string8_to_f32(&tokens[1], &current->diffuse_color.y);
        string8_to_f32(&tokens[2], &current->diffuse_color.z);
      }
    } else if (vkr_string8_starts_with(&line, "Ks")) {
      String8 tokens[3];
      String8 values = vkr_string8_trimmed_suffix(&line, 2);
      uint32_t count = string8_split_whitespace(&values, tokens, 3);
      if (count >= 3) {
        string8_to_f32(&tokens[0], &current->specular_color.x);
        string8_to_f32(&tokens[1], &current->specular_color.y);
        string8_to_f32(&tokens[2], &current->specular_color.z);
      }
    } else if (vkr_string8_starts_with(&line, "Ns")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 2);
      float32_t shininess = 0.0f;
      string8_to_f32(&value, &shininess);
      if (shininess > 0.0f) {
        current->shininess = shininess;
      }
    } else if (vkr_string8_starts_with(&line, "map_Kd")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->diffuse_map =
          vkr_mesh_loader_texture_path(state->load_arena, &value);
    } else if (vkr_string8_starts_with(&line, "map_Ks")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->specular_map =
          vkr_mesh_loader_texture_path(state->load_arena, &value);
    } else if (vkr_string8_starts_with(&line, "map_bump") ||
               vkr_string8_starts_with(&line, "bump")) {
      uint64_t idx = vkr_string8_starts_with(&line, "map_bump") ? 8 : 4;
      String8 value = vkr_string8_trimmed_suffix(&line, idx);
      current->normal_map =
          vkr_mesh_loader_texture_path(state->load_arena, &value);
    } else if (vkr_string8_starts_with(&line, "shader")) {
      String8 value = vkr_string8_trimmed_suffix(&line, 6);
      current->shader_name = string8_duplicate(state->load_arena, &value);
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_parse_obj(VkrMeshLoaderState *state) {
  FilePath file_path =
      file_path_create((const char *)state->obj_path.str, state->load_arena,
                       FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to open obj '%s'", file_path.path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  String8 file_str = {0};
  FileError read_err = file_read_string(&fh, state->load_arena, &file_str);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE) {
    log_error("MeshLoader: failed to read obj '%s'", file_path.path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  uint64_t offset = 0;
  while (offset < file_str.length) {
    uint64_t line_end = offset;
    while (line_end < file_str.length && file_str.str[line_end] != '\n' &&
           file_str.str[line_end] != '\r') {
      line_end++;
    }
    String8 line = string8_substring(&file_str, offset, line_end);
    offset = line_end;
    while (offset < file_str.length &&
           (file_str.str[offset] == '\n' || file_str.str[offset] == '\r')) {
      offset++;
    }
    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#')
      continue;

    if (vkr_string8_starts_with(&line, "v ")) {
      String8 coords = vkr_string8_trimmed_suffix(&line, 1);
      String8 tokens[3];
      uint32_t count = string8_split_whitespace(&coords, tokens, 3);
      if (count >= 3) {
        float32_t x = 0, y = 0, z = 0;
        string8_to_f32(&tokens[0], &x);
        string8_to_f32(&tokens[1], &y);
        string8_to_f32(&tokens[2], &z);
        vector_push_Vec3(&state->positions, vec3_new(x, y, z));
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "vn")) {
      String8 coords = vkr_string8_trimmed_suffix(&line, 2);
      String8 tokens[3];
      uint32_t count = string8_split_whitespace(&coords, tokens, 3);
      if (count >= 3) {
        float32_t x = 0, y = 0, z = 0;
        string8_to_f32(&tokens[0], &x);
        string8_to_f32(&tokens[1], &y);
        string8_to_f32(&tokens[2], &z);
        vector_push_Vec3(&state->normals, vec3_new(x, y, z));
      }
      continue;
    }

    if (vkr_string8_starts_with(&line, "vt")) {
      String8 coords = vkr_string8_trimmed_suffix(&line, 2);
      String8 tokens[2];
      uint32_t count = string8_split_whitespace(&coords, tokens, 2);
      if (count >= 2) {
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
          string8_duplicate(state->load_arena, &material_name);
      continue;
    }

    if (vkr_string8_starts_with(&line, "o ") ||
        vkr_string8_starts_with(&line, "g ")) {
      vkr_mesh_loader_finalize_subset(state);
      String8 name = vkr_string8_trimmed_suffix(&line, 1);
      state->builder.name = string8_duplicate(state->load_arena, &name);
      continue;
    }

    if (vkr_string8_starts_with(&line, "f ")) {
      String8 face = vkr_string8_trimmed_suffix(&line, 1);
      String8 tokens[64];
      uint32_t count = string8_split_whitespace(&face, tokens, 64);
      if (count >= 3) {
        vkr_mesh_loader_push_face(state, count, tokens);
      }
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

// Payload for mesh loading job (reads and parses mesh without material loading)
typedef struct VkrMeshLoadJobPayload {
  String8 mesh_path;
  VkrMeshLoaderContext *context;
  Arena *result_arena;
  Arena *temp_arena;
  VkrMeshLoaderResult *result;
  Vector_VkrMeshPendingMaterial *pending_materials; // Output: materials to load
  VkrRendererError *error;
  bool8_t *success;
} VkrMeshLoadJobPayload;

// Internal: Read binary cache without loading materials
vkr_internal bool8_t vkr_mesh_loader_read_binary_no_materials(
    VkrMeshLoaderState *state, String8 cache_path,
    Vector_VkrMeshPendingMaterial *out_pending_materials) {
  assert_log(state != NULL, "State is NULL");

  if (!cache_path.str || cache_path.length == 0)
    return false_v;

  FilePath file_path = file_path_create(
      (const char *)cache_path.str, state->load_arena, FILE_PATH_TYPE_RELATIVE);

  if (!file_exists(&file_path)) {
    return false_v;
  }

  // Read file data
  uint8_t *data = NULL;
  uint64_t size = 0;

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&file_path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    return false_v;
  }

  FileError read_err = file_read_all(&fh, state->load_arena, &data, &size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !data || size == 0) {
    return false_v;
  }

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
  if (!vkr_mesh_loader_read_string(&reader, state->load_arena, &cached_name)) {
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

    if (!vkr_mesh_loader_read_string(&reader, state->load_arena,
                                     &subset_name) ||
        !vkr_mesh_loader_read_string(&reader, state->load_arena,
                                     &material_path) ||
        !vkr_mesh_loader_read_string(&reader, state->load_arena,
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

    VkrVertex3d *vertices = vkr_allocator_alloc(&state->allocator, vertex_bytes,
                                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    uint32_t *indices = vkr_allocator_alloc(&state->allocator, index_bytes,
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

      // Collect material for batch loading
      VkrMeshPendingMaterial pending = {
          .material_path = material_path,
          .handle = VKR_MATERIAL_HANDLE_INVALID,
      };
      vector_push_VkrMeshPendingMaterial(out_pending_materials, pending);
    }

    subset.material_name = material_path;
    subset.pipeline_domain = (VkrPipelineDomain)pipeline_domain;
    subset.shader_override =
        shader_override.length > 0
            ? string8_duplicate(state->load_arena, &shader_override)
            : (String8){0};
    subset.material_handle = VKR_MATERIAL_HANDLE_INVALID;

    vector_push_VkrMeshLoaderSubset(&state->subsets, subset);
  }

  log_debug("Read cache '%s' (%u subsets, %u pending materials)",
            file_path.path.str, subset_count,
            (uint32_t)out_pending_materials->length);
  return true_v;
}

// Job function for loading a single mesh
vkr_internal bool8_t vkr_mesh_load_job_run(VkrJobContext *ctx, void *payload) {
  VkrMeshLoadJobPayload *job = (VkrMeshLoadJobPayload *)payload;

  *job->success = false_v;
  *job->error = VKR_RENDERER_ERROR_NONE;

  // Use the job's thread-local scratch arena instead of the shared context
  // arena to avoid thread-safety issues when multiple mesh jobs run in parallel
  Arena *job_scratch =
      ctx->worker_arena ? ctx->worker_arena : job->result_arena;

  VkrMeshLoaderState state = vkr_mesh_loader_state_create(
      job->context, job->result_arena, job->temp_arena, job_scratch,
      job->mesh_path, job->error);

  // Try to load from cache first
  String8 cache_path = vkr_mesh_loader_cache_path(&state);
  bool8_t loaded_from_cache = false_v;

  if (cache_path.str) {
    loaded_from_cache = vkr_mesh_loader_read_binary_no_materials(
        &state, cache_path, job->pending_materials);
  }

  if (!loaded_from_cache) {
    // Parse OBJ (this will populate subsets and materials)
    if (!vkr_mesh_loader_parse_obj(&state)) {
      return false_v;
    }

    // Write cache for next time
    if (cache_path.str) {
      vkr_mesh_loader_write_binary(&state, cache_path);
    }

    // Collect materials from parsed OBJ
    for (uint64_t i = 0; i < state.subsets.length; i++) {
      VkrMeshLoaderSubset *subset =
          vector_get_VkrMeshLoaderSubset(&state.subsets, i);
      if (subset->material_name.str && subset->material_name.length > 0) {
        VkrMeshPendingMaterial pending = {
            .material_path = subset->material_name,
            .handle = VKR_MATERIAL_HANDLE_INVALID,
        };
        vector_push_VkrMeshPendingMaterial(job->pending_materials, pending);
      }
    }
  }

  if (state.subsets.length == 0) {
    *job->error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  // Build result
  Array_VkrMeshLoaderSubset subset_array =
      array_create_VkrMeshLoaderSubset(job->result_arena, state.subsets.length);
  for (uint64_t i = 0; i < state.subsets.length; ++i) {
    array_set_VkrMeshLoaderSubset(&subset_array, i, state.subsets.data[i]);
  }

  job->result->arena = job->result_arena;
  job->result->source_path =
      string8_duplicate(job->result_arena, &job->mesh_path);
  job->result->root_transform = vkr_transform_identity();
  job->result->subsets = subset_array;

  *job->success = true_v;
  return true_v;
}

uint32_t vkr_mesh_loader_load_batch(VkrMeshLoaderContext *context,
                                    const String8 *mesh_paths, uint32_t count,
                                    Arena *temp_arena,
                                    VkrMeshBatchResult *out_results) {
  assert_log(context != NULL, "Context is NULL");
  assert_log(mesh_paths != NULL, "Mesh paths is NULL");
  assert_log(out_results != NULL, "Out results is NULL");

  if (count == 0) {
    return 0;
  }

  // Initialize outputs
  for (uint32_t i = 0; i < count; i++) {
    out_results[i].result = NULL;
    out_results[i].error = VKR_RENDERER_ERROR_NONE;
    out_results[i].success = false_v;
  }

  VkrJobSystem *job_sys = context->job_system;
  Scratch scratch = scratch_create(temp_arena);

  // Allocate per-mesh structures
  VkrMeshLoaderResult **results =
      arena_alloc(scratch.arena, sizeof(VkrMeshLoaderResult *) * count,
                  ARENA_MEMORY_TAG_ARRAY);
  Arena **result_arenas = arena_alloc(scratch.arena, sizeof(Arena *) * count,
                                      ARENA_MEMORY_TAG_ARRAY);
  Vector_VkrMeshPendingMaterial *pending_materials_per_mesh =
      arena_alloc(scratch.arena, sizeof(Vector_VkrMeshPendingMaterial) * count,
                  ARENA_MEMORY_TAG_ARRAY);
  VkrRendererError *errors = arena_alloc(
      scratch.arena, sizeof(VkrRendererError) * count, ARENA_MEMORY_TAG_ARRAY);
  bool8_t *success = arena_alloc(scratch.arena, sizeof(bool8_t) * count,
                                 ARENA_MEMORY_TAG_ARRAY);
  VkrJobHandle *job_handles = arena_alloc(
      scratch.arena, sizeof(VkrJobHandle) * count, ARENA_MEMORY_TAG_ARRAY);
  VkrMeshLoadJobPayload *payloads =
      arena_alloc(scratch.arena, sizeof(VkrMeshLoadJobPayload) * count,
                  ARENA_MEMORY_TAG_ARRAY);
  bool8_t *job_submitted = arena_alloc(scratch.arena, sizeof(bool8_t) * count,
                                       ARENA_MEMORY_TAG_ARRAY);

  if (!results || !result_arenas || !pending_materials_per_mesh || !errors ||
      !success || !job_handles || !payloads || !job_submitted) {
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
    for (uint32_t i = 0; i < count; i++) {
      out_results[i].error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // Initialize per-mesh data
  for (uint32_t i = 0; i < count; i++) {
    result_arenas[i] = arena_create(MB(6));
    if (!result_arenas[i]) {
      for (uint32_t j = 0; j < i; j++) {
        arena_destroy(result_arenas[j]);
      }
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      for (uint32_t k = 0; k < count; k++) {
        out_results[k].error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
      return 0;
    }

    results[i] = arena_alloc(result_arenas[i], sizeof(VkrMeshLoaderResult),
                             ARENA_MEMORY_TAG_STRUCT);
    pending_materials_per_mesh[i] =
        vector_create_VkrMeshPendingMaterial(result_arenas[i]);
    errors[i] = VKR_RENDERER_ERROR_NONE;
    success[i] = false_v;
    job_submitted[i] = false_v;
  }

  // Phase 1: Submit all mesh load jobs in parallel
  if (job_sys) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);

    for (uint32_t i = 0; i < count; i++) {
      if (!mesh_paths[i].str || mesh_paths[i].length == 0) {
        continue;
      }

      payloads[i] = (VkrMeshLoadJobPayload){
          .mesh_path = mesh_paths[i],
          .context = context,
          .result_arena = result_arenas[i],
          .temp_arena = temp_arena,
          .result = results[i],
          .pending_materials = &pending_materials_per_mesh[i],
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

      if (vkr_job_submit(job_sys, &job_desc, &job_handles[i])) {
        job_submitted[i] = true_v;
      }
    }

    // Wait for all mesh load jobs to complete
    for (uint32_t i = 0; i < count; i++) {
      if (job_submitted[i]) {
        vkr_job_wait(job_sys, job_handles[i]);
      }
    }
  } else {
    // Synchronous fallback
    for (uint32_t i = 0; i < count; i++) {
      if (!mesh_paths[i].str || mesh_paths[i].length == 0) {
        continue;
      }

      payloads[i] = (VkrMeshLoadJobPayload){
          .mesh_path = mesh_paths[i],
          .context = context,
          .result_arena = result_arenas[i],
          .temp_arena = temp_arena,
          .result = results[i],
          .pending_materials = &pending_materials_per_mesh[i],
          .error = &errors[i],
          .success = &success[i],
      };

      VkrJobContext fake_ctx = {
          .system = NULL,
          .worker_index = 0,
          .thread_id = 0,
          .worker_arena = scratch.arena,
          .scratch = scratch,
      };
      vkr_mesh_load_job_run(&fake_ctx, &payloads[i]);
    }
  }

  // Phase 2: Collect ALL material paths from ALL meshes
  uint32_t total_materials = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (success[i]) {
      total_materials += (uint32_t)pending_materials_per_mesh[i].length;
    }
  }

  log_debug("Mesh batch: %u meshes loaded, %u total materials to load", count,
            total_materials);

  if (total_materials > 0) {
    // Allocate material batch arrays
    String8 *all_material_paths =
        arena_alloc(scratch.arena, sizeof(String8) * total_materials,
                    ARENA_MEMORY_TAG_ARRAY);
    VkrMaterialHandle *all_material_handles =
        arena_alloc(scratch.arena, sizeof(VkrMaterialHandle) * total_materials,
                    ARENA_MEMORY_TAG_ARRAY);
    VkrRendererError *all_material_errors =
        arena_alloc(scratch.arena, sizeof(VkrRendererError) * total_materials,
                    ARENA_MEMORY_TAG_ARRAY);
    uint32_t *material_mesh_index =
        arena_alloc(scratch.arena, sizeof(uint32_t) * total_materials,
                    ARENA_MEMORY_TAG_ARRAY);
    uint32_t *material_local_index =
        arena_alloc(scratch.arena, sizeof(uint32_t) * total_materials,
                    ARENA_MEMORY_TAG_ARRAY);

    if (!all_material_paths || !all_material_handles || !all_material_errors ||
        !material_mesh_index || !material_local_index) {
      // Clean up and fall back to individual loading
      for (uint32_t i = 0; i < count; i++) {
        if (success[i] && results[i]) {
          out_results[i].result = results[i];
          out_results[i].error = VKR_RENDERER_ERROR_NONE;
          out_results[i].success = true_v;
        } else {
          out_results[i].error = errors[i];
          arena_destroy(result_arenas[i]);
        }
      }
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      return count;
    }

    // Collect all material paths
    uint32_t mat_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (!success[i])
        continue;

      for (uint64_t j = 0; j < pending_materials_per_mesh[i].length; j++) {
        VkrMeshPendingMaterial *pending = vector_get_VkrMeshPendingMaterial(
            &pending_materials_per_mesh[i], j);
        all_material_paths[mat_idx] = pending->material_path;
        material_mesh_index[mat_idx] = i;
        material_local_index[mat_idx] = (uint32_t)j;
        mat_idx++;
      }
    }

    // Phase 3: Batch load ALL materials at once
    VkrMaterialBatchContext batch_ctx = {
        .material_system = context->material_system,
        .job_system = job_sys,
        .arena = scratch.arena,
        .temp_arena = temp_arena,
    };

    uint32_t materials_loaded = vkr_material_loader_load_batch(
        &batch_ctx, all_material_paths, total_materials, all_material_handles,
        all_material_errors);

    log_debug("Mesh batch: %u/%u materials loaded", materials_loaded,
              total_materials);

    // Phase 4: Assign material handles back to mesh subsets
    for (uint32_t m = 0; m < total_materials; m++) {
      if (all_material_handles[m].id == 0)
        continue;

      uint32_t mesh_idx = material_mesh_index[m];
      uint32_t local_idx = material_local_index[m];

      // Update the pending material
      VkrMeshPendingMaterial *pending = vector_get_VkrMeshPendingMaterial(
          &pending_materials_per_mesh[mesh_idx], local_idx);
      pending->handle = all_material_handles[m];
      vkr_material_system_add_ref(context->material_system, pending->handle);
    }

    // Assign handles to subsets
    for (uint32_t i = 0; i < count; i++) {
      if (!success[i] || !results[i])
        continue;

      uint64_t pending_idx = 0;
      for (uint64_t s = 0; s < results[i]->subsets.length; s++) {
        VkrMeshLoaderSubset *subset = &results[i]->subsets.data[s];
        if (subset->material_name.str && subset->material_name.length > 0) {
          // Find matching pending material
          for (uint64_t j = pending_idx;
               j < pending_materials_per_mesh[i].length; j++) {
            VkrMeshPendingMaterial *pending = vector_get_VkrMeshPendingMaterial(
                &pending_materials_per_mesh[i], j);
            if (string8_equalsi(&pending->material_path,
                                &subset->material_name)) {
              subset->material_handle = pending->handle;
              pending_idx = j + 1;
              break;
            }
          }
        }
      }
    }
  }

  // Build final results
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
      arena_destroy(result_arenas[i]);
    }
  }

  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  log_debug("Mesh batch complete: %u/%u meshes loaded successfully",
            loaded_count, count);
  return loaded_count;
}

void vkr_mesh_loader_free_batch_results(VkrMeshBatchResult *results,
                                        uint32_t count) {
  if (!results)
    return;

  for (uint32_t i = 0; i < count; i++) {
    if (results[i].result && results[i].result->arena) {
      arena_destroy(results[i].result->arena);
      results[i].result = NULL;
    }
  }
}

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MESH;
  loader.resource_system = context;
  loader.can_load = vkr_mesh_loader_can_load;
  // Note: load/unload not set - use vkr_mesh_loader_load_batch instead
  loader.load = NULL;
  loader.unload = NULL;
  return loader;
}
