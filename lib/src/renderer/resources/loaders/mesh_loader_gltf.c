#include "renderer/resources/loaders/mesh_loader_gltf.h"

#include <cgltf.h>

#include "containers/str.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "math/mat.h"

#define VKR_FNV1A64_OFFSET_BASIS 0xcbf29ce484222325ull
#define VKR_FNV1A64_PRIME 0x100000001b3ull

vkr_internal void
vkr_mesh_loader_gltf_set_error(const VkrMeshLoaderGltfParseInfo *info,
                               VkrRendererError error) {
  if (info && info->out_error) {
    *info->out_error = error;
  }
}

vkr_internal String8
vkr_mesh_loader_gltf_alpha_mode_to_string(cgltf_alpha_mode alpha_mode) {
  switch (alpha_mode) {
  case cgltf_alpha_mode_mask:
    return string8_lit("mask");
  case cgltf_alpha_mode_blend:
    return string8_lit("blend");
  case cgltf_alpha_mode_opaque:
  default:
    return string8_lit("opaque");
  }
}

vkr_internal Mat4
vkr_mesh_loader_gltf_mat4_from_cgltf(const cgltf_float source[16]) {
  return mat4_new(source[0], source[1], source[2], source[3], source[4],
                  source[5], source[6], source[7], source[8], source[9],
                  source[10], source[11], source[12], source[13], source[14],
                  source[15]);
}

/**
 * @brief Hashes the canonical glTF source path for cross-asset material IDs.
 *
 * The generated `.mt` stem is used as the material-system lookup key before
 * file parsing in some load paths. Including a source-path hash prevents
 * collisions for assets that each contain `material[0]`, `material[1]`, etc.
 */
vkr_internal uint64_t
vkr_mesh_loader_gltf_hash_source_path(String8 source_path) {
  if (!source_path.str || source_path.length == 0) {
    return VKR_FNV1A64_OFFSET_BASIS;
  }

  uint64_t hash = VKR_FNV1A64_OFFSET_BASIS;
  for (uint64_t i = 0; i < source_path.length; ++i) {
    hash ^= (uint64_t)source_path.str[i];
    hash *= VKR_FNV1A64_PRIME;
  }

  return hash;
}

vkr_internal String8
vkr_mesh_loader_gltf_make_material_id(VkrAllocator *allocator,
                                      uint64_t source_hash,
                                      uint32_t material_index) {
  return string8_create_formatted(allocator, "gltf_mat_%016llx_%u",
                                  (unsigned long long)source_hash,
                                  material_index);
}

