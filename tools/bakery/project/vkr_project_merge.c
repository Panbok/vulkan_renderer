#include "vkr_project_internal.h"

#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The depot's merge drivers (ADR-133, "Merge"):
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
 * entities when each kept the base list as its start. An overlay only one
 * side changed is taken whole; when both changed it, it merges per entity
 * (vkr_merge_overlay): overrides, deletions and created entities by the
 * entity they name, the fields of an entity both sides changed one by
 * one, and the scene and collision settings each as one value. The other
 * fields of `scene.json` merge one by one.
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

// =============================================================================
// Edit overlay, per entity
// =============================================================================

/* One side's overlay in the merge's own terms. Every `scene_entity` number
 * in its records, which indexes that file's `document_ids`, becomes the
 * entity's ID, and every `{"created": n}` becomes the created entity's
 * `uuid`, so the records of the three sides compare and the merged file
 * can number them anew. A record's key names its entity: `<entity>/<node>`
 * for an override or a deletion, the `uuid` for a created entity. */
typedef struct VkrMergeOverlay {
  const VkrBakeryJson *document;
  const VkrBakeryJson *document_ids;
  /* Created id, in decimal, to its uuid, and back. */
  VkrProjectSet created_uuids;
  VkrProjectSet created_ids;
  /* The records with references rewritten (created ones without `id`), in
     file order, and each key to its record. */
  VkrProjectNodes overrides;
  VkrProjectNodes created;
  VkrProjectSet override_keys;
  VkrProjectSet created_keys;
} VkrMergeOverlay;

vkr_internal const VkrBakeryJson *vkr_merge_node(const VkrProjectSet *set,
                                                 const char *key) {
  return (const VkrBakeryJson *)vkr_project_set_get(set, key);
}

vkr_internal void vkr_merge_overlay_free(VkrMergeOverlay *side) {
  vkr_project_set_free(&side->created_uuids);
  vkr_project_set_free(&side->created_ids);
  vkr_project_set_free(&side->override_keys);
  vkr_project_set_free(&side->created_keys);
}

vkr_internal void vkr_merge_set_text(Arena *arena, VkrBakeryJson *node,
                                     const char *text) {
  node->type = VKR_BAKERY_JSON_STRING;
  node->string = vkr_bakery_json_cstr(arena, text)->string;
}

/* The key a rewritten record is filed under; NULL for a record without
   the members that name its entity. */
vkr_internal const char *vkr_merge_key(VkrProjectJob *job,
                                       const VkrBakeryJson *record,
                                       bool8_t created) {
  if (created) {
    return vkr_project_json_text(record, "uuid");
  }
  const char *entity = vkr_project_json_text(record, "scene_entity");
  int64_t node = 0;
  return entity && vkr_bakery_json_get_int(record, "gltf_node", &node)
             ? vkr_project_printf(job, "%s/%lld", entity, (long long)node)
             : NULL;
}

/* Rewrites a record's entity references into IDs, in place. */
vkr_internal void vkr_merge_canonical(VkrProjectJob *job,
                                      const VkrMergeOverlay *side,
                                      VkrBakeryJson *node) {
  for (VkrBakeryJson *child = node->first; child; child = child->next) {
    const char *key = node->type == VKR_BAKERY_JSON_OBJECT
                          ? (const char *)child->key.str
                          : "";
    if (child->type == VKR_BAKERY_JSON_INT &&
        strcmp(key, "scene_entity") == 0) {
      const VkrBakeryJson *id =
          side->document_ids && child->integer >= 0
              ? vkr_bakery_json_at(side->document_ids, (uint32_t)child->integer)
              : NULL;
      const char *text = id ? vkr_bakery_json_cstr_value(job->arena, id) : NULL;
      /* A file without document IDs names entities by their index. */
      vkr_merge_set_text(
          job->arena, child,
          text ? text
               : vkr_project_printf(job, "#%lld", (long long)child->integer));
    } else if (child->type == VKR_BAKERY_JSON_INT &&
               strcmp(key, "created") == 0) {
      const char *uuid = vkr_project_set_get(
          &side->created_uuids,
          vkr_project_printf(job, "%lld", (long long)child->integer));
      vkr_merge_set_text(job->arena, child,
                         uuid ? uuid
                              : vkr_project_printf(job, "#missing-%lld",
                                                   (long long)child->integer));
    } else if (child->type == VKR_BAKERY_JSON_OBJECT ||
               child->type == VKR_BAKERY_JSON_ARRAY) {
      vkr_merge_canonical(job, side, child);
    }
  }
}

