#include "vkr_project_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Workspace cleanup and the scene and project deletions that follow a
 * project-store membership change. */

typedef struct VkrProjectCleanup {
  VkrProjectJob *job;
  int64_t now;
  uint32_t removed;
  uint64_t freed;
} VkrProjectCleanup;

vkr_internal void vkr_project_drop(VkrProjectCleanup *cleanup,
                                   const char *path) {
  cleanup->freed += vkr_project_remove_tree(path);
  cleanup->removed += 1u;
}

vkr_internal bool8_t vkr_project_child_path(const char *directory,
                                            const char *name, char *out) {
  return vkr_bakery_path_join(out, VKR_PROJECT_PATH, directory, name);
}

/* Removes empty directories below `directory`, deepest first. */
vkr_internal void vkr_project_prune_empty(VkrProjectJob *job,
                                          const char *directory) {
  VkrProjectStrings names;
  const bool8_t failed_before = job->failed;
  if (!vkr_project_list(job, directory, &names)) {
    vkr_project_forgive(job, failed_before);
    return;
  }
  for (uint32_t i = names.count; i > 0u; --i) {
    char child[VKR_PROJECT_PATH];
    if (!vkr_project_child_path(directory, names.items[i - 1u], child) ||
        vkr_project_lstat_is_link(child) || !vkr_bakery_is_directory(child)) {
      continue;
    }
    vkr_project_prune_empty(job, child);
    VkrProjectStrings remaining;
    if (vkr_project_list(job, child, &remaining) && remaining.count == 0u) {
#if defined(_WIN32)
      (void)vkr_bakery_remove_tree(child);
#else
      (void)rmdir(child);
#endif
    }
    vkr_project_forgive(job, failed_before);
  }
}

/* Reads one listed scene's references; false marks the workspace unreadable
   so shared caches stay untouched. */
vkr_internal bool8_t vkr_project_scene_references(VkrProjectCleanup *cleanup,
                                                  const char *scene_root,
                                                  VkrProjectSet *digests) {
  VkrProjectJob *job = cleanup->job;
  const bool8_t failed_before = job->failed;
  char scene_path[VKR_PROJECT_PATH];
  (void)snprintf(scene_path, sizeof(scene_path), "%s/scene.json", scene_root);
  VkrBakeryJson *scene = vkr_project_read_managed_scene(job, scene_path);
  VkrBakeryJson *raw =
      scene ? vkr_project_load_json(job, scene_path,
                                    VKR_PROJECT_MAX_DOCUMENT_BYTES)
            : NULL;
  VkrBakeryJson *overlay = NULL;
  if (raw && vkr_project_truthy(vkr_bakery_json_get(scene, "edit_overlay"))) {
    char overlay_path[VKR_PROJECT_PATH];
    if (vkr_project_contained(job, scene_root,
                              vkr_project_json_text(scene, "edit_overlay"),
                              true_v, overlay_path)) {
      overlay =
          vkr_project_load_json(job, overlay_path, VKR_PROJECT_MAX_JSON_BYTES);
    }
    if (!overlay) {
      raw = NULL;
    }
  }
  if (!raw) {
    vkr_project_forgive(job, failed_before);
    return false_v;
  }
  VkrProjectSet revisions = {0};
  vkr_project_document_references(scene, digests, &revisions);
  vkr_project_document_references(overlay, digests, &revisions);
  char builds[VKR_PROJECT_PATH];
  (void)snprintf(builds, sizeof(builds), "%s/builds", scene_root);
  VkrProjectStrings names = {0};
  if (vkr_bakery_is_directory(builds) &&
      vkr_project_list(job, builds, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      char revision[VKR_PROJECT_PATH];
      if (vkr_project_child_path(builds, names.items[i], revision) &&
          !vkr_project_set_has(&revisions, names.items[i]) &&
          vkr_project_older_than(
              revision, VKR_PROJECT_UNREFERENCED_GRACE_SECONDS, cleanup->now)) {
        vkr_project_drop(cleanup, revision);
      }
    }
  }
  vkr_project_set_free(&revisions);
  const char *current = vkr_project_json_text(raw, "inventory");
  char inventories[VKR_PROJECT_PATH];
  (void)snprintf(inventories, sizeof(inventories), "%s/inventory", scene_root);
  if (vkr_bakery_is_directory(inventories) &&
      vkr_project_list(job, inventories, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      const uint64_t length = strlen(names.items[i]);
      if (length < 5u || strcmp(names.items[i] + length - 5u, ".json") != 0) {
        continue;
      }
      char inventory[VKR_PROJECT_PATH];
      const char *reference =
          vkr_project_printf(job, "inventory/%s", names.items[i]);
      if (vkr_project_child_path(inventories, names.items[i], inventory) &&
          (!current || strcmp(reference, current) != 0) &&
          vkr_project_older_than(inventory,
                                 VKR_PROJECT_UNREFERENCED_GRACE_SECONDS,
                                 cleanup->now)) {
        vkr_project_drop(cleanup, inventory);
      }
    }
  }
  vkr_project_forgive(job, failed_before);
  return true_v;
}

