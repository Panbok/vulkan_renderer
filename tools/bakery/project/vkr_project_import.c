#include "vkr_project_internal.h"

#include "../vkr_bakery_buffer.h"
#include "filesystem/filesystem.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Imports: source snapshots, model/font/material/texture/cubemap records,
 * the editor bootstrap bundle and project fonts. Texture packing runs as
 * cached `texture` actions in one graph per bundle. */

/* A 2x2 mid-gray RGBA PNG that stands in for a missing model image. */
vkr_internal const uint8_t vkr_project_placeholder_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
    0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24,
    0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
    0x68, 0x68, 0x68, 0xf8, 0x0f, 0xc2, 0x0c, 0x30, 0x06, 0x00, 0x56,
    0xf4, 0x09, 0xfd, 0x4b, 0x4b, 0xe9, 0x2c, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

vkr_internal bool8_t vkr_project_is_image_suffix(const char *suffix) {
  return !strcmp(suffix, ".png") || !strcmp(suffix, ".jpg") ||
         !strcmp(suffix, ".jpeg") || !strcmp(suffix, ".bmp") ||
         !strcmp(suffix, ".tga");
}

vkr_internal VkrBakeryJson *vkr_project_cstr(VkrProjectJob *job,
                                             const char *text) {
  return text ? vkr_bakery_json_cstr(job->arena, text)
              : vkr_bakery_json_null(job->arena);
}

vkr_internal VkrBakeryJson *vkr_project_int(VkrProjectJob *job, int64_t value) {
  return vkr_bakery_json_int(job->arena, value);
}

vkr_internal bool8_t vkr_project_file_size(const char *path, uint64_t *out) {
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return false_v;
  }
  *out = info.size;
  return true_v;
}

/* Splits text into lines like str.splitlines() for \n, \r\n and \r. */
bool8_t vkr_project_split_lines(VkrProjectJob *job, const char *text,
                                VkrProjectStrings *out) {
  MemZero(out, sizeof(*out));
  const char *start = text;
  for (const char *c = text;; ++c) {
    if (*c == '\n' || *c == '\r' || *c == 0) {
      if (*c == 0 && c == start) {
        break;
      }
      char *line = (char *)arena_alloc(job->arena, (uint64_t)(c - start) + 1u,
                                       ARENA_MEMORY_TAG_STRING);
      if (!line) {
        return vkr_project_fail(job, "Out of memory");
      }
      MemCopy(line, start, (size_t)(c - start));
      line[c - start] = 0;
      VKR_PROJECT_TRY(vkr_project_strings_push(job, out, line));
      if (*c == 0) {
        break;
      }
      if (*c == '\r' && c[1] == '\n') {
        c += 1;
      }
      start = c + 1;
      if (*start == 0) {
        break;
      }
    }
  }
  return true_v;
}

bool8_t vkr_project_write_lines(VkrProjectJob *job, const char *path,
                                const VkrProjectStrings *lines) {
  VkrBakeryBuffer buffer = {0};
  for (uint32_t i = 0u; i < lines->count; ++i) {
    if (i) {
      vkr_bakery_buffer_append(&buffer, "\n", 1u);
    }
    vkr_bakery_buffer_append_cstr(&buffer, lines->items[i]);
  }
  vkr_bakery_buffer_append(&buffer, "\n", 1u);
  const bool8_t ok = !buffer.failed &&
                     vkr_project_write_text(
                         job, path, (const char *)buffer.data, buffer.length);
  vkr_bakery_buffer_free(&buffer);
  return ok;
}

/* Splits "key=value" with str.partition and strips both sides. */
bool8_t vkr_project_partition_line(const char *line, char *key,
                                   uint32_t key_capacity, char *value,
                                   uint32_t capacity) {
  const char *equals = strchr(line, '=');
  if (!equals) {
    return false_v;
  }
  const char *key_start = line;
  const char *key_end = equals;
  while (key_start < key_end && (*key_start == ' ' || *key_start == '\t')) {
    key_start += 1;
  }
  while (key_end > key_start &&
         (key_end[-1] == ' ' || key_end[-1] == '\t' || key_end[-1] == '\r')) {
    key_end -= 1;
  }
  (void)snprintf(key, key_capacity, "%.*s", (int)(key_end - key_start),
                 key_start);
  const char *value_start = equals + 1;
  while (*value_start == ' ' || *value_start == '\t') {
    value_start += 1;
  }
  const char *value_end = value_start + strlen(value_start);
  while (value_end > value_start &&
         (value_end[-1] == ' ' || value_end[-1] == '\t' ||
          value_end[-1] == '\r' || value_end[-1] == '\n')) {
    value_end -= 1;
  }
  (void)snprintf(value, capacity, "%.*s", (int)(value_end - value_start),
                 value_start);
  return true_v;
}

// =============================================================================
// Records
// =============================================================================

VkrBakeryJson *vkr_project_artifact(VkrProjectJob *job, const char *kind,
                                    const char *name, const char *path,
                                    const char *role, const char *import_id,
                                    const char *source,
                                    VkrBakeryJson *metadata) {
  Arena *arena = job->arena;
  char id[37];
  vkr_project_uuid4(id);
  const char *source_reference = NULL;
  const char *path_reference = NULL;
  char hash[VKR_BAKERY_SHA256_HEX];
  if ((source && !vkr_project_managed_reference(job, source, job->stage,
                                                &source_reference)) ||
      !vkr_project_managed_reference(job, path, job->stage, &path_reference) ||
      !vkr_project_digest(job, path, hash)) {
    return NULL;
  }
  VkrBakeryJson *asset = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, asset, "id", vkr_bakery_json_cstr(arena, id));
  vkr_bakery_json_set(arena, asset, "kind", vkr_bakery_json_cstr(arena, kind));
  vkr_bakery_json_set(arena, asset, "name", vkr_bakery_json_cstr(arena, name));
  vkr_bakery_json_set(arena, asset, "import_id",
                      vkr_project_cstr(job, import_id));
  vkr_bakery_json_set(arena, asset, "source",
                      vkr_project_cstr(job, source_reference));
  VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
  VkrBakeryJson *product = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, product, "role",
                      vkr_bakery_json_cstr(arena, role ? role : kind));
  vkr_bakery_json_set(arena, product, "path",
                      vkr_bakery_json_cstr(arena, path_reference));
  vkr_bakery_json_set(arena, product, "version", vkr_project_int(job, 1));
  vkr_bakery_json_append(artifacts, product);
  vkr_bakery_json_set(arena, asset, "artifacts", artifacts);
  vkr_bakery_json_set(
      arena, asset, "fingerprint",
      vkr_bakery_json_cstr(arena, vkr_project_printf(job, "sha256:%s", hash)));
  for (VkrBakeryJson *field = metadata ? metadata->first : NULL; field;) {
    VkrBakeryJson *next = field->next;
    vkr_bakery_json_set(arena, asset, (const char *)field->key.str,
                        vkr_bakery_json_clone(arena, field));
    field = next;
  }
  vkr_bakery_json_append(job->assets, asset);
  return vkr_project_reference(job, job->asset_scope, id, role ? role : kind);
}

VkrBakeryJson *vkr_project_document(VkrProjectJob *job) {
  VkrBakeryJson *project = NULL;
  if (vkr_bakery_is_file(job->project_path)) {
    project = vkr_project_load_json(job, job->project_path,
                                    VKR_PROJECT_MAX_JSON_BYTES);
    if (!project) {
      return NULL;
    }
  } else {
    project = vkr_bakery_json_object(job->arena);
    vkr_bakery_json_set(job->arena, project, "assets",
                        vkr_bakery_json_array(job->arena));
    vkr_bakery_json_set(job->arena, project, "default_font",
                        vkr_bakery_json_null(job->arena));
  }
  if (job->pending_project_assets) {
    vkr_bakery_json_set(
        job->arena, project, "assets",
        vkr_bakery_json_clone(job->arena, job->pending_project_assets));
  }
  if (job->pending_default_font) {
    vkr_bakery_json_set(
        job->arena, project, "default_font",
        vkr_bakery_json_clone(job->arena, job->pending_default_font));
  }
  return project;
}

// =============================================================================
// Bootstrap bundle and project font
// =============================================================================

vkr_internal bool8_t vkr_project_font_type(VkrProjectJob *job,
                                           const char *config, char *out_type) {
  const char *text = NULL;
  VKR_PROJECT_TRY(vkr_project_read_text(job, config, VKR_PROJECT_MAX_JSON_BYTES,
                                        &text, NULL));
  VkrProjectStrings lines;
  VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
  out_type[0] = 0;
  for (uint32_t i = 0u; i < lines.count; ++i) {
    char key[256];
    char value[1024];
    if (vkr_project_partition_line(lines.items[i], key, sizeof(key), value,
                                   sizeof(value)) &&
        strcmp(key, "type") == 0) {
      (void)snprintf(out_type, 256, "%s", value);
      break;
    }
  }
  return true_v;
}

bool8_t vkr_project_ensure_bootstrap(VkrProjectJob *job) {
  char bundle[VKR_PROJECT_PATH];
  char manifest_path[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/editor/bundles/1", job->workspace);
  (void)snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
                 bundle);
  if (vkr_bakery_is_file(manifest_path)) {
    VkrBakeryJson *manifest =
        vkr_project_load_json(job, manifest_path, VKR_PROJECT_MAX_JSON_BYTES);
    VKR_PROJECT_TRY(manifest);
    int64_t version = 0;
    const VkrBakeryJson *files = vkr_bakery_json_get(manifest, "files");
    if (!vkr_bakery_json_get_int(manifest, "version", &version) ||
        version != 1 || !files || files->type != VKR_BAKERY_JSON_ARRAY) {
      return vkr_project_fail(job, "Editor bundle manifest is invalid; repair "
                                   "the installed editor bundle");
    }
    for (const VkrBakeryJson *record = files->first; record;
         record = record->next) {
      char path[VKR_PROJECT_PATH];
      char hash[VKR_BAKERY_SHA256_HEX];
      const char *expected = vkr_project_json_text(record, "sha256");
      VKR_PROJECT_TRY(vkr_project_contained(
          job, bundle, vkr_project_json_text(record, "path"), true_v, path));
      if (!vkr_bakery_is_file(path) ||
          !vkr_bakery_hash_file(path, hash, NULL) || !expected ||
          strcmp(hash, expected) != 0) {
        return vkr_project_fail(job, "Editor bootstrap content is missing or "
                                     "changed; repair the installed editor "
                                     "bundle");
      }
    }
    return true_v;
  }
  if (job->read_only) {
    return vkr_project_fail(job, "Editor bundle is unavailable; open this "
                                 "workspace with write access to prepare it "
                                 "first");
  }
  const char *requested =
      vkr_project_json_text(job->request, "bootstrap_directory");
  if (!requested || !requested[0]) {
    /* Library callers may validate CPU-only documents without launching the
       editor. Installed editor requests always provide this root. */
    return true_v;
  }
  char source[VKR_PROJECT_PATH];
  if (!vkr_project_resolve(requested, true_v, source, sizeof(source))) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            requested);
  }
  char fonts[VKR_PROJECT_PATH];
  char default_config[VKR_PROJECT_PATH];
  (void)vkr_bakery_path_join(fonts, sizeof(fonts), source, "fonts");
  (void)vkr_bakery_path_join(default_config, sizeof(default_config), fonts,
                             "UbuntuMono-cooked.fontcfg");
  if (!vkr_bakery_is_file(default_config)) {
    return vkr_project_fail(job,
                            "Installed editor bootstrap has no default cooked "
                            "font");
  }
  char parent[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(parent, sizeof(parent), bundle);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, parent));
  char staging[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_mkdtemp(job, parent, ".bootstrap-", staging));
  bool8_t ok =
      vkr_project_progress(job, "Preparing editor resources", 0.05, "");
  Arena *arena = job->arena;
  VkrBakeryJson *files = vkr_bakery_json_array(arena);
  VkrBakeryJson *assets = vkr_bakery_json_array(arena);
  VkrProjectStrings paths = {0};
  ok = ok && vkr_project_walk_files(job, fonts, &paths, true_v,
                                    "Installed editor bundle contains a "
                                    "symlink");
  for (uint32_t i = 0u; ok && i < paths.count; ++i) {
    char relative[VKR_PROJECT_PATH];
    char destination[VKR_PROJECT_PATH];
    const char *reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    ok = vkr_bakery_path_relative(source, paths.items[i], relative,
                                  sizeof(relative)) &&
         vkr_bakery_path_join(destination, sizeof(destination), staging,
                              relative) &&
         vkr_project_copy_file(job, paths.items[i], destination) &&
         vkr_project_managed_reference(job, destination, staging, &reference) &&
         vkr_project_digest(job, destination, hash);
    if (ok) {
      VkrBakeryJson *record = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, record, "path",
                          vkr_bakery_json_cstr(arena, reference));
      vkr_bakery_json_set(arena, record, "sha256",
                          vkr_bakery_json_cstr(arena, hash));
      vkr_bakery_json_append(files, record);
    }
  }
  char staged_fonts[VKR_PROJECT_PATH];
  (void)vkr_bakery_path_join(staged_fonts, sizeof(staged_fonts), staging,
                             "fonts");
  VkrProjectStrings names = {0};
  ok = ok && vkr_project_list(job, staged_fonts, &names);
  for (uint32_t i = 0u; ok && i < names.count; ++i) {
    char suffix[32];
    vkr_project_suffix_lower(names.items[i], suffix, sizeof(suffix));
    if (strcmp(suffix, ".fontcfg") != 0) {
      continue;
    }
    char config[VKR_PROJECT_PATH];
    char type[256];
    (void)vkr_bakery_path_join(config, sizeof(config), staged_fonts,
                               names.items[i]);
    ok = vkr_project_font_type(job, config, type);
    if (!ok || strcmp(type, "cooked_mtsdf") != 0) {
      continue;
    }
    VkrBakeryJson *visited = vkr_bakery_json_object(arena);
    ok =
        vkr_project_validate_bundle_dependencies(job, staging, config, visited);
    /* Phosphor atlases draw editor icons; they are not scene fonts. */
    if (!ok || strncmp(names.items[i], "Phosphor", 8u) == 0) {
      continue;
    }
    char stem[256];
    char hash[VKR_BAKERY_SHA256_HEX];
    const char *reference = NULL;
    vkr_project_stem(names.items[i], stem, sizeof(stem));
    ok = vkr_project_managed_reference(job, config, staging, &reference) &&
         vkr_project_digest(job, config, hash);
    if (!ok) {
      break;
    }
    VkrBakeryJson *asset = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, asset, "id",
        vkr_bakery_json_cstr(
            arena, strcmp(names.items[i], "UbuntuMono-cooked.fontcfg") == 0
                       ? "default-scene-font"
                       : stem));
    vkr_bakery_json_set(arena, asset, "kind",
                        vkr_bakery_json_cstr(arena, "font"));
    vkr_bakery_json_set(arena, asset, "name",
                        vkr_bakery_json_cstr(arena, stem));
    VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
    VkrBakeryJson *product = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, product, "role",
                        vkr_bakery_json_cstr(arena, "font"));
    vkr_bakery_json_set(arena, product, "path",
                        vkr_bakery_json_cstr(arena, reference));
    vkr_bakery_json_set(arena, product, "version", vkr_project_int(job, 1));
    vkr_bakery_json_append(artifacts, product);
    vkr_bakery_json_set(arena, asset, "artifacts", artifacts);
    vkr_bakery_json_set(arena, asset, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
    vkr_bakery_json_append(assets, asset);
  }
  if (ok) {
    VkrBakeryJson *manifest = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, manifest, "version", vkr_project_int(job, 1));
    vkr_bakery_json_set(arena, manifest, "id",
                        vkr_bakery_json_cstr(arena, "vkr-editor-bundle-1"));
    vkr_bakery_json_set(arena, manifest, "assets", assets);
    vkr_bakery_json_set(arena, manifest, "files", files);
    char staged_manifest[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(staged_manifest, sizeof(staged_manifest),
                               staging, "manifest.json");
    ok = vkr_project_atomic_json(job, staged_manifest, manifest);
    if (ok && vkr_project_exists(bundle)) {
      ok = vkr_project_fail(job, "An incomplete editor bundle exists; repair "
                                 "it before opening the workspace");
    }
    ok = ok && vkr_project_publish_directory(job, staging, bundle);
  }
  if (vkr_project_exists(staging)) {
    (void)vkr_project_remove_tree(staging);
  }
  return ok;
}

