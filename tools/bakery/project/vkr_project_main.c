#include "vkr_project_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `vkr_bakery project --request <file> --result <file>`: one managed project
 * transaction. The request and result documents keep version 1 of the former
 * tools/editor_project_jobs.py contract. */

#define VKR_PROJECT_ARENA_RESERVE GB(4)
#define VKR_PROJECT_ARENA_COMMIT MB(4)

vkr_internal const char *const vkr_project_cleanup_operations[] = {
    "create_project",        "create_scene",           "bake_scene",
    "delete_scene",          "delete_project",         "add_entities",
    "import_project_assets", "import_assets",          "reimport_asset",
    "rebuild_asset",         "rename_asset",           "delete_asset",
    "finalize_textures",     "finalize_project_assets"};

vkr_internal const char *const vkr_project_scene_optional[] = {
    "create_project", "delete_project",  "import_project_assets",
    "inspect_scene",  "collect_garbage", "finalize_project_assets"};

vkr_internal bool8_t vkr_project_listed(const char *value,
                                        const char *const *items,
                                        uint32_t count) {
  for (uint32_t i = 0u; value && i < count; ++i) {
    if (strcmp(value, items[i]) == 0) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_project_required_path(VkrProjectJob *job,
                                               const char *key, bool8_t strict,
                                               char *out) {
  const char *value = vkr_project_json_text(job->request, key);
  if (!value) {
    return vkr_project_fail(job, "'%s'", key);
  }
  if (!vkr_project_resolve(value, strict, out, VKR_PROJECT_PATH)) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            value);
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_job_init(VkrProjectJob *job) {
  Arena *arena = job->arena;
  int64_t version = 0;
  if (!vkr_project_integer(vkr_bakery_json_get(job->request, "version"),
                           &version) ||
      version != VKR_PROJECT_VERSION) {
    return vkr_project_fail(job, "Unsupported project job request version");
  }
  job->operation = vkr_project_json_text(job->request, "operation");
  VKR_PROJECT_TRY(
      vkr_project_required_path(job, "workspace_root", true_v, job->workspace));
  VKR_PROJECT_TRY(vkr_project_required_path(job, "project_path", false_v,
                                            job->project_path));
  vkr_bakery_path_parent(job->project_root, sizeof(job->project_root),
                         job->project_path);
  const VkrBakeryJson *read_only =
      vkr_bakery_json_get(job->request, "read_only");
  if (read_only && read_only->type != VKR_BAKERY_JSON_BOOL) {
    return vkr_project_fail(job, "read_only must be boolean");
  }
  job->read_only = read_only && read_only->boolean;
  if (job->read_only) {
    if (!job->operation || strcmp(job->operation, "prepare_scene") != 0) {
      return vkr_project_fail(
          job, "Read-only workspace allows opening prepared scenes only");
    }
    const char *raw = vkr_project_json_text(job->request, "runtime_directory");
    if (!raw || !vkr_bakery_path_is_absolute(raw)) {
      return vkr_project_fail(job, "Read-only opening requires an absolute "
                                   "local runtime_directory");
    }
    (void)vkr_project_resolve(raw, false_v, job->runtime_directory,
                              sizeof(job->runtime_directory));
    if (vkr_project_is_relative_to(job->runtime_directory, job->workspace) ||
        vkr_project_is_relative_to(job->result_path, job->workspace)) {
      return vkr_project_fail(job, "Read-only runtime and result files must be "
                                   "outside the workspace");
    }
  }
  char projects[VKR_PROJECT_PATH];
  char project_parent[VKR_PROJECT_PATH];
  (void)snprintf(projects, sizeof(projects), "%s/projects", job->workspace);
  vkr_bakery_path_parent(project_parent, sizeof(project_parent),
                         job->project_root);
  if (strcmp(vkr_bakery_path_name(job->project_path), "project.json") != 0 ||
      strcmp(project_parent, projects) != 0) {
    return vkr_project_fail(job, "Project must belong directly to the chosen "
                                 "workspace projects directory");
  }
  const char *project_id = NULL;
  VKR_PROJECT_TRY(vkr_project_identifier(
      job, vkr_bakery_json_cstr(arena, vkr_bakery_path_name(job->project_root)),
      &project_id));
  const VkrBakeryJson *scene_id = vkr_bakery_json_get(job->request, "scene_id");
  if (vkr_project_truthy(scene_id)) {
    VKR_PROJECT_TRY(vkr_project_identifier(job, scene_id, &job->scene_id));
  }
  if (!job->scene_id &&
      !vkr_project_listed(job->operation, vkr_project_scene_optional,
                          ArrayCount(vkr_project_scene_optional))) {
    return vkr_project_fail(job, "Scene operation requires a scene identifier");
  }
  (void)snprintf(job->final_path, sizeof(job->final_path), "%s/scenes/%s",
                 job->project_root, job->scene_id ? job->scene_id : "unused");
  const char *legacy = vkr_project_json_text(job->request, "legacy_root");
  (void)vkr_project_resolve(legacy && legacy[0] ? legacy : PROJECT_SOURCE_DIR,
                            false_v, job->legacy_root,
                            sizeof(job->legacy_root));
  (void)snprintf(job->generated_root, sizeof(job->generated_root),
                 "%s/cache/generated", job->workspace);
  const char *tier = vkr_project_json_text(job->request, "texture_tier");
  if (tier && strcmp(tier, "preview") != 0 && strcmp(tier, "final") != 0 &&
      strcmp(tier, "deferred") != 0) {
    return vkr_project_fail(job,
                            "texture_tier must be preview, deferred or final");
  }
  job->texture_preview = tier && strcmp(tier, "preview") == 0;
  job->texture_deferred = tier && strcmp(tier, "deferred") == 0;
  /* Workspace-derived textures use the host's native encoding unless the
     request names one: ASTC 4x4 where the GPU samples it (Apple silicon),
     transcodable UASTC elsewhere. Every encoding has a preview tier. */
  const char *encoding =
      vkr_project_json_text(job->request, "texture_encoding");
  job->texture_encoding =
      VKR_PROJECT_NATIVE_ASTC ? VKR_VKT_ENCODING_ASTC : VKR_VKT_ENCODING_UASTC;
  if (encoding && !vkr_vkt_parse_encoding(encoding, &job->texture_encoding)) {
    return vkr_project_fail(job, "texture_encoding must be uastc, astc or "
                                 "astc-fast (astc-fast needs Apple's system "
                                 "encoder)");
  }
  /* `texture_encode_speed` "fast" encodes ASTC with the system encoder where
     it exists (astc-fast), as the editor asks for textures only it shows;
     UASTC has no fast encoding and ignores it. */
  const char *speed =
      vkr_project_json_text(job->request, "texture_encode_speed");
  if (speed && strcmp(speed, "fast") != 0 && strcmp(speed, "final") != 0) {
    return vkr_project_fail(job, "texture_encode_speed must be fast or final");
  }
  if (speed && strcmp(speed, "fast") == 0 &&
      job->texture_encoding == VKR_VKT_ENCODING_ASTC) {
    (void)vkr_vkt_parse_encoding("astc-fast", &job->texture_encoding);
  }
  /* An editor finalize names the log it reads finished materials from and
     the materials it shows first (ADR-077). */
  const char *ready_log = vkr_project_json_text(job->request, "ready_log");
  job->ready_log = ready_log && ready_log[0] ? ready_log : NULL;
  const VkrBakeryJson *priority =
      vkr_bakery_json_get(job->request, "material_priority");
  if (priority && priority->type != VKR_BAKERY_JSON_ARRAY) {
    return vkr_project_fail(job, "material_priority must be an array of "
                                 "material names");
  }
  for (const VkrBakeryJson *name = priority ? priority->first : NULL; name;
       name = name->next) {
    if (name->type != VKR_BAKERY_JSON_STRING || !name->string.length ||
        memchr(name->string.str, '\n', name->string.length)) {
      return vkr_project_fail(job, "material_priority must be an array of "
                                   "material names");
    }
  }
  job->material_priority = priority;
  job->asset_scope = "scene";
  job->warnings = vkr_bakery_json_array(arena);
  job->assets = vkr_bakery_json_array(arena);
  job->sources = vkr_bakery_json_object(arena);
  job->inspections = vkr_bakery_json_object(arena);
  job->collision_seen = vkr_bakery_json_object(arena);
  job->texture_seeds = vkr_bakery_json_object(arena);
  job->asset_names = vkr_bakery_json_object(arena);
  job->source_names = vkr_bakery_json_object(arena);
  return true_v;
}

vkr_internal bool8_t vkr_project_write_status(VkrProjectJob *job,
                                              const char *status,
                                              const char *stage,
                                              float64_t progress,
                                              const char *detail) {
  Arena *arena = job->arena;
  VkrBakeryJson *value = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, value, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, value, "status",
                      vkr_bakery_json_cstr(arena, status));
  vkr_bakery_json_set(arena, value, "stage",
                      vkr_bakery_json_cstr(arena, stage));
  vkr_bakery_json_set(arena, value, "progress",
                      vkr_bakery_json_float(arena, progress));
  if (detail) {
    vkr_bakery_json_set(arena, value, "detail",
                        vkr_bakery_json_cstr(arena, detail));
  }
  return vkr_project_atomic_json(job, job->progress_path, value);
}

vkr_internal void vkr_project_cleanup_after_success(VkrProjectJob *job) {
  if (job->read_only ||
      !vkr_project_listed(job->operation, vkr_project_cleanup_operations,
                          ArrayCount(vkr_project_cleanup_operations))) {
    return;
  }
  /* The result is already durable, so a cleanup failure or cancellation only
     leaves garbage. */
  if (!vkr_project_collect_garbage(job, NULL, NULL)) {
    printf("Warning: workspace cleanup stopped: %s\n", job->error);
    fflush(stdout);
    job->failed = false_v;
    job->error[0] = 0;
  }
}

vkr_internal VkrBakeryJson *vkr_project_dispatch(VkrProjectJob *job) {
  const char *operation = job->operation ? job->operation : "";
  if (!vkr_project_ensure_bootstrap(job) ||
      (!job->read_only && !vkr_project_prepare_font(job))) {
    return NULL;
  }
  if (strcmp(operation, "create_project") == 0) {
    Arena *arena = job->arena;
    VkrBakeryJson *result = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, result, "version",
                        vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
    vkr_bakery_json_set(arena, result, "status",
                        vkr_bakery_json_cstr(arena, "complete"));
    vkr_bakery_json_set(arena, result, "fonts", vkr_bakery_json_array(arena));
    vkr_bakery_json_set(arena, result, "warnings", job->warnings);
    return result;
  }
  if (strcmp(operation, "create_scene") == 0) {
    return vkr_project_create(job);
  }
  if (strcmp(operation, "bake_scene") == 0 ||
      strcmp(operation, "effective_bake_runtime") == 0) {
    char path[VKR_PROJECT_PATH];
    char expected_path[VKR_PROJECT_PATH];
    char expected[VKR_PROJECT_PATH];
    const char *requested = vkr_project_json_text(job->request, "scene_path");
    if (!requested ||
        !vkr_project_resolve(requested, true_v, path, sizeof(path))) {
      vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                       requested ? requested : "None");
      return NULL;
    }
    (void)snprintf(expected_path, sizeof(expected_path), "%s/scene.json",
                   job->final_path);
    (void)vkr_project_resolve(expected_path, false_v, expected,
                              sizeof(expected));
    if (strcmp(path, expected) != 0) {
      vkr_project_fail(job, "Bake scene does not match project membership");
      return NULL;
    }
    VkrBakeryJson *scene = vkr_project_read_managed_scene(job, path);
    if (!scene || !vkr_project_migrate_source_references(job, scene, path)) {
      return NULL;
    }
    if (strcmp(operation, "effective_bake_runtime") == 0) {
      /* The runtime a bake observes, with authored overrides applied; it
         publishes nothing into the scene. */
      char root[VKR_PROJECT_PATH];
      vkr_bakery_path_parent(root, sizeof(root), path);
      return vkr_project_effective_bake_runtime(job, scene, root);
    }
    return vkr_project_prepare_unbuilt(job, path, scene);
  }
  if (strcmp(operation, "import_project_assets") == 0) {
    return vkr_project_import_project_assets(job);
  }
  if (strcmp(operation, "finalize_project_assets") == 0) {
    return vkr_project_finalize_project_assets(job);
  }
  if (strcmp(operation, "prepare_scene") == 0) {
    return vkr_project_prepare(
        job, vkr_project_json_text(job->request, "scene_path"));
  }
  static const char *const edits[] = {
      "add_entities", "import_assets", "reimport_asset",   "rebuild_asset",
      "rename_asset", "delete_asset",  "finalize_textures"};
  if (vkr_project_listed(operation, edits, ArrayCount(edits))) {
    return vkr_project_edit_assets(job);
  }
  vkr_project_fail(job, "Unknown project job operation");
  return NULL;
}