/* A copy of `object` without its member `key`. */
vkr_internal VkrBakeryJson *
vkr_merge_without(Arena *arena, const VkrBakeryJson *object, const char *key) {
  VkrBakeryJson *copy = vkr_bakery_json_object(arena);
  for (const VkrBakeryJson *field = object->first; field; field = field->next) {
    if (strcmp((const char *)field->key.str, key) != 0) {
      vkr_bakery_json_set(arena, copy, (const char *)field->key.str,
                          vkr_bakery_json_clone(arena, field));
    }
  }
  return copy;
}

/* Files a rewritten record under its key; false for a record without one
   or a key the side files twice (the loader keeps such duplicates
   unapplied, so they do not merge per entity). */
vkr_internal bool8_t vkr_merge_file(VkrProjectJob *job, VkrProjectSet *keys,
                                    VkrProjectNodes *records,
                                    VkrBakeryJson *record, bool8_t created) {
  const char *key = vkr_merge_key(job, record, created);
  if (!key || vkr_project_set_has(keys, key)) {
    return false_v;
  }
  if (!vkr_project_set_add(keys, key, (const char *)record) ||
      !vkr_project_nodes_push(job, records, record)) {
    return vkr_project_fail(job, "Out of memory");
  }
  return true_v;
}

/* Reads one side's overlay; a NULL `document` reads as one without edits.
   False, with the job not failed, when the file cannot merge per entity:
   an older version, a created entity without `uuid`, or a duplicate. */
vkr_internal bool8_t vkr_merge_overlay_read(VkrProjectJob *job,
                                            const VkrBakeryJson *document,
                                            VkrMergeOverlay *side) {
  Arena *arena = job->arena;
  side->document = document;
  if (!document) {
    return true_v;
  }
  int64_t version = 0;
  if (document->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bakery_json_get_int(document, "version", &version) || version < 4) {
    return false_v;
  }
  side->document_ids = vkr_bakery_json_get(document, "document_ids");
  const VkrBakeryJson *created = vkr_bakery_json_get(document, "created");
  for (const VkrBakeryJson *item = created ? created->first : NULL; item;
       item = item->next) {
    int64_t id = 0;
    const char *uuid = vkr_project_json_text(item, "uuid");
    if (!uuid || !vkr_bakery_json_get_int(item, "id", &id)) {
      return false_v;
    }
    const char *number = vkr_project_printf(job, "%lld", (long long)id);
    if (!vkr_project_set_add(&side->created_uuids, number, uuid) ||
        !vkr_project_set_add(&side->created_ids, uuid, number)) {
      return vkr_project_fail(job, "Out of memory");
    }
  }
  const VkrBakeryJson *overrides = vkr_bakery_json_get(document, "overrides");
  for (const VkrBakeryJson *item = overrides ? overrides->first : NULL; item;
       item = item->next) {
    VkrBakeryJson *record = vkr_bakery_json_clone(arena, item);
    vkr_merge_canonical(job, side, record);
    if (!vkr_merge_file(job, &side->override_keys, &side->overrides, record,
                        false_v)) {
      return false_v;
    }
  }
  for (const VkrBakeryJson *item = created ? created->first : NULL; item;
       item = item->next) {
    VkrBakeryJson *record = vkr_merge_without(arena, item, "id");
    vkr_merge_canonical(job, side, record);
    if (!vkr_merge_file(job, &side->created_keys, &side->created, record,
                        true_v)) {
      return false_v;
    }
  }
  return true_v;
}

/* One entity's merged record, NULL when it leaves or conflicts. When both
   sides changed it, a deletion against a change, or a side without it, is
   a conflict; otherwise its fields merge one by one and its components by
   type. A record names every field the entity has, so a field only one
   side changed from the base takes that side, while an entity the base
   did not edit conflicts on each field the sides hold apart. */