bool8_t vkr_project_prepare_font(VkrProjectJob *job) {
  const char *source =
      vkr_project_json_text(job->request, "project_font_source");
  if (!source || !source[0]) {
    return true_v;
  }
  char staging_parent[VKR_PROJECT_PATH];
  (void)snprintf(staging_parent, sizeof(staging_parent), "%s/.staging",
                 job->project_root);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, staging_parent));
  char temporary[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(
      vkr_project_mkdtemp(job, staging_parent, "project-font-", temporary));
  char old_stage[VKR_PROJECT_PATH];
  (void)snprintf(old_stage, sizeof(old_stage), "%s", job->stage);
  VkrBakeryJson *old_assets = job->assets;
  VkrBakeryJson *old_bakes = vkr_bakery_json_get(job->request, "bakes");
  Arena *arena = job->arena;
  VkrBakeryJson *bakes = old_bakes ? vkr_bakery_json_clone(arena, old_bakes)
                                   : vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, bakes, "prepare_assets",
                      vkr_bakery_json_bool(arena, true_v));
  vkr_bakery_json_set(arena, job->request, "bakes", bakes);
  (void)snprintf(job->stage, sizeof(job->stage), "%s", temporary);
  job->assets = vkr_bakery_json_array(arena);

  bool8_t ok = true_v;
  VkrBakeryJson *reference = vkr_project_import_font(job, source);
  ok = reference != NULL;
  VkrBakeryJson *records = job->assets;
  for (VkrBakeryJson *record = ok ? records->first : NULL; ok && record;
       record = record->next) {
    VkrBakeryJson *closure = vkr_bakery_json_object(arena);
    VkrBakeryJson *products = vkr_bakery_json_get(record, "artifacts");
    for (VkrBakeryJson *product = products ? products->first : NULL;
         ok && product; product = product->next) {
      char path[VKR_PROJECT_PATH];
      ok = vkr_project_contained(job, temporary,
                                 vkr_project_json_text(product, "path"), true_v,
                                 path) &&
           vkr_project_validate_bundle_dependencies(job, temporary, path,
                                                    closure);
    }
    VkrBakeryJson *digests = vkr_bakery_json_object(arena);
    for (VkrBakeryJson *entry = ok ? closure->first : NULL; ok && entry;
         entry = entry->next) {
      const char *path = (const char *)entry->key.str;
      const char *relative = NULL;
      char hash[VKR_BAKERY_SHA256_HEX];
      ok = vkr_project_managed_reference(job, path, temporary, &relative) &&
           vkr_project_digest(job, path, hash);
      if (ok) {
        vkr_bakery_json_set(arena, digests, relative,
                            vkr_bakery_json_cstr(arena, hash));
      }
    }
    if (ok) {
      vkr_bakery_json_set(arena, record, "closure", digests);
    }
  }
  if (ok) {
    char builds[VKR_PROJECT_PATH];
    char staged_builds[VKR_PROJECT_PATH];
    (void)snprintf(builds, sizeof(builds), "%s/builds", job->project_root);
    (void)snprintf(staged_builds, sizeof(staged_builds), "%s/builds",
                   temporary);
    ok = vkr_project_make_dirs(job, builds);
    VkrProjectStrings revisions = {0};
    ok = ok && vkr_project_list(job, staged_builds, &revisions);
    for (uint32_t i = 0u; ok && i < revisions.count; ++i) {
      char from[VKR_PROJECT_PATH];
      char destination[VKR_PROJECT_PATH];
      (void)vkr_bakery_path_join(from, sizeof(from), staged_builds,
                                 revisions.items[i]);
      (void)vkr_bakery_path_join(destination, sizeof(destination), builds,
                                 revisions.items[i]);
      if (vkr_project_exists(destination)) {
        ok = vkr_project_fail(job, "Project font build revision collision");
        break;
      }
      ok = vkr_project_publish_directory(job, from, destination) &&
           vkr_project_strings_push(job, &job->project_builds, destination);
    }
  }
  if (ok) {
    VkrBakeryJson *project = vkr_project_document(job);
    ok = project != NULL;
    if (ok) {
      VkrBakeryJson *pending = vkr_bakery_json_array(arena);
      const VkrBakeryJson *existing = vkr_bakery_json_get(project, "assets");
      for (const VkrBakeryJson *item = existing ? existing->first : NULL; item;
           item = item->next) {
        vkr_bakery_json_append(pending, vkr_bakery_json_clone(arena, item));
      }
      for (const VkrBakeryJson *item = records->first; item;
           item = item->next) {
        vkr_bakery_json_append(pending, vkr_bakery_json_clone(arena, item));
      }
      job->pending_project_assets = pending;
      VkrBakeryJson *font = vkr_bakery_json_clone(arena, reference);
      vkr_bakery_json_set(arena, font, "scope",
                          vkr_bakery_json_cstr(arena, "project"));
      job->pending_default_font = font;
      VkrBakeryJson *document = vkr_project_document(job);
      ok = document && vkr_project_validate_document(
                           job, document, VKR_PROJECT_MAX_MANIFEST_BYTES);
    }
  }
  (void)snprintf(job->stage, sizeof(job->stage), "%s", old_stage);
  job->assets = old_assets;
  if (old_bakes) {
    vkr_bakery_json_set(arena, job->request, "bakes", old_bakes);
  } else {
    vkr_bakery_json_remove(job->request, "bakes");
  }
  (void)vkr_project_remove_tree(temporary);
  return ok;
}

// =============================================================================
// Model snapshots
// =============================================================================

typedef struct VkrProjectSnapshot {
  VkrProjectJob *job;
  char directory[VKR_PROJECT_PATH];
  VkrBakeryJson *dependencies;
} VkrProjectSnapshot;

/* Drops the images no texture samples as its source, such as the
   MSFT_texture_dds alternates Bistro lists for all 343 textures (2 GiB), so
   the snapshot neither copies nor hashes files the cooker never reads. The
   kept images are renumbered and a texture extension naming a dropped image
   is removed, with its entry in `extensionsUsed` once no texture uses it; a
   required extension keeps every image. */
vkr_internal bool8_t vkr_project_prune_unsampled_images(VkrProjectJob *job,
                                                        VkrBakeryJson *model) {
  Arena *arena = job->arena;
  VkrBakeryJson *images = vkr_bakery_json_get(model, "images");
  VkrBakeryJson *textures = vkr_bakery_json_get(model, "textures");
  if (!images || images->type != VKR_BAKERY_JSON_ARRAY || !images->count ||
      vkr_bakery_json_get(model, "extensionsRequired")) {
    return true_v;
  }
  const uint32_t count = images->count;
  int64_t *remap = (int64_t *)arena_alloc(arena, sizeof(int64_t) * count,
                                          ARENA_MEMORY_TAG_ARRAY);
  if (!remap) {
    return vkr_project_fail(job, "Out of memory");
  }
  for (uint32_t i = 0u; i < count; ++i) {
    remap[i] = -1;
  }
  for (const VkrBakeryJson *texture = textures ? textures->first : NULL;
       texture; texture = texture->next) {
    int64_t source = -1;
    if (vkr_project_integer(vkr_bakery_json_get(texture, "source"), &source) &&
        source >= 0 && source < (int64_t)count) {
      remap[source] = 0;
    }
  }
  int64_t kept = 0;
  VkrBakeryJson *pruned = vkr_bakery_json_array(arena);
  uint32_t index = 0u;
  for (VkrBakeryJson *image = images->first; image;
       image = image->next, ++index) {
    if (remap[index] >= 0) {
      remap[index] = kept++;
      vkr_bakery_json_append(pruned, vkr_bakery_json_clone(arena, image));
    }
  }
  if (kept == (int64_t)count) {
    return true_v;
  }
  VkrBakeryJson *used_names = vkr_bakery_json_array(arena);
  for (VkrBakeryJson *texture = textures ? textures->first : NULL; texture;
       texture = texture->next) {
    int64_t source = -1;
    if (vkr_project_integer(vkr_bakery_json_get(texture, "source"), &source) &&
        source >= 0 && source < (int64_t)count) {
      vkr_bakery_json_set(arena, texture, "source",
                          vkr_bakery_json_int(arena, remap[source]));
    }
    VkrBakeryJson *extensions = vkr_bakery_json_get(texture, "extensions");
    VkrBakeryJson *kept_extensions = vkr_bakery_json_object(arena);
    for (VkrBakeryJson *extension = extensions ? extensions->first : NULL;
         extension; extension = extension->next) {
      int64_t alternate = -1;
      const bool8_t names_image = vkr_project_integer(
          vkr_bakery_json_get(extension, "source"), &alternate);
      if (names_image && (alternate < 0 || alternate >= (int64_t)count ||
                          remap[alternate] < 0)) {
        continue;
      }
      VkrBakeryJson *copy = vkr_bakery_json_clone(arena, extension);
      if (names_image) {
        vkr_bakery_json_set(arena, copy, "source",
                            vkr_bakery_json_int(arena, remap[alternate]));
      }
      const char *name = vkr_project_printf(
          job, "%.*s", (int)extension->key.length, extension->key.str);
      vkr_bakery_json_set(arena, kept_extensions, name, copy);
      bool8_t listed = false_v;
      for (const VkrBakeryJson *used = used_names->first; used && !listed;
           used = used->next) {
        listed = vkr_bakery_json_is_string(used, name);
      }
      if (!listed) {
        vkr_bakery_json_append(used_names, vkr_bakery_json_cstr(arena, name));
      }
    }
    if (extensions) {
      if (kept_extensions->count) {
        vkr_bakery_json_set(arena, texture, "extensions", kept_extensions);
      } else {
        (void)vkr_bakery_json_remove(texture, "extensions");
      }
    }
  }
  /* An extension survives in extensionsUsed if a texture still uses it or
     it applies to something other than textures. */
  VkrBakeryJson *declared = vkr_bakery_json_get(model, "extensionsUsed");
  if (declared && declared->type == VKR_BAKERY_JSON_ARRAY) {
    VkrBakeryJson *remaining = vkr_bakery_json_array(arena);
    for (const VkrBakeryJson *name = declared->first; name; name = name->next) {
      bool8_t texture_only_dropped = false_v;
      if (vkr_bakery_json_is_string(name, "MSFT_texture_dds") ||
          vkr_bakery_json_is_string(name, "KHR_texture_basisu") ||
          vkr_bakery_json_is_string(name, "EXT_texture_webp") ||
          vkr_bakery_json_is_string(name, "EXT_texture_avif")) {
        texture_only_dropped = true_v;
        for (const VkrBakeryJson *used = used_names->first; used;
             used = used->next) {
          if (vkr_bakery_json_equal(used, name)) {
            texture_only_dropped = false_v;
          }
        }
      }
      if (!texture_only_dropped) {
        vkr_bakery_json_append(remaining, vkr_bakery_json_clone(arena, name));
      }
    }
    vkr_bakery_json_set(arena, model, "extensionsUsed", remaining);
  }
  vkr_bakery_json_set(arena, model, "images", pruned);
  return true_v;
}

vkr_internal bool8_t vkr_project_snapshot_dependency(
    VkrProjectSnapshot *snapshot, const char *value, const char *origin,
    bool8_t texture, char *out) {
  VkrProjectJob *job = snapshot->job;
  Arena *arena = job->arena;
  char located[VKR_PROJECT_PATH];
  if (texture) {
    vkr_project_gltf_texture_source(value, origin, job->legacy_root, located);
  } else {
    (void)vkr_bakery_path_join(located, sizeof(located), origin, value);
  }
  char suffix[32];
  vkr_project_suffix_lower(value, suffix, sizeof(suffix));
  bool8_t placeholders = false_v;
  (void)vkr_bakery_json_get_bool(job->request, "use_placeholders",
                                 &placeholders);
  if (placeholders && !vkr_bakery_is_file(located) &&
      vkr_project_is_image_suffix(suffix)) {
    (void)snprintf(out, VKR_PROJECT_PATH, "%s/dependencies/placeholder-%u.png",
                   snapshot->directory, snapshot->dependencies->count);
    const char *reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    VKR_PROJECT_TRY(vkr_project_write_text(
        job, out, (const char *)vkr_project_placeholder_png,
        sizeof(vkr_project_placeholder_png)));
    VKR_PROJECT_TRY(vkr_project_managed_reference(job, out, snapshot->directory,
                                                  &reference));
    VKR_PROJECT_TRY(vkr_project_digest(job, out, hash));
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "path",
                        vkr_bakery_json_cstr(arena, reference));
    vkr_bakery_json_set(arena, entry, "sha256",
                        vkr_bakery_json_cstr(arena, hash));
    vkr_bakery_json_set(
        arena, entry, "bytes",
        vkr_project_int(job, (int64_t)sizeof(vkr_project_placeholder_png)));
    vkr_bakery_json_set(arena, entry, "placeholder_for",
                        vkr_bakery_json_cstr(arena, value));
    vkr_bakery_json_append(snapshot->dependencies, entry);
    vkr_project_warn(job, "Missing image %s was replaced by a placeholder",
                     value);
    return true_v;
  }
  char resolved[VKR_PROJECT_PATH];
  char dependencies[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_source_file(job, located, resolved));
  (void)snprintf(dependencies, sizeof(dependencies), "%s/dependencies",
                 snapshot->directory);
  VKR_PROJECT_TRY(vkr_project_copy_blob(job, resolved, dependencies, out));
  const char *reference = NULL;
  char hash[VKR_BAKERY_SHA256_HEX];
  uint64_t size = 0u;
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, out, snapshot->directory, &reference));
  VKR_PROJECT_TRY(vkr_project_digest(job, out, hash));
  (void)vkr_project_file_size(out, &size);
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entry, "path",
                      vkr_bakery_json_cstr(arena, reference));
  vkr_bakery_json_set(arena, entry, "sha256",
                      vkr_bakery_json_cstr(arena, hash));
  vkr_bakery_json_set(arena, entry, "bytes",
                      vkr_project_int(job, (int64_t)size));
  vkr_bakery_json_append(snapshot->dependencies, entry);
  return true_v;
}