vkr_internal bool8_t vkr_project_finish(VkrProjectJob *job,
                                        VkrBakeryJson *result) {
  Arena *arena = job->arena;
  const char *scene_path = vkr_project_json_text(result, "scene_path");
  if (scene_path && scene_path[0] &&
      !vkr_bakery_json_get(result, "manifest_fingerprint")) {
    uint8_t *bytes = NULL;
    uint64_t length = 0u;
    if (!vkr_bakery_read_file(scene_path, 0u, &bytes, &length)) {
      return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                              scene_path);
    }
    char fingerprint[17];
    vkr_project_fingerprint(bytes, length, fingerprint);
    free(bytes);
    vkr_bakery_json_set(arena, result, "manifest_fingerprint",
                        vkr_bakery_json_cstr(arena, fingerprint));
  }
  VkrBakeryJson *project = vkr_project_document(job);
  VKR_PROJECT_TRY(project);
  const VkrBakeryJson *assets = vkr_bakery_json_get(project, "assets");
  vkr_bakery_json_set(arena, result, "project_assets",
                      assets ? vkr_bakery_json_clone(arena, assets)
                             : vkr_bakery_json_array(arena));
  const VkrBakeryJson *font = vkr_bakery_json_get(project, "default_font");
  VkrBakeryJson *default_font = NULL;
  if (vkr_project_truthy(font)) {
    default_font = vkr_bakery_json_clone(arena, font);
  } else {
    default_font = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, default_font, "scope",
                        vkr_bakery_json_cstr(arena, "editor"));
    vkr_bakery_json_set(arena, default_font, "id",
                        vkr_bakery_json_cstr(arena, "default-scene-font"));
  }
  vkr_bakery_json_set(arena, result, "default_font", default_font);
  /* Once success may be observable, cleanup must not remove its
     resources. */
  job->final_owned = false_v;
  job->project_builds.count = 0u;
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, job->result_path, result));
  return vkr_project_write_status(job, "complete", "Ready", 1.0, NULL);
}