/* Collects one project's references and removes its unlisted and stale
   entries. Returns false when a document is unreadable. */
vkr_internal bool8_t vkr_project_clean_project(VkrProjectCleanup *cleanup,
                                               const char *project,
                                               VkrProjectSet *digests) {
  VkrProjectJob *job = cleanup->job;
  const bool8_t failed_before = job->failed;
  char manifest[VKR_PROJECT_PATH];
  (void)snprintf(manifest, sizeof(manifest), "%s/project.json", project);
  if (!vkr_bakery_is_file(manifest)) {
    if (strcmp(project, job->project_root) != 0 &&
        vkr_project_older_than(project, VKR_PROJECT_ORPHAN_GRACE_SECONDS,
                               cleanup->now)) {
      vkr_project_drop(cleanup, project);
    }
    return true_v;
  }
  VkrBakeryJson *document =
      vkr_project_load_json(job, manifest, VKR_PROJECT_MAX_DOCUMENT_BYTES);
  const VkrBakeryJson *scenes = vkr_bakery_json_get(document, "scenes");
  bool8_t members_valid =
      document && (!scenes || scenes->type == VKR_BAKERY_JSON_ARRAY);
  for (const VkrBakeryJson *entry = members_valid && scenes ? scenes->first
                                                            : NULL;
       entry; entry = entry->next) {
    members_valid = members_valid && entry->type == VKR_BAKERY_JSON_OBJECT &&
                    vkr_bakery_json_get(entry, "id");
  }
  if (!members_valid) {
    vkr_project_forgive(job, failed_before);
    return false_v;
  }
  vkr_project_document_references(vkr_bakery_json_get(document, "assets"),
                                  digests, NULL);
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", project);
  VkrProjectStrings names = {0};
  if (vkr_bakery_is_directory(staging) &&
      vkr_project_list(job, staging, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      char child[VKR_PROJECT_PATH];
      if (vkr_project_child_path(staging, names.items[i], child) &&
          vkr_project_older_than(child, VKR_PROJECT_UNREFERENCED_GRACE_SECONDS,
                                 cleanup->now)) {
        vkr_project_drop(cleanup, child);
      }
    }
  }
  bool8_t readable = true_v;
  char scenes_directory[VKR_PROJECT_PATH];
  (void)snprintf(scenes_directory, sizeof(scenes_directory), "%s/scenes",
                 project);
  if (vkr_bakery_is_directory(scenes_directory) &&
      vkr_project_list(job, scenes_directory, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      char scene_root[VKR_PROJECT_PATH];
      if (!vkr_project_child_path(scenes_directory, names.items[i],
                                  scene_root) ||
          vkr_project_lstat_is_link(scene_root) ||
          !vkr_bakery_is_directory(scene_root)) {
        continue;
      }
      /* A recent unlisted scene may await its membership save; it stays
         live until the grace period proves it abandoned. */
      const bool8_t listed =
          vkr_project_record_by_id(scenes, names.items[i]) != NULL;
      if (!listed && strcmp(scene_root, job->final_path) != 0 &&
          vkr_project_older_than(scene_root, VKR_PROJECT_ORPHAN_GRACE_SECONDS,
                                 cleanup->now)) {
        vkr_project_drop(cleanup, scene_root);
        continue;
      }
      readable = vkr_project_scene_references(cleanup, scene_root, digests) &&
                 readable;
    }
  }
  vkr_project_forgive(job, failed_before);
  return readable;
}

