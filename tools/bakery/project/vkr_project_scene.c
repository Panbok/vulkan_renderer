#include "vkr_project_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scene operations: authored overlays, scene inspection, prefabs, entity
 * placement, scene creation, semantic checks and bundle dependency closures. */

vkr_internal const char vkr_project_zero_fingerprint[] = "0000000000000000";

void vkr_project_forgive(VkrProjectJob *job, bool8_t failed_before) {
  if (!failed_before && !job->cancelled) {
    job->failed = false_v;
    job->error[0] = 0;
  }
}

vkr_internal void vkr_project_scene_id_fingerprint(VkrProjectJob *job,
                                                   char out[17]) {
  vkr_project_fingerprint((const uint8_t *)job->scene_id, strlen(job->scene_id),
                          out);
}

// =============================================================================
// Authored overlays
// =============================================================================

bool8_t vkr_project_checked_overlay(VkrProjectJob *job,
                                    const VkrBakeryJson *overlay) {
  int64_t version = 0;
  const VkrBakeryJson *overrides = vkr_bakery_json_get(overlay, "overrides");
  if (!overlay || overlay->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_project_integer(vkr_bakery_json_get(overlay, "version"), &version) ||
      version < 1 || version > 5 || !overrides ||
      overrides->type != VKR_BAKERY_JSON_ARRAY) {
    return vkr_project_fail(job, "Unsupported authored override journal");
  }
  for (const VkrBakeryJson *record = overrides->first; record;
       record = record->next) {
    if (record->type != VKR_BAKERY_JSON_OBJECT) {
      return vkr_project_fail(job, "Invalid authored override record");
    }
  }
  return true_v;
}

bool8_t vkr_project_overlay_colliders(VkrProjectJob *job,
                                      const VkrBakeryJson *overlay,
                                      VkrProjectNodes *out) {
  MemZero(out, sizeof(*out));
  const VkrBakeryJson *overrides = vkr_bakery_json_get(overlay, "overrides");
  for (const VkrBakeryJson *record = overrides ? overrides->first : NULL;
       record; record = record->next) {
    const VkrBakeryJson *physics = vkr_bakery_json_get(record, "physics");
    if (!physics || physics->type == VKR_BAKERY_JSON_NULL) {
      continue;
    }
    const VkrBakeryJson *colliders =
        physics->type == VKR_BAKERY_JSON_OBJECT
            ? vkr_bakery_json_get(physics, "colliders")
            : NULL;
    if (physics->type != VKR_BAKERY_JSON_OBJECT ||
        (colliders && colliders->type != VKR_BAKERY_JSON_ARRAY)) {
      return vkr_project_fail(job, "Invalid physics collider list");
    }
    if (colliders && colliders->count > 32u) {
      return vkr_project_fail(job, "Physics body exceeds collider capacity");
    }
    for (VkrBakeryJson *collider = colliders ? colliders->first : NULL;
         collider; collider = collider->next) {
      const VkrBakeryJson *asset = vkr_bakery_json_get(collider, "asset");
      if (collider->type != VKR_BAKERY_JSON_OBJECT ||
          (asset && asset->type != VKR_BAKERY_JSON_STRING)) {
        return vkr_project_fail(job, "Invalid collision asset reference");
      }
      float64_t shape = 0.0;
      const bool8_t geometry =
          vkr_project_number(vkr_bakery_json_get(collider, "shape"), &shape) &&
          (shape == 3.0 || shape == 4.0);
      const bool8_t named = asset && asset->string.length;
      if (geometry && !named) {
        return vkr_project_fail(
            job, "Geometry collider requires a cooked collision asset");
      }
      if (named) {
        VKR_PROJECT_TRY(vkr_project_nodes_push(job, out, collider));
      }
    }
  }
  return true_v;
}

bool8_t vkr_project_overlay_identity(VkrProjectJob *job,
                                     const VkrBakeryJson *reference,
                                     const char *seed,
                                     const VkrBakeryJson *scene,
                                     const char *root, char out[17]) {
  int64_t index = 0;
  int64_t node = 0;
  const VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  const uint32_t entity_count =
      entities && entities->type == VKR_BAKERY_JSON_ARRAY ? entities->count
                                                          : 0u;
  if (!vkr_project_integer(vkr_bakery_json_get(reference, "scene_entity"),
                           &index) ||
      index < 0 || index >= (int64_t)entity_count ||
      !vkr_project_integer(vkr_bakery_json_get(reference, "gltf_node"),
                           &node) ||
      node < -1) {
    return vkr_project_fail(
        job, "Authored override references a missing source entity");
  }
  const VkrBakeryJson *entity = vkr_bakery_json_at(entities, (uint32_t)index);
  const VkrBakeryJson *mesh = vkr_bakery_json_get(entity, "mesh");
  const VkrBakeryJson *asset_reference = vkr_bakery_json_get(mesh, "asset");
  const char *mesh_path = vkr_project_json_text(mesh, "path");
  char resolved_mesh[VKR_PROJECT_PATH] = {0};
  if (mesh_path && mesh_path[0]) {
    (void)snprintf(resolved_mesh, sizeof(resolved_mesh), "%s", mesh_path);
  }
  if (vkr_project_truthy(asset_reference)) {
    if (!vkr_bakery_json_is_string(
            vkr_bakery_json_get(asset_reference, "scope"), "scene")) {
      return vkr_project_fail(job, "Import the project mesh locally before "
                                   "cloning authored source-node edits");
    }
    const VkrBakeryJson *inventory = vkr_bakery_json_get(scene, "assets");
    if (!inventory) {
      inventory = job->assets;
    }
    const VkrBakeryJson *asset = vkr_project_record_by_id(
        inventory, vkr_project_json_text(asset_reference, "id"));
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(asset, "artifacts");
    if (!asset || !vkr_project_truthy(artifacts)) {
      return vkr_project_fail(
          job, "Authored source-node edits require a built mesh");
    }
    VKR_PROJECT_TRY(vkr_project_contained(
        job, root, vkr_project_json_text(artifacts->first, "path"), true_v,
        resolved_mesh));
  }
  if (resolved_mesh[0]) {
    VkrBakeryJson *info = vkr_project_inspect_mesh(job, resolved_mesh);
    VKR_PROJECT_TRY(info);
    const VkrBakeryJson *nodes = vkr_bakery_json_get(info, "nodes");
    if (node != -1) {
      bool8_t found = false_v;
      for (const VkrBakeryJson *entry = nodes ? nodes->first : NULL;
           entry && !found; entry = entry->next) {
        int64_t value = 0;
        found =
            vkr_project_integer(vkr_bakery_json_get(entry, "index"), &value) &&
            value == node;
      }
      if (!found) {
        return vkr_project_fail(
            job, "Authored override references a missing cooked source node");
      }
    }
    if (vkr_project_truthy(nodes)) {
      const char *fingerprint = vkr_project_json_text(info, "fingerprint");
      vkr_project_mesh_identity(seed, fingerprint ? fingerprint : "0", out);
      return true_v;
    }
  } else if (node != -1) {
    return vkr_project_fail(
        job, "Authored source-node edit targets an entity without a mesh");
  }
  (void)snprintf(out, 17, "%s", seed);
  return true_v;
}

bool8_t vkr_project_inspect_collision(VkrProjectJob *job, const char *value,
                                      char *out) {
  VKR_PROJECT_TRY(vkr_project_source_file(job, value, out));
  char suffix[16];
  vkr_project_suffix_lower(out, suffix, sizeof(suffix));
  VkrBakeryStat info;
  if (strcmp(suffix, ".vkc") != 0 || !vkr_bakery_stat(out, &info) ||
      info.size < 48u || info.size > 64u * 1024u * 1024u) {
    return vkr_project_fail(
        job, "Collision dependency must be a bounded cooked .vkc file");
  }
  char fingerprint[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_digest(job, out, fingerprint));
  const char *key = vkr_project_printf(job, "%s|%s", out, fingerprint);
  if (vkr_bakery_json_get(job->collision_seen, key)) {
    return true_v;
  }
  const char *arguments[] = {"--inspect", "--input", out};
  VKR_PROJECT_TRY(vkr_project_run_tool(job, "collision", arguments,
                                       ArrayCount(arguments),
                                       "Validating cooked collision", 0, NULL));
  char after[VKR_BAKERY_SHA256_HEX];
  VKR_PROJECT_TRY(vkr_project_digest(job, out, after));
  if (strcmp(after, fingerprint) != 0) {
    return vkr_project_fail(job,
                            "Collision dependency changed during validation");
  }
  vkr_bakery_json_set(job->arena, job->collision_seen, key,
                      vkr_bakery_json_bool(job->arena, true_v));
  return true_v;
}

