#include "vkr_project_internal.h"

#include "platform/vkr_platform.h"

#include <stdio.h>
#include <string.h>

/* The depot's merge drivers (docs/proposals/depot-git-model.md, "Merge"):
 * `merge_scene_unit` and `merge_project_document` merge one unit of
 * managed project files that both sides of a depot merge changed. The
 * depot writes each side's member files into `base`, `mine` and `theirs`
 * at their paths relative to the anchor's folder; the driver writes the
 * merged files into `output` at the same relative paths, and its result
 * says which to write and which to delete, or lists the conflicts:
 *
 *   {"merged": true, "write": [...], "delete": [...]}
 *   {"merged": false, "conflicts": [{"path", "id", "reason"}...]}
 *
 * A scene unit is `scene.json`, its live inventory and its edit overlay.
 * Records of the inventory merge by `id` against the base: a record one
 * side changed takes that side, one both changed alike is kept, and one
 * both changed apart, or one changed while the other removed it, is a
 * conflict. The overlay names scene entities by index, so its entity list
 * may only grow at the end: one side's entities, or both sides' appended
 * entities when each kept the base list as its start. The overlay itself
 * merges whole, and the other fields of `scene.json` merge one by one.
 * `project.json` merges its `scenes` and `assets` by `id` and its other
 * fields one by one. The job reads the request's folders and writes only
 * into `output`. */

typedef struct VkrMerge {
  VkrProjectJob *job;
  /* {path, id, reason} of each conflict. */
  VkrBakeryJson *conflicts;
} VkrMerge;

vkr_internal void vkr_merge_conflict(VkrMerge *merge, const char *path,
                                     const char *id, const char *reason) {
  Arena *arena = merge->job->arena;
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entry, "path", vkr_bakery_json_cstr(arena, path));
  vkr_bakery_json_set(arena, entry, "id",
                      id ? vkr_bakery_json_cstr(arena, id)
                         : vkr_bakery_json_null(arena));
  vkr_bakery_json_set(arena, entry, "reason",
                      vkr_bakery_json_cstr(arena, reason));
  vkr_bakery_json_append(merge->conflicts, entry);
}

/* Whether two values are equal, a missing one matching only another
   missing one. */
vkr_internal bool8_t vkr_merge_same(const VkrBakeryJson *a,
                                    const VkrBakeryJson *b) {
  return (!a && !b) || (a && b && vkr_bakery_json_equal(a, b));
}

/* The three-way choice for one value: the side that changed it, or the
   common value; NULL with `*out_conflict` when both changed it apart.
   `*out_present` is false when the result is "absent". */
vkr_internal const VkrBakeryJson *vkr_merge_choose(const VkrBakeryJson *base,
                                                   const VkrBakeryJson *mine,
                                                   const VkrBakeryJson *theirs,
                                                   bool8_t *out_conflict,
                                                   bool8_t *out_present) {
  *out_conflict = false_v;
  const VkrBakeryJson *chosen = NULL;
  if (vkr_merge_same(mine, theirs) || vkr_merge_same(theirs, base)) {
    chosen = mine;
  } else if (vkr_merge_same(mine, base)) {
    chosen = theirs;
  } else {
    *out_conflict = true_v;
  }
  *out_present = chosen != NULL;
  return chosen;
}

vkr_internal const VkrBakeryJson *vkr_merge_record(const VkrBakeryJson *list,
                                                   const char *id) {
  for (const VkrBakeryJson *item = list ? list->first : NULL; item;
       item = item->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "id"), id)) {
      return item;
    }
  }
  return NULL;
}

/* Merges three lists of records with an `id`, in mine's order and then
   theirs' new records. A record without an `id` fails the merge. */