/* Legacy per-workspace texture cache from the Python job runner. */
vkr_internal void
vkr_project_clean_texture_cache(VkrProjectCleanup *cleanup,
                                const VkrProjectSet *digests) {
  VkrProjectJob *job = cleanup->job;
  const bool8_t failed_before = job->failed;
  char textures[VKR_PROJECT_PATH];
  (void)snprintf(textures, sizeof(textures), "%s/cache/textures",
                 job->workspace);
  VkrProjectStrings names = {0};
  if (!vkr_bakery_is_directory(textures) ||
      !vkr_project_list(job, textures, &names)) {
    vkr_project_forgive(job, failed_before);
    return;
  }
  for (uint32_t i = 0u; i < names.count; ++i) {
    char entry[VKR_PROJECT_PATH];
    char manifest[VKR_PROJECT_PATH];
    if (!vkr_project_child_path(textures, names.items[i], entry)) {
      continue;
    }
    (void)snprintf(manifest, sizeof(manifest), "%s/manifest.json", entry);
    if (!vkr_bakery_is_file(manifest)) {
      if (vkr_project_older_than(entry, VKR_PROJECT_UNREFERENCED_GRACE_SECONDS,
                                 cleanup->now)) {
        vkr_project_drop(cleanup, entry);
      }
      continue;
    }
    VkrBakeryJson *recorded =
        vkr_project_load_json(job, manifest, VKR_PROJECT_MAX_JSON_BYTES);
    vkr_project_forgive(job, failed_before);
    int64_t version = 0;
    const char *hash = vkr_project_json_text(recorded, "sha256");
    if (!vkr_project_integer(
            vkr_bakery_json_get(vkr_bakery_json_get(recorded, "recipe"),
                                "version"),
            &version) ||
        version != 2 || !hash || !vkr_project_set_has(digests, hash)) {
      vkr_project_drop(cleanup, entry);
    }
  }
}

/* Content-addressed derived textures the mesh cooker shares across imports;
   an index of size, mtime and digest avoids rehashing unchanged files. An
   unreferenced file is removed once it is older than the grace period. */
vkr_internal bool8_t vkr_project_clean_generated(VkrProjectCleanup *cleanup,
                                                 const VkrProjectSet *digests) {
  VkrProjectJob *job = cleanup->job;
  Arena *arena = job->arena;
  const bool8_t failed_before = job->failed;
  char generated[VKR_PROJECT_PATH];
  char index_path[VKR_PROJECT_PATH];
  (void)snprintf(generated, sizeof(generated), "%s/cache/generated",
                 job->workspace);
  (void)snprintf(index_path, sizeof(index_path),
                 "%s/cache/generated-index.json", job->workspace);
  VkrBakeryJson *index = NULL;
  if (vkr_bakery_is_file(index_path)) {
    index = vkr_project_load_json(job, index_path, VKR_PROJECT_MAX_JSON_BYTES);
    vkr_project_forgive(job, failed_before);
  }
  if (!vkr_bakery_is_directory(generated)) {
    return true_v;
  }
  VkrProjectStrings files = {0};
  if (!vkr_project_walk_files(job, generated, &files, false_v, NULL)) {
    vkr_project_forgive(job, failed_before);
  }
  VkrBakeryJson *kept = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < files.count; ++i) {
    const char *path = files.items[i];
    const char *relative = NULL;
    VkrBakeryStat info;
    if (!vkr_project_managed_reference(job, path, generated, &relative) ||
        !vkr_bakery_stat(path, &info)) {
      vkr_project_forgive(job, failed_before);
      continue;
    }
    /* A file younger than the grace period stays whether or not a scene
       names it, so its digest decides nothing yet; a job that just wrote
       gigabytes of derived textures does not hash them again here. */
    if (!vkr_project_older_than(path, VKR_PROJECT_UNREFERENCED_GRACE_SECONDS,
                                cleanup->now)) {
      continue;
    }
    const VkrBakeryJson *known = vkr_bakery_json_get(index, relative);
    int64_t known_size = -1;
    int64_t known_mtime = -1;
    const VkrBakeryJson *known_digest = vkr_bakery_json_at(known, 2u);
    char content[VKR_BAKERY_SHA256_HEX] = {0};
    if (vkr_project_integer(vkr_bakery_json_at(known, 0u), &known_size) &&
        vkr_project_integer(vkr_bakery_json_at(known, 1u), &known_mtime) &&
        known_size == (int64_t)info.size && known_mtime == info.mtime_ns &&
        known_digest && known_digest->type == VKR_BAKERY_JSON_STRING) {
      (void)snprintf(content, sizeof(content), "%s",
                     (const char *)known_digest->string.str);
    } else if (!vkr_project_digest(job, path, content)) {
      vkr_project_forgive(job, failed_before);
      continue;
    }
    if (vkr_project_set_has(digests, content)) {
      VkrBakeryJson *record = vkr_bakery_json_array(arena);
      vkr_bakery_json_append(record,
                             vkr_bakery_json_int(arena, (int64_t)info.size));
      vkr_bakery_json_append(record, vkr_bakery_json_int(arena, info.mtime_ns));
      vkr_bakery_json_append(record, vkr_bakery_json_cstr(arena, content));
      vkr_bakery_json_set(arena, kept, relative, record);
    } else {
      /* Intermediates no bundle holds, such as the converted images a paired
         bake reads, stay a day so importing the model again reuses them. */
      vkr_project_drop(cleanup, path);
    }
  }
  vkr_project_prune_empty(job, generated);
  return vkr_project_atomic_json(job, index_path, kept);
}