vkr_internal bool8_t vkr_project_import_overlay_collision(
    VkrProjectJob *job, const char *value, const char *source_root,
    const VkrBakeryJson *source_scene, char *out) {
  if (vkr_bakery_path_is_absolute(value)) {
    return vkr_project_source_file(job, value, out);
  }
  if (!source_scene) {
    /* Legacy runtime resolves physics assets against PROJECT_SOURCE_DIR. */
    char joined[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(joined, sizeof(joined), job->legacy_root, value);
    return vkr_project_source_file(job, joined, out);
  }
  VKR_PROJECT_TRY(vkr_project_validate_managed_path(job, value));
  /* A detached scene bundle still carries its original workspace-relative
     references. Its own closure can be resolved without the old workspace. */
  const char *scene_id = vkr_project_json_text(source_scene, "id");
  char parts[4][VKR_PROJECT_PATH];
  const char *cursor = value;
  uint32_t part = 0u;
  while (part < 4u && cursor) {
    const char *slash = strchr(cursor, '/');
    const uint64_t length = slash ? (uint64_t)(slash - cursor) : strlen(cursor);
    (void)snprintf(parts[part], VKR_PROJECT_PATH, "%.*s", (int)length, cursor);
    part += 1u;
    cursor = slash ? slash + 1 : NULL;
  }
  if (part == 4u && cursor && cursor[0] && strcmp(parts[0], "projects") == 0 &&
      strcmp(parts[2], "scenes") == 0 && scene_id &&
      strcmp(parts[3], scene_id) == 0) {
    char contained[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(
        vkr_project_contained(job, source_root, cursor, true_v, contained));
    return vkr_project_source_file(job, contained, out);
  }
  char scenes[VKR_PROJECT_PATH];
  char project[VKR_PROJECT_PATH];
  char projects[VKR_PROJECT_PATH];
  char workspace[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(scenes, sizeof(scenes), source_root);
  vkr_bakery_path_parent(project, sizeof(project), scenes);
  vkr_bakery_path_parent(projects, sizeof(projects), project);
  vkr_bakery_path_parent(workspace, sizeof(workspace), projects);
  if (strcmp(vkr_bakery_path_name(scenes), "scenes") != 0 ||
      strcmp(vkr_bakery_path_name(projects), "projects") != 0) {
    return vkr_project_fail(job, "External collision dependency requires its "
                                 "original managed workspace");
  }
  char contained[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(
      vkr_project_contained(job, workspace, value, true_v, contained));
  return vkr_project_source_file(job, contained, out);
}

bool8_t vkr_project_remap_overlay(VkrProjectJob *job, const char *path,
                                  const char *expected, VkrBakeryJson *scene,
                                  const char *source_root, bool8_t managed) {
  Arena *arena = job->arena;
  VkrBakeryJson *overlay =
      vkr_project_load_json(job, path, VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(overlay);
  VKR_PROJECT_TRY(vkr_project_checked_overlay(job, overlay));
  VKR_PROJECT_TRY(vkr_project_remap_overlay_indices(
      job, overlay, vkr_bakery_json_get(scene, "entities")));
  char current[17];
  vkr_project_scene_id_fingerprint(job, current);
  VkrBakeryJson *seen = vkr_bakery_json_object(arena);
  VkrBakeryJson *overrides = vkr_bakery_json_get(overlay, "overrides");
  for (VkrBakeryJson *record = overrides->first; record;
       record = record->next) {
    int64_t entity = 0;
    int64_t node = 0;
    if (!vkr_project_integer(vkr_bakery_json_get(record, "scene_entity"),
                             &entity) ||
        !vkr_project_integer(vkr_bakery_json_get(record, "gltf_node"), &node)) {
      return vkr_project_fail(job, "Invalid authored override source identity");
    }
    const char *key = vkr_project_printf(job, "%lld|%lld", (long long)entity,
                                         (long long)node);
    if (vkr_bakery_json_get(seen, key)) {
      return vkr_project_fail(job,
                              "Duplicate authored override source identity");
    }
    vkr_bakery_json_set(arena, seen, key, vkr_bakery_json_bool(arena, true_v));
    VkrProjectOverlayReference references[20];
    uint32_t count = 0u;
    VKR_PROJECT_TRY(vkr_project_overlay_references(
        job, record, references, ArrayCount(references), &count));
    for (uint32_t r = 0u; r < count; ++r) {
      VkrBakeryJson *reference = references[r].reference;
      const VkrBakeryJson *value =
          vkr_bakery_json_get(reference, references[r].field);
      if (!references[r].required &&
          vkr_bakery_json_is_string(value, vkr_project_zero_fingerprint)) {
        continue;
      }
      char old_hash[17];
      char new_hash[17];
      VKR_PROJECT_TRY(vkr_project_overlay_identity(
          job, reference, expected, scene, job->stage, old_hash));
      if (!vkr_bakery_json_is_string(value, old_hash)) {
        return vkr_project_fail(
            job, "Authored overrides conflict with the selected source scene");
      }
      VKR_PROJECT_TRY(vkr_project_overlay_identity(
          job, reference, current, scene, job->stage, new_hash));
      vkr_bakery_json_set(arena, reference, references[r].field,
                          vkr_bakery_json_cstr(arena, new_hash));
    }
  }
  VkrProjectNodes colliders;
  VKR_PROJECT_TRY(vkr_project_overlay_colliders(job, overlay, &colliders));
  for (uint32_t i = 0u; i < colliders.count; ++i) {
    VkrBakeryJson *collider = colliders.items[i];
    char source[VKR_PROJECT_PATH];
    char validated[VKR_PROJECT_PATH];
    char copied[VKR_PROJECT_PATH];
    char collisions[VKR_PROJECT_PATH];
    char relative[VKR_PROJECT_PATH];
    char final_path[VKR_PROJECT_PATH];
    const char *reference = NULL;
    (void)snprintf(collisions, sizeof(collisions), "%s/builds/collisions",
                   job->stage);
    VKR_PROJECT_TRY(vkr_project_import_overlay_collision(
        job, vkr_project_json_text(collider, "asset"), source_root,
        managed ? scene : NULL, source));
    VKR_PROJECT_TRY(vkr_project_inspect_collision(job, source, validated));
    VKR_PROJECT_TRY(vkr_project_copy_blob(job, validated, collisions, copied));
    if (!vkr_bakery_path_relative(job->stage, copied, relative,
                                  sizeof(relative)) ||
        !vkr_bakery_path_join(final_path, sizeof(final_path), job->final_path,
                              relative)) {
      return vkr_project_fail(job, "Path too long");
    }
    VKR_PROJECT_TRY(vkr_project_managed_reference(job, final_path,
                                                  job->workspace, &reference));
    vkr_bakery_json_set(arena, collider, "asset",
                        vkr_bakery_json_cstr(arena, reference));
  }
  return vkr_project_atomic_json(job, path, overlay);
}

bool8_t vkr_project_validate_overlay_dependencies(VkrProjectJob *job,
                                                  const VkrBakeryJson *overlay,
                                                  const char *root) {
  VkrProjectNodes colliders;
  VKR_PROJECT_TRY(vkr_project_overlay_colliders(job, overlay, &colliders));
  const char *final_reference = NULL;
  if (colliders.count) {
    VKR_PROJECT_TRY(vkr_project_managed_reference(
        job, job->final_path, job->workspace, &final_reference));
  }
  for (uint32_t i = 0u; i < colliders.count; ++i) {
    const char *value = vkr_project_json_text(colliders.items[i], "asset");
    VKR_PROJECT_TRY(vkr_project_validate_managed_path(job, value));
    const uint64_t length = strlen(final_reference);
    char path[VKR_PROJECT_PATH];
    char validated[VKR_PROJECT_PATH];
    if (strncmp(value, final_reference, length) == 0 && value[length] == '/') {
      VKR_PROJECT_TRY(
          vkr_project_contained(job, root, value + length + 1u, true_v, path));
    } else {
      VKR_PROJECT_TRY(
          vkr_project_contained(job, job->workspace, value, true_v, path));
    }
    VKR_PROJECT_TRY(vkr_project_inspect_collision(job, path, validated));
  }
  return true_v;
}

// =============================================================================
// Model animations
// =============================================================================

void vkr_project_bind_model_animations(VkrProjectJob *job, VkrBakeryJson *scene,
                                       const VkrProjectInventory *inventories,
                                       uint32_t inventory_count) {
  Arena *arena = job->arena;
  VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  for (VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    const VkrBakeryJson *reference =
        vkr_bakery_json_get(vkr_bakery_json_get(entity, "mesh"), "asset");
    if (!reference || reference->type != VKR_BAKERY_JSON_OBJECT) {
      continue;
    }
    const VkrBakeryJson *records = NULL;
    for (uint32_t i = 0u; i < inventory_count; ++i) {
      if (vkr_bakery_json_is_string(vkr_bakery_json_get(reference, "scope"),
                                    inventories[i].scope)) {
        records = inventories[i].records;
      }
    }
    const VkrBakeryJson *record = vkr_project_record_by_id(
        records, vkr_project_json_text(reference, "id"));
    if (!record) {
      continue;
    }
    VkrBakeryJson *bank = vkr_bakery_json_clone(arena, reference);
    vkr_bakery_json_set(arena, bank, "role",
                        vkr_bakery_json_cstr(arena, "animation"));
    bool8_t products = false_v;
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    for (const VkrBakeryJson *item = artifacts ? artifacts->first : NULL; item;
         item = item->next) {
      products = products ||
                 vkr_bakery_json_is_string(vkr_bakery_json_get(item, "role"),
                                           "animation");
    }
    const VkrBakeryJson *animation = vkr_bakery_json_get(entity, "animation");
    if (products && (!animation || animation->type == VKR_BAKERY_JSON_NULL)) {
      VkrBakeryJson *component = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, component, "asset", bank);
      vkr_bakery_json_set(arena, entity, "animation", component);
    } else if (!products && animation &&
               animation->type == VKR_BAKERY_JSON_OBJECT &&
               vkr_bakery_json_equal(vkr_bakery_json_get(animation, "asset"),
                                     bank)) {
      /* A reimport can legitimately replace an animated model with a
         static one. */
      vkr_bakery_json_remove(entity, "animation");
    }
  }
}

// =============================================================================
// Scene inspection
// =============================================================================

vkr_internal void vkr_project_uri_unquote(const char *uri, char *out,
                                          uint32_t capacity) {
  uint32_t length = 0u;
  for (const char *c = uri; *c && length + 1u < capacity; ++c) {
    int32_t high = -1;
    int32_t low = -1;
    if (*c == '%' && c[1] && c[2]) {
      high = (c[1] >= '0' && c[1] <= '9')   ? c[1] - '0'
             : (c[1] >= 'a' && c[1] <= 'f') ? c[1] - 'a' + 10
             : (c[1] >= 'A' && c[1] <= 'F') ? c[1] - 'A' + 10
                                            : -1;
      low = (c[2] >= '0' && c[2] <= '9')   ? c[2] - '0'
            : (c[2] >= 'a' && c[2] <= 'f') ? c[2] - 'a' + 10
            : (c[2] >= 'A' && c[2] <= 'F') ? c[2] - 'A' + 10
                                           : -1;
    }
    if (high >= 0 && low >= 0) {
      out[length++] = (char)(high * 16 + low);
      c += 2;
    } else {
      out[length++] = *c;
    }
  }
  out[length] = 0;
}

vkr_internal bool8_t vkr_project_inspect_gltf_dependencies(
    VkrProjectJob *job, const char *path, VkrProjectStrings *missing) {
  VkrBakeryJson *document =
      vkr_project_load_json(job, path, VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(document);
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), path);
  static const char *const fields[] = {"buffers", "images"};
  for (uint32_t f = 0u; f < ArrayCount(fields); ++f) {
    const VkrBakeryJson *entries = vkr_bakery_json_get(document, fields[f]);
    for (const VkrBakeryJson *entry = entries ? entries->first : NULL; entry;
         entry = entry->next) {
      const VkrBakeryJson *uri_value = vkr_bakery_json_get(entry, "uri");
      if (!uri_value || uri_value->type != VKR_BAKERY_JSON_STRING) {
        continue;
      }
      const char *uri = (const char *)uri_value->string.str;
      if (strncmp(uri, "data:", 5u) == 0) {
        continue;
      }
      const bool8_t failed_before = job->failed;
      char value[VKR_PROJECT_PATH];
      char located[VKR_PROJECT_PATH];
      bool8_t found = vkr_project_gltf_uri_path(job, uri, value, sizeof(value));
      if (found && f == 1u) {
        vkr_project_gltf_texture_source(value, origin, job->legacy_root,
                                        located);
      } else if (found) {
        found = vkr_bakery_path_join(located, sizeof(located), origin, value);
      }
      found = found && vkr_bakery_is_file(located);
      vkr_project_forgive(job, failed_before);
      if (!found) {
        char unquoted[VKR_PROJECT_PATH];
        vkr_project_uri_unquote(uri, unquoted, sizeof(unquoted));
        VKR_PROJECT_TRY(vkr_project_strings_push(
            job, missing,
            vkr_project_printf(job, "%s: %s", vkr_bakery_path_name(path),
                               unquoted)));
      }
    }
  }
  return true_v;
}

vkr_internal int vkr_project_compare_text(const void *lhs, const void *rhs) {
  return strcmp(*(const char *const *)lhs, *(const char *const *)rhs);
}

vkr_internal bool8_t vkr_project_add_unique(VkrProjectJob *job,
                                            VkrProjectStrings *list,
                                            const char *text) {
  for (uint32_t i = 0u; i < list->count; ++i) {
    if (strcmp(list->items[i], text) == 0) {
      return true_v;
    }
  }
  return vkr_project_strings_push(job, list, text);
}

VkrBakeryJson *vkr_project_inspect_scene(VkrProjectJob *job) {
  Arena *arena = job->arena;
  char source[VKR_PROJECT_PATH];
  if (!vkr_project_source_file(
          job, vkr_project_json_text(job->request, "source_scene"), source)) {
    return NULL;
  }
  VkrBakeryJson *scene =
      vkr_project_load_json(job, source, VKR_PROJECT_MAX_JSON_BYTES);
  if (!scene) {
    return NULL;
  }
  const VkrBakeryJson *version_value = vkr_bakery_json_get(scene, "version");
  int64_t version = 1;
  if (version_value) {
    version = version_value->type == VKR_BAKERY_JSON_INT
                  ? version_value->integer
                  : -1;
  }
  const VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  if (entities && entities->type != VKR_BAKERY_JSON_ARRAY) {
    vkr_project_fail(job, "Invalid entity array");
    return NULL;
  }
  char overlay[VKR_PROJECT_PATH];
  (void)snprintf(overlay, sizeof(overlay), "%s.editor.json", source);
  VkrBakeryJson *warnings = vkr_bakery_json_array(arena);
  VkrBakeryJson *report = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, report, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, report, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, report, "scene_version",
                      version_value
                          ? vkr_bakery_json_clone(arena, version_value)
                          : vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(
      arena, report, "entities",
      vkr_bakery_json_int(arena, entities ? entities->count : 0));
  vkr_bakery_json_set(arena, report, "meshes", vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, report, "materials",
                      vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, report, "probes", vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, report, "missing", vkr_bakery_json_array(arena));
  vkr_bakery_json_set(arena, report, "missing_count",
                      vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, report, "missing_images",
                      vkr_bakery_json_int(arena, 0));
  vkr_bakery_json_set(arena, report, "warnings", warnings);
  vkr_bakery_json_set(arena, report, "saved_edits",
                      vkr_bakery_json_bool(arena, vkr_bakery_is_file(overlay)));
  if (version == 3 || version == 4 || version == VKR_PROJECT_SCENE_VERSION) {
    /* A managed scene bundle is its own closure; the import validates it. */
    int64_t meshes = 0;
    int64_t materials = 0;
    const VkrBakeryJson *assets = vkr_bakery_json_get(scene, "assets");
    for (const VkrBakeryJson *asset = assets ? assets->first : NULL; asset;
         asset = asset->next) {
      const VkrBakeryJson *kind = vkr_bakery_json_get(asset, "kind");
      meshes += vkr_bakery_json_is_string(kind, "mesh") ? 1 : 0;
      materials += vkr_bakery_json_is_string(kind, "material") ? 1 : 0;
    }
    vkr_bakery_json_set(arena, report, "meshes",
                        vkr_bakery_json_int(arena, meshes));
    vkr_bakery_json_set(arena, report, "materials",
                        vkr_bakery_json_int(arena, materials));
    return report;
  }
  if (version != 1 && version != 2) {
    vkr_project_fail(job, "Unsupported scene JSON version");
    return NULL;
  }
  VkrProjectStrings missing = {0};
  VkrProjectStrings meshes = {0};
  VkrProjectStrings materials = {0};
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    if (entity->type != VKR_BAKERY_JSON_OBJECT) {
      continue;
    }
    const VkrBakeryJson *mesh = vkr_bakery_json_get(entity, "mesh");
    const VkrBakeryJson *mesh_path = vkr_bakery_json_get(mesh, "path");
    if (mesh && mesh->type == VKR_BAKERY_JSON_OBJECT &&
        vkr_project_truthy(mesh_path) &&
        mesh_path->type == VKR_BAKERY_JSON_STRING) {
      if (!vkr_project_add_unique(job, &meshes,
                                  (const char *)mesh_path->string.str)) {
        return NULL;
      }
    }
    const VkrBakeryJson *shape = vkr_bakery_json_get(entity, "shape");
    const VkrBakeryJson *material = vkr_bakery_json_get(shape, "material");
    const VkrBakeryJson *material_path = vkr_bakery_json_get(material, "path");
    if (material && material->type == VKR_BAKERY_JSON_OBJECT &&
        vkr_project_truthy(material_path) &&
        material_path->type == VKR_BAKERY_JSON_STRING) {
      if (!vkr_project_add_unique(job, &materials,
                                  (const char *)material_path->string.str)) {
        return NULL;
      }
    }
    const VkrBakeryJson *text = vkr_bakery_json_get(entity, "text3d");
    const char *font = vkr_project_json_text(text, "font");
    if (text && text->type == VKR_BAKERY_JSON_OBJECT && font &&
        strcmp(font, "UbuntuMono") && strcmp(font, "UbuntuMono-cooked") &&
        strcmp(font, "default-scene-font")) {
      vkr_bakery_json_append(
          warnings,
          vkr_bakery_json_cstr(
              arena,
              vkr_project_printf(job,
                                 "Text font %s needs its source located before "
                                 "import",
                                 font)));
    }
  }
  char origin[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(origin, sizeof(origin), source);
  if (meshes.count) {
    qsort(meshes.items, meshes.count, sizeof(meshes.items[0]),
          vkr_project_compare_text);
  }
  if (materials.count) {
    qsort(materials.items, materials.count, sizeof(materials.items[0]),
          vkr_project_compare_text);
  }
  for (uint32_t i = 0u; i < meshes.count; ++i) {
    char path[VKR_PROJECT_PATH];
    const bool8_t failed_before = job->failed;
    const bool8_t found = vkr_project_legacy_source(
        job, meshes.items[i], origin, job->legacy_root, path);
    vkr_project_forgive(job, failed_before);
    if (!found) {
      if (!vkr_project_strings_push(job, &missing, meshes.items[i])) {
        return NULL;
      }
      continue;
    }
    char suffix[16];
    vkr_project_suffix_lower(path, suffix, sizeof(suffix));
    if (strcmp(suffix, ".gltf") == 0 &&
        !vkr_project_inspect_gltf_dependencies(job, path, &missing)) {
      return NULL;
    }
  }
  for (uint32_t i = 0u; i < materials.count; ++i) {
    char path[VKR_PROJECT_PATH];
    const bool8_t failed_before = job->failed;
    const bool8_t found = vkr_project_legacy_source(
        job, materials.items[i], origin, job->legacy_root, path);
    vkr_project_forgive(job, failed_before);
    if (!found &&
        !vkr_project_strings_push(job, &missing, materials.items[i])) {
      return NULL;
    }
  }
  int64_t probe_count = 0;
  const VkrBakeryJson *probes = vkr_bakery_json_get(scene, "reflection_probes");
  for (const VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    probe_count +=
        probe->type == VKR_BAKERY_JSON_OBJECT &&
                vkr_project_truthy(vkr_bakery_json_get(probe, "cubemap"))
            ? 1
            : 0;
  }
  const VkrBakeryJson *volume = vkr_bakery_json_get(scene, "diffuse_volume");
  const char *volume_path = vkr_project_json_text(volume, "path");
  if (volume && volume->type == VKR_BAKERY_JSON_OBJECT && volume_path &&
      volume_path[0]) {
    char path[VKR_PROJECT_PATH];
    const bool8_t failed_before = job->failed;
    const bool8_t found = vkr_project_legacy_source(job, volume_path, origin,
                                                    job->legacy_root, path);
    vkr_project_forgive(job, failed_before);
    if (!found && !vkr_project_strings_push(job, &missing, volume_path)) {
      return NULL;
    }
  }
  const VkrBakeryJson *environment = vkr_bakery_json_get(scene, "environment");
  if (environment && environment->type == VKR_BAKERY_JSON_OBJECT) {
    static const char *const removed[] = {"equirect", "cubemap"};
    for (uint32_t i = 0u; i < ArrayCount(removed); ++i) {
      if (vkr_bakery_json_get(environment, removed[i])) {
        vkr_bakery_json_append(
            warnings,
            vkr_bakery_json_cstr(
                arena, vkr_project_printf(job,
                                          "The removed environment %s sky "
                                          "image will be dropped",
                                          removed[i])));
      }
    }
  }
  int64_t missing_images = 0;
  VkrBakeryJson *listed = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < missing.count; ++i) {
    const char *item = missing.items[i];
    const char *tail = item;
    for (const char *found = strstr(item, ": "); found;
         found = strstr(found + 2, ": ")) {
      tail = found + 2;
    }
    char suffix[16];
    vkr_project_suffix_lower(tail, suffix, sizeof(suffix));
    missing_images += (!strcmp(suffix, ".png") || !strcmp(suffix, ".jpg") ||
                       !strcmp(suffix, ".jpeg") || !strcmp(suffix, ".bmp") ||
                       !strcmp(suffix, ".tga"))
                          ? 1
                          : 0;
    if (i < 16u) {
      vkr_bakery_json_append(listed, vkr_bakery_json_cstr(arena, item));
    }
  }
  vkr_bakery_json_set(arena, report, "probes",
                      vkr_bakery_json_int(arena, probe_count));
  vkr_bakery_json_set(arena, report, "meshes",
                      vkr_bakery_json_int(arena, meshes.count));
  vkr_bakery_json_set(arena, report, "materials",
                      vkr_bakery_json_int(arena, materials.count));
  vkr_bakery_json_set(arena, report, "missing_count",
                      vkr_bakery_json_int(arena, missing.count));
  vkr_bakery_json_set(arena, report, "missing_images",
                      vkr_bakery_json_int(arena, missing_images));
  vkr_bakery_json_set(arena, report, "missing", listed);
  return report;
}

// =============================================================================
// Prefabs and new entities
// =============================================================================

typedef struct VkrProjectPrefabCopy {
  VkrProjectJob *job;
  const VkrBakeryJson *source;
  const char *prefab_root;
  VkrBakeryJson *copied;
} VkrProjectPrefabCopy;

vkr_internal VkrBakeryJson *
vkr_project_prefab_asset(VkrProjectPrefabCopy *copy,
                         const VkrBakeryJson *value) {
  VkrProjectJob *job = copy->job;
  Arena *arena = job->arena;
  if (value->type == VKR_BAKERY_JSON_ARRAY) {
    VkrBakeryJson *result = vkr_bakery_json_array(arena);
    for (const VkrBakeryJson *item = value->first; item; item = item->next) {
      VkrBakeryJson *mapped = vkr_project_prefab_asset(copy, item);
      if (!mapped) {
        return NULL;
      }
      vkr_bakery_json_append(result, mapped);
    }
    return result;
  }
  if (value->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_bakery_json_clone(arena, value);
  }
  const VkrBakeryJson *id = vkr_bakery_json_get(value, "id");
  if (vkr_bakery_json_is_string(vkr_bakery_json_get(value, "scope"), "scene") &&
      id) {
    const char *key =
        id->type == VKR_BAKERY_JSON_STRING ? (const char *)id->string.str : "";
    const VkrBakeryJson *existing = vkr_bakery_json_get(copy->copied, key);
    if (!existing) {
      const VkrBakeryJson *records =
          vkr_bakery_json_get(copy->source, "assets");
      const VkrBakeryJson *match = NULL;
      uint32_t matches = 0u;
      for (const VkrBakeryJson *record = records ? records->first : NULL;
           record; record = record->next) {
        if (vkr_bakery_json_is_string(vkr_bakery_json_get(record, "id"), key)) {
          match = record;
          matches += 1u;
        }
      }
      if (matches != 1u) {
        vkr_project_fail(job, "The prefab references an unknown scene asset");
        return NULL;
      }
      VkrBakeryJson *record = vkr_bakery_json_clone(arena, match);
      char new_id[37];
      vkr_project_uuid4(new_id);
      /* The bundle directory holds the artifact's dependencies. */
      if (!vkr_project_copy_bundle(job, record, copy->prefab_root, new_id,
                                   "A prefab asset contains a symlink",
                                   "A prefab asset has no artifact; rebuild "
                                   "it")) {
        return NULL;
      }
      vkr_bakery_json_append(job->assets, record);
      vkr_bakery_json_set(arena, copy->copied, key,
                          vkr_bakery_json_cstr(arena, new_id));
      existing = vkr_bakery_json_get(copy->copied, key);
    }
    VkrBakeryJson *result = vkr_bakery_json_clone(arena, value);
    vkr_bakery_json_set(arena, result, "id",
                        vkr_bakery_json_clone(arena, existing));
    return result;
  }
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  for (const VkrBakeryJson *field = value->first; field; field = field->next) {
    VkrBakeryJson *mapped = vkr_project_prefab_asset(copy, field);
    if (!mapped) {
      return NULL;
    }
    vkr_bakery_json_set(arena, result, (const char *)field->key.str, mapped);
  }
  return result;
}

/* str(value)[:512] for a UTF-8 string: the first 512 code points. */
vkr_internal const char *vkr_project_truncate_code_points(VkrProjectJob *job,
                                                          const char *text,
                                                          uint32_t limit) {
  uint32_t points = 0u;
  const char *c = text;
  while (*c) {
    if (((uint8_t)*c & 0xC0u) != 0x80u) {
      if (points == limit) {
        break;
      }
      points += 1u;
    }
    c += 1;
  }
  return vkr_project_printf(job, "%.*s", (int)(c - text), text);
}

vkr_internal bool8_t vkr_project_instantiate_prefab(
    VkrProjectJob *job, VkrBakeryJson *scene, const VkrBakeryJson *prefab) {
  Arena *arena = job->arena;
  if (!prefab || prefab->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_project_fail(job, "A prefab needs a scene id");
  }
  const char *prefab_id = NULL;
  VKR_PROJECT_TRY(vkr_project_identifier(
      job, vkr_bakery_json_get(prefab, "scene_id"), &prefab_id));
  VkrBakeryJson *project = vkr_project_document(job);
  VKR_PROJECT_TRY(project);
  const VkrBakeryJson *member = vkr_project_record_by_id(
      vkr_bakery_json_get(project, "scenes"), prefab_id);
  if (strcmp(prefab_id, job->scene_id) == 0 || !member) {
    return vkr_project_fail(job,
                            "A prefab must be another scene of this project");
  }
  char prefab_root[VKR_PROJECT_PATH];
  char prefab_path[VKR_PROJECT_PATH];
  (void)snprintf(prefab_root, sizeof(prefab_root), "%s/scenes/%s",
                 job->project_root, prefab_id);
  (void)snprintf(prefab_path, sizeof(prefab_path), "%s/scene.json",
                 prefab_root);
  VkrBakeryJson *source = vkr_project_read_managed_scene(job, prefab_path);
  VKR_PROJECT_TRY(source);
  VKR_PROJECT_TRY(
      vkr_project_migrate_source_references(job, source, prefab_path));
  const VkrBakeryJson *entities = vkr_bakery_json_get(source, "entities");
  VkrBakeryJson *target = vkr_bakery_json_get(scene, "entities");
  const uint32_t source_count = entities ? entities->count : 0u;
  if ((uint64_t)target->count + 1u + source_count > VKR_PROJECT_MAX_ENTITIES) {
    return vkr_project_fail(job,
                            "Adding entities exceeds the scene entity limit");
  }
  VkrProjectPrefabCopy copy = {.job = job,
                               .source = source,
                               .prefab_root = prefab_root,
                               .copied = vkr_bakery_json_object(arena)};
  const char *name = vkr_project_json_text(prefab, "name");
  if (!name || !name[0]) {
    name = vkr_project_json_text(member, "name");
  }
  if (!name || !name[0]) {
    name = "Prefab";
  }
  char id[37];
  vkr_project_uuid4(id);
  VkrBakeryJson *root = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, root, "id", vkr_bakery_json_cstr(arena, id));
  vkr_bakery_json_set(
      arena, root, "name",
      vkr_bakery_json_cstr(arena,
                           vkr_project_truncate_code_points(job, name, 512u)));
  vkr_bakery_json_set(arena, root, "parent", vkr_bakery_json_null(arena));
  const VkrBakeryJson *transform = vkr_bakery_json_get(prefab, "transform");
  vkr_bakery_json_set(arena, root, "transform",
                      vkr_project_truthy(transform)
                          ? vkr_bakery_json_clone(arena, transform)
                          : vkr_project_identity_transform(job));
  const int64_t offset = (int64_t)target->count + 1;
  vkr_bakery_json_append(target, root);
  for (const VkrBakeryJson *entity = entities ? entities->first : NULL; entity;
       entity = entity->next) {
    VkrBakeryJson *instance = vkr_project_prefab_asset(&copy, entity);
    VKR_PROJECT_TRY(instance);
    vkr_project_uuid4(id);
    vkr_bakery_json_set(arena, instance, "id", vkr_bakery_json_cstr(arena, id));
    const VkrBakeryJson *parent = vkr_bakery_json_get(instance, "parent");
    int64_t parent_index = 0;
    if (!parent || parent->type == VKR_BAKERY_JSON_NULL) {
      parent_index = offset - 1;
    } else if (vkr_project_integer(parent, &parent_index)) {
      parent_index += offset;
    } else {
      return vkr_project_fail(job, "Invalid prefab entity parent");
    }
    vkr_bakery_json_set(arena, instance, "parent",
                        vkr_bakery_json_int(arena, parent_index));
    vkr_bakery_json_append(target, instance);
  }
  return true_v;
}

vkr_internal VkrBakeryJson *vkr_project_new_mesh_entity(VkrProjectJob *job,
                                                        const char *name,
                                                        VkrBakeryJson *asset) {
  Arena *arena = job->arena;
  char id[37];
  vkr_project_uuid4(id);
  VkrBakeryJson *entity = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entity, "id", vkr_bakery_json_cstr(arena, id));
  vkr_bakery_json_set(arena, entity, "name", vkr_bakery_json_cstr(arena, name));
  vkr_bakery_json_set(arena, entity, "parent", vkr_bakery_json_null(arena));
  vkr_bakery_json_set(arena, entity, "transform",
                      vkr_project_identity_transform(job));
  VkrBakeryJson *mesh = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, mesh, "asset", asset);
  vkr_bakery_json_set(arena, mesh, "pipeline_domain",
                      vkr_bakery_json_cstr(arena, "world"));
  vkr_bakery_json_set(arena, entity, "mesh", mesh);
  return entity;
}