vkr_internal VkrBakeryJson *vkr_merge_entity(VkrMerge *merge, const char *key,
                                             const VkrBakeryJson *base,
                                             const VkrBakeryJson *mine,
                                             const VkrBakeryJson *theirs) {
  VkrProjectJob *job = merge->job;
  Arena *arena = job->arena;
  bool8_t conflict = false_v;
  bool8_t present = false_v;
  const VkrBakeryJson *chosen =
      vkr_merge_choose(base, mine, theirs, &conflict, &present);
  if (!conflict) {
    return present ? vkr_bakery_json_clone(arena, chosen) : NULL;
  }
  const char *name = vkr_project_json_text(mine ? mine : theirs, "name");
  const char *path = vkr_project_printf(job, "edits %s%s(%s)", name ? name : "",
                                        name ? " " : "", key);
  if (!mine || !theirs) {
    vkr_merge_conflict(merge, path, NULL,
                       "changed on one side, removed on the other");
    return NULL;
  }
  if (vkr_bakery_json_get(mine, "deleted") ||
      vkr_bakery_json_get(theirs, "deleted")) {
    vkr_merge_conflict(merge, path, NULL,
                       "deleted on one side, changed on the other");
    return NULL;
  }
  static const char *const skip[] = {"components"};
  VkrBakeryJson *merged =
      vkr_merge_fields(merge, path, base, mine, theirs, skip, ArrayCount(skip));
  const VkrBakeryJson *components[3] = {
      base ? vkr_bakery_json_get(base, "components") : NULL,
      vkr_bakery_json_get(mine, "components"),
      vkr_bakery_json_get(theirs, "components")};
  if (components[1] || components[2]) {
    vkr_bakery_json_set(
        arena, merged, "components",
        vkr_merge_fields(merge, vkr_project_printf(job, "%s components", path),
                         components[0], components[1], components[2], NULL,
                         0u));
  }
  return merged;
}

/* Merges one list (`overrides` or `created`) by key: mine's records in
   their order, then theirs' that mine lacks, and deletions after the rest,
   as the writer puts them. */
vkr_internal bool8_t vkr_merge_entity_list(VkrMerge *merge,
                                           const VkrMergeOverlay sides[3],
                                           bool8_t created,
                                           VkrProjectNodes *out) {
  VkrProjectJob *job = merge->job;
  VkrProjectNodes deletions = {0};
  for (uint32_t s = 1u; s <= 2u; ++s) {
    const VkrProjectNodes *records =
        created ? &sides[s].created : &sides[s].overrides;
    for (uint32_t i = 0u; i < records->count; ++i) {
      const char *key = vkr_merge_key(job, records->items[i], created);
      const VkrProjectSet *keys[3];
      for (uint32_t k = 0u; k < 3u; ++k) {
        keys[k] = created ? &sides[k].created_keys : &sides[k].override_keys;
      }
      if (s == 2u && vkr_project_set_has(keys[1], key)) {
        continue;
      }
      VkrBakeryJson *merged = vkr_merge_entity(
          merge, key, vkr_merge_node(keys[0], key),
          vkr_merge_node(keys[1], key), vkr_merge_node(keys[2], key));
      if (!merged) {
        continue;
      }
      VkrProjectNodes *list =
          vkr_bakery_json_get(merged, "deleted") ? &deletions : out;
      VKR_PROJECT_TRY(vkr_project_nodes_push(job, list, merged));
    }
  }
  for (uint32_t i = 0u; i < deletions.count; ++i) {
    VKR_PROJECT_TRY(vkr_project_nodes_push(job, out, deletions.items[i]));
  }
  return true_v;
}

/* The merged file's numbering: the merged scene's entity IDs as
   `document_ids`, extended by IDs records name that the scene lost, and
   the created entities' ids. */
typedef struct VkrMergeNumbers {
  VkrBakeryJson *document_ids;
  VkrProjectSet entity_index;
  VkrProjectSet created_ids;
} VkrMergeNumbers;

/* Rewrites a merged record's IDs into the merged file's numbers, in place;
   false for a parent the merge removed. */