bool8_t vkr_project_collect_garbage(VkrProjectJob *job, uint32_t *out_removed,
                                    uint64_t *out_freed) {
  VkrProjectCleanup cleanup = {.job = job, .now = (int64_t)time(NULL)};
  VkrProjectSet digests = {0};
  bool8_t readable = true_v;
  char projects[VKR_PROJECT_PATH];
  (void)snprintf(projects, sizeof(projects), "%s/projects", job->workspace);
  VkrProjectStrings names = {0};
  if (vkr_bakery_is_directory(projects)) {
    VKR_PROJECT_TRY(vkr_project_list(job, projects, &names));
  }
  for (uint32_t i = 0u; i < names.count; ++i) {
    char project[VKR_PROJECT_PATH];
    if (!vkr_project_child_path(projects, names.items[i], project) ||
        vkr_project_lstat_is_link(project) ||
        !vkr_bakery_is_directory(project) ||
        !vkr_project_identifier_valid(names.items[i])) {
      continue;
    }
    readable =
        vkr_project_clean_project(&cleanup, project, &digests) && readable;
  }
  bool8_t ok = true_v;
  if (readable) {
    const uint64_t freed_before = cleanup.freed;
    vkr_project_clean_texture_cache(&cleanup, &digests);
    ok = vkr_project_clean_generated(&cleanup, &digests);
    if (cleanup.freed > freed_before) {
      printf("Workspace cleanup: removed unused cache entries (%.1f MiB)\n",
             (float64_t)(cleanup.freed - freed_before) / 1048576.0);
      fflush(stdout);
    }
  }
  vkr_project_set_free(&digests);
  char jobs[VKR_PROJECT_PATH];
  char result_parent[VKR_PROJECT_PATH];
  (void)snprintf(jobs, sizeof(jobs), "%s/jobs", job->workspace);
  vkr_bakery_path_parent(result_parent, sizeof(result_parent),
                         job->result_path);
  if (ok && vkr_bakery_is_directory(jobs) &&
      vkr_project_list(job, jobs, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      char entry[VKR_PROJECT_PATH];
      if (vkr_project_child_path(jobs, names.items[i], entry) &&
          strcmp(entry, result_parent) != 0 &&
          vkr_project_older_than(entry, VKR_PROJECT_JOB_RETENTION_SECONDS,
                                 cleanup.now)) {
        vkr_project_drop(&cleanup, entry);
      }
    }
  }
  if (cleanup.removed) {
    printf("Workspace cleanup: removed %u unreferenced paths, %.1f MiB\n",
           cleanup.removed, (float64_t)cleanup.freed / 1048576.0);
    fflush(stdout);
  }
  if (out_removed) {
    *out_removed = cleanup.removed;
  }
  if (out_freed) {
    *out_freed = cleanup.freed;
  }
  return ok;
}

// =============================================================================
// Deletion
// =============================================================================