vkr_internal bool8_t vkr_project_execute(VkrProjectJob *job) {
  const char *operation = job->operation ? job->operation : "";
  if (strcmp(operation, "collect_garbage") == 0) {
    if (job->read_only) {
      return vkr_project_fail(job, "Cannot clean a read-only workspace");
    }
    uint32_t removed = 0u;
    uint64_t freed = 0u;
    if (!vkr_project_collect_garbage(job, &removed, &freed)) {
      return false_v;
    }
    Arena *arena = job->arena;
    VkrBakeryJson *result = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, result, "version",
                        vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
    vkr_bakery_json_set(arena, result, "status",
                        vkr_bakery_json_cstr(arena, "complete"));
    vkr_bakery_json_set(arena, result, "removed",
                        vkr_bakery_json_int(arena, removed));
    vkr_bakery_json_set(arena, result, "freed_bytes",
                        vkr_bakery_json_int(arena, (int64_t)freed));
    return vkr_project_atomic_json(job, job->result_path, result) &&
           vkr_project_write_status(job, "complete", "Cleaned", 1.0, NULL);
  }
  if (strcmp(operation, "inspect_scene") == 0) {
    VkrBakeryJson *result = vkr_project_inspect_scene(job);
    return result && vkr_project_atomic_json(job, job->result_path, result) &&
           vkr_project_write_status(job, "complete", "Inspected", 1.0, NULL);
  }
  if (strcmp(operation, "delete_scene") == 0 ||
      strcmp(operation, "delete_project") == 0) {
    const bool8_t scene = strcmp(operation, "delete_scene") == 0;
    VkrBakeryJson *result =
        scene ? vkr_project_delete_scene(job) : vkr_project_delete_project(job);
    if (!result || !vkr_project_atomic_json(job, job->result_path, result) ||
        !vkr_project_write_status(job, "complete",
                                  scene ? "Scene deleted" : "Project deleted",
                                  1.0, NULL)) {
      return false_v;
    }
    vkr_project_cleanup_after_success(job);
    return true_v;
  }
  VkrBakeryJson *result = vkr_project_dispatch(job);
  if (!result || !vkr_project_finish(job, result)) {
    return false_v;
  }
  vkr_project_cleanup_after_success(job);
  return true_v;
}