const char *vkr_project_strip_text(VkrProjectJob *job, const char *text) {
  const char *start = text;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r' ||
         *start == '\v' || *start == '\f') {
    start += 1;
  }
  const char *end = start + strlen(start);
  while (end > start &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
          end[-1] == '\r' || end[-1] == '\v' || end[-1] == '\f')) {
    end -= 1;
  }
  return vkr_project_printf(job, "%.*s", (int)(end - start), start);
}

bool8_t vkr_project_append_entities(VkrProjectJob *job, VkrBakeryJson *scene,
                                    int64_t *out_first) {
  Arena *arena = job->arena;
  const VkrBakeryJson *models = vkr_bakery_json_get(job->request, "models");
  const VkrBakeryJson *lights = vkr_bakery_json_get(job->request, "lights");
  const VkrBakeryJson *prefabs = vkr_bakery_json_get(job->request, "prefabs");
  if ((models && models->type != VKR_BAKERY_JSON_ARRAY) ||
      (lights && lights->type != VKR_BAKERY_JSON_ARRAY) ||
      (prefabs && prefabs->type != VKR_BAKERY_JSON_ARRAY)) {
    return vkr_project_fail(job, "Models, lights and prefabs must be arrays");
  }
  VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  if (!entities) {
    entities = vkr_bakery_json_array(arena);
    vkr_bakery_json_set(arena, scene, "entities", entities);
  }
  const int64_t first = entities->count;
  if ((uint64_t)first + (models ? models->count : 0u) +
          (lights ? lights->count : 0u) >
      VKR_PROJECT_MAX_ENTITIES) {
    return vkr_project_fail(job,
                            "Adding entities exceeds the scene entity limit");
  }
  for (const VkrBakeryJson *prefab = prefabs ? prefabs->first : NULL; prefab;
       prefab = prefab->next) {
    VKR_PROJECT_TRY(vkr_project_instantiate_prefab(job, scene, prefab));
  }
  for (const VkrBakeryJson *model = models ? models->first : NULL; model;
       model = model->next) {
    const bool8_t object = model->type == VKR_BAKERY_JSON_OBJECT;
    if (object && vkr_bakery_json_get(model, "asset")) {
      /* An existing mesh asset, as a Content drop places it; lowering
         verifies the reference names a built mesh. */
      const VkrBakeryJson *asset = vkr_bakery_json_get(model, "asset");
      const VkrBakeryJson *scope = vkr_bakery_json_get(asset, "scope");
      const char *asset_id = vkr_project_json_text(asset, "id");
      if (!asset || asset->type != VKR_BAKERY_JSON_OBJECT ||
          !(vkr_bakery_json_is_string(scope, "scene") ||
            vkr_bakery_json_is_string(scope, "project") ||
            vkr_bakery_json_is_string(scope, "editor")) ||
          !asset_id || !asset_id[0]) {
        return vkr_project_fail(job,
                                "A placed model needs an asset scope and id");
      }
      const char *name = vkr_project_json_text(model, "name");
      if (!name || !vkr_project_strip_text(job, name)[0] ||
          strlen(name) > 512u) {
        name = "Mesh";
      }
      VkrBakeryJson *reference = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, reference, "scope",
                          vkr_bakery_json_clone(arena, scope));
      vkr_bakery_json_set(arena, reference, "id",
                          vkr_bakery_json_cstr(arena, asset_id));
      vkr_bakery_json_set(arena, reference, "role",
                          vkr_bakery_json_cstr(arena, "mesh"));
      VkrBakeryJson *entity = vkr_project_new_mesh_entity(
          job, vkr_project_strip_text(job, name), reference);
      const VkrBakeryJson *transform = vkr_bakery_json_get(model, "transform");
      if (vkr_project_truthy(transform)) {
        vkr_bakery_json_set(arena, entity, "transform",
                            vkr_bakery_json_clone(arena, transform));
      }
      vkr_bakery_json_append(entities, entity);
      continue;
    }
    const char *value = object ? vkr_project_json_text(model, "source")
                               : (model->type == VKR_BAKERY_JSON_STRING
                                      ? (const char *)model->string.str
                                      : NULL);
    char source[VKR_PROJECT_PATH];
    VKR_PROJECT_TRY(vkr_project_source_file(job, value, source));
    char suffix[16];
    vkr_project_suffix_lower(source, suffix, sizeof(suffix));
    VkrBakeryJson *reference = NULL;
    if (strcmp(suffix, ".vkb") == 0) {
      reference = vkr_project_import_cooked_mesh(job, source);
    } else if (!strcmp(suffix, ".obj") || !strcmp(suffix, ".gltf") ||
               !strcmp(suffix, ".glb")) {
      reference = vkr_project_import_model(job, source);
    } else {
      return vkr_project_fail(
          job, "Models must be OBJ, glTF, GLB or cooked VKB files");
    }
    VKR_PROJECT_TRY(reference);
    char stem[512];
    vkr_project_stem(source, stem, sizeof(stem));
    VkrBakeryJson *entity = vkr_project_new_mesh_entity(job, stem, reference);
    const VkrBakeryJson *transform =
        object ? vkr_bakery_json_get(model, "transform") : NULL;
    if (vkr_project_truthy(transform)) {
      vkr_bakery_json_set(arena, entity, "transform",
                          vkr_bakery_json_clone(arena, transform));
    }
    vkr_bakery_json_append(entities, entity);
  }
  static const char *const kinds[] = {"point_light", "spot_light",
                                      "directional_light", "rectangle_light"};
  static const char *const forbidden[] = {"mesh", "shape", "text3d"};
  for (const VkrBakeryJson *light = lights ? lights->first : NULL; light;
       light = light->next) {
    uint32_t present = 0u;
    for (uint32_t k = 0u; k < ArrayCount(kinds); ++k) {
      present += vkr_bakery_json_get(light, kinds[k]) ? 1u : 0u;
    }
    if (light->type != VKR_BAKERY_JSON_OBJECT || present != 1u) {
      return vkr_project_fail(
          job, "Each light requires exactly one light component");
    }
    for (uint32_t f = 0u; f < ArrayCount(forbidden); ++f) {
      if (vkr_bakery_json_get(light, forbidden[f])) {
        return vkr_project_fail(job, "New light entities cannot contain model, "
                                     "shape or text components");
      }
    }
    VkrBakeryJson *entity = vkr_bakery_json_clone(arena, light);
    VkrBakeryJson *spot = vkr_bakery_json_get(entity, "spot_light");
    if (spot) {
      vkr_bakery_json_remove(entity, "spot_light");
      VkrBakeryJson *point = spot->type == VKR_BAKERY_JSON_OBJECT
                                 ? spot
                                 : vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, point, "kind", vkr_bakery_json_int(arena, 2));
      vkr_bakery_json_set(arena, entity, "point_light", point);
    }
    char id[37];
    vkr_project_uuid4(id);
    vkr_bakery_json_set(arena, entity, "id", vkr_bakery_json_cstr(arena, id));
    vkr_bakery_json_append(entities, entity);
  }
  *out_first = first;
  return true_v;
}