vkr_internal String8 vkr_mesh_loader_gltf_append_query(VkrAllocator *allocator,
                                                       String8 path,
                                                       const char *suffix) {
  if (!allocator || !path.str || path.length == 0 || !suffix ||
      suffix[0] == 0) {
    return path;
  }

  bool8_t has_query = false_v;
  for (uint64_t i = 0; i < path.length; ++i) {
    if (path.str[i] == '?') {
      has_query = true_v;
      break;
    }
  }

  return string8_create_formatted(allocator, has_query ? "%.*s&%s" : "%.*s?%s",
                                  (int32_t)path.length, path.str, suffix);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_path_is_absolute(String8 path) {
  return (path.length > 0 && (path.str[0] == '/' || path.str[0] == '\\')) ||
         (path.length > 1 && path.str[1] == ':');
}

vkr_internal bool8_t vkr_mesh_loader_gltf_path_exists(VkrAllocator *allocator,
                                                      String8 path) {
  if (!allocator || !path.str || path.length == 0) {
    return false_v;
  }

  char *path_cstr = (char *)vkr_allocator_alloc(
      allocator, path.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!path_cstr) {
    return false_v;
  }
  MemCopy(path_cstr, path.str, path.length);
  path_cstr[path.length] = '\0';

  FilePathType type = vkr_mesh_loader_gltf_path_is_absolute(path)
                          ? FILE_PATH_TYPE_ABSOLUTE
                          : FILE_PATH_TYPE_RELATIVE;
  FilePath fp = file_path_create(path_cstr, allocator, type);
  bool8_t exists = file_exists(&fp);
  vkr_allocator_free(allocator, path_cstr, path.length + 1,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return exists;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_find_existing_texture_file(
    VkrAllocator *allocator, String8 base_path, String8 *out_existing_path) {
  assert_log(out_existing_path != NULL, "Out existing path is NULL");
  *out_existing_path = (String8){0};

  if (vkr_mesh_loader_gltf_path_exists(allocator, base_path)) {
    *out_existing_path = string8_duplicate(allocator, &base_path);
    return true_v;
  }

  String8 sidecar_vkt = string8_create_formatted(
      allocator, "%.*s.vkt", (int32_t)base_path.length, base_path.str);
  if (vkr_mesh_loader_gltf_path_exists(allocator, sidecar_vkt)) {
    *out_existing_path = sidecar_vkt;
    return true_v;
  }

  return false_v;
}

vkr_internal String8 vkr_mesh_loader_gltf_basename_view(String8 path) {
  if (!path.str || path.length == 0) {
    return (String8){0};
  }

  uint64_t start = 0;
  for (uint64_t i = path.length; i > 0; --i) {
    const uint8_t ch = path.str[i - 1];
    if (ch == '/' || ch == '\\') {
      start = i;
      break;
    }
  }
  return string8_substring(&path, start, path.length);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_push_unique_path(
    Vector_String8 *paths, String8 value, VkrAllocator *allocator) {
  if (!paths || !allocator || !value.str || value.length == 0) {
    return true_v;
  }

  for (uint64_t i = 0; i < paths->length; ++i) {
    String8 *existing = vector_get_String8(paths, i);
    if (existing && string8_equalsi(existing, &value)) {
      return true_v;
    }
  }

  String8 copy = string8_duplicate(allocator, &value);
  vector_push_String8(paths, copy);
  return true_v;
}

vkr_internal String8 vkr_mesh_loader_gltf_resolve_relative_texture_uri(
    const VkrMeshLoaderGltfParseInfo *info, String8 uri, bool8_t *out_found,
    String8 *out_existing_path, bool8_t log_missing) {
  assert_log(info != NULL, "Parse info is NULL");
  assert_log(out_found != NULL, "Out found is NULL");
  assert_log(out_existing_path != NULL, "Out existing path is NULL");

  *out_found = false_v;
  *out_existing_path = (String8){0};

  if (vkr_mesh_loader_gltf_path_is_absolute(uri)) {
    String8 absolute = string8_duplicate(info->load_allocator, &uri);
    *out_found = vkr_mesh_loader_gltf_find_existing_texture_file(
        info->load_allocator, absolute, out_existing_path);
    return absolute;
  }

  String8 source_candidate =
      file_path_join(info->load_allocator, info->source_dir, uri);
  String8 assets_candidate =
      file_path_join(info->load_allocator, string8_lit("assets"), uri);
  String8 basename = vkr_mesh_loader_gltf_basename_view(uri);
  String8 assets_textures_candidate =
      basename.length > 0
          ? file_path_join(info->load_allocator, string8_lit("assets/textures"),
                           basename)
          : (String8){0};
  bool8_t has_distinct_assets_textures_candidate =
      assets_textures_candidate.length > 0 &&
      !string8_equals(&assets_candidate, &assets_textures_candidate);

  if (vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, source_candidate, out_existing_path)) {
    *out_found = true_v;
    return source_candidate;
  }
  if (vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, assets_candidate, out_existing_path)) {
    *out_found = true_v;
    return assets_candidate;
  }
  if (has_distinct_assets_textures_candidate &&
      vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, assets_textures_candidate, out_existing_path)) {
    *out_found = true_v;
    return assets_textures_candidate;
  }

  if (log_missing) {
    if (has_distinct_assets_textures_candidate) {
      log_warn("MeshLoader(glTF): texture '%.*s' not found; tried '%.*s', "
               "'%.*s', '%.*s'",
               (int32_t)uri.length, uri.str, (int32_t)source_candidate.length,
               source_candidate.str, (int32_t)assets_candidate.length,
               assets_candidate.str, (int32_t)assets_textures_candidate.length,
               assets_textures_candidate.str);
    } else {
      log_warn(
          "MeshLoader(glTF): texture '%.*s' not found; tried '%.*s', '%.*s'",
          (int32_t)uri.length, uri.str, (int32_t)source_candidate.length,
          source_candidate.str, (int32_t)assets_candidate.length,
          assets_candidate.str);
    }
  }

  return source_candidate;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_resolve_texture_path(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_texture_view *view,
    const char *intent_query, String8 *out_path) {
  assert_log(out_path != NULL, "Out path is NULL");

  *out_path = (String8){0};
  if (!view || !view->texture || !view->texture->image) {
    return true_v;
  }

  cgltf_image *image = view->texture->image;
  if (image->uri) {
    String8 uri = string8_create_from_cstr((const uint8_t *)image->uri,
                                           string_length(image->uri));
    if (vkr_string8_starts_with(&uri, "data:")) {
      log_error("MeshLoader(glTF): embedded data URI images are not supported");
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }

    bool8_t found = false_v;
    String8 existing_path = {0};
    String8 resolved = vkr_mesh_loader_gltf_resolve_relative_texture_uri(
        info, uri, &found, &existing_path, true_v);
    (void)found;
    (void)existing_path;

    *out_path = vkr_mesh_loader_gltf_append_query(info->load_allocator,
                                                  resolved, intent_query);
    return true_v;
  }

  if (image->buffer_view) {
    log_error(
        "MeshLoader(glTF): embedded buffer_view images are not supported");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_line(FileHandle *file,
                                                     String8 line) {
  if (!line.str || line.length == 0) {
    return true_v;
  }
  return file_write_line(file, &line) == FILE_ERROR_NONE ? true_v : false_v;
}

typedef struct VkrMeshLoaderGltfTextureWriteLine {
  const char *key;
  String8 value;
  const char *prefix_literal;
} VkrMeshLoaderGltfTextureWriteLine;

vkr_internal bool8_t vkr_mesh_loader_gltf_write_literal_line(
    FileHandle *file, const char *literal) {
  if (!literal || literal[0] == '\0') {
    return true_v;
  }
  String8 line = string8_create_from_cstr((const uint8_t *)literal,
                                          string_length(literal));
  return vkr_mesh_loader_gltf_write_line(file, line);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_string(
    FileHandle *file, VkrAllocator *allocator, const char *key, String8 value) {
  if (!file || !allocator || !key || !value.str || value.length == 0) {
    return false_v;
  }

  return vkr_mesh_loader_gltf_write_line(
      file, string8_create_formatted(allocator, "%s=%.*s", key,
                                     (int32_t)value.length, value.str));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_f32(
    FileHandle *file, VkrAllocator *allocator, const char *key,
    float32_t value) {
  if (!file || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      file, string8_create_formatted(allocator, "%s=%f", key, value));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_vec3(
    FileHandle *file, VkrAllocator *allocator, const char *key, Vec3 value) {
  if (!file || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      file, string8_create_formatted(allocator, "%s=%f,%f,%f", key, value.x,
                                     value.y, value.z));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_vec4(
    FileHandle *file, VkrAllocator *allocator, const char *key, Vec4 value) {
  if (!file || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      file, string8_create_formatted(allocator, "%s=%f,%f,%f,%f", key,
                                     value.x, value.y, value.z, value.w));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_optional_texture_line(
    FileHandle *file, VkrAllocator *allocator,
    const VkrMeshLoaderGltfTextureWriteLine *line) {
  if (!file || !allocator || !line) {
    return false_v;
  }
  if (!line->value.str || line->value.length == 0) {
    return true_v;
  }
  if (!vkr_mesh_loader_gltf_write_literal_line(file, line->prefix_literal)) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_key_string(file, allocator, line->key,
                                                line->value);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_material_file(
    const VkrMeshLoaderGltfParseInfo *info, String8 material_id,
    const cgltf_material *material, String8 material_path) {
  if (!info || !material_id.str || material_id.length == 0 ||
      !material_path.str || material_path.length == 0) {
    return false_v;
  }

  FilePath file_path =
      file_path_create((const char *)material_path.str, info->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle file = {0};
  FileError open_error = file_open(&file_path, mode, &file);
  if (open_error != FILE_ERROR_NONE) {
    log_error("MeshLoader(glTF): failed to open generated material '%s': %s",
              file_path.path.str, file_get_error_string(open_error).str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
  Vec4 base_color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
  float32_t metallic = 1.0f;
  float32_t roughness = 1.0f;
  if (material->has_pbr_metallic_roughness) {
    base_color = vec4_new(pbr->base_color_factor[0], pbr->base_color_factor[1],
                          pbr->base_color_factor[2], pbr->base_color_factor[3]);
    metallic = (float32_t)pbr->metallic_factor;
    roughness = (float32_t)pbr->roughness_factor;
  }

  float32_t normal_scale = material->normal_texture.scale != 0.0f
                               ? (float32_t)material->normal_texture.scale
                               : 1.0f;
  float32_t occlusion_strength =
      material->occlusion_texture.scale != 0.0f
          ? (float32_t)material->occlusion_texture.scale
          : 1.0f;
  Vec3 emissive_factor =
      vec3_new(material->emissive_factor[0], material->emissive_factor[1],
               material->emissive_factor[2]);

  float32_t alpha_cutoff = (float32_t)material->alpha_cutoff;
  if (material->alpha_mode == cgltf_alpha_mode_mask && alpha_cutoff <= 0.0f) {
    alpha_cutoff = 0.5f;
  }

  String8 alpha_mode =
      vkr_mesh_loader_gltf_alpha_mode_to_string(material->alpha_mode);

  String8 base_color_texture = {0};
  String8 metallic_roughness_texture = {0};
  String8 occlusion_texture = {0};
  String8 normal_texture = {0};
  String8 emissive_texture = {0};

  if (!vkr_mesh_loader_gltf_resolve_texture_path(info, &pbr->base_color_texture,
                                                 "cs=srgb&tc=color_srgb",
                                                 &base_color_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &pbr->metallic_roughness_texture, "tc=data_mask",
          &metallic_roughness_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->occlusion_texture, "tc=data_mask",
          &occlusion_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->normal_texture, "tc=normal_rg", &normal_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->emissive_texture, "cs=srgb&tc=color_srgb",
          &emissive_texture)) {
    file_close(&file);
    return false_v;
  }

  bool8_t ok = true_v;
  ok = ok && vkr_mesh_loader_gltf_write_key_string(
                 &file, info->load_allocator, "name", material_id);
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(&file, "type=pbr");
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(
                 &file, "base_color_colorspace=srgb");
  ok = ok && vkr_mesh_loader_gltf_write_key_vec4(
                 &file, info->load_allocator, "base_color", base_color);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                 &file, info->load_allocator, "metallic", metallic);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                 &file, info->load_allocator, "roughness", roughness);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                 &file, info->load_allocator, "normal_scale", normal_scale);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&file, info->load_allocator,
                                                 "occlusion_strength",
                                                 occlusion_strength);
  ok = ok && vkr_mesh_loader_gltf_write_key_vec3(
                 &file, info->load_allocator, "emissive_factor",
                 emissive_factor);
  ok = ok && vkr_mesh_loader_gltf_write_key_string(
                 &file, info->load_allocator, "alpha_mode", alpha_mode);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                 &file, info->load_allocator, "alpha_cutoff", alpha_cutoff);
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(
                 &file, "shader=shader.pbr.world");
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(&file, "pipeline=world");

  if (ok) {
    const VkrMeshLoaderGltfTextureWriteLine texture_lines[] = {
        {.key = "base_color_texture",
         .value = base_color_texture,
         .prefix_literal = NULL},
        {.key = "metallic_roughness_texture",
         .value = metallic_roughness_texture,
         .prefix_literal = NULL},
        {.key = "occlusion_texture",
         .value = occlusion_texture,
         .prefix_literal = NULL},
        {.key = "normal_texture",
         .value = normal_texture,
         .prefix_literal = NULL},
        {.key = "emissive_texture",
         .value = emissive_texture,
         .prefix_literal = "emissive_colorspace=srgb"},
    };

    for (uint32_t i = 0; i < ArrayCount(texture_lines); ++i) {
      ok = ok && vkr_mesh_loader_gltf_write_optional_texture_line(
                     &file, info->load_allocator, &texture_lines[i]);
    }
  }

  file_close(&file);
  if (!ok) {
    log_error("MeshLoader(glTF): failed writing generated material '%s'",
              file_path.path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_material_files(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    String8 *material_paths, Vector_String8 *out_generated_material_paths) {
  if (!info || !data || !material_paths) {
    return false_v;
  }
  if (data->materials_count == 0) {
    return true_v;
  }

  String8 material_dir = string8_create_formatted(
      info->load_allocator, "assets/materials/%.*s",
      (int32_t)info->source_stem.length, info->source_stem.str);
  FilePath material_dir_path =
      file_path_create((const char *)material_dir.str, info->load_allocator,
                       FILE_PATH_TYPE_RELATIVE);
  if (!file_ensure_directory(info->load_allocator, &material_dir_path.path)) {
    log_error("MeshLoader(glTF): failed to create material directory '%s'",
              string8_cstr(&material_dir));
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint64_t source_hash = vkr_mesh_loader_gltf_hash_source_path(info->source_path);
  for (uint32_t i = 0; i < (uint32_t)data->materials_count; ++i) {
    String8 material_id = vkr_mesh_loader_gltf_make_material_id(
        info->load_allocator, source_hash, i);
    material_paths[i] = string8_create_formatted(
        info->load_allocator, "%.*s/%.*s.mt", (int32_t)material_dir.length,
        material_dir.str, (int32_t)material_id.length, material_id.str);
    if (!vkr_mesh_loader_gltf_write_material_file(
            info, material_id, &data->materials[i], material_paths[i])) {
      return false_v;
    }
    if (out_generated_material_paths) {
      vkr_mesh_loader_gltf_push_unique_path(out_generated_material_paths,
                                            material_paths[i],
                                            info->load_allocator);
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_read_vec2(
    const cgltf_accessor *accessor, cgltf_size index, Vec2 *out_value) {
  if (!out_value) {
    return false_v;
  }
  *out_value = vec2_zero();
  if (!accessor) {
    return true_v;
  }
  cgltf_float value[2] = {0.0f, 0.0f};
  if (!cgltf_accessor_read_float(accessor, index, value, 2)) {
    return false_v;
  }
  *out_value = vec2_new((float32_t)value[0], (float32_t)value[1]);
  return true_v;
}

vkr_internal bool8_t
vkr_mesh_loader_gltf_read_vec3(const cgltf_accessor *accessor, cgltf_size index,
                               Vec3 *out_value, Vec3 default_value) {
  if (!out_value) {
    return false_v;
  }
  *out_value = default_value;
  if (!accessor) {
    return true_v;
  }

  cgltf_float value[3] = {default_value.x, default_value.y, default_value.z};
  if (!cgltf_accessor_read_float(accessor, index, value, 3)) {
    return false_v;
  }
  *out_value =
      vec3_new((float32_t)value[0], (float32_t)value[1], (float32_t)value[2]);
  return true_v;
}

vkr_internal bool8_t
vkr_mesh_loader_gltf_read_vec4(const cgltf_accessor *accessor, cgltf_size index,
                               Vec4 *out_value, Vec4 default_value) {
  if (!out_value) {
    return false_v;
  }
  *out_value = default_value;
  if (!accessor) {
    return true_v;
  }

  cgltf_float value[4] = {default_value.x, default_value.y, default_value.z,
                          default_value.w};
  if (!cgltf_accessor_read_float(accessor, index, value, 4)) {
    return false_v;
  }
  *out_value = vec4_new((float32_t)value[0], (float32_t)value[1],
                        (float32_t)value[2], (float32_t)value[3]);
  return true_v;
}

vkr_internal Vec3 vkr_mesh_loader_gltf_transform_position(Mat4 world,
                                                          Vec3 position) {
  Vec4 p =
      mat4_mul_vec4(world, vec4_new(position.x, position.y, position.z, 1.0f));
  return vec3_new(p.x, p.y, p.z);
}

vkr_internal Vec3 vkr_mesh_loader_gltf_transform_direction(Mat4 normal_matrix,
                                                           Vec3 direction,
                                                           Vec3 fallback) {
  Vec4 d = mat4_mul_vec4(normal_matrix,
                         vec4_new(direction.x, direction.y, direction.z, 0.0f));
  Vec3 value = vec3_new(d.x, d.y, d.z);
  float32_t len_sq = vec3_length_squared(value);
  if (len_sq <= VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON) {
    return fallback;
  }
  return vec3_normalize(value);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_emit_primitive(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const cgltf_primitive *primitive, Mat4 world, Mat4 normal_matrix,
    const String8 *material_paths, uint32_t *in_out_primitive_count) {
  if (!info || !data || !primitive || !in_out_primitive_count) {
    return false_v;
  }

  cgltf_primitive_type primitive_type = primitive->type;
  if (primitive_type == cgltf_primitive_type_invalid) {
    primitive_type = cgltf_primitive_type_triangles;
  }

  if (primitive_type != cgltf_primitive_type_triangles) {
    log_warn("MeshLoader(glTF): skipping unsupported primitive mode %d",
             (int)primitive_type);
    return true_v;
  }

  const cgltf_accessor *position_accessor =
      cgltf_find_accessor(primitive, cgltf_attribute_type_position, 0);
  if (!position_accessor || position_accessor->count == 0) {
    log_error("MeshLoader(glTF): primitive is missing required POSITION");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  const cgltf_accessor *normal_accessor =
      cgltf_find_accessor(primitive, cgltf_attribute_type_normal, 0);
  const cgltf_accessor *tangent_accessor =
      cgltf_find_accessor(primitive, cgltf_attribute_type_tangent, 0);
  const cgltf_accessor *texcoord_accessor =
      cgltf_find_accessor(primitive, cgltf_attribute_type_texcoord, 0);
  const cgltf_accessor *color_accessor =
      cgltf_find_accessor(primitive, cgltf_attribute_type_color, 0);

  const cgltf_size pos_count = position_accessor->count;
  const cgltf_size idx_count =
      primitive->indices ? primitive->indices->count : pos_count;
  if (pos_count == 0 || idx_count == 0) {
    return true_v;
  }
  if (pos_count > (cgltf_size)UINT32_MAX ||
      (primitive->indices && primitive->indices->count > (cgltf_size)UINT32_MAX)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  const uint32_t vertex_count = (uint32_t)pos_count;
  const uint32_t index_count = (uint32_t)idx_count;

  VkrAllocatorScope primitive_scope =
      vkr_allocator_begin_scope(info->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&primitive_scope)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }

  VkrVertex3d *vertices = (VkrVertex3d *)vkr_allocator_alloc(
      info->scratch_allocator, sizeof(VkrVertex3d) * vertex_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *indices = (uint32_t *)vkr_allocator_alloc(
      info->scratch_allocator, sizeof(uint32_t) * index_count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!vertices || !indices) {
    vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }

  for (uint32_t i = 0; i < vertex_count; ++i) {
    Vec3 position = vec3_zero();
    Vec3 normal = vec3_new(0.0f, 1.0f, 0.0f);
    Vec4 tangent = vec4_new(1.0f, 0.0f, 0.0f, 1.0f);
    Vec2 texcoord = vec2_zero();
    Vec4 color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);

    if (!vkr_mesh_loader_gltf_read_vec3(position_accessor, i, &position,
                                        vec3_zero()) ||
        !vkr_mesh_loader_gltf_read_vec3(normal_accessor, i, &normal,
                                        vec3_new(0.0f, 1.0f, 0.0f)) ||
        !vkr_mesh_loader_gltf_read_vec4(tangent_accessor, i, &tangent,
                                        vec4_new(1.0f, 0.0f, 0.0f, 1.0f)) ||
        !vkr_mesh_loader_gltf_read_vec2(texcoord_accessor, i, &texcoord) ||
        !vkr_mesh_loader_gltf_read_vec4(color_accessor, i, &color,
                                        vec4_new(1.0f, 1.0f, 1.0f, 1.0f))) {
      vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      log_error("MeshLoader(glTF): failed to read primitive accessor data");
      return false_v;
    }

    Vec3 world_position =
        vkr_mesh_loader_gltf_transform_position(world, position);
    Vec3 world_normal = vkr_mesh_loader_gltf_transform_direction(
        normal_matrix, normal, vec3_new(0.0f, 1.0f, 0.0f));
    Vec3 world_tangent = vkr_mesh_loader_gltf_transform_direction(
        normal_matrix, vec3_new(tangent.x, tangent.y, tangent.z),
        vec3_new(1.0f, 0.0f, 0.0f));

    vertices[i].position = vkr_vertex_pack_vec3(world_position);
    vertices[i].normal = vkr_vertex_pack_vec3(world_normal);
    vertices[i].texcoord = texcoord;
    vertices[i].colour = color;
    vertices[i].tangent =
        vec4_new(world_tangent.x, world_tangent.y, world_tangent.z, tangent.w);
  }

  if (primitive->indices) {
    for (uint32_t i = 0; i < index_count; ++i) {
      cgltf_size index = cgltf_accessor_read_index(primitive->indices, i);
      if (index >= vertex_count) {
        vkr_allocator_end_scope(&primitive_scope,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        log_error("MeshLoader(glTF): invalid index %llu (vertex_count=%u)",
                  (unsigned long long)index, vertex_count);
        vkr_mesh_loader_gltf_set_error(info,
                                       VKR_RENDERER_ERROR_INVALID_PARAMETER);
        return false_v;
      }
      indices[i] = (uint32_t)index;
    }
  } else {
    for (uint32_t i = 0; i < index_count; ++i) {
      indices[i] = i;
    }
  }

  String8 material_path = {0};
  if (primitive->material && material_paths) {
    cgltf_size material_index = cgltf_material_index(data, primitive->material);
    if (material_index < data->materials_count) {
      material_path = material_paths[material_index];
    }
  }

  VkrMeshLoaderGltfPrimitive out_primitive = {
      .vertices = vertices,
      .vertex_count = vertex_count,
      .indices = indices,
      .index_count = index_count,
      .material_path = material_path,
  };
  if (!info->on_primitive ||
      !info->on_primitive(info->user_data, &out_primitive)) {
    vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_gltf_set_error(info,
                                   VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    return false_v;
  }

  (*in_out_primitive_count)++;
  vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_emit_node(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const cgltf_node *node, const String8 *material_paths,
    uint32_t *in_out_primitive_count) {
  if (!info || !data || !node || !in_out_primitive_count) {
    return false_v;
  }

  cgltf_float world_values[16] = {0};
  cgltf_node_transform_world(node, world_values);
  Mat4 world = vkr_mesh_loader_gltf_mat4_from_cgltf(world_values);
  Mat4 normal_matrix = mat4_transpose(mat4_inverse(world));

  if (node->mesh) {
    for (uint32_t primitive_index = 0;
         primitive_index < (uint32_t)node->mesh->primitives_count;
         ++primitive_index) {
      if (!vkr_mesh_loader_gltf_emit_primitive(
              info, data, &node->mesh->primitives[primitive_index], world,
              normal_matrix, material_paths, in_out_primitive_count)) {
        return false_v;
      }
    }
  }

  for (uint32_t i = 0; i < (uint32_t)node->children_count; ++i) {
    if (!vkr_mesh_loader_gltf_emit_node(info, data, node->children[i],
                                        material_paths,
                                        in_out_primitive_count)) {
      return false_v;
    }
  }

  return true_v;
}

vkr_internal void vkr_mesh_loader_gltf_collect_dependencies(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data) {
  if (!info || !data || !info->out_dependency_paths) {
    return;
  }

  vkr_mesh_loader_gltf_push_unique_path(
      info->out_dependency_paths, info->source_path, info->load_allocator);

  for (uint32_t i = 0; i < (uint32_t)data->buffers_count; ++i) {
    const cgltf_buffer *buffer = &data->buffers[i];
    if (!buffer->uri) {
      continue;
    }
    String8 uri = string8_create_from_cstr((const uint8_t *)buffer->uri,
                                           string_length(buffer->uri));
    if (vkr_string8_starts_with(&uri, "data:")) {
      continue;
    }
    String8 resolved =
        vkr_mesh_loader_gltf_path_is_absolute(uri)
            ? string8_duplicate(info->load_allocator, &uri)
            : file_path_join(info->load_allocator, info->source_dir, uri);
    if (vkr_mesh_loader_gltf_path_exists(info->load_allocator, resolved)) {
      vkr_mesh_loader_gltf_push_unique_path(info->out_dependency_paths,
                                            resolved, info->load_allocator);
    }
  }

  for (uint32_t i = 0; i < (uint32_t)data->images_count; ++i) {
    const cgltf_image *image = &data->images[i];
    if (!image->uri) {
      continue;
    }

    String8 uri = string8_create_from_cstr((const uint8_t *)image->uri,
                                           string_length(image->uri));
    if (vkr_string8_starts_with(&uri, "data:")) {
      continue;
    }

    bool8_t found = false_v;
    String8 existing_path = {0};
    (void)vkr_mesh_loader_gltf_resolve_relative_texture_uri(
        info, uri, &found, &existing_path, false_v);
    if (found && existing_path.str && existing_path.length > 0) {
      vkr_mesh_loader_gltf_push_unique_path(
          info->out_dependency_paths, existing_path, info->load_allocator);
    }
  }
}

vkr_internal bool8_t vkr_mesh_loader_gltf_run_parse(
    const VkrMeshLoaderGltfParseInfo *info, bool8_t emit_primitives) {
  if (!info || !info->load_allocator || !info->scratch_allocator ||
      !info->source_path.str || info->source_path.length == 0 ||
      (emit_primitives && !info->on_primitive)) {
    return false_v;
  }

  vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_NONE);

  String8 cstr_path = string8_create_formatted(
      info->load_allocator, "%.*s", (int32_t)info->source_path.length,
      info->source_path.str);

  cgltf_options options = {0};
  cgltf_data *data = NULL;
  cgltf_result parse_result =
      cgltf_parse_file(&options, (const char *)cstr_path.str, &data);
  if (parse_result != cgltf_result_success || !data) {
    log_error("MeshLoader(glTF): failed to parse '%s' (result=%d)",
              string8_cstr(&cstr_path), (int)parse_result);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  bool8_t ok = true_v;
  do {
    cgltf_result load_buffers_result =
        cgltf_load_buffers(&options, data, (const char *)cstr_path.str);
    if (load_buffers_result != cgltf_result_success) {
      log_error("MeshLoader(glTF): failed to load buffers for '%s' (result=%d)",
                string8_cstr(&cstr_path), (int)load_buffers_result);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }

    cgltf_result validate_result = cgltf_validate(data);
    if (validate_result != cgltf_result_success) {
      log_error("MeshLoader(glTF): validation failed for '%s' (result=%d)",
                string8_cstr(&cstr_path), (int)validate_result);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }

    vkr_mesh_loader_gltf_collect_dependencies(info, data);

    String8 *material_paths = NULL;
    if (data->materials_count > 0) {
      material_paths = (String8 *)vkr_allocator_alloc(
          info->load_allocator, sizeof(String8) * data->materials_count,
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!material_paths) {
        vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
        ok = false_v;
        break;
      }
      MemZero(material_paths, sizeof(String8) * data->materials_count);
      if (!vkr_mesh_loader_gltf_write_material_files(
              info, data, material_paths, info->out_generated_material_paths)) {
        ok = false_v;
        break;
      }
    }

    if (!emit_primitives) {
      break;
    }

    uint32_t primitive_count = 0;
    const cgltf_scene *scene =
        data->scene ? data->scene
                    : (data->scenes_count > 0 ? &data->scenes[0] : NULL);

    if (scene) {
      for (uint32_t i = 0; i < (uint32_t)scene->nodes_count; ++i) {
        if (!vkr_mesh_loader_gltf_emit_node(info, data, scene->nodes[i],
                                            material_paths, &primitive_count)) {
          ok = false_v;
          break;
        }
      }
    } else {
      for (uint32_t i = 0; i < (uint32_t)data->nodes_count; ++i) {
        const cgltf_node *node = &data->nodes[i];
        if (node->parent) {
          continue;
        }
        if (!vkr_mesh_loader_gltf_emit_node(info, data, node, material_paths,
                                            &primitive_count)) {
          ok = false_v;
          break;
        }
      }
    }

    if (!ok) {
      break;
    }

    if (primitive_count == 0) {
      log_error("MeshLoader(glTF): no renderable triangle primitives in '%s'",
                string8_cstr(&cstr_path));
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }
  } while (false);

  if (data) {
    cgltf_free(data);
  }

  return ok;
}

bool8_t vkr_mesh_loader_gltf_parse(const VkrMeshLoaderGltfParseInfo *info) {
  return vkr_mesh_loader_gltf_run_parse(info, true_v);
}

bool8_t vkr_mesh_loader_gltf_generate_materials(
    const VkrMeshLoaderGltfParseInfo *info) {
  return vkr_mesh_loader_gltf_run_parse(info, false_v);
}