typedef enum VkrProjectNode {
  VKR_PROJECT_NODE_MISSING = 0,
  VKR_PROJECT_NODE_LINK,
  VKR_PROJECT_NODE_DIRECTORY,
  VKR_PROJECT_NODE_OTHER,
} VkrProjectNode;

/* lstat classification; Windows junctions and reparse points count as
   links, which must never become recursive deletion roots. */
vkr_internal VkrProjectNode vkr_project_node_kind(const char *path) {
  if (vkr_project_lstat_is_link(path)) {
    return VKR_PROJECT_NODE_LINK;
  }
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || !info.exists) {
    return VKR_PROJECT_NODE_MISSING;
  }
  return info.is_directory ? VKR_PROJECT_NODE_DIRECTORY
                           : VKR_PROJECT_NODE_OTHER;
}

/* Rejects any link below `root` before a recursive removal. */
vkr_internal bool8_t vkr_project_check_tree(VkrProjectJob *job,
                                            const char *root,
                                            const char *message) {
  VkrProjectStrings names;
  VKR_PROJECT_TRY(vkr_project_list(job, root, &names));
  for (uint32_t i = 0u; i < names.count; ++i) {
    char child[VKR_PROJECT_PATH];
    if (!vkr_project_child_path(root, names.items[i], child)) {
      return vkr_project_fail(job, "Path too long below %s", root);
    }
    const VkrProjectNode kind = vkr_project_node_kind(child);
    if (kind == VKR_PROJECT_NODE_LINK) {
      return vkr_project_fail(job, "%s: %s", message, child);
    }
    if (kind == VKR_PROJECT_NODE_DIRECTORY) {
      VKR_PROJECT_TRY(vkr_project_check_tree(job, child, message));
    }
  }
  return true_v;
}