// =============================================================================
// Semantic checks
// =============================================================================

vkr_internal bool8_t vkr_project_nonnegative(const VkrBakeryJson *object,
                                             const char *key) {
  const VkrBakeryJson *value = vkr_bakery_json_get(object, key);
  float64_t number = 1.0;
  if (value && !vkr_project_number(value, &number)) {
    return false_v;
  }
  return isfinite(number) && number >= 0.0;
}

bool8_t vkr_project_validate_semantics(VkrProjectJob *job,
                                       const VkrBakeryJson *scene) {
  const VkrBakeryJson *entities = vkr_bakery_json_get(scene, "entities");
  if (entities && (entities->type != VKR_BAKERY_JSON_ARRAY ||
                   entities->count > VKR_PROJECT_MAX_ENTITIES)) {
    return vkr_project_fail(job, "Invalid entity array");
  }
  const VkrBakeryJson *environment = vkr_bakery_json_get(scene, "environment");
  if (!vkr_project_truthy(environment)) {
    environment = NULL;
  }
  if (vkr_bakery_json_get(environment, "asset") ||
      vkr_bakery_json_get(environment, "equirect") ||
      vkr_bakery_json_get(environment, "cubemap")) {
    return vkr_project_fail(job, "Scene environment images were removed; "
                                 "author atmosphere or environment.constant");
  }
  static const char *const scales[] = {"intensity", "diffuse_intensity",
                                       "specular_intensity"};
  for (uint32_t i = 0u; i < ArrayCount(scales); ++i) {
    if (!vkr_project_nonnegative(environment, scales[i])) {
      return vkr_project_fail(job,
                              "Environment %s must be finite and "
                              "nonnegative",
                              scales[i]);
    }
  }
  const VkrBakeryJson *enabled = vkr_bakery_json_get(environment, "enabled");
  if (enabled && enabled->type != VKR_BAKERY_JSON_BOOL) {
    return vkr_project_fail(job, "Environment enabled must be boolean");
  }
  const VkrBakeryJson *probes = vkr_bakery_json_get(scene, "reflection_probes");
  if (probes &&
      (probes->type != VKR_BAKERY_JSON_ARRAY || probes->count > 64u)) {
    return vkr_project_fail(job, "Invalid reflection probe array");
  }
  static const char *const vectors[] = {"center", "extents"};
  static const char *const probe_scales[] = {
      "blend_distance", "intensity", "diffuse_intensity", "specular_intensity"};
  for (const VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    for (uint32_t v = 0u; v < ArrayCount(vectors); ++v) {
      const VkrBakeryJson *values = vkr_bakery_json_get(probe, vectors[v]);
      bool8_t valid = values && values->type == VKR_BAKERY_JSON_ARRAY &&
                      values->count == 3u;
      bool8_t positive = true_v;
      for (const VkrBakeryJson *item = valid ? values->first : NULL; item;
           item = item->next) {
        float64_t number = 0.0;
        valid = valid && vkr_project_number(item, &number) && isfinite(number);
        positive = positive && number > 0.0;
      }
      if (!valid) {
        return vkr_project_fail(job, "Probe %s requires three finite values",
                                vectors[v]);
      }
      if (v == 1u && !positive) {
        return vkr_project_fail(job, "Probe extents must be positive");
      }
    }
    for (uint32_t i = 0u; i < ArrayCount(probe_scales); ++i) {
      if (!vkr_project_nonnegative(probe, probe_scales[i])) {
        return vkr_project_fail(job, "Probe %s must be finite and nonnegative",
                                probe_scales[i]);
      }
    }
    const VkrBakeryJson *probe_enabled = vkr_bakery_json_get(probe, "enabled");
    if (probe_enabled && probe_enabled->type != VKR_BAKERY_JSON_BOOL) {
      return vkr_project_fail(job, "Probe enabled must be boolean");
    }
  }
  return true_v;
}