vkr_internal VkrBakeryJson *vkr_merge_records(VkrMerge *merge, const char *path,
                                              const VkrBakeryJson *base,
                                              const VkrBakeryJson *mine,
                                              const VkrBakeryJson *theirs) {
  Arena *arena = merge->job->arena;
  VkrBakeryJson *merged = vkr_bakery_json_array(arena);
  const VkrBakeryJson *lists[] = {base, mine, theirs};
  for (uint32_t l = 0u; l < ArrayCount(lists); ++l) {
    for (const VkrBakeryJson *item = lists[l] ? lists[l]->first : NULL; item;
         item = item->next) {
      if (!vkr_project_json_text(item, "id")) {
        vkr_project_fail(merge->job, "A record of %s has no id", path);
        return NULL;
      }
    }
  }
  /* Mine's records in its order, then theirs' that mine lacks. */
  for (uint32_t side = 1u; side <= 2u; ++side) {
    for (const VkrBakeryJson *item = lists[side] ? lists[side]->first : NULL;
         item; item = item->next) {
      const char *id = vkr_project_json_text(item, "id");
      if (side == 2u && vkr_merge_record(mine, id)) {
        continue;
      }
      bool8_t conflict = false_v;
      bool8_t present = false_v;
      const VkrBakeryJson *chosen = vkr_merge_choose(
          vkr_merge_record(base, id), vkr_merge_record(mine, id),
          vkr_merge_record(theirs, id), &conflict, &present);
      if (conflict) {
        vkr_merge_conflict(merge, path, id,
                           vkr_merge_record(mine, id) &&
                                   vkr_merge_record(theirs, id)
                               ? "changed on both sides"
                               : "changed on one side, removed on the other");
      } else if (present) {
        vkr_bakery_json_append(merged, vkr_bakery_json_clone(arena, chosen));
      }
    }
  }
  return merged;
}

/* Whether `list` starts with every item of `prefix`, in order. */
vkr_internal bool8_t vkr_merge_starts_with(const VkrBakeryJson *list,
                                           const VkrBakeryJson *prefix) {
  const VkrBakeryJson *item = list ? list->first : NULL;
  for (const VkrBakeryJson *want = prefix ? prefix->first : NULL; want;
       want = want->next) {
    if (!item || !vkr_bakery_json_equal(item, want)) {
      return false_v;
    }
    item = item->next;
  }
  return true_v;
}

/* Scene entities: one side's, or both sides' appended ones after the base
   list when each kept it as its start, so the overlay's entity indices
   still name the entities they named. */
vkr_internal VkrBakeryJson *
vkr_merge_entities(VkrMerge *merge, const char *path, const VkrBakeryJson *base,
                   const VkrBakeryJson *mine, const VkrBakeryJson *theirs) {
  Arena *arena = merge->job->arena;
  bool8_t conflict = false_v;
  bool8_t present = false_v;
  const VkrBakeryJson *chosen =
      vkr_merge_choose(base, mine, theirs, &conflict, &present);
  if (!conflict) {
    return present ? vkr_bakery_json_clone(arena, chosen) : NULL;
  }
  if (!base || !mine || !theirs || !vkr_merge_starts_with(mine, base) ||
      !vkr_merge_starts_with(theirs, base)) {
    vkr_merge_conflict(merge, path, "entities",
                       "entities changed on both sides other than by "
                       "appending");
    return NULL;
  }
  VkrBakeryJson *merged = vkr_bakery_json_clone(arena, mine);
  const VkrBakeryJson *added = theirs->first;
  for (uint32_t i = 0u; i < base->count; ++i) {
    added = added->next;
  }
  for (; added; added = added->next) {
    const char *id = vkr_project_json_text(added, "id");
    if (id && vkr_merge_record(mine, id)) {
      vkr_merge_conflict(merge, path, id,
                         "an entity both sides added with one id");
      return NULL;
    }
    vkr_bakery_json_append(merged, vkr_bakery_json_clone(arena, added));
  }
  return merged;
}

