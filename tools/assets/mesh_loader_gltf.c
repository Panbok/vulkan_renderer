#include "assets/mesh_loader_gltf.h"
#include "assets/vkr_cgltf.h"
#include "assets/vkr_gltf_material_conversion.h"
#include "assets/vkr_image_decode.h"
#include "assets/vkr_mesh_encode.h"
#include "assets/vkr_meshoptimizer_bridge.h"
#include "vkr_color_transfer.h"
#include "vkr_geometry_data.h"

#include <cgltf.h>
#include <math.h>
#include <stb_image.h>
#include <stdlib.h>
#include <string.h>

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_hash.h"
#include "core/vkr_json.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "math/mat.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_vkt_packer.h"

#define VKR_FNV1A64_OFFSET_BASIS 0xcbf29ce484222325ull
#define VKR_FNV1A64_PRIME 0x100000001b3ull
#define VKR_GLTF_SPEC_GLOSS_CACHE_VERSION 3u
#define VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX 4096u
#define VKR_GLTF_DECAL_NORMAL_OFFSET_MAX_METERS 0.1
#define VKR_GLTF_IMPORT_SIDECAR_VERSION 1

typedef struct VkrMeshLoaderGltfDecalOverrides {
  float32_t *offsets;
  uint32_t count;
} VkrMeshLoaderGltfDecalOverrides;

vkr_internal void
vkr_mesh_loader_gltf_set_error(const VkrMeshLoaderGltfParseInfo *info,
                               VkrRendererError error) {
  if (info && info->out_error) {
    *info->out_error = error;
  }
}

/* Managed writes use explicit absolute paths; legacy callers keep their roots.
 */
vkr_internal String8 vkr_mesh_loader_gltf_output_path(
    const VkrMeshLoaderGltfParseInfo *info, String8 legacy_path) {
  if (!info->bundle_root.length) {
    return legacy_path;
  }
  /* Derived names encode source content and parameters, so a shared root
   * lets later imports reuse them; bundling clones what materials use. */
  const String8 generated = string8_lit("assets/textures/generated/");
  if (info->generated_root.length && legacy_path.length > generated.length &&
      MemCompare(legacy_path.str, generated.str, generated.length) == 0) {
    String8 suffix =
        string8_substring(&legacy_path, generated.length, legacy_path.length);
    return file_path_join(info->load_allocator, info->generated_root, suffix);
  }
  String8 suffix = string8_substring(&legacy_path, 7, legacy_path.length);
  return file_path_join(info->load_allocator, info->bundle_root, suffix);
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

  uint64_t start = 0;
  while (start + 1 < source_path.length && source_path.str[start] == '.' &&
         (source_path.str[start + 1] == '/' ||
          source_path.str[start + 1] == '\\')) {
    start += 2;
  }
  for (uint64_t i = start; i < source_path.length; ++i) {
    const bool8_t segment_start = i == start || source_path.str[i - 1] == '/' ||
                                  source_path.str[i - 1] == '\\';
    if (!segment_start) {
      continue;
    }
    const uint64_t remaining = source_path.length - i;
    const bool8_t assets =
        remaining > 7 && MemCompare(source_path.str + i, "assets", 6) == 0 &&
        (source_path.str[i + 6] == '/' || source_path.str[i + 6] == '\\');
    const bool8_t tests =
        remaining > 6 && MemCompare(source_path.str + i, "tests", 5) == 0 &&
        (source_path.str[i + 5] == '/' || source_path.str[i + 5] == '\\');
    if (assets || tests) {
      start = i;
      break;
    }
  }

  uint64_t hash = VKR_FNV1A64_OFFSET_BASIS;
  for (uint64_t i = start; i < source_path.length; ++i) {
    const uint8_t byte =
        source_path.str[i] == '\\' ? (uint8_t)'/' : source_path.str[i];
    hash ^= (uint64_t)byte;
    hash *= VKR_FNV1A64_PRIME;
  }

  return hash;
}

vkr_internal uint64_t vkr_mesh_loader_gltf_hash_bytes(uint64_t hash,
                                                      const void *data,
                                                      uint64_t size) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (uint64_t i = 0; i < size; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= VKR_FNV1A64_PRIME;
  }
  return hash;
}