vkr_internal bool8_t vkr_project_snapshot_gltf(VkrProjectSnapshot *snapshot,
                                               const char *source,
                                               const char *destination,
                                               bool8_t glb,
                                               VkrBakeryJson *material_names) {
  VkrProjectJob *job = snapshot->job;
  Arena *arena = job->arena;
  VkrBakeryJson *model = NULL;
  uint8_t *data = NULL;
  uint64_t length = 0u;
  typedef struct VkrProjectChunk {
    uint32_t kind;
    const uint8_t *data;
    uint32_t length;
  } VkrProjectChunk;
  VkrProjectChunk chunks[16];
  uint32_t chunk_count = 0u;
  if (glb) {
    uint64_t size = 0u;
    (void)vkr_project_file_size(source, &size);
    if (size > VKR_PROJECT_MAX_IMPORT_BYTES) {
      return vkr_project_fail(job, "GLB exceeds import size limit");
    }
    if (!vkr_bakery_read_file(source, 0u, &data, &length)) {
      return vkr_project_fail(job, "Cannot read %s", source);
    }
    uint32_t header[3] = {0u, 0u, 0u};
    if (length >= 12u) {
      MemCopy(header, data, 12u);
    }
    if (length < 20u || header[0] != 0x46546C67u || header[1] != 2u ||
        header[2] != length) {
      free(data);
      return vkr_project_fail(job, "Invalid GLB header");
    }
    uint64_t offset = 12u;
    while (offset < length) {
      if (offset + 8u > length) {
        free(data);
        return vkr_project_fail(job, "Truncated GLB chunk");
      }
      uint32_t chunk_length = 0u;
      uint32_t kind = 0u;
      MemCopy(&chunk_length, data + offset, 4u);
      MemCopy(&kind, data + offset + 4u, 4u);
      offset += 8u;
      if (chunk_length > length - offset) {
        free(data);
        return vkr_project_fail(job, "Truncated GLB payload");
      }
      if (chunk_count == ArrayCount(chunks)) {
        free(data);
        return vkr_project_fail(job, "GLB has too many chunks");
      }
      chunks[chunk_count++] =
          (VkrProjectChunk){kind, data + offset, chunk_length};
      offset += chunk_length;
    }
    if (!chunk_count || chunks[0].kind != 0x4E4F534Au) {
      free(data);
      return vkr_project_fail(job, "GLB has no JSON chunk");
    }
    VkrBakeryJsonError error;
    model = vkr_bakery_json_parse(arena, chunks[0].data, chunks[0].length,
                                  1024u, &error);
    if (!model) {
      free(data);
      return vkr_project_fail(job, "Invalid GLB JSON: %s", error.message);
    }
  } else {
    model = vkr_project_load_json(job, source, VKR_PROJECT_MAX_MODEL_BYTES);
    VKR_PROJECT_TRY(model);
  }
  const VkrBakeryJson *materials = vkr_bakery_json_get(model, "materials");
  uint32_t index = 0u;
  for (const VkrBakeryJson *item = materials ? materials->first : NULL; item;
       item = item->next, ++index) {
    const char *name = vkr_project_json_text(item, "name");
    vkr_bakery_json_append(
        material_names,
        vkr_bakery_json_cstr(
            arena, name && name[0]
                       ? name
                       : vkr_project_printf(job, "Material %u", index + 1u)));
  }
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), source);
  static const char *const fields[] = {"buffers", "images"};
  if (!vkr_project_prune_unsampled_images(job, model)) {
    return false_v;
  }
  /* A first import hashes every source; hash them together before the
     copies below name each by its digest. */
  VkrProjectStrings sources = {0};
  for (uint32_t f = 0u; f < ArrayCount(fields); ++f) {
    VkrBakeryJson *items = vkr_bakery_json_get(model, fields[f]);
    for (VkrBakeryJson *item = items ? items->first : NULL; item;
         item = item->next) {
      const char *uri = vkr_project_json_text(item, "uri");
      char path[VKR_PROJECT_PATH];
      char located[VKR_PROJECT_PATH];
      char resolved[VKR_PROJECT_PATH];
      if (!uri || !uri[0] || strncmp(uri, "data:", 5u) == 0 ||
          !vkr_project_gltf_uri_path(job, uri, path, sizeof(path))) {
        continue;
      }
      if (f == 1u) {
        vkr_project_gltf_texture_source(path, origin, job->legacy_root,
                                        located);
      } else {
        (void)vkr_bakery_path_join(located, sizeof(located), origin, path);
      }
      if (vkr_project_resolve(located, true_v, resolved, sizeof(resolved)) &&
          !vkr_project_strings_push(job, &sources,
                                    vkr_project_strdup(job, resolved))) {
        return false_v;
      }
    }
  }
  vkr_bakery_index_prefetch(vkr_project_index(job),
                            (const char *const *)sources.items, sources.count);
  /* Copying, flushing and scanning each new file one at a time dominated a
     Windows import; place the blobs the loop below names in parallel. */
  char dependencies[VKR_PROJECT_PATH];
  (void)snprintf(dependencies, sizeof(dependencies), "%s/dependencies",
                 snapshot->directory);
  VKR_PROJECT_TRY(vkr_project_prefetch_blobs(
      job, (const char *const *)sources.items, sources.count, dependencies));
  bool8_t ok = true_v;
  for (uint32_t f = 0u; ok && f < ArrayCount(fields); ++f) {
    VkrBakeryJson *items = vkr_bakery_json_get(model, fields[f]);
    for (VkrBakeryJson *item = items ? items->first : NULL; ok && item;
         item = item->next) {
      const char *uri = vkr_project_json_text(item, "uri");
      if (!uri || !uri[0] || strncmp(uri, "data:", 5u) == 0) {
        continue;
      }
      char path[VKR_PROJECT_PATH];
      char copied[VKR_PROJECT_PATH];
      const char *reference = NULL;
      ok = vkr_project_gltf_uri_path(job, uri, path, sizeof(path)) &&
           vkr_project_snapshot_dependency(snapshot, path, origin, f == 1u,
                                           copied) &&
           vkr_project_managed_reference(job, copied, snapshot->directory,
                                         &reference);
      if (ok) {
        vkr_bakery_json_set(arena, item, "uri",
                            vkr_bakery_json_cstr(arena, reference));
      }
    }
  }
  if (ok && !glb) {
    ok = vkr_project_atomic_json(job, destination, model);
  } else if (ok) {
    String8 text = {0};
    ok = vkr_bakery_json_write(arena, model, VKR_BAKERY_JSON_PYTHON_COMPACT,
                               &text);
    VkrBakeryBuffer output = {0};
    if (ok) {
      const uint32_t padded = (uint32_t)((text.length + 3u) & ~3ull);
      uint32_t total = 12u + 8u + padded;
      for (uint32_t c = 1u; c < chunk_count; ++c) {
        total += 8u + chunks[c].length;
      }
      const uint32_t header[3] = {0x46546C67u, 2u, total};
      vkr_bakery_buffer_append(&output, header, sizeof(header));
      const uint32_t json_header[2] = {padded, 0x4E4F534Au};
      vkr_bakery_buffer_append(&output, json_header, sizeof(json_header));
      vkr_bakery_buffer_append(&output, text.str, text.length);
      for (uint64_t pad = text.length; pad < padded; ++pad) {
        vkr_bakery_buffer_append(&output, " ", 1u);
      }
      for (uint32_t c = 1u; c < chunk_count; ++c) {
        const uint32_t chunk_header[2] = {chunks[c].length, chunks[c].kind};
        vkr_bakery_buffer_append(&output, chunk_header, sizeof(chunk_header));
        vkr_bakery_buffer_append(&output, chunks[c].data, chunks[c].length);
      }
      ok = !output.failed &&
           vkr_project_write_text(job, destination, (const char *)output.data,
                                  output.length);
    }
    vkr_bakery_buffer_free(&output);
  }
  free(data);
  return ok;
}

vkr_internal bool8_t vkr_project_snapshot_obj(VkrProjectSnapshot *snapshot,
                                              const char *source,
                                              const char *destination,
                                              VkrBakeryJson *material_names) {
  VkrProjectJob *job = snapshot->job;
  Arena *arena = job->arena;
  const char *text = NULL;
  VKR_PROJECT_TRY(vkr_project_read_text(
      job, source, VKR_PROJECT_MAX_MODEL_BYTES, &text, NULL));
  if ((uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB &&
      (uint8_t)text[2] == 0xBF) {
    text += 3;
  }
  VkrProjectStrings lines;
  VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
  VkrProjectStrings output = {0};
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), source);
  for (uint32_t i = 0u; i < lines.count; ++i) {
    VkrProjectStrings tokens;
    VKR_PROJECT_TRY(vkr_project_model_tokens(job, lines.items[i], &tokens));
    if (!tokens.count || strcmp(tokens.items[0], "mtllib") != 0) {
      VKR_PROJECT_TRY(vkr_project_strings_push(job, &output, lines.items[i]));
      continue;
    }
    if (tokens.count < 2u) {
      return vkr_project_fail(job, "OBJ mtllib has no filename");
    }
    for (uint32_t l = 1u; l < tokens.count; ++l) {
      char library_path[VKR_PROJECT_PATH];
      char mtl_source[VKR_PROJECT_PATH];
      (void)vkr_bakery_path_join(library_path, sizeof(library_path), origin,
                                 tokens.items[l]);
      VKR_PROJECT_TRY(vkr_project_source_file(job, library_path, mtl_source));
      char hash[VKR_BAKERY_SHA256_HEX];
      VKR_PROJECT_TRY(vkr_project_digest(job, mtl_source, hash));
      char mtl[VKR_PROJECT_PATH];
      (void)snprintf(mtl, sizeof(mtl), "%s/material_%s.mtl",
                     snapshot->directory, hash);
      const char *mtl_text = NULL;
      VKR_PROJECT_TRY(vkr_project_read_text(
          job, mtl_source, VKR_PROJECT_MAX_MODEL_BYTES, &mtl_text, NULL));
      if ((uint8_t)mtl_text[0] == 0xEF && (uint8_t)mtl_text[1] == 0xBB &&
          (uint8_t)mtl_text[2] == 0xBF) {
        mtl_text += 3;
      }
      VkrProjectStrings material_lines;
      VKR_PROJECT_TRY(vkr_project_split_lines(job, mtl_text, &material_lines));
      VkrProjectStrings rewritten = {0};
      char mtl_origin[VKR_PROJECT_PATH];
      vkr_bakery_path_parent(mtl_origin, sizeof(mtl_origin), mtl_source);
      for (uint32_t m = 0u; m < material_lines.count; ++m) {
        const char *line = material_lines.items[m];
        VkrProjectStrings fields;
        VKR_PROJECT_TRY(vkr_project_model_tokens(job, line, &fields));
        if (fields.count && strcmp(fields.items[0], "newmtl") == 0) {
          VkrBakeryBuffer joined = {0};
          for (uint32_t n = 1u; n < fields.count; ++n) {
            if (n > 1u) {
              vkr_bakery_buffer_append(&joined, " ", 1u);
            }
            vkr_bakery_buffer_append_cstr(&joined, fields.items[n]);
          }
          vkr_bakery_json_append(
              material_names,
              vkr_bakery_json_cstr(
                  arena, joined.data ? (const char *)joined.data : ""));
          vkr_bakery_buffer_free(&joined);
        }
        if (fields.count && (strncmp(fields.items[0], "map_", 4u) == 0 ||
                             !strcmp(fields.items[0], "bump") ||
                             !strcmp(fields.items[0], "disp") ||
                             !strcmp(fields.items[0], "decal") ||
                             !strcmp(fields.items[0], "norm"))) {
          if (fields.count != 2u) {
            return vkr_project_fail(
                job, "MTL texture options need explicit conversion: %s",
                fields.items[0]);
          }
          char copied[VKR_PROJECT_PATH];
          const char *reference = NULL;
          VKR_PROJECT_TRY(vkr_project_snapshot_dependency(
              snapshot, fields.items[1], mtl_origin, false_v, copied));
          if (strcmp(fields.items[0], "map_Kd") &&
              strcmp(fields.items[0], "map_Ks") &&
              strcmp(fields.items[0], "bump") &&
              strcmp(fields.items[0], "map_bump")) {
            vkr_project_warn(
                job,
                "OBJ channel %s is retained in the source snapshot "
                "but is not rendered",
                fields.items[0]);
          }
          VKR_PROJECT_TRY(vkr_project_managed_reference(
              job, copied, snapshot->directory, &reference));
          line = vkr_project_printf(job, "%s %s", fields.items[0], reference);
        }
        VKR_PROJECT_TRY(vkr_project_strings_push(job, &rewritten, line));
      }
      VKR_PROJECT_TRY(vkr_project_write_lines(job, mtl, &rewritten));
      char mtl_hash[VKR_BAKERY_SHA256_HEX];
      uint64_t size = 0u;
      VKR_PROJECT_TRY(vkr_project_digest(job, mtl, mtl_hash));
      (void)vkr_project_file_size(mtl, &size);
      VkrBakeryJson *entry = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(
          arena, entry, "path",
          vkr_bakery_json_cstr(arena, vkr_bakery_path_name(mtl)));
      vkr_bakery_json_set(arena, entry, "sha256",
                          vkr_bakery_json_cstr(arena, mtl_hash));
      vkr_bakery_json_set(arena, entry, "bytes",
                          vkr_project_int(job, (int64_t)size));
      vkr_bakery_json_append(snapshot->dependencies, entry);
      VKR_PROJECT_TRY(vkr_project_strings_push(
          job, &output,
          vkr_project_printf(job, "mtllib %s", vkr_bakery_path_name(mtl))));
    }
  }
  return vkr_project_write_lines(job, destination, &output);
}

bool8_t vkr_project_snapshot_model(VkrProjectJob *job, const char *source,
                                   const char *import_id, char *out) {
  Arena *arena = job->arena;
  char resolved[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_source_file(job, source, resolved));
  uint64_t size = 0u;
  (void)vkr_project_file_size(resolved, &size);
  if (size > VKR_PROJECT_MAX_MODEL_BYTES) {
    return vkr_project_fail(job,
                            "Model source exceeds the 256 MiB parser budget");
  }
  char original[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_digest(job, resolved, original));
  VkrProjectSnapshot snapshot = {.job = job};
  (void)snprintf(snapshot.directory, sizeof(snapshot.directory),
                 "%s/sources/%s", job->stage, import_id);
  if (vkr_project_exists(snapshot.directory)) {
    return vkr_project_fail(job, "[Errno 17] File exists: '%s'",
                            snapshot.directory);
  }
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, snapshot.directory));
  snapshot.dependencies = vkr_bakery_json_array(arena);
  char suffix[32];
  vkr_project_suffix_lower(resolved, suffix, sizeof(suffix));
  (void)snprintf(out, VKR_PROJECT_PATH, "%s/source%s", snapshot.directory,
                 suffix);
  VkrBakeryJson *material_names = vkr_bakery_json_array(arena);
  if (!strcmp(suffix, ".gltf") || !strcmp(suffix, ".glb")) {
    VKR_PROJECT_TRY(vkr_project_snapshot_gltf(
        &snapshot, resolved, out, !strcmp(suffix, ".glb"), material_names));
  } else if (!strcmp(suffix, ".obj")) {
    VKR_PROJECT_TRY(
        vkr_project_snapshot_obj(&snapshot, resolved, out, material_names));
  } else {
    return vkr_project_fail(job, "Models must be GLTF, GLB or OBJ");
  }
  char after[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_digest(job, resolved, after));
  if (strcmp(after, original) != 0) {
    return vkr_project_fail(job, "Model source changed while collecting "
                                 "dependencies; retry import");
  }
  char hash[VKR_BAKERY_SHA256_HEX];
  uint64_t out_size = 0u;
  VKR_PROJECT_TRY(vkr_project_digest(job, out, hash));
  (void)vkr_project_file_size(out, &out_size);
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entry, "path",
                      vkr_bakery_json_cstr(arena, vkr_bakery_path_name(out)));
  vkr_bakery_json_set(arena, entry, "sha256",
                      vkr_bakery_json_cstr(arena, hash));
  vkr_bakery_json_set(arena, entry, "bytes",
                      vkr_project_int(job, (int64_t)out_size));
  vkr_bakery_json_append(snapshot.dependencies, entry);
  const char *reference = NULL;
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, out, job->stage, &reference));
  VkrBakeryJson *manifest = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, manifest, "version", vkr_project_int(job, 1));
  vkr_bakery_json_set(arena, manifest, "id",
                      vkr_bakery_json_cstr(arena, import_id));
  vkr_bakery_json_set(arena, manifest, "source",
                      vkr_bakery_json_cstr(arena, reference));
  vkr_bakery_json_set(arena, manifest, "original_sha256",
                      vkr_bakery_json_cstr(arena, original));
  vkr_bakery_json_set(arena, manifest, "material_names", material_names);
  vkr_bakery_json_set(arena, manifest, "dependencies", snapshot.dependencies);
  vkr_bakery_json_set(arena, manifest, "reimport_status",
                      vkr_bakery_json_cstr(arena, "source_snapshot"));
  vkr_bakery_json_set(arena, manifest, "unsupported_features",
                      vkr_bakery_json_clone(arena, job->warnings));
  vkr_bakery_json_set(arena, job->sources, import_id, manifest);
  return true_v;
}

