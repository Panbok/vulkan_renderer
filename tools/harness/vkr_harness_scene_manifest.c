#include "vkr_harness.h"

#define VKR_HARNESS_FNV1A64_OFFSET_BASIS 0xcbf29ce484222325ull
#define VKR_HARNESS_FNV1A64_PRIME 0x100000001b3ull

static bool8_t vkr_harness_scene_path_below(const char *root,
                                            const char *path) {
  const uint64_t root_length = string_length(root);
  return file_path_starts_with(path, root) &&
         (path[root_length] == '\0' || path[root_length] == '/' ||
          path[root_length] == '\\');
}

static bool8_t vkr_harness_scene_asset_extension(const char *path) {
  static const char *extensions[] = {
      ".json",    ".gltf", ".glb", ".bin", ".obj", ".mtl",  ".mt",   ".png",
      ".jpg",     ".jpeg", ".bmp", ".tga", ".hdr", ".ktx",  ".ktx2", ".vkt",
      ".fontcfg", ".ttf",  ".ttc", ".fnt", ".vkf", ".vkfa",
  };
  uint64_t length = 0u;
  while (path[length] && path[length] != '?' && path[length] != '#') {
    length++;
  }
  for (uint32_t i = 0; i < ArrayCount(extensions); ++i) {
    const uint64_t suffix_length = string_length(extensions[i]);
    if (length < suffix_length) {
      continue;
    }
    bool8_t matches = true_v;
    for (uint64_t c = 0; c < suffix_length; ++c) {
      char a = path[length - suffix_length + c];
      char b = extensions[i][c];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (a != b) {
        matches = false_v;
        break;
      }
    }
    if (matches) {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t vkr_harness_scene_path_ends_with(const char *path,
                                                const char *suffix) {
  const uint64_t length = string_length(path);
  const uint64_t suffix_length = string_length(suffix);
  if (length < suffix_length) {
    return false_v;
  }
  for (uint64_t i = 0; i < suffix_length; ++i) {
    char a = path[length - suffix_length + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (a != b) {
      return false_v;
    }
  }
  return true_v;
}

static int32_t vkr_harness_scene_asset_compare(const void *a, const void *b) {
  const VkrHarnessSceneAsset *lhs = a;
  const VkrHarnessSceneAsset *rhs = b;
  return string_compare(lhs->path, rhs->path);
}

static bool8_t vkr_harness_scene_generated_material_source(
    const char *owner, char out_source[VKR_HARNESS_PATH_MAX]) {
  static const char forward_prefix[] = "assets/materials/";
  static const char native_prefix[] = "assets\\materials\\";
  const uint32_t prefix_length = (uint32_t)sizeof(forward_prefix) - 1u;
  if (!string_n_equals(owner, forward_prefix, prefix_length) &&
      !string_n_equals(owner, native_prefix, prefix_length)) {
    return false_v;
  }

  const char *stem = owner + prefix_length;
  const char *separator = stem;
  while (*separator && *separator != '/' && *separator != '\\') {
    separator++;
  }
  static const char generated_prefix[] = "gltf_mat_";
  if (separator == stem || !*separator ||
      !string_n_equals(separator + 1u, generated_prefix,
                       sizeof(generated_prefix) - 1u)) {
    return false_v;
  }

  return string_format(out_source, VKR_HARNESS_PATH_MAX,
                       "assets/models/%.*s.gltf", (int32_t)(separator - stem),
                       stem) > 0;
}

static bool8_t vkr_harness_scene_manifest_resolve(
    const char *resolved_root, const char *owner_relative,
    const char *reference, char out_relative[VKR_HARNESS_PATH_MAX],
    char out_absolute[VKR_HARNESS_PATH_MAX]) {
  if (!reference || reference[0] == '\0' || string_find(reference, "://") ||
      string_n_equals(reference, "data:", 5u)) {
    return false_v;
  }

  char clean[VKR_HARNESS_PATH_MAX];
  uint32_t clean_length = 0u;
  for (uint32_t i = 0;
       reference[i] && reference[i] != '?' && reference[i] != '#'; ++i) {
    if (clean_length + 1u >= sizeof(clean)) {
      return false_v;
    }
    if (reference[i] == '%' && reference[i + 1u] && reference[i + 2u]) {
      const char hi = reference[i + 1u];
      const char lo = reference[i + 2u];
      const int32_t hi_value = hi >= '0' && hi <= '9'   ? hi - '0'
                               : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10
                               : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10
                                                        : -1;
      const int32_t lo_value = lo >= '0' && lo <= '9'   ? lo - '0'
                               : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10
                               : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10
                                                        : -1;
      if (hi_value >= 0 && lo_value >= 0) {
        clean[clean_length++] = (char)((hi_value << 4) | lo_value);
        i += 2u;
        continue;
      }
    }
    clean[clean_length++] = reference[i] == '\\' ? '/' : reference[i];
  }
  clean[clean_length] = '\0';
  if (!vkr_harness_scene_asset_extension(clean)) {
    return false_v;
  }

  char candidate[VKR_HARNESS_PATH_MAX];
  if (string_n_equals(clean, "assets/", 7u) ||
      string_n_equals(clean, "tests/", 6u)) {
    if (string_format(candidate, sizeof(candidate), "%s/%s", resolved_root,
                      clean) <= 0) {
      return false_v;
    }
  } else {
    char owner_absolute[VKR_HARNESS_PATH_MAX];
    char owner_directory[VKR_HARNESS_PATH_MAX];
    if (string_format(owner_absolute, sizeof(owner_absolute), "%s/%s",
                      resolved_root, owner_relative) <= 0 ||
        !vkr_harness_path_parent(owner_absolute, owner_directory) ||
        string_format(candidate, sizeof(candidate), "%s/%s", owner_directory,
                      clean) <= 0) {
      return false_v;
    }
  }

  if (!vkr_harness_realpath(candidate, out_absolute) ||
      !vkr_harness_scene_path_below(resolved_root, out_absolute)) {
    const uint64_t owner_length = string_length(owner_relative);
    const bool8_t owner_is_mtl =
        owner_length >= 4u &&
        string_equals(owner_relative + owner_length - 4u, ".mtl");
    const bool8_t owner_is_gltf =
        owner_length >= 5u &&
        string_equals(owner_relative + owner_length - 5u, ".gltf");
    const bool8_t owner_is_glb =
        owner_length >= 4u &&
        string_equals(owner_relative + owner_length - 4u, ".glb");
    const bool8_t owner_is_fnt =
        owner_length >= 4u &&
        string_equals(owner_relative + owner_length - 4u, ".fnt");
    if (!owner_is_mtl && !owner_is_gltf && !owner_is_glb && !owner_is_fnt) {
      return false_v;
    }
    if (owner_is_gltf || owner_is_glb) {
      static const char *roots[] = {"assets", "assets/textures"};
      for (uint32_t root_index = 0; root_index < ArrayCount(roots);
           ++root_index) {
        if (string_format(candidate, sizeof(candidate), "%s/%s/%s",
                          resolved_root, roots[root_index], clean) <= 0 ||
            !vkr_harness_realpath(candidate, out_absolute) ||
            !vkr_harness_scene_path_below(resolved_root, out_absolute)) {
          continue;
        }
        const uint64_t root_length = string_length(resolved_root);
        const char *relative = out_absolute + root_length;
        if (*relative == '/' || *relative == '\\') {
          relative++;
        }
        return string_format(out_relative, VKR_HARNESS_PATH_MAX, "%s",
                             relative) > 0;
      }
    }
    if ((owner_is_gltf || owner_is_glb) &&
        string_n_equals(clean, "objects/", 8u)) {
      if (string_format(candidate, sizeof(candidate), "%s/assets/textures/%s",
                        resolved_root, clean + 8u) > 0 &&
          vkr_harness_realpath(candidate, out_absolute) &&
          vkr_harness_scene_path_below(resolved_root, out_absolute)) {
        const uint64_t root_length = string_length(resolved_root);
        const char *relative = out_absolute + root_length;
        if (*relative == '/' || *relative == '\\') {
          relative++;
        }
        return string_format(out_relative, VKR_HARNESS_PATH_MAX, "%s",
                             relative) > 0;
      }
    }
    const char *basename = clean;
    for (const char *cursor = clean; *cursor; ++cursor) {
      if (*cursor == '/' || *cursor == '\\') {
        basename = cursor + 1;
      }
    }
    if (string_format(candidate, sizeof(candidate), "%s/assets/textures/%s",
                      resolved_root, basename) <= 0 ||
        !vkr_harness_realpath(candidate, out_absolute) ||
        !vkr_harness_scene_path_below(resolved_root, out_absolute)) {
      return false_v;
    }
  }
  const uint64_t root_length = string_length(resolved_root);
  const char *relative = out_absolute + root_length;
  if (*relative == '/' || *relative == '\\') {
    relative++;
  }
  return string_format(out_relative, VKR_HARNESS_PATH_MAX, "%s", relative) > 0;
}

static bool8_t
vkr_harness_scene_manifest_enqueue(VkrHarnessSceneManifest *manifest,
                                   const char *relative,
                                   VkrHarnessError *out_error) {
  char canonical[VKR_HARNESS_PATH_MAX];
  uint32_t length = 0u;
  while (relative[length]) {
    if (length + 1u >= sizeof(canonical)) {
      vkr_harness_error_set(out_error, "scene_manifest.path_limit", "$.scene",
                            "Scene dependency path exceeds %u bytes",
                            VKR_HARNESS_PATH_MAX - 1u);
      return false_v;
    }
    canonical[length] = relative[length] == '\\' ? '/' : relative[length];
    length++;
  }
  canonical[length] = '\0';

  for (uint32_t i = 0; i < manifest->asset_count; ++i) {
    if (string_equals(manifest->assets[i].path, canonical)) {
      return true_v;
    }
  }
  if (manifest->asset_count >= VKR_HARNESS_MAX_SCENE_ASSETS) {
    vkr_harness_error_set(out_error, "scene_manifest.asset_limit", "$.scene",
                          "Scene dependency count exceeds %u",
                          VKR_HARNESS_MAX_SCENE_ASSETS);
    return false_v;
  }
  string_format(manifest->assets[manifest->asset_count++].path,
                VKR_HARNESS_PATH_MAX, "%s", canonical);
  return true_v;
}

static bool8_t vkr_harness_scene_manifest_add_reference(
    const char *resolved_root, const char *owner,
    VkrHarnessSceneManifest *manifest, const char *reference, bool8_t required,
    VkrHarnessError *out_error) {
  char relative[VKR_HARNESS_PATH_MAX];
  char absolute[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_scene_manifest_resolve(resolved_root, owner, reference,
                                          relative, absolute)) {
    if (required) {
      char generated_source[VKR_HARNESS_PATH_MAX];
      if (vkr_harness_scene_generated_material_source(owner,
                                                      generated_source)) {
#if defined(PLATFORM_WINDOWS)
        vkr_harness_error_set(
            out_error, "scene_manifest.missing", "$.scene",
            "Generated dependency '%s' is missing (query ignored). Rebuild "
            "with "
            "'tools\\cook_vkr_meshes.bat %s'",
            reference, generated_source);
#else
        vkr_harness_error_set(
            out_error, "scene_manifest.missing", "$.scene",
            "Generated dependency '%s' is missing (query ignored). Rebuild "
            "with "
            "'./tools/cook_vkr_meshes.sh %s'",
            reference, generated_source);
#endif
      } else {
        vkr_harness_error_set(
            out_error, "scene_manifest.missing", "$.scene",
            "Dependency '%s' referenced by '%s' is missing or escapes the "
            "repository",
            reference, owner);
      }
      return false_v;
    }
    return true_v;
  }
  return vkr_harness_scene_manifest_enqueue(manifest, relative, out_error);
}

/**
 * Prepared glTF import writes runtime material files outside the source glTF.
 * They are optional before first import, but once present they and their packed
 * texture siblings affect rendered pixels and therefore belong to workload
 * identity.
 */
static bool8_t vkr_harness_scene_manifest_add_gltf_material_cache(
    const char *resolved_root, const char *gltf_relative,
    VkrHarnessSceneManifest *manifest, VkrHarnessError *out_error) {
  const char *stem = gltf_relative;
  for (const char *cursor = gltf_relative; *cursor; ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      stem = cursor + 1;
    }
  }
  uint64_t stem_length = string_length(stem);
  while (stem_length > 0u && stem[stem_length - 1u] != '.') {
    stem_length--;
  }
  if (stem_length == 0u || stem_length == 1u) {
    return true_v;
  }
  stem_length--;

  uint64_t source_hash = VKR_HARNESS_FNV1A64_OFFSET_BASIS;
  for (uint64_t i = 0; gltf_relative[i]; ++i) {
    source_hash ^= (uint64_t)(uint8_t)gltf_relative[i];
    source_hash *= VKR_HARNESS_FNV1A64_PRIME;
  }

  for (uint32_t material_index = 0u;
       material_index < VKR_HARNESS_MAX_SCENE_ASSETS; ++material_index) {
    char reference[VKR_HARNESS_PATH_MAX];
    if (string_format(reference, sizeof(reference),
                      "assets/materials/%.*s/gltf_mat_%016llx_%u.mt",
                      (int32_t)stem_length, stem,
                      (unsigned long long)source_hash, material_index) <= 0) {
      return false_v;
    }
    char relative[VKR_HARNESS_PATH_MAX];
    char absolute[VKR_HARNESS_PATH_MAX];
    if (!vkr_harness_scene_manifest_resolve(resolved_root, gltf_relative,
                                            reference, relative, absolute)) {
      break;
    }
    if (!vkr_harness_scene_manifest_enqueue(manifest, relative, out_error)) {
      return false_v;
    }
  }
  return true_v;
}

typedef struct VkrHarnessSceneHashJob {
  VkrHarnessSceneManifest *manifest;
  uint32_t *asset_indices;
  char (*absolute_paths)[VKR_HARNESS_PATH_MAX];
  uint32_t begin;
  uint32_t end;
  bool8_t success;
} VkrHarnessSceneHashJob;

static void *vkr_harness_scene_hash_worker(void *argument) {
  VkrHarnessSceneHashJob *job = argument;
  job->success = true_v;
  for (uint32_t i = job->begin; i < job->end; ++i) {
    VkrHarnessSceneAsset *asset = &job->manifest->assets[job->asset_indices[i]];
    if (!vkr_harness_sha256_file_sized(job->absolute_paths[i], asset->sha256,
                                       &asset->size)) {
      job->success = false_v;
      break;
    }
  }
  return NULL;
}

static bool8_t
vkr_harness_scene_manifest_hash_missing(const char *resolved_root, Arena *arena,
                                        VkrHarnessSceneManifest *manifest,
                                        VkrHarnessError *out_error) {
  uint32_t *indices = arena_alloc(
      arena, sizeof(*indices) * manifest->asset_count, ARENA_MEMORY_TAG_ARRAY);
  char(*paths)[VKR_HARNESS_PATH_MAX] = arena_alloc(
      arena, sizeof(*paths) * manifest->asset_count, ARENA_MEMORY_TAG_ARRAY);
  if (!indices || !paths) {
    return false_v;
  }
  uint32_t count = 0u;
  for (uint32_t i = 0u; i < manifest->asset_count; ++i) {
    if (manifest->assets[i].sha256[0] != '\0') {
      continue;
    }
    if (!vkr_harness_resolve_existing_path(
            resolved_root, manifest->assets[i].path, paths[count], out_error)) {
      return false_v;
    }
    indices[count++] = i;
  }
  if (count == 0u) {
    return true_v;
  }

  const uint32_t worker_count =
      Min(Min(Max(vkr_platform_get_logical_core_count(), 1u), 8u), count);
  VkrThread threads[8] = {0};
  VkrHarnessSceneHashJob jobs[8] = {0};
  VkrAllocator thread_allocator = {.ctx = arena};
  vkr_allocator_arena(&thread_allocator);
  uint32_t created = 0u;
  for (uint32_t worker = 0u; worker < worker_count; ++worker) {
    jobs[worker] = (VkrHarnessSceneHashJob){
        .manifest = manifest,
        .asset_indices = indices,
        .absolute_paths = paths,
        .begin = (uint32_t)(((uint64_t)count * worker) / worker_count),
        .end = (uint32_t)(((uint64_t)count * (worker + 1u)) / worker_count),
    };
    if (!vkr_thread_create(&thread_allocator, &threads[worker],
                           vkr_harness_scene_hash_worker, &jobs[worker])) {
      break;
    }
    created++;
  }
  bool8_t ok = created == worker_count;
  for (uint32_t worker = 0u; worker < created; ++worker) {
    ok = vkr_thread_join(threads[worker]) && jobs[worker].success && ok;
    (void)vkr_thread_destroy(&thread_allocator, &threads[worker]);
  }
  if (!ok) {
    vkr_harness_error_set(out_error, "scene_manifest.unreadable", "$.scene",
                          "Unable to digest the scene dependency closure");
  }
  return ok;
}

static void
vkr_harness_scene_manifest_digest(VkrHarnessSceneManifest *manifest) {
  vkr_sort(manifest->assets, manifest->asset_count, sizeof(*manifest->assets),
           vkr_harness_scene_asset_compare);
  VkrHarnessSha256 hash;
  vkr_harness_sha256_begin(&hash);
  for (uint32_t i = 0; i < manifest->asset_count; ++i) {
    const VkrHarnessSceneAsset *asset = &manifest->assets[i];
    const uint32_t path_length = (uint32_t)string_length(asset->path);
    const uint8_t path_prefix[4] = {
        (uint8_t)(path_length >> 24u), (uint8_t)(path_length >> 16u),
        (uint8_t)(path_length >> 8u), (uint8_t)path_length};
    vkr_harness_sha256_update(&hash, path_prefix, sizeof(path_prefix));
    vkr_harness_sha256_update(&hash, asset->path, path_length);
    vkr_harness_sha256_update(&hash, asset->sha256,
                              string_length(asset->sha256));
    uint8_t size_bytes[8];
    for (uint32_t b = 0; b < 8u; ++b) {
      size_bytes[b] = (uint8_t)(asset->size >> ((7u - b) * 8u));
    }
    vkr_harness_sha256_update(&hash, size_bytes, sizeof(size_bytes));
  }
  vkr_harness_sha256_end(&hash, manifest->sha256);
}

bool8_t vkr_harness_scene_manifest_build(const char *repo_root,
                                         const char *scene, Arena *arena,
                                         VkrHarnessSceneManifest *out_manifest,
                                         VkrHarnessError *out_error) {
  if (!repo_root || !scene || !arena || !out_manifest) {
    return false_v;
  }
  MemZero(out_manifest, sizeof(*out_manifest));
  out_manifest->assets = arena_alloc(
      arena, sizeof(VkrHarnessSceneAsset) * VKR_HARNESS_MAX_SCENE_ASSETS,
      ARENA_MEMORY_TAG_ARRAY);
  if (!out_manifest->assets) {
    vkr_harness_error_set(out_error, "scene_manifest.allocate", "$.scene",
                          "Unable to allocate scene manifest storage");
    return false_v;
  }
  MemZero(out_manifest->assets,
          sizeof(VkrHarnessSceneAsset) * VKR_HARNESS_MAX_SCENE_ASSETS);
  string_format(out_manifest->scene, sizeof(out_manifest->scene), "%s", scene);

  char resolved_root[VKR_HARNESS_PATH_MAX];
  if (!vkr_harness_realpath(repo_root, resolved_root) ||
      !vkr_harness_scene_manifest_add_reference(
          resolved_root, scene, out_manifest, scene, true_v, out_error)) {
    return false_v;
  }

  Arena *file_arena = arena_create(MB(64), MB(4));
  if (!file_arena) {
    return false_v;
  }
  bool8_t ok = true_v;
  for (uint32_t asset_index = 0; ok && asset_index < out_manifest->asset_count;
       ++asset_index) {
    VkrHarnessSceneAsset *asset = &out_manifest->assets[asset_index];
    const bool8_t is_json =
        vkr_harness_scene_path_ends_with(asset->path, ".json") ||
        vkr_harness_scene_path_ends_with(asset->path, ".gltf");
    const bool8_t is_glb =
        vkr_harness_scene_path_ends_with(asset->path, ".glb");
    const bool8_t scan_lines =
        vkr_harness_scene_path_ends_with(asset->path, ".obj") ||
        vkr_harness_scene_path_ends_with(asset->path, ".mtl") ||
        vkr_harness_scene_path_ends_with(asset->path, ".mt") ||
        vkr_harness_scene_path_ends_with(asset->path, ".fontcfg") ||
        vkr_harness_scene_path_ends_with(asset->path, ".fnt");
    if ((vkr_harness_scene_path_ends_with(asset->path, ".gltf") || is_glb) &&
        !vkr_harness_scene_manifest_add_gltf_material_cache(
            resolved_root, asset->path, out_manifest, out_error)) {
      ok = false_v;
      break;
    }
    char absolute[VKR_HARNESS_PATH_MAX];
    if (!vkr_harness_resolve_existing_path(resolved_root, asset->path, absolute,
                                           out_error)) {
      ok = false_v;
      break;
    }

    if (!is_json && !is_glb && !scan_lines) {
      if (!vkr_harness_scene_path_ends_with(asset->path, ".vkt")) {
        char packed[VKR_HARNESS_PATH_MAX];
        if (string_format(packed, sizeof(packed), "%s.vkt", asset->path) > 0) {
          (void)vkr_harness_scene_manifest_add_reference(
              resolved_root, asset->path, out_manifest, packed, false_v,
              out_error);
        }
      }
      continue;
    }

    Scratch scratch = scratch_create(file_arena);
    uint8_t *bytes = NULL;
    uint64_t size = 0u;
    if (!vkr_harness_read_file(absolute, file_arena, &bytes, &size)) {
      scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
      vkr_harness_error_set(out_error, "scene_manifest.unreadable", "$.scene",
                            "Unable to read scene dependency '%s'",
                            asset->path);
      ok = false_v;
      break;
    }
    asset->size = size;
    vkr_harness_sha256_bytes(bytes, size, asset->sha256);
    const uint8_t *parse_bytes = bytes;
    uint64_t parse_size = size;
    if (is_glb) {
      if (size < 20u || MemCompare(bytes, "glTF", 4u) != 0) {
        vkr_harness_error_set(out_error, "scene_manifest.invalid_glb",
                              "$.scene", "Invalid GLB dependency '%s'",
                              asset->path);
        ok = false_v;
      } else {
        const uint32_t chunk_length =
            (uint32_t)bytes[12] | ((uint32_t)bytes[13] << 8u) |
            ((uint32_t)bytes[14] << 16u) | ((uint32_t)bytes[15] << 24u);
        const uint32_t chunk_type =
            (uint32_t)bytes[16] | ((uint32_t)bytes[17] << 8u) |
            ((uint32_t)bytes[18] << 16u) | ((uint32_t)bytes[19] << 24u);
        if (chunk_type != 0x4E4F534Au || (uint64_t)chunk_length + 20u > size) {
          vkr_harness_error_set(out_error, "scene_manifest.invalid_glb",
                                "$.scene", "GLB '%s' has no valid JSON chunk",
                                asset->path);
          ok = false_v;
        } else {
          parse_bytes = bytes + 20u;
          parse_size = chunk_length;
        }
      }
    }

    // JSON/glTF quoted URIs and paths. Cubemap sources are expressed as a
    // base path plus extension, so expand the renderer's six-face convention.
    bool8_t expect_cubemap_base = false_v;
    bool8_t expect_cubemap_extension = false_v;
    char cubemap_base[VKR_HARNESS_PATH_MAX] = {0};
    for (uint64_t i = 0; ok && (is_json || is_glb) && i < parse_size; ++i) {
      if (parse_bytes[i] != '"') {
        continue;
      }
      uint64_t end = i + 1u;
      while (end < parse_size && parse_bytes[end] != '"' &&
             parse_bytes[end] != '\n' && parse_bytes[end] != '\r') {
        end++;
      }
      if (end >= parse_size || parse_bytes[end] != '"' || end - i - 1u == 0u ||
          end - i - 1u >= VKR_HARNESS_PATH_MAX) {
        continue;
      }
      char token[VKR_HARNESS_PATH_MAX];
      MemCopy(token, parse_bytes + i + 1u, end - i - 1u);
      token[end - i - 1u] = '\0';
      if (expect_cubemap_base) {
        string_format(cubemap_base, sizeof(cubemap_base), "%s", token);
        expect_cubemap_base = false_v;
      } else if (expect_cubemap_extension) {
        static const char *faces[] = {"r", "l", "u", "d", "f", "b"};
        for (uint32_t face = 0; face < ArrayCount(faces); ++face) {
          char face_path[VKR_HARNESS_PATH_MAX];
          string_format(face_path, sizeof(face_path), "%s_%s.%s", cubemap_base,
                        faces[face], token);
          if (!vkr_harness_scene_manifest_add_reference(
                  resolved_root, asset->path, out_manifest, face_path, true_v,
                  out_error)) {
            ok = false_v;
            break;
          }
        }
        cubemap_base[0] = '\0';
        expect_cubemap_extension = false_v;
        if (!ok) {
          break;
        }
      } else if (string_equals(token, "base_path")) {
        expect_cubemap_base = true_v;
      } else if (cubemap_base[0] && string_equals(token, "extension")) {
        expect_cubemap_extension = true_v;
      }
      if (vkr_harness_scene_asset_extension(token) &&
          !vkr_harness_scene_manifest_add_reference(resolved_root, asset->path,
                                                    out_manifest, token, true_v,
                                                    out_error)) {
        ok = false_v;
        break;
      }
      i = end;
    }

    // OBJ/MTL/material line tokens. Queries are retained until resolution so
    // material sampler controls do not alter the referenced file identity.
    for (uint64_t i = 0; ok && scan_lines && i < parse_size;) {
      while (i < parse_size &&
             (parse_bytes[i] == ' ' || parse_bytes[i] == '\t' ||
              parse_bytes[i] == '\r' || parse_bytes[i] == '\n' ||
              parse_bytes[i] == '=' || parse_bytes[i] == ',' ||
              parse_bytes[i] == '"')) {
        i++;
      }
      if (i >= parse_size) {
        break;
      }
      if (parse_bytes[i] == '#') {
        while (i < parse_size && parse_bytes[i] != '\n') {
          i++;
        }
        continue;
      }
      uint64_t end = i;
      // '=' separates a material key from its value, but it also appears inside
      // a sampler query. Once the token enters a query it runs to the real
      // delimiter, so the retained reference keeps the whole '?cs=srgb'.
      bool8_t in_query = false_v;
      while (end < parse_size && parse_bytes[end] != ' ' &&
             parse_bytes[end] != '\t' && parse_bytes[end] != '\r' &&
             parse_bytes[end] != '\n' && parse_bytes[end] != ',' &&
             parse_bytes[end] != '"' && (in_query || parse_bytes[end] != '=')) {
        in_query = in_query || parse_bytes[end] == '?';
        end++;
      }
      if (end > i && end - i < VKR_HARNESS_PATH_MAX) {
        char token[VKR_HARNESS_PATH_MAX];
        MemCopy(token, parse_bytes + i, end - i);
        token[end - i] = '\0';
        bool8_t runtime_reference = true_v;
        const uint64_t owner_length = string_length(asset->path);
        const bool8_t owner_is_mtl =
            owner_length >= 4u &&
            string_equals(asset->path + owner_length - 4u, ".mtl");
        const bool8_t owner_is_obj =
            owner_length >= 4u &&
            string_equals(asset->path + owner_length - 4u, ".obj");
        if (owner_is_mtl || owner_is_obj) {
          uint64_t line_start = i;
          while (line_start > 0u && parse_bytes[line_start - 1u] != '\n' &&
                 parse_bytes[line_start - 1u] != '\r') {
            line_start--;
          }
          while (line_start < parse_size && (parse_bytes[line_start] == ' ' ||
                                             parse_bytes[line_start] == '\t')) {
            line_start++;
          }
          const uint64_t remaining = parse_size - line_start;
          runtime_reference =
              owner_is_obj
                  ? (remaining >= 6u &&
                     MemCompare(parse_bytes + line_start, "mtllib", 6u) == 0)
                  : ((remaining >= 6u && MemCompare(parse_bytes + line_start,
                                                    "map_Kd", 6u) == 0) ||
                     (remaining >= 6u && MemCompare(parse_bytes + line_start,
                                                    "map_Ks", 6u) == 0) ||
                     (remaining >= 8u && MemCompare(parse_bytes + line_start,
                                                    "map_bump", 8u) == 0) ||
                     (remaining >= 4u &&
                      MemCompare(parse_bytes + line_start, "bump", 4u) == 0));
        }
        if (runtime_reference && vkr_harness_scene_asset_extension(token) &&
            !vkr_harness_scene_manifest_add_reference(
                resolved_root, asset->path, out_manifest, token, true_v,
                out_error)) {
          ok = false_v;
          break;
        }
      }
      i = end + (end == i ? 1u : 0u);
    }

    // Runtime texture loading prefers a packed sibling when one exists.
    if (ok && vkr_harness_scene_path_ends_with(asset->path, ".fnt")) {
      char cache[VKR_HARNESS_PATH_MAX];
      if (string_format(cache, sizeof(cache), "%s.vkf", asset->path) > 0) {
        (void)vkr_harness_scene_manifest_add_reference(
            resolved_root, asset->path, out_manifest, cache, false_v,
            out_error);
      }
    } else if (ok && !vkr_harness_scene_path_ends_with(asset->path, ".vkt")) {
      char packed[VKR_HARNESS_PATH_MAX];
      if (string_format(packed, sizeof(packed), "%s.vkt", asset->path) > 0) {
        (void)vkr_harness_scene_manifest_add_reference(
            resolved_root, asset->path, out_manifest, packed, false_v,
            out_error);
      }
    }
    scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  }
  arena_destroy(file_arena);
  if (!ok) {
    return false_v;
  }
  if (!vkr_harness_scene_manifest_hash_missing(resolved_root, arena,
                                               out_manifest, out_error)) {
    return false_v;
  }
  vkr_harness_scene_manifest_digest(out_manifest);
  return true_v;
}

bool8_t
vkr_harness_scene_manifest_write(const char *path,
                                 const VkrHarnessSceneManifest *manifest,
                                 VkrHarnessError *out_error) {
  if (!path || !manifest || !manifest->assets) {
    return false_v;
  }
  VkrJsonFileWriter file = {0};
  if (!vkr_json_file_writer_begin(
          &file, string8_create_from_cstr((const uint8_t *)path,
                                          string_length(path)))) {
    vkr_harness_error_set(out_error, "scene_manifest.write", "$.scene",
                          "Unable to begin scene manifest '%s'", path);
    return false_v;
  }
  VkrJsonWriter *writer = &file.writer;
  bool8_t ok =
      vkr_json_writer_begin_object(writer) &&
      vkr_harness_json_emit_u64(writer, "schema_version", 1u) &&
      vkr_harness_json_emit_string(writer, "kind",
                                   "vkr.harness.scene-content-manifest") &&
      vkr_harness_json_emit_string(writer, "scene", manifest->scene) &&
      vkr_harness_json_emit_string(writer, "sha256", manifest->sha256) &&
      vkr_json_writer_name(writer, string8_lit("assets")) &&
      vkr_json_writer_begin_array(writer);
  for (uint32_t i = 0; ok && i < manifest->asset_count; ++i) {
    ok = vkr_json_writer_begin_object(writer) &&
         vkr_harness_json_emit_string(writer, "path",
                                      manifest->assets[i].path) &&
         vkr_harness_json_emit_string(writer, "sha256",
                                      manifest->assets[i].sha256) &&
         vkr_harness_json_emit_u64(writer, "bytes", manifest->assets[i].size) &&
         vkr_json_writer_end_object(writer);
  }
  ok = ok && vkr_json_writer_end_array(writer) &&
       vkr_json_writer_end_object(writer) && vkr_json_file_writer_commit(&file);
  if (!ok) {
    vkr_json_file_writer_abort(&file);
    vkr_harness_error_set(out_error, "scene_manifest.write", "$.scene",
                          "Unable to publish scene manifest '%s'", path);
  }
  return ok;
}

bool8_t vkr_harness_scene_manifest_verify_file(const char *path,
                                               const char *expected_digest) {
  if (!path || !expected_digest || expected_digest[0] == '\0') {
    return false_v;
  }
  char observed[VKR_HARNESS_DIGEST_MAX] = {0};
  return vkr_harness_sha256_file(path, observed) &&
                 string_equals(observed, expected_digest)
             ? true_v
             : false_v;
}

bool8_t vkr_harness_scene_manifest_publish(const char *repo_root,
                                           const char *scene,
                                           const char *run_root, Arena *arena,
                                           VkrHarnessReport *report,
                                           VkrHarnessError *out_error) {
  if (!run_root || !arena || !report) {
    return false_v;
  }
  Scratch scratch = scratch_create(arena);
  VkrHarnessSceneManifest manifest = {0};
  char path[VKR_HARNESS_PATH_MAX];
  string_format(path, sizeof(path), "%s/scene-content-manifest.json", run_root);
  const bool8_t ok =
      vkr_harness_scene_manifest_build(repo_root, scene, arena, &manifest,
                                       out_error) &&
      vkr_harness_scene_manifest_write(path, &manifest, out_error) &&
      vkr_harness_report_add_artifact(report, "scene.content_manifest",
                                      "scene-content-manifest.json",
                                      "application/json", path);
  scratch_destroy(scratch, ARENA_MEMORY_TAG_ARRAY);
  return ok;
}