VkrBakeryJson *vkr_project_delete_project(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (job->read_only) {
    vkr_project_fail(job, "Cannot delete a project in a read-only workspace");
    return NULL;
  }
  if (vkr_project_is_relative_to(job->result_path, job->project_root)) {
    vkr_project_fail(job,
                     "Delete result must be outside the project directory");
    return NULL;
  }
  if (vkr_project_exists(job->project_path) ||
      vkr_project_lstat_is_link(job->project_path)) {
    vkr_project_fail(job, "Project is still published; remove its manifest "
                          "before deleting files");
    return NULL;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(
      arena, result, "deleted_project_id",
      vkr_bakery_json_cstr(arena, vkr_bakery_path_name(job->project_root)));
  const VkrProjectNode kind = vkr_project_node_kind(job->project_root);
  if (kind == VKR_PROJECT_NODE_MISSING) {
    return result;
  }
  const char *message = "Project deletion refuses links or reparse points";
  if (kind == VKR_PROJECT_NODE_LINK) {
    vkr_project_fail(job, "%s: %s", message, job->project_root);
    return NULL;
  }
  if (kind == VKR_PROJECT_NODE_DIRECTORY &&
      !vkr_project_check_tree(job, job->project_root, message)) {
    return NULL;
  }
  if (!vkr_project_progress(job, "Deleting project files", 0.1, "")) {
    return NULL;
  }
  (void)vkr_project_remove_tree(job->project_root);
  if (vkr_project_exists(job->project_root)) {
    vkr_project_fail(job, "Cannot remove every project file below %s",
                     job->project_root);
    return NULL;
  }
  return result;
}

/* PurePath form: repeated separators and "." parts drop; ".." stays, so a
   path that walks out and back never equals the direct one. */
vkr_internal bool8_t vkr_project_pure_path(const char *path, char *out,
                                           uint32_t capacity) {
  uint32_t length = 0u;
  const char *cursor = path;
  while (*cursor) {
    while (*cursor == '/') {
      cursor += 1;
    }
    const char *end = strchr(cursor, '/');
    const uint32_t part =
        end ? (uint32_t)(end - cursor) : (uint32_t)strlen(cursor);
    if (part && !(part == 1u && cursor[0] == '.')) {
      if (length + part + 2u > capacity) {
        return false_v;
      }
      out[length++] = '/';
      MemCopy(out + length, cursor, part);
      length += part;
    }
    cursor += part;
  }
  if (!length) {
    out[length++] = '/';
  }
  out[length] = 0;
  return true_v;
}

VkrBakeryJson *vkr_project_delete_scene(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (job->read_only) {
    vkr_project_fail(job, "Cannot delete a scene in a read-only workspace");
    return NULL;
  }
  char expected[VKR_PROJECT_PATH];
  char normalized[VKR_PROJECT_PATH];
  (void)snprintf(expected, sizeof(expected), "%s/scene.json", job->final_path);
  const char *raw_path = vkr_project_json_text(job->request, "scene_path");
  if (!raw_path || !vkr_bakery_path_is_absolute(raw_path) ||
      !vkr_project_pure_path(raw_path, normalized, sizeof(normalized)) ||
      strcmp(normalized, expected) != 0) {
    vkr_project_fail(job, "Delete scene path must be the exact project-owned "
                          "scene manifest");
    return NULL;
  }
  if (vkr_project_is_relative_to(job->result_path, job->final_path)) {
    vkr_project_fail(job, "Delete result must be outside the scene directory");
    return NULL;
  }
  const char *message = "Scene deletion refuses links or reparse points";
  /* Validate ancestors before resolving or traversing the deletion tree. */
  char scenes[VKR_PROJECT_PATH];
  (void)snprintf(scenes, sizeof(scenes), "%s/scenes", job->project_root);
  const char *ancestors[] = {job->project_root, scenes};
  for (uint32_t i = 0u; i < ArrayCount(ancestors); ++i) {
    if (vkr_project_node_kind(ancestors[i]) == VKR_PROJECT_NODE_LINK) {
      vkr_project_fail(job, "%s: %s", message, ancestors[i]);
      return NULL;
    }
  }
  char resolved[VKR_PROJECT_PATH];
  (void)vkr_project_resolve(expected, false_v, resolved, sizeof(resolved));
  if (strcmp(resolved, expected) != 0) {
    vkr_project_fail(job,
                     "Delete scene path escapes its project-owned directory");
    return NULL;
  }
  VkrBakeryJson *project =
      vkr_project_load_json(job, job->project_path, VKR_PROJECT_MAX_JSON_BYTES);
  if (!project) {
    return NULL;
  }
  int64_t version = 0;
  const VkrBakeryJson *members = vkr_bakery_json_get(project, "scenes");
  if (!vkr_project_integer(vkr_bakery_json_get(project, "version"), &version) ||
      version != VKR_PROJECT_VERSION || !members ||
      members->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_project_fail(job, "Invalid project manifest for scene deletion");
    return NULL;
  }
  for (const VkrBakeryJson *member = members->first; member;
       member = member->next) {
    const char *member_path = vkr_project_json_text(member, "path");
    if (member->type != VKR_BAKERY_JSON_OBJECT || !member_path) {
      vkr_project_fail(job, "Invalid project scene membership");
      return NULL;
    }
    char joined[VKR_PROJECT_PATH];
    char member_resolved[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(joined, sizeof(joined), job->project_root,
                               member_path);
    (void)vkr_project_resolve(joined, false_v, member_resolved,
                              sizeof(member_resolved));
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(member, "id"),
                                  job->scene_id) ||
        vkr_project_is_relative_to(member_resolved, job->final_path)) {
      vkr_project_fail(job, "Scene is still referenced by the project; remove "
                            "membership before deleting files");
      return NULL;
    }
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "deleted_scene_id",
                      vkr_bakery_json_cstr(arena, job->scene_id));
  const VkrProjectNode kind = vkr_project_node_kind(job->final_path);
  if (kind == VKR_PROJECT_NODE_MISSING) {
    return result;
  }
  if (kind == VKR_PROJECT_NODE_LINK) {
    vkr_project_fail(job, "%s: %s", message, job->final_path);
    return NULL;
  }
  if (kind != VKR_PROJECT_NODE_DIRECTORY) {
    vkr_project_fail(job, "Scene deletion root is not a directory");
    return NULL;
  }
  if (!vkr_project_check_tree(job, job->final_path, message) ||
      !vkr_project_progress(job, "Deleting scene files", 0.1, "")) {
    return NULL;
  }
  (void)vkr_project_remove_tree(job->final_path);
  if (vkr_project_exists(job->final_path)) {
    vkr_project_fail(job, "Cannot remove every scene file below %s",
                     job->final_path);
    return NULL;
  }
  return result;
}