// =============================================================================
// Models, animations, materials and textures
// =============================================================================

bool8_t vkr_project_cook_model_animation(VkrProjectJob *job,
                                         VkrBakeryJson *record,
                                         const char *source, const char *mesh) {
  VkrBakeryJson *info = vkr_project_inspect_mesh(job, mesh);
  VKR_PROJECT_TRY(info);
  int64_t skins = 0;
  int64_t animations = 0;
  (void)vkr_bakery_json_get_int(info, "skin_count", &skins);
  (void)vkr_bakery_json_get_int(info, "animation_count", &animations);
  if (!skins || !animations) {
    return true_v;
  }
  char bank[VKR_PROJECT_PATH];
  char directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), mesh);
  (void)vkr_bakery_path_join(bank, sizeof(bank), directory, "animation.vka");
  const char *arguments[] = {"--input", source, "--output", bank};
  VKR_PROJECT_TRY(vkr_project_run_tool(job, "animation", arguments,
                                       ArrayCount(arguments),
                                       "Cooking animations", 0, NULL));
  if (!vkr_bakery_is_file(bank)) {
    return vkr_project_fail(job, "Animation cooker did not publish its bank");
  }
  Arena *arena = job->arena;
  const char *reference = NULL;
  char hash[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, bank, job->stage, &reference));
  VKR_PROJECT_TRY(vkr_project_digest(job, bank, hash));
  VkrBakeryJson *product = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, product, "role",
                      vkr_bakery_json_cstr(arena, "animation"));
  vkr_bakery_json_set(arena, product, "path",
                      vkr_bakery_json_cstr(arena, reference));
  vkr_bakery_json_set(arena, product, "version", vkr_project_int(job, 1));
  vkr_bakery_json_set(
      arena, product, "fingerprint",
      vkr_bakery_json_cstr(arena, vkr_project_printf(job, "sha256:%s", hash)));
  vkr_bakery_json_append(vkr_bakery_json_get(record, "artifacts"), product);
  vkr_bakery_json_set(arena, record, "animation_count",
                      vkr_project_int(job, animations));
  return true_v;
}

VkrBakeryJson *vkr_project_import_model(VkrProjectJob *job,
                                        const char *source) {
  Arena *arena = job->arena;
  char import_id[37];
  vkr_project_uuid4(import_id);
  char snapshot[VKR_PROJECT_PATH];
  if (!vkr_project_snapshot_model(job, source, import_id, snapshot)) {
    return NULL;
  }
  char bundle[VKR_PROJECT_PATH];
  char mesh[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage, import_id);
  (void)snprintf(mesh, sizeof(mesh), "%s/mesh.vkb", bundle);
  if (!vkr_project_make_dirs(job, bundle)) {
    return NULL;
  }
  char stem[512];
  vkr_project_stem(source, stem, sizeof(stem));
  bool8_t prepare = true_v;
  const VkrBakeryJson *bakes = vkr_bakery_json_get(job->request, "bakes");
  if (vkr_bakery_json_get_bool(bakes, "prepare_assets", &prepare) && !prepare) {
    char asset_id[37];
    vkr_project_uuid4(asset_id);
    const char *reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    if (!vkr_project_managed_reference(job, snapshot, job->stage, &reference) ||
        !vkr_project_digest(job, snapshot, hash)) {
      return NULL;
    }
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, record, "id",
                        vkr_bakery_json_cstr(arena, asset_id));
    vkr_bakery_json_set(arena, record, "kind",
                        vkr_bakery_json_cstr(arena, "mesh"));
    vkr_bakery_json_set(arena, record, "name",
                        vkr_bakery_json_cstr(arena, stem));
    vkr_bakery_json_set(arena, record, "import_id",
                        vkr_bakery_json_cstr(arena, import_id));
    vkr_bakery_json_set(arena, record, "source",
                        vkr_bakery_json_cstr(arena, reference));
    vkr_bakery_json_set(arena, record, "artifacts",
                        vkr_bakery_json_array(arena));
    vkr_project_set_mesh_recipe(job, record,
                                vkr_project_requested_lightmap_density(job));
    vkr_bakery_json_set(arena, record, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
    vkr_bakery_json_append(job->assets, record);
    char manifest_path[VKR_PROJECT_PATH];
    (void)snprintf(manifest_path, sizeof(manifest_path), "%s/imports/%s.json",
                   job->stage, import_id);
    if (!vkr_project_atomic_json(
            job, manifest_path, vkr_bakery_json_get(job->sources, import_id))) {
      return NULL;
    }
    return vkr_project_reference(job, job->asset_scope, asset_id, "mesh");
  }
  const float32_t lightmap_density =
      vkr_project_requested_lightmap_density(job);
  if (!vkr_project_cook_mesh(job, snapshot, mesh, bundle, import_id,
                             lightmap_density, "Cooking model")) {
    return NULL;
  }
  VkrBakeryJson *reference = vkr_project_artifact(job, "mesh", stem, mesh, NULL,
                                                  import_id, snapshot, NULL);
  if (reference) {
    vkr_project_mark_tier(job, job->assets->last);
    vkr_project_set_mesh_recipe(job, job->assets->last, lightmap_density);
  }
  if (!reference ||
      !vkr_project_cook_model_animation(job, job->assets->last, snapshot,
                                        mesh) ||
      !vkr_project_index_bundle(job, bundle, import_id)) {
    return NULL;
  }
  VkrBakeryJson *manifest = vkr_bakery_json_get(job->sources, import_id);
  VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
  for (VkrBakeryJson *record = job->assets->first; record;
       record = record->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(record, "import_id"),
                                  import_id)) {
      vkr_bakery_json_append(artifacts, vkr_bakery_json_clone(arena, record));
    }
  }
  vkr_bakery_json_set(arena, manifest, "artifacts", artifacts);
  char manifest_path[VKR_PROJECT_PATH];
  (void)snprintf(manifest_path, sizeof(manifest_path), "%s/imports/%s.json",
                 job->stage, import_id);
  if (!vkr_project_atomic_json(job, manifest_path, manifest)) {
    return NULL;
  }
  return reference;
}

/* One material texture line: its class and resolved source. */
typedef struct VkrProjectTextureLine {
  const char *key;
  const char *query;
  bool8_t has_query;
  char source[VKR_PROJECT_PATH];
  char hash[VKR_BAKERY_SHA256_HEX];
  char texture_class[16];
  char destination[VKR_PROJECT_PATH];
} VkrProjectTextureLine;

vkr_internal bool8_t vkr_project_query_value(const char *query,
                                             const char *name, char *out,
                                             uint32_t capacity) {
  /* parse_qs keeps the first value of a repeated name. */
  const uint64_t length = strlen(name);
  for (const char *cursor = query; cursor && *cursor;) {
    const char *end = strpbrk(cursor, "&;");
    const char *equals = strchr(cursor, '=');
    if (equals && (!end || equals < end) &&
        (uint64_t)(equals - cursor) == length &&
        strncmp(cursor, name, length) == 0) {
      const char *value = equals + 1;
      const uint64_t value_length =
          end ? (uint64_t)(end - value) : strlen(value);
      if (value_length) {
        (void)snprintf(out, capacity, "%.*s", (int)value_length, value);
        return true_v;
      }
    }
    cursor = end ? end + 1 : NULL;
  }
  return false_v;
}

vkr_internal bool8_t vkr_project_texture_line(VkrProjectJob *job,
                                              const char *material,
                                              const char *key,
                                              const char *value,
                                              VkrProjectTextureLine *out) {
  MemZero(out, sizeof(*out));
  out->key = key;
  char raw[VKR_PROJECT_PATH];
  (void)snprintf(raw, sizeof(raw), "%s", value);
  char *marker = strchr(raw, '?');
  if (marker) {
    *marker = 0;
    out->has_query = true_v;
    out->query = vkr_project_strdup(job, marker + 1);
  }
  char directory[VKR_PROJECT_PATH];
  char joined[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), material);
  (void)vkr_bakery_path_join(joined, sizeof(joined), directory, raw);
  if (!vkr_project_resolve(joined, true_v, out->source, sizeof(out->source))) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            joined);
  }
  char texture_class[64] = {0};
  const char *query = out->query ? out->query : "";
  if (!vkr_project_query_value(query, "tc", texture_class,
                               sizeof(texture_class)) &&
      !vkr_project_query_value(query, "class", texture_class,
                               sizeof(texture_class))) {
    char color_space[16] = {0};
    const bool8_t srgb = vkr_project_query_value(query, "cs", color_space,
                                                 sizeof(color_space)) &&
                         strcmp(color_space, "srgb") == 0;
    (void)snprintf(texture_class, sizeof(texture_class), "%s",
                   strstr(key, "normal") ? "normal_rg"
                   : (!strcmp(key, "diffuse_texture") ||
                      !strcmp(key, "base_color_texture") ||
                      !strcmp(key, "emissive_texture") || srgb)
                       ? "color_srgb"
                       : "data_mask");
  }
  for (char *c = texture_class; *c; ++c) {
    if (*c == '_') {
      *c = '-';
    }
  }
  if (strcmp(texture_class, "normal-rg") &&
      strcmp(texture_class, "data-mask") &&
      strcmp(texture_class, "color-srgb") &&
      strcmp(texture_class, "color-linear")) {
    return vkr_project_fail(job, "Unsupported material texture class");
  }
  (void)snprintf(out->texture_class, sizeof(out->texture_class), "%s",
                 texture_class);
  VKR_PROJECT_TRY(vkr_project_digest(job, out->source, out->hash));
  char source_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(source_directory, sizeof(source_directory),
                         out->source);
  (void)snprintf(out->destination, sizeof(out->destination), "%s/%s-%s%s.vkt",
                 source_directory, out->hash, out->texture_class,
                 vkr_project_texture_suffix(job));
  return true_v;
}

bool8_t vkr_project_pack_bundle_textures(VkrProjectJob *job,
                                         const VkrProjectStrings *materials) {
  /* Every texture a material still names by source image becomes one
     cached texture action; they run in parallel under the memory budget,
     then each material line points at its packed .vkt. */
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, job->config)) {
    return vkr_project_fail(job, "Cannot start the texture graph");
  }
  vkr_bakery_graph_use_index(&graph, vkr_project_index(job));
  bool8_t ok = true_v;
  uint32_t pending = 0u;
  VkrBakeryJson *planned = vkr_bakery_json_object(job->arena);
  for (uint32_t m = 0u; ok && m < materials->count; ++m) {
    const char *text = NULL;
    VkrProjectStrings lines;
    ok = vkr_project_read_text(job, materials->items[m],
                               VKR_PROJECT_MAX_JSON_BYTES, &text, NULL) &&
         vkr_project_split_lines(job, text, &lines);
    for (uint32_t i = 0u; ok && i < lines.count; ++i) {
      char key[256];
      char value[VKR_PROJECT_PATH];
      const uint64_t key_length =
          vkr_project_partition_line(lines.items[i], key, sizeof(key), value,
                                     sizeof(value))
              ? strlen(key)
              : 0u;
      if (key_length < 8u || strcmp(key + key_length - 8u, "_texture") != 0 ||
          !value[0]) {
        continue;
      }
      VkrProjectTextureLine texture;
      char raw_suffix[32];
      char raw[VKR_PROJECT_PATH];
      (void)snprintf(raw, sizeof(raw), "%s", value);
      char *query = strchr(raw, '?');
      if (query) {
        *query = 0;
      }
      vkr_project_suffix_lower(raw, raw_suffix, sizeof(raw_suffix));
      if (!strcmp(raw_suffix, ".vkt")) {
        continue;
      }
      ok = vkr_project_texture_line(job, materials->items[m], key, value,
                                    &texture);
      if (!ok || vkr_bakery_is_file(texture.destination) ||
          vkr_bakery_json_get(planned, texture.destination)) {
        continue;
      }
      vkr_bakery_json_set(job->arena, planned, texture.destination,
                          vkr_bakery_json_bool(job->arena, true_v));
      VkrBakeryJson *recipe = vkr_bakery_json_object(graph.arena);
      vkr_bakery_json_set(
          graph.arena, recipe, "class",
          vkr_bakery_json_cstr(graph.arena, texture.texture_class));
      vkr_bakery_json_set(graph.arena, recipe, "tier",
                          vkr_bakery_json_cstr(graph.arena, job->texture_preview
                                                                ? "preview"
                                                                : "final"));
      vkr_bakery_json_set(
          graph.arena, recipe, "encoding",
          vkr_bakery_json_cstr(graph.arena,
                               vkr_project_texture_encoding_name(job)));
      VkrBakeryAction *action =
          vkr_bakery_graph_add(&graph, vkr_bakery_producer_find("texture"),
                               texture.source, recipe, texture.destination);
      if (!action) {
        ok = vkr_project_fail(job,
                              "Texture packer did not publish its artifact");
        break;
      }
      const char *seed =
          vkr_project_json_text(job->texture_seeds, texture.hash);
      action->seed_path = seed ? vkr_bakery_graph_strdup(&graph, seed) : NULL;
      pending += 1u;
    }
  }
  if (ok && pending) {
    ok = vkr_project_progress(job, "Preparing texture", -1.0,
                              vkr_project_printf(job, "%u textures", pending));
    if (ok && !vkr_bakery_cache_prepare(job->config)) {
      ok = vkr_project_fail(job, "The bakery cache is not writable");
    }
    if (ok && !vkr_bakery_graph_execute(&graph)) {
      ok = vkr_project_check_cancel(job) &&
           vkr_project_fail(job, "Preparing texture failed; see the job log");
    }
  }
  vkr_bakery_graph_shutdown(&graph);
  return ok;
}

/* Records in the index the digests the cook reported for files of
   `bundle` (vkr_mesh_cook_set_digest_log), each for the file as it stands
   when its size still matches: the revision's textures are then not hashed
   again. Malformed lines are skipped; those files are hashed as before. */