vkr_internal String8 vkr_mesh_loader_gltf_make_material_id(
    VkrAllocator *allocator, uint64_t source_hash, uint32_t material_index) {
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

vkr_internal bool8_t vkr_mesh_loader_gltf_path_equals(String8 lhs,
                                                      String8 rhs) {
#if defined(PLATFORM_WINDOWS)
  return string8_equalsi(&lhs, &rhs);
#else
  return string8_equals(&lhs, &rhs);
#endif
}

vkr_internal bool8_t vkr_mesh_loader_gltf_push_unique_path(
    Array_String8 *paths, String8 value, VkrAllocator *allocator) {
  if (!paths || !allocator || !value.str || value.length == 0) {
    return true_v;
  }

  for (uint64_t i = 0; i < paths->length; ++i) {
    String8 *existing = array_get_String8(paths, i);
    if (existing && vkr_mesh_loader_gltf_path_equals(*existing, value)) {
      return true_v;
    }
  }

  String8 copy = string8_duplicate(allocator, &value);
  return copy.str && array_push_String8(paths, copy);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_publish_pair_paths(
    Array_String8 *paths, String8 normal_path, String8 roughness_path,
    VkrAllocator *allocator) {
  if (!paths) {
    return true_v;
  }

  bool8_t have_normal = false_v;
  bool8_t have_roughness = false_v;
  for (uint64_t i = 0; i < paths->length; ++i) {
    String8 *existing = array_get_String8(paths, i);
    have_normal =
        have_normal ||
        (existing && vkr_mesh_loader_gltf_path_equals(*existing, normal_path));
    have_roughness =
        have_roughness || (existing && vkr_mesh_loader_gltf_path_equals(
                                           *existing, roughness_path));
  }

  const uint64_t additions =
      (have_normal ? 0u : 1u) + (have_roughness ? 0u : 1u);
  if (additions == 0u) {
    return true_v;
  }
  if (paths->length > UINT64_MAX - additions ||
      !array_reserve_String8(paths, paths->length + additions)) {
    return false_v;
  }

  String8 normal_copy =
      have_normal ? (String8){0} : string8_duplicate(allocator, &normal_path);
  String8 roughness_copy = have_roughness
                               ? (String8){0}
                               : string8_duplicate(allocator, &roughness_path);
  if ((!have_normal && !normal_copy.str) ||
      (!have_roughness && !roughness_copy.str)) {
    return false_v;
  }
  return (have_normal || array_push_String8(paths, normal_copy)) &&
         (have_roughness || array_push_String8(paths, roughness_copy));
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
  if (info->bundle_root.length) {
    *out_found = vkr_mesh_loader_gltf_find_existing_texture_file(
        info->load_allocator, source_candidate, out_existing_path);
    return source_candidate;
  }
  String8 assets_candidate =
      file_path_join(info->load_allocator, string8_lit("assets"), uri);
  String8 assets_textures_uri_candidate =
      file_path_join(info->load_allocator, string8_lit("assets/textures"), uri);
  String8 legacy_objects_candidate = {0};
  const bool8_t has_legacy_objects_candidate =
      uri.length > sizeof("objects/") - 1u &&
      vkr_string8_starts_with(&uri, "objects/");
  if (has_legacy_objects_candidate) {
    String8 texture_relative =
        string8_substring(&uri, sizeof("objects/") - 1u, uri.length);
    legacy_objects_candidate = string8_create_formatted(
        info->load_allocator, "assets/textures/%.*s",
        (int32_t)texture_relative.length, texture_relative.str);
  }
  String8 basename = vkr_mesh_loader_gltf_basename_view(uri);
  String8 assets_textures_basename_candidate =
      basename.length > 0
          ? file_path_join(info->load_allocator, string8_lit("assets/textures"),
                           basename)
          : assets_textures_uri_candidate;

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
  if (vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, assets_textures_uri_candidate,
          out_existing_path)) {
    *out_found = true_v;
    return assets_textures_uri_candidate;
  }
  if (has_legacy_objects_candidate &&
      vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, legacy_objects_candidate, out_existing_path)) {
    *out_found = true_v;
    return legacy_objects_candidate;
  }
  if (!string8_equals(&assets_textures_uri_candidate,
                      &assets_textures_basename_candidate) &&
      vkr_mesh_loader_gltf_find_existing_texture_file(
          info->load_allocator, assets_textures_basename_candidate,
          out_existing_path)) {
    *out_found = true_v;
    return assets_textures_basename_candidate;
  }

  if (log_missing) {
    if (has_legacy_objects_candidate) {
      log_warn("MeshLoader(glTF): texture '%.*s' not found; tried '%.*s', "
               "'%.*s', '%.*s', '%.*s', '%.*s'",
               (int32_t)uri.length, uri.str, (int32_t)source_candidate.length,
               source_candidate.str, (int32_t)assets_candidate.length,
               assets_candidate.str,
               (int32_t)assets_textures_uri_candidate.length,
               assets_textures_uri_candidate.str,
               (int32_t)legacy_objects_candidate.length,
               legacy_objects_candidate.str,
               (int32_t)assets_textures_basename_candidate.length,
               assets_textures_basename_candidate.str);
    } else {
      log_warn("MeshLoader(glTF): texture '%.*s' not found; tried '%.*s', "
               "'%.*s', '%.*s', '%.*s'",
               (int32_t)uri.length, uri.str, (int32_t)source_candidate.length,
               source_candidate.str, (int32_t)assets_candidate.length,
               assets_candidate.str,
               (int32_t)assets_textures_uri_candidate.length,
               assets_textures_uri_candidate.str,
               (int32_t)assets_textures_basename_candidate.length,
               assets_textures_basename_candidate.str);
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

vkr_internal String8 vkr_mesh_loader_gltf_strip_query(String8 path) {
  for (uint64_t i = 0; i < path.length; ++i) {
    if (path.str[i] == '?') {
      return string8_substring(&path, 0, i);
    }
  }
  return path;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_has_vkt_extension(String8 path) {
  const String8 extension = string8_lit(".vkt");
  if (path.length < extension.length) {
    return false_v;
  }
  String8 suffix =
      string8_substring(&path, path.length - extension.length, path.length);
  return string8_equalsi(&suffix, &extension);
}

/* Names a cutout's source image file: its path, in the scratch allocator,
   and the hash of its bytes. A packed texture cannot be a cutout source. */
vkr_internal bool8_t vkr_mesh_loader_gltf_cutout_file_source(
    const VkrMeshLoaderGltfParseInfo *info, String8 source_path,
    VkrVktSource *out_source, uint64_t *out_hash) {
  if (vkr_mesh_loader_gltf_has_vkt_extension(source_path)) {
    log_error("MeshLoader(glTF): cannot bake cutout variant from packed source "
              "'%.*s'; the source image is required",
              (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  String8 source_sidecar =
      string8_create_formatted(info->scratch_allocator, "%.*s.vkt",
                               (int32_t)source_path.length, source_path.str);
  if (!vkr_mesh_loader_gltf_path_exists(info->scratch_allocator, source_path) &&
      source_sidecar.str &&
      vkr_mesh_loader_gltf_path_exists(info->scratch_allocator,
                                       source_sidecar)) {
    log_error(
        "MeshLoader(glTF): cannot bake cutout variant from packed sidecar "
        "'%.*s'; the source image '%.*s' is required",
        (int32_t)source_sidecar.length, source_sidecar.str,
        (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  String8 source_cstr =
      string8_create_formatted(info->scratch_allocator, "%.*s",
                               (int32_t)source_path.length, source_path.str);
  if (!source_cstr.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  FilePath source_file =
      file_path_create((const char *)source_cstr.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(source_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&source_file, read_mode, &file) != FILE_ERROR_NONE) {
    log_error("MeshLoader(glTF): failed to open cutout source '%.*s'",
              (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }
  uint8_t *source_bytes = NULL;
  uint64_t source_size = 0u;
  const FileError read_error = file_read_all(&file, info->scratch_allocator,
                                             &source_bytes, &source_size);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !source_bytes || source_size == 0u) {
    log_error("MeshLoader(glTF): failed to read cutout source '%.*s'",
              (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  *out_hash = vkr_mesh_loader_gltf_hash_bytes(VKR_FNV1A64_OFFSET_BASIS,
                                              source_bytes, source_size);
  *out_source = (VkrVktSource){.path = (const char *)source_cstr.str};
  return true_v;
}

/* Bakes the cutout variant of a base color texture: the image file of
   `source_texture`, or `converted` pixels, which name the variant by their
   hash. A converted source without pixels returns VKR_VKT_PACK_STALE unless
   the variant is current. */
vkr_internal VkrVktPackResult vkr_mesh_loader_gltf_bake_cutout_variant(
    const VkrMeshLoaderGltfParseInfo *info, String8 source_texture,
    const VkrVktSource *converted, float32_t alpha_cutoff,
    float32_t alpha_factor, String8 *out_texture) {
  if (!info || !info->load_allocator || !info->scratch_allocator ||
      !out_texture ||
      (!converted && (!source_texture.str || source_texture.length == 0u))) {
    return VKR_VKT_PACK_FAILED;
  }
  *out_texture = (String8){0};

  VkrAllocatorScope scope = vkr_allocator_begin_scope(info->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return VKR_VKT_PACK_FAILED;
  }

  VkrVktPackResult result = VKR_VKT_PACK_FAILED;
  String8 source_path = converted
                            ? string8_lit("converted base color")
                            : vkr_mesh_loader_gltf_strip_query(source_texture);
  VkrVktSource source = {0};
  uint64_t source_hash = 0u;
  if (converted) {
    source = *converted;
    source_hash = converted->hash;
  } else if (!vkr_mesh_loader_gltf_cutout_file_source(info, source_path,
                                                      &source, &source_hash)) {
    goto cleanup;
  }

  uint32_t cutoff_bits = 0u;
  uint32_t factor_bits = 0u;
  MemCopy(&cutoff_bits, &alpha_cutoff, sizeof(cutoff_bits));
  MemCopy(&factor_bits, &alpha_factor, sizeof(factor_bits));
  String8 variant_path = string8_create_formatted(
      info->load_allocator,
      "assets/textures/generated/cutout_v%u/source_%016llx_cutoff_%08x_"
      "factor_%08x%s.vkt",
      VKR_VKT_CUTOUT_POLICY_VERSION, (unsigned long long)source_hash,
      cutoff_bits, factor_bits, vkr_vkt_variant_suffix());
  variant_path = vkr_mesh_loader_gltf_output_path(info, variant_path);
  if (!variant_path.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }

  const VkrVktPackResult packed = vkr_vkt_pack_cutout(
      &source, (const char *)variant_path.str, alpha_cutoff, alpha_factor);
  if (packed == VKR_VKT_PACK_STALE) {
    result = VKR_VKT_PACK_STALE;
    goto cleanup;
  }
  if (packed != VKR_VKT_PACK_SUCCESS) {
    log_error("MeshLoader(glTF): cutout pack failed for '%.*s'",
              (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info,
                                   VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    goto cleanup;
  }
  if (info->out_generated_asset_paths &&
      !vkr_mesh_loader_gltf_push_unique_path(info->out_generated_asset_paths,
                                             variant_path,
                                             info->load_allocator)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }
  *out_texture = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, variant_path, "cs=srgb&tc=color_srgb");
  result = out_texture->str ? VKR_VKT_PACK_SUCCESS : VKR_VKT_PACK_FAILED;
  if (result != VKR_VKT_PACK_SUCCESS) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
  }

cleanup:
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_pair_view_is_compatible(
    const cgltf_texture_view *view, const char *role) {
  if (!view || !view->texture || !view->texture->image ||
      !view->texture->image->uri) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s is not an external image",
             role);
    return false_v;
  }

  String8 uri =
      string8_create_from_cstr((const uint8_t *)view->texture->image->uri,
                               string_length(view->texture->image->uri));
  if (vkr_string8_starts_with(&uri, "data:")) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s is embedded",
             role);
    return false_v;
  }
  if (view->texcoord != 0u || view->has_transform) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s does not use untransformed UV0",
             role);
    return false_v;
  }

  const cgltf_sampler *sampler = view->texture->sampler;
  if (sampler &&
      (sampler->wrap_s != cgltf_wrap_mode_repeat ||
       sampler->wrap_t != cgltf_wrap_mode_repeat ||
       (sampler->mag_filter != cgltf_filter_type_undefined &&
        sampler->mag_filter != cgltf_filter_type_linear) ||
       (sampler->min_filter != cgltf_filter_type_undefined &&
        sampler->min_filter != cgltf_filter_type_linear_mipmap_linear))) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s sampler is not repeat/linear mip-linear",
             role);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_pair_source_is_compatible(
    const VkrMeshLoaderGltfParseInfo *info, String8 texture, const char *role) {
  String8 source_path = vkr_mesh_loader_gltf_strip_query(texture);
  if (!source_path.str || source_path.length == 0u ||
      vkr_mesh_loader_gltf_has_vkt_extension(source_path)) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s is packed or missing its source image",
             role);
    return false_v;
  }

  if (!vkr_mesh_loader_gltf_path_exists(info->scratch_allocator, source_path)) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "%s source image is unavailable",
             role);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_hash_pair_source(
    const VkrMeshLoaderGltfParseInfo *info, String8 source_path,
    String8 source_cstr, uint64_t *out_hash) {
  if (!info || !source_path.str || !source_cstr.str || !out_hash) {
    return false_v;
  }

  FilePath source_file =
      file_path_create((const char *)source_cstr.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(source_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  /* The packer's memoized FNV-1a, which its skip checks of this pair then
     reuse instead of reading the file again. */
  unsigned long long hash = 0u;
  if (!source_file.path.str ||
      !vkr_vkt_hash_file((const char *)source_file.path.str, &hash)) {
    log_error("MeshLoader(glTF): failed to read paired source '%.*s'",
              (int32_t)source_path.length, source_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }
  *out_hash = (uint64_t)hash;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_pair_mappings_are_compatible(
    const cgltf_material *material, const cgltf_texture_view *normal_view,
    const cgltf_texture_view *roughness_view) {
  if (!vkr_mesh_loader_gltf_pair_view_is_compatible(normal_view, "normal")) {
    return false_v;
  }
  if (!material->has_pbr_specular_glossiness) {
    return !roughness_view || !roughness_view->texture ||
                   !roughness_view->texture->image
               ? true_v
               : vkr_mesh_loader_gltf_pair_view_is_compatible(
                     roughness_view, "metallic-roughness");
  }

  const cgltf_pbr_specular_glossiness *spec_gloss =
      &material->pbr_specular_glossiness;
  const cgltf_texture_view *converted_sources[] = {
      &spec_gloss->diffuse_texture, &spec_gloss->specular_glossiness_texture};
  for (uint32_t i = 0; i < ArrayCount(converted_sources); ++i) {
    const cgltf_texture_view *source = converted_sources[i];
    if (!source->texture || !source->texture->image) {
      continue;
    }
    if (!vkr_mesh_loader_gltf_pair_view_is_compatible(
            source, i == 0u ? "spec-gloss diffuse" : "spec-gloss")) {
      return false_v;
    }
    if (source->texcoord != normal_view->texcoord) {
      log_warn(
          "MeshLoader(glTF): skipping paired normal/roughness bake because "
          "normal and converted spec-gloss sources use different UV sets");
      return false_v;
    }
  }
  return true_v;
}

/* Bakes the paired normal/roughness variant. Roughness comes from the image
   file of `roughness_texture`, or from `converted_roughness` pixels, which
   name the variant by their hash; a converted source without pixels returns
   VKR_VKT_PACK_STALE unless the variant is current. */
vkr_internal VkrVktPackResult
vkr_mesh_loader_gltf_bake_normal_roughness_variant(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    const cgltf_texture_view *normal_view,
    const cgltf_texture_view *roughness_view, String8 normal_texture,
    String8 roughness_texture, const VkrVktSource *converted_roughness,
    float32_t normal_scale, float32_t roughness_factor,
    String8 *out_normal_texture, String8 *out_roughness_texture) {
  if (!info || !info->load_allocator || !info->scratch_allocator || !material ||
      !out_normal_texture || !out_roughness_texture) {
    return VKR_VKT_PACK_FAILED;
  }
  *out_normal_texture = (String8){0};
  *out_roughness_texture = (String8){0};
  if (!normal_texture.str || normal_texture.length == 0u ||
      !vkr_mesh_loader_gltf_pair_mappings_are_compatible(material, normal_view,
                                                         roughness_view)) {
    return VKR_VKT_PACK_INCOMPATIBLE;
  }

  VkrAllocatorScope scope = vkr_allocator_begin_scope(info->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return VKR_VKT_PACK_FAILED;
  }

  VkrVktPackResult result = VKR_VKT_PACK_FAILED;
  String8 normal_source = vkr_mesh_loader_gltf_strip_query(normal_texture);
  const bool8_t has_roughness_file = !converted_roughness &&
                                     roughness_texture.str &&
                                     roughness_texture.length > 0u;
  const bool8_t has_roughness_source =
      converted_roughness != NULL || has_roughness_file;
  String8 roughness_source =
      has_roughness_file ? vkr_mesh_loader_gltf_strip_query(roughness_texture)
                         : (String8){0};
  if (!vkr_mesh_loader_gltf_pair_source_is_compatible(info, normal_texture,
                                                      "normal") ||
      (has_roughness_file &&
       !vkr_mesh_loader_gltf_pair_source_is_compatible(info, roughness_texture,
                                                       "metallic-roughness"))) {
    result = VKR_VKT_PACK_INCOMPATIBLE;
    goto cleanup;
  }

  String8 normal_cstr = {0};
  String8 roughness_cstr = {0};
  uint64_t normal_hash = 0u;
  uint64_t roughness_hash = 0u;
  normal_cstr = string8_create_formatted(info->scratch_allocator, "%.*s",
                                         (int32_t)normal_source.length,
                                         normal_source.str);
  roughness_cstr =
      has_roughness_file
          ? string8_create_formatted(info->scratch_allocator, "%.*s",
                                     (int32_t)roughness_source.length,
                                     roughness_source.str)
          : (String8){0};
  if (!normal_cstr.str || (has_roughness_file && !roughness_cstr.str)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }

  VkrAllocatorScope hash_scope =
      vkr_allocator_begin_scope(info->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&hash_scope)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }
  const bool8_t hashes_ok =
      vkr_mesh_loader_gltf_hash_pair_source(info, normal_source, normal_cstr,
                                            &normal_hash) &&
      (!has_roughness_file ||
       vkr_mesh_loader_gltf_hash_pair_source(info, roughness_source,
                                             roughness_cstr, &roughness_hash));
  if (converted_roughness) {
    roughness_hash = converted_roughness->hash;
  }
  vkr_allocator_end_scope(&hash_scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!hashes_ok) {
    goto cleanup;
  }

  normal_scale = normal_scale == 0.0f ? 0.0f : normal_scale;
  roughness_factor = roughness_factor == 0.0f ? 0.0f : roughness_factor;

  uint32_t normal_scale_bits = 0u;
  uint32_t roughness_factor_bits = 0u;
  MemCopy(&normal_scale_bits, &normal_scale, sizeof(normal_scale_bits));
  MemCopy(&roughness_factor_bits, &roughness_factor,
          sizeof(roughness_factor_bits));
  String8 recipe = has_roughness_source
                       ? string8_create_formatted(
                             info->load_allocator,
                             "assets/textures/generated/normalrough_v%u/"
                             "normal_%016llx_roughness_%016llx_scale_%08x_"
                             "factor_%08x",
                             VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION,
                             (unsigned long long)normal_hash,
                             (unsigned long long)roughness_hash,
                             normal_scale_bits, roughness_factor_bits)
                       : string8_create_formatted(
                             info->load_allocator,
                             "assets/textures/generated/normalrough_v%u/"
                             "normal_%016llx_roughness_missing_scale_%08x_"
                             "factor_%08x",
                             VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION,
                             (unsigned long long)normal_hash, normal_scale_bits,
                             roughness_factor_bits);
  recipe = vkr_mesh_loader_gltf_output_path(info, recipe);
  /* The encoded normal depends only on its source and scale; roughness and
   * its factor change only the paired roughness, so materials share it. */
  String8 normal_recipe = string8_create_formatted(
      info->load_allocator,
      "assets/textures/generated/normalrough_v%u/normal_%016llx_scale_%08x",
      VKR_VKT_NORMAL_ROUGHNESS_POLICY_VERSION, (unsigned long long)normal_hash,
      normal_scale_bits);
  normal_recipe = vkr_mesh_loader_gltf_output_path(info, normal_recipe);
  if (!recipe.str || !normal_recipe.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }
  /* A preview-tier or fast-encoded bake never takes a final bake's name. */
  const char *tier = vkr_vkt_variant_suffix();
  String8 normal_variant = string8_create_formatted(
      info->load_allocator, "%.*s_normal%s.vkt", (int32_t)normal_recipe.length,
      normal_recipe.str, tier);
  String8 roughness_variant =
      string8_create_formatted(info->load_allocator, "%.*s_metalrough%s.vkt",
                               (int32_t)recipe.length, recipe.str, tier);
  if (!normal_variant.str || !roughness_variant.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }

  const VkrVktSource roughness =
      converted_roughness
          ? *converted_roughness
          : (VkrVktSource){.path = (const char *)roughness_cstr.str};
  const VkrVktPackResult pack_result = vkr_vkt_pack_normal_roughness(
      (const char *)normal_cstr.str, has_roughness_source ? &roughness : NULL,
      (const char *)normal_variant.str, (const char *)roughness_variant.str,
      normal_scale, roughness_factor);
  if (pack_result == VKR_VKT_PACK_STALE) {
    result = VKR_VKT_PACK_STALE;
    goto cleanup;
  }
  if (pack_result == VKR_VKT_PACK_INCOMPATIBLE) {
    log_warn("MeshLoader(glTF): skipping paired normal/roughness bake because "
             "the source image dimensions differ");
    result = VKR_VKT_PACK_INCOMPATIBLE;
    goto cleanup;
  }
  if (pack_result != VKR_VKT_PACK_SUCCESS) {
    log_error("MeshLoader(glTF): paired normal/roughness pack failed for "
              "'%.*s'",
              (int32_t)normal_source.length, normal_source.str);
    vkr_mesh_loader_gltf_set_error(info,
                                   VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    goto cleanup;
  }

  String8 cooked_normal = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, normal_variant, "tc=normal_rg");
  String8 cooked_roughness = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, roughness_variant, "tc=data_mask");
  if (!cooked_normal.str || !cooked_roughness.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }
  if (!vkr_mesh_loader_gltf_publish_pair_paths(
          info->out_generated_asset_paths, normal_variant, roughness_variant,
          info->load_allocator)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    goto cleanup;
  }
  *out_normal_texture = cooked_normal;
  *out_roughness_texture = cooked_roughness;
  result = VKR_VKT_PACK_SUCCESS;

cleanup:
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_material_has_non_unit_vertex_alpha(
    const cgltf_data *data, const cgltf_material *material,
    bool8_t *out_non_unit) {
  if (!data || !material || !out_non_unit) {
    return false_v;
  }
  *out_non_unit = false_v;
  for (cgltf_size mesh_index = 0; mesh_index < data->meshes_count;
       ++mesh_index) {
    const cgltf_mesh *mesh = &data->meshes[mesh_index];
    for (cgltf_size primitive_index = 0;
         primitive_index < mesh->primitives_count; ++primitive_index) {
      const cgltf_primitive *primitive = &mesh->primitives[primitive_index];
      if (primitive->material != material) {
        continue;
      }
      const cgltf_accessor *color =
          cgltf_find_accessor(primitive, cgltf_attribute_type_color, 0);
      if (!color) {
        continue;
      }
      if (color->type == cgltf_type_vec3) {
        continue;
      }
      if (color->type != cgltf_type_vec4) {
        return false_v;
      }
      for (cgltf_size vertex = 0; vertex < color->count; ++vertex) {
        cgltf_float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (!cgltf_accessor_read_float(color, vertex, value, 4u) ||
            !isfinite(value[3])) {
          return false_v;
        }
        if (value[3] != 1.0f) {
          *out_non_unit = true_v;
          return true_v;
        }
      }
    }
  }
  return true_v;
}

typedef struct VkrMeshLoaderGltfEncodedImage {
  uint8_t *bytes;
  uint64_t size;
  String8 path;
} VkrMeshLoaderGltfEncodedImage;

typedef struct VkrMeshLoaderGltfDecodedImage {
  uint8_t *pixels;
  int32_t width;
  int32_t height;
} VkrMeshLoaderGltfDecodedImage;

typedef struct VkrMeshLoaderGltfPreparedSpecGloss {
  Vec4 base_color_factor;
  float32_t metallic_factor;
  float32_t roughness_factor;
  Vec3 dielectric_specular_factor;
  String8 base_color_texture;
  String8 metallic_roughness_texture;
} VkrMeshLoaderGltfPreparedSpecGloss;

/* A spec-gloss material converted to metallic-roughness, with factors.
   `textured` is false when the material has no textures to convert, and a
   uniform image becomes a factor instead. A repository cook publishes the
   images as PNGs (`base_path`, `metal_rough_path`), which the runtime reads
   as sources. A managed cook packs them itself, from the pixels it keeps
   (`base_pixels`, `metal_rough_pixels`, in the load allocator), named by
   their content hashes; after a memo hit the pixels stay null until a pack
   finds its output stale. */
typedef struct VkrMeshLoaderGltfConvertedSpecGloss {
  bool8_t textured;
  bool8_t base_uniform;
  bool8_t metal_rough_uniform;
  String8 base_path;
  String8 metal_rough_path;
  int32_t width;
  int32_t height;
  uint64_t base_hash;
  uint64_t metal_rough_hash;
  uint8_t *base_pixels;
  uint8_t *metal_rough_pixels;
  Vec4 base_color_factor;
  float32_t metallic_factor;
  float32_t roughness_factor;
  Vec3 dielectric_specular_factor;
} VkrMeshLoaderGltfConvertedSpecGloss;

vkr_internal void vkr_mesh_loader_gltf_publish_prepared_spec_gloss(
    const VkrMeshLoaderGltfParseInfo *info, String8 base_path,
    String8 metal_rough_path, Vec4 base_color_factor, float32_t metallic_factor,
    float32_t roughness_factor, Vec3 dielectric_specular_factor,
    VkrMeshLoaderGltfPreparedSpecGloss *out_prepared) {
  if (info->out_generated_asset_paths) {
    vkr_mesh_loader_gltf_push_unique_path(info->out_generated_asset_paths,
                                          base_path, info->load_allocator);
    vkr_mesh_loader_gltf_push_unique_path(info->out_generated_asset_paths,
                                          metal_rough_path,
                                          info->load_allocator);
  }
  out_prepared->base_color_factor = base_color_factor;
  out_prepared->metallic_factor = metallic_factor;
  out_prepared->roughness_factor = roughness_factor;
  out_prepared->dielectric_specular_factor = dielectric_specular_factor;
  out_prepared->base_color_texture = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, base_path, "cs=srgb&tc=color_srgb");
  out_prepared->metallic_roughness_texture = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, metal_rough_path, "tc=data_mask");
}

/* Publishes `bytes` at `output_path` through a temporary file. Two
   materials that convert to identical pixels publish one path, possibly from
   two workers at once. Encoding runs unlocked; publication is serialized, and
   the second publisher finds the file present. The lock covers one process;
   another cook of the same source writes its own temporary file. */
vkr_internal bool8_t vkr_mesh_loader_gltf_publish_bytes(
    const VkrMeshLoaderGltfParseInfo *info, String8 output_path,
    const uint8_t *bytes, uint64_t size, bool8_t *out_created) {
  vkr_local_persist VkrAtomicBool publish_lock = false_v;
  while (vkr_atomic_bool_exchange(&publish_lock, true_v,
                                  VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_platform_sleep(0u);
  }
  String8 temp_path = string8_create_formatted(
      info->scratch_allocator, "%.*s.tmp.%u", (int32_t)output_path.length,
      output_path.str, vkr_platform_get_process_id());
  FilePath temp =
      file_path_create((const char *)temp_path.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(temp_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  FilePath output =
      file_path_create((const char *)output_path.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(output_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  *out_created = false_v;
  if (file_exists(&output)) {
    vkr_atomic_bool_store(&publish_lock, false_v, VKR_MEMORY_ORDER_RELEASE);
    return true_v;
  }
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle file = {0};
  uint64_t written = 0;
  const FileError open_error = file_open(&temp, mode, &file);
  const bool8_t write_ok =
      open_error == FILE_ERROR_NONE &&
      file_write(&file, size, bytes, &written) == FILE_ERROR_NONE &&
      written == size && file_sync(&file) == FILE_ERROR_NONE;
  if (file.handle) {
    file_close(&file);
  }
  const bool8_t published =
      write_ok && file_rename(&temp, &output, true_v) == FILE_ERROR_NONE;
  *out_created = published;
  if (!published) {
    (void)file_remove(&temp);
  }
  vkr_atomic_bool_store(&publish_lock, false_v, VKR_MEMORY_ORDER_RELEASE);
  if (!published) {
    log_error("MeshLoader(glTF): failed to publish '%.*s'",
              (int32_t)output_path.length, output_path.str);
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_png_atomic(
    const VkrMeshLoaderGltfParseInfo *info, String8 output_path,
    const uint8_t *pixels, int32_t width, int32_t height,
    bool8_t *out_created) {
  if (!info || !info->scratch_allocator || !output_path.str || !pixels ||
      width <= 0 || height <= 0) {
    return false_v;
  }

  size_t png_size = 0u;
  void *png = vkr_vkt_encode_png_rgba8(pixels, width, height, &png_size);
  if (!png || png_size == 0u) {
    vkr_vkt_free_png(png);
    log_error("MeshLoader(glTF): failed to encode prepared texture '%.*s'",
              (int32_t)output_path.length, output_path.str);
    return false_v;
  }
  const bool8_t published = vkr_mesh_loader_gltf_publish_bytes(
      info, output_path, png, png_size, out_created);
  vkr_vkt_free_png(png);
  return published;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_pixels_are_uniform(
    const uint8_t *pixels, uint64_t pixel_count) {
  if (!pixels || pixel_count == 0) {
    return false_v;
  }
  for (uint64_t i = 1; i < pixel_count; ++i) {
    if (MemCompare(pixels, pixels + i * 4u, 4u) != 0) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal String8 vkr_mesh_loader_gltf_make_content_texture_path(
    VkrAllocator *allocator, String8 output_dir, const char *semantic,
    const uint8_t *pixels, int32_t width, int32_t height) {
  const uint64_t byte_count = (uint64_t)width * (uint64_t)height * 4u;
  const uint64_t content_hash = vkr_mesh_loader_gltf_hash_bytes(
      VKR_FNV1A64_OFFSET_BASIS, pixels, byte_count);
  return string8_create_formatted(allocator, "%.*s/%s_%dx%d_%016llx.png",
                                  (int32_t)output_dir.length, output_dir.str,
                                  semantic, width, height,
                                  (unsigned long long)content_hash);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_publish_content_texture(
    const VkrMeshLoaderGltfParseInfo *info, String8 path, const uint8_t *pixels,
    int32_t width, int32_t height, bool8_t *out_created) {
  *out_created = false_v;
  if (vkr_mesh_loader_gltf_path_exists(info->scratch_allocator, path)) {
    return true_v;
  }
  return vkr_mesh_loader_gltf_write_png_atomic(info, path, pixels, width,
                                               height, out_created);
}

vkr_internal void vkr_mesh_loader_gltf_remove_content_texture(
    const VkrMeshLoaderGltfParseInfo *info, String8 path) {
  if (!info || !path.str || path.length == 0u) {
    return;
  }
  String8 cstr = string8_create_formatted(info->scratch_allocator, "%.*s",
                                          (int32_t)path.length, path.str);
  const FilePathType type = vkr_mesh_loader_gltf_path_is_absolute(path)
                                ? FILE_PATH_TYPE_ABSOLUTE
                                : FILE_PATH_TYPE_RELATIVE;
  FilePath file =
      file_path_create((const char *)cstr.str, info->scratch_allocator, type);
  if (file_remove(&file) != FILE_ERROR_NONE) {
    log_warn("MeshLoader(glTF): failed to roll back prepared texture '%.*s'",
             (int32_t)path.length, path.str);
  }
}

/* Reads the encoded source image of a view into the scratch allocator.
   Leaves `out_encoded` empty for a view without a texture. */
vkr_internal bool8_t vkr_mesh_loader_gltf_read_texture_view(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_texture_view *view,
    VkrMeshLoaderGltfEncodedImage *out_encoded) {
  if (!info || !out_encoded) {
    return false_v;
  }
  *out_encoded = (VkrMeshLoaderGltfEncodedImage){0};
  if (!view || !view->texture || !view->texture->image) {
    return true_v;
  }
  if (!view->texture->image->uri || view->texture->image->buffer_view) {
    log_error("MeshLoader(glTF): prepared spec-gloss conversion requires an "
              "external source image");
    return false_v;
  }

  String8 uri =
      string8_create_from_cstr((const uint8_t *)view->texture->image->uri,
                               string_length(view->texture->image->uri));
  bool8_t found = false_v;
  String8 existing_path = {0};
  (void)vkr_mesh_loader_gltf_resolve_relative_texture_uri(
      info, uri, &found, &existing_path, true_v);
  if (!found || !existing_path.str || existing_path.length == 0) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  const String8 vkt_extension = string8_lit(".vkt");
  if (existing_path.length >= vkt_extension.length) {
    String8 suffix = string8_substring(
        &existing_path, existing_path.length - vkt_extension.length,
        existing_path.length);
    if (string8_equalsi(&suffix, &vkt_extension)) {
      log_error("MeshLoader(glTF): prepared spec-gloss conversion needs the "
                "source image, not only '%.*s'",
                (int32_t)existing_path.length, existing_path.str);
      vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
      return false_v;
    }
  }

  FilePathType type = vkr_mesh_loader_gltf_path_is_absolute(existing_path)
                          ? FILE_PATH_TYPE_ABSOLUTE
                          : FILE_PATH_TYPE_RELATIVE;
  FilePath path = file_path_create((const char *)existing_path.str,
                                   info->scratch_allocator, type);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&path, mode, &file) != FILE_ERROR_NONE) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint8_t *encoded = NULL;
  uint64_t encoded_size = 0;
  FileError read_error =
      file_read_all(&file, info->scratch_allocator, &encoded, &encoded_size);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !encoded || encoded_size == 0 ||
      encoded_size > INT32_MAX) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }
  *out_encoded = (VkrMeshLoaderGltfEncodedImage){
      .bytes = encoded,
      .size = encoded_size,
      .path = existing_path,
  };
  return true_v;
}

/* Releases a decoded image's pixels to the packer's reused blocks. */
vkr_internal void
vkr_mesh_loader_gltf_free_image(VkrMeshLoaderGltfDecodedImage *image) {
  vkr_vkt_reused_release(image->pixels,
                         (size_t)image->width * (size_t)image->height * 4u);
  *image = (VkrMeshLoaderGltfDecodedImage){0};
}

vkr_internal uint8_t *vkr_mesh_loader_gltf_decoded_pixels(void *context,
                                                          uint32_t width,
                                                          uint32_t height) {
  VkrMeshLoaderGltfDecodedImage *image = context;
  vkr_mesh_loader_gltf_free_image(image);
  image->pixels = vkr_vkt_reused_allocate((size_t)width * height * 4u);
  image->width = image->pixels ? (int32_t)width : 0;
  image->height = image->pixels ? (int32_t)height : 0;
  return image->pixels;
}

vkr_internal void *vkr_mesh_loader_gltf_decode_scratch(void *context,
                                                       size_t size) {
  (void)context;
  return vkr_vkt_reused_allocate(size);
}

vkr_internal void
vkr_mesh_loader_gltf_release_scratch(void *context, void *memory, size_t size) {
  (void)context;
  vkr_vkt_reused_release(memory, size);
}

/* Decodes a source image read by vkr_mesh_loader_gltf_read_texture_view to
   RGBA8 in the packer's reused blocks, which a cook's many large sources
   would otherwise take fresh from the system each time; release it with
   vkr_mesh_loader_gltf_free_image. An empty source leaves `out_image`
   empty. */
vkr_internal bool8_t
vkr_mesh_loader_gltf_decode_image(const VkrMeshLoaderGltfParseInfo *info,
                                  const VkrMeshLoaderGltfEncodedImage *encoded,
                                  VkrMeshLoaderGltfDecodedImage *out_image) {
  *out_image = (VkrMeshLoaderGltfDecodedImage){0};
  if (!encoded->bytes) {
    return true_v;
  }

  const VkrImageDecodeTarget target = {
      .pixels = vkr_mesh_loader_gltf_decoded_pixels,
      .allocate = vkr_mesh_loader_gltf_decode_scratch,
      .release = vkr_mesh_loader_gltf_release_scratch,
      .context = out_image,
  };
  if (!vkr_image_decode_rgba8_into(encoded->bytes, encoded->size, 0, &target)) {
    vkr_mesh_loader_gltf_free_image(out_image);
  }
  if (!out_image->pixels || out_image->width <= 0 || out_image->height <= 0) {
    log_error("MeshLoader(glTF): failed to decode spec-gloss source '%.*s'",
              (int32_t)encoded->path.length, encoded->path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  return true_v;
}

/* One memo slot of the specular-glossiness conversion: the diffuse and
   specular-glossiness texels, and the base-color and metallic-roughness
   texels they convert to. */
#define VKR_GLTF_CONVERSION_MEMO_BITS 12u
#define VKR_GLTF_CONVERSION_MEMO_SLOTS (1u << VKR_GLTF_CONVERSION_MEMO_BITS)
typedef struct VkrMeshLoaderGltfConversionMemo {
  uint64_t key;
  uint8_t texel[8];
  bool8_t filled;
} VkrMeshLoaderGltfConversionMemo;

vkr_internal uint8_t vkr_mesh_loader_gltf_unorm8(float32_t value) {
  return (uint8_t)(Clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

vkr_internal int32_t vkr_mesh_loader_gltf_wrap_texel(int32_t value,
                                                     int32_t extent,
                                                     cgltf_wrap_mode mode) {
  if (extent <= 1) {
    return 0;
  }
  if (mode == cgltf_wrap_mode_clamp_to_edge) {
    return Clamp(value, 0, extent - 1);
  }
  if (mode == cgltf_wrap_mode_mirrored_repeat) {
    const int32_t period = extent * 2;
    int32_t wrapped = value % period;
    if (wrapped < 0) {
      wrapped += period;
    }
    return wrapped < extent ? wrapped : period - wrapped - 1;
  }
  int32_t wrapped = value % extent;
  return wrapped < 0 ? wrapped + extent : wrapped;
}

vkr_internal Vec4 vkr_mesh_loader_gltf_decode_texel(const uint8_t *pixels,
                                                    int32_t width, int32_t x,
                                                    int32_t y, bool8_t rgb_srgb,
                                                    const float32_t *srgb_lut) {
  const uint8_t *texel =
      pixels + ((uint64_t)y * (uint64_t)width + (uint64_t)x) * 4u;
  Vec4 value =
      vec4_new((float32_t)texel[0] / 255.0f, (float32_t)texel[1] / 255.0f,
               (float32_t)texel[2] / 255.0f, (float32_t)texel[3] / 255.0f);
  if (rgb_srgb) {
    value.x = srgb_lut[texel[0]];
    value.y = srgb_lut[texel[1]];
    value.z = srgb_lut[texel[2]];
  }
  return value;
}

vkr_internal Vec4 vkr_mesh_loader_gltf_sample_image(
    const VkrMeshLoaderGltfDecodedImage *image, const cgltf_texture_view *view,
    float32_t u, float32_t v, bool8_t rgb_srgb, const float32_t *srgb_lut) {
  if (!image || !image->pixels || image->width <= 0 || image->height <= 0) {
    return vec4_one();
  }

  const cgltf_sampler *sampler =
      view && view->texture ? view->texture->sampler : NULL;
  const cgltf_wrap_mode wrap_s = sampler && sampler->wrap_s != 0
                                     ? sampler->wrap_s
                                     : cgltf_wrap_mode_repeat;
  const cgltf_wrap_mode wrap_t = sampler && sampler->wrap_t != 0
                                     ? sampler->wrap_t
                                     : cgltf_wrap_mode_repeat;
  const bool8_t nearest =
      sampler && sampler->mag_filter == cgltf_filter_type_nearest &&
      (sampler->min_filter == cgltf_filter_type_nearest ||
       sampler->min_filter == cgltf_filter_type_nearest_mipmap_nearest ||
       sampler->min_filter == cgltf_filter_type_nearest_mipmap_linear);
  const float32_t source_x = u * (float32_t)image->width - 0.5f;
  const float32_t source_y = v * (float32_t)image->height - 0.5f;
  if (nearest) {
    const int32_t x = vkr_mesh_loader_gltf_wrap_texel(
        (int32_t)floorf(source_x + 0.5f), image->width, wrap_s);
    const int32_t y = vkr_mesh_loader_gltf_wrap_texel(
        (int32_t)floorf(source_y + 0.5f), image->height, wrap_t);
    return vkr_mesh_loader_gltf_decode_texel(image->pixels, image->width, x, y,
                                             rgb_srgb, srgb_lut);
  }

  const float32_t source_x_floor = floorf(source_x);
  const float32_t source_y_floor = floorf(source_y);
  const int32_t x0_unwrapped = (int32_t)source_x_floor;
  const int32_t y0_unwrapped = (int32_t)source_y_floor;
  const int32_t x0 =
      vkr_mesh_loader_gltf_wrap_texel(x0_unwrapped, image->width, wrap_s);
  const int32_t x1 =
      vkr_mesh_loader_gltf_wrap_texel(x0_unwrapped + 1, image->width, wrap_s);
  const int32_t y0 =
      vkr_mesh_loader_gltf_wrap_texel(y0_unwrapped, image->height, wrap_t);
  const int32_t y1 =
      vkr_mesh_loader_gltf_wrap_texel(y0_unwrapped + 1, image->height, wrap_t);
  const float32_t tx = source_x - source_x_floor;
  const float32_t ty = source_y - source_y_floor;
  const Vec4 a = vkr_mesh_loader_gltf_decode_texel(image->pixels, image->width,
                                                   x0, y0, rgb_srgb, srgb_lut);
  const Vec4 b = vkr_mesh_loader_gltf_decode_texel(image->pixels, image->width,
                                                   x1, y0, rgb_srgb, srgb_lut);
  const Vec4 c = vkr_mesh_loader_gltf_decode_texel(image->pixels, image->width,
                                                   x0, y1, rgb_srgb, srgb_lut);
  const Vec4 d = vkr_mesh_loader_gltf_decode_texel(image->pixels, image->width,
                                                   x1, y1, rgb_srgb, srgb_lut);
  const Vec4 top = vec4_new(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * tx,
                            a.z + (b.z - a.z) * tx, a.w + (b.w - a.w) * tx);
  const Vec4 bottom = vec4_new(c.x + (d.x - c.x) * tx, c.y + (d.y - c.y) * tx,
                               c.z + (d.z - c.z) * tx, c.w + (d.w - c.w) * tx);
  return vec4_new(
      top.x + (bottom.x - top.x) * ty, top.y + (bottom.y - top.y) * ty,
      top.z + (bottom.z - top.z) * ty, top.w + (bottom.w - top.w) * ty);
}

/* Converted images are named by their pixels. A shared generated root keeps
   them in one directory, so importing a model again finds them instead of
   encoding them again; repository cooks keep one directory per source. */
vkr_internal String8 vkr_mesh_loader_gltf_spec_gloss_output_dir(
    const VkrMeshLoaderGltfParseInfo *info, uint64_t source_hash) {
  String8 output_dir =
      info->generated_root.length
          ? string8_create_formatted(info->load_allocator,
                                     "assets/textures/generated/gltf_sg%u",
                                     VKR_GLTF_SPEC_GLOSS_CACHE_VERSION)
          : string8_create_formatted(
                info->load_allocator,
                "assets/textures/generated/gltf_sg%u_%016llx",
                VKR_GLTF_SPEC_GLOSS_CACHE_VERSION,
                (unsigned long long)source_hash);
  return vkr_mesh_loader_gltf_output_path(info, output_dir);
}

/* The memo maps a managed spec-gloss material's conversion inputs to its
   converted images' extent and content hashes and its factors, so importing
   a model again names its packed textures without decoding and converting
   the sources; only a pack whose output is missing or stale converts again.
   The key covers everything the conversion reads: both source files' bytes,
   their samplers and the material factors. VKR_GLTF_SPEC_GLOSS_CACHE_VERSION
   names the conversion itself; bump VKR_GLTF_SPEC_GLOSS_MEMO_VERSION when
   the memo format changes. */
#define VKR_GLTF_SPEC_GLOSS_MEMO_VERSION 2u

vkr_internal void vkr_mesh_loader_gltf_spec_gloss_memo_view(
    VkrSha256 *hash, const cgltf_texture_view *view,
    const VkrMeshLoaderGltfEncodedImage *encoded) {
  const cgltf_sampler *sampler =
      view && view->texture ? view->texture->sampler : NULL;
  const int32_t sampler_state[4] = {
      sampler ? (int32_t)sampler->wrap_s : -1,
      sampler ? (int32_t)sampler->wrap_t : -1,
      sampler ? (int32_t)sampler->mag_filter : -1,
      sampler ? (int32_t)sampler->min_filter : -1,
  };
  vkr_sha256_update(hash, sampler_state, sizeof(sampler_state));
  vkr_sha256_update(hash, &encoded->size, sizeof(encoded->size));
  if (encoded->bytes) {
    vkr_sha256_update(hash, encoded->bytes, encoded->size);
  }
}

vkr_internal String8 vkr_mesh_loader_gltf_spec_gloss_memo_path(
    const VkrMeshLoaderGltfParseInfo *info, String8 output_dir,
    const cgltf_pbr_specular_glossiness *source,
    const VkrMeshLoaderGltfEncodedImage *diffuse,
    const VkrMeshLoaderGltfEncodedImage *spec_gloss) {
  const uint32_t versions[2] = {VKR_GLTF_SPEC_GLOSS_MEMO_VERSION,
                                VKR_GLTF_SPEC_GLOSS_CACHE_VERSION};
  const float32_t factors[8] = {
      source->diffuse_factor[0],  source->diffuse_factor[1],
      source->diffuse_factor[2],  source->diffuse_factor[3],
      source->specular_factor[0], source->specular_factor[1],
      source->specular_factor[2], source->glossiness_factor,
  };
  VkrSha256 hash;
  vkr_sha256_init(&hash);
  vkr_sha256_update(&hash, versions, sizeof(versions));
  vkr_sha256_update(&hash, factors, sizeof(factors));
  vkr_mesh_loader_gltf_spec_gloss_memo_view(&hash, &source->diffuse_texture,
                                            diffuse);
  vkr_mesh_loader_gltf_spec_gloss_memo_view(
      &hash, &source->specular_glossiness_texture, spec_gloss);
  uint8_t digest[VKR_SHA256_DIGEST_SIZE];
  vkr_sha256_final(&hash, digest);
  char hex[VKR_SHA256_HEX_SIZE];
  vkr_sha256_hex(digest, hex);
  return string8_create_formatted(info->load_allocator, "%.*s/memo/%s.sgm",
                                  (int32_t)output_dir.length, output_dir.str,
                                  hex);
}

/* Fills `out_converted`, without pixels, from a memo. Returns false on any
   miss, and the caller converts. */
vkr_internal bool8_t vkr_mesh_loader_gltf_spec_gloss_memo_read(
    const VkrMeshLoaderGltfParseInfo *info, String8 memo_path,
    VkrGltfMetalRoughSample factor_converted,
    VkrMeshLoaderGltfConvertedSpecGloss *out_converted) {
  FilePath path =
      file_path_create((const char *)memo_path.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(memo_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&path, mode, &file) != FILE_ERROR_NONE) {
    return false_v;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  const FileError read_error =
      file_read_all(&file, info->scratch_allocator, &bytes, &size);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !bytes || size == 0 || size > KB(1)) {
    return false_v;
  }

  char text[KB(1) + 1];
  MemCopy(text, bytes, size);
  text[size] = '\0';
  uint32_t version = 0;
  int32_t width = 0;
  int32_t height = 0;
  char hashes[2][17];
  uint32_t bits[6] = {0};
  if (sscanf(text, "vkr-sg-memo %u %d %d %16s %16s %x %x %x %x %x %x", &version,
             &width, &height, hashes[0], hashes[1], &bits[0], &bits[1],
             &bits[2], &bits[3], &bits[4], &bits[5]) != 11 ||
      version != VKR_GLTF_SPEC_GLOSS_MEMO_VERSION || width <= 0 ||
      height <= 0) {
    return false_v;
  }
  bool8_t uniform[2] = {false_v, false_v};
  uint64_t values[2] = {0u, 0u};
  for (uint32_t i = 0; i < 2u; ++i) {
    uniform[i] = hashes[i][0] == '-' && hashes[i][1] == '\0';
    unsigned long long value = 0u;
    if (!uniform[i] && sscanf(hashes[i], "%16llx", &value) != 1) {
      return false_v;
    }
    values[i] = value;
  }

  float32_t factors[6];
  MemCopy(factors, bits, sizeof(factors));
  *out_converted = (VkrMeshLoaderGltfConvertedSpecGloss){
      .textured = true_v,
      .base_uniform = uniform[0],
      .metal_rough_uniform = uniform[1],
      .width = width,
      .height = height,
      .base_hash = values[0],
      .metal_rough_hash = values[1],
      .base_color_factor =
          vec4_new(factors[0], factors[1], factors[2], factors[3]),
      .metallic_factor = factors[4],
      .roughness_factor = factors[5],
      .dielectric_specular_factor = factor_converted.dielectric_specular,
  };
  return true_v;
}

/* Records a conversion under its memo key. A failed write only costs the
   next import a conversion. */
vkr_internal void vkr_mesh_loader_gltf_spec_gloss_memo_write(
    const VkrMeshLoaderGltfParseInfo *info, String8 memo_path,
    const VkrMeshLoaderGltfConvertedSpecGloss *converted) {
  char hashes[2][17] = {"-", "-"};
  if (!converted->base_uniform) {
    (void)snprintf(hashes[0], sizeof(hashes[0]), "%016llx",
                   (unsigned long long)converted->base_hash);
  }
  if (!converted->metal_rough_uniform) {
    (void)snprintf(hashes[1], sizeof(hashes[1]), "%016llx",
                   (unsigned long long)converted->metal_rough_hash);
  }
  const float32_t factors[6] = {
      converted->base_color_factor.x, converted->base_color_factor.y,
      converted->base_color_factor.z, converted->base_color_factor.w,
      converted->metallic_factor,     converted->roughness_factor,
  };
  uint32_t bits[6];
  MemCopy(bits, factors, sizeof(bits));
  String8 text = string8_create_formatted(
      info->scratch_allocator,
      "vkr-sg-memo %u\n%d %d\n%s\n%s\n%08x %08x %08x %08x %08x %08x\n",
      VKR_GLTF_SPEC_GLOSS_MEMO_VERSION, converted->width, converted->height,
      hashes[0], hashes[1], bits[0], bits[1], bits[2], bits[3], bits[4],
      bits[5]);

  String8 memo_dir = memo_path;
  for (uint64_t c = memo_path.length; c > 0; --c) {
    if (memo_path.str[c - 1u] == '/') {
      memo_dir = string8_create_formatted(info->scratch_allocator, "%.*s",
                                          (int32_t)(c - 1u), memo_path.str);
      break;
    }
  }
  FilePath memo_dir_path =
      file_path_create((const char *)memo_dir.str, info->scratch_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(memo_dir)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  bool8_t created = false_v;
  if (!file_ensure_directory(info->scratch_allocator, &memo_dir_path.path) ||
      !vkr_mesh_loader_gltf_publish_bytes(info, memo_path, text.str,
                                          text.length, &created)) {
    log_warn("MeshLoader(glTF): spec-gloss memo '%.*s' was not recorded",
             (int32_t)memo_path.length, memo_path.str);
  }
}

/* Converts decoded spec-gloss images into metallic-roughness texels at the
   combined extent. An image at that extent is read texel by texel; a smaller
   one is sampled with its view. */
vkr_internal void vkr_mesh_loader_gltf_convert_spec_gloss_pixels(
    const VkrMeshLoaderGltfParseInfo *info,
    VkrGltfSpecGlossSample factor_sample,
    const VkrMeshLoaderGltfDecodedImage *diffuse,
    const cgltf_texture_view *diffuse_view,
    const VkrMeshLoaderGltfDecodedImage *spec_gloss,
    const cgltf_texture_view *spec_gloss_view, int32_t width, int32_t height,
    uint8_t *base_pixels, uint8_t *metal_rough_pixels) {
  float32_t srgb_to_linear_lut[256];
  uint8_t linear_to_srgb_lut[VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX + 1u];
  for (uint32_t i = 0; i < 256u; ++i) {
    srgb_to_linear_lut[i] = vkr_srgb_to_linear((float32_t)i / 255.0f);
  }
  for (uint32_t i = 0; i <= VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX; ++i) {
    linear_to_srgb_lut[i] = vkr_mesh_loader_gltf_unorm8(vkr_linear_to_srgb(
        (float32_t)i / (float32_t)VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX));
  }

  /* When both images are read at their own size, a texel's output depends
     only on its eight source bytes, and neighbouring texels often repeat
     them: a direct-mapped memo skips the conversion's square roots and
     divides for repeats, with the same bytes as computing them. */
  const bool8_t memoized = diffuse->pixels && diffuse->width == width &&
                           diffuse->height == height && spec_gloss->pixels &&
                           spec_gloss->width == width &&
                           spec_gloss->height == height;
  VkrMeshLoaderGltfConversionMemo *memo =
      memoized ? (VkrMeshLoaderGltfConversionMemo *)vkr_allocator_alloc(
                     info->scratch_allocator,
                     sizeof(*memo) * VKR_GLTF_CONVERSION_MEMO_SLOTS,
                     VKR_ALLOCATOR_MEMORY_TAG_TEXTURE)
               : NULL;
  if (memo) {
    MemZero(memo, sizeof(*memo) * VKR_GLTF_CONVERSION_MEMO_SLOTS);
  }

  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const uint64_t i = (uint64_t)y * (uint64_t)width + (uint64_t)x;
      uint8_t *base = base_pixels + i * 4u;
      uint8_t *metal_rough = metal_rough_pixels + i * 4u;
      VkrMeshLoaderGltfConversionMemo *slot = NULL;
      uint64_t key = 0u;
      if (memo) {
        uint32_t diffuse_bytes = 0u;
        uint32_t spec_gloss_bytes = 0u;
        MemCopy(&diffuse_bytes, diffuse->pixels + i * 4u, 4u);
        MemCopy(&spec_gloss_bytes, spec_gloss->pixels + i * 4u, 4u);
        key = (uint64_t)diffuse_bytes | ((uint64_t)spec_gloss_bytes << 32u);
        slot = &memo[(key * 0x9e3779b97f4a7c15ull) >>
                     (64u - VKR_GLTF_CONVERSION_MEMO_BITS)];
        if (slot->filled && slot->key == key) {
          MemCopy(base, slot->texel, 4u);
          MemCopy(metal_rough, slot->texel + 4u, 4u);
          continue;
        }
      }
      const float32_t u = ((float32_t)x + 0.5f) / (float32_t)width;
      const float32_t v = ((float32_t)y + 0.5f) / (float32_t)height;
      const Vec4 diffuse_texel =
          diffuse->pixels && diffuse->width == width &&
                  diffuse->height == height
              ? vkr_mesh_loader_gltf_decode_texel(diffuse->pixels,
                                                  diffuse->width, x, y, true_v,
                                                  srgb_to_linear_lut)
              : vkr_mesh_loader_gltf_sample_image(diffuse, diffuse_view, u, v,
                                                  true_v, srgb_to_linear_lut);
      const Vec4 spec_gloss_texel =
          spec_gloss->pixels && spec_gloss->width == width &&
                  spec_gloss->height == height
              ? vkr_mesh_loader_gltf_decode_texel(spec_gloss->pixels,
                                                  spec_gloss->width, x, y,
                                                  true_v, srgb_to_linear_lut)
              : vkr_mesh_loader_gltf_sample_image(spec_gloss, spec_gloss_view,
                                                  u, v, true_v,
                                                  srgb_to_linear_lut);
      VkrGltfSpecGlossSample sample = factor_sample;
      sample.diffuse.x *= diffuse_texel.x;
      sample.diffuse.y *= diffuse_texel.y;
      sample.diffuse.z *= diffuse_texel.z;
      sample.diffuse.w *= diffuse_texel.w;
      sample.specular.x *= spec_gloss_texel.x;
      sample.specular.y *= spec_gloss_texel.y;
      sample.specular.z *= spec_gloss_texel.z;
      sample.glossiness *= spec_gloss_texel.w;

      const VkrGltfMetalRoughSample converted =
          vkr_gltf_convert_spec_gloss_sample(sample);
      const uint32_t base_r =
          (uint32_t)(Clamp(converted.base_color.x, 0.0f, 1.0f) *
                         (float32_t)VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX +
                     0.5f);
      const uint32_t base_g =
          (uint32_t)(Clamp(converted.base_color.y, 0.0f, 1.0f) *
                         (float32_t)VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX +
                     0.5f);
      const uint32_t base_b =
          (uint32_t)(Clamp(converted.base_color.z, 0.0f, 1.0f) *
                         (float32_t)VKR_GLTF_LINEAR_TO_SRGB_LUT_MAX +
                     0.5f);
      base[0] = linear_to_srgb_lut[base_r];
      base[1] = linear_to_srgb_lut[base_g];
      base[2] = linear_to_srgb_lut[base_b];
      base[3] = vkr_mesh_loader_gltf_unorm8(converted.base_color.w);

      metal_rough[0] = 255u;
      metal_rough[1] = vkr_mesh_loader_gltf_unorm8(converted.roughness);
      metal_rough[2] = vkr_mesh_loader_gltf_unorm8(converted.metallic);
      metal_rough[3] = 255u;
      if (slot) {
        slot->key = key;
        slot->filled = true_v;
        MemCopy(slot->texel, base, 4u);
        MemCopy(slot->texel + 4u, metal_rough, 4u);
      }
    }
  }
}

/* Decodes and converts one material's images; a repository cook publishes
   them as PNGs and a managed cook keeps the pixels. A managed cook first
   consults the memo when `use_memo` is set. Reads only the parse info's paths
   and writes through its allocators and error, so workers run it on private
   copies of the info. */
vkr_internal bool8_t vkr_mesh_loader_gltf_convert_spec_gloss_inner(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    uint64_t source_hash, bool8_t use_memo,
    VkrMeshLoaderGltfConvertedSpecGloss *out_converted) {
  if (!info || !material || !out_converted ||
      !material->has_pbr_specular_glossiness) {
    return false_v;
  }

  const cgltf_pbr_specular_glossiness *source =
      &material->pbr_specular_glossiness;
  const cgltf_texture_view *diffuse_view = &source->diffuse_texture;
  const cgltf_texture_view *spec_gloss_view =
      &source->specular_glossiness_texture;
  const bool8_t has_diffuse_texture =
      diffuse_view->texture && diffuse_view->texture->image;
  const bool8_t has_spec_gloss_texture =
      spec_gloss_view->texture && spec_gloss_view->texture->image;

  VkrGltfSpecGlossSample factor_sample = {
      .diffuse = vec4_new(source->diffuse_factor[0], source->diffuse_factor[1],
                          source->diffuse_factor[2], source->diffuse_factor[3]),
      .specular =
          vec3_new(source->specular_factor[0], source->specular_factor[1],
                   source->specular_factor[2]),
      .glossiness = (float32_t)source->glossiness_factor,
  };
  const VkrGltfMetalRoughSample factor_converted =
      vkr_gltf_convert_spec_gloss_sample(factor_sample);
  if ((!has_diffuse_texture && !has_spec_gloss_texture) ||
      info->defer_textures) {
    *out_converted = (VkrMeshLoaderGltfConvertedSpecGloss){
        .base_color_factor = factor_converted.base_color,
        .metallic_factor = factor_converted.metallic,
        .roughness_factor = factor_converted.roughness,
        .dielectric_specular_factor = factor_converted.dielectric_specular,
    };
    return true_v;
  }

  if ((diffuse_view->has_transform || spec_gloss_view->has_transform) ||
      (has_diffuse_texture && has_spec_gloss_texture &&
       diffuse_view->texcoord != spec_gloss_view->texcoord)) {
    log_error("MeshLoader(glTF): spec-gloss texture lowering requires matching "
              "UV sets and no KHR_texture_transform");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  const String8 output_dir =
      vkr_mesh_loader_gltf_spec_gloss_output_dir(info, source_hash);

  VkrMeshLoaderGltfEncodedImage diffuse_source = {0};
  VkrMeshLoaderGltfEncodedImage spec_gloss_source = {0};
  if (!vkr_mesh_loader_gltf_read_texture_view(info, diffuse_view,
                                              &diffuse_source) ||
      !vkr_mesh_loader_gltf_read_texture_view(info, spec_gloss_view,
                                              &spec_gloss_source)) {
    return false_v;
  }

  /* Only the shared directory outlives one cook, so only it keeps a memo. */
  const bool8_t managed = info->generated_root.length > 0u;
  String8 memo_path = {0};
  if (managed) {
    memo_path = vkr_mesh_loader_gltf_spec_gloss_memo_path(
        info, output_dir, source, &diffuse_source, &spec_gloss_source);
    if (use_memo && vkr_mesh_loader_gltf_spec_gloss_memo_read(
                        info, memo_path, factor_converted, out_converted)) {
      return true_v;
    }
  }

  VkrMeshLoaderGltfDecodedImage diffuse = {0};
  VkrMeshLoaderGltfDecodedImage spec_gloss = {0};
  if (!vkr_mesh_loader_gltf_decode_image(info, &diffuse_source, &diffuse) ||
      !vkr_mesh_loader_gltf_decode_image(info, &spec_gloss_source,
                                         &spec_gloss)) {
    vkr_mesh_loader_gltf_free_image(&diffuse);
    vkr_mesh_loader_gltf_free_image(&spec_gloss);
    return false_v;
  }

  const int32_t width = Max(diffuse.width, spec_gloss.width);
  const int32_t height = Max(diffuse.height, spec_gloss.height);

  const uint64_t pixel_count = (uint64_t)width * (uint64_t)height;
  if (pixel_count == 0 || pixel_count > UINT64_MAX / 4u) {
    vkr_mesh_loader_gltf_free_image(&diffuse);
    vkr_mesh_loader_gltf_free_image(&spec_gloss);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  /* A managed cook packs the pixels after this returns. */
  VkrAllocator *pixel_allocator =
      managed ? info->load_allocator : info->scratch_allocator;
  uint8_t *base_pixels = (uint8_t *)vkr_allocator_alloc(
      pixel_allocator, pixel_count * 4u, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  uint8_t *metal_rough_pixels = (uint8_t *)vkr_allocator_alloc(
      pixel_allocator, pixel_count * 4u, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  if (!base_pixels || !metal_rough_pixels) {
    vkr_mesh_loader_gltf_free_image(&diffuse);
    vkr_mesh_loader_gltf_free_image(&spec_gloss);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }

  vkr_mesh_loader_gltf_convert_spec_gloss_pixels(
      info, factor_sample, &diffuse, diffuse_view, &spec_gloss, spec_gloss_view,
      width, height, base_pixels, metal_rough_pixels);

  const bool8_t base_uniform =
      vkr_mesh_loader_gltf_pixels_are_uniform(base_pixels, pixel_count);
  const bool8_t metal_rough_uniform =
      vkr_mesh_loader_gltf_pixels_are_uniform(metal_rough_pixels, pixel_count);
  const Vec4 base_factor =
      base_uniform
          ? vec4_new(vkr_srgb_to_linear((float32_t)base_pixels[0] / 255.0f),
                     vkr_srgb_to_linear((float32_t)base_pixels[1] / 255.0f),
                     vkr_srgb_to_linear((float32_t)base_pixels[2] / 255.0f),
                     (float32_t)base_pixels[3] / 255.0f)
          : vec4_one();
  const float32_t roughness_factor =
      metal_rough_uniform ? (float32_t)metal_rough_pixels[1] / 255.0f : 1.0f;
  const float32_t metallic_factor =
      metal_rough_uniform ? (float32_t)metal_rough_pixels[2] / 255.0f : 1.0f;
  if (managed) {
    vkr_mesh_loader_gltf_free_image(&diffuse);
    vkr_mesh_loader_gltf_free_image(&spec_gloss);
    const uint64_t byte_count = pixel_count * 4u;
    *out_converted = (VkrMeshLoaderGltfConvertedSpecGloss){
        .textured = true_v,
        .base_uniform = base_uniform,
        .metal_rough_uniform = metal_rough_uniform,
        .width = width,
        .height = height,
        .base_hash = base_uniform ? 0u
                                  : vkr_mesh_loader_gltf_hash_bytes(
                                        VKR_FNV1A64_OFFSET_BASIS, base_pixels,
                                        byte_count),
        .metal_rough_hash =
            metal_rough_uniform
                ? 0u
                : vkr_mesh_loader_gltf_hash_bytes(
                      VKR_FNV1A64_OFFSET_BASIS, metal_rough_pixels, byte_count),
        .base_pixels = base_uniform ? NULL : base_pixels,
        .metal_rough_pixels = metal_rough_uniform ? NULL : metal_rough_pixels,
        .base_color_factor = base_factor,
        .metallic_factor = metallic_factor,
        .roughness_factor = roughness_factor,
        .dielectric_specular_factor = factor_converted.dielectric_specular,
    };
    vkr_mesh_loader_gltf_spec_gloss_memo_write(info, memo_path, out_converted);
    return true_v;
  }

  const String8 base_path =
      base_uniform ? (String8){0}
                   : vkr_mesh_loader_gltf_make_content_texture_path(
                         info->load_allocator, output_dir, "basecolor",
                         base_pixels, width, height);
  const String8 metal_rough_path =
      metal_rough_uniform ? (String8){0}
                          : vkr_mesh_loader_gltf_make_content_texture_path(
                                info->load_allocator, output_dir, "metalrough",
                                metal_rough_pixels, width, height);

  bool8_t ok = true_v;
  bool8_t base_created = false_v;
  bool8_t metal_rough_created = false_v;
  if (!base_uniform || !metal_rough_uniform) {
    FilePath output_dir_path =
        file_path_create((const char *)output_dir.str, info->load_allocator,
                         vkr_mesh_loader_gltf_path_is_absolute(output_dir)
                             ? FILE_PATH_TYPE_ABSOLUTE
                             : FILE_PATH_TYPE_RELATIVE);
    ok = file_ensure_directory(info->load_allocator, &output_dir_path.path);
  }
  if (ok && !base_uniform) {
    ok = vkr_mesh_loader_gltf_publish_content_texture(
        info, base_path, base_pixels, width, height, &base_created);
  }
  if (ok && !metal_rough_uniform) {
    ok = vkr_mesh_loader_gltf_publish_content_texture(
        info, metal_rough_path, metal_rough_pixels, width, height,
        &metal_rough_created);
  }

  vkr_mesh_loader_gltf_free_image(&diffuse);
  vkr_mesh_loader_gltf_free_image(&spec_gloss);
  if (!ok) {
    if (metal_rough_created) {
      vkr_mesh_loader_gltf_remove_content_texture(info, metal_rough_path);
    }
    if (base_created) {
      vkr_mesh_loader_gltf_remove_content_texture(info, base_path);
    }
    vkr_mesh_loader_gltf_set_error(info,
                                   VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    return false_v;
  }

  *out_converted = (VkrMeshLoaderGltfConvertedSpecGloss){
      .textured = true_v,
      .base_uniform = base_uniform,
      .metal_rough_uniform = metal_rough_uniform,
      .base_path = base_path,
      .metal_rough_path = metal_rough_path,
      .width = width,
      .height = height,
      .base_color_factor = base_factor,
      .metallic_factor = metallic_factor,
      .roughness_factor = roughness_factor,
      .dielectric_specular_factor = factor_converted.dielectric_specular,
  };
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_convert_spec_gloss(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    uint64_t source_hash, bool8_t use_memo,
    VkrMeshLoaderGltfConvertedSpecGloss *out_converted) {
  if (!info || !info->scratch_allocator) {
    return false_v;
  }
  VkrAllocatorScope scope = vkr_allocator_begin_scope(info->scratch_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  const bool8_t result = vkr_mesh_loader_gltf_convert_spec_gloss_inner(
      info, material, source_hash, use_memo, out_converted);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_TEXTURE);
  return result;
}

/* Records the converted images as generated assets and forms the material's
   texture lines. Main thread. */
vkr_internal void vkr_mesh_loader_gltf_prepare_spec_gloss(
    const VkrMeshLoaderGltfParseInfo *info,
    const VkrMeshLoaderGltfConvertedSpecGloss *converted,
    VkrMeshLoaderGltfPreparedSpecGloss *out_prepared) {
  if (!converted->textured) {
    *out_prepared = (VkrMeshLoaderGltfPreparedSpecGloss){
        .base_color_factor = converted->base_color_factor,
        .metallic_factor = converted->metallic_factor,
        .roughness_factor = converted->roughness_factor,
        .dielectric_specular_factor = converted->dielectric_specular_factor,
    };
    return;
  }
  vkr_mesh_loader_gltf_publish_prepared_spec_gloss(
      info, converted->base_path, converted->metal_rough_path,
      converted->base_color_factor, converted->metallic_factor,
      converted->roughness_factor, converted->dielectric_specular_factor,
      out_prepared);
}

// =============================================================================
// Converted spec-gloss packs
// =============================================================================

/* The pack source of one converted image of a managed cook. */
vkr_internal VkrVktSource vkr_mesh_loader_gltf_converted_source(
    const VkrMeshLoaderGltfConvertedSpecGloss *converted, bool8_t base) {
  return (VkrVktSource){
      .pixels = base ? converted->base_pixels : converted->metal_rough_pixels,
      .width = (uint32_t)converted->width,
      .height = (uint32_t)converted->height,
      .hash = base ? converted->base_hash : converted->metal_rough_hash,
  };
}

/* Converts a memo hit's images again once a pack finds its output missing or
   stale. The conversion is deterministic, so it must reproduce the memo;
   a difference means the conversion changed without a new
   VKR_GLTF_SPEC_GLOSS_CACHE_VERSION. */
vkr_internal bool8_t vkr_mesh_loader_gltf_convert_pixels(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    uint64_t source_hash, VkrMeshLoaderGltfConvertedSpecGloss *converted) {
  VkrMeshLoaderGltfConvertedSpecGloss fresh = {0};
  if (!vkr_mesh_loader_gltf_convert_spec_gloss(info, material, source_hash,
                                               false_v, &fresh)) {
    return false_v;
  }
  if (fresh.base_uniform != converted->base_uniform ||
      fresh.metal_rough_uniform != converted->metal_rough_uniform ||
      fresh.width != converted->width || fresh.height != converted->height ||
      fresh.base_hash != converted->base_hash ||
      fresh.metal_rough_hash != converted->metal_rough_hash) {
    log_error("MeshLoader(glTF): spec-gloss conversion of '%s' differs from "
              "its memo; the conversion changed without a new cache version",
              material->name ? material->name : "<unnamed>");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  *converted = fresh;
  return true_v;
}

/* Packs one converted image of a managed cook as a plain texture named by
   its content hash and the process settings. The base colour of an opaque
   material packs without alpha, under its own name. */
vkr_internal VkrVktPackResult vkr_mesh_loader_gltf_pack_converted(
    const VkrMeshLoaderGltfParseInfo *info, uint64_t source_hash,
    const VkrMeshLoaderGltfConvertedSpecGloss *converted, bool8_t base,
    bool8_t opaque, String8 *out_texture) {
  const String8 output_dir =
      vkr_mesh_loader_gltf_spec_gloss_output_dir(info, source_hash);
  const String8 output = string8_create_formatted(
      info->load_allocator, "%.*s/%s_%dx%d_%016llx%s.vkt",
      (int32_t)output_dir.length, output_dir.str,
      base ? (opaque ? "basecolor_opaque" : "basecolor") : "metalrough",
      converted->width, converted->height,
      (unsigned long long)(base ? converted->base_hash
                                : converted->metal_rough_hash),
      vkr_vkt_variant_suffix());
  if (!output.str) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return VKR_VKT_PACK_FAILED;
  }
  const VkrVktSource source =
      vkr_mesh_loader_gltf_converted_source(converted, base);
  const VkrVktPackResult result =
      base && opaque
          ? vkr_vkt_pack_opaque(&source, (const char *)output.str)
          : vkr_vkt_pack_image(&source, base ? "color-srgb" : "data-mask",
                               (const char *)output.str);
  if (result == VKR_VKT_PACK_STALE) {
    return result;
  }
  if (result != VKR_VKT_PACK_SUCCESS) {
    log_error("MeshLoader(glTF): failed to pack converted texture '%.*s'",
              (int32_t)output.length, output.str);
    vkr_mesh_loader_gltf_set_error(info,
                                   VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    return VKR_VKT_PACK_FAILED;
  }
  if (info->out_generated_asset_paths &&
      !vkr_mesh_loader_gltf_push_unique_path(info->out_generated_asset_paths,
                                             output, info->load_allocator)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return VKR_VKT_PACK_FAILED;
  }
  *out_texture = vkr_mesh_loader_gltf_append_query(
      info->load_allocator, output,
      base ? "cs=srgb&tc=color_srgb" : "tc=data_mask");
  return VKR_VKT_PACK_SUCCESS;
}

// =============================================================================
// Parallel material files
// =============================================================================

/* Runs `worker(context)` on up to `max_workers` threads, the caller among
   them; a failed thread start only reduces parallelism. */
vkr_internal void
vkr_mesh_loader_gltf_run_workers(const VkrMeshLoaderGltfParseInfo *info,
                                 uint32_t items, uint32_t max_workers,
                                 void *(*worker)(void *), void *context) {
  const uint32_t cores = Max(vkr_platform_get_logical_core_count(), 1u);
  const uint32_t worker_count = Max(Min(Min(cores, items), max_workers), 1u);
  VkrThread threads[16] = {0};
  uint32_t started = 0u;
  for (uint32_t i = 1u; i < worker_count && started < ArrayCount(threads);
       ++i) {
    if (vkr_thread_create(info->load_allocator, &threads[started], worker,
                          context)) {
      ++started;
    }
  }
  (void)worker(context);
  for (uint32_t i = 0u; i < started; ++i) {
    (void)vkr_thread_join(threads[i]);
    (void)vkr_thread_destroy(info->load_allocator, &threads[i]);
  }
}

/* A generated material's text, built in memory and written at once, so a
   cook that records ready materials can also copy it into its ready log. */
typedef struct VkrMeshLoaderGltfText {
  VkrAllocator *allocator;
  uint8_t *data;
  uint64_t length;
  uint64_t capacity;
} VkrMeshLoaderGltfText;

/* Appends `length` bytes to `text`, growing it in its allocator. */
vkr_internal bool8_t vkr_mesh_loader_gltf_text_append(
    VkrMeshLoaderGltfText *text, const void *bytes, uint64_t length) {
  if (text->length + length > text->capacity) {
    const uint64_t capacity =
        Max(text->capacity * 2u, text->length + length + KB(1));
    uint8_t *grown = vkr_allocator_alloc(text->allocator, capacity,
                                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!grown) {
      return false_v;
    }
    if (text->length) {
      MemCopy(grown, text->data, text->length);
    }
    text->data = grown;
    text->capacity = capacity;
  }
  if (length) {
    MemCopy(text->data + text->length, bytes, length);
  }
  text->length += length;
  return true_v;
}

vkr_internal bool8_t
vkr_mesh_loader_gltf_write_line(VkrMeshLoaderGltfText *text, String8 line) {
  if (!line.str || line.length == 0) {
    return true_v;
  }
  return vkr_mesh_loader_gltf_text_append(text, line.str, line.length) &&
         vkr_mesh_loader_gltf_text_append(text, "\n", 1u);
}

typedef struct VkrMeshLoaderGltfTextureWriteLine {
  const char *key;
  String8 value;
  const char *prefix_literal;
} VkrMeshLoaderGltfTextureWriteLine;

vkr_internal bool8_t vkr_mesh_loader_gltf_write_literal_line(
    VkrMeshLoaderGltfText *text, const char *literal) {
  if (!literal || literal[0] == '\0') {
    return true_v;
  }
  String8 line = string8_create_from_cstr((const uint8_t *)literal,
                                          string_length(literal));
  return vkr_mesh_loader_gltf_write_line(text, line);
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_string(
    VkrMeshLoaderGltfText *text, VkrAllocator *allocator, const char *key,
    String8 value) {
  if (!text || !allocator || !key || !value.str || value.length == 0) {
    return false_v;
  }

  return vkr_mesh_loader_gltf_write_line(
      text, string8_create_formatted(allocator, "%s=%.*s", key,
                                     (int32_t)value.length, value.str));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_f32(
    VkrMeshLoaderGltfText *text, VkrAllocator *allocator, const char *key,
    float32_t value) {
  if (!text || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      text, string8_create_formatted(allocator, "%s=%f", key, value));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_vec3(
    VkrMeshLoaderGltfText *text, VkrAllocator *allocator, const char *key,
    Vec3 value) {
  if (!text || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      text, string8_create_formatted(allocator, "%s=%f,%f,%f", key, value.x,
                                     value.y, value.z));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_key_vec4(
    VkrMeshLoaderGltfText *text, VkrAllocator *allocator, const char *key,
    Vec4 value) {
  if (!text || !allocator || !key) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_line(
      text, string8_create_formatted(allocator, "%s=%f,%f,%f,%f", key, value.x,
                                     value.y, value.z, value.w));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_optional_texture_line(
    VkrMeshLoaderGltfText *text, VkrAllocator *allocator,
    const VkrMeshLoaderGltfTextureWriteLine *line) {
  if (!text || !allocator || !line) {
    return false_v;
  }
  if (!line->value.str || line->value.length == 0) {
    return true_v;
  }
  if (!vkr_mesh_loader_gltf_write_literal_line(text, line->prefix_literal)) {
    return false_v;
  }
  return vkr_mesh_loader_gltf_write_key_string(text, allocator, line->key,
                                               line->value);
}

/* Layer maps share one portable external-image and sampler contract. */
vkr_internal bool8_t vkr_mesh_loader_gltf_validate_layer_views(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    const cgltf_texture_view *const *views, uint32_t view_count) {
  for (uint32_t i = 0; i < view_count; ++i) {
    const cgltf_texture_view *view = views[i];
    if (!view->texture)
      continue;
    const cgltf_image *image = view->texture->image;
    const cgltf_sampler *sampler = view->texture->sampler;
    if (view->texcoord != 0u || view->has_transform || !image || !image->uri ||
        !image->uri[0] || image->buffer_view ||
        (image->uri[0] == 'd' && image->uri[1] == 'a' && image->uri[2] == 't' &&
         image->uri[3] == 'a' && image->uri[4] == ':') ||
        (sampler &&
         (sampler->wrap_s != cgltf_wrap_mode_repeat ||
          sampler->wrap_t != cgltf_wrap_mode_repeat ||
          (sampler->mag_filter != cgltf_filter_type_undefined &&
           sampler->mag_filter != cgltf_filter_type_linear) ||
          (sampler->min_filter != cgltf_filter_type_undefined &&
           sampler->min_filter != cgltf_filter_type_linear_mipmap_linear)))) {
      log_error(
          "MeshLoader(glTF): layer map %u in '%s' requires an external image, "
          "untransformed UV0 and repeat/linear mip-linear sampling",
          i, material->name ? material->name : "<unnamed>");
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_validate_clearcoat(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material) {
  if (!material->has_clearcoat)
    return true_v;
  const cgltf_clearcoat *coat = &material->clearcoat;
  if (material->has_pbr_specular_glossiness || material->unlit ||
      !isfinite(coat->clearcoat_factor) || coat->clearcoat_factor < 0.0f ||
      coat->clearcoat_factor > 1.0f ||
      !isfinite(coat->clearcoat_roughness_factor) ||
      coat->clearcoat_roughness_factor < 0.0f ||
      coat->clearcoat_roughness_factor > 1.0f ||
      !isfinite(coat->clearcoat_normal_texture.scale)) {
    log_error(
        "MeshLoader(glTF): invalid or incompatible clearcoat material '%s'",
        material->name ? material->name : "<unnamed>");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  const cgltf_texture_view *views[] = {&coat->clearcoat_texture,
                                       &coat->clearcoat_roughness_texture,
                                       &coat->clearcoat_normal_texture};
  return vkr_mesh_loader_gltf_validate_layer_views(info, material, views,
                                                   ArrayCount(views));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_validate_sheen(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material) {
  if (!material->has_sheen)
    return true_v;
  const cgltf_sheen *sheen = &material->sheen;
  if (material->has_pbr_specular_glossiness || material->unlit ||
      !isfinite(sheen->sheen_roughness_factor) ||
      sheen->sheen_roughness_factor < 0.0f ||
      sheen->sheen_roughness_factor > 1.0f ||
      !isfinite(sheen->sheen_color_factor[0]) ||
      sheen->sheen_color_factor[0] < 0.0f ||
      sheen->sheen_color_factor[0] > 1.0f ||
      !isfinite(sheen->sheen_color_factor[1]) ||
      sheen->sheen_color_factor[1] < 0.0f ||
      sheen->sheen_color_factor[1] > 1.0f ||
      !isfinite(sheen->sheen_color_factor[2]) ||
      sheen->sheen_color_factor[2] < 0.0f ||
      sheen->sheen_color_factor[2] > 1.0f) {
    log_error("MeshLoader(glTF): invalid or incompatible sheen material '%s'",
              material->name ? material->name : "<unnamed>");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  const cgltf_texture_view *views[] = {&sheen->sheen_color_texture,
                                       &sheen->sheen_roughness_texture};
  return vkr_mesh_loader_gltf_validate_layer_views(info, material, views,
                                                   ArrayCount(views));
}

vkr_internal bool8_t vkr_mesh_loader_gltf_validate_anisotropy(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material) {
  if (!material->has_anisotropy)
    return true_v;
  const cgltf_anisotropy *anisotropy = &material->anisotropy;
  if (material->has_pbr_specular_glossiness || material->unlit ||
      !isfinite(anisotropy->anisotropy_strength) ||
      anisotropy->anisotropy_strength < 0.0f ||
      anisotropy->anisotropy_strength > 1.0f ||
      !isfinite(anisotropy->anisotropy_rotation) ||
      (anisotropy->anisotropy_strength > 0.0f && material->has_transmission &&
       material->transmission.transmission_factor > 0.0f)) {
    log_error("MeshLoader(glTF): invalid anisotropy or unsupported anisotropic "
              "refraction in '%s'",
              material->name ? material->name : "<unnamed>");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  const cgltf_texture_view *views[] = {&anisotropy->anisotropy_texture};
  return vkr_mesh_loader_gltf_validate_layer_views(info, material, views,
                                                   ArrayCount(views));
}

/** Texture paths a generated material file references; empty paths are
    omitted from the file. */
typedef struct VkrMeshLoaderGltfMaterialTextures {
  String8 base_color_texture;
  String8 metallic_roughness_texture;
  String8 occlusion_texture;
  String8 normal_texture;
  String8 emissive_texture;
  String8 transmission_texture;
  String8 thickness_texture;
  String8 anisotropy_texture;
  String8 sheen_color_texture;
  String8 sheen_roughness_texture;
  String8 clearcoat_texture;
  String8 clearcoat_roughness_texture;
  String8 clearcoat_normal_texture;
} VkrMeshLoaderGltfMaterialTextures;

/**
 * Resolves the material's texture paths. A specular-glossiness material keeps
 * the base color and metallic-roughness textures it already prepared.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_resolve_material_textures(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    const cgltf_texture_view *base_color_texture_view,
    const cgltf_texture_view *metallic_roughness_texture_view,
    VkrMeshLoaderGltfMaterialTextures *textures) {
  if ((!material->has_pbr_specular_glossiness &&
       (!vkr_mesh_loader_gltf_resolve_texture_path(
            info, base_color_texture_view, "cs=srgb&tc=color_srgb",
            &textures->base_color_texture) ||
        !vkr_mesh_loader_gltf_resolve_texture_path(
            info, metallic_roughness_texture_view, "tc=data_mask",
            &textures->metallic_roughness_texture))) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->occlusion_texture, "tc=data_mask",
          &textures->occlusion_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->normal_texture, "tc=normal_rg",
          &textures->normal_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info, &material->emissive_texture, "cs=srgb&tc=color_srgb",
          &textures->emissive_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_transmission
              ? &material->transmission.transmission_texture
              : NULL,
          "tc=data_mask", &textures->transmission_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_volume ? &material->volume.thickness_texture : NULL,
          "tc=data_mask", &textures->thickness_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_clearcoat ? &material->clearcoat.clearcoat_texture
                                  : NULL,
          "tc=data_mask", &textures->clearcoat_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_clearcoat
              ? &material->clearcoat.clearcoat_roughness_texture
              : NULL,
          "tc=data_mask", &textures->clearcoat_roughness_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_clearcoat
              ? &material->clearcoat.clearcoat_normal_texture
              : NULL,
          "tc=normal_rg", &textures->clearcoat_normal_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_sheen ? &material->sheen.sheen_color_texture : NULL,
          "cs=srgb&tc=color_srgb", &textures->sheen_color_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_sheen ? &material->sheen.sheen_roughness_texture : NULL,
          "tc=data_mask", &textures->sheen_roughness_texture) ||
      !vkr_mesh_loader_gltf_resolve_texture_path(
          info,
          material->has_anisotropy ? &material->anisotropy.anisotropy_texture
                                   : NULL,
          "tc=data_mask", &textures->anisotropy_texture)) {
    return false_v;
  }
  return true_v;
}

/** Writes the anisotropy, sheen, and clearcoat factors a material uses. */
vkr_internal bool8_t vkr_mesh_loader_gltf_write_extension_lines(
    VkrMeshLoaderGltfText *text, const VkrMeshLoaderGltfParseInfo *info,
    const cgltf_material *material) {
  bool8_t ok = true_v;
  if (material->has_anisotropy) {
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "anisotropy_strength",
                   material->anisotropy.anisotropy_strength);
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "anisotropy_rotation",
                   remainderf(material->anisotropy.anisotropy_rotation,
                              6.283185307179586f));
  }
  if (material->has_sheen) {
    ok = ok && vkr_mesh_loader_gltf_write_key_vec3(
                   text, info->load_allocator, "sheen_color",
                   vec3_new(material->sheen.sheen_color_factor[0],
                            material->sheen.sheen_color_factor[1],
                            material->sheen.sheen_color_factor[2]));
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "sheen_roughness",
                   material->sheen.sheen_roughness_factor);
  }

  if (material->has_clearcoat) {
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "clearcoat_factor",
                   material->clearcoat.clearcoat_factor);
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "clearcoat_roughness",
                   material->clearcoat.clearcoat_roughness_factor);
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   text, info->load_allocator, "clearcoat_normal_scale",
                   material->clearcoat.clearcoat_normal_texture.texture
                       ? material->clearcoat.clearcoat_normal_texture.scale
                       : 1.0f);
  }
  return ok;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_texture_lines(
    VkrMeshLoaderGltfText *text, const VkrMeshLoaderGltfParseInfo *info,
    const VkrMeshLoaderGltfMaterialTextures *textures) {
  const VkrMeshLoaderGltfTextureWriteLine texture_lines[] = {
      {.key = "base_color_texture",
       .value = textures->base_color_texture,
       .prefix_literal = NULL},
      {.key = "metallic_roughness_texture",
       .value = textures->metallic_roughness_texture,
       .prefix_literal = NULL},
      {.key = "occlusion_texture",
       .value = textures->occlusion_texture,
       .prefix_literal = NULL},
      {.key = "normal_texture",
       .value = textures->normal_texture,
       .prefix_literal = NULL},
      {.key = "emissive_texture",
       .value = textures->emissive_texture,
       .prefix_literal = "emissive_colorspace=srgb"},
      {.key = "transmission_texture",
       .value = textures->transmission_texture,
       .prefix_literal = NULL},
      {.key = "sheen_color_texture",
       .value = textures->sheen_color_texture,
       .prefix_literal = "sheen_color_colorspace=srgb"},
      {.key = "sheen_roughness_texture",
       .value = textures->sheen_roughness_texture},
      {.key = "anisotropy_texture", .value = textures->anisotropy_texture},
      {.key = "clearcoat_texture", .value = textures->clearcoat_texture},
      {.key = "clearcoat_roughness_texture",
       .value = textures->clearcoat_roughness_texture},
      {.key = "clearcoat_normal_texture",
       .value = textures->clearcoat_normal_texture},
      {.key = "thickness_texture",
       .value = textures->thickness_texture,
       .prefix_literal = NULL},
  };

  bool8_t ok = true_v;
  for (uint32_t i = 0; i < ArrayCount(texture_lines); ++i) {
    ok = ok && vkr_mesh_loader_gltf_write_optional_texture_line(
                   text, info->load_allocator, &texture_lines[i]);
  }
  return ok;
}

/* Writes `text` to `path` through a synced temporary file and a rename. Two
   cooks of one source (a manifest's plain and light-range targets) publish the
   same materials at once, so the temporary name carries the process. */
vkr_internal bool8_t vkr_mesh_loader_gltf_write_text_atomic(
    const VkrMeshLoaderGltfParseInfo *info, String8 path,
    const VkrMeshLoaderGltfText *text) {
  FilePath file_path = file_path_create(
      (const char *)path.str, info->load_allocator,
      vkr_mesh_loader_gltf_path_is_absolute(path) ? FILE_PATH_TYPE_ABSOLUTE
                                                  : FILE_PATH_TYPE_RELATIVE);
  String8 temp_path = string8_create_formatted(
      info->load_allocator, "%.*s.tmp.%u", (int32_t)path.length, path.str,
      vkr_platform_get_process_id());
  FilePath temp_file_path =
      file_path_create((const char *)temp_path.str, info->load_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(temp_path)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle file = {0};
  FileError open_error = file_open(&temp_file_path, mode, &file);
  if (open_error != FILE_ERROR_NONE) {
    log_error("MeshLoader(glTF): failed to open generated material '%s': %s",
              file_path.path.str, file_get_error_string(open_error).str);
    return false_v;
  }
  uint64_t written = 0u;
  const bool8_t ok = file_write(&file, text->length, text->data, &written) ==
                         FILE_ERROR_NONE &&
                     written == text->length &&
                     file_sync(&file) == FILE_ERROR_NONE;
  file_close(&file);
  if (!ok ||
      file_rename(&temp_file_path, &file_path, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temp_file_path);
    return false_v;
  }
  return true_v;
}

/* True when every texture a material names is a packed file at an absolute
   path, which stays valid after the revision being built moves. */
vkr_internal bool8_t vkr_mesh_loader_gltf_textures_are_final(
    const VkrMeshLoaderGltfMaterialTextures *textures) {
  const String8 values[] = {
      textures->base_color_texture,       textures->metallic_roughness_texture,
      textures->occlusion_texture,        textures->normal_texture,
      textures->emissive_texture,         textures->transmission_texture,
      textures->thickness_texture,        textures->anisotropy_texture,
      textures->sheen_color_texture,      textures->sheen_roughness_texture,
      textures->clearcoat_texture,        textures->clearcoat_roughness_texture,
      textures->clearcoat_normal_texture,
  };
  for (uint32_t i = 0u; i < ArrayCount(values); ++i) {
    const String8 path = vkr_mesh_loader_gltf_strip_query(values[i]);
    if (path.length && (!vkr_mesh_loader_gltf_path_is_absolute(path) ||
                        !vkr_mesh_loader_gltf_has_vkt_extension(path))) {
      return false_v;
    }
  }
  return true_v;
}

/* Appends `value` to `text` as the body of a JSON string. */
vkr_internal bool8_t vkr_mesh_loader_gltf_text_append_json(
    VkrMeshLoaderGltfText *text, const uint8_t *value, uint64_t length) {
  bool8_t ok = true_v;
  for (uint64_t i = 0u; ok && i < length; ++i) {
    const uint8_t c = value[i];
    char escaped[8];
    if (c == '"' || c == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)c;
      ok = vkr_mesh_loader_gltf_text_append(text, escaped, 2u);
    } else if (c == '\n') {
      ok = vkr_mesh_loader_gltf_text_append(text, "\\n", 2u);
    } else if (c < 0x20u) {
      const int32_t count = snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      ok = vkr_mesh_loader_gltf_text_append(text, escaped, (uint64_t)count);
    } else {
      ok = vkr_mesh_loader_gltf_text_append(text, &c, 1u);
    }
  }
  return ok;
}

/* Records a material whose textures are all final in the cook's ready log as
   one JSON line: its name, its file (against which the text's references
   resolve) and its text. The editor applies it to the live material before
   the revision publishes. A line appends whole under a lock and a reader
   ignores an unterminated last line; a record that fails to append only
   means the material appears when the revision publishes. */
vkr_internal void vkr_mesh_loader_gltf_record_ready(
    const VkrMeshLoaderGltfParseInfo *info, String8 material_id,
    String8 material_path, const VkrMeshLoaderGltfText *text,
    const VkrMeshLoaderGltfMaterialTextures *textures) {
  if (!info->ready_log || !info->ready_log[0] ||
      !vkr_mesh_loader_gltf_textures_are_final(textures)) {
    return;
  }
  VkrMeshLoaderGltfText line = {.allocator = info->scratch_allocator};
  const bool8_t built =
      vkr_mesh_loader_gltf_text_append(&line, "{\"material\":\"", 13u) &&
      vkr_mesh_loader_gltf_text_append_json(&line, material_id.str,
                                            material_id.length) &&
      vkr_mesh_loader_gltf_text_append(&line, "\",\"path\":\"", 10u) &&
      vkr_mesh_loader_gltf_text_append_json(&line, material_path.str,
                                            material_path.length) &&
      vkr_mesh_loader_gltf_text_append(&line, "\",\"definition\":\"", 16u) &&
      vkr_mesh_loader_gltf_text_append_json(&line, text->data, text->length) &&
      vkr_mesh_loader_gltf_text_append(&line, "\"}\n", 3u);
  if (!built) {
    return;
  }
  vkr_local_persist VkrAtomicBool append_lock = false_v;
  while (vkr_atomic_bool_exchange(&append_lock, true_v,
                                  VKR_MEMORY_ORDER_ACQUIRE)) {
    vkr_platform_sleep(0u);
  }
  FILE *log = file_fopen(info->ready_log, "ab");
  const bool8_t appended =
      log && fwrite(line.data, 1u, (size_t)line.length, log) == line.length;
  if (log) {
    (void)fclose(log);
  }
  vkr_atomic_bool_store(&append_lock, false_v, VKR_MEMORY_ORDER_RELEASE);
  if (!appended) {
    log_warn("MeshLoader(glTF): ready record for '%.*s' was not written",
             (int32_t)material_id.length, material_id.str);
  }
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_material_file(
    const VkrMeshLoaderGltfParseInfo *info, String8 material_id,
    const cgltf_material *material, String8 material_path, uint64_t source_hash,
    bool8_t non_unit_vertex_alpha) {
  if (!info || !material_id.str || material_id.length == 0 ||
      !material_path.str || material_path.length == 0) {
    return false_v;
  }

  const cgltf_pbr_metallic_roughness *pbr = &material->pbr_metallic_roughness;
  const cgltf_texture_view *base_color_texture_view = &pbr->base_color_texture;
  const cgltf_texture_view *metallic_roughness_texture_view =
      &pbr->metallic_roughness_texture;
  Vec4 base_color = vec4_new(1.0f, 1.0f, 1.0f, 1.0f);
  float32_t metallic = 1.0f;
  float32_t roughness = 1.0f;
  Vec3 dielectric_specular = vec3_new(0.04f, 0.04f, 0.04f);
  VkrMeshLoaderGltfPreparedSpecGloss prepared_spec_gloss = {0};
  /* A managed cook packs converted images itself: as cutouts, as the
     roughness of a paired bake or, otherwise, as plain textures. */
  VkrMeshLoaderGltfConvertedSpecGloss converted = {0};
  bool8_t pack_base = false_v;
  bool8_t pack_metal_rough = false_v;
  if (material->has_pbr_specular_glossiness) {
    if (!vkr_mesh_loader_gltf_convert_spec_gloss(info, material, source_hash,
                                                 true_v, &converted)) {
      return false_v;
    }
    const bool8_t packs = converted.textured && info->generated_root.length;
    pack_base = packs && !converted.base_uniform;
    pack_metal_rough = packs && !converted.metal_rough_uniform;
    vkr_mesh_loader_gltf_prepare_spec_gloss(info, &converted,
                                            &prepared_spec_gloss);
    base_color = prepared_spec_gloss.base_color_factor;
    metallic = prepared_spec_gloss.metallic_factor;
    roughness = prepared_spec_gloss.roughness_factor;
    dielectric_specular = prepared_spec_gloss.dielectric_specular_factor;
    base_color_texture_view = NULL;
    metallic_roughness_texture_view = NULL;
  } else if (material->has_pbr_metallic_roughness) {
    base_color = vec4_new(pbr->base_color_factor[0], pbr->base_color_factor[1],
                          pbr->base_color_factor[2], pbr->base_color_factor[3]);
    metallic = (float32_t)pbr->metallic_factor;
    roughness = (float32_t)pbr->roughness_factor;
  }

  // cgltf defaults a present texture view to scale 1, preserving authored zero.
  float32_t normal_scale = material->normal_texture.texture != NULL
                               ? (float32_t)material->normal_texture.scale
                               : 1.0f;
  float32_t occlusion_strength =
      material->occlusion_texture.scale != 0.0f
          ? (float32_t)material->occlusion_texture.scale
          : 1.0f;
  Vec3 emissive_factor =
      vec3_new(material->emissive_factor[0], material->emissive_factor[1],
               material->emissive_factor[2]);
  const float32_t transmission_factor =
      material->has_transmission
          ? (float32_t)material->transmission.transmission_factor
          : 0.0f;
  if (transmission_factor > 0.0f && !material->has_pbr_specular_glossiness &&
      !pbr->metallic_roughness_texture.texture) {
    metallic = pbr->has_metallic_factor ? metallic : 0.0f;
    roughness = pbr->has_roughness_factor ? roughness : 0.0f;
  }
  const float32_t ior = material->has_ior ? (float32_t)material->ior.ior : 1.5f;
  const float32_t thickness_factor =
      material->has_volume ? (float32_t)material->volume.thickness_factor
                           : 0.0f;
  const Vec3 attenuation_color =
      material->has_volume ? vec3_new(material->volume.attenuation_color[0],
                                      material->volume.attenuation_color[1],
                                      material->volume.attenuation_color[2])
                           : vec3_new(1.0f, 1.0f, 1.0f);
  const float32_t attenuation_distance =
      material->has_volume && material->volume.attenuation_distance < 1.0e30f
          ? (float32_t)material->volume.attenuation_distance
          : 0.0f;

  float32_t alpha_cutoff = (float32_t)material->alpha_cutoff;
  if (material->alpha_mode == cgltf_alpha_mode_mask && alpha_cutoff <= 0.0f) {
    alpha_cutoff = 0.5f;
  }

  String8 alpha_mode =
      vkr_mesh_loader_gltf_alpha_mode_to_string(material->alpha_mode);

  VkrMeshLoaderGltfMaterialTextures textures = {0};

  if (material->has_pbr_specular_glossiness) {
    textures.base_color_texture = prepared_spec_gloss.base_color_texture;
    textures.metallic_roughness_texture =
        prepared_spec_gloss.metallic_roughness_texture;
  }

  if (!info->defer_textures &&
      !vkr_mesh_loader_gltf_resolve_material_textures(
          info, material, base_color_texture_view,
          metallic_roughness_texture_view, &textures)) {
    return false_v;
  }

  /* Each pack below retries once with converted pixels when a memo hit's
     output turns out missing or stale. */
  if (material->alpha_mode == cgltf_alpha_mode_mask &&
      (pack_base || (textures.base_color_texture.str &&
                     textures.base_color_texture.length > 0u))) {
    if (non_unit_vertex_alpha) {
      log_warn("MeshLoader(glTF): skipping alpha-coverage variant for MASK "
               "material '%s' because COLOR_0 has non-unit alpha",
               material->name ? material->name : "<unnamed>");
    } else {
      VkrVktPackResult cutout = VKR_VKT_PACK_STALE;
      for (uint32_t attempt = 0u; cutout == VKR_VKT_PACK_STALE && attempt < 2u;
           ++attempt) {
        if (attempt > 0u && !vkr_mesh_loader_gltf_convert_pixels(
                                info, material, source_hash, &converted)) {
          return false_v;
        }
        const VkrVktSource source =
            vkr_mesh_loader_gltf_converted_source(&converted, true_v);
        cutout = vkr_mesh_loader_gltf_bake_cutout_variant(
            info, textures.base_color_texture, pack_base ? &source : NULL,
            alpha_cutoff, base_color.w, &textures.base_color_texture);
      }
      if (cutout != VKR_VKT_PACK_SUCCESS) {
        return false_v;
      }
      pack_base = false_v;
    }
  }

  /* The pairing below folds the roughness factor into its texture; the file
     keeps the factor as the bound the texture scales, which the renderer
     needs to tell rough glass from smooth (ADR-087). */
  const float32_t roughness_max = roughness;
  if (textures.normal_texture.str && textures.normal_texture.length > 0u) {
    String8 cooked_normal_texture = {0};
    String8 cooked_metallic_roughness_texture = {0};
    VkrVktPackResult pair_result = VKR_VKT_PACK_STALE;
    for (uint32_t attempt = 0u;
         pair_result == VKR_VKT_PACK_STALE && attempt < 2u; ++attempt) {
      if (attempt > 0u && !vkr_mesh_loader_gltf_convert_pixels(
                              info, material, source_hash, &converted)) {
        return false_v;
      }
      const VkrVktSource source =
          vkr_mesh_loader_gltf_converted_source(&converted, false_v);
      pair_result = vkr_mesh_loader_gltf_bake_normal_roughness_variant(
          info, material, &material->normal_texture,
          metallic_roughness_texture_view, textures.normal_texture,
          textures.metallic_roughness_texture,
          pack_metal_rough ? &source : NULL, normal_scale, roughness,
          &cooked_normal_texture, &cooked_metallic_roughness_texture);
    }
    if (pair_result == VKR_VKT_PACK_FAILED ||
        pair_result == VKR_VKT_PACK_STALE) {
      return false_v;
    }
    if (pair_result == VKR_VKT_PACK_SUCCESS) {
      textures.normal_texture = cooked_normal_texture;
      textures.metallic_roughness_texture = cooked_metallic_roughness_texture;
      normal_scale = 1.0f;
      roughness = 1.0f;
      pack_metal_rough = false_v;
    }
  }

  const bool8_t plain_packs[2] = {pack_base, pack_metal_rough};
  String8 *plain_textures[2] = {&textures.base_color_texture,
                                &textures.metallic_roughness_texture};
  for (uint32_t i = 0u; i < ArrayCount(plain_packs); ++i) {
    VkrVktPackResult packed =
        plain_packs[i] ? VKR_VKT_PACK_STALE : VKR_VKT_PACK_SUCCESS;
    for (uint32_t attempt = 0u; packed == VKR_VKT_PACK_STALE && attempt < 2u;
         ++attempt) {
      if (attempt > 0u && !vkr_mesh_loader_gltf_convert_pixels(
                              info, material, source_hash, &converted)) {
        return false_v;
      }
      packed = vkr_mesh_loader_gltf_pack_converted(
          info, source_hash, &converted, i == 0u,
          material->alpha_mode == cgltf_alpha_mode_opaque, plain_textures[i]);
    }
    if (packed != VKR_VKT_PACK_SUCCESS) {
      return false_v;
    }
  }

  VkrMeshLoaderGltfText text = {.allocator = info->load_allocator};
  bool8_t ok = true_v;
  ok = ok && vkr_mesh_loader_gltf_write_key_string(&text, info->load_allocator,
                                                   "name", material_id);
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(&text, "type=pbr");
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(
                 &text, "base_color_colorspace=srgb");
  ok = ok && vkr_mesh_loader_gltf_write_key_vec4(&text, info->load_allocator,
                                                 "base_color", base_color);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "metallic", metallic);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "roughness", roughness);
  if (textures.metallic_roughness_texture.str &&
      textures.metallic_roughness_texture.length > 0u) {
    ok = ok && vkr_mesh_loader_gltf_write_key_f32(
                   &text, info->load_allocator, "roughness_max", roughness_max);
  }
  ok = ok && vkr_mesh_loader_gltf_write_key_vec3(&text, info->load_allocator,
                                                 "dielectric_specular",
                                                 dielectric_specular);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "normal_scale", normal_scale);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "occlusion_strength",
                                                occlusion_strength);
  ok = ok &&
       vkr_mesh_loader_gltf_write_key_vec3(&text, info->load_allocator,
                                           "emissive_factor", emissive_factor);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "transmission_factor",
                                                transmission_factor);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "ior", ior);
  ok = ok &&
       vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                          "thickness_factor", thickness_factor);
  ok = ok && vkr_mesh_loader_gltf_write_key_vec3(&text, info->load_allocator,
                                                 "attenuation_color",
                                                 attenuation_color);
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "attenuation_distance",
                                                attenuation_distance);
  ok = ok && vkr_mesh_loader_gltf_write_key_string(&text, info->load_allocator,
                                                   "alpha_mode", alpha_mode);
  ok = ok && vkr_mesh_loader_gltf_write_literal_line(
                 &text, material->double_sided ? "double_sided=true"
                                               : "double_sided=false");
  ok = ok && vkr_mesh_loader_gltf_write_key_f32(&text, info->load_allocator,
                                                "alpha_cutoff", alpha_cutoff);

  ok = ok && vkr_mesh_loader_gltf_write_extension_lines(&text, info, material);
  ok = ok && vkr_mesh_loader_gltf_write_texture_lines(&text, info, &textures);

  if (!ok ||
      !vkr_mesh_loader_gltf_write_text_atomic(info, material_path, &text)) {
    log_error("MeshLoader(glTF): failed publishing generated material '%.*s'",
              (int32_t)material_path.length, material_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }
  vkr_mesh_loader_gltf_record_ready(info, material_id, material_path, &text,
                                    &textures);
  return true_v;
}

/* One material's file and derived textures: spec-gloss conversion, cutout
   variants, paired normal/roughness bakes and converted packs run on workers
   with private arenas; the paths of the generated textures return in malloc
   storage. */
typedef struct VkrMeshLoaderGltfMaterialWork {
  String8 material_id;
  String8 material_path;
  bool8_t non_unit_vertex_alpha;
  bool8_t succeeded;
  VkrRendererError error;
  String8 *generated;
  uint32_t generated_count;
} VkrMeshLoaderGltfMaterialWork;

typedef struct VkrMeshLoaderGltfMaterialJob {
  const VkrMeshLoaderGltfParseInfo *info;
  const cgltf_data *data;
  uint64_t source_hash;
  const uint32_t *order; /* Material indices in the order workers take them. */
  VkrMeshLoaderGltfMaterialWork *work;
  uint32_t count;
  VkrAtomicUint32 next;
} VkrMeshLoaderGltfMaterialJob;

/* A worker's arenas hold at most one material's images at a time. */
#define VKR_GLTF_CONVERT_ARENA_RESERVE GB(4)
#define VKR_GLTF_CONVERT_ARENA_COMMIT MB(16)

/* With the native block encoders, decoding and mips weigh as much as the
   encode, so each logical core takes a material, up to the worker array's
   16. */
#define VKR_GLTF_MATERIAL_MAX_WORKERS 16u

vkr_internal void *vkr_mesh_loader_gltf_material_worker(void *argument) {
  VkrMeshLoaderGltfMaterialJob *job = argument;
  Arena *load_arena = arena_create(VKR_GLTF_CONVERT_ARENA_RESERVE,
                                   VKR_GLTF_CONVERT_ARENA_COMMIT);
  Arena *scratch_arena = arena_create(VKR_GLTF_CONVERT_ARENA_RESERVE,
                                      VKR_GLTF_CONVERT_ARENA_COMMIT);
  VkrAllocator load = {.ctx = load_arena};
  VkrAllocator scratch = {.ctx = scratch_arena};
  const bool8_t ready = load_arena && scratch_arena &&
                        vkr_allocator_arena(&load) &&
                        vkr_allocator_arena(&scratch);
  for (;;) {
    const uint32_t position =
        vkr_atomic_uint32_fetch_add(&job->next, 1u, VKR_MEMORY_ORDER_RELAXED);
    if (position >= job->count) {
      break;
    }
    const uint32_t index = job->order[position];
    VkrMeshLoaderGltfMaterialWork *work = &job->work[index];
    if (!ready) {
      work->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }
    VkrAllocatorScope scope = vkr_allocator_begin_scope(&load);
    Array_String8 generated = {.allocator = &load};
    VkrMeshLoaderGltfParseInfo private_info = *job->info;
    private_info.load_allocator = &load;
    private_info.scratch_allocator = &scratch;
    private_info.out_error = &work->error;
    private_info.out_generated_asset_paths =
        job->info->out_generated_asset_paths ? &generated : NULL;
    bool8_t ok = vkr_mesh_loader_gltf_write_material_file(
        &private_info, work->material_id, &job->data->materials[index],
        work->material_path, job->source_hash, work->non_unit_vertex_alpha);
    if (ok && generated.length) {
      work->generated = calloc(generated.length, sizeof(String8));
      ok = work->generated != NULL;
      for (uint64_t g = 0; ok && g < generated.length; ++g) {
        const String8 path = *array_get_String8(&generated, g);
        uint8_t *copy = malloc(path.length ? path.length : 1u);
        ok = copy != NULL;
        if (ok) {
          MemCopy(copy, path.str, path.length);
          work->generated[work->generated_count++] =
              (String8){.str = copy, .length = path.length};
        }
      }
      if (!ok) {
        work->error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      }
    }
    work->succeeded = ok;
    vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (load_arena) {
    arena_destroy(load_arena);
  }
  if (scratch_arena) {
    arena_destroy(scratch_arena);
  }
  return NULL;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_write_material_files(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    String8 *material_paths, Array_String8 *out_generated_material_paths) {
  if (!info || !data || !material_paths) {
    return false_v;
  }
  if (data->materials_count == 0) {
    return true_v;
  }

  for (cgltf_size i = 0; i < data->materials_count; ++i) {
    if (!vkr_mesh_loader_gltf_validate_clearcoat(info, &data->materials[i]) ||
        !vkr_mesh_loader_gltf_validate_sheen(info, &data->materials[i]) ||
        !vkr_mesh_loader_gltf_validate_anisotropy(info, &data->materials[i]))
      return false_v;
  }

  /* Check tangent-space availability before publishing any material file. */
  for (cgltf_size m = 0; m < data->meshes_count; ++m) {
    for (cgltf_size p = 0; p < data->meshes[m].primitives_count; ++p) {
      const cgltf_primitive *primitive = &data->meshes[m].primitives[p];
      const cgltf_material *material = primitive->material;
      if (!material || !material->has_anisotropy ||
          material->anisotropy.anisotropy_strength <= 0.0f)
        continue;
      const cgltf_accessor *normal =
          cgltf_find_accessor(primitive, cgltf_attribute_type_normal, 0);
      const cgltf_accessor *tangent =
          cgltf_find_accessor(primitive, cgltf_attribute_type_tangent, 0);
      const cgltf_accessor *uv =
          cgltf_find_accessor(primitive, cgltf_attribute_type_texcoord, 0);
      if ((!(normal && tangent) &&
           (!material->normal_texture.texture || !uv)) ||
          (!uv && (material->anisotropy.anisotropy_texture.texture ||
                   material->normal_texture.texture))) {
        log_error("MeshLoader(glTF): anisotropy requires NORMAL/TANGENT or a "
                  "normal texture with UV0");
        vkr_mesh_loader_gltf_set_error(info,
                                       VKR_RENDERER_ERROR_INVALID_PARAMETER);
        return false_v;
      }
      const cgltf_texture_view *normal_view[] = {&material->normal_texture};
      if (!vkr_mesh_loader_gltf_validate_layer_views(info, material,
                                                     normal_view, 1u))
        return false_v;
    }
  }

  String8 material_dir = string8_create_formatted(
      info->load_allocator, "assets/materials/%.*s",
      (int32_t)info->source_stem.length, info->source_stem.str);
  if (info->bundle_root.length) {
    material_dir = file_path_join(info->load_allocator, info->bundle_root,
                                  string8_lit("materials"));
  }
  FilePath material_dir_path =
      file_path_create((const char *)material_dir.str, info->load_allocator,
                       vkr_mesh_loader_gltf_path_is_absolute(material_dir)
                           ? FILE_PATH_TYPE_ABSOLUTE
                           : FILE_PATH_TYPE_RELATIVE);
  if (!file_ensure_directory(info->load_allocator, &material_dir_path.path)) {
    log_error("MeshLoader(glTF): failed to create material directory '%s'",
              string8_cstr(&material_dir));
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint64_t source_hash = vkr_mesh_loader_gltf_hash_source_path(
      info->import_id.length ? info->import_id : info->source_path);
  const uint32_t count = (uint32_t)data->materials_count;
  VkrMeshLoaderGltfMaterialWork *work =
      vkr_allocator_alloc(info->load_allocator, sizeof(*work) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!work) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  MemZero(work, sizeof(*work) * count);
  for (uint32_t i = 0; i < count; ++i) {
    const cgltf_material *material = &data->materials[i];
    const bool8_t has_base_texture =
        material->has_pbr_specular_glossiness
            ? material->pbr_specular_glossiness.diffuse_texture.texture ||
                  material->pbr_specular_glossiness.specular_glossiness_texture
                      .texture
            : material->pbr_metallic_roughness.base_color_texture.texture !=
                  NULL;
    if (material->alpha_mode == cgltf_alpha_mode_mask && has_base_texture &&
        !vkr_mesh_loader_gltf_material_has_non_unit_vertex_alpha(
            data, material, &work[i].non_unit_vertex_alpha)) {
      log_error("MeshLoader(glTF): failed to inspect COLOR_0 alpha for "
                "material %u",
                i);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    work[i].material_id = vkr_mesh_loader_gltf_make_material_id(
        info->load_allocator, source_hash, i);
    material_paths[i] = string8_create_formatted(
        info->load_allocator, "%.*s/%.*s.mt", (int32_t)material_dir.length,
        material_dir.str, (int32_t)work[i].material_id.length,
        work[i].material_id.str);
    work[i].material_path = material_paths[i];
  }

  /* Requested materials start first, so the ones the editor shows arrive
     first; the rest follow in material order. */
  uint32_t *order =
      vkr_allocator_alloc(info->load_allocator, sizeof(*order) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *ordered =
      vkr_allocator_alloc(info->load_allocator, sizeof(*ordered) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!order || !ordered) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  MemZero(ordered, sizeof(*ordered) * count);
  uint32_t ordered_count = 0u;
  for (uint32_t p = 0u; p < info->material_priority_count; ++p) {
    for (uint32_t i = 0u; i < count; ++i) {
      if (!ordered[i] &&
          string8_equals(&work[i].material_id, &info->material_priority[p])) {
        ordered[i] = true_v;
        order[ordered_count++] = i;
        break;
      }
    }
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (!ordered[i]) {
      order[ordered_count++] = i;
    }
  }

  VkrMeshLoaderGltfMaterialJob job = {.info = info,
                                      .data = data,
                                      .source_hash = source_hash,
                                      .order = order,
                                      .work = work,
                                      .count = count};
  /* The workers' sources do not change during the cook, so each is read and
     hashed once however many packs name it. */
  vkr_vkt_begin_file_hash_scope();
  vkr_mesh_loader_gltf_run_workers(info, count, VKR_GLTF_MATERIAL_MAX_WORKERS,
                                   vkr_mesh_loader_gltf_material_worker, &job);
  vkr_vkt_end_file_hash_scope();

  /* Results publish in material order, as a serial cook would. */
  bool8_t ok = true_v;
  for (uint32_t i = 0; i < count; ++i) {
    if (ok && !work[i].succeeded) {
      vkr_mesh_loader_gltf_set_error(info,
                                     work[i].error != VKR_RENDERER_ERROR_NONE
                                         ? work[i].error
                                         : VKR_RENDERER_ERROR_UNKNOWN);
      ok = false_v;
    }
    for (uint32_t g = 0; g < work[i].generated_count; ++g) {
      if (ok && info->out_generated_asset_paths) {
        String8 path = work[i].generated[g];
        String8 copy = string8_duplicate(info->load_allocator, &path);
        ok = copy.str &&
             vkr_mesh_loader_gltf_push_unique_path(
                 info->out_generated_asset_paths, copy, info->load_allocator);
      }
      free(work[i].generated[g].str);
    }
    free(work[i].generated);
    if (ok && out_generated_material_paths) {
      vkr_mesh_loader_gltf_push_unique_path(out_generated_material_paths,
                                            material_paths[i],
                                            info->load_allocator);
    }
  }
  return ok;
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

vkr_internal bool8_t vkr_mesh_loader_gltf_transform_unit_direction(
    Mat4 normal_matrix, Vec3 direction, Vec3 *out_direction) {
  assert_log(out_direction != NULL, "Out transformed direction is NULL");
  Vec4 d = mat4_mul_vec4(normal_matrix,
                         vec4_new(direction.x, direction.y, direction.z, 0.0f));
  Vec3 value = vec3_new(d.x, d.y, d.z);
  float32_t len_sq = vec3_length_squared(value);
  if (!isfinite(len_sq) || len_sq <= VKR_FLOAT_EPSILON * VKR_FLOAT_EPSILON)
    return false_v;
  *out_direction = vec3_normalize(value);
  return true_v;
}

vkr_internal Vec3 vkr_mesh_loader_gltf_transform_direction(Mat4 normal_matrix,
                                                           Vec3 direction,
                                                           Vec3 fallback) {
  Vec3 transformed = vec3_zero();
  return vkr_mesh_loader_gltf_transform_unit_direction(normal_matrix, direction,
                                                       &transformed)
             ? transformed
             : fallback;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_decal_normal_offset(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_material *material,
    float32_t sidecar_offset_meters, float32_t *out_offset_meters) {
  assert_log(out_offset_meters != NULL, "Out decal offset is NULL");
  *out_offset_meters = sidecar_offset_meters;
  if (!material || !material->extras.data) {
    return true_v;
  }

  String8 extras =
      string8_create_from_cstr((const uint8_t *)material->extras.data,
                               string_length(material->extras.data));
  VkrJsonReader reader = vkr_json_reader_from_string(extras);
  if (!vkr_json_find_field(&reader, "vkr_decal_normal_offset_meters")) {
    return true_v;
  }

  float64_t offset_meters = 0.0;
  if (!vkr_json_parse_double(&reader, &offset_meters) ||
      !isfinite(offset_meters) || offset_meters <= 0.0 ||
      offset_meters > VKR_GLTF_DECAL_NORMAL_OFFSET_MAX_METERS) {
    log_error("MeshLoader(glTF): material '%s' has invalid "
              "vkr_decal_normal_offset_meters; expected (0, %.3f] meters",
              material->name ? material->name : "<unnamed>",
              VKR_GLTF_DECAL_NORMAL_OFFSET_MAX_METERS);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  *out_offset_meters = (float32_t)offset_meters;
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_read_decal_overrides(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    VkrMeshLoaderGltfDecalOverrides *out_overrides) {
  assert_log(out_overrides != NULL, "Out decal overrides are NULL");
  *out_overrides = (VkrMeshLoaderGltfDecalOverrides){0};

  String8 sidecar_path = string8_create_formatted(
      info->scratch_allocator, "%.*s.vkr.json",
      (int32_t)info->source_path.length, info->source_path.str);
  if (!vkr_mesh_loader_gltf_path_exists(info->scratch_allocator,
                                        sidecar_path)) {
    return true_v;
  }

  FilePathType type = vkr_mesh_loader_gltf_path_is_absolute(sidecar_path)
                          ? FILE_PATH_TYPE_ABSOLUTE
                          : FILE_PATH_TYPE_RELATIVE;
  FilePath path = file_path_create((const char *)sidecar_path.str,
                                   info->scratch_allocator, type);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&path, mode, &file) != FILE_ERROR_NONE) {
    log_error("MeshLoader(glTF): failed to open import sidecar '%.*s'",
              (int32_t)sidecar_path.length, sidecar_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint8_t *bytes = NULL;
  uint64_t byte_count = 0;
  FileError read_error =
      file_read_all(&file, info->scratch_allocator, &bytes, &byte_count);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !bytes || byte_count == 0) {
    log_error("MeshLoader(glTF): failed to read import sidecar '%.*s'",
              (int32_t)sidecar_path.length, sidecar_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  VkrJsonReader reader = vkr_json_reader_create(bytes, byte_count);
  int32_t schema_version = 0;
  if (!vkr_json_get_int(&reader, "schema_version", &schema_version) ||
      schema_version != VKR_GLTF_IMPORT_SIDECAR_VERSION ||
      !vkr_json_find_array(&reader, "materials")) {
    log_error("MeshLoader(glTF): invalid import sidecar '%.*s'",
              (int32_t)sidecar_path.length, sidecar_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  float32_t *offsets = NULL;
  if (data->materials_count > 0) {
    offsets = (float32_t *)vkr_allocator_alloc(info->scratch_allocator,
                                               (uint64_t)data->materials_count *
                                                   sizeof(float32_t),
                                               VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!offsets) {
      vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return false_v;
    }
    MemZero(offsets, (uint64_t)data->materials_count * sizeof(float32_t));
  }

  while (vkr_json_next_array_element(&reader)) {
    VkrJsonReader entry = {0};
    String8 material_name = {0};
    float64_t offset_meters = 0.0;
    if (!vkr_json_enter_object(&reader, &entry) ||
        !vkr_json_get_string(&entry, "name", &material_name) ||
        material_name.length == 0 ||
        !vkr_json_get_double(&entry, "vkr_decal_normal_offset_meters",
                             &offset_meters) ||
        !isfinite(offset_meters) || offset_meters <= 0.0 ||
        offset_meters > VKR_GLTF_DECAL_NORMAL_OFFSET_MAX_METERS) {
      log_error("MeshLoader(glTF): invalid material entry in import sidecar "
                "'%.*s'",
                (int32_t)sidecar_path.length, sidecar_path.str);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }

    uint32_t matches = 0;
    for (uint32_t i = 0; i < (uint32_t)data->materials_count; ++i) {
      const cgltf_material *material = &data->materials[i];
      if (!material->name ||
          string_length(material->name) != material_name.length ||
          MemCompare(material->name, material_name.str, material_name.length) !=
              0) {
        continue;
      }
      if (offsets[i] > 0.0f) {
        log_error("MeshLoader(glTF): duplicate material '%.*s' in import "
                  "sidecar '%.*s'",
                  (int32_t)material_name.length, material_name.str,
                  (int32_t)sidecar_path.length, sidecar_path.str);
        vkr_mesh_loader_gltf_set_error(info,
                                       VKR_RENDERER_ERROR_INVALID_PARAMETER);
        return false_v;
      }
      offsets[i] = (float32_t)offset_meters;
      matches++;
    }
    if (matches == 0) {
      log_error("MeshLoader(glTF): import sidecar '%.*s' names unknown "
                "material '%.*s'",
                (int32_t)sidecar_path.length, sidecar_path.str,
                (int32_t)material_name.length, material_name.str);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
  }
  vkr_json_skip_whitespace(&reader);
  if (reader.pos >= reader.length || reader.data[reader.pos] != ']') {
    log_error("MeshLoader(glTF): malformed materials array in import sidecar "
              "'%.*s'",
              (int32_t)sidecar_path.length, sidecar_path.str);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (info->out_dependency_paths) {
    vkr_mesh_loader_gltf_push_unique_path(info->out_dependency_paths,
                                          sidecar_path, info->load_allocator);
  }
  out_overrides->offsets = offsets;
  out_overrides->count = (uint32_t)data->materials_count;
  return true_v;
}

/* The source mesh can be bound by multiple nodes to different skin palettes.
 * Keep joint indices palette-relative and prove every binding can address them.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_read_skin(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const cgltf_mesh *mesh, const cgltf_primitive *primitive,
    uint32_t vertex_count, VkrMeshSkinVertex **out_vertices) {
  *out_vertices = NULL;
  const cgltf_accessor *joints = NULL;
  const cgltf_accessor *weights = NULL;
  for (cgltf_size i = 0; i < primitive->attributes_count; ++i) {
    const cgltf_attribute *attribute = &primitive->attributes[i];
    if (attribute->type != cgltf_attribute_type_joints &&
        attribute->type != cgltf_attribute_type_weights) {
      continue;
    }
    if (attribute->index != 0) {
      log_error("MeshLoader(glTF): only JOINTS_0/WEIGHTS_0 are supported");
      return false_v;
    }
    const cgltf_accessor **target =
        attribute->type == cgltf_attribute_type_joints ? &joints : &weights;
    if (*target || !attribute->data) {
      return false_v;
    }
    *target = attribute->data;
  }
  uint32_t joint_limit = UINT32_MAX;
  bool8_t skinned = false_v;
  for (cgltf_size i = 0; i < data->nodes_count; ++i) {
    const cgltf_node *node = &data->nodes[i];
    if (node->mesh == mesh && node->skin) {
      if (!node->skin->joints_count || node->skin->joints_count > UINT32_MAX) {
        return false_v;
      }
      skinned = true_v;
      joint_limit = Min(joint_limit, (uint32_t)node->skin->joints_count);
    }
  }
  if (!joints && !weights) {
    if (skinned) {
      log_error("MeshLoader(glTF): skinned primitive has no joint influences");
    }
    return !skinned;
  }
  if (!data->skins_count) {
    log_error("MeshLoader(glTF): influences require a source skin definition");
    return false_v;
  }
  if (!joints || !weights || joints->type != cgltf_type_vec4 ||
      weights->type != cgltf_type_vec4 || joints->count != vertex_count ||
      weights->count != vertex_count || joints->normalized ||
      (joints->component_type != cgltf_component_type_r_8u &&
       joints->component_type != cgltf_component_type_r_16u) ||
      !((weights->component_type == cgltf_component_type_r_32f &&
         !weights->normalized) ||
        ((weights->component_type == cgltf_component_type_r_8u ||
          weights->component_type == cgltf_component_type_r_16u) &&
         weights->normalized))) {
    log_error("MeshLoader(glTF): invalid joint/weight accessor pair");
    return false_v;
  }
  const uint64_t element_count = (uint64_t)vertex_count * 4u;
  float32_t *joint_values = vkr_allocator_alloc(
      info->scratch_allocator, element_count * sizeof(float32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  float32_t *weight_values = vkr_allocator_alloc(
      info->scratch_allocator, element_count * sizeof(float32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshSkinVertex *vertices = vkr_allocator_alloc(
      info->scratch_allocator, (uint64_t)vertex_count * sizeof(*vertices),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!joint_values || !weight_values || !vertices) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  if (cgltf_accessor_unpack_floats(joints, joint_values, element_count) !=
          element_count ||
      cgltf_accessor_unpack_floats(weights, weight_values, element_count) !=
          element_count) {
    return false_v;
  }
  for (uint32_t i = 0; i < vertex_count; ++i) {
    float64_t sum = 0.0;
    for (uint32_t lane = 0; lane < 4u; ++lane) {
      const float32_t weight = weight_values[(uint64_t)i * 4u + lane];
      const float32_t joint = joint_values[(uint64_t)i * 4u + lane];
      if (!isfinite(weight) || weight < 0.0f || !isfinite(joint) ||
          joint < 0.0f || joint > 65535.0f || floorf(joint) != joint ||
          (weight > 0.0f && (uint32_t)joint >= joint_limit)) {
        log_error("MeshLoader(glTF): invalid vertex joint or weight");
        return false_v;
      }
      sum += weight;
    }
    if (!(sum > 0.0) || !isfinite(sum)) {
      log_error("MeshLoader(glTF): vertex has no positive skin weight");
      return false_v;
    }
    for (uint32_t lane = 0; lane < 4u; ++lane) {
      const uint64_t index = (uint64_t)i * 4u + lane;
      const float32_t weight = (float32_t)(weight_values[index] / sum);
      vertices[i].weights[lane] = weight > 0.0f ? weight : 0.0f;
      vertices[i].joints[lane] =
          weight > 0.0f ? (uint32_t)joint_values[index] : 0u;
    }
  }
  *out_vertices = vertices;
  return true_v;
}

/**
 * Resolves how far a decal primitive is pushed along its normals: the
 * material's `vkr_decal_normal_offset_meters` extra, else its sidecar override.
 * A positive offset requires NORMAL data.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_resolve_decal_offset(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const cgltf_primitive *primitive,
    const VkrMeshLoaderGltfDecalOverrides *decal_overrides,
    const cgltf_accessor *normal_accessor,
    float32_t *out_decal_normal_offset_meters) {
  float32_t sidecar_offset_meters = 0.0f;
  if (primitive->material && decal_overrides && decal_overrides->offsets) {
    cgltf_size material_index = cgltf_material_index(data, primitive->material);
    if (material_index < decal_overrides->count) {
      sidecar_offset_meters = decal_overrides->offsets[material_index];
    }
  }
  if (!vkr_mesh_loader_gltf_decal_normal_offset(
          info, primitive->material, sidecar_offset_meters,
          out_decal_normal_offset_meters)) {
    return false_v;
  }
  if (*out_decal_normal_offset_meters > 0.0f && !normal_accessor) {
    log_error("MeshLoader(glTF): decal material '%s' requires NORMAL data",
              primitive->material && primitive->material->name
                  ? primitive->material->name
                  : "<unnamed>");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  return true_v;
}

/**
 * Inverts the decal node transform that maps normal offsets back into the
 * primitive's local space.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_invert_decal_world(
    const VkrMeshLoaderGltfParseInfo *info, Mat4 decal_world,
    Mat4 *out_inverse_decal, Mat4 *out_decal_normal_matrix) {
  const float32_t determinant = mat4_determinant(decal_world);
  if (!isfinite(determinant) || fabsf(determinant) < 1e-6f) {
    log_error("MeshLoader(glTF): decal offset requires an invertible node "
              "transform");
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  const Mat4 inverse_decal = mat4_inverse(decal_world);
  for (uint32_t f = 0; f < 16u; ++f)
    if (!isfinite(inverse_decal.elements[f])) {
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
  *out_inverse_decal = inverse_decal;
  *out_decal_normal_matrix = mat4_transpose(inverse_decal);
  return true_v;
}

/**
 * Reads the primitive's indices, rejecting one outside its vertices, or
 * numbers the vertices in order when the primitive has no index accessor.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_read_indices(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_primitive *primitive,
    VkrAllocatorScope *primitive_scope, uint32_t vertex_count,
    uint32_t index_count, uint32_t *indices) {
  if (primitive->indices) {
    for (uint32_t i = 0; i < index_count; ++i) {
      cgltf_size index = cgltf_accessor_read_index(primitive->indices, i);
      if (index >= vertex_count) {
        vkr_allocator_end_scope(primitive_scope,
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
  return true_v;
}

/**
 * Gives every triangle corner its own vertex carrying the face normal and
 * renumbers the indices to match.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_split_flat_triangles(
    const VkrMeshLoaderGltfParseInfo *info, VkrAllocatorScope *primitive_scope,
    uint32_t *indices, uint32_t index_count, VkrVertex3d **in_out_vertices,
    VkrMeshSkinVertex **in_out_skin_vertices, uint32_t *in_out_vertex_count) {
  const VkrVertex3d *vertices = *in_out_vertices;
  const VkrMeshSkinVertex *skin_vertices = *in_out_skin_vertices;
  /* glTF requires flat normals when absent. Split corners before deriving
     the tangent basis so neighboring faces cannot average their normals. */
  VkrVertex3d *flat_vertices = vkr_allocator_alloc(
      info->scratch_allocator, (uint64_t)index_count * sizeof(VkrVertex3d),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMeshSkinVertex *flat_skin =
      skin_vertices
          ? vkr_allocator_alloc(info->scratch_allocator,
                                (uint64_t)index_count * sizeof(*flat_skin),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (!flat_vertices || (skin_vertices && !flat_skin)) {
    vkr_allocator_end_scope(primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  for (uint32_t i = 0; i < index_count; i += 3u) {
    const Vec3 p0 = vkr_vertex_unpack_vec3(vertices[indices[i]].position);
    const Vec3 p1 = vkr_vertex_unpack_vec3(vertices[indices[i + 1u]].position);
    const Vec3 p2 = vkr_vertex_unpack_vec3(vertices[indices[i + 2u]].position);
    const Vec3 n =
        vec3_normalize(vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0)));
    for (uint32_t c = 0; c < 3u; ++c) {
      flat_vertices[i + c] = vertices[indices[i + c]];
      if (flat_skin) {
        flat_skin[i + c] = skin_vertices[indices[i + c]];
      }
      flat_vertices[i + c].normal = vkr_vertex_pack_vec3(n);
      indices[i + c] = i + c;
    }
  }
  *in_out_vertices = flat_vertices;
  *in_out_skin_vertices = flat_skin;
  *in_out_vertex_count = index_count;
  return true_v;
}

/**
 * Makes each tangent orthogonal to its normal and its handedness a unit sign,
 * rejecting a degenerate basis.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_orthonormalize_tangents(
    const VkrMeshLoaderGltfParseInfo *info, VkrAllocatorScope *primitive_scope,
    VkrVertex3d *vertices, uint32_t vertex_count) {
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const Vec3 n = vkr_vertex_unpack_vec3(vertices[i].normal);
    Vec3 t = vec3_new(vertices[i].tangent.x, vertices[i].tangent.y,
                      vertices[i].tangent.z);
    t = vec3_sub(t, vec3_scale(n, vec3_dot(n, t)));
    if (!isfinite(n.x) || !isfinite(n.y) || !isfinite(n.z) ||
        vec3_length(n) < 1e-6f || !isfinite(t.x) || !isfinite(t.y) ||
        !isfinite(t.z) || vec3_length(t) < 1e-6f ||
        !isfinite(vertices[i].tangent.w) ||
        fabsf(vertices[i].tangent.w) < 0.5f) {
      log_error("MeshLoader(glTF): anisotropy has a degenerate tangent basis");
      vkr_allocator_end_scope(primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    t = vec3_normalize(t);
    vertices[i].tangent =
        vec4_new(t.x, t.y, t.z, vertices[i].tangent.w < 0.0f ? -1.0f : 1.0f);
  }
  return true_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_emit_primitive(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const cgltf_mesh *mesh, const cgltf_primitive *primitive, Mat4 world,
    Mat4 normal_matrix, Mat4 decal_world, const String8 *material_paths,
    const VkrMeshLoaderGltfDecalOverrides *decal_overrides,
    uint32_t *in_out_primitive_count) {
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

  const bool8_t anisotropic =
      primitive->material && primitive->material->has_anisotropy &&
      primitive->material->anisotropy.anisotropy_strength > 0.0f;
  float32_t decal_normal_offset_meters = 0.0f;
  if (!vkr_mesh_loader_gltf_resolve_decal_offset(
          info, data, primitive, decal_overrides, normal_accessor,
          &decal_normal_offset_meters)) {
    return false_v;
  }

  Mat4 inverse_decal = mat4_identity();
  Mat4 decal_normal_matrix = mat4_identity();
  if (decal_normal_offset_meters != 0.0f &&
      !vkr_mesh_loader_gltf_invert_decal_world(
          info, decal_world, &inverse_decal, &decal_normal_matrix)) {
    return false_v;
  }

  const cgltf_size pos_count = position_accessor->count;
  const cgltf_size idx_count =
      primitive->indices ? primitive->indices->count : pos_count;
  if (pos_count == 0 || idx_count == 0) {
    return true_v;
  }
  if (pos_count > (cgltf_size)UINT32_MAX ||
      (primitive->indices &&
       primitive->indices->count > (cgltf_size)UINT32_MAX)) {
    vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }
  uint32_t vertex_count = (uint32_t)pos_count;
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

  VkrMeshSkinVertex *skin_vertices = NULL;
  if (!vkr_mesh_loader_gltf_read_skin(info, data, mesh, primitive, vertex_count,
                                      &skin_vertices)) {
    vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!info->out_error || *info->out_error == VKR_RENDERER_ERROR_NONE) {
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
    }
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
    Vec3 world_normal = vec3_zero();
    const bool8_t normal_valid = vkr_mesh_loader_gltf_transform_unit_direction(
        normal_matrix, normal, &world_normal);
    if (!normal_valid && (decal_normal_offset_meters > 0.0f || anisotropic)) {
      vkr_allocator_end_scope(&primitive_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      log_error("MeshLoader(glTF): decal material '%s' has a degenerate NORMAL",
                primitive->material && primitive->material->name
                    ? primitive->material->name
                    : "<unnamed>");
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    if (!normal_valid)
      world_normal = vec3_new(0.0f, 1.0f, 0.0f);
    if (decal_normal_offset_meters != 0.0f) {
      Vec3 offset_normal;
      if (!vkr_mesh_loader_gltf_transform_unit_direction(
              decal_normal_matrix, normal, &offset_normal)) {
        vkr_allocator_end_scope(&primitive_scope,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        return false_v;
      }
      Vec4 local_offset = mat4_mul_vec4(
          inverse_decal,
          vec4_new(offset_normal.x, offset_normal.y, offset_normal.z, 0.0f));
      world_position = vec3_add(
          world_position,
          vec3_scale(vec3_new(local_offset.x, local_offset.y, local_offset.z),
                     decal_normal_offset_meters));
    }
    Vec3 world_tangent = vec3_new(1.0f, 0.0f, 0.0f);
    if (anisotropic && tangent_accessor) {
      if (!vkr_mesh_loader_gltf_transform_unit_direction(
              world, vec3_new(tangent.x, tangent.y, tangent.z),
              &world_tangent)) {
        log_error(
            "MeshLoader(glTF): anisotropy has an invalid authored tangent");
        vkr_allocator_end_scope(&primitive_scope,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        vkr_mesh_loader_gltf_set_error(info,
                                       VKR_RENDERER_ERROR_INVALID_PARAMETER);
        return false_v;
      }
    } else {
      world_tangent = vkr_mesh_loader_gltf_transform_direction(
          world, vec3_new(tangent.x, tangent.y, tangent.z), world_tangent);
    }

    if (texcoord_accessor) {
      // glTF uses an upper-left texture origin; VKR's 2D image paths store
      // vertically flipped rows and therefore use a bottom-left origin.
      texcoord.y = 1.0f - texcoord.y;
    }

    vertices[i].position = vkr_vertex_pack_vec3(world_position);
    vertices[i].normal = vkr_vertex_pack_vec3(world_normal);
    vertices[i].texcoord = texcoord;
    vertices[i].colour = color;
    vertices[i].tangent =
        vec4_new(world_tangent.x, world_tangent.y, world_tangent.z,
                 anisotropic ? -tangent.w : tangent.w);
  }

  if (!vkr_mesh_loader_gltf_read_indices(info, primitive, &primitive_scope,
                                         vertex_count, index_count, indices)) {
    return false_v;
  }

  String8 material_path = {0};
  if (primitive->material && material_paths) {
    cgltf_size material_index = cgltf_material_index(data, primitive->material);
    if (material_index < data->materials_count) {
      material_path = material_paths[material_index];
    }
  }

  if (anisotropic) {
    if (!normal_accessor && !vkr_mesh_loader_gltf_split_flat_triangles(
                                info, &primitive_scope, indices, index_count,
                                &vertices, &skin_vertices, &vertex_count)) {
      return false_v;
    }
    if (!tangent_accessor)
      vkr_geometry_generate_tangents(info->scratch_allocator, vertices,
                                     vertex_count, indices, index_count);
    if (!vkr_mesh_loader_gltf_orthonormalize_tangents(info, &primitive_scope,
                                                      vertices, vertex_count)) {
      return false_v;
    }
  }

  VkrMeshLoaderGltfPrimitive out_primitive = {
      .vertices = vertices,
      .skin_vertices = skin_vertices,
      .vertex_count = vertex_count,
      .indices = indices,
      .index_count = index_count,
      .material_path = material_path,
      .preserve_tangents = anisotropic,
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

vkr_internal uint64_t vkr_mesh_source_hash(uint64_t hash, const void *bytes,
                                           uint64_t size) {
  const uint8_t *data = bytes;
  for (uint64_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  return hash;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_emit_scene(
    const VkrMeshLoaderGltfParseInfo *info, const cgltf_data *data,
    const String8 *material_paths,
    const VkrMeshLoaderGltfDecalOverrides *decal_overrides,
    uint32_t *primitive_count) {
  if (!info->out_source || !data->nodes_count ||
      data->nodes_count > UINT32_MAX ||
      data->meshes_count > UINT32_MAX - data->nodes_count ||
      data->animations_count > UINT32_MAX)
    return false_v;
  VkrMeshSource *source = info->out_source;
  // Each node may add one mesh variant; indices are stored as uint32_t.
  const cgltf_size mesh_capacity = data->meshes_count + data->nodes_count;
  if (mesh_capacity < data->meshes_count || mesh_capacity > UINT32_MAX)
    return false_v;
  *source = (VkrMeshSource){
      .nodes = array_create_filled_VkrMeshSourceNode(info->load_allocator,
                                                     data->nodes_count),
      .meshes = array_create_filled_VkrMeshSourceMesh(info->load_allocator,
                                                      mesh_capacity),
      .animation_count = (uint32_t)data->animations_count,
      .fingerprint = vkr_cgltf_source_fingerprint(data),
  };
  if ((data->nodes_count && !source->nodes.data) ||
      (mesh_capacity && !source->meshes.data))
    return false_v;
  source->meshes.length = data->meshes_count;
  MemZero(source->meshes.data, mesh_capacity * sizeof(VkrMeshSourceMesh));
  Mat4 *variant_worlds =
      mesh_capacity ? vkr_allocator_alloc(info->load_allocator,
                                          mesh_capacity * sizeof(Mat4),
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
                    : NULL;
  bool8_t *has_decal =
      data->meshes_count
          ? vkr_allocator_alloc(info->load_allocator, data->meshes_count,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (data->meshes_count && (!variant_worlds || !has_decal))
    return false_v;
  if (data->meshes_count)
    MemZero(has_decal, data->meshes_count);
  for (uint32_t m = 0; m < data->meshes_count; ++m) {
    source->meshes.data[m].source_mesh_index = m;
    variant_worlds[m] = mat4_identity();
    for (cgltf_size p = 0; p < data->meshes[m].primitives_count; ++p) {
      const cgltf_material *material = data->meshes[m].primitives[p].material;
      const uint32_t index =
          material ? (uint32_t)(material - data->materials) : UINT32_MAX;
      const float32_t override = index < decal_overrides->count
                                     ? decal_overrides->offsets[index]
                                     : 0.0f;
      float32_t offset = 0.0f;
      if (!vkr_mesh_loader_gltf_decal_normal_offset(info, material, override,
                                                    &offset))
        return false_v;
      has_decal[m] |= offset != 0.0f;
    }
  }
  if (data->skins_count > 65536u) {
    return false_v;
  }
  for (cgltf_size i = 0; i < data->skins_count; ++i) {
    if (!data->skins[i].joints_count ||
        data->skins[i].joints_count > data->nodes_count) {
      return false_v;
    }
  }
  if (info->out_skin) {
    *info->out_skin = (VkrMeshSkinData){
        .animation_fingerprint = data->skins_count ? source->fingerprint : 0u,
        .skin_count = (uint32_t)data->skins_count,
    };
    if (data->skins_count) {
      info->out_skin->joint_counts = vkr_allocator_alloc(
          info->load_allocator, data->skins_count * sizeof(uint32_t),
          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      if (!info->out_skin->joint_counts) {
        vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
        return false_v;
      }
    }
  }
  for (cgltf_size i = 0; i < data->skins_count; ++i) {
    if (info->out_skin) {
      info->out_skin->joint_counts[i] = (uint32_t)data->skins[i].joints_count;
    }
  }
  source->fingerprint =
      vkr_mesh_source_hash(source->fingerprint, decal_overrides->offsets,
                           decal_overrides->count * sizeof(float32_t));
  uint32_t *root_indices = vkr_allocator_alloc(
      info->load_allocator, data->nodes_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (data->nodes_count && !root_indices)
    return false_v;
  for (uint32_t i = 0; i < data->nodes_count; ++i)
    root_indices[i] = UINT32_MAX;
  const cgltf_scene *scene =
      data->scene ? data->scene
                  : (data->scenes_count ? &data->scenes[0] : NULL);
  for (uint32_t i = 0; i < data->nodes_count; ++i) {
    const cgltf_node *node = &data->nodes[i];
    const cgltf_node *root = node;
    uint32_t depth = 0;
    while (root->parent && root_indices[root - data->nodes] == UINT32_MAX &&
           depth++ < data->nodes_count)
      root = root->parent;
    if (depth >= data->nodes_count && root->parent)
      return false_v;
    const uint32_t root_index = root_indices[root - data->nodes] == UINT32_MAX
                                    ? (uint32_t)(root - data->nodes)
                                    : root_indices[root - data->nodes];
    const cgltf_node *path = node;
    while (path && root_indices[path - data->nodes] == UINT32_MAX) {
      root_indices[path - data->nodes] = root_index;
      path = path->parent;
    }
    root = &data->nodes[root_index];
    bool8_t in_scene = !scene;
    for (cgltf_size r = 0; scene && r < scene->nodes_count; ++r)
      in_scene |= scene->nodes[r] == root;
    cgltf_float local[16];
    cgltf_node_transform_local(node, local);
    VkrMeshSourceNode *out = &source->nodes.data[i];
    *out = (VkrMeshSourceNode){
        .name = node->name ? string8_create_formatted(info->load_allocator,
                                                      "%s", node->name)
                           : (String8){0},
        .local = vkr_mesh_loader_gltf_mat4_from_cgltf(local),
        .parent =
            node->parent ? (uint32_t)(node->parent - data->nodes) : UINT32_MAX,
        .mesh = node->mesh ? (uint32_t)(node->mesh - data->meshes) : UINT32_MAX,
        .mesh_variant =
            node->mesh ? (uint32_t)(node->mesh - data->meshes) : UINT32_MAX,
        .camera = node->camera ? (uint32_t)(node->camera - data->cameras)
                               : UINT32_MAX,
        .skin = node->skin ? (uint32_t)(node->skin - data->skins) : UINT32_MAX,
        .light =
            node->light ? (uint32_t)(node->light - data->lights) : UINT32_MAX,
        .in_scene = in_scene,
    };
    if (node->light) {
      const cgltf_light *light = node->light;
      out->punctual = (VkrMeshSourceLight){
          .color = vec3_new(light->color[0], light->color[1], light->color[2]),
          .intensity = light->intensity,
          .range = light->range,
          .inner_cone = light->spot_inner_cone_angle,
          .outer_cone = light->spot_outer_cone_angle,
          .kind = light->type == cgltf_light_type_directional ? 1u
                  : light->type == cgltf_light_type_point     ? 2u
                  : light->type == cgltf_light_type_spot      ? 3u
                                                              : 0u,
      };
    }
    if (node->name && !out->name.str)
      return false_v;
    for (uint32_t f = 0; f < 16u; ++f)
      if (!isfinite(out->local.elements[f]))
        return false_v;
    if (!isfinite(out->punctual.color.x) || !isfinite(out->punctual.color.y) ||
        !isfinite(out->punctual.color.z) ||
        !isfinite(out->punctual.intensity) || !isfinite(out->punctual.range) ||
        !isfinite(out->punctual.inner_cone) ||
        !isfinite(out->punctual.outer_cone))
      return false_v;
  }
  Mat4 *node_worlds = vkr_allocator_alloc(info->scratch_allocator,
                                          data->nodes_count * sizeof(Mat4),
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint8_t *world_ready =
      vkr_allocator_alloc(info->scratch_allocator, data->nodes_count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint32_t *node_stack = vkr_allocator_alloc(
      info->scratch_allocator, data->nodes_count * sizeof(uint32_t),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (data->nodes_count && (!node_worlds || !world_ready || !node_stack))
    return false_v;
  MemZero(world_ready, data->nodes_count);
  for (uint32_t n = 0; n < data->nodes_count; ++n) {
    uint32_t cursor = n, count = 0u;
    while (cursor != UINT32_MAX && !world_ready[cursor]) {
      node_stack[count++] = cursor;
      cursor = source->nodes.data[cursor].parent;
    }
    while (count) {
      const uint32_t index = node_stack[--count];
      const VkrMeshSourceNode *node = &source->nodes.data[index];
      node_worlds[index] =
          node->parent == UINT32_MAX
              ? node->local
              : mat4_mul(node_worlds[node->parent], node->local);
      world_ready[index] = 1u;
    }
  }
  for (uint32_t n = 0; n < data->nodes_count; ++n) {
    VkrMeshSourceNode *node = &source->nodes.data[n];
    if (!node->in_scene || node->mesh == UINT32_MAX || !has_decal[node->mesh])
      continue;
    Mat4 world = node_worlds[n];
    world.elements[12] = world.elements[13] = world.elements[14] = 0.0f;
    if (has_decal[node->mesh] == 1u) {
      variant_worlds[node->mesh] = world;
      has_decal[node->mesh] = 2u;
    }
    uint32_t variant = UINT32_MAX;
    for (uint32_t m = 0; m < source->meshes.length; ++m) {
      if (source->meshes.data[m].source_mesh_index == node->mesh &&
          MemCompare(&variant_worlds[m], &world, sizeof(Mat4)) == 0) {
        variant = m;
        break;
      }
    }
    if (variant == UINT32_MAX) {
      variant = (uint32_t)source->meshes.length++;
      source->meshes.data[variant].source_mesh_index = node->mesh;
      variant_worlds[variant] = world;
    }
    node->mesh_variant = variant;
  }
  for (uint32_t m = 0; m < source->meshes.length; ++m) {
    VkrMeshSourceMesh *mesh = &source->meshes.data[m];
    mesh->first_range = *primitive_count;
    bool8_t referenced = false_v;
    for (uint32_t n = 0; n < source->nodes.length; ++n)
      referenced |= source->nodes.data[n].in_scene &&
                    source->nodes.data[n].mesh_variant == m;
    if (!referenced)
      continue;
    const cgltf_mesh *input = &data->meshes[mesh->source_mesh_index];
    for (cgltf_size p = 0; p < input->primitives_count; ++p) {
      if (!vkr_mesh_loader_gltf_emit_primitive(
              info, data, input, &input->primitives[p], mat4_identity(),
              mat4_identity(), variant_worlds[m], material_paths,
              decal_overrides, primitive_count))
        return false_v;
    }
    mesh->range_count = *primitive_count - mesh->first_range;
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

  /* Only images a texture samples are read: an image named solely by an
     extension the loader ignores (MSFT_texture_dds alternates, 2 GiB in
     Bistro) is no dependency, and a deferred cook reads no image. */
  if (info->defer_textures) {
    return;
  }
  for (uint32_t i = 0; i < (uint32_t)data->images_count; ++i) {
    const cgltf_image *image = &data->images[i];
    bool8_t sampled = false_v;
    for (cgltf_size t = 0; t < data->textures_count && !sampled; ++t) {
      sampled = data->textures[t].image == image;
    }
    if (!image->uri || !sampled) {
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

vkr_internal bool8_t vkr_mesh_loader_gltf_required_extension(
    const cgltf_data *data, const char *name) {
  for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
    if (data->extensions_required[i] &&
        string_equals(data->extensions_required[i], name)) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_mesh_loader_gltf_decode_meshopt(
    const VkrMeshLoaderGltfParseInfo *info, cgltf_data *data) {
  if (vkr_mesh_loader_gltf_required_extension(data,
                                              "KHR_meshopt_compression")) {
    log_error("MeshLoader(glTF): required KHR_meshopt_compression is "
              "unsupported");
    return false_v;
  }
  for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
    cgltf_buffer_view *view = &data->buffer_views[i];
    if (!view->has_meshopt_compression || view->meshopt_compression.is_khr) {
      continue;
    }
    const cgltf_meshopt_compression *compression = &view->meshopt_compression;
    if (!compression->buffer || !compression->buffer->data ||
        compression->count == 0 || compression->stride == 0 ||
        compression->count > SIZE_MAX / compression->stride) {
      return false_v;
    }
    const size_t decoded_size =
        (size_t)compression->count * (size_t)compression->stride;
    if (decoded_size != view->size ||
        compression->offset > compression->buffer->size ||
        compression->size > compression->buffer->size - compression->offset) {
      return false_v;
    }
    uint8_t *decoded = vkr_allocator_alloc(info->load_allocator, decoded_size,
                                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!decoded) {
      vkr_mesh_loader_gltf_set_error(info, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return false_v;
    }
    VkrMeshoptGltfMode mode = (VkrMeshoptGltfMode)compression->mode;
    VkrMeshoptGltfFilter filter = (VkrMeshoptGltfFilter)compression->filter;
    const uint8_t *encoded =
        (const uint8_t *)compression->buffer->data + compression->offset;
    if (vkr_meshopt_decode_gltf_buffer(decoded, compression->count,
                                       compression->stride, encoded,
                                       compression->size, mode, filter) != 0) {
      log_error("MeshLoader(glTF): EXT_meshopt_compression decode failed for "
                "buffer view %llu",
                (unsigned long long)i);
      return false_v;
    }
    view->data = decoded;
  }
  return true_v;
}

/* cgltf owns replaced URIs through its allocator; image bytes are job scratch.
 */
vkr_internal bool8_t vkr_mesh_loader_gltf_extract_images(
    const VkrMeshLoaderGltfParseInfo *info, cgltf_data *data) {
  const uint64_t max_encoded = MB(64);
  for (cgltf_size i = 0; i < data->images_count; ++i) {
    cgltf_image *image = &data->images[i];
    const uint8_t *bytes = NULL;
    uint64_t size = 0;
    void *decoded = NULL;
    if (image->buffer_view) {
      bytes = cgltf_buffer_view_data(image->buffer_view);
      size = image->buffer_view->size;
    } else if (image->uri && strncmp(image->uri, "data:", 5) == 0) {
      const char *comma = strchr(image->uri, ',');
      if (!comma || comma - image->uri < 7 ||
          strncmp(comma - 7, ";base64", 7) != 0) {
        return false_v;
      }
      const char *base64 = comma + 1;
      uint64_t length = strlen(base64);
      if (!length || length % 4 || length > (max_encoded / 3 + 1) * 4) {
        return false_v;
      }
      size = length / 4 * 3;
      if (base64[length - 1] == '=') {
        --size;
      }
      if (base64[length - 2] == '=') {
        --size;
      }
      cgltf_options options = {.memory = data->memory,
                               .file = vkr_cgltf_file_options()};
      if (cgltf_load_buffer_base64(&options, size, base64, &decoded) !=
          cgltf_result_success) {
        return false_v;
      }
      bytes = decoded;
    } else {
      /* External image URIs become filenames once at this import boundary.
       * Generated image paths below are already host filenames. */
      if (image->uri) {
        cgltf_size length = cgltf_decode_uri(image->uri);
        if (memchr(image->uri, 0, length)) {
          return false_v;
        }
      }
      continue;
    }
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    bool8_t valid = bytes && size && size <= max_encoded &&
                    stbi_info_from_memory(bytes, (int32_t)size, &width, &height,
                                          &channels) &&
                    width > 0 && height > 0 &&
                    (uint64_t)width * height <= UINT64_C(67108864);
    bool8_t png =
        valid && size >= 8 && MemCompare(bytes, "\x89PNG\r\n\x1a\n", 8) == 0;
    bool8_t jpeg = valid && size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
                   bytes[2] == 0xff;
    String8 path = {0};
    if (png || jpeg) {
      uint64_t hash = vkr_mesh_loader_gltf_hash_bytes(VKR_FNV1A64_OFFSET_BASIS,
                                                      bytes, size);
      path = string8_create_formatted(
          info->load_allocator, "assets/textures/generated/embedded/%016llx.%s",
          (unsigned long long)hash, png ? "png" : "jpg");
      path = vkr_mesh_loader_gltf_output_path(info, path);
      FilePath absolute =
          file_path_create((const char *)path.str, info->load_allocator,
                           vkr_mesh_loader_gltf_path_is_absolute(path)
                               ? FILE_PATH_TYPE_ABSOLUTE
                               : FILE_PATH_TYPE_RELATIVE);
      path = absolute.path;
      valid = vkr_mesh_cooked_write_atomic(info->scratch_allocator, path, bytes,
                                           size);
    } else {
      valid = false_v;
    }
    if (decoded) {
      data->memory.free_func(data->memory.user_data, decoded);
    }
    if (!valid) {
      log_error("MeshLoader(glTF): invalid, oversized or unsupported embedded "
                "image %llu",
                (unsigned long long)i);
      return false_v;
    }
    char *uri =
        data->memory.alloc_func(data->memory.user_data, path.length + 1);
    if (!uri) {
      return false_v;
    }
    MemCopy(uri, path.str, path.length);
    uri[path.length] = 0;
    data->memory.free_func(data->memory.user_data, image->uri);
    image->uri = uri;
    image->buffer_view = NULL;
  }
  return true_v;
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

  cgltf_options options = {.file = vkr_cgltf_file_options()};
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
    if (!vkr_mesh_loader_gltf_decode_meshopt(info, data)) {
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }

    if (!vkr_mesh_loader_gltf_extract_images(info, data)) {
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }
    vkr_mesh_loader_gltf_collect_dependencies(info, data);

    VkrMeshLoaderGltfDecalOverrides decal_overrides = {0};
    if (!vkr_mesh_loader_gltf_read_decal_overrides(info, data,
                                                   &decal_overrides)) {
      ok = false_v;
      break;
    }

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
    ok = vkr_mesh_loader_gltf_emit_scene(info, data, material_paths,
                                         &decal_overrides, &primitive_count);

    if (!ok) {
      if (!info->out_error || *info->out_error == VKR_RENDERER_ERROR_NONE)
        vkr_mesh_loader_gltf_set_error(info,
                                       VKR_RENDERER_ERROR_INVALID_PARAMETER);
      break;
    }

    if (primitive_count == 0 && !info->out_source->nodes.length) {
      log_error("MeshLoader(glTF): no renderable triangle primitives in '%s'",
                string8_cstr(&cstr_path));
      vkr_mesh_loader_gltf_set_error(info,
                                     VKR_RENDERER_ERROR_INVALID_PARAMETER);
      ok = false_v;
      break;
    }
  } while (false);

  if (data) {
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
      cgltf_buffer_view *view = &data->buffer_views[i];
      if (view->has_meshopt_compression && !view->meshopt_compression.is_khr) {
        view->data = NULL;
      }
    }
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