vkr_internal bool8_t vkr_merge_numbered(VkrProjectJob *job,
                                        VkrMergeNumbers *numbers,
                                        VkrBakeryJson *node) {
  for (VkrBakeryJson *child = node->first; child; child = child->next) {
    const char *key = node->type == VKR_BAKERY_JSON_OBJECT
                          ? (const char *)child->key.str
                          : "";
    const char *text = child->type == VKR_BAKERY_JSON_STRING
                           ? vkr_bakery_json_cstr_value(job->arena, child)
                           : NULL;
    if (text && strcmp(key, "scene_entity") == 0) {
      int64_t index = 0;
      if (text[0] == '#') {
        index = strtoll(text + 1, NULL, 10);
      } else {
        const char *known = vkr_project_set_get(&numbers->entity_index, text);
        if (!known) {
          /* An entity the merged scene lost keeps a place after its own,
             as the editor writes a kept record's lost ID. */
          known = vkr_project_printf(job, "%u", numbers->document_ids->count);
          vkr_bakery_json_append(numbers->document_ids,
                                 vkr_bakery_json_cstr(job->arena, text));
          if (!vkr_project_set_add(&numbers->entity_index, text, known)) {
            return vkr_project_fail(job, "Out of memory");
          }
        }
        index = strtoll(known, NULL, 10);
      }
      child->type = VKR_BAKERY_JSON_INT;
      child->integer = index;
    } else if (text && strcmp(key, "created") == 0) {
      const char *id = vkr_project_set_get(&numbers->created_ids, text);
      if (!id) {
        return false_v;
      }
      child->type = VKR_BAKERY_JSON_INT;
      child->integer = strtoll(id, NULL, 10);
    } else if (child->type == VKR_BAKERY_JSON_OBJECT ||
               child->type == VKR_BAKERY_JSON_ARRAY) {
      VKR_PROJECT_TRY(vkr_merge_numbered(job, numbers, child));
    }
  }
  return true_v;
}

/* Gives each merged created entity an id: its own where no earlier one
   took it, else one past every id any side used. */
vkr_internal bool8_t vkr_merge_created_ids(VkrProjectJob *job,
                                           const VkrMergeOverlay sides[3],
                                           const VkrProjectNodes *created,
                                           VkrMergeNumbers *numbers) {
  int64_t next = 1;
  for (uint32_t s = 0u; s < 3u; ++s) {
    const VkrProjectSet *ids = &sides[s].created_ids;
    for (uint32_t slot = 0u; slot < ids->capacity; ++slot) {
      if (ids->keys[slot]) {
        next = Max(next, strtoll(ids->values[slot], NULL, 10) + 1);
      }
    }
  }
  VkrProjectSet taken = {0};
  bool8_t ok = true_v;
  for (uint32_t pass = 0u; pass < 2u && ok; ++pass) {
    for (uint32_t i = 0u; i < created->count && ok; ++i) {
      const char *uuid = vkr_project_json_text(created->items[i], "uuid");
      if (vkr_project_set_has(&numbers->created_ids, uuid)) {
        continue;
      }
      const char *own = vkr_project_set_get(&sides[1].created_ids, uuid);
      own = own ? own : vkr_project_set_get(&sides[2].created_ids, uuid);
      const char *id = NULL;
      if (pass == 0u && own && !vkr_project_set_has(&taken, own)) {
        id = own;
      } else if (pass == 1u) {
        id = vkr_project_printf(job, "%lld", (long long)next++);
      }
      if (id) {
        ok = vkr_project_set_add(&taken, id, uuid) &&
             vkr_project_set_add(&numbers->created_ids, uuid, id);
      }
    }
  }
  vkr_project_set_free(&taken);
  return ok ? true_v : vkr_project_fail(job, "Out of memory");
}

/* The whole-object members of the overlay merge as one value each. */
vkr_internal void vkr_merge_overlay_member(VkrMerge *merge,
                                           const VkrMergeOverlay sides[3],
                                           const char *key,
                                           VkrBakeryJson *out) {
  Arena *arena = merge->job->arena;
  const VkrBakeryJson *values[3];
  for (uint32_t s = 0u; s < 3u; ++s) {
    values[s] =
        sides[s].document ? vkr_bakery_json_get(sides[s].document, key) : NULL;
  }
  bool8_t conflict = false_v;
  bool8_t present = false_v;
  const VkrBakeryJson *chosen =
      vkr_merge_choose(values[0], values[1], values[2], &conflict, &present);
  if (conflict) {
    vkr_merge_conflict(merge, "edits", key, "changed on both sides");
  } else if (present) {
    vkr_bakery_json_set(arena, out, key, vkr_bakery_json_clone(arena, chosen));
  }
}