vkr_internal void vkr_project_adopt_digests(VkrProjectJob *job,
                                            const char *bundle,
                                            const char *digests) {
  VkrBakeryIndex *index = vkr_project_index(job);
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!index || !vkr_bakery_read_file(digests, MB(64), &data, &length)) {
    return;
  }
  uint64_t start = 0u;
  while (start < length) {
    uint64_t end = start;
    while (end < length && data[end] != '\n') {
      ++end;
    }
    char line[VKR_PROJECT_PATH + 96];
    const uint64_t line_length = end - start;
    if (line_length < sizeof(line)) {
      MemCopy(line, data + start, line_length);
      line[line_length] = 0;
      char hash[VKR_BAKERY_SHA256_HEX];
      unsigned long long size = 0u;
      int consumed = 0;
      bool8_t valid =
          sscanf(line, "%64s %llu %n", hash, &size, &consumed) == 2 &&
          strlen(hash) == 64u && consumed > 0;
      for (uint32_t c = 0u; valid && c < 64u; ++c) {
        valid = (hash[c] >= '0' && hash[c] <= '9') ||
                (hash[c] >= 'a' && hash[c] <= 'f');
      }
      const char *relative = line + consumed;
      valid = valid && relative[0] && relative[0] != '/' &&
              !strchr(relative, ':') && !strstr(relative, "..");
      char path[VKR_PROJECT_PATH];
      VkrBakeryStat info;
      if (valid && vkr_bakery_path_join(path, sizeof(path), bundle, relative) &&
          vkr_bakery_stat(path, &info) && info.exists && !info.is_directory &&
          info.size == size) {
        vkr_bakery_index_record(index, path, hash);
      }
    }
    start = end + 1u;
  }
  free(data);
}

/* Cooks one model into `bundle` through `tool mesh` at the job's tier. */
float32_t vkr_project_requested_lightmap_density(const VkrProjectJob *job) {
  float64_t density = 0.0;
  if (!vkr_bakery_json_get_number(
          vkr_bakery_json_get(job->request, "model_settings"),
          "lightmap_texels_per_unit", &density) ||
      !isfinite(density) || density <= 0.0 || density > 1024.0) {
    return 0.0f;
  }
  return (float32_t)density;
}

float32_t vkr_project_record_lightmap_density(const VkrBakeryJson *record) {
  float64_t density = 0.0;
  if (!vkr_bakery_json_get_number(
          vkr_bakery_json_get(vkr_bakery_json_get(record, "recipe"),
                              "settings"),
          "lightmap_texels_per_unit", &density) ||
      !isfinite(density) || density <= 0.0 || density > 1024.0) {
    return 0.0f;
  }
  return (float32_t)density;
}

void vkr_project_set_mesh_recipe(VkrProjectJob *job, VkrBakeryJson *record,
                                 float32_t lightmap_texels_per_unit) {
  Arena *arena = job->arena;
  VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, recipe, "tool",
                      vkr_bakery_json_cstr(arena, "mesh"));
  vkr_bakery_json_set(arena, recipe, "version", vkr_project_int(job, 1));
  if (lightmap_texels_per_unit > 0.0f) {
    VkrBakeryJson *settings = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, settings, "lightmap_texels_per_unit",
        vkr_bakery_json_float(arena, (float64_t)lightmap_texels_per_unit));
    vkr_bakery_json_set(arena, recipe, "settings", settings);
  }
  vkr_bakery_json_set(arena, record, "recipe", recipe);
}

bool8_t vkr_project_cook_mesh(VkrProjectJob *job, const char *source,
                              const char *output, const char *bundle,
                              const char *import_id,
                              float32_t lightmap_texels_per_unit,
                              const char *label) {
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, job->generated_root));
  /* The workspace publishes every file by rename, so the cook may hard
     link its files into the bundle where the volume cannot clone them. */
  const char *arguments[28] = {"--input",          source,
                               "--output",         output,
                               "--bundle-root",    bundle,
                               "--import-id",      import_id,
                               "--generated-root", job->generated_root,
                               "--link-root",      job->workspace};
  uint32_t count = 12u + vkr_project_tier_arguments(job, arguments + 12u);
  /* The cook reports its artifact as `--inspect` would, sparing the
     validation that follows an inspection process. */
  char directory[VKR_PROJECT_PATH];
  char id[37];
  (void)snprintf(directory, sizeof(directory), "%s/jobs/inspection",
                 job->workspace);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, directory));
  vkr_project_uuid4(id);
  const char *report = vkr_project_printf(job, "%s/%s.json", directory, id);
  arguments[count++] = "--inspect-output";
  arguments[count++] = report;
  const char *digests = vkr_project_printf(job, "%s/%s.digests", directory, id);
  arguments[count++] = "--digest-output";
  arguments[count++] = digests;
  if (job->ready_log) {
    arguments[count++] = "--ready-log";
    arguments[count++] = job->ready_log;
  }
  if (lightmap_texels_per_unit > 0.0f) {
    arguments[count++] = "--lightmap-texels-per-unit";
    arguments[count++] =
        vkr_project_printf(job, "%.9g", (float64_t)lightmap_texels_per_unit);
  }
  const char *priority = NULL;
  if (job->material_priority && job->material_priority->first) {
    VkrBakeryBuffer names = {0};
    for (const VkrBakeryJson *name = job->material_priority->first; name;
         name = name->next) {
      vkr_bakery_buffer_append(&names, name->string.str, name->string.length);
      vkr_bakery_buffer_append(&names, "\n", 1u);
    }
    priority = vkr_project_printf(job, "%s/%s.priority", directory, id);
    const bool8_t written =
        !names.failed &&
        vkr_bakery_write_file_atomic(priority, names.data, names.length);
    vkr_bakery_buffer_free(&names);
    if (written) {
      arguments[count++] = "--material-priority";
      arguments[count++] = priority;
    }
  }
  const bool8_t cooked =
      vkr_project_run_tool(job, "mesh", arguments, count, label, 0, NULL);
  if (priority) {
    (void)vkr_bakery_remove_file(priority);
  }
  if (cooked) {
    vkr_project_adopt_digests(job, bundle, digests);
  }
  (void)vkr_bakery_remove_file(digests);
  VKR_PROJECT_TRY(cooked);
  const bool8_t adopted = vkr_project_adopt_inspection(job, output, report);
  (void)vkr_bakery_remove_file(report);
  return adopted;
}

void vkr_project_record_ready_remaining(VkrProjectJob *job,
                                        VkrBakeryJson *const *records,
                                        uint32_t count, const char *root) {
  if (!job->ready_log || !count) {
    return;
  }
  /* The cook recorded materials whose textures it finished; the rest, which
     the job's texture step packed, are recorded from their published files,
     whose relative references resolve against them. */
  uint8_t *recorded = NULL;
  uint64_t recorded_length = 0u;
  (void)vkr_bakery_read_file(job->ready_log, MB(256), &recorded,
                             &recorded_length);
  VkrBakeryBuffer lines = {0};
  for (uint32_t r = 0u; r < count; ++r) {
    const VkrBakeryJson *artifacts =
        vkr_bakery_json_get(records[r], "artifacts");
    const char *artifact =
        artifacts ? vkr_project_json_text(artifacts->first, "path") : NULL;
    char mesh[VKR_PROJECT_PATH];
    char directory[VKR_PROJECT_PATH];
    char materials[VKR_PROJECT_PATH];
    VkrProjectStrings names = {0};
    if (!artifact ||
        !vkr_bakery_path_join(mesh, sizeof(mesh), root, artifact)) {
      continue;
    }
    vkr_bakery_path_parent(directory, sizeof(directory), mesh);
    if (!vkr_bakery_path_join(materials, sizeof(materials), directory,
                              "materials") ||
        !vkr_project_list(job, materials, &names)) {
      continue;
    }
    for (uint32_t i = 0u; i < names.count; ++i) {
      const char *name = names.items[i];
      const uint64_t length = strlen(name);
      if (length < 4u || strcmp(name + length - 3u, ".mt") != 0) {
        continue;
      }
      const char *needle = vkr_project_printf(job, "{\"material\":\"%.*s\"",
                                              (int)(length - 3u), name);
      if (recorded && strstr((const char *)recorded, needle)) {
        continue;
      }
      char path[VKR_PROJECT_PATH];
      const char *text = NULL;
      if (!vkr_bakery_path_join(path, sizeof(path), materials, name) ||
          !vkr_project_read_text(job, path, VKR_PROJECT_MAX_JSON_BYTES, &text,
                                 NULL)) {
        continue;
      }
      VkrBakeryJson *line = vkr_bakery_json_object(job->arena);
      vkr_bakery_json_set(
          job->arena, line, "material",
          vkr_bakery_json_cstr(
              job->arena,
              vkr_project_printf(job, "%.*s", (int)(length - 3u), name)));
      vkr_bakery_json_set(job->arena, line, "path",
                          vkr_bakery_json_cstr(job->arena, path));
      vkr_bakery_json_set(job->arena, line, "definition",
                          vkr_bakery_json_cstr(job->arena, text));
      String8 encoded = {0};
      if (vkr_bakery_json_write(job->arena, line, VKR_BAKERY_JSON_COMPACT,
                                &encoded)) {
        vkr_bakery_buffer_append(&lines, encoded.str, encoded.length);
        vkr_bakery_buffer_append(&lines, "\n", 1u);
      }
    }
  }
  free(recorded);
  if (lines.length && !lines.failed &&
      !vkr_bakery_append_file(job->ready_log, lines.data, lines.length)) {
    vkr_project_warn(job, "Cannot record finished materials in %s",
                     job->ready_log);
  }
  vkr_bakery_buffer_free(&lines);
}

/* Rewrites a material's source-image texture lines to their packed .vkt. */
bool8_t vkr_project_point_material(VkrProjectJob *job, const char *material) {
  Arena *arena = job->arena;
  const char *text = NULL;
  VkrProjectStrings lines;
  VKR_PROJECT_TRY(vkr_project_read_text(
      job, material, VKR_PROJECT_MAX_JSON_BYTES, &text, NULL));
  VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
  bool8_t changed = false_v;
  char material_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(material_directory, sizeof(material_directory),
                         material);
  for (uint32_t i = 0u; i < lines.count; ++i) {
    char key[256];
    char value[VKR_PROJECT_PATH];
    const uint64_t key_length =
        vkr_project_partition_line(lines.items[i], key, sizeof(key), value,
                                   sizeof(value))
            ? strlen(key)
            : 0u;
    if (key_length < 8u || strcmp(key + key_length - 8u, "_texture") != 0 ||
        !value[0]) {
      continue;
    }
    char raw[VKR_PROJECT_PATH];
    char raw_suffix[32];
    (void)snprintf(raw, sizeof(raw), "%s", value);
    char *query = strchr(raw, '?');
    if (query) {
      *query = 0;
    }
    char joined[VKR_PROJECT_PATH];
    char resolved[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(joined, sizeof(joined), material_directory, raw);
    if (!vkr_project_resolve(joined, true_v, resolved, sizeof(resolved))) {
      return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                              joined);
    }
    vkr_project_suffix_lower(resolved, raw_suffix, sizeof(raw_suffix));
    if (!strcmp(raw_suffix, ".vkt")) {
      continue;
    }
    VkrProjectTextureLine texture;
    VKR_PROJECT_TRY(
        vkr_project_texture_line(job, material, key, value, &texture));
    if (!vkr_bakery_is_file(texture.destination)) {
      return vkr_project_fail(job,
                              "Texture packer did not publish its artifact");
    }
    const char *material_name =
        vkr_project_json_text(job->asset_names, material);
    char material_stem[512];
    vkr_project_stem(material, material_stem, sizeof(material_stem));
    char short_key[256];
    (void)snprintf(short_key, sizeof(short_key), "%.*s", (int)(key_length - 8u),
                   key);
    const char *source_name =
        vkr_project_json_text(job->source_names, texture.hash);
    const char *display =
        source_name
            ? source_name
            : vkr_project_printf(job, "%s %s",
                                 material_name ? material_name : material_stem,
                                 short_key);
    vkr_bakery_json_set(arena, job->asset_names, texture.destination,
                        vkr_bakery_json_cstr(arena, display));
    if (!vkr_bakery_json_get(job->asset_names, texture.source)) {
      vkr_bakery_json_set(arena, job->asset_names, texture.source,
                          vkr_bakery_json_cstr(arena, display));
    }
    char relative[VKR_PROJECT_PATH];
    if (!vkr_project_relpath(texture.destination, material_directory, relative,
                             sizeof(relative))) {
      return vkr_project_fail(job, "Path too long");
    }
    lines.items[i] = vkr_project_printf(job, "%s=./%s%s%s", key, relative,
                                        texture.has_query ? "?" : "",
                                        texture.has_query ? texture.query : "");
    changed = true_v;
  }
  if (changed) {
    VKR_PROJECT_TRY(vkr_project_write_lines(job, material, &lines));
  }
  return true_v;
}