/* Merges an object's fields one by one; fields in `skip` stay out. */
vkr_internal VkrBakeryJson *
vkr_merge_fields(VkrMerge *merge, const char *path, const VkrBakeryJson *base,
                 const VkrBakeryJson *mine, const VkrBakeryJson *theirs,
                 const char *const *skip, uint32_t skip_count) {
  Arena *arena = merge->job->arena;
  VkrBakeryJson *merged = vkr_bakery_json_object(arena);
  const VkrBakeryJson *sides[] = {mine, theirs, base};
  for (uint32_t s = 0u; s < ArrayCount(sides); ++s) {
    for (const VkrBakeryJson *field = sides[s] ? sides[s]->first : NULL; field;
         field = field->next) {
      const char *key = (const char *)field->key.str;
      bool8_t skipped = vkr_bakery_json_get(merged, key) != NULL;
      for (uint32_t k = 0u; k < skip_count && !skipped; ++k) {
        skipped = strcmp(key, skip[k]) == 0;
      }
      /* A field seen on an earlier side was decided there. */
      for (uint32_t earlier = 0u; earlier < s && !skipped; ++earlier) {
        skipped = sides[earlier] && vkr_bakery_json_get(sides[earlier], key);
      }
      if (skipped) {
        continue;
      }
      bool8_t conflict = false_v;
      bool8_t present = false_v;
      const VkrBakeryJson *chosen = vkr_merge_choose(
          vkr_bakery_json_get(base, key), vkr_bakery_json_get(mine, key),
          vkr_bakery_json_get(theirs, key), &conflict, &present);
      if (conflict) {
        vkr_merge_conflict(merge, path, key, "changed on both sides");
      } else if (present) {
        vkr_bakery_json_set(arena, merged, key,
                            vkr_bakery_json_clone(arena, chosen));
      }
    }
  }
  return merged;
}

/* A side's file at `relative` below its folder, or NULL when the side
   lacks it. */
vkr_internal VkrBakeryJson *vkr_merge_side_file(VkrProjectJob *job,
                                                const char *side_root,
                                                const char *relative) {
  char path[VKR_PROJECT_PATH];
  if (!relative ||
      !vkr_project_contained(job, side_root, relative, false_v, path)) {
    return NULL;
  }
  return vkr_bakery_is_file(path)
             ? vkr_project_load_json(job, path, VKR_PROJECT_MAX_OVERLAY_BYTES)
             : NULL;
}

/* The request's absolute folder `key`, which must exist. */
vkr_internal bool8_t vkr_merge_folder(VkrProjectJob *job, const char *key,
                                      char *out) {
  const char *value = vkr_project_json_text(job->request, key);
  if (!value || !vkr_bakery_path_is_absolute(value) ||
      !vkr_project_resolve(value, true_v, out, VKR_PROJECT_PATH) ||
      !vkr_bakery_is_directory(out)) {
    return vkr_project_fail(job, "The merge needs an existing folder in '%s'",
                            key);
  }
  return true_v;
}

vkr_internal bool8_t vkr_merge_write(VkrProjectJob *job, const char *output,
                                     const char *relative, VkrBakeryJson *value,
                                     VkrBakeryJson *written) {
  char path[VKR_PROJECT_PATH];
  char folder[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_contained(job, output, relative, false_v, path));
  vkr_bakery_path_parent(folder, sizeof(folder), path);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, folder));
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, path, value));
  vkr_bakery_json_append(written, vkr_bakery_json_cstr(job->arena, relative));
  return true_v;
}

