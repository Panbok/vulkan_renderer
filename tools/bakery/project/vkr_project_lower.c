#include "vkr_project_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lowering a managed scene to its runtime view, preparing unbuilt scenes,
 * project asset imports and the scene asset edit operations. */

vkr_internal const char *const vkr_project_faces[] = {"r", "l", "u",
                                                      "d", "f", "b"};

vkr_internal int vkr_project_compare_keys(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

/* Python dict equality of two string-to-value objects. */
vkr_internal bool8_t vkr_project_same_mapping(const VkrBakeryJson *lhs,
                                              const VkrBakeryJson *rhs) {
  if (!lhs || !rhs || lhs->type != VKR_BAKERY_JSON_OBJECT ||
      rhs->type != VKR_BAKERY_JSON_OBJECT || lhs->count != rhs->count) {
    return false_v;
  }
  for (const VkrBakeryJson *field = lhs->first; field; field = field->next) {
    const VkrBakeryJson *other =
        vkr_bakery_json_get(rhs, (const char *)field->key.str);
    if (!other || !vkr_bakery_json_equal(field, other)) {
      return false_v;
    }
  }
  return true_v;
}

/* Digest map {owner-relative path: sha256} of a resolved path set, in
   reference order. */
vkr_internal VkrBakeryJson *vkr_project_closure(VkrProjectJob *job,
                                                const VkrBakeryJson *paths,
                                                const char *owner) {
  Arena *arena = job->arena;
  VkrProjectStrings keys = {0};
  /* A new revision's files are not in the index yet; hash them together. */
  VkrProjectStrings files = {0};
  for (const VkrBakeryJson *entry = paths->first; entry; entry = entry->next) {
    if (!vkr_project_strings_push(job, &files, (const char *)entry->key.str)) {
      return NULL;
    }
  }
  vkr_bakery_index_prefetch(vkr_project_index(job),
                            (const char *const *)files.items, files.count);
  for (const VkrBakeryJson *entry = paths->first; entry; entry = entry->next) {
    const char *reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    /* "reference\nhash" sorts by reference; references hold no newline. */
    if (!vkr_project_managed_reference(job, (const char *)entry->key.str, owner,
                                       &reference) ||
        !vkr_project_digest(job, (const char *)entry->key.str, hash) ||
        !vkr_project_strings_push(
            job, &keys, vkr_project_printf(job, "%s\n%s", reference, hash))) {
      return NULL;
    }
  }
  if (keys.count) {
    qsort(keys.items, keys.count, sizeof(keys.items[0]),
          vkr_project_compare_keys);
  }
  VkrBakeryJson *closure = vkr_bakery_json_object(arena);
  for (uint32_t i = 0u; i < keys.count; ++i) {
    const char *separator = strchr(keys.items[i], '\n');
    const char *reference = vkr_project_printf(
        job, "%.*s", (int)(separator - keys.items[i]), keys.items[i]);
    vkr_bakery_json_set(arena, closure, reference,
                        vkr_bakery_json_cstr(arena, separator + 1));
  }
  return closure;
}

// =============================================================================
// Lowering
// =============================================================================

typedef struct VkrProjectLowering {
  VkrProjectJob *job;
  VkrBakeryJson *scene;
  VkrBakeryJson *project;
  VkrProjectInventory inventories[3];
  uint32_t inventory_count;
  VkrBakeryJson *fonts;
} VkrProjectLowering;

vkr_internal const VkrProjectInventory *
vkr_project_lowering_scope(const VkrProjectLowering *lowering,
                           const VkrBakeryJson *scope) {
  for (uint32_t i = 0u; i < lowering->inventory_count; ++i) {
    if (vkr_bakery_json_is_string(scope, lowering->inventories[i].scope)) {
      return &lowering->inventories[i];
    }
  }
  return NULL;
}

vkr_internal const char *vkr_project_record_label(const VkrBakeryJson *record,
                                                  const char *fallback) {
  const VkrBakeryJson *name = vkr_bakery_json_get(record, "name");
  if (name && name->type == VKR_BAKERY_JSON_STRING) {
    return (const char *)name->string.str;
  }
  return fallback;
}

/* A lowered file reference: the resolved path, replaced in portable mode by
   its package content identity. */
vkr_internal bool8_t vkr_project_lowered_path(VkrProjectJob *job, char *path) {
  if (!job->portable) {
    return true_v;
  }
  char identity[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_project_portable_identity(job, path, identity));
  MemCopy(path, identity, strlen(identity) + 1u);
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_asset(
    VkrProjectLowering *lowering, const VkrBakeryJson *reference,
    const char *default_role, char *out_path, const VkrBakeryJson **out_record,
    const VkrProjectInventory **out_owner) {
  VkrProjectJob *job = lowering->job;
  const VkrProjectInventory *inventory =
      reference && reference->type == VKR_BAKERY_JSON_OBJECT
          ? vkr_project_lowering_scope(lowering,
                                       vkr_bakery_json_get(reference, "scope"))
          : NULL;
  if (!inventory) {
    return vkr_project_fail(job, "Missing %s asset scope", default_role);
  }
  const VkrBakeryJson *id = vkr_bakery_json_get(reference, "id");
  const VkrBakeryJson *record = NULL;
  uint32_t matches = 0u;
  for (const VkrBakeryJson *item =
           inventory->records ? inventory->records->first : NULL;
       item; item = item->next) {
    const VkrBakeryJson *item_id = vkr_bakery_json_get(item, "id");
    if ((!id && !item_id) ||
        (id && item_id && vkr_bakery_json_equal(id, item_id))) {
      record = item;
      matches += 1u;
    }
  }
  if (matches != 1u) {
    return vkr_project_fail(
        job, "Missing or duplicate %s asset: %s", default_role,
        id && id->type == VKR_BAKERY_JSON_STRING ? (const char *)id->string.str
                                                 : "None");
  }
  const VkrBakeryJson *role_value = vkr_bakery_json_get(reference, "role");
  const char *role =
      role_value ? vkr_project_json_text(reference, "role") : default_role;
  if (!role || strcmp(role, default_role) != 0) {
    return vkr_project_fail(job, "Expected %s artifact role, got %s",
                            default_role, role ? role : "None");
  }
  const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
  const VkrBakeryJson *product = NULL;
  uint32_t products = 0u;
  for (const VkrBakeryJson *item = artifacts ? artifacts->first : NULL; item;
       item = item->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "role"), role)) {
      product = item;
      products += 1u;
    }
  }
  if (products != 1u) {
    return vkr_project_fail(job,
                            "Asset %s has no unique %s artifact; rebuild it",
                            vkr_project_record_label(record, role), role);
  }
  VKR_PROJECT_TRY(vkr_project_contained(job, inventory->owner,
                                        vkr_project_json_text(product, "path"),
                                        true_v, out_path));
  if (!vkr_bakery_is_file(out_path)) {
    return vkr_project_fail(job, "Asset is not a file: %s",
                            vkr_bakery_path_name(out_path));
  }
  VKR_PROJECT_TRY(vkr_project_lowered_path(job, out_path));
  if (out_record) {
    *out_record = record;
  }
  if (out_owner) {
    *out_owner = inventory;
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_font(VkrProjectLowering *lowering,
                                            const VkrBakeryJson *reference,
                                            const char **out_name) {
  VkrProjectJob *job = lowering->job;
  Arena *arena = job->arena;
  if (!vkr_project_truthy(reference)) {
    reference = vkr_bakery_json_get(lowering->scene, "default_font");
  }
  if (!vkr_project_truthy(reference)) {
    reference = vkr_bakery_json_get(lowering->project, "default_font");
  }
  const bool8_t editor_default =
      reference && reference->type == VKR_BAKERY_JSON_OBJECT &&
      reference->count == 2u &&
      vkr_bakery_json_is_string(vkr_bakery_json_get(reference, "scope"),
                                "editor") &&
      vkr_bakery_json_is_string(vkr_bakery_json_get(reference, "id"),
                                "default-scene-font");
  bool8_t editor_inventory = false_v;
  for (uint32_t i = 0u; i < lowering->inventory_count; ++i) {
    editor_inventory = editor_inventory ||
                       strcmp(lowering->inventories[i].scope, "editor") == 0;
  }
  if (!reference || reference->type == VKR_BAKERY_JSON_NULL ||
      (editor_default && !editor_inventory)) {
    /* C bootstrap always registers this explicit editor font. */
    *out_name = "default-scene-font";
    return true_v;
  }
  char path[VKR_PROJECT_PATH];
  const VkrBakeryJson *record = NULL;
  VKR_PROJECT_TRY(vkr_project_lower_asset(lowering, reference, "font", path,
                                          &record, NULL));
  const char *name = vkr_project_json_text(record, "id");
  VkrBakeryJson *font = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, font, "name", vkr_bakery_json_cstr(arena, name));
  vkr_bakery_json_set(arena, font, "config", vkr_bakery_json_cstr(arena, path));
  vkr_bakery_json_set(arena, lowering->fonts, name, font);
  *out_name = name;
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_cube(VkrProjectLowering *lowering,
                                            VkrBakeryJson *component,
                                            const char *default_role) {
  VkrProjectJob *job = lowering->job;
  Arena *arena = job->arena;
  VkrBakeryJson *reference = vkr_bakery_json_get(component, "asset");
  const bool8_t legacy = vkr_bakery_json_get(component, "cubemap") != NULL;
  if (!reference) {
    /* A probe the editor added waits for its first bake: it lowers without
       a cubemap and marked pending, so the runtime keeps it off rather than
       reflecting the environment (ADR-103). */
    if (vkr_project_truthy(vkr_bakery_json_get(component, "enabled"))) {
      vkr_bakery_json_set(job->arena, component, "bake_pending",
                          vkr_bakery_json_bool(job->arena, true_v));
    }
    if (legacy) {
      return vkr_project_fail(job,
                              "Managed probe still contains a legacy path");
    }
    return true_v;
  }
  if (legacy) {
    return vkr_project_fail(job,
                            "Managed probe mixes asset and legacy path fields");
  }
  vkr_bakery_json_remove(component, "asset");
  char path[VKR_PROJECT_PATH];
  const VkrBakeryJson *record = NULL;
  const VkrProjectInventory *owner = NULL;
  VKR_PROJECT_TRY(vkr_project_lower_asset(lowering, reference, default_role,
                                          path, &record, &owner));
  const char *kind = vkr_project_json_text(record, "source_kind");
  VkrBakeryJson *cubemap = vkr_bakery_json_object(arena);
  if (kind && strcmp(kind, "faces") == 0) {
    char base[VKR_PROJECT_PATH];
    const char *extension = vkr_project_json_text(record, "extension");
    const char *base_reference = NULL;
    VKR_PROJECT_TRY(vkr_project_contained(
        job, owner->owner, vkr_project_json_text(record, "base_path"), false_v,
        base));
    VKR_PROJECT_TRY(vkr_project_managed_reference(job, base, owner->owner,
                                                  &base_reference));
    for (uint32_t f = 0u; f < ArrayCount(vkr_project_faces); ++f) {
      char face[VKR_PROJECT_PATH];
      VKR_PROJECT_TRY(vkr_project_contained(
          job, owner->owner,
          vkr_project_printf(job, "%s_%s.%s", base_reference,
                             vkr_project_faces[f], extension ? extension : ""),
          true_v, face));
    }
    VKR_PROJECT_TRY(vkr_project_lowered_path(job, base));
    vkr_bakery_json_set(arena, cubemap, "base_path",
                        vkr_bakery_json_cstr(arena, base));
    vkr_bakery_json_set(
        arena, cubemap, "extension",
        vkr_bakery_json_cstr(arena, extension ? extension : ""));
  } else {
    vkr_bakery_json_set(arena, cubemap, "path",
                        vkr_bakery_json_cstr(arena, path));
  }
  vkr_bakery_json_set(arena, component, "cubemap", cubemap);
  return true_v;
}

vkr_internal bool8_t vkr_project_check_inventory(
    VkrProjectJob *job, VkrProjectInventory *inventory) {
  Arena *arena = job->arena;
  VkrBakeryJson *records = inventory->records;
  if (!records || records->type != VKR_BAKERY_JSON_ARRAY ||
      records->count > VKR_PROJECT_MAX_IMPORT_FILES) {
    return vkr_project_fail(job, "Asset inventory exceeds the supported bound");
  }
  VkrBakeryJson *ids = vkr_bakery_json_object(arena);
  for (VkrBakeryJson *record = records->first; record; record = record->next) {
    const VkrBakeryJson *id = vkr_bakery_json_get(record, "id");
    if (record->type != VKR_BAKERY_JSON_OBJECT || !vkr_project_truthy(id) ||
        id->type != VKR_BAKERY_JSON_STRING ||
        vkr_bakery_json_get(ids, (const char *)id->string.str)) {
      return vkr_project_fail(job, "Duplicate or invalid asset identifier");
    }
    vkr_bakery_json_set(arena, ids, (const char *)id->string.str,
                        vkr_bakery_json_bool(arena, true_v));
    VkrBakeryJson *record_paths = vkr_bakery_json_object(arena);
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    uint32_t index = 0u;
    for (const VkrBakeryJson *product = artifacts ? artifacts->first : NULL;
         product; product = product->next, ++index) {
      char path[VKR_PROJECT_PATH];
      VKR_PROJECT_TRY(vkr_project_contained(
          job, inventory->owner, vkr_project_json_text(product, "path"), true_v,
          path));
      if (!vkr_bakery_is_file(path)) {
        return vkr_project_fail(job, "Asset artifact is not a regular file");
      }
      const VkrBakeryJson *expected =
          vkr_bakery_json_get(product, "fingerprint");
      if (!vkr_project_truthy(expected) && index == 0u) {
        expected = vkr_bakery_json_get(record, "fingerprint");
      }
      if (vkr_project_truthy(expected)) {
        char hash[VKR_BAKERY_SHA256_HEX];
        VKR_PROJECT_TRY(vkr_project_digest(job, path, hash));
        if (!vkr_bakery_json_is_string(
                expected, vkr_project_printf(job, "sha256:%s", hash))) {
          return vkr_project_fail(
              job, "Asset bytes changed: %s; rebuild or reimport it",
              vkr_project_record_label(record, vkr_bakery_path_name(path)));
        }
      }
      VKR_PROJECT_TRY(vkr_project_validate_bundle_dependencies(
          job, inventory->owner, path, record_paths));
    }
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(record, "source_kind"),
                                  "faces")) {
      const char *base = vkr_project_json_text(record, "base_path");
      const char *extension = vkr_project_json_text(record, "extension");
      for (uint32_t f = 0u; f < ArrayCount(vkr_project_faces); ++f) {
        char face[VKR_PROJECT_PATH];
        VKR_PROJECT_TRY(vkr_project_contained(
            job, inventory->owner,
            vkr_project_printf(job, "%s_%s.%s", base ? base : "",
                               vkr_project_faces[f],
                               extension ? extension : ""),
            true_v, face));
        vkr_bakery_json_set(arena, record_paths, face,
                            vkr_bakery_json_bool(arena, true_v));
      }
    }
    const VkrBakeryJson *closure = vkr_bakery_json_get(record, "closure");
    VkrBakeryJson *current =
        vkr_project_closure(job, record_paths, inventory->owner);
    VKR_PROJECT_TRY(current);
    if (closure && closure->type != VKR_BAKERY_JSON_NULL &&
        !vkr_project_same_mapping(closure, current)) {
      return vkr_project_fail(
          job, "Asset dependency closure changed: %s; rebuild or reimport it",
          vkr_project_record_label(record, (const char *)id->string.str));
    }
    if (record_paths->count && !job->read_only) {
      vkr_bakery_json_set(arena, record, "closure", current);
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_animation(VkrProjectJob *job,
                                                 const VkrBakeryJson *component,
                                                 const VkrBakeryJson *record) {
  static const char *const known[] = {"clip", "rate", "loop", "playing",
                                      "controller"};
  for (const VkrBakeryJson *field = component->first; field;
       field = field->next) {
    bool8_t listed = false_v;
    for (uint32_t k = 0u; k < ArrayCount(known); ++k) {
      listed = listed ||
               (field->key.length == strlen(known[k]) &&
                MemCompare(field->key.str, known[k], field->key.length) == 0);
    }
    if (!listed) {
      return vkr_project_fail(job, "Animation has unknown playback fields");
    }
  }
  const VkrBakeryJson *controller =
      vkr_bakery_json_get(component, "controller");
  if (controller && controller->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_project_fail(job,
                            "Animation controller must be a versioned object");
  }
  int64_t clip = 0;
  int64_t animation_count = 0;
  const VkrBakeryJson *clip_value = vkr_bakery_json_get(component, "clip");
  (void)vkr_project_integer(vkr_bakery_json_get(record, "animation_count"),
                            &animation_count);
  if ((clip_value && !vkr_project_integer(clip_value, &clip)) || clip < 0 ||
      clip >= animation_count) {
    return vkr_project_fail(job, "Animation clip is unavailable in the "
                                 "rebuilt bank; select a valid clip");
  }
  const VkrBakeryJson *rate_value = vkr_bakery_json_get(component, "rate");
  float64_t rate = 1.0;
  if ((rate_value && !vkr_project_number(rate_value, &rate)) ||
      !isfinite(rate)) {
    return vkr_project_fail(job, "Animation playback rate must be finite");
  }
  const VkrBakeryJson *loop = vkr_bakery_json_get(component, "loop");
  const VkrBakeryJson *playing = vkr_bakery_json_get(component, "playing");
  if ((loop && loop->type != VKR_BAKERY_JSON_BOOL) ||
      (playing && playing->type != VKR_BAKERY_JSON_BOOL)) {
    return vkr_project_fail(job, "Animation loop and playing must be boolean");
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_entity(VkrProjectLowering *lowering,
                                              VkrBakeryJson *entity,
                                              bool8_t keep_ids) {
  VkrProjectJob *job = lowering->job;
  Arena *arena = job->arena;
  if (!keep_ids) {
    vkr_bakery_json_remove(entity, "id");
  }
  VkrBakeryJson *components[3] = {
      vkr_bakery_json_get(entity, "mesh"),
      vkr_bakery_json_get(entity, "animation"),
      vkr_bakery_json_get(vkr_bakery_json_get(entity, "shape"), "material")};
  static const char *const roles[] = {"mesh", "animation", "material"};
  for (uint32_t c = 0u; c < ArrayCount(roles); ++c) {
    VkrBakeryJson *component = components[c];
    if (!vkr_project_truthy(component)) {
      continue;
    }
    if (vkr_bakery_json_get(component, "path")) {
      return vkr_project_fail(job, "Managed %s contains a legacy path",
                              roles[c]);
    }
    VkrBakeryJson *reference = vkr_bakery_json_get(component, "asset");
    if (!reference) {
      return vkr_project_fail(
          job, "Managed %s requires a typed asset reference", roles[c]);
    }
    vkr_bakery_json_remove(component, "asset");
    char resolved[VKR_PROJECT_PATH];
    const VkrBakeryJson *record = NULL;
    VKR_PROJECT_TRY(vkr_project_lower_asset(lowering, reference, roles[c],
                                            resolved, &record, NULL));
    if (c == 1u) {
      VKR_PROJECT_TRY(vkr_project_lower_animation(job, component, record));
    }
    vkr_bakery_json_set(arena, component, "path",
                        vkr_bakery_json_cstr(arena, resolved));
  }
  VkrBakeryJson *text = vkr_bakery_json_get(entity, "text3d");
  if (text && text->type == VKR_BAKERY_JSON_OBJECT) {
    const char *name = NULL;
    VKR_PROJECT_TRY(vkr_project_lower_font(
        lowering, vkr_bakery_json_get(text, "font"), &name));
    vkr_bakery_json_set(arena, text, "font", vkr_bakery_json_cstr(arena, name));
  }
  return true_v;
}

vkr_internal void vkr_project_inventory_init(VkrProjectInventory *inventory,
                                             Arena *arena, const char *scope,
                                             const char *owner,
                                             VkrBakeryJson *records) {
  inventory->scope = scope;
  (void)snprintf(inventory->owner, sizeof(inventory->owner), "%s", owner);
  inventory->records = records ? records : vkr_bakery_json_array(arena);
}

/* A portable overlay names each collision asset, a workspace-relative path
   in the workspace, by the content identity of the file it resolves to, as
   vkr_project_validate_overlay_dependencies resolves it below `root`. */
vkr_internal bool8_t vkr_project_portable_overlay(VkrProjectJob *job,
                                                  VkrBakeryJson *overlay,
                                                  const char *root) {
  VkrProjectNodes colliders;
  VKR_PROJECT_TRY(vkr_project_overlay_colliders(job, overlay, &colliders));
  const char *final_reference = NULL;
  if (colliders.count && job->scene_id) {
    VKR_PROJECT_TRY(vkr_project_managed_reference(
        job, job->final_path, job->workspace, &final_reference));
  }
  for (uint32_t i = 0u; i < colliders.count; ++i) {
    const char *value = vkr_project_json_text(colliders.items[i], "asset");
    VKR_PROJECT_TRY(vkr_project_validate_managed_path(job, value));
    const uint64_t length = final_reference ? strlen(final_reference) : 0u;
    char path[VKR_PROJECT_PATH];
    if (final_reference && strncmp(value, final_reference, length) == 0 &&
        value[length] == '/') {
      VKR_PROJECT_TRY(
          vkr_project_contained(job, root, value + length + 1u, true_v, path));
    } else {
      VKR_PROJECT_TRY(
          vkr_project_contained(job, job->workspace, value, true_v, path));
    }
    char identity[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(vkr_project_portable_identity(job, path, identity));
    vkr_bakery_json_set(job->arena, colliders.items[i], "asset",
                        vkr_bakery_json_cstr(job->arena, identity));
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_lower_overlay(VkrProjectJob *job,
                                               const VkrBakeryJson *scene,
                                               const char *root,
                                               char *edit_path) {
  Arena *arena = job->arena;
  const bool8_t has_overlay =
      vkr_project_truthy(vkr_bakery_json_get(scene, "edit_overlay"));
  if (has_overlay) {
    VKR_PROJECT_TRY(vkr_project_contained(
        job, root, vkr_project_json_text(scene, "edit_overlay"), true_v,
        edit_path));
    if (!vkr_bakery_is_file(edit_path)) {
      return vkr_project_fail(
          job, "Selected authored override journal is unavailable");
    }
    VkrBakeryJson *journal =
        vkr_project_load_json(job, edit_path, VKR_PROJECT_MAX_JSON_BYTES);
    VKR_PROJECT_TRY(journal);
    VKR_PROJECT_TRY(vkr_project_checked_overlay(job, journal));
    VKR_PROJECT_TRY(
        vkr_project_validate_overlay_dependencies(job, journal, root));
  } else {
    (void)snprintf(edit_path, VKR_PROJECT_PATH, "%s/edits/scene.editor.json",
                   root);
  }
  if (job->read_only) {
    VkrBakeryJson *selected = NULL;
    if (has_overlay) {
      selected =
          vkr_project_load_json(job, edit_path, VKR_PROJECT_MAX_JSON_BYTES);
      VKR_PROJECT_TRY(selected);
    } else {
      selected = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, selected, "version",
                          vkr_bakery_json_int(arena, 1));
      vkr_bakery_json_set(arena, selected, "overrides",
                          vkr_bakery_json_array(arena));
    }
    if (job->portable) {
      VKR_PROJECT_TRY(vkr_project_portable_overlay(job, selected, root));
    }
    (void)vkr_bakery_path_join(edit_path, VKR_PROJECT_PATH,
                               job->runtime_directory, "scene.editor.json");
    return vkr_project_atomic_json(job, edit_path, selected);
  }
  char parent[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(parent, sizeof(parent), edit_path);
  return vkr_project_make_dirs(job, parent);
}

VkrBakeryJson *vkr_project_lower(VkrProjectJob *job, VkrBakeryJson *scene,
                                 const char *root, bool8_t publish_runtime) {
  Arena *arena = job->arena;
  int64_t version = 0;
  if (!vkr_project_integer(vkr_bakery_json_get(scene, "version"), &version) ||
      (version != 3 && version != 4 && version != VKR_PROJECT_SCENE_VERSION) ||
      !vkr_bakery_json_is_string(vkr_bakery_json_get(scene, "id"),
                                 job->scene_id)) {
    vkr_project_fail(job, "Managed scene version or ID mismatch");
    return NULL;
  }
  VkrProjectLowering lowering = {.job = job, .scene = scene};
  lowering.project = vkr_project_document(job);
  if (!lowering.project) {
    return NULL;
  }
  lowering.fonts = vkr_bakery_json_object(arena);
  vkr_project_inventory_init(&lowering.inventories[0], arena, "scene", root,
                             vkr_bakery_json_get(scene, "assets"));
  vkr_project_inventory_init(&lowering.inventories[1], arena, "project",
                             job->project_root,
                             vkr_bakery_json_get(lowering.project, "assets"));
  lowering.inventory_count = 2u;
  char editor_manifest[VKR_PROJECT_PATH];
  (void)snprintf(editor_manifest, sizeof(editor_manifest),
                 "%s/editor/bundles/1/manifest.json", job->workspace);
  if (!vkr_bakery_is_file(editor_manifest)) {
    (void)snprintf(editor_manifest, sizeof(editor_manifest),
                   "%s/editor/bundle.json", job->workspace);
  }
  if (vkr_bakery_is_file(editor_manifest)) {
    VkrBakeryJson *manifest =
        vkr_project_load_json(job, editor_manifest, VKR_PROJECT_MAX_JSON_BYTES);
    if (!manifest) {
      return NULL;
    }
    char owner[VKR_PROJECT_PATH];
    vkr_bakery_path_parent(owner, sizeof(owner), editor_manifest);
    vkr_project_inventory_init(&lowering.inventories[2], arena, "editor", owner,
                               vkr_bakery_json_get(manifest, "assets"));
    lowering.inventory_count = 3u;
  }
  vkr_project_bind_model_animations(job, scene, lowering.inventories,
                                    lowering.inventory_count);
  for (uint32_t i = 0u; i < lowering.inventory_count; ++i) {
    if (!vkr_project_check_inventory(job, &lowering.inventories[i])) {
      return NULL;
    }
  }
  VkrBakeryJson *runtime = vkr_bakery_json_clone(arena, scene);
  vkr_bakery_json_set(arena, runtime, "version", vkr_bakery_json_int(arena, 2));
  vkr_bakery_json_set(arena, runtime, "source_identity",
                      vkr_bakery_json_cstr(arena, job->scene_id));
  static const char *const dropped[] = {"id", "assets", "default_font",
                                        "bake_recipes", "edit_overlay"};
  for (uint32_t i = 0u; i < ArrayCount(dropped); ++i) {
    vkr_bakery_json_remove(runtime, dropped[i]);
  }
  VkrBakeryJson *probes = vkr_bakery_json_get(runtime, "reflection_probes");
  for (VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    if (!vkr_project_lower_cube(&lowering, probe, "probe-cube")) {
      return NULL;
    }
  }
  /* Baked scene blocks reference their managed artifact; the runtime reads
     a path (ADR-054, ADR-087). */
  static const struct {
    const char *key;
    const char *role;
  } baked_blocks[] = {{"diffuse_volume", "volume"}, {"lightmaps", "lightmap"}};
  for (uint32_t b = 0u; b < ArrayCount(baked_blocks); ++b) {
    VkrBakeryJson *block = vkr_bakery_json_get(runtime, baked_blocks[b].key);
    if (!vkr_project_truthy(block)) {
      continue;
    }
    if (vkr_bakery_json_get(block, "path")) {
      vkr_project_fail(job, "Managed %s contains a legacy path",
                       baked_blocks[b].key);
      return NULL;
    }
    VkrBakeryJson *reference = vkr_bakery_json_get(block, "asset");
    if (reference) {
      vkr_bakery_json_remove(block, "asset");
      char path[VKR_PROJECT_PATH];
      if (!vkr_project_lower_asset(&lowering, reference, baked_blocks[b].role,
                                   path, NULL, NULL)) {
        return NULL;
      }
      vkr_bakery_json_set(arena, block, "path",
                          vkr_bakery_json_cstr(arena, path));
    }
  }
  /* Document ids reach the runtime, which binds overlays through them; a
     document missing any id binds by index (ADR-076). */
  VkrBakeryJson *entities = vkr_bakery_json_get(runtime, "entities");
  VkrBakeryJson *ids = NULL;
  const bool8_t failed_before = job->failed;
  const bool8_t keep_ids = vkr_project_document_entity_ids(
      job, entities ? entities : vkr_bakery_json_array(arena), &ids);
  vkr_project_forgive(job, failed_before);
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    if (!vkr_project_lower_entity(&lowering, entity, keep_ids)) {
      return NULL;
    }
  }
  /* Register the selected scene font before any scene text is
     instantiated. */
  const char *default_name = NULL;
  char edit_path[VKR_PROJECT_PATH];
  if (!vkr_project_lower_font(&lowering, NULL, &default_name) ||
      !vkr_project_lower_overlay(job, scene, root, edit_path)) {
    return NULL;
  }
  String8 canonical = {0};
  if (!vkr_bakery_json_write(arena, runtime, VKR_BAKERY_JSON_PYTHON_SORTED,
                             &canonical)) {
    vkr_project_fail(job, "Out of range float values are not JSON compliant");
    return NULL;
  }
  char cache_key[VKR_BAKERY_SHA256_HEX];
  vkr_bakery_hash_bytes(canonical.str, canonical.length, cache_key);
  char runtime_path[VKR_PROJECT_PATH];
  if (job->read_only) {
    (void)snprintf(runtime_path, sizeof(runtime_path), "%s/%s.scene.json",
                   job->runtime_directory, cache_key);
  } else {
    (void)snprintf(runtime_path, sizeof(runtime_path),
                   "%s/.runtime/%s.scene.json", root, cache_key);
  }
  if (publish_runtime && !vkr_project_atomic_json(job, runtime_path, runtime)) {
    return NULL;
  }
  VkrBakeryJson *fonts = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *font = lowering.fonts->first; font;
       font = font->next) {
    vkr_bakery_json_append(fonts, vkr_bakery_json_clone(arena, font));
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "scene_id",
                      vkr_bakery_json_cstr(arena, job->scene_id));
  vkr_bakery_json_set(
      arena, result, "scene_path",
      vkr_bakery_json_cstr(arena,
                           vkr_project_printf(job, "%s/scene.json", root)));
  vkr_bakery_json_set(arena, result, "runtime_path",
                      vkr_bakery_json_cstr(arena, runtime_path));
  vkr_bakery_json_set(arena, result, "edit_path",
                      vkr_bakery_json_cstr(arena, edit_path));
  vkr_bakery_json_set(arena, result, "fonts", fonts);
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  /* Assets still at the preview texture tier; the editor finalizes them. */
  vkr_bakery_json_set(
      arena, result, "preview_assets",
      vkr_bakery_json_int(arena, vkr_project_count_preview(
                                     vkr_bakery_json_get(scene, "assets"))));
  return result;
}

/* Every string of the World that names a file relative to the document
   (`./` or `../`) must resolve to an existing file inside the project, and no
   string may be an absolute path: a package holds neither. */
vkr_internal bool8_t vkr_project_check_world_value(VkrProjectJob *job,
                                                   const VkrBakeryJson *value) {
  if (value->type == VKR_BAKERY_JSON_STRING) {
    const char *text = vkr_project_printf(
        job, "%.*s", (int)value->string.length, value->string.str);
    if (vkr_bakery_path_is_absolute(text)) {
      return vkr_project_fail(job,
                              "The World names an absolute path, which a "
                              "package cannot hold: %s",
                              text);
    }
    if (strncmp(text, "./", 2u) == 0 || strncmp(text, "../", 3u) == 0) {
      char path[VKR_PROJECT_PATH];
      const char *query = strchr(text, '?');
      const char *relative =
          query ? vkr_project_printf(job, "%.*s", (int)(query - text), text)
                : text;
      VKR_PROJECT_TRY(vkr_project_contained(job, job->project_root,
                                            relative + 2u, true_v, path));
      if (!vkr_bakery_is_file(path)) {
        return vkr_project_fail(job, "The World names a missing file: %s",
                                text);
      }
    }
    return true_v;
  }
  for (const VkrBakeryJson *child = value->first; child; child = child->next) {
    VKR_PROJECT_TRY(vkr_project_check_world_value(job, child));
  }
  return true_v;
}

VkrBakeryJson *vkr_project_package_world(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (!job->read_only || !job->portable) {
    vkr_project_fail(job, "package_world requires a read-only portable job");
    return NULL;
  }
  if (!vkr_project_progress(job, "Packaging the World", 0.2, "")) {
    return NULL;
  }
  char document[VKR_PROJECT_PATH];
  char overlay_path[VKR_PROJECT_PATH];
  char world_path[VKR_PROJECT_PATH] = {0};
  char edit_path[VKR_PROJECT_PATH] = {0};
  (void)snprintf(document, sizeof(document), "%s/world.scene.json",
                 job->project_root);
  (void)snprintf(overlay_path, sizeof(overlay_path), "%s/world.editor.json",
                 job->project_root);
  if (vkr_bakery_is_file(document)) {
    /* The document ships byte-identical: without a source identity, the
       overlay binds World entities by a fingerprint of these bytes. */
    const char *text = NULL;
    uint64_t length = 0u;
    if (!vkr_project_read_text(job, document, VKR_PROJECT_MAX_JSON_BYTES, &text,
                               &length)) {
      return NULL;
    }
    VkrBakeryJsonError error = {0};
    const VkrBakeryJson *world = vkr_bakery_json_parse(
        arena, (const uint8_t *)text, length, 256u, &error);
    if (!world || world->type != VKR_BAKERY_JSON_OBJECT) {
      vkr_project_fail(job, "The World document is not valid JSON: %s",
                       error.message);
      return NULL;
    }
    (void)vkr_bakery_path_join(world_path, sizeof(world_path),
                               job->runtime_directory, "world.scene.json");
    if (!vkr_project_check_world_value(job, world) ||
        !vkr_project_write_text(job, world_path, text, length)) {
      return NULL;
    }
  }
  if (vkr_bakery_is_file(overlay_path)) {
    VkrBakeryJson *overlay =
        vkr_project_load_json(job, overlay_path, VKR_PROJECT_MAX_JSON_BYTES);
    (void)vkr_bakery_path_join(edit_path, sizeof(edit_path),
                               job->runtime_directory, "world.editor.json");
    if (!overlay || !vkr_project_checked_overlay(job, overlay) ||
        !vkr_project_validate_overlay_dependencies(job, overlay,
                                                   job->project_root) ||
        !vkr_project_portable_overlay(job, overlay, job->project_root) ||
        !vkr_project_atomic_json(job, edit_path, overlay)) {
      return NULL;
    }
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "world_path",
                      vkr_bakery_json_cstr(arena, world_path));
  vkr_bakery_json_set(arena, result, "edit_path",
                      vkr_bakery_json_cstr(arena, edit_path));
  vkr_bakery_json_set(arena, result, "fonts", vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  return result;
}

// =============================================================================
// Preparing a scene
// =============================================================================

vkr_internal bool8_t vkr_project_is_material_name(const char *name) {
  const uint64_t length = strlen(name);
  return length >= 3u && strcmp(name + length - 3u, ".mt") == 0;
}

vkr_internal bool8_t vkr_project_bundle_needs_textures(VkrProjectJob *job,
                                                       const char *bundle,
                                                       bool8_t *out) {
  *out = false_v;
  char materials[VKR_PROJECT_PATH];
  (void)snprintf(materials, sizeof(materials), "%s/materials", bundle);
  VkrProjectStrings names;
  VKR_PROJECT_TRY(vkr_project_list(job, materials, &names));
  for (uint32_t i = 0u; i < names.count && !*out; ++i) {
    if (!vkr_project_is_material_name(names.items[i])) {
      continue;
    }
    char path[VKR_PROJECT_PATH];
    const char *text = NULL;
    VkrProjectStrings lines;
    (void)vkr_bakery_path_join(path, sizeof(path), materials, names.items[i]);
    VKR_PROJECT_TRY(vkr_project_read_text(job, path, VKR_PROJECT_MAX_JSON_BYTES,
                                          &text, NULL));
    VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
    for (uint32_t l = 0u; l < lines.count && !*out; ++l) {
      char key[256];
      char value[VKR_PROJECT_PATH];
      if (!vkr_project_partition_line(lines.items[l], key, sizeof(key), value,
                                      sizeof(value))) {
        continue;
      }
      const uint64_t key_length = strlen(key);
      if (key_length < 8u || strcmp(key + key_length - 8u, "_texture") != 0 ||
          !value[0]) {
        continue;
      }
      char *query = strchr(value, '?');
      if (query) {
        *query = 0;
      }
      char suffix[16];
      vkr_project_suffix_lower(value, suffix, sizeof(suffix));
      *out = strcmp(suffix, ".vkt") != 0;
    }
  }
  return true_v;
}

bool8_t vkr_project_texture_repair_bundles(VkrProjectJob *job,
                                           const VkrBakeryJson *scene,
                                           const char *root,
                                           VkrProjectStrings *out) {
  MemZero(out, sizeof(*out));
  const VkrBakeryJson *assets = vkr_bakery_json_get(scene, "assets");
  for (const VkrBakeryJson *record = assets ? assets->first : NULL; record;
       record = record->next) {
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    for (const VkrBakeryJson *artifact = artifacts ? artifacts->first : NULL;
         artifact; artifact = artifact->next) {
      const char *path = vkr_project_json_text(artifact, "path");
      if (!path || strncmp(path, "builds/", 7u) != 0) {
        continue;
      }
      const char *revision = path + 7;
      const char *slash = strchr(revision, '/');
      if (!slash || slash == revision || !slash[1]) {
        continue;
      }
      char bundle[VKR_PROJECT_PATH];
      VKR_PROJECT_TRY(vkr_project_contained(
          job, root,
          vkr_project_printf(job, "builds/%.*s", (int)(slash - revision),
                             revision),
          true_v, bundle));
      bool8_t needed = false_v;
      if (!vkr_bakery_is_file(vkr_project_printf(
              job, "%s/" VKR_PROJECT_PACKED_MARKER, bundle))) {
        VKR_PROJECT_TRY(
            vkr_project_bundle_needs_textures(job, bundle, &needed));
      }
      bool8_t listed = false_v;
      for (uint32_t i = 0u; i < out->count && !listed; ++i) {
        listed = strcmp(out->items[i], bundle) == 0;
      }
      if (needed && !listed) {
        VKR_PROJECT_TRY(vkr_project_strings_push(
            job, out, vkr_project_strdup(job, bundle)));
      }
    }
  }
  return true_v;
}

VkrBakeryJson *vkr_project_prepare(VkrProjectJob *job, const char *scene_path) {
  char path[VKR_PROJECT_PATH];
  char expected_path[VKR_PROJECT_PATH];
  char expected[VKR_PROJECT_PATH];
  if (!scene_path ||
      !vkr_project_resolve(scene_path, true_v, path, sizeof(path))) {
    vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                     scene_path ? scene_path : "None");
    return NULL;
  }
  (void)snprintf(expected_path, sizeof(expected_path), "%s/scene.json",
                 job->final_path);
  (void)vkr_project_resolve(expected_path, false_v, expected, sizeof(expected));
  if (strcmp(path, expected) != 0) {
    vkr_project_fail(job, "Scene path does not match its project membership");
    return NULL;
  }
  if (!vkr_project_progress(job, "Resolving scene assets", 0.2, "")) {
    return NULL;
  }
  uint8_t *scene_bytes = NULL;
  uint64_t scene_length = 0u;
  if (!vkr_bakery_read_file(path, 0u, &scene_bytes, &scene_length)) {
    vkr_project_fail(job, "Cannot read %s", path);
    return NULL;
  }
  char project_before[VKR_BAKERY_SHA256_HEX] = {0};
  char directory[VKR_PROJECT_PATH];
  VkrBakeryJson *result = NULL;
  VkrBakeryJson *scene = NULL;
  VkrProjectStrings repairs = {0};
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  if (job->read_only &&
      !vkr_project_digest(job, job->project_path, project_before)) {
    goto cleanup;
  }
  scene = vkr_project_read_managed_scene(job, path);
  if (!scene || !vkr_project_migrate_source_references(job, scene, path) ||
      !vkr_project_validate_semantics(job, scene) ||
      !vkr_project_texture_repair_bundles(job, scene, directory, &repairs)) {
    goto cleanup;
  }
  if (vkr_project_has_unbuilt(vkr_bakery_json_get(scene, "assets")) ||
      vkr_project_pending_bakes(scene) || repairs.count) {
    if (job->read_only) {
      vkr_project_fail(job, "Scene has unbuilt assets or needs texture "
                            "preparation; open with write access and "
                            "prepare it first");
      goto cleanup;
    }
    result = vkr_project_prepare_unbuilt(job, path, scene);
    goto cleanup;
  }
  result = vkr_project_lower(job, scene, directory, true_v);
  if (!result) {
    if (job->read_only && !job->cancelled) {
      char message[sizeof(job->error)];
      (void)snprintf(message, sizeof(message),
                     "%s; open with write access to repair the scene, then "
                     "retry",
                     job->error);
      (void)snprintf(job->error, sizeof(job->error), "%s", message);
    }
    goto cleanup;
  }
  if (job->read_only) {
    uint8_t *after = NULL;
    uint64_t after_length = 0u;
    char project_after[VKR_BAKERY_SHA256_HEX] = {0};
    const bool8_t same =
        vkr_bakery_read_file(path, 0u, &after, &after_length) &&
        after_length == scene_length &&
        MemCompare(after, scene_bytes, scene_length) == 0 &&
        vkr_bakery_hash_file(job->project_path, project_after, NULL) &&
        strcmp(project_after, project_before) == 0;
    free(after);
    if (!same) {
      vkr_project_fail(job, "Workspace changed during read-only preparation; "
                            "retry opening the scene");
      result = NULL;
      goto cleanup;
    }
    char fingerprint[17];
    vkr_project_fingerprint(scene_bytes, scene_length, fingerprint);
    vkr_bakery_json_set(job->arena, result, "manifest_fingerprint",
                        vkr_bakery_json_cstr(job->arena, fingerprint));
  }
cleanup:
  free(scene_bytes);
  return result;
}

/* Publishes each child of stage/<folder> under `destination_base/<folder>`
   for the folders named, recording every destination in `published`. */
vkr_internal bool8_t vkr_project_publish_folders(VkrProjectJob *job,
                                                 const char *const *folders,
                                                 uint32_t folder_count,
                                                 const char *destination_base,
                                                 const char *collision,
                                                 VkrProjectStrings *published) {
  for (uint32_t f = 0u; f < folder_count; ++f) {
    char incoming[VKR_PROJECT_PATH];
    char destination_root[VKR_PROJECT_PATH];
    (void)snprintf(incoming, sizeof(incoming), "%s/%s", job->stage, folders[f]);
    if (!vkr_project_exists(incoming)) {
      continue;
    }
    (void)snprintf(destination_root, sizeof(destination_root), "%s/%s",
                   destination_base, folders[f]);
    VKR_PROJECT_TRY(vkr_project_make_dirs(job, destination_root));
    VkrProjectStrings names;
    VKR_PROJECT_TRY(vkr_project_list(job, incoming, &names));
    for (uint32_t i = 0u; i < names.count; ++i) {
      char source[VKR_PROJECT_PATH];
      char destination[VKR_PROJECT_PATH];
      (void)vkr_bakery_path_join(source, sizeof(source), incoming,
                                 names.items[i]);
      (void)vkr_bakery_path_join(destination, sizeof(destination),
                                 destination_root, names.items[i]);
      if (vkr_project_exists(destination)) {
        return vkr_project_fail(job, "%s", collision);
      }
      if (vkr_bakery_is_directory(source)) {
        VKR_PROJECT_TRY(
            vkr_project_publish_directory(job, source, destination));
      } else if (!vkr_bakery_rename(source, destination, false_v)) {
        return vkr_project_fail(job, "Cannot publish %s", destination);
      } else {
        vkr_bakery_index_move_tree(job->index, source, destination);
      }
      VKR_PROJECT_TRY(vkr_project_strings_push(
          job, published, vkr_project_strdup(job, destination)));
    }
  }
  return true_v;
}

/* Reads the display name and texture seeds of one repository material a
   cooked bundle was imported from. A failure stops that material only. */
vkr_internal void vkr_project_repair_material(VkrProjectJob *job,
                                              const char *original,
                                              const VkrBakeryJson *entry) {
  Arena *arena = job->arena;
  const bool8_t failed_before = job->failed;
  char material[VKR_PROJECT_PATH];
  char joined[VKR_PROJECT_PATH];
  char key[VKR_PROJECT_PATH];
  const char *text = NULL;
  VkrProjectStrings lines = {0};
  if (entry->type != VKR_BAKERY_JSON_STRING ||
      !vkr_project_legacy_source(job, (const char *)entry->key.str, original,
                                 job->legacy_root, material)) {
    vkr_project_forgive(job, failed_before);
    return;
  }
  (void)vkr_bakery_path_join(joined, sizeof(joined), original,
                             (const char *)entry->string.str);
  (void)vkr_project_resolve(joined, false_v, key, sizeof(key));
  char stem[512];
  vkr_project_stem(material, stem, sizeof(stem));
  vkr_bakery_json_set(arena, job->asset_names, key,
                      vkr_bakery_json_cstr(arena, stem));
  if (!vkr_project_read_text(job, material, VKR_PROJECT_MAX_JSON_BYTES, &text,
                             NULL) ||
      !vkr_project_split_lines(job, text, &lines)) {
    vkr_project_forgive(job, failed_before);
    return;
  }
  char material_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(material_directory, sizeof(material_directory),
                         material);
  for (uint32_t l = 0u; l < lines.count; ++l) {
    char field[256];
    char value[VKR_PROJECT_PATH];
    if (!vkr_project_partition_line(lines.items[l], field, sizeof(field), value,
                                    sizeof(value)) ||
        !value[0]) {
      continue;
    }
    const uint64_t field_length = strlen(field);
    if (strcmp(field, "name") == 0) {
      vkr_bakery_json_set(arena, job->asset_names, key,
                          vkr_bakery_json_cstr(arena, value));
      continue;
    }
    if (field_length < 8u ||
        strcmp(field + field_length - 8u, "_texture") != 0) {
      continue;
    }
    char *query = strchr(value, '?');
    if (query) {
      *query = 0;
    }
    char source[VKR_PROJECT_PATH];
    char hash[VKR_BAKERY_SHA256_HEX];
    if (!vkr_project_legacy_source(job, value, material_directory,
                                   job->legacy_root, source) ||
        !vkr_project_digest(job, source, hash)) {
      vkr_project_forgive(job, failed_before);
      return;
    }
    char source_stem[512];
    vkr_project_stem(source, source_stem, sizeof(source_stem));
    vkr_bakery_json_set(arena, job->source_names, hash,
                        vkr_bakery_json_cstr(arena, source_stem));
    char seed[VKR_PROJECT_PATH];
    (void)snprintf(seed, sizeof(seed), "%s.vkt", source);
    if (vkr_bakery_is_file(seed)) {
      vkr_bakery_json_set(arena, job->texture_seeds, hash,
                          vkr_bakery_json_cstr(arena, seed));
    }
  }
}

vkr_internal bool8_t vkr_project_repair_names(VkrProjectJob *job,
                                              const char *original) {
  VkrProjectStrings names;
  VKR_PROJECT_TRY(vkr_project_list(job, original, &names));
  for (uint32_t i = 0u; i < names.count; ++i) {
    const uint64_t length = strlen(names.items[i]);
    if (length < 15u ||
        strcmp(names.items[i] + length - 15u, ".vkb.remap.json") != 0) {
      continue;
    }
    char sidecar[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(sidecar, sizeof(sidecar), original,
                               names.items[i]);
    VkrBakeryJson *document =
        vkr_project_load_json(job, sidecar, VKR_PROJECT_MAX_JSON_BYTES);
    VKR_PROJECT_TRY(document);
    const VkrBakeryJson *materials = vkr_bakery_json_get(document, "materials");
    for (const VkrBakeryJson *entry = materials ? materials->first : NULL;
         entry; entry = entry->next) {
      vkr_project_repair_material(job, original, entry);
    }
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_repair_records(VkrProjectJob *job,
                                                const char *prefix,
                                                const char *replacement) {
  Arena *arena = job->arena;
  const uint64_t prefix_length = strlen(prefix);
  for (VkrBakeryJson *record = job->assets->first; record;
       record = record->next) {
    bool8_t changed = false_v;
    VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    for (VkrBakeryJson *product = artifacts ? artifacts->first : NULL; product;
         product = product->next) {
      const char *path = vkr_project_json_text(product, "path");
      if (path && strncmp(path, prefix, prefix_length) == 0) {
        vkr_bakery_json_set(
            arena, product, "path",
            vkr_bakery_json_cstr(arena,
                                 vkr_project_printf(job, "%s%s", replacement,
                                                    path + prefix_length)));
        vkr_bakery_json_remove(product, "fingerprint");
        changed = true_v;
      }
    }
    if (!changed) {
      continue;
    }
    const char *source = vkr_project_json_text(record, "source");
    if (source && strncmp(source, prefix, prefix_length) == 0) {
      vkr_bakery_json_set(
          arena, record, "source",
          vkr_bakery_json_cstr(arena,
                               vkr_project_printf(job, "%s%s", replacement,
                                                  source + prefix_length)));
    }
    vkr_bakery_json_remove(record, "closure");
    char product[VKR_PROJECT_PATH];
    char hash[VKR_BAKERY_SHA256_HEX];
    (void)vkr_bakery_path_join(product, sizeof(product), job->stage,
                               vkr_project_json_text(artifacts->first, "path"));
    VKR_PROJECT_TRY(vkr_project_digest(job, product, hash));
    const VkrBakeryJson *name = vkr_bakery_json_get(job->asset_names, product);
    if (!vkr_project_truthy(name)) {
      name = vkr_bakery_json_get(job->source_names, hash);
    }
    if (name) {
      vkr_bakery_json_set(arena, record, "name",
                          vkr_bakery_json_clone(arena, name));
    }
    vkr_bakery_json_set(arena, record, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_repair_bundle(VkrProjectJob *job,
                                               const char *original,
                                               const char *scene_root) {
  Arena *arena = job->arena;
  VKR_PROJECT_TRY(vkr_project_repair_names(job, original));
  char id[37];
  vkr_project_uuid4(id);
  char revision[VKR_PROJECT_PATH];
  (void)snprintf(revision, sizeof(revision), "%s/builds/%s", job->stage, id);
  VkrProjectStrings files;
  VKR_PROJECT_TRY(vkr_project_walk_files(job, original, &files, false_v, NULL));
  for (uint32_t i = 0u; i < files.count; ++i) {
    char relative[VKR_PROJECT_PATH];
    char copied[VKR_PROJECT_PATH];
    char resolved[VKR_PROJECT_PATH];
    if (!vkr_bakery_path_relative(original, files.items[i], relative,
                                  sizeof(relative)) ||
        !vkr_bakery_path_join(copied, sizeof(copied), revision, relative)) {
      return vkr_project_fail(job, "Path too long");
    }
    VKR_PROJECT_TRY(vkr_project_copy_file(job, files.items[i], copied));
    (void)vkr_project_resolve(files.items[i], false_v, resolved,
                              sizeof(resolved));
    const VkrBakeryJson *name = vkr_bakery_json_get(job->asset_names, resolved);
    if (name) {
      vkr_bakery_json_set(arena, job->asset_names, copied,
                          vkr_bakery_json_clone(arena, name));
    }
  }
  char materials_directory[VKR_PROJECT_PATH];
  (void)snprintf(materials_directory, sizeof(materials_directory),
                 "%s/materials", revision);
  VkrProjectStrings names;
  VKR_PROJECT_TRY(vkr_project_list(job, materials_directory, &names));
  VkrProjectStrings materials = {0};
  for (uint32_t i = 0u; i < names.count; ++i) {
    if (vkr_project_is_material_name(names.items[i])) {
      VKR_PROJECT_TRY(vkr_project_strings_push(
          job, &materials,
          vkr_project_printf(job, "%s/%s", materials_directory,
                             names.items[i])));
    }
  }
  VKR_PROJECT_TRY(vkr_project_pack_bundle_textures(job, &materials));
  for (uint32_t i = 0u; i < materials.count; ++i) {
    VKR_PROJECT_TRY(vkr_project_point_material(job, materials.items[i]));
  }
  const char *prefix = NULL;
  const char *replacement = NULL;
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, original, scene_root, &prefix));
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, revision, job->stage, &replacement));
  return vkr_project_repair_records(
      job, vkr_project_printf(job, "%s/", prefix),
      vkr_project_printf(job, "%s/", replacement));
}

vkr_internal bool8_t vkr_project_build_record(VkrProjectJob *job,
                                              VkrBakeryJson *record,
                                              const char *scene_root) {
  Arena *arena = job->arena;
  const char *kind = vkr_project_json_text(record, "kind");
  char id[37];
  vkr_project_uuid4(id);
  char bundle[VKR_PROJECT_PATH];
  char output[VKR_PROJECT_PATH];
  char source[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage, id);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, bundle));
  if (kind && strcmp(kind, "mesh") == 0) {
    const char *import_id = vkr_project_json_text(record, "import_id");
    VKR_PROJECT_TRY(vkr_project_contained(
        job, scene_root, vkr_project_json_text(record, "source"), true_v,
        source));
    (void)snprintf(output, sizeof(output), "%s/mesh.vkb", bundle);
    VKR_PROJECT_TRY(vkr_project_cook_mesh(
        job, source, output, bundle, import_id ? import_id : "",
        vkr_project_record_lightmap_density(record), "Preparing model"));
    VKR_PROJECT_TRY(vkr_project_index_bundle(job, bundle, import_id));
  } else if (kind && strcmp(kind, "font") == 0) {
    char config[VKR_PROJECT_PATH];
    char copied[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(vkr_project_contained(
        job, scene_root,
        vkr_project_json_text(vkr_bakery_json_get(record, "recipe"), "config"),
        true_v, config));
    VKR_PROJECT_TRY(vkr_project_contained(
        job, scene_root, vkr_project_json_text(record, "source"), true_v,
        source));
    (void)vkr_bakery_path_join(copied, sizeof(copied), bundle,
                               vkr_bakery_path_name(source));
    VKR_PROJECT_TRY(vkr_project_copy_file(job, source, copied));
    (void)vkr_bakery_path_join(output, sizeof(output), bundle,
                               vkr_bakery_path_name(config));
    VKR_PROJECT_TRY(vkr_project_copy_file(job, config, output));
    const char *arguments[] = {"--config", output};
    VKR_PROJECT_TRY(vkr_project_run_tool(job, "font", arguments,
                                         ArrayCount(arguments),
                                         "Preparing font", 0, NULL));
  } else {
    return vkr_project_fail(job, "No build recipe for %s",
                            kind ? kind : "None");
  }
  const char *reference = NULL;
  char hash[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(
      vkr_project_managed_reference(job, output, job->stage, &reference));
  VKR_PROJECT_TRY(vkr_project_digest(job, output, hash));
  VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
  VkrBakeryJson *product = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, product, "role",
                      vkr_bakery_json_cstr(arena, kind));
  vkr_bakery_json_set(arena, product, "path",
                      vkr_bakery_json_cstr(arena, reference));
  vkr_bakery_json_set(arena, product, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_append(artifacts, product);
  vkr_bakery_json_set(arena, record, "artifacts", artifacts);
  vkr_bakery_json_set(
      arena, record, "fingerprint",
      vkr_bakery_json_cstr(arena, vkr_project_printf(job, "sha256:%s", hash)));
  vkr_bakery_json_remove(record, "closure");
  if (strcmp(kind, "mesh") == 0) {
    VKR_PROJECT_TRY(
        vkr_project_cook_model_animation(job, record, source, output));
  }
  return true_v;
}

vkr_internal VkrBakeryJson *
vkr_project_merged_bakes(VkrProjectJob *job, const VkrBakeryJson *scene) {
  Arena *arena = job->arena;
  VkrBakeryJson *merged = vkr_bakery_json_object(arena);
  const VkrBakeryJson *sources[] = {vkr_bakery_json_get(scene, "bake_recipes"),
                                    vkr_bakery_json_get(job->request, "bakes")};
  for (uint32_t s = 0u; s < ArrayCount(sources); ++s) {
    for (const VkrBakeryJson *field = sources[s] ? sources[s]->first : NULL;
         field; field = field->next) {
      vkr_bakery_json_set(arena, merged, (const char *)field->key.str,
                          vkr_bakery_json_clone(arena, field));
    }
  }
  vkr_bakery_json_set(arena, merged, "prepare_assets",
                      vkr_bakery_json_bool(arena, true_v));
  return merged;
}

VkrBakeryJson *vkr_project_prepare_unbuilt(VkrProjectJob *job, const char *path,
                                           VkrBakeryJson *scene) {
  Arena *arena = job->arena;
  char original_fingerprint[VKR_BAKERY_SHA256_HEX];
  char scene_root[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(scene_root, sizeof(scene_root), path);
  if (!vkr_project_digest(job, path, original_fingerprint)) {
    return NULL;
  }
  if (vkr_project_truthy(vkr_bakery_json_get(scene, "edit_overlay"))) {
    char overlay[VKR_PROJECT_PATH];
    if (!vkr_project_contained(job, scene_root,
                               vkr_project_json_text(scene, "edit_overlay"),
                               true_v, overlay) ||
        !vkr_project_load_json(job, overlay, VKR_PROJECT_MAX_JSON_BYTES)) {
      return NULL;
    }
  }
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  if (!vkr_project_make_dirs(job, staging) ||
      !vkr_project_mkdtemp(job, staging,
                           vkr_project_printf(job, "%s-build-", job->scene_id),
                           job->stage)) {
    return NULL;
  }
  job->assets = vkr_bakery_json_get(scene, "assets");
  if (!job->assets) {
    vkr_project_fail(job, "'assets'");
    return NULL;
  }
  VkrProjectStrings repairs;
  if (!vkr_project_texture_repair_bundles(job, scene, scene_root, &repairs)) {
    return NULL;
  }
  for (uint32_t i = 0u; i < repairs.count; ++i) {
    if (!vkr_project_repair_bundle(job, repairs.items[i], scene_root)) {
      return NULL;
    }
  }
  /* Records appended while building (materials, textures) already carry
     artifacts; stop at the last record present before the loop. */
  VkrBakeryJson *last = job->assets->last;
  for (VkrBakeryJson *record = job->assets->first; record;
       record = record->next) {
    if (!vkr_project_truthy(vkr_bakery_json_get(record, "artifacts")) &&
        !vkr_bakery_json_is_string(vkr_bakery_json_get(record, "kind"),
                                   "probe-cube") &&
        !vkr_project_build_record(job, record, scene_root)) {
      return NULL;
    }
    if (record == last) {
      break;
    }
  }
  if (!vkr_project_validate_semantics(job, scene)) {
    return NULL;
  }
  char staged_builds[VKR_PROJECT_PATH];
  (void)snprintf(staged_builds, sizeof(staged_builds), "%s/builds", job->stage);
  static const char *const folders[] = {"builds"};
  if (!vkr_project_make_dirs(job, staged_builds) ||
      !vkr_project_publish_folders(job, folders, ArrayCount(folders),
                                   scene_root, "Build revision collision",
                                   &job->published_builds)) {
    return NULL;
  }
  vkr_bakery_json_set(arena, job->request, "bakes",
                      vkr_project_merged_bakes(job, scene));
  if (!vkr_project_perform_bakes(job, scene, scene_root)) {
    return NULL;
  }
  VkrBakeryJson *result = vkr_project_lower(job, scene, scene_root, true_v);
  char after[VKR_BAKERY_SHA256_HEX];
  if (!result || !vkr_project_digest(job, path, after)) {
    return NULL;
  }
  if (strcmp(after, original_fingerprint) != 0) {
    vkr_project_fail(job,
                     "Scene changed while preparing assets; retry the build");
    return NULL;
  }
  /* Transfer ownership before publication: cancellation may retain an orphan
     revision, but can never delete a revision referenced by a committed
     scene. */
  if (!vkr_project_validate_managed_scene(job, scene, NULL, NULL, NULL)) {
    return NULL;
  }
  job->published_builds.count = 0u;
  return vkr_project_write_managed_scene(job, path, scene) ? result : NULL;
}

// =============================================================================
// Project asset imports
// =============================================================================

vkr_internal const char *vkr_project_source_value(const VkrBakeryJson *value) {
  if (value && value->type == VKR_BAKERY_JSON_OBJECT) {
    return vkr_project_json_text(value, "source");
  }
  if (value && value->type == VKR_BAKERY_JSON_STRING) {
    return (const char *)value->string.str;
  }
  return NULL;
}

/* Records the dependency closure of one record whose artifacts the job
   built into its stage. */
vkr_internal bool8_t vkr_project_record_closure(VkrProjectJob *job,
                                                VkrBakeryJson *record) {
  VkrBakeryJson *visited = vkr_bakery_json_object(job->arena);
  const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
  for (const VkrBakeryJson *product = artifacts ? artifacts->first : NULL;
       product; product = product->next) {
    char path[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(vkr_project_contained(
        job, job->stage, vkr_project_json_text(product, "path"), true_v, path));
    VKR_PROJECT_TRY(vkr_project_validate_bundle_dependencies(job, job->stage,
                                                             path, visited));
  }
  VkrBakeryJson *closure = vkr_project_closure(job, visited, job->stage);
  VKR_PROJECT_TRY(closure);
  vkr_bakery_json_set(job->arena, record, "closure", closure);
  return true_v;
}

vkr_internal bool8_t vkr_project_record_closures(VkrProjectJob *job,
                                                 VkrBakeryJson *records) {
  for (VkrBakeryJson *record = records->first; record; record = record->next) {
    VKR_PROJECT_TRY(vkr_project_record_closure(job, record));
  }
  return true_v;
}

/* A record whose first artifact the job built into its stage; a record an
   earlier job published keeps its artifacts and closure in the project. */
vkr_internal bool8_t vkr_project_record_staged(VkrProjectJob *job,
                                               const VkrBakeryJson *record) {
  const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
  const char *relative = artifacts && artifacts->first
                             ? vkr_project_json_text(artifacts->first, "path")
                             : NULL;
  char path[VKR_PROJECT_PATH];
  return relative && relative[0] &&
         vkr_bakery_path_join(path, sizeof(path), job->stage, relative) &&
         vkr_bakery_is_file(path);
}

VkrBakeryJson *vkr_project_import_project_assets(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (job->read_only) {
    vkr_project_fail(job, "Cannot import into a read-only workspace");
    return NULL;
  }
  const VkrBakeryJson *sources = vkr_bakery_json_get(job->request, "sources");
  if (!sources || sources->type != VKR_BAKERY_JSON_ARRAY || !sources->count) {
    vkr_project_fail(job, "Select at least one asset to import");
    return NULL;
  }
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  if (!vkr_project_make_dirs(job, staging) ||
      !vkr_project_mkdtemp(job, staging, "project-assets-", job->stage)) {
    return NULL;
  }
  job->assets = vkr_bakery_json_array(arena);
  job->asset_scope = "project";
  for (const VkrBakeryJson *value = sources->first; value;
       value = value->next) {
    char source[VKR_PROJECT_PATH];
    if (!vkr_project_source_file(job, vkr_project_source_value(value),
                                 source) ||
        !vkr_project_import_source(job, source)) {
      return NULL;
    }
  }
  VkrBakeryJson *records = job->assets;
  static const char *const folders[] = {"sources", "builds", "imports"};
  if (!vkr_project_record_closures(job, records) ||
      !vkr_project_publish_folders(
          job, folders, ArrayCount(folders), job->project_root,
          "Project asset revision already exists", &job->project_builds)) {
    return NULL;
  }
  VkrBakeryJson *project = vkr_project_document(job);
  if (!project) {
    return NULL;
  }
  VkrBakeryJson *pending = vkr_bakery_json_array(arena);
  const VkrBakeryJson *existing = vkr_bakery_json_get(project, "assets");
  for (const VkrBakeryJson *item = existing ? existing->first : NULL; item;
       item = item->next) {
    vkr_bakery_json_append(pending, vkr_bakery_json_clone(arena, item));
  }
  VkrBakeryJson *imported = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *item = records->first; item; item = item->next) {
    vkr_bakery_json_append(pending, vkr_bakery_json_clone(arena, item));
    vkr_bakery_json_append(
        imported,
        vkr_bakery_json_clone(arena, vkr_bakery_json_get(item, "id")));
  }
  job->pending_project_assets = pending;
  VkrBakeryJson *document = vkr_project_document(job);
  if (!document || !vkr_project_validate_document(
                       job, document, VKR_PROJECT_MAX_MANIFEST_BYTES)) {
    return NULL;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "fonts", vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  vkr_bakery_json_set(arena, result, "imported_assets", imported);
  /* Deferred or preview imports the editor finalizes in the background. */
  vkr_bakery_json_set(
      arena, result, "preview_assets",
      vkr_bakery_json_int(arena, vkr_project_count_preview(records)));
  return result;
}

vkr_internal bool8_t vkr_project_rebuild_asset(VkrProjectJob *job,
                                               const char *operation,
                                               VkrBakeryJson *record,
                                               const char *scene_root);

VkrBakeryJson *vkr_project_finalize_project_assets(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (job->read_only) {
    vkr_project_fail(job, "Cannot finalize in a read-only workspace");
    return NULL;
  }
  VkrBakeryJson *project = vkr_project_document(job);
  if (!project) {
    return NULL;
  }
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  if (!vkr_project_make_dirs(job, staging) ||
      !vkr_project_mkdtemp(job, staging, "project-finalize-", job->stage)) {
    return NULL;
  }
  /* Every project mesh imported at the preview or deferred tier rebuilds at
     the final tier under its identity; scenes naming it keep their
     references and read the new revision when they next prepare. */
  job->assets =
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(project, "assets"));
  if (!job->assets) {
    job->assets = vkr_bakery_json_array(arena);
  }
  job->asset_scope = "project";
  job->texture_preview = false_v;
  job->texture_deferred = false_v;
  VkrProjectNodes pending = {0};
  for (VkrBakeryJson *item = job->assets->first; item; item = item->next) {
    const VkrBakeryJson *tier = vkr_bakery_json_get(item, "texture_tier");
    if ((vkr_bakery_json_is_string(tier, "preview") ||
         vkr_bakery_json_is_string(tier, "deferred")) &&
        !vkr_project_nodes_push(job, &pending, item)) {
      return NULL;
    }
  }
  VkrBakeryJson *finalized = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < pending.count; ++i) {
    if (!vkr_project_rebuild_asset(job, "rebuild_asset", pending.items[i],
                                   job->project_root)) {
      return NULL;
    }
    vkr_bakery_json_append(
        finalized, vkr_bakery_json_clone(
                       arena, vkr_bakery_json_get(pending.items[i], "id")));
  }
  /* Only the records this job rebuilt have artifacts in its stage; other
     imports of the project keep their published closures. */
  for (VkrBakeryJson *record = job->assets->first; record;
       record = record->next) {
    if (vkr_project_record_staged(job, record) &&
        !vkr_project_record_closure(job, record)) {
      return NULL;
    }
  }
  static const char *const folders[] = {"sources", "builds", "imports"};
  if (!vkr_project_publish_folders(
          job, folders, ArrayCount(folders), job->project_root,
          "Project asset revision already exists", &job->project_builds)) {
    return NULL;
  }
  job->pending_project_assets = job->assets;
  vkr_project_record_ready_remaining(job, pending.items, pending.count,
                                     job->project_root);
  VkrBakeryJson *document = vkr_project_document(job);
  if (!document || !vkr_project_validate_document(
                       job, document, VKR_PROJECT_MAX_MANIFEST_BYTES)) {
    return NULL;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "fonts", vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  vkr_bakery_json_set(arena, result, "finalized_assets", finalized);
  vkr_bakery_json_set(
      arena, result, "preview_assets",
      vkr_bakery_json_int(arena, vkr_project_count_preview(job->assets)));
  return result;
}

// =============================================================================
// Scene asset edits
// =============================================================================

vkr_internal bool8_t vkr_project_uses_asset(const VkrBakeryJson *value,
                                            const char *id) {
  if (!value) {
    return false_v;
  }
  if (value->type == VKR_BAKERY_JSON_OBJECT &&
      vkr_bakery_json_is_string(vkr_bakery_json_get(value, "scope"), "scene") &&
      vkr_bakery_json_is_string(vkr_bakery_json_get(value, "id"), id)) {
    return true_v;
  }
  if (value->type == VKR_BAKERY_JSON_OBJECT ||
      value->type == VKR_BAKERY_JSON_ARRAY) {
    for (const VkrBakeryJson *child = value->first; child;
         child = child->next) {
      if (vkr_project_uses_asset(child, id)) {
        return true_v;
      }
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_project_valid_asset_name(const char *name,
                                                  const char *stripped) {
  if (!stripped[0] || strlen(stripped) > 512u) {
    return false_v;
  }
  for (const char *c = name; *c; ++c) {
    if ((uint8_t)*c < 32u || (uint8_t)*c == 127u) {
      return false_v;
    }
  }
  return true_v;
}

/* Detaches every record after the first `count` from `records`. */
vkr_internal VkrBakeryJson *vkr_project_detach_tail(VkrBakeryJson *records,
                                                    uint32_t count) {
  if (records->count <= count) {
    return NULL;
  }
  VkrBakeryJson *tail = NULL;
  if (count == 0u) {
    tail = records->first;
    records->first = NULL;
    records->last = NULL;
  } else {
    VkrBakeryJson *keep = vkr_bakery_json_at(records, count - 1u);
    tail = keep->next;
    keep->next = NULL;
    records->last = keep;
  }
  records->count = count;
  return tail;
}

/* (kind, source_key or name) of a generated record as one comparable key. */
vkr_internal const char *vkr_project_source_key(VkrProjectJob *job,
                                                const VkrBakeryJson *record) {
  const char *kind = vkr_project_json_text(record, "kind");
  const VkrBakeryJson *key = vkr_bakery_json_get(record, "source_key");
  if (!key) {
    key = vkr_bakery_json_get(record, "name");
  }
  String8 text = {0};
  if (key &&
      vkr_bakery_json_write(job->arena, key, VKR_BAKERY_JSON_COMPACT, &text)) {
    return vkr_project_printf(job, "%s\n%.*s", kind ? kind : "",
                              (int)text.length, (const char *)text.str);
  }
  return vkr_project_printf(job, "%s\n", kind ? kind : "");
}

/* Rebuilds a mesh bundle and merges its generated material and texture
   records into the records of the same import. */
vkr_internal bool8_t vkr_project_rebuild_mesh(
    VkrProjectJob *job, const char *asset_id, const char *import_id,
    const char *source, const char *bundle, const char *output,
    float32_t lightmap_texels_per_unit) {
  Arena *arena = job->arena;
  VKR_PROJECT_TRY(vkr_project_cook_mesh(job, source, output, bundle, import_id,
                                        lightmap_texels_per_unit,
                                        "Rebuilding model"));
  VkrProjectNodes existing = {0};
  VkrProjectStrings existing_keys = {0};
  for (VkrBakeryJson *item = job->assets->first; item; item = item->next) {
    if (vkr_bakery_json_is_string(vkr_bakery_json_get(item, "import_id"),
                                  import_id) &&
        !vkr_bakery_json_is_string(vkr_bakery_json_get(item, "id"), asset_id)) {
      VKR_PROJECT_TRY(vkr_project_nodes_push(job, &existing, item));
      VKR_PROJECT_TRY(vkr_project_strings_push(
          job, &existing_keys, vkr_project_source_key(job, item)));
    }
  }
  const uint32_t previous_count = job->assets->count;
  VKR_PROJECT_TRY(vkr_project_index_bundle(job, bundle, import_id));
  VkrBakeryJson *generated =
      vkr_project_detach_tail(job->assets, previous_count);
  while (generated) {
    VkrBakeryJson *next = generated->next;
    const char *key = vkr_project_source_key(job, generated);
    VkrBakeryJson *prior = NULL;
    /* A dict keeps the last record for a repeated key. */
    for (uint32_t i = 0u; i < existing.count; ++i) {
      if (strcmp(existing_keys.items[i], key) == 0) {
        prior = existing.items[i];
      }
    }
    if (prior) {
      vkr_bakery_json_set(
          arena, generated, "id",
          vkr_bakery_json_clone(arena, vkr_bakery_json_get(prior, "id")));
      vkr_bakery_json_remove(prior, "closure");
      for (const VkrBakeryJson *field = generated->first; field;
           field = field->next) {
        vkr_bakery_json_set(arena, prior, (const char *)field->key.str,
                            vkr_bakery_json_clone(arena, field));
      }
    } else {
      vkr_bakery_json_append(job->assets, generated);
    }
    generated = next;
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_rebuild_asset(VkrProjectJob *job,
                                               const char *operation,
                                               VkrBakeryJson *record,
                                               const char *scene_root) {
  Arena *arena = job->arena;
  const char *asset_id =
      vkr_project_strdup(job, vkr_project_json_text(record, "id"));
  const char *import_id = vkr_project_json_text(record, "import_id");
  char generated_id[37];
  if (!import_id || !import_id[0]) {
    vkr_project_uuid4(generated_id);
    import_id = generated_id;
  }
  import_id = vkr_project_strdup(job, import_id);
  char revision[37];
  vkr_project_uuid4(revision);
  char bundle[VKR_PROJECT_PATH];
  (void)snprintf(bundle, sizeof(bundle), "%s/builds/%s", job->stage, revision);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, bundle));
  const bool8_t reimport = strcmp(operation, "reimport_asset") == 0;
  char source[VKR_PROJECT_PATH];
  if (reimport) {
    const char *new_source = vkr_project_json_text(job->request, "source");
    if (!new_source || !new_source[0]) {
      return vkr_project_fail(job,
                              "Reimport requires a newly selected source file");
    }
    VKR_PROJECT_TRY(vkr_project_source_file(job, new_source, source));
  } else {
    const char *recorded = vkr_project_json_text(record, "source");
    if (!recorded || !recorded[0]) {
      return vkr_project_fail(
          job, "Source snapshot unavailable; use Reimport to select a source");
    }
    VKR_PROJECT_TRY(
        vkr_project_contained(job, scene_root, recorded, true_v, source));
  }
  const char *kind =
      vkr_project_strdup(job, vkr_project_json_text(record, "kind"));
  char output[VKR_PROJECT_PATH];
  const char *reference = NULL;
  if (kind && strcmp(kind, "mesh") == 0) {
    if (reimport) {
      char snapshot[VKR_PROJECT_PATH];
      VKR_PROJECT_TRY(
          vkr_project_snapshot_model(job, source, revision, snapshot));
      (void)snprintf(source, sizeof(source), "%s", snapshot);
      VKR_PROJECT_TRY(
          vkr_project_managed_reference(job, source, job->stage, &reference));
      vkr_bakery_json_set(arena, record, "source",
                          vkr_bakery_json_cstr(arena, reference));
    }
    (void)snprintf(output, sizeof(output), "%s/mesh.vkb", bundle);
    /* A rebuild or reimport the request asks for applies the request's
       model settings when it names them, so a model can gain or drop
       lightmap UVs; texture finalization and any request without them
       repeat the recorded settings. */
    const bool8_t requested =
        job->operation && strcmp(job->operation, operation) == 0 &&
        vkr_bakery_json_get(job->request, "model_settings") != NULL;
    const float32_t lightmap_density =
        requested ? vkr_project_requested_lightmap_density(job)
                  : vkr_project_record_lightmap_density(record);
    VKR_PROJECT_TRY(vkr_project_rebuild_mesh(job, asset_id, import_id, source,
                                             bundle, output, lightmap_density));
    vkr_project_mark_tier(job, record);
    vkr_project_set_mesh_recipe(job, record, lightmap_density);
  } else if (kind && strcmp(kind, "font") == 0) {
    VkrBakeryJson *previous = job->assets;
    job->assets = vkr_bakery_json_array(arena);
    VkrBakeryJson *imported = vkr_project_import_font(job, source);
    VkrBakeryJson *generated = job->assets->first;
    job->assets = previous;
    VKR_PROJECT_TRY(imported);
    const VkrBakeryJson *artifacts =
        vkr_bakery_json_get(generated, "artifacts");
    if (!generated || !artifacts || !artifacts->first) {
      return vkr_project_fail(job, "list index out of range");
    }
    (void)vkr_bakery_path_join(output, sizeof(output), job->stage,
                               vkr_project_json_text(artifacts->first, "path"));
    const VkrBakeryJson *recipe = vkr_bakery_json_get(generated, "recipe");
    const VkrBakeryJson *generated_source =
        vkr_bakery_json_get(generated, "source");
    vkr_bakery_json_set(arena, record, "source",
                        generated_source
                            ? vkr_bakery_json_clone(arena, generated_source)
                            : vkr_bakery_json_null(arena));
    vkr_bakery_json_set(arena, record, "recipe",
                        recipe ? vkr_bakery_json_clone(arena, recipe)
                               : vkr_bakery_json_object(arena));
  } else if (kind && strcmp(kind, "material") == 0) {
    VKR_PROJECT_TRY(vkr_project_import_material(job, source, bundle, import_id,
                                                0u, output));
    VKR_PROJECT_TRY(
        vkr_project_managed_reference(job, output, job->stage, &reference));
    vkr_bakery_json_set(arena, record, "source",
                        vkr_bakery_json_cstr(arena, reference));
  } else if (kind && (strcmp(kind, "texture") == 0 ||
                      strcmp(kind, "environment") == 0)) {
    VKR_PROJECT_TRY(vkr_project_copy_blob(job, source, bundle, output));
    VKR_PROJECT_TRY(
        vkr_project_managed_reference(job, output, job->stage, &reference));
    vkr_bakery_json_set(arena, record, "source",
                        vkr_bakery_json_cstr(arena, reference));
  } else {
    return vkr_project_fail(
        job, "Use the scene bake controls to rebuild this generated asset");
  }
  const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
  const char *role = kind;
  if (vkr_project_truthy(artifacts)) {
    const VkrBakeryJson *first_role =
        vkr_bakery_json_get(artifacts->first, "role");
    if (first_role && first_role->type == VKR_BAKERY_JSON_STRING) {
      role = vkr_project_strdup(job, (const char *)first_role->string.str);
    }
  }
  const char *output_reference = NULL;
  char hash[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_managed_reference(job, output, job->stage,
                                                &output_reference));
  VKR_PROJECT_TRY(vkr_project_digest(job, output, hash));
  vkr_bakery_json_remove(record, "closure");
  VkrBakeryJson *products = vkr_bakery_json_array(arena);
  VkrBakeryJson *product = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, product, "role",
                      vkr_bakery_json_cstr(arena, role));
  vkr_bakery_json_set(arena, product, "path",
                      vkr_bakery_json_cstr(arena, output_reference));
  vkr_bakery_json_set(arena, product, "version", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_append(products, product);
  vkr_bakery_json_set(arena, record, "id",
                      vkr_bakery_json_cstr(arena, asset_id));
  vkr_bakery_json_set(arena, record, "import_id",
                      vkr_bakery_json_cstr(arena, import_id));
  vkr_bakery_json_set(arena, record, "import_revision",
                      vkr_bakery_json_cstr(arena, revision));
  vkr_bakery_json_set(arena, record, "reimport_status",
                      vkr_bakery_json_cstr(arena, "source_snapshot"));
  vkr_bakery_json_set(arena, record, "artifacts", products);
  vkr_bakery_json_set(
      arena, record, "fingerprint",
      vkr_bakery_json_cstr(arena, vkr_project_printf(job, "sha256:%s", hash)));
  vkr_bakery_json_remove(record, "animation_count");
  if (strcmp(kind, "mesh") == 0) {
    VKR_PROJECT_TRY(
        vkr_project_cook_model_animation(job, record, source, output));
  }
  const VkrBakeryJson *source_manifest =
      vkr_bakery_json_get(job->sources, revision);
  const VkrBakeryJson *dependencies =
      vkr_bakery_json_get(source_manifest, "dependencies");
  const VkrBakeryJson *record_source = vkr_bakery_json_get(record, "source");
  VkrBakeryJson *import_record = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, import_record, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, import_record, "id",
                      vkr_bakery_json_cstr(arena, import_id));
  vkr_bakery_json_set(arena, import_record, "revision",
                      vkr_bakery_json_cstr(arena, revision));
  vkr_bakery_json_set(arena, import_record, "source",
                      record_source
                          ? vkr_bakery_json_clone(arena, record_source)
                          : vkr_bakery_json_null(arena));
  vkr_bakery_json_set(arena, import_record, "asset",
                      vkr_bakery_json_clone(arena, record));
  vkr_bakery_json_set(arena, import_record, "dependencies",
                      dependencies ? vkr_bakery_json_clone(arena, dependencies)
                                   : vkr_bakery_json_array(arena));
  return vkr_project_atomic_json(
      job,
      vkr_project_printf(job, "%s/imports/%s-%s.json", job->stage, import_id,
                         revision),
      import_record);
}

vkr_internal bool8_t vkr_project_import_scene_sources(VkrProjectJob *job,
                                                      VkrBakeryJson *scene) {
  Arena *arena = job->arena;
  VkrProjectNodes sources = {0};
  static const char *const lists[] = {"models", "sources"};
  for (uint32_t l = 0u; l < ArrayCount(lists); ++l) {
    VkrBakeryJson *items = vkr_bakery_json_get(job->request, lists[l]);
    for (VkrBakeryJson *item = items ? items->first : NULL; item;
         item = item->next) {
      VKR_PROJECT_TRY(vkr_project_nodes_push(job, &sources, item));
    }
  }
  if (!sources.count) {
    return vkr_project_fail(job, "Select at least one asset to import");
  }
  bool8_t add_instances = false_v;
  (void)vkr_bakery_json_get_bool(job->request, "add_instances", &add_instances);
  for (uint32_t i = 0u; i < sources.count; ++i) {
    char source[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(vkr_project_source_file(
        job, vkr_project_source_value(sources.items[i]), source));
    VkrBakeryJson *reference = vkr_project_import_source(job, source);
    VKR_PROJECT_TRY(reference);
    char suffix[16];
    vkr_project_suffix_lower(source, suffix, sizeof(suffix));
    const bool8_t model = !strcmp(suffix, ".obj") || !strcmp(suffix, ".gltf") ||
                          !strcmp(suffix, ".glb");
    if (!model || !add_instances) {
      continue;
    }
    VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
    if (!entities) {
      entities = vkr_bakery_json_array(arena);
      vkr_bakery_json_set(arena, scene, "entities", entities);
    }
    char id[37];
    char stem[512];
    vkr_project_uuid4(id);
    vkr_project_stem(source, stem, sizeof(stem));
    VkrBakeryJson *entity = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entity, "id", vkr_bakery_json_cstr(arena, id));
    vkr_bakery_json_set(arena, entity, "name",
                        vkr_bakery_json_cstr(arena, stem));
    vkr_bakery_json_set(arena, entity, "parent", vkr_bakery_json_null(arena));
    vkr_bakery_json_set(arena, entity, "transform",
                        vkr_project_identity_transform(job));
    VkrBakeryJson *mesh = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, mesh, "asset", reference);
    vkr_bakery_json_set(arena, mesh, "pipeline_domain",
                        vkr_bakery_json_cstr(arena, "world"));
    vkr_bakery_json_set(arena, entity, "mesh", mesh);
    vkr_bakery_json_append(entities, entity);
  }
  return true_v;
}

/* Lowers the edited scene, checks it is unchanged on disk, then commits it. */
vkr_internal VkrBakeryJson *
vkr_project_commit_edit(VkrProjectJob *job, VkrBakeryJson *scene,
                        const char *path, const char *scene_root,
                        const char *fingerprint, const char *changed,
                        bool8_t validate) {
  VkrBakeryJson *result = vkr_project_lower(job, scene, scene_root, true_v);
  char after[VKR_BAKERY_SHA256_HEX];
  if (!result || !vkr_project_digest(job, path, after)) {
    return NULL;
  }
  if (strcmp(after, fingerprint) != 0) {
    vkr_project_fail(job, "%s", changed);
    return NULL;
  }
  if (validate) {
    /* Transfer ownership before publication: cancellation may retain an
       orphan revision, but can never delete a revision referenced by a
       committed scene. */
    if (!vkr_project_validate_managed_scene(job, scene, NULL, NULL, NULL)) {
      return NULL;
    }
    job->published_builds.count = 0u;
  }
  return vkr_project_write_managed_scene(job, path, scene) ? result : NULL;
}

vkr_internal VkrBakeryJson *
vkr_project_delete_asset(VkrProjectJob *job, VkrBakeryJson *scene,
                         VkrBakeryJson *record, const char *path,
                         const char *scene_root, const char *fingerprint) {
  Arena *arena = job->arena;
  const char *label = vkr_project_record_label(record, "The asset");
  const char *record_id = vkr_project_json_text(record, "id");
  /* Deleting drops the record; unreferenced builds are collected by
     workspace cleanup. An asset the scene still uses stays. */
  for (const VkrBakeryJson *field = scene->first; field; field = field->next) {
    if (field->key.length == 6u &&
        MemCompare(field->key.str, "assets", 6u) == 0) {
      continue;
    }
    if (record_id && vkr_project_uses_asset(field, record_id)) {
      vkr_project_fail(job, "%s is used by the scene; remove its uses first",
                       label);
      return NULL;
    }
  }
  VkrBakeryJson *others = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *item = job->assets->first; item;
       item = item->next) {
    if (item != record) {
      vkr_bakery_json_append(others, vkr_bakery_json_clone(arena, item));
    }
  }
  String8 serialized = {0};
  if (!vkr_bakery_json_write(arena, others, VKR_BAKERY_JSON_PYTHON_COMPACT,
                             &serialized)) {
    vkr_project_fail(job, "Out of memory");
    return NULL;
  }
  if (record_id && strstr((const char *)serialized.str, record_id)) {
    vkr_project_fail(job, "%s is used by another asset; delete that first",
                     label);
    return NULL;
  }
  VkrBakeryJson *kept = vkr_bakery_json_array(arena);
  for (VkrBakeryJson *item = job->assets->first; item;) {
    VkrBakeryJson *next = item->next;
    if (item != record) {
      vkr_bakery_json_append(kept, item);
    }
    item = next;
  }
  job->assets->first = kept->first;
  job->assets->last = kept->last;
  job->assets->count = kept->count;
  return vkr_project_commit_edit(job, scene, path, scene_root, fingerprint,
                                 "Scene changed during delete; retry", false_v);
}

vkr_internal VkrBakeryJson *
vkr_project_rename_asset(VkrProjectJob *job, VkrBakeryJson *scene,
                         VkrBakeryJson *record, const char *path,
                         const char *scene_root, const char *fingerprint) {
  const char *name = vkr_project_json_text(job->request, "name");
  const char *stripped = name ? vkr_project_strip_text(job, name) : "";
  if (!name || !vkr_project_valid_asset_name(name, stripped)) {
    vkr_project_fail(job, "Asset name must be 1\xE2\x80\x93"
                          "512 UTF-8 bytes without control characters");
    return NULL;
  }
  vkr_bakery_json_set(job->arena, record, "name",
                      vkr_bakery_json_cstr(job->arena, stripped));
  return vkr_project_commit_edit(job, scene, path, scene_root, fingerprint,
                                 "Scene changed during rename; retry", false_v);
}

VkrBakeryJson *vkr_project_edit_assets(VkrProjectJob *job) {
  Arena *arena = job->arena;
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
  (void)vkr_project_resolve(expected_path, false_v, expected, sizeof(expected));
  if (strcmp(path, expected) != 0) {
    vkr_project_fail(job,
                     "Asset operation scene does not match project membership");
    return NULL;
  }
  char scene_root[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(scene_root, sizeof(scene_root), path);
  VkrBakeryJson *scene = vkr_project_read_managed_scene(job, path);
  char fingerprint[VKR_BAKERY_SHA256_HEX];
  if (!scene || !vkr_project_migrate_source_references(job, scene, path) ||
      !vkr_project_digest(job, path, fingerprint)) {
    return NULL;
  }
  if (vkr_project_truthy(vkr_bakery_json_get(scene, "edit_overlay"))) {
    char overlay[VKR_PROJECT_PATH];
    if (!vkr_project_contained(job, scene_root,
                               vkr_project_json_text(scene, "edit_overlay"),
                               true_v, overlay) ||
        !vkr_project_load_json(job, overlay, VKR_PROJECT_MAX_JSON_BYTES)) {
      return NULL;
    }
  }
  if (!vkr_project_validate_semantics(job, scene)) {
    return NULL;
  }
  job->assets = vkr_bakery_json_get(scene, "assets");
  if (!job->assets) {
    vkr_project_fail(job, "'assets'");
    return NULL;
  }
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  if (!vkr_project_make_dirs(job, staging) ||
      !vkr_project_mkdtemp(job, staging,
                           vkr_project_printf(job, "%s-assets-", job->scene_id),
                           job->stage)) {
    return NULL;
  }
  const char *operation = job->operation;
  bool8_t added = false_v;
  /* Records a finalize rebuilt, which the job then reports as ready. */
  VkrProjectNodes finalized = {0};
  int64_t added_entity = 0;
  if (strcmp(operation, "add_entities") == 0) {
    if (!vkr_project_truthy(vkr_bakery_json_get(job->request, "models")) &&
        !vkr_project_truthy(vkr_bakery_json_get(job->request, "lights")) &&
        !vkr_project_truthy(vkr_bakery_json_get(job->request, "prefabs"))) {
      vkr_project_fail(job, "Select a model, light or scene to add");
      return NULL;
    }
    if (!vkr_project_append_entities(job, scene, &added_entity) ||
        !vkr_project_validate_semantics(job, scene)) {
      return NULL;
    }
    added = true_v;
  } else if (strcmp(operation, "import_assets") == 0) {
    if (!vkr_project_import_scene_sources(job, scene)) {
      return NULL;
    }
  } else if (strcmp(operation, "finalize_textures") == 0) {
    /* Every mesh imported at the preview or deferred tier rebuilds at the
       final tier; entities keep naming the same asset identities. */
    job->texture_preview = false_v;
    job->texture_deferred = false_v;
    for (VkrBakeryJson *item = job->assets->first; item; item = item->next) {
      const VkrBakeryJson *tier = vkr_bakery_json_get(item, "texture_tier");
      if (vkr_bakery_json_is_string(tier, "preview") ||
          vkr_bakery_json_is_string(tier, "deferred")) {
        if (!vkr_project_nodes_push(job, &finalized, item)) {
          return NULL;
        }
      }
    }
    for (uint32_t i = 0u; i < finalized.count; ++i) {
      if (!vkr_project_rebuild_asset(job, "rebuild_asset", finalized.items[i],
                                     scene_root)) {
        return NULL;
      }
    }
  } else {
    const VkrBakeryJson *requested_id =
        vkr_bakery_json_get(job->request, "asset_id");
    VkrBakeryJson *record = NULL;
    uint32_t matches = 0u;
    for (VkrBakeryJson *item = job->assets->first; item; item = item->next) {
      const VkrBakeryJson *id = vkr_bakery_json_get(item, "id");
      if ((id && requested_id && vkr_bakery_json_equal(id, requested_id)) ||
          (!id && !requested_id)) {
        record = item;
        matches += 1u;
      }
    }
    if (matches != 1u) {
      vkr_project_fail(job,
                       "Select one scene-owned asset to rebuild or reimport");
      return NULL;
    }
    if (strcmp(operation, "delete_asset") == 0) {
      return vkr_project_delete_asset(job, scene, record, path, scene_root,
                                      fingerprint);
    }
    if (strcmp(operation, "rename_asset") == 0) {
      return vkr_project_rename_asset(job, scene, record, path, scene_root,
                                      fingerprint);
    }
    if (!vkr_project_rebuild_asset(job, operation, record, scene_root)) {
      return NULL;
    }
  }
  vkr_bakery_json_set(arena, scene, "assets", job->assets);
  static const char *const folders[] = {"sources", "builds", "imports"};
  if (!vkr_project_publish_folders(job, folders, ArrayCount(folders),
                                   scene_root, "Asset revision already exists",
                                   &job->published_builds)) {
    return NULL;
  }
  VkrBakeryJson *result = vkr_project_commit_edit(
      job, scene, path, scene_root, fingerprint,
      "Scene changed during import; retry without losing the previous "
      "revision",
      true_v);
  if (result && added) {
    vkr_bakery_json_set(arena, result, "added_scene_entity",
                        vkr_bakery_json_int(arena, added_entity));
  }
  if (result) {
    vkr_project_record_ready_remaining(job, finalized.items, finalized.count,
                                       scene_root);
  }

  return result;
}