bool8_t vkr_project_index_bundle(VkrProjectJob *job, const char *bundle,
                                 const char *import_id) {
  Arena *arena = job->arena;
  VkrBakeryJson *manifest = vkr_bakery_json_get(job->sources, import_id);
  char manifest_path[VKR_PROJECT_PATH];
  (void)snprintf(manifest_path, sizeof(manifest_path), "%s/imports/%s.json",
                 job->final_path, import_id);
  if (!manifest && vkr_bakery_is_file(manifest_path)) {
    manifest =
        vkr_project_load_json(job, manifest_path, VKR_PROJECT_MAX_JSON_BYTES);
    VKR_PROJECT_TRY(manifest);
  }
  const VkrBakeryJson *names = vkr_bakery_json_get(manifest, "material_names");
  char materials_directory[VKR_PROJECT_PATH];
  (void)snprintf(materials_directory, sizeof(materials_directory),
                 "%s/materials", bundle);
  VkrProjectStrings entries;
  VKR_PROJECT_TRY(vkr_project_list(job, materials_directory, &entries));
  VkrProjectStrings materials = {0};
  for (uint32_t i = 0u; i < entries.count; ++i) {
    /* Path.glob('*.mt') matches the exact case-sensitive suffix. */
    const uint64_t length = strlen(entries.items[i]);
    if (length < 3u || strcmp(entries.items[i] + length - 3u, ".mt") != 0) {
      continue;
    }
    char path[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(path, sizeof(path), materials_directory,
                               entries.items[i]);
    VKR_PROJECT_TRY(vkr_project_strings_push(job, &materials, path));
    char stem[512];
    vkr_project_stem(path, stem, sizeof(stem));
    const char *index = strrchr(stem, '_');
    index = index ? index + 1 : stem;
    bool8_t digits = index[0] != 0;
    for (const char *c = index; *c; ++c) {
      digits = digits && *c >= '0' && *c <= '9';
    }
    if (digits && names && strtoull(index, NULL, 10) < names->count &&
        !vkr_bakery_json_get(job->asset_names, path)) {
      const VkrBakeryJson *name =
          vkr_bakery_json_at(names, (uint32_t)strtoull(index, NULL, 10));
      if (name && name->type == VKR_BAKERY_JSON_STRING) {
        vkr_bakery_json_set(arena, job->asset_names, path,
                            vkr_bakery_json_clone(arena, name));
      }
    }
  }
  VKR_PROJECT_TRY(vkr_project_pack_bundle_textures(job, &materials));
  for (uint32_t i = 0u; i < materials.count; ++i) {
    const char *material = materials.items[i];
    VKR_PROJECT_TRY(vkr_project_point_material(job, material));
    char stem[512];
    vkr_project_stem(material, stem, sizeof(stem));
    const char *index = strrchr(stem, '_');
    index = index ? index + 1 : stem;
    const char *name = vkr_project_json_text(job->asset_names, material);
    VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, metadata, "source_key",
        vkr_bakery_json_cstr(arena,
                             vkr_project_printf(job, "material:%s", index)));
    VKR_PROJECT_TRY(vkr_project_artifact(job, "material", name ? name : stem,
                                         material, NULL, import_id, NULL,
                                         metadata));
  }
  char textures_directory[VKR_PROJECT_PATH];
  (void)snprintf(textures_directory, sizeof(textures_directory), "%s/textures",
                 bundle);
  VkrProjectStrings textures;
  VKR_PROJECT_TRY(vkr_project_list(job, textures_directory, &textures));
  /* Textures without a display name are named by their digest; hash them
     together on the index's workers instead of one by one below. */
  VkrProjectStrings unnamed = {0};
  for (uint32_t i = 0u; i < textures.count; ++i) {
    char path[VKR_PROJECT_PATH];
    if (strchr(textures.items[i], '.') &&
        vkr_bakery_path_join(path, sizeof(path), textures_directory,
                             textures.items[i]) &&
        !vkr_project_json_text(job->asset_names, path)) {
      VKR_PROJECT_TRY(vkr_project_strings_push(job, &unnamed, path));
    }
  }
  vkr_bakery_index_prefetch(vkr_project_index(job),
                            (const char *const *)unnamed.items, unnamed.count);
  for (uint32_t i = 0u; i < textures.count; ++i) {
    /* glob('*.*'): names containing a dot, files and directories. */
    if (!strchr(textures.items[i], '.')) {
      continue;
    }
    char path[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(path, sizeof(path), textures_directory,
                               textures.items[i]);
    if (!vkr_bakery_is_file(path)) {
      continue;
    }
    const char *display = vkr_project_json_text(job->asset_names, path);
    if (!display) {
      char hash[VKR_BAKERY_SHA256_HEX];
      VKR_PROJECT_TRY(vkr_project_digest(job, path, hash));
      display = vkr_project_json_text(job->source_names, hash);
    }
    char stem[512];
    vkr_project_stem(path, stem, sizeof(stem));
    VKR_PROJECT_TRY(vkr_project_artifact(job, "texture",
                                         display ? display : stem, path, NULL,
                                         import_id, NULL, NULL));
  }
  /* Every material now names packed textures. The marker, published with
     the immutable revision, spares each later preparation from reading the
     materials again. */
  return vkr_project_write_text(
      job, vkr_project_printf(job, "%s/" VKR_PROJECT_PACKED_MARKER, bundle), "",
      0u);
}

bool8_t vkr_project_import_material(VkrProjectJob *job, const char *source,
                                    const char *bundle, const char *import_id,
                                    uint32_t number, char *out) {
  Arena *arena = job->arena;
  char resolved[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_source_file(job, source, resolved));
  (void)snprintf(out, VKR_PROJECT_PATH, "%s/materials/%s_%u.mt", bundle,
                 import_id, number);
  const char *text = NULL;
  VkrProjectStrings lines;
  VKR_PROJECT_TRY(vkr_project_read_text(
      job, resolved, VKR_PROJECT_MAX_JSON_BYTES, &text, NULL));
  VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
  char display[512];
  vkr_project_stem(resolved, display, sizeof(display));
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), resolved);
  char textures[VKR_PROJECT_PATH];
  (void)snprintf(textures, sizeof(textures), "%s/textures", bundle);
  for (uint32_t i = 0u; i < lines.count; ++i) {
    char key[256];
    char value[VKR_PROJECT_PATH];
    if (!vkr_project_partition_line(lines.items[i], key, sizeof(key), value,
                                    sizeof(value))) {
      continue;
    }
    const uint64_t key_length = strlen(key);
    if (!strcmp(key, "name")) {
      if (value[0]) {
        (void)snprintf(display, sizeof(display), "%s", value);
      }
      lines.items[i] = vkr_project_printf(job, "name=%s_%u", import_id, number);
    } else if (key_length >= 8u && !strcmp(key + key_length - 8u, "_texture") &&
               value[0]) {
      char path[VKR_PROJECT_PATH];
      (void)snprintf(path, sizeof(path), "%s", value);
      char *marker = strchr(path, '?');
      const char *suffix = marker ? marker + 1 : "";
      if (marker) {
        *marker = 0;
      }
      char texture[VKR_PROJECT_PATH];
      char copied[VKR_PROJECT_PATH];
      VKR_PROJECT_TRY(vkr_project_legacy_source(job, path, origin,
                                                job->legacy_root, texture));
      VKR_PROJECT_TRY(vkr_project_copy_blob(job, texture, textures, copied));
      lines.items[i] = vkr_project_printf(
          job, "%s=./../textures/%s%s%s", key, vkr_bakery_path_name(copied),
          marker ? "?" : "", marker ? suffix : "");
    }
  }
  VKR_PROJECT_TRY(vkr_project_write_lines(job, out, &lines));
  vkr_bakery_json_set(arena, job->asset_names, out,
                      vkr_bakery_json_cstr(arena, display));
  VkrProjectStrings single = {0};
  VKR_PROJECT_TRY(vkr_project_strings_push(job, &single, out));
  VKR_PROJECT_TRY(vkr_project_pack_bundle_textures(job, &single));
  return vkr_project_point_material(job, out);
}

VkrBakeryJson *vkr_project_import_cooked_mesh(VkrProjectJob *job,
                                              const char *source) {
  Arena *arena = job->arena;
  char import_id[37];
  vkr_project_uuid4(import_id);
  char bundle[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage, import_id);
  if (!vkr_project_make_dirs(job, bundle)) {
    return NULL;
  }
  const char *inspect_path =
      vkr_project_printf(job, "%s/inspection.json", bundle);
  const char *arguments[] = {"--inspect", "--input", source, "--output",
                             inspect_path};
  if (!vkr_project_run_tool(job, "mesh", arguments, ArrayCount(arguments),
                            "Inspecting cooked model", 0, NULL)) {
    return NULL;
  }
  VkrBakeryJson *inventory =
      vkr_project_load_json(job, inspect_path, VKR_PROJECT_MAX_JSON_BYTES);
  if (!inventory) {
    return NULL;
  }
  int64_t version = 0;
  const VkrBakeryJson *materials = vkr_bakery_json_get(inventory, "materials");
  if (!vkr_bakery_json_get_int(inventory, "version", &version) ||
      version != 1 || !materials || materials->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_project_fail(job, "Cooker returned an invalid material inventory");
    return NULL;
  }
  char mesh[VKR_PROJECT_PATH];
  (void)snprintf(mesh, sizeof(mesh), "%s/mesh.vkb", bundle);
  if (!vkr_project_copy_file(job, source, mesh)) {
    return NULL;
  }
  char existing[VKR_PROJECT_PATH];
  (void)snprintf(existing, sizeof(existing), "%s.remap.json", source);
  const VkrBakeryJson *old_map = NULL;
  if (vkr_project_exists(existing)) {
    VkrBakeryJson *document =
        vkr_project_load_json(job, existing, VKR_PROJECT_MAX_JSON_BYTES);
    if (!document) {
      return NULL;
    }
    old_map = vkr_bakery_json_get(document, "materials");
  }
  char source_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(source_directory, sizeof(source_directory), source);
  VkrBakeryJson *remaps = vkr_bakery_json_object(arena);
  for (const VkrBakeryJson *item = materials->first; item; item = item->next) {
    const char *old_reference = (const char *)item->string.str;
    if (vkr_bakery_json_get(remaps, old_reference)) {
      continue; /* dict.fromkeys keeps the first occurrence. */
    }
    const char *mapped =
        old_map ? vkr_project_json_text(old_map, old_reference) : old_reference;
    if (!mapped || !mapped[0]) {
      vkr_project_fail(job, "Cooked dependency has no immutable remap: %s",
                       old_reference);
      return NULL;
    }
    char material[VKR_PROJECT_PATH];
    char imported[VKR_PROJECT_PATH];
    const char *relative = NULL;
    if (!vkr_project_legacy_source(job, mapped, source_directory,
                                   job->legacy_root, material) ||
        !vkr_project_import_material(job, material, bundle, import_id,
                                     remaps->count, imported) ||
        !vkr_project_managed_reference(job, imported, bundle, &relative)) {
      return NULL;
    }
    vkr_bakery_json_set(
        arena, remaps, old_reference,
        vkr_bakery_json_cstr(arena, vkr_project_printf(job, "./%s", relative)));
  }
  VkrBakeryJson *remap = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, remap, "version", vkr_project_int(job, 1));
  vkr_bakery_json_set(arena, remap, "materials", remaps);
  if (!vkr_project_atomic_json(
          job, vkr_project_printf(job, "%s.remap.json", mesh), remap)) {
    return NULL;
  }
  char stem[512];
  vkr_project_stem(source, stem, sizeof(stem));
  VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, metadata, "reimport_status",
                      vkr_bakery_json_cstr(arena, "source_unavailable"));
  const VkrBakeryJson *fingerprint =
      vkr_bakery_json_get(inventory, "fingerprint");
  vkr_bakery_json_set(arena, metadata, "source_fingerprint",
                      fingerprint ? vkr_bakery_json_clone(arena, fingerprint)
                                  : vkr_bakery_json_null(arena));
  VkrBakeryJson *reference = vkr_project_artifact(job, "mesh", stem, mesh, NULL,
                                                  import_id, NULL, metadata);
  if (!reference || !vkr_project_index_bundle(job, bundle, import_id)) {
    return NULL;
  }
  VkrBakeryJson *record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, record, "version", vkr_project_int(job, 1));
  vkr_bakery_json_set(arena, record, "id",
                      vkr_bakery_json_cstr(arena, import_id));
  vkr_bakery_json_set(arena, record, "reimport_status",
                      vkr_bakery_json_cstr(arena, "source_unavailable"));
  vkr_bakery_json_set(arena, record, "material_remaps",
                      vkr_bakery_json_clone(arena, remaps));
  VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
  for (VkrBakeryJson *item = job->assets->first; item; item = item->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "import_id"),
                                  import_id)) {
      vkr_bakery_json_append(artifacts, vkr_bakery_json_clone(arena, item));
    }
  }
  vkr_bakery_json_set(arena, record, "artifacts", artifacts);
  if (!vkr_project_atomic_json(
          job,
          vkr_project_printf(job, "%s/imports/%s.json", job->stage, import_id),
          record)) {
    return NULL;
  }
  return reference;
}

VkrBakeryJson *vkr_project_import_font(VkrProjectJob *job, const char *source) {
  Arena *arena = job->arena;
  char resolved[VKR_PROJECT_PATH];
  if (!vkr_project_source_file(job, source, resolved)) {
    return NULL;
  }
  char import_id[37];
  vkr_project_uuid4(import_id);
  char bundle[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage, import_id);
  if (!vkr_project_make_dirs(job, bundle)) {
    return NULL;
  }
  char suffix[32];
  vkr_project_suffix_lower(resolved, suffix, sizeof(suffix));
  char copied[VKR_PROJECT_PATH];
  (void)snprintf(copied, sizeof(copied), "%s/font%s", bundle, suffix);
  if (!vkr_project_copy_file(job, resolved, copied)) {
    return NULL;
  }
  if (strcmp(suffix, ".ttf") && strcmp(suffix, ".otf")) {
    vkr_project_fail(job, "Scene font must be a TTF or OTF file");
    return NULL;
  }
  const char *face_info = vkr_project_printf(job, "%s/face.json", bundle);
  const char *inspect[] = {"--inspect-font", copied, "--output", face_info};
  if (!vkr_project_run_tool(job, "font", inspect, ArrayCount(inspect),
                            "Inspecting font face", 0, NULL)) {
    return NULL;
  }
  VkrBakeryJson *face =
      vkr_project_load_json(job, face_info, VKR_PROJECT_MAX_JSON_BYTES);
  int64_t version = 0;
  int64_t face_index = -1;
  const char *face_name = face ? vkr_project_json_text(face, "face") : NULL;
  if (!face || !vkr_bakery_json_get_int(face, "version", &version) ||
      version != 1 ||
      !vkr_bakery_json_get_int(face, "face_index", &face_index) ||
      face_index != 0 || !face_name) {
    vkr_project_fail(job, "Font cooker returned invalid face metadata");
    return NULL;
  }
  char config[VKR_PROJECT_PATH];
  (void)snprintf(config, sizeof(config), "%s/font.fontcfg", bundle);
  const char *text = vkr_project_printf(
      job,
      "type=cooked_mtsdf\nsource=%s\nfile=font.vkfa\nface=%s\nface_index=0\n"
      "size=32\ncharset=U+0020-U+007E\ncharset=U+00A0-U+00FF\n"
      "fallback=U+003F\nfallback_policy=reject\nfield=mtsdf\n"
      "atlas_width=1024\natlas_height=1024\natlas_px_per_em=64\n"
      "distance_range=16\ninner_padding_px=0\nouter_padding_px=0\n"
      "edge_coloring=inktrap\nedge_angle_degrees=3\nedge_seed=1\n"
      "pixel_format=rgba8_unorm\nmiter_limit=1\nscanline=true\nthreads=1\n",
      vkr_bakery_path_name(copied), face_name);
  if (!vkr_project_write_text(job, config, text, strlen(text))) {
    return NULL;
  }
  char stem[512];
  vkr_project_stem(resolved, stem, sizeof(stem));
  bool8_t prepare = true_v;
  const VkrBakeryJson *bakes = vkr_bakery_json_get(job->request, "bakes");
  if (vkr_bakery_json_get_bool(bakes, "prepare_assets", &prepare) && !prepare) {
    char asset_id[37];
    vkr_project_uuid4(asset_id);
    const char *copied_reference = NULL;
    const char *config_reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    if (!vkr_project_managed_reference(job, copied, job->stage,
                                       &copied_reference) ||
        !vkr_project_managed_reference(job, config, job->stage,
                                       &config_reference) ||
        !vkr_project_digest(job, copied, hash)) {
      return NULL;
    }
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, record, "id",
                        vkr_bakery_json_cstr(arena, asset_id));
    vkr_bakery_json_set(arena, record, "kind",
                        vkr_bakery_json_cstr(arena, "font"));
    vkr_bakery_json_set(arena, record, "name",
                        vkr_bakery_json_cstr(arena, stem));
    vkr_bakery_json_set(arena, record, "import_id",
                        vkr_bakery_json_cstr(arena, import_id));
    vkr_bakery_json_set(arena, record, "source",
                        vkr_bakery_json_cstr(arena, copied_reference));
    vkr_bakery_json_set(arena, record, "artifacts",
                        vkr_bakery_json_array(arena));
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "tool",
                        vkr_bakery_json_cstr(arena, "font"));
    vkr_bakery_json_set(arena, recipe, "version", vkr_project_int(job, 1));
    vkr_bakery_json_set(arena, recipe, "config",
                        vkr_bakery_json_cstr(arena, config_reference));
    vkr_bakery_json_set(arena, record, "recipe", recipe);
    vkr_bakery_json_set(arena, record, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
    vkr_bakery_json_append(job->assets, record);
    return vkr_project_reference(job, job->asset_scope, asset_id, "font");
  }
  const char *cook[] = {"--config", config};
  if (!vkr_project_run_tool(job, "font", cook, ArrayCount(cook),
                            "Cooking scene font", 0, NULL)) {
    return NULL;
  }
  char atlas[VKR_PROJECT_PATH];
  (void)snprintf(atlas, sizeof(atlas), "%s/font.vkfa", bundle);
  if (!vkr_bakery_is_file(atlas)) {
    vkr_project_fail(job, "Font cooker did not publish its artifact");
    return NULL;
  }
  return vkr_project_artifact(job, "font", stem, config, NULL, import_id,
                              copied, NULL);
}