// =============================================================================
// Bundle dependency closures
// =============================================================================

vkr_internal bool8_t vkr_project_validate_resolved_dependencies(
    VkrProjectJob *job, const char *owner, const char *path,
    VkrBakeryJson *visited);

vkr_internal bool8_t vkr_project_bundle_dependency(
    VkrProjectJob *job, const char *owner, const char *path, const char *value,
    bool8_t explicit_relative, VkrBakeryJson *visited) {
  if (!value || !value[0] || strchr(value, '\\') || strchr(value, ':')) {
    return vkr_project_fail(job, "Invalid bundle dependency path");
  }
  char raw[VKR_PROJECT_PATH];
  (void)snprintf(raw, sizeof(raw), "%s", value);
  char *query = strchr(raw, '?');
  if (query) {
    *query = 0;
  }
  if (vkr_bakery_path_is_absolute(raw) ||
      (explicit_relative && strncmp(raw, "./", 2u) != 0)) {
    return vkr_project_fail(job, "Managed bundle dependency must be "
                                 "file-relative");
  }
  char directory[VKR_PROJECT_PATH];
  char joined[VKR_PROJECT_PATH];
  char target[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  (void)vkr_bakery_path_join(joined, sizeof(joined), directory, raw);
  if (!vkr_project_resolve(joined, true_v, target, sizeof(target))) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            joined);
  }
  if (!vkr_bakery_is_file(target) ||
      !vkr_project_is_relative_to(target, owner)) {
    return vkr_project_fail(job, "Bundle dependency is unavailable or escapes "
                                 "its owner");
  }
  return vkr_project_validate_resolved_dependencies(job, owner, target,
                                                    visited);
}