/* The scene unit: `scene.json`, its live inventory and its overlay. */
vkr_internal bool8_t vkr_merge_scene(VkrMerge *merge, const char *roots[3],
                                     const char *output,
                                     VkrBakeryJson *written) {
  VkrProjectJob *job = merge->job;
  Arena *arena = job->arena;
  VkrBakeryJson *scenes[3];
  for (uint32_t s = 0u; s < 3u; ++s) {
    scenes[s] = vkr_merge_side_file(job, roots[s], "scene.json");
    VKR_PROJECT_TRY(!job->failed);
  }
  if (!scenes[1] || !scenes[2]) {
    vkr_merge_conflict(merge, "scene.json", NULL,
                       "the scene was removed on one side");
    return true_v;
  }
  /* Each side's inventory and overlay, as its scene names them. */
  const VkrBakeryJson *inventories[3];
  const VkrBakeryJson *overlays[3];
  for (uint32_t s = 0u; s < 3u; ++s) {
    const char *inventory =
        scenes[s] ? vkr_project_json_text(scenes[s], "inventory") : NULL;
    const char *overlay =
        scenes[s] ? vkr_project_json_text(scenes[s], "edit_overlay") : NULL;
    const VkrBakeryJson *document =
        vkr_merge_side_file(job, roots[s], inventory);
    VKR_PROJECT_TRY(!job->failed);
    inventories[s] = document ? vkr_bakery_json_get(document, "assets") : NULL;
    overlays[s] = overlay && overlay[0]
                      ? vkr_merge_side_file(job, roots[s], overlay)
                      : NULL;
    VKR_PROJECT_TRY(!job->failed);
    if (s > 0u && inventory && !document) {
      return vkr_project_fail(job,
                              "The scene names the inventory %s, which "
                              "the merge did not get",
                              inventory);
    }
  }
  VkrBakeryJson *records = vkr_merge_records(merge, "inventory", inventories[0],
                                             inventories[1], inventories[2]);
  VKR_PROJECT_TRY(records);
  VkrBakeryJson *entities = vkr_merge_entities(
      merge, "scene.json", vkr_bakery_json_get(scenes[0], "entities"),
      vkr_bakery_json_get(scenes[1], "entities"),
      vkr_bakery_json_get(scenes[2], "entities"));
  static const char *const skip[] = {"inventory", "edit_overlay", "entities"};
  VkrBakeryJson *scene =
      vkr_merge_fields(merge, "scene.json", scenes[0], scenes[1], scenes[2],
                       skip, ArrayCount(skip));
  /* The overlay merges whole until scene edits merge per entity. */
  bool8_t overlay_conflict = false_v;
  bool8_t overlay_present = false_v;
  const VkrBakeryJson *overlay =
      vkr_merge_choose(overlays[0], overlays[1], overlays[2], &overlay_conflict,
                       &overlay_present);
  if (overlay_conflict) {
    vkr_merge_conflict(merge, "edits", NULL,
                       "both sides edited the scene; their edits merge per "
                       "entity in a later depot version");
  }
  if (merge->conflicts->count) {
    return true_v;
  }
  if (entities) {
    vkr_bakery_json_set(arena, scene, "entities", entities);
  }
  if (overlay_present) {
    const VkrBakeryJson *named =
        overlay == overlays[1] ? vkr_bakery_json_get(scenes[1], "edit_overlay")
                               : vkr_bakery_json_get(scenes[2], "edit_overlay");
    vkr_bakery_json_set(arena, scene, "edit_overlay",
                        vkr_bakery_json_clone(arena, named));
  }
  /* The merged records: one side's inventory file when they are its own,
     else a new one. */
  const char *inventory = NULL;
  for (uint32_t s = 1u; s <= 2u && !inventory; ++s) {
    if (inventories[s] && vkr_bakery_json_equal(inventories[s], records)) {
      inventory = vkr_project_json_text(scenes[s], "inventory");
    }
  }
  if (!inventory) {
    char id[37];
    vkr_project_uuid4(id);
    inventory = vkr_project_printf(job, "inventory/%s.json", id);
    VkrBakeryJson *document = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, document, "version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_set(arena, document, "assets", records);
    VKR_PROJECT_TRY(vkr_merge_write(job, output, inventory, document, written));
  }
  vkr_bakery_json_set(arena, scene, "inventory",
                      vkr_bakery_json_cstr(arena, inventory));
  return vkr_merge_write(job, output, "scene.json", scene, written);
}

vkr_internal bool8_t vkr_merge_project(VkrMerge *merge, const char *roots[3],
                                       const char *output,
                                       VkrBakeryJson *written) {
  VkrProjectJob *job = merge->job;
  Arena *arena = job->arena;
  VkrBakeryJson *documents[3];
  for (uint32_t s = 0u; s < 3u; ++s) {
    documents[s] = vkr_merge_side_file(job, roots[s], "project.json");
    VKR_PROJECT_TRY(!job->failed);
  }
  if (!documents[1] || !documents[2]) {
    vkr_merge_conflict(merge, "project.json", NULL,
                       "the project was removed on one side");
    return true_v;
  }
  static const char *const lists[] = {"scenes", "assets"};
  VkrBakeryJson *merged_lists[ArrayCount(lists)];
  for (uint32_t i = 0u; i < ArrayCount(lists); ++i) {
    merged_lists[i] = vkr_merge_records(
        merge, vkr_project_printf(job, "project.json %s", lists[i]),
        vkr_bakery_json_get(documents[0], lists[i]),
        vkr_bakery_json_get(documents[1], lists[i]),
        vkr_bakery_json_get(documents[2], lists[i]));
    VKR_PROJECT_TRY(merged_lists[i]);
  }
  VkrBakeryJson *project =
      vkr_merge_fields(merge, "project.json", documents[0], documents[1],
                       documents[2], lists, ArrayCount(lists));
  if (merge->conflicts->count) {
    return true_v;
  }
  for (uint32_t i = 0u; i < ArrayCount(lists); ++i) {
    vkr_bakery_json_set(arena, project, lists[i], merged_lists[i]);
  }
  return vkr_merge_write(job, output, "project.json", project, written);
}