vkr_internal VkrBakeryJson *vkr_project_import_cubemap(VkrProjectJob *job,
                                                       VkrBakeryJson *value,
                                                       const char *origin) {
  Arena *arena = job->arena;
  if (value && value->type == VKR_BAKERY_JSON_OBJECT &&
      vkr_project_json_text(value, "path") &&
      vkr_project_json_text(value, "path")[0]) {
    const char *base = vkr_project_json_text(value, "base_path");
    const char *extension = vkr_project_json_text(value, "extension");
    if ((base && base[0]) || (extension && extension[0])) {
      vkr_project_fail(job, "Cubemap mixes a packed path and face paths");
      return NULL;
    }
    return vkr_project_import_cubemap(job, vkr_bakery_json_get(value, "path"),
                                      origin);
  }
  if (value && value->type == VKR_BAKERY_JSON_STRING) {
    char source[VKR_PROJECT_PATH];
    char directory[VKR_PROJECT_PATH];
    char copied[VKR_PROJECT_PATH];
    (void)snprintf(directory, sizeof(directory), "%s/builds/environments",
                   job->stage);
    if (!vkr_project_legacy_source(job, (const char *)value->string.str, origin,
                                   job->legacy_root, source) ||
        !vkr_project_copy_blob(job, source, directory, copied)) {
      return NULL;
    }
    char stem[512];
    vkr_project_stem(source, stem, sizeof(stem));
    VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, metadata, "source_kind",
                        vkr_bakery_json_cstr(arena, "cubemap"));
    return vkr_project_artifact(job, "environment", stem, copied, NULL, NULL,
                                NULL, metadata);
  }
  const char *base = value && value->type == VKR_BAKERY_JSON_OBJECT
                         ? vkr_project_json_text(value, "base_path")
                         : NULL;
  if (base && base[0]) {
    const char *requested_extension = vkr_project_json_text(value, "extension");
    char extension[32];
    (void)snprintf(extension, sizeof(extension), "%s",
                   requested_extension ? requested_extension : "png");
    const char *trimmed = extension;
    while (*trimmed == '.') {
      trimmed += 1;
    }
    bool8_t valid = trimmed[0] != 0;
    for (const char *c = trimmed; *c; ++c) {
      valid = valid && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9'));
    }
    if (!valid) {
      vkr_project_fail(job, "Invalid cube face extension");
      return NULL;
    }
    char import_id[37];
    vkr_project_uuid4(import_id);
    char directory[VKR_PROJECT_PATH];
    (void)snprintf(directory, sizeof(directory), "%s/builds/%s", job->stage,
                   import_id);
    if (!vkr_project_make_dirs(job, directory)) {
      return NULL;
    }
    static const char *const faces[] = {"r", "l", "u", "d", "f", "b"};
    char output_extension[32] = {0};
    char first[VKR_PROJECT_PATH] = {0};
    for (uint32_t f = 0u; f < ArrayCount(faces); ++f) {
      char source[VKR_PROJECT_PATH];
      if (!vkr_project_legacy_source(
              job, vkr_project_printf(job, "%s_%s.%s", base, faces[f], trimmed),
              origin, job->legacy_root, source)) {
        return NULL;
      }
      const char *dot = strrchr(vkr_bakery_path_name(source), '.');
      const char *actual = dot ? dot + 1 : "";
      if (output_extension[0] && strcmp(actual, output_extension) != 0) {
        vkr_project_fail(job, "Cubemap faces use inconsistent source formats");
        return NULL;
      }
      (void)snprintf(output_extension, sizeof(output_extension), "%s", actual);
      char copied[VKR_PROJECT_PATH];
      (void)snprintf(copied, sizeof(copied), "%s/cube_%s.%s", directory,
                     faces[f], output_extension);
      if (!vkr_project_copy_file(job, source, copied)) {
        return NULL;
      }
      if (f == 0u) {
        (void)snprintf(first, sizeof(first), "%s", copied);
      }
    }
    const char *base_reference = NULL;
    char cube[VKR_PROJECT_PATH];
    (void)snprintf(cube, sizeof(cube), "%s/cube", directory);
    if (!vkr_project_managed_reference(job, cube, job->stage,
                                       &base_reference)) {
      return NULL;
    }
    VkrBakeryJson *metadata = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, metadata, "source_kind",
                        vkr_bakery_json_cstr(arena, "faces"));
    vkr_bakery_json_set(arena, metadata, "base_path",
                        vkr_bakery_json_cstr(arena, base_reference));
    vkr_bakery_json_set(arena, metadata, "extension",
                        vkr_bakery_json_cstr(arena, output_extension));
    return vkr_project_artifact(job, "environment", "Cubemap", first, NULL,
                                import_id, NULL, metadata);
  }
  vkr_project_fail(job, "Unsupported cubemap asset");
  return NULL;
}

VkrBakeryJson *vkr_project_import_source(VkrProjectJob *job,
                                         const char *source) {
  char suffix[32];
  vkr_project_suffix_lower(source, suffix, sizeof(suffix));
  if (!strcmp(suffix, ".obj") || !strcmp(suffix, ".gltf") ||
      !strcmp(suffix, ".glb")) {
    return vkr_project_import_model(job, source);
  }
  if (!strcmp(suffix, ".vkb")) {
    return vkr_project_import_cooked_mesh(job, source);
  }
  VkrBakeryJson *none = vkr_bakery_json_null(job->arena);
  if (!strcmp(suffix, ".ttf") || !strcmp(suffix, ".otf")) {
    return vkr_project_import_font(job, source) ? none : NULL;
  }
  if (!strcmp(suffix, ".mt")) {
    char import_id[37];
    vkr_project_uuid4(import_id);
    char bundle[VKR_PROJECT_PATH];
    char material[VKR_PROJECT_PATH];
    (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage,
                   import_id);
    char stem[512];
    vkr_project_stem(source, stem, sizeof(stem));
    if (!vkr_project_import_material(job, source, bundle, import_id, 0u,
                                     material) ||
        !vkr_project_artifact(job, "material", stem, material, NULL, import_id,
                              material, NULL)) {
      return NULL;
    }
    return none;
  }
  if (vkr_project_is_image_suffix(suffix) || !strcmp(suffix, ".vkt")) {
    char revision[37];
    vkr_project_uuid4(revision);
    char directory[VKR_PROJECT_PATH];
    char imported[VKR_PROJECT_PATH];
    (void)snprintf(directory, sizeof(directory), "%s/builds/%s", job->stage,
                   revision);
    char stem[512];
    vkr_project_stem(source, stem, sizeof(stem));
    if (!vkr_project_copy_blob(job, source, directory, imported) ||
        !vkr_project_artifact(job, "texture", stem, imported, NULL, NULL,
                              imported, NULL)) {
      return NULL;
    }
    return none;
  }
  vkr_project_fail(job, "Unsupported asset format: %s", suffix);
  return NULL;
}

// =============================================================================
// Legacy and managed scene imports
// =============================================================================

VkrBakeryJson *vkr_project_import_scene(VkrProjectJob *job,
                                        const char *requested) {
  Arena *arena = job->arena;
  char source[VKR_PROJECT_PATH];
  if (!vkr_project_source_file(job, requested, source)) {
    return NULL;
  }
  VkrBakeryJson *scene =
      vkr_project_load_json(job, source, VKR_PROJECT_MAX_JSON_BYTES);
  if (!scene) {
    return NULL;
  }
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), source);
  int64_t version = 1;
  const VkrBakeryJson *version_value = vkr_bakery_json_get(scene, "version");
  if (version_value && version_value->type == VKR_BAKERY_JSON_INT) {
    version = version_value->integer;
  }
  if (version_value &&
      (version == 3 || version == 4 || version == VKR_PROJECT_SCENE_VERSION)) {
    VkrBakeryJson *managed = vkr_project_read_managed_scene(job, source);
    return managed ? vkr_project_import_managed_scene(job, managed, origin)
                   : NULL;
  }
  if (version_value && (version_value->type != VKR_BAKERY_JSON_INT ||
                        (version != 1 && version != 2))) {
    vkr_project_fail(job, "Unsupported scene JSON version");
    return NULL;
  }
  scene = vkr_bakery_json_clone(arena, scene);
  VkrBakeryJson *environment = vkr_bakery_json_get(scene, "environment");
  if (environment && environment->type == VKR_BAKERY_JSON_OBJECT) {
    static const char *const removed[] = {"equirect", "cubemap"};
    for (uint32_t i = 0u; i < ArrayCount(removed); ++i) {
      if (vkr_bakery_json_remove(environment, removed[i])) {
        vkr_project_warn(job,
                         "Dropped the removed environment %s sky image; author "
                         "atmosphere or environment.constant instead",
                         removed[i]);
      }
    }
  }
  VkrBakeryJson *probes = vkr_bakery_json_get(scene, "reflection_probes");
  for (VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    VkrBakeryJson *cubemap = vkr_bakery_json_get(probe, "cubemap");
    if (!cubemap || cubemap->type == VKR_BAKERY_JSON_NULL ||
        (cubemap->type == VKR_BAKERY_JSON_STRING && !cubemap->string.length)) {
      continue;
    }
    vkr_bakery_json_remove(probe, "cubemap");
    VkrBakeryJson *reference = vkr_project_import_cubemap(job, cubemap, origin);
    if (!reference) {
      return NULL;
    }
    vkr_bakery_json_set(arena, reference, "role",
                        vkr_bakery_json_cstr(arena, "probe-cube"));
    VkrBakeryJson *first = vkr_bakery_json_at(
        vkr_bakery_json_get(job->assets->last, "artifacts"), 0u);
    vkr_bakery_json_set(arena, first, "role",
                        vkr_bakery_json_cstr(arena, "probe-cube"));
    vkr_bakery_json_set(arena, probe, "asset", reference);
  }
  VkrBakeryJson *volume = vkr_bakery_json_get(scene, "diffuse_volume");
  if (volume && volume->type == VKR_BAKERY_JSON_OBJECT && volume->count) {
    const char *path = vkr_project_json_text(volume, "path");
    if (path && path[0]) {
      char original[VKR_PROJECT_PATH];
      char directory[VKR_PROJECT_PATH];
      char copied[VKR_PROJECT_PATH];
      (void)snprintf(directory, sizeof(directory), "%s/builds/volumes",
                     job->stage);
      if (!vkr_project_legacy_source(job, path, origin, job->legacy_root,
                                     original)) {
        return NULL;
      }
      vkr_bakery_json_remove(volume, "path");
      char stem[512];
      vkr_project_stem(original, stem, sizeof(stem));
      VkrBakeryJson *reference = NULL;
      if (!vkr_project_copy_blob(job, original, directory, copied) ||
          !(reference = vkr_project_artifact(job, "volume", stem, copied, NULL,
                                             NULL, NULL, NULL))) {
        return NULL;
      }
      vkr_bakery_json_set(arena, volume, "asset", reference);
    }
  }
  VkrBakeryJson *mesh_cache = vkr_bakery_json_object(arena);
  VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    char id[37];
    vkr_project_uuid4(id);
    vkr_bakery_json_set(arena, entity, "id", vkr_bakery_json_cstr(arena, id));
    VkrBakeryJson *mesh = vkr_bakery_json_get(entity, "mesh");
    const char *mesh_path = vkr_project_json_text(mesh, "path");
    if (mesh && mesh_path && mesh_path[0]) {
      char original[VKR_PROJECT_PATH];
      if (!vkr_project_legacy_source(job, mesh_path, origin, job->legacy_root,
                                     original)) {
        return NULL;
      }
      vkr_bakery_json_remove(mesh, "path");
      VkrBakeryJson *cached = vkr_bakery_json_get(mesh_cache, original);
      if (!cached) {
        char suffix[16];
        vkr_project_suffix_lower(original, suffix, sizeof(suffix));
        cached = !strcmp(suffix, ".vkb")
                     ? vkr_project_import_cooked_mesh(job, original)
                     : vkr_project_import_model(job, original);
        if (!cached) {
          return NULL;
        }
        vkr_bakery_json_set(arena, mesh_cache, original, cached);
      }
      vkr_bakery_json_set(arena, mesh, "asset",
                          vkr_bakery_json_clone(arena, cached));
    }
    VkrBakeryJson *shape = vkr_bakery_json_get(entity, "shape");
    VkrBakeryJson *material = vkr_bakery_json_get(shape, "material");
    const char *material_name = vkr_project_json_text(material, "name");
    const char *material_path = vkr_project_json_text(material, "path");
    if (material && material_name && material_name[0] &&
        !(material_path && material_path[0])) {
      vkr_project_fail(job, "Legacy named material needs its explicit source "
                            "path before import");
      return NULL;
    }
    if (material && material_path && material_path[0]) {
      char original[VKR_PROJECT_PATH];
      if (!vkr_project_legacy_source(job, material_path, origin,
                                     job->legacy_root, original)) {
        return NULL;
      }
      vkr_bakery_json_remove(material, "path");
      char import_id[37];
      vkr_project_uuid4(import_id);
      char bundle[VKR_PROJECT_PATH];
      char copied[VKR_PROJECT_PATH];
      (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage,
                     import_id);
      char stem[512];
      vkr_project_stem(original, stem, sizeof(stem));
      VkrBakeryJson *reference = NULL;
      if (!vkr_project_import_material(job, original, bundle, import_id, 0u,
                                       copied) ||
          !(reference = vkr_project_artifact(job, "material", stem, copied,
                                             NULL, import_id, NULL, NULL))) {
        return NULL;
      }
      vkr_bakery_json_set(arena, material, "asset", reference);
    }
    VkrBakeryJson *text = vkr_bakery_json_get(entity, "text3d");
    const char *font = vkr_project_json_text(text, "font");
    if (text && font) {
      if (strcmp(font, "UbuntuMono") && strcmp(font, "UbuntuMono-cooked") &&
          strcmp(font, "default-scene-font")) {
        vkr_project_fail(job,
                         "Locate the source/config for legacy text font %s "
                         "before importing",
                         font);
        return NULL;
      }
      vkr_bakery_json_set(arena, text, "font", vkr_bakery_json_null(arena));
    }
  }
  /* Authored overlay is imported separately only after native fingerprint
     validation. */
  char overlay[VKR_PROJECT_PATH];
  (void)snprintf(overlay, sizeof(overlay), "%s.editor.json", source);
  bool8_t include_edits = true_v;
  (void)vkr_bakery_json_get_bool(job->request, "include_edits", &include_edits);
  if (vkr_bakery_is_file(overlay) && include_edits) {
    char copied[VKR_PROJECT_PATH];
    (void)snprintf(copied, sizeof(copied), "%s/edits/scene.editor.json",
                   job->stage);
    if (!vkr_project_copy_file(job, overlay, copied)) {
      return NULL;
    }
    char expected[17];
    const char *identity = vkr_project_json_text(scene, "source_identity");
    if (identity && identity[0]) {
      vkr_project_fingerprint((const uint8_t *)identity, strlen(identity),
                              expected);
    } else {
      uint8_t *bytes = NULL;
      uint64_t length = 0u;
      if (!vkr_bakery_read_file(source, 0u, &bytes, &length)) {
        vkr_project_fail(job, "Cannot read %s", source);
        return NULL;
      }
      vkr_project_fingerprint(bytes, length, expected);
      free(bytes);
    }
    const char *reference = NULL;
    if (!vkr_project_remap_overlay(job, copied, expected, scene, origin,
                                   false_v) ||
        !vkr_project_managed_reference(job, copied, job->stage, &reference)) {
      return NULL;
    }
    vkr_bakery_json_set(arena, scene, "edit_overlay",
                        vkr_bakery_json_cstr(arena, reference));
  }
  vkr_bakery_json_remove(scene, "source_identity");
  return scene;
}