/* Merges three overlays both sides changed, per entity, for a scene whose
   merged entities are `entities`. NULL with `*out_whole` when a file does
   not merge per entity (the unit then conflicts as a whole), or NULL with
   the conflicts recorded. */
vkr_internal VkrBakeryJson *
vkr_merge_overlay(VkrMerge *merge, const VkrBakeryJson *const overlays[3],
                  const VkrBakeryJson *entities, bool8_t *out_whole) {
  VkrProjectJob *job = merge->job;
  Arena *arena = job->arena;
  *out_whole = false_v;
  VkrMergeOverlay sides[3] = {0};
  VkrMergeNumbers numbers = {.document_ids = vkr_bakery_json_array(arena)};
  VkrProjectNodes overrides = {0};
  VkrProjectNodes created = {0};
  VkrBakeryJson *document = NULL;
  const uint32_t conflicts_before = merge->conflicts->count;
  for (uint32_t s = 0u; s < 3u && !*out_whole; ++s) {
    *out_whole = !vkr_merge_overlay_read(job, overlays[s], &sides[s]);
  }
  if (*out_whole || job->failed ||
      !vkr_merge_entity_list(merge, sides, false_v, &overrides) ||
      !vkr_merge_entity_list(merge, sides, true_v, &created)) {
    goto cleanup;
  }
  /* The merged scene's entity IDs, when every entity has one. */
  bool8_t ids = entities && entities->count > 0u;
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL;
       entity && ids; entity = entity->next) {
    const char *id = vkr_project_json_text(entity, "id");
    ids = id && !vkr_project_set_has(&numbers.entity_index, id) &&
          vkr_project_set_add(
              &numbers.entity_index, id,
              vkr_project_printf(job, "%u", numbers.document_ids->count));
    if (ids) {
      vkr_bakery_json_append(numbers.document_ids,
                             vkr_bakery_json_cstr(arena, id));
    }
  }
  if (!ids) {
    vkr_project_set_free(&numbers.entity_index);
    numbers.entity_index = (VkrProjectSet){0};
    numbers.document_ids = vkr_bakery_json_array(arena);
  }
  if (!vkr_merge_created_ids(job, sides, &created, &numbers)) {
    goto cleanup;
  }
  VkrBakeryJson *override_list = vkr_bakery_json_array(arena);
  VkrBakeryJson *created_list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < overrides.count + created.count; ++i) {
    const bool8_t is_created = i >= overrides.count;
    VkrBakeryJson *record =
        is_created ? created.items[i - overrides.count] : overrides.items[i];
    const char *key = vkr_merge_key(job, record, is_created);
    if (!vkr_merge_numbered(job, &numbers, record)) {
      if (job->failed) {
        goto cleanup;
      }
      vkr_merge_conflict(merge, vkr_project_printf(job, "edits (%s)", key),
                         "parent", "its parent was removed on the other side");
      continue;
    }
    if (!is_created) {
      vkr_bakery_json_append(override_list, record);
      continue;
    }
    /* `id` first, as the editor writes it. */
    VkrBakeryJson *numbered = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, numbered, "id",
        vkr_bakery_json_int(
            arena,
            strtoll(vkr_project_set_get(&numbers.created_ids,
                                        vkr_project_json_text(record, "uuid")),
                    NULL, 10)));
    for (VkrBakeryJson *field = record->first; field; field = field->next) {
      vkr_bakery_json_set(arena, numbered, (const char *)field->key.str,
                          vkr_bakery_json_clone(arena, field));
    }
    vkr_bakery_json_append(created_list, numbered);
  }
  /* A record whose scene entity has no merged ID cannot be numbered. */
  if (!ids && numbers.document_ids->count > 0u) {
    *out_whole = true_v;
    goto cleanup;
  }
  if (merge->conflicts->count > conflicts_before) {
    goto cleanup;
  }
  /* The writer's order: `document_ids` before the records it numbers, and
     the settings last. */
  document = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, document, "version",
                      vkr_bakery_json_int(arena, 5));
  if (numbers.document_ids->count > 0u) {
    vkr_bakery_json_set(arena, document, "document_ids", numbers.document_ids);
  }
  vkr_bakery_json_set(arena, document, "overrides", override_list);
  bool8_t has_created = created_list->count > 0u;
  for (uint32_t s = 1u; s <= 2u && !has_created; ++s) {
    has_created = sides[s].document &&
                  vkr_bakery_json_get(sides[s].document, "created") != NULL;
  }
  if (has_created) {
    vkr_bakery_json_set(arena, document, "created", created_list);
  }
  vkr_merge_overlay_member(merge, sides, "scene_settings", document);
  vkr_merge_overlay_member(merge, sides, "collision_settings", document);
  if (merge->conflicts->count > conflicts_before) {
    document = NULL;
  }