// =============================================================================
// Pull report
// =============================================================================

/* Runs `operation` on one scene as a child job and returns its result, or
   NULL with `*out_error` set. A child keeps each scene's job state its
   own. */
vkr_internal const VkrBakeryJson *
vkr_pull_child(VkrProjectJob *job, const char *folder, const char *operation,
               const char *scene_id, const char *scene_path,
               const char **out_error) {
  Arena *arena = job->arena;
  *out_error = NULL;
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, request, "operation",
                      vkr_bakery_json_cstr(arena, operation));
  vkr_bakery_json_set(arena, request, "workspace_root",
                      vkr_bakery_json_cstr(arena, job->workspace));
  vkr_bakery_json_set(arena, request, "project_path",
                      vkr_bakery_json_cstr(arena, job->project_path));
  vkr_bakery_json_set(arena, request, "scene_id",
                      vkr_bakery_json_cstr(arena, scene_id));
  vkr_bakery_json_set(arena, request, "scene_path",
                      vkr_bakery_json_cstr(arena, scene_path));
  const VkrBakeryJson *legacy =
      vkr_bakery_json_get(job->request, "legacy_root");
  if (legacy) {
    vkr_bakery_json_set(arena, request, "legacy_root",
                        vkr_bakery_json_clone(arena, legacy));
  }
  const char *request_path = vkr_project_printf(job, "%s/%s-%s.request.json",
                                                folder, scene_id, operation);
  const char *result_path = vkr_project_printf(job, "%s/%s-%s.result.json",
                                               folder, scene_id, operation);
  char self[VKR_PROJECT_PATH];
  if (!vkr_project_atomic_json(job, request_path, request) ||
      !vkr_bakery_executable_path(self, sizeof(self))) {
    *out_error = "the child job could not start";
    return NULL;
  }
  const char *arguments[] = {"project", "--request", request_path, "--result",
                             result_path};
  const VkrPlatformProcessConfig config = {
      .executable = self,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path =
          vkr_project_printf(job, "%s/%s-%s.log", folder, scene_id, operation),
      .hidden = true_v,
      .terminate_process_tree = true_v,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  const bool8_t ran =
      vkr_platform_process_run(&config, &exit_code, &timed_out) && !timed_out;
  const VkrBakeryJson *result =
      vkr_bakery_is_file(result_path)
          ? vkr_project_load_json(job, result_path, VKR_PROJECT_MAX_JSON_BYTES)
          : NULL;
  if (!ran || exit_code != 0 || !result) {
    const char *error = result ? vkr_project_json_text(result, "error") : NULL;
    *out_error = error && error[0]
                     ? vkr_project_strdup(job, error)
                     : vkr_project_printf(job, "%s exited %d", operation,
                                          (int)exit_code);
    return NULL;
  }
  return result;
}