bool8_t vkr_project_copy_bundle(VkrProjectJob *job, VkrBakeryJson *record,
                                const char *owner, const char *new_id,
                                const char *link_error,
                                const char *missing_error) {
  VkrBakeryJson *products = vkr_bakery_json_get(record, "artifacts");
  if (!products || !products->count) {
    return vkr_project_fail(job, "%s", missing_error);
  }
  char first[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_contained(
      job, owner, vkr_project_json_text(products->first, "path"), true_v,
      first));
  char parent[VKR_PROJECT_PATH];
  char bundle_root[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(parent, sizeof(parent), first);
  if (!strcmp(vkr_bakery_path_name(parent), "materials")) {
    vkr_bakery_path_parent(bundle_root, sizeof(bundle_root), parent);
  } else {
    (void)snprintf(bundle_root, sizeof(bundle_root), "%s", parent);
  }
  char target[VKR_PROJECT_PATH];
  (void)snprintf(target, sizeof(target), "%s/builds/%s", job->stage, new_id);
  VkrProjectStrings files;
  VKR_PROJECT_TRY(
      vkr_project_walk_files(job, bundle_root, &files, true_v, link_error));
  for (uint32_t i = 0u; i < files.count; ++i) {
    char relative[VKR_PROJECT_PATH];
    char destination[VKR_PROJECT_PATH];
    if (!vkr_bakery_path_relative(bundle_root, files.items[i], relative,
                                  sizeof(relative)) ||
        !vkr_bakery_path_join(destination, sizeof(destination), target,
                              relative)) {
      return vkr_project_fail(job, "Path too long");
    }
    VKR_PROJECT_TRY(vkr_project_copy_file(job, files.items[i], destination));
  }
  for (VkrBakeryJson *product = products->first; product;
       product = product->next) {
    char old_path[VKR_PROJECT_PATH];
    char relative[VKR_PROJECT_PATH];
    char moved[VKR_PROJECT_PATH];
    const char *reference = NULL;
    VKR_PROJECT_TRY(vkr_project_contained(
        job, owner, vkr_project_json_text(product, "path"), true_v, old_path));
    if (!vkr_bakery_path_relative(bundle_root, old_path, relative,
                                  sizeof(relative)) ||
        !vkr_bakery_path_join(moved, sizeof(moved), target, relative)) {
      return vkr_project_fail(job, "'%s' is not in the subpath of '%s'",
                              old_path, bundle_root);
    }
    VKR_PROJECT_TRY(
        vkr_project_managed_reference(job, moved, job->stage, &reference));
    vkr_bakery_json_set(job->arena, product, "path",
                        vkr_bakery_json_cstr(job->arena, reference));
  }
  vkr_bakery_json_remove(record, "closure");
  vkr_bakery_json_set(job->arena, record, "id",
                      vkr_bakery_json_cstr(job->arena, new_id));
  vkr_bakery_json_set(job->arena, record, "source",
                      vkr_bakery_json_null(job->arena));
  return true_v;
}

typedef struct VkrProjectRemap {
  VkrProjectJob *job;
  VkrBakeryJson *remapped_ids;
  VkrBakeryJson *shared;
  VkrBakeryJson *source_project;
  char source_project_root[VKR_PROJECT_PATH];
} VkrProjectRemap;

vkr_internal VkrBakeryJson *vkr_project_remap_value(VkrProjectRemap *remap,
                                                    VkrBakeryJson *value) {
  VkrProjectJob *job = remap->job;
  Arena *arena = job->arena;
  if (!value) {
    return NULL;
  }
  if (value->type == VKR_BAKERY_JSON_ARRAY) {
    VkrBakeryJson *copy = vkr_bakery_json_array(arena);
    for (VkrBakeryJson *item = value->first; item; item = item->next) {
      VkrBakeryJson *mapped = vkr_project_remap_value(remap, item);
      if (!mapped) {
        return NULL;
      }
      vkr_bakery_json_append(copy, mapped);
    }
    return copy;
  }
  if (value->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_bakery_json_clone(arena, value);
  }
  const char *scope = vkr_project_json_text(value, "scope");
  VkrBakeryJson *id = vkr_bakery_json_get(value, "id");
  if (scope && !strcmp(scope, "scene") && id) {
    const VkrBakeryJson *mapped =
        id->type == VKR_BAKERY_JSON_STRING
            ? vkr_bakery_json_get(remap->remapped_ids,
                                  (const char *)id->string.str)
            : NULL;
    if (!mapped) {
      vkr_project_fail(job, "Imported scene references an unknown asset");
      return NULL;
    }
    VkrBakeryJson *copy = vkr_bakery_json_clone(arena, value);
    vkr_bakery_json_set(arena, copy, "id",
                        vkr_bakery_json_clone(arena, mapped));
    return copy;
  }
  if (scope && !strcmp(scope, "project") && id) {
    const char *key =
        id->type == VKR_BAKERY_JSON_STRING ? (const char *)id->string.str : "";
    const VkrBakeryJson *existing = vkr_bakery_json_get(remap->shared, key);
    if (!existing) {
      const VkrBakeryJson *records =
          vkr_bakery_json_get(remap->source_project, "assets");
      VkrBakeryJson *match = NULL;
      uint32_t matches = 0u;
      for (VkrBakeryJson *record = records ? records->first : NULL; record;
           record = record->next) {
        if (vkr_bakery_json_is_string(vkr_bakery_json_get(record, "id"), key)) {
          match = record;
          matches += 1u;
        }
      }
      if (matches != 1u) {
        vkr_project_fail(job,
                         "Imported scene has an unavailable project asset");
        return NULL;
      }
      VkrBakeryJson *record = vkr_bakery_json_clone(arena, match);
      char new_id[37];
      vkr_project_uuid4(new_id);
      if (!vkr_project_copy_bundle(job, record, remap->source_project_root,
                                   new_id,
                                   "Imported project asset contains a symlink",
                                   "Imported project asset has no artifact")) {
        return NULL;
      }
      vkr_bakery_json_append(job->assets, record);
      vkr_bakery_json_set(arena, remap->shared, key,
                          vkr_bakery_json_cstr(arena, new_id));
      existing = vkr_bakery_json_get(remap->shared, key);
    }
    VkrBakeryJson *copy = vkr_bakery_json_clone(arena, value);
    vkr_bakery_json_set(arena, copy, "scope",
                        vkr_bakery_json_cstr(arena, "scene"));
    vkr_bakery_json_set(arena, copy, "id",
                        vkr_bakery_json_clone(arena, existing));
    return copy;
  }
  VkrBakeryJson *copy = vkr_bakery_json_object(arena);
  for (VkrBakeryJson *field = value->first; field; field = field->next) {
    VkrBakeryJson *mapped = vkr_project_remap_value(remap, field);
    if (!mapped) {
      return NULL;
    }
    vkr_bakery_json_set(arena, copy, (const char *)field->key.str, mapped);
  }
  return copy;
}

VkrBakeryJson *vkr_project_import_managed_scene(VkrProjectJob *job,
                                                VkrBakeryJson *scene,
                                                const char *origin) {
  Arena *arena = job->arena;
  scene = vkr_bakery_json_clone(arena, scene);
  const char *label = vkr_project_printf(job, "%s/scene.json", origin);
  if (!vkr_project_migrate_source_references(job, scene, label)) {
    return NULL;
  }
  VkrBakeryJson *assets = vkr_bakery_json_get(scene, "assets");
  for (VkrBakeryJson *record = assets ? assets->first : NULL; record;
       record = record->next) {
    const VkrBakeryJson *scope = vkr_bakery_json_get(record, "scope");
    if (scope && scope->type != VKR_BAKERY_JSON_NULL &&
        !vkr_bakery_json_is_string(scope, "scene")) {
      vkr_project_fail(job,
                       "Managed scene import has an invalid asset inventory");
      return NULL;
    }
  }
  /* A managed scene bundle is the dependency closure. Copy only this owner. */
  VkrProjectStrings children;
  if (!vkr_project_list(job, origin, &children)) {
    return NULL;
  }
  for (uint32_t i = 0u; i < children.count; ++i) {
    const char *name = children.items[i];
    if (!strcmp(name, ".runtime") || !strcmp(name, "scene.json") ||
        !strcmp(name, "inventory")) {
      continue;
    }
    char child[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(child, sizeof(child), origin, name);
    if (vkr_project_lstat_is_link(child)) {
      vkr_project_fail(job,
                       "Resolve symlinks before importing a managed scene");
      return NULL;
    }
    if (vkr_bakery_is_directory(child)) {
      VkrProjectStrings files;
      if (!vkr_project_walk_files(job, child, &files, true_v,
                                  "Managed scene contains a symlink")) {
        return NULL;
      }
      for (uint32_t f = 0u; f < files.count; ++f) {
        char relative[VKR_PROJECT_PATH];
        char destination[VKR_PROJECT_PATH];
        if (!vkr_bakery_path_relative(origin, files.items[f], relative,
                                      sizeof(relative)) ||
            !vkr_bakery_path_join(destination, sizeof(destination), job->stage,
                                  relative) ||
            !vkr_project_copy_file(job, files.items[f], destination)) {
          if (!job->failed) {
            vkr_project_fail(job, "Path too long");
          }
          return NULL;
        }
      }
    } else if (vkr_bakery_is_file(child)) {
      char destination[VKR_PROJECT_PATH];
      (void)vkr_bakery_path_join(destination, sizeof(destination), job->stage,
                                 name);
      if (!vkr_project_copy_file(job, child, destination)) {
        return NULL;
      }
    }
  }
  char imports[VKR_PROJECT_PATH];
  (void)snprintf(imports, sizeof(imports), "%s/imports", job->stage);
  VkrProjectStrings import_names;
  if (!vkr_project_list(job, imports, &import_names)) {
    return NULL;
  }
  for (uint32_t i = 0u; i < import_names.count; ++i) {
    const uint64_t length = strlen(import_names.items[i]);
    if (length < 5u || strcmp(import_names.items[i] + length - 5u, ".json")) {
      continue;
    }
    char path[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(path, sizeof(path), imports,
                               import_names.items[i]);
    VkrBakeryJson *record =
        vkr_project_load_json(job, path, VKR_PROJECT_MAX_JSON_BYTES);
    if (!record) {
      return NULL;
    }
    VkrBakeryJson *migrated = vkr_bakery_json_clone(arena, record);
    if (!vkr_project_migrate_source_references(job, migrated, path)) {
      return NULL;
    }
    if (!vkr_bakery_json_equal(migrated, record) &&
        !vkr_project_atomic_json(job, path, migrated)) {
      return NULL;
    }
  }
  job->assets = assets ? assets : vkr_bakery_json_array(arena);
  const char *overlay_reference = vkr_project_json_text(scene, "edit_overlay");
  if (overlay_reference && overlay_reference[0]) {
    char overlay[VKR_PROJECT_PATH];
    const char *scene_id = NULL;
    if (!vkr_project_contained(job, job->stage, overlay_reference, false_v,
                               overlay) ||
        !vkr_project_identifier(job, vkr_bakery_json_get(scene, "id"),
                                &scene_id)) {
      return NULL;
    }
    char expected[17];
    vkr_project_fingerprint((const uint8_t *)scene_id, strlen(scene_id),
                            expected);
    if (!vkr_project_remap_overlay(job, overlay, expected, scene, origin,
                                   true_v)) {
      return NULL;
    }
  }
  VkrProjectRemap remap = {.job = job,
                           .remapped_ids = vkr_bakery_json_object(arena),
                           .shared = vkr_bakery_json_object(arena)};
  char source_project_path[VKR_PROJECT_PATH];
  char scenes_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(scenes_directory, sizeof(scenes_directory), origin);
  vkr_bakery_path_parent(remap.source_project_root,
                         sizeof(remap.source_project_root), scenes_directory);
  (void)snprintf(source_project_path, sizeof(source_project_path),
                 "%s/project.json", remap.source_project_root);
  if (vkr_bakery_is_file(source_project_path)) {
    remap.source_project = vkr_project_load_json(job, source_project_path,
                                                 VKR_PROJECT_MAX_JSON_BYTES);
    if (!remap.source_project) {
      return NULL;
    }
  } else {
    remap.source_project = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, remap.source_project, "assets",
                        vkr_bakery_json_array(arena));
  }
  const VkrBakeryJson *default_font =
      vkr_bakery_json_get(scene, "default_font");
  if (!default_font || default_font->type == VKR_BAKERY_JSON_NULL ||
      (default_font->type == VKR_BAKERY_JSON_OBJECT && !default_font->count)) {
    const VkrBakeryJson *project_font =
        vkr_bakery_json_get(remap.source_project, "default_font");
    vkr_bakery_json_set(arena, scene, "default_font",
                        project_font
                            ? vkr_bakery_json_clone(arena, project_font)
                            : vkr_bakery_json_null(arena));
  }
  for (VkrBakeryJson *record = job->assets->first; record;
       record = record->next) {
    char fresh[37];
    vkr_project_uuid4(fresh);
    const char *old_id = vkr_project_json_text(record, "id");
    if (old_id) {
      vkr_bakery_json_set(arena, remap.remapped_ids, old_id,
                          vkr_bakery_json_cstr(arena, fresh));
    }
    vkr_bakery_json_set(arena, record, "id",
                        vkr_bakery_json_cstr(arena, fresh));
  }
  for (VkrBakeryJson *field = scene->first; field; field = field->next) {
    if (field->key.length == 6u && !MemCompare(field->key.str, "assets", 6u)) {
      continue;
    }
    VkrBakeryJson *mapped = vkr_project_remap_value(&remap, field);
    if (!mapped) {
      return NULL;
    }
    vkr_bakery_json_set(arena, scene, (const char *)field->key.str, mapped);
  }
  VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    char fresh[37];
    vkr_project_uuid4(fresh);
    vkr_bakery_json_set(arena, entity, "id",
                        vkr_bakery_json_cstr(arena, fresh));
  }
  /* The copied overlay already binds by current index; name the copy's new
     ids so the runtime keeps that binding. */
  if (overlay_reference && overlay_reference[0]) {
    char overlay_path[VKR_PROJECT_PATH];
    if (!vkr_project_contained(job, job->stage,
                               vkr_project_json_text(scene, "edit_overlay"),
                               false_v, overlay_path)) {
      return NULL;
    }
    if (vkr_bakery_is_file(overlay_path)) {
      VkrBakeryJson *overlay =
          vkr_project_load_json(job, overlay_path, VKR_PROJECT_MAX_JSON_BYTES);
      if (!overlay) {
        return NULL;
      }
      if (vkr_bakery_json_get(overlay, "document_ids")) {
        VkrBakeryJson *ids = vkr_bakery_json_array(arena);
        for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
             entity = entity->next) {
          vkr_bakery_json_append(
              ids,
              vkr_bakery_json_cstr(arena, vkr_project_json_text(entity, "id")));
        }
        vkr_bakery_json_set(arena, overlay, "document_ids", ids);
        if (!vkr_project_atomic_json(job, overlay_path, overlay)) {
          return NULL;
        }
      }
    }
  }
  return scene;
}
