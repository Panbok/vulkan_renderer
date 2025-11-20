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

vkr_global Arena *loader_arena = NULL;
vkr_global VkrAllocator loader_allocator = {0};
vkr_global Scratch loader_scratch = {0};

Vector(Vec2);
Vector(Vec3);
Vector(VkrVertex3d);
Vector(VkrMeshLoaderSubset);
#define DEFAULT_SHADER string8_lit("shader.default.world")

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

  if (mat->material_handle.id == 0) {
    VkrResourceHandleInfo handle_info = {0};
    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                                 mat->generated_path, state->temp_arena,
                                 &handle_info, &err)) {
      mat->material_handle = handle_info.as.material;
      if (state->context && state->context->material_system &&
          mat->material_handle.id != 0) {
        vkr_material_system_add_ref(state->context->material_system,
                                    mat->material_handle);
      }
    } else if (state->context && state->context->material_system) {
      VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
      VkrMaterialHandle existing = vkr_material_system_acquire(
          state->context->material_system, mat->name, true_v, &acquire_err);
      if (existing.id != 0) {
        mat->material_handle = existing;
      } else {
        log_error("MeshLoader: material reuse failed for '%.*s'",
                  (int32_t)mat->name.length, mat->name.str);
        return false_v;
      }
    } else {
      log_error("MeshLoader: material load failed for '%.*s'",
                (int32_t)mat->generated_path.length, mat->generated_path.str);
      return false_v;
    }
  }

  if (out_path) {
    *out_path = mat->generated_path;
  }

  if (out_handle) {
    *out_handle = mat->material_handle;
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

vkr_internal bool8_t vkr_mesh_loader_load(VkrResourceLoader *self, String8 name,
                                          Arena *temp_arena,
                                          VkrResourceHandleInfo *out_handle,
                                          VkrRendererError *out_error) {
  assert_log(self != NULL, "Loader is NULL");
  assert_log(out_handle != NULL, "Handle is NULL");
  assert_log(out_error != NULL, "Error is NULL");

  VkrMeshLoaderContext *context = (VkrMeshLoaderContext *)self->resource_system;
  if (!context || !context->geometry_system || !context->material_system) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  loader_scratch = scratch_create(loader_arena);

  VkrMeshLoaderState state = {
      .context = context,
      .load_arena = loader_scratch.arena,
      .temp_arena = temp_arena,
      .scratch_arena = context->scratch_arena ? context->scratch_arena
                                              : loader_scratch.arena,
      .allocator = loader_allocator,
      .positions = vector_create_Vec3(loader_scratch.arena),
      .normals = vector_create_Vec3(loader_scratch.arena),
      .texcoords = vector_create_Vec2(loader_scratch.arena),
      .subsets = vector_create_VkrMeshLoaderSubset(loader_scratch.arena),
      .materials = vector_create_VkrMeshLoaderMaterialDef(loader_scratch.arena),
      .obj_path = string8_duplicate(loader_scratch.arena, &name),
      .obj_dir = file_path_get_directory(loader_scratch.arena, name),
      .obj_stem = string8_get_stem(loader_scratch.arena, name),
      .out_error = out_error,
  };
  state.material_dir =
      vkr_mesh_loader_make_material_dir(loader_scratch.arena, &state.obj_stem);
  vkr_mesh_loader_builder_init(&state.builder, loader_scratch.arena);

  if (!vkr_mesh_loader_parse_obj(&state)) {
    scratch_destroy(loader_scratch, ARENA_MEMORY_TAG_STRUCT);
    out_handle->as.mesh = NULL;
    return false_v;
  }

  if (state.subsets.length == 0) {
    log_warn("MeshLoader: file '%.*s' produced no geometry",
             (int32_t)name.length, name.str);
    arena_destroy(loader_arena);
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  Array_VkrMeshLoaderSubset subset_array =
      array_create_VkrMeshLoaderSubset(loader_arena, state.subsets.length);
  for (uint64_t i = 0; i < state.subsets.length; ++i) {
    VkrMeshLoaderSubset subset = state.subsets.data[i];
    array_set_VkrMeshLoaderSubset(&subset_array, i, subset);
  }

  VkrMeshLoaderResult *result =
      vkr_allocator_alloc(&loader_allocator, sizeof(VkrMeshLoaderResult),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  result->arena = loader_scratch.arena;
  result->source_path = string8_duplicate(loader_scratch.arena, &name);
  result->root_transform = vkr_transform_identity();
  result->subsets = subset_array;

  out_handle->type = VKR_RESOURCE_TYPE_MESH;
  out_handle->loader_id = self->id;
  out_handle->as.mesh = result;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal void vkr_mesh_loader_unload(VkrResourceLoader *self,
                                         const VkrResourceHandleInfo *handle,
                                         String8 name) {
  (void)name;
  if (!self || !handle || !handle->as.mesh)
    return;

  VkrMeshLoaderResult *result = handle->as.mesh;
  if (result && result->arena) {
    Arena *arena = result->arena;
    result->arena = NULL;
    vkr_allocator_free(&loader_allocator, loader_scratch.arena,
                       arena_pos(arena), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    scratch_destroy(loader_scratch, ARENA_MEMORY_TAG_STRUCT);
  }
}

VkrResourceLoader vkr_mesh_loader_create(VkrMeshLoaderContext *context) {
  loader_arena = arena_create(MB(6));
  if (!loader_arena) {
    log_fatal("Failed to create loader arena for mesh loader");
    return (VkrResourceLoader){0};
  }

  loader_allocator = (VkrAllocator){.ctx = loader_arena};
  vkr_allocator_arena(&loader_allocator);

  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MESH;
  loader.resource_system = context;
  loader.can_load = vkr_mesh_loader_can_load;
  loader.load = vkr_mesh_loader_load;
  loader.unload = vkr_mesh_loader_unload;
  return loader;
}