vkr_internal void vkr_project_release(VkrProjectJob *job) {
  if (job->stage[0]) {
    (void)vkr_project_remove_tree(job->stage);
  }
  if (job->final_owned) {
    (void)vkr_project_remove_tree(job->final_path);
  }
  const VkrProjectStrings *lists[] = {&job->published_builds,
                                      &job->project_builds};
  for (uint32_t l = 0u; l < ArrayCount(lists); ++l) {
    for (uint32_t i = 0u; i < lists[l]->count; ++i) {
      (void)vkr_project_remove_tree(lists[l]->items[i]);
    }
  }
}

vkr_internal void vkr_project_write_failure(VkrProjectJob *job,
                                            const char *status) {
  Arena *arena = job->arena;
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, status));
  vkr_bakery_json_set(arena, result, "error",
                      vkr_bakery_json_cstr(arena, job->error));
  char error[sizeof(job->error)];
  MemCopy(error, job->error, sizeof(error));
  (void)vkr_project_atomic_json(job, job->result_path, result);
  (void)vkr_project_write_status(job, status, status, 0.0, error);
  MemCopy(job->error, error, sizeof(error));
}

int vkr_project_main(const VkrBakeryConfig *config, const char *request_path,
                     const char *result_path) {
  if (!request_path || !result_path) {
    fprintf(stderr, "project requires --request <file> and --result <file>\n");
    return 2;
  }
  Arena *arena =
      arena_create(VKR_PROJECT_ARENA_RESERVE, VKR_PROJECT_ARENA_COMMIT);
  if (!arena) {
    fprintf(stderr, "Out of memory\n");
    return 1;
  }
  VkrProjectJob *job = (VkrProjectJob *)arena_alloc(
      arena, sizeof(VkrProjectJob), ARENA_MEMORY_TAG_STRUCT);
  if (!job) {
    arena_destroy(arena);
    fprintf(stderr, "Out of memory\n");
    return 1;
  }
  MemZero(job, sizeof(*job));
  job->config = config;
  job->arena = arena;
  (void)vkr_project_resolve(result_path, false_v, job->result_path,
                            sizeof(job->result_path));
  (void)snprintf(job->progress_path, sizeof(job->progress_path),
                 "%s.progress.json", job->result_path);
  job->request =
      vkr_project_load_json(job, request_path, VKR_PROJECT_MAX_JSON_BYTES);
  if (!job->request || !vkr_project_job_init(job)) {
    /* A read-only request must not write inside the workspace it opens. */
    bool8_t safe = true_v;
    bool8_t read_only = false_v;
    const char *workspace =
        vkr_project_json_text(job->request, "workspace_root");
    if (job->request &&
        vkr_bakery_json_get_bool(job->request, "read_only", &read_only) &&
        read_only && workspace && workspace[0]) {
      char resolved[VKR_PROJECT_PATH];
      (void)vkr_project_resolve(workspace, false_v, resolved, sizeof(resolved));
      safe = !vkr_project_is_relative_to(job->result_path, resolved);
    }
    if (safe) {
      Arena *result_arena = job->arena;
      VkrBakeryJson *result = vkr_bakery_json_object(result_arena);
      vkr_bakery_json_set(
          result_arena, result, "version",
          vkr_bakery_json_int(result_arena, VKR_PROJECT_VERSION));
      vkr_bakery_json_set(result_arena, result, "status",
                          vkr_bakery_json_cstr(result_arena, "failed"));
      vkr_bakery_json_set(result_arena, result, "error",
                          vkr_bakery_json_cstr(result_arena, job->error));
      char error[sizeof(job->error)];
      MemCopy(error, job->error, sizeof(error));
      (void)vkr_project_atomic_json(job, job->result_path, result);
      MemCopy(job->error, error, sizeof(error));
    }
    fprintf(stderr, "%s\n", job->error);
    vkr_bakery_index_close(job->index);
    arena_destroy(arena);
    return 1;
  }
  const bool8_t ok = vkr_project_execute(job);
  int exit_code = 0;
  if (!ok) {
    const bool8_t cancelled =
        job->cancelled || vkr_atomic_bool_load(&vkr_bakery_cancel_requested,
                                               VKR_MEMORY_ORDER_RELAXED);
    vkr_project_write_failure(job, cancelled ? "cancelled" : "failed");
    fprintf(stderr, "%s\n", job->error);
    fflush(stderr);
    exit_code = cancelled ? 2 : 1;
  }
  vkr_project_release(job);
  vkr_bakery_index_close(job->index);
  arena_destroy(arena);
  return exit_code;
}