VkrBakeryJson *vkr_project_pull_report(VkrProjectJob *job) {
  Arena *arena = job->arena;
  const VkrBakeryJson *project = vkr_project_document(job);
  if (!project) {
    return NULL;
  }
  /* The scenes the pull changed, by ID; none named means every scene. */
  const VkrBakeryJson *wanted = vkr_bakery_json_get(job->request, "scenes");
  if (wanted && wanted->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_project_fail(job, "scenes must be a list of scene IDs");
    return NULL;
  }
  char id[37];
  vkr_project_uuid4(id);
  const char *folder =
      vkr_project_printf(job, "%s/.staging/pull-%s", job->project_root, id);
  if (!vkr_project_make_dirs(job, folder)) {
    return NULL;
  }
  VkrBakeryJson *reports = vkr_bakery_json_array(arena);
  uint32_t open_count = 0u;
  const VkrBakeryJson *scenes = vkr_bakery_json_get(project, "scenes");
  for (const VkrBakeryJson *scene = scenes ? scenes->first : NULL; scene;
       scene = scene->next) {
    const char *scene_id = vkr_project_json_text(scene, "id");
    const char *relative = vkr_project_json_text(scene, "path");
    bool8_t listed = wanted == NULL || wanted->count == 0u;
    for (const VkrBakeryJson *item = wanted ? wanted->first : NULL;
         item && !listed; item = item->next) {
      listed = vkr_bakery_json_is_string(item, scene_id ? scene_id : "");
    }
    if (!scene_id || !relative || !listed) {
      continue;
    }
    VkrBakeryJson *report = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, report, "scene_id",
                        vkr_bakery_json_cstr(arena, scene_id));
    vkr_bakery_json_append(reports, report);
    char scene_path[VKR_PROJECT_PATH];
    if (!vkr_project_contained(job, job->project_root, relative, false_v,
                               scene_path)) {
      return NULL;
    }
    if (!vkr_bakery_is_file(scene_path)) {
      /* A project may list a scene this copy does not hold. */
      vkr_bakery_json_set(arena, report, "present",
                          vkr_bakery_json_bool(arena, false_v));
      continue;
    }
    vkr_bakery_json_set(arena, report, "present",
                        vkr_bakery_json_bool(arena, true_v));
    const char *error = NULL;
    const VkrBakeryJson *recook = vkr_pull_child(job, folder, "recook_textures",
                                                 scene_id, scene_path, &error);
    if (recook) {
      static const char *const fields[] = {"recooked", "skipped",
                                           "missing_source", "needs_rebuild"};
      for (uint32_t i = 0u; i < ArrayCount(fields); ++i) {
        const VkrBakeryJson *value = vkr_bakery_json_get(recook, fields[i]);
        if (value) {
          vkr_bakery_json_set(arena, report, fields[i],
                              vkr_bakery_json_clone(arena, value));
        }
      }
    } else {
      vkr_bakery_json_set(arena, report, "recook_error",
                          vkr_bakery_json_cstr(arena, error));
    }
    const VkrBakeryJson *prepared = vkr_pull_child(
        job, folder, "prepare_scene", scene_id, scene_path, &error);
    vkr_bakery_json_set(arena, report, "opens",
                        vkr_bakery_json_bool(arena, prepared != NULL));
    if (prepared) {
      open_count += 1u;
    } else {
      vkr_bakery_json_set(arena, report, "prepare_error",
                          vkr_bakery_json_cstr(arena, error));
    }
  }
  (void)vkr_project_remove_tree(folder);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "scenes", reports);
  vkr_bakery_json_set(arena, result, "scenes_open",
                      vkr_bakery_json_int(arena, open_count));
  return result;
}

// =============================================================================
// Drivers
// =============================================================================

VkrBakeryJson *vkr_project_merge(VkrProjectJob *job) {
  Arena *arena = job->arena;
  const bool8_t scene = strcmp(job->operation, "merge_scene_unit") == 0;
  char base[VKR_PROJECT_PATH];
  char mine[VKR_PROJECT_PATH];
  char theirs[VKR_PROJECT_PATH];
  char output[VKR_PROJECT_PATH];
  if (!vkr_merge_folder(job, "base", base) ||
      !vkr_merge_folder(job, "mine", mine) ||
      !vkr_merge_folder(job, "theirs", theirs) ||
      !vkr_merge_folder(job, "output", output)) {
    return NULL;
  }
  const char *anchor = vkr_project_json_text(job->request, "anchor");
  const char *expected = scene ? "scene.json" : "project.json";
  if (!anchor || strcmp(vkr_bakery_path_name(anchor), expected) != 0) {
    vkr_project_fail(job, "The %s driver merges units anchored on %s",
                     job->operation, expected);
    return NULL;
  }
  VkrMerge merge = {.job = job, .conflicts = vkr_bakery_json_array(arena)};
  VkrBakeryJson *written = vkr_bakery_json_array(arena);
  const char *roots[3] = {base, mine, theirs};
  if (!(scene ? vkr_merge_scene(&merge, roots, output, written)
              : vkr_merge_project(&merge, roots, output, written))) {
    return NULL;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  const bool8_t merged = merge.conflicts->count == 0u;
  vkr_bakery_json_set(arena, result, "merged",
                      vkr_bakery_json_bool(arena, merged));
  if (merged) {
    vkr_bakery_json_set(arena, result, "write", written);
    vkr_bakery_json_set(arena, result, "delete", vkr_bakery_json_array(arena));
  } else {
    vkr_bakery_json_set(arena, result, "conflicts", merge.conflicts);
  }
  return result;
}