cleanup:
  for (uint32_t s = 0u; s < 3u; ++s) {
    vkr_merge_overlay_free(&sides[s]);
  }
  vkr_project_set_free(&numbers.entity_index);
  vkr_project_set_free(&numbers.created_ids);
  return job->failed ? NULL : document;
}

/* Writes a merged overlay on one line, as the editor writes it, so it
   stays within the size the editor loads; a larger one is a conflict. */
vkr_internal bool8_t vkr_merge_write_overlay(VkrMerge *merge,
                                             const char *output,
                                             const char *relative,
                                             const VkrBakeryJson *value,
                                             VkrBakeryJson *written) {
  VkrProjectJob *job = merge->job;
  char path[VKR_PROJECT_PATH];
  char folder[VKR_PROJECT_PATH];
  String8 text = {0};
  VKR_PROJECT_TRY(vkr_project_contained(job, output, relative, false_v, path));
  if (!vkr_bakery_json_write(job->arena, value, VKR_BAKERY_JSON_COMPACT,
                             &text)) {
    return vkr_project_fail(job, "The merged edits could not be written");
  }
  if (text.length > VKR_PROJECT_MAX_OVERLAY_BYTES) {
    vkr_merge_conflict(merge, "edits", NULL,
                       "the merged edits are larger than the editor loads");
    return true_v;
  }
  vkr_bakery_path_parent(folder, sizeof(folder), path);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, folder));
  if (!vkr_bakery_write_file_atomic(path, text.str, text.length)) {
    return vkr_project_fail(job, "Cannot write %s", path);
  }
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
  /* One side's overlay when only it changed; else they merge per entity. */
  bool8_t overlay_conflict = false_v;
  bool8_t overlay_present = false_v;
  const VkrBakeryJson *overlay =
      vkr_merge_choose(overlays[0], overlays[1], overlays[2], &overlay_conflict,
                       &overlay_present);
  VkrBakeryJson *merged_overlay = NULL;
  if (overlay_conflict) {
    bool8_t whole = false_v;
    merged_overlay = vkr_merge_overlay(
        merge, overlays,
        entities ? entities : vkr_bakery_json_get(scenes[1], "entities"),
        &whole);
    VKR_PROJECT_TRY(!job->failed);
    if (whole) {
      vkr_merge_conflict(merge, "edits", NULL,
                         "both sides edited the scene, and an overlay does "
                         "not merge per entity (an older version, a created "
                         "entity without uuid, or one entity twice)");
    }
  }
  if (merge->conflicts->count) {
    return true_v;
  }
  if (entities) {
    vkr_bakery_json_set(arena, scene, "entities", entities);
  }
  if (merged_overlay) {
    char id[37];
    vkr_project_uuid4(id);
    const char *name = vkr_project_printf(job, "edits/%s.json", id);
    VKR_PROJECT_TRY(
        vkr_merge_write_overlay(merge, output, name, merged_overlay, written));
    if (merge->conflicts->count) {
      return true_v;
    }
    vkr_bakery_json_set(arena, scene, "edit_overlay",
                        vkr_bakery_json_cstr(arena, name));
  } else if (overlay_present) {
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