bool8_t vkr_project_validate_bundle_dependencies(VkrProjectJob *job,
                                                 const char *owner_value,
                                                 const char *path_value,
                                                 VkrBakeryJson *visited) {
  char path[VKR_PROJECT_PATH];
  char owner[VKR_PROJECT_PATH];
  if (!vkr_project_resolve(path_value, true_v, path, sizeof(path))) {
    return vkr_project_fail(job, "[Errno 2] No such file or directory: '%s'",
                            path_value);
  }
  (void)vkr_project_resolve(owner_value, false_v, owner, sizeof(owner));
  return vkr_project_validate_resolved_dependencies(job, owner, path, visited);
}

/* The closure below `path`, with `path` and `owner` already resolved: each
   dependency resolves once, rather than again as a path and again for the
   unchanged owner, which on Windows cost a handle open, final-path query
   and close each through every file-system filter. */
vkr_internal bool8_t vkr_project_validate_resolved_dependencies(
    VkrProjectJob *job, const char *owner, const char *path,
    VkrBakeryJson *visited) {
  Arena *arena = job->arena;
  if (vkr_bakery_json_get(visited, path)) {
    return true_v;
  }
  vkr_bakery_json_set(arena, visited, path,
                      vkr_bakery_json_bool(arena, true_v));
  if (!vkr_project_is_relative_to(path, owner)) {
    return vkr_project_fail(job, "Bundle dependency escapes its managed owner");
  }
  char suffix[16];
  vkr_project_suffix_lower(path, suffix, sizeof(suffix));
  if (strcmp(suffix, ".vkb") == 0) {
    char sidecar[VKR_PROJECT_PATH];
    char resolved_sidecar[VKR_PROJECT_PATH];
    (void)snprintf(sidecar, sizeof(sidecar), "%s.remap.json", path);
    if (!vkr_bakery_is_file(sidecar) ||
        !vkr_project_resolve(sidecar, false_v, resolved_sidecar,
                             sizeof(resolved_sidecar)) ||
        !vkr_project_is_relative_to(resolved_sidecar, owner)) {
      return vkr_project_fail(job, "Managed mesh has no contained immutable "
                                   "material map");
    }
    vkr_bakery_json_set(arena, visited, resolved_sidecar,
                        vkr_bakery_json_bool(arena, true_v));
    VkrBakeryJson *remap =
        vkr_project_load_json(job, sidecar, VKR_PROJECT_MAX_JSON_BYTES);
    VKR_PROJECT_TRY(remap);
    int64_t version = 0;
    const VkrBakeryJson *materials = vkr_bakery_json_get(remap, "materials");
    if (remap->count != 2u || !vkr_bakery_json_get(remap, "version") ||
        !materials ||
        !vkr_project_integer(vkr_bakery_json_get(remap, "version"), &version) ||
        version != 1 || materials->type != VKR_BAKERY_JSON_OBJECT) {
      return vkr_project_fail(job, "Invalid immutable material map");
    }
    VkrBakeryJson *info = vkr_project_inspect_mesh(job, path);
    VKR_PROJECT_TRY(info);
    const VkrBakeryJson *listed = vkr_bakery_json_get(info, "materials");
    bool8_t same = true_v;
    uint32_t distinct = 0u;
    VkrBakeryJson *names = vkr_bakery_json_object(arena);
    for (const VkrBakeryJson *item = listed ? listed->first : NULL; item;
         item = item->next) {
      if (item->type != VKR_BAKERY_JSON_STRING) {
        same = false_v;
        continue;
      }
      const char *name = (const char *)item->string.str;
      if (!vkr_bakery_json_get(names, name)) {
        vkr_bakery_json_set(arena, names, name,
                            vkr_bakery_json_bool(arena, true_v));
        distinct += 1u;
      }
      same = same && vkr_bakery_json_get(materials, name) != NULL;
    }
    if (!same || distinct != materials->count) {
      return vkr_project_fail(job, "Immutable material map does not match "
                                   "cooked dependency inventory");
    }
    for (const VkrBakeryJson *value = materials->first; value;
         value = value->next) {
      VKR_PROJECT_TRY(
          vkr_project_bundle_dependency(job, owner, path,
                                        value->type == VKR_BAKERY_JSON_STRING
                                            ? (const char *)value->string.str
                                            : NULL,
                                        true_v, visited));
    }
    return true_v;
  }
  const bool8_t material = strcmp(suffix, ".mt") == 0;
  const bool8_t font = strcmp(suffix, ".fontcfg") == 0;
  if (!material && !font) {
    return true_v;
  }
  VkrBakeryStat info;
  if (!vkr_bakery_stat(path, &info) || info.size > VKR_PROJECT_MAX_JSON_BYTES) {
    return vkr_project_fail(job, "Material or font configuration exceeds the "
                                 "parser limit");
  }
  const char *text = NULL;
  VKR_PROJECT_TRY(vkr_project_read_text(job, path, VKR_PROJECT_MAX_JSON_BYTES,
                                        &text, NULL));
  VkrProjectStrings lines;
  VKR_PROJECT_TRY(vkr_project_split_lines(job, text, &lines));
  VkrBakeryJson *keys = vkr_bakery_json_object(arena);
  bool8_t cooked_font = false_v;
  bool8_t font_file = false_v;
  for (uint32_t i = 0u; i < lines.count; ++i) {
    char key[256];
    char value[VKR_PROJECT_PATH];
    if (!vkr_project_partition_line(lines.items[i], key, sizeof(key), value,
                                    sizeof(value)) ||
        key[0] == '#') {
      continue;
    }
    const uint64_t key_length = strlen(key);
    if (material && key_length >= 8u &&
        strcmp(key + key_length - 8u, "_texture") == 0 && value[0]) {
      if (vkr_bakery_json_get(keys, key)) {
        return vkr_project_fail(job,
                                "Material contains a duplicate texture field");
      }
      vkr_bakery_json_set(arena, keys, key,
                          vkr_bakery_json_bool(arena, true_v));
      VKR_PROJECT_TRY(vkr_project_bundle_dependency(job, owner, path, value,
                                                    true_v, visited));
    } else if (font) {
      if (strcmp(key, "type") == 0) {
        cooked_font = strcmp(value, "cooked_mtsdf") == 0;
      } else if (strcmp(key, "file") == 0) {
        if (font_file) {
          return vkr_project_fail(
              job, "Font configuration contains duplicate files");
        }
        VKR_PROJECT_TRY(vkr_project_bundle_dependency(job, owner, path, value,
                                                      false_v, visited));
        font_file = true_v;
      }
    }
  }
  if (font && (!cooked_font || !font_file)) {
    return vkr_project_fail(job,
                            "Managed fonts require a cooked MTSDF artifact");
  }
  return true_v;
}

// =============================================================================
// Scene creation
// =============================================================================

bool8_t vkr_project_pending_bakes(const VkrBakeryJson *scene) {
  const VkrBakeryJson *recipes = vkr_bakery_json_get(scene, "bake_recipes");
  const VkrBakeryJson *prepare = vkr_bakery_json_get(recipes, "prepare_assets");
  return prepare && prepare->type == VKR_BAKERY_JSON_BOOL &&
         !prepare->boolean &&
         (vkr_project_truthy(vkr_bakery_json_get(recipes, "reflection")) ||
          vkr_project_truthy(vkr_bakery_json_get(recipes, "diffuse")));
}

bool8_t vkr_project_has_unbuilt(const VkrBakeryJson *assets) {
  for (const VkrBakeryJson *record = assets ? assets->first : NULL; record;
       record = record->next) {
    if (!vkr_project_truthy(vkr_bakery_json_get(record, "artifacts"))) {
      return true_v;
    }
  }
  return false_v;
}

VkrBakeryJson *vkr_project_create(VkrProjectJob *job) {
  Arena *arena = job->arena;
  if (vkr_project_exists(job->final_path)) {
    vkr_project_fail(job, "Scene identifier already exists");
    return NULL;
  }
  char staging[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  if (!vkr_project_make_dirs(job, staging) ||
      !vkr_project_mkdtemp(job, staging,
                           vkr_project_printf(job, "%s-", job->scene_id),
                           job->stage) ||
      !vkr_project_progress(job, "Preparing scene", 0.05, "")) {
    return NULL;
  }
  VkrBakeryJson *scene = NULL;
  const char *source_scene =
      vkr_project_json_text(job->request, "source_scene");
  if (source_scene && source_scene[0]) {
    scene = vkr_project_import_scene(job, source_scene);
    if (!scene) {
      return NULL;
    }
  } else {
    /* A new scene authors no sky: the project World's sky light, atmosphere
       and sun apply until the scene adds its own (ADR-076). */
    scene = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, scene, "version", vkr_bakery_json_int(arena, 3));
    vkr_bakery_json_set(arena, scene, "entities", vkr_bakery_json_array(arena));
    vkr_bakery_json_set(arena, scene, "reflection_probes",
                        vkr_bakery_json_array(arena));
  }
  int64_t first = 0;
  if (!vkr_project_append_entities(job, scene, &first)) {
    return NULL;
  }
  /* The physical sky supplies the sky, sun and global IBL; the environment
     block keeps only its sky-light scales. An imported scene keeps its
     authored sun, medium and cloud layer; a new physical sky starts with the
     default cloud layer. */
  const VkrBakeryJson *atmosphere =
      vkr_bakery_json_get(job->request, "atmosphere");
  if (vkr_project_truthy(vkr_bakery_json_get(atmosphere, "enabled"))) {
    const VkrBakeryJson *environment =
        vkr_bakery_json_get(job->request, "environment");
    const VkrBakeryJson *existing = vkr_bakery_json_get(scene, "atmosphere");
    VkrBakeryJson *merged = vkr_project_truthy(existing)
                                ? vkr_bakery_json_clone(arena, existing)
                                : vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, merged, "enabled",
                        vkr_bakery_json_bool(arena, true_v));
    vkr_bakery_json_set(arena, scene, "atmosphere", merged);
    if (!vkr_bakery_json_get(scene, "clouds")) {
      VkrBakeryJson *clouds = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, clouds, "enabled",
                          vkr_bakery_json_bool(arena, true_v));
      vkr_bakery_json_set(arena, scene, "clouds", clouds);
    }
    VkrBakeryJson *kept = vkr_bakery_json_object(arena);
    static const char *const keys[] = {
        "enabled", "intensity", "diffuse_intensity", "specular_intensity"};
    for (uint32_t i = 0u; i < ArrayCount(keys); ++i) {
      const VkrBakeryJson *value =
          vkr_project_truthy(environment)
              ? vkr_bakery_json_get(environment, keys[i])
              : NULL;
      if (value) {
        vkr_bakery_json_set(arena, kept, keys[i],
                            vkr_bakery_json_clone(arena, value));
      }
    }
    if (!vkr_bakery_json_get(kept, "enabled")) {
      vkr_bakery_json_set(arena, kept, "enabled",
                          vkr_bakery_json_bool(arena, true_v));
    }
    vkr_bakery_json_set(arena, scene, "environment", kept);
  }
  const VkrBakeryJson *requested_probes =
      vkr_bakery_json_get(job->request, "reflection_probes");
  if (requested_probes && requested_probes->type != VKR_BAKERY_JSON_NULL) {
    vkr_bakery_json_set(arena, scene, "reflection_probes",
                        vkr_bakery_json_clone(arena, requested_probes));
  }
  VkrBakeryJson *probes = vkr_bakery_json_get(scene, "reflection_probes");
  for (VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    const VkrBakeryJson *enabled = vkr_bakery_json_get(probe, "enabled");
    if ((enabled && !vkr_project_truthy(enabled)) ||
        vkr_project_truthy(vkr_bakery_json_get(probe, "asset"))) {
      continue;
    }
    char asset_id[37];
    vkr_project_uuid4(asset_id);
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, record, "id",
                        vkr_bakery_json_cstr(arena, asset_id));
    vkr_bakery_json_set(arena, record, "kind",
                        vkr_bakery_json_cstr(arena, "probe-cube"));
    vkr_bakery_json_set(arena, record, "name",
                        vkr_bakery_json_cstr(arena, "Reflection probe"));
    vkr_bakery_json_set(arena, record, "import_id",
                        vkr_bakery_json_cstr(arena, asset_id));
    vkr_bakery_json_set(arena, record, "source", vkr_bakery_json_null(arena));
    vkr_bakery_json_set(arena, record, "artifacts",
                        vkr_bakery_json_array(arena));
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "tool",
                        vkr_bakery_json_cstr(arena, "reflection"));
    vkr_bakery_json_set(arena, recipe, "version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_set(arena, record, "recipe", recipe);
    vkr_bakery_json_set(arena, record, "source_kind",
                        vkr_bakery_json_cstr(arena, "cubemap"));
    vkr_bakery_json_append(job->assets, record);
    vkr_bakery_json_set(
        arena, probe, "asset",
        vkr_project_reference(job, "scene", asset_id, "probe-cube"));
  }
  const char *font_source = vkr_project_json_text(job->request, "font_source");
  if (font_source && font_source[0]) {
    VkrBakeryJson *font = vkr_project_import_font(job, font_source);
    if (!font) {
      return NULL;
    }
    vkr_bakery_json_set(arena, scene, "default_font", font);
  }
  vkr_bakery_json_set(arena, scene, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_SCENE_VERSION));
  vkr_bakery_json_set(arena, scene, "id",
                      vkr_bakery_json_cstr(arena, job->scene_id));
  vkr_bakery_json_set(arena, scene, "assets", job->assets);
  if (!vkr_bakery_json_get(scene, "default_font")) {
    vkr_bakery_json_set(arena, scene, "default_font",
                        vkr_bakery_json_null(arena));
  }
  if (!vkr_bakery_json_get(scene, "edit_overlay")) {
    vkr_bakery_json_set(arena, scene, "edit_overlay",
                        vkr_bakery_json_null(arena));
  }
  if (!vkr_bakery_json_get(scene, "bake_recipes")) {
    vkr_bakery_json_set(arena, scene, "bake_recipes",
                        vkr_bakery_json_object(arena));
  }
  if (!vkr_project_validate_semantics(job, scene) ||
      !vkr_project_perform_bakes(job, scene, job->stage) ||
      !vkr_project_validate_managed_scene(job, scene, NULL, NULL, NULL) ||
      !vkr_project_progress(job, "Validating scene dependencies", 0.85, "") ||
      !vkr_project_validate_semantics(job, scene)) {
    return NULL;
  }
  const bool8_t unbuilt =
      vkr_project_has_unbuilt(job->assets) || vkr_project_pending_bakes(scene);
  if (!unbuilt && !vkr_project_lower(job, scene, job->stage, false_v)) {
    return NULL;
  }
  char scene_path[VKR_PROJECT_PATH];
  char parent[VKR_PROJECT_PATH];
  (void)snprintf(scene_path, sizeof(scene_path), "%s/scene.json", job->stage);
  vkr_bakery_path_parent(parent, sizeof(parent), job->final_path);
  if (!vkr_project_write_managed_scene(job, scene_path, scene) ||
      !vkr_project_make_dirs(job, parent)) {
    return NULL;
  }
  if (vkr_project_exists(job->final_path)) {
    vkr_project_fail(job, "Scene was created by another writer");
    return NULL;
  }
  if (!vkr_project_publish_directory(job, job->stage, job->final_path)) {
    return NULL;
  }
  job->final_owned = true_v;
  job->stage[0] = 0;
  if (!vkr_project_progress(job, "Opening scene", 0.95, "")) {
    return NULL;
  }
  char final_scene[VKR_PROJECT_PATH];
  (void)snprintf(final_scene, sizeof(final_scene), "%s/scene.json",
                 job->final_path);
  if (unbuilt) {
    VkrBakeryJson *result = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, result, "version",
                        vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
    vkr_bakery_json_set(arena, result, "status",
                        vkr_bakery_json_cstr(arena, "unbuilt"));
    vkr_bakery_json_set(arena, result, "scene_id",
                        vkr_bakery_json_cstr(arena, job->scene_id));
    vkr_bakery_json_set(arena, result, "scene_path",
                        vkr_bakery_json_cstr(arena, final_scene));
    vkr_bakery_json_set(arena, result, "fonts", vkr_bakery_json_array(arena));
    vkr_bakery_json_set(arena, result, "warnings", job->warnings);
    return result;
  }
  return vkr_project_prepare(job, final_scene);
}
