#include "vkr_project_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scene bakes: the authored-overlay runtime that bakes observe, and the
 * reflection-probe and diffuse-volume bakes run as `vkr_bakery bake`. */

vkr_internal VkrBakeryJson *vkr_project_edit_value(VkrProjectJob *job,
                                                   const VkrBakeryJson *edit,
                                                   const char *key) {
  const VkrBakeryJson *value = vkr_bakery_json_get(edit, key);
  return value ? vkr_bakery_json_clone(job->arena, value)
               : vkr_bakery_json_null(job->arena);
}

/* edit[key][0], or `fallback` when the key is absent. */
vkr_internal VkrBakeryJson *vkr_project_edit_first(VkrProjectJob *job,
                                                   const VkrBakeryJson *edit,
                                                   const char *key,
                                                   VkrBakeryJson *fallback) {
  const VkrBakeryJson *value = vkr_bakery_json_get(edit, key);
  if (!value) {
    return fallback;
  }
  const VkrBakeryJson *first = vkr_bakery_json_at(value, 0u);
  return first ? vkr_bakery_json_clone(job->arena, first)
               : vkr_bakery_json_null(job->arena);
}

vkr_internal void vkr_project_apply_wrapper(VkrProjectJob *job,
                                            VkrBakeryJson *entity,
                                            const VkrBakeryJson *edit) {
  Arena *arena = job->arena;
  int64_t fields = 0;
  (void)vkr_project_integer(vkr_bakery_json_get(edit, "fields"), &fields);
  if (fields & 1) {
    VkrBakeryJson *transform = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, transform, "pos",
                        vkr_project_edit_value(job, edit, "position"));
    vkr_bakery_json_set(arena, transform, "rot",
                        vkr_project_edit_value(job, edit, "rotation"));
    vkr_bakery_json_set(arena, transform, "scale",
                        vkr_project_edit_value(job, edit, "scale"));
    vkr_bakery_json_set(arena, entity, "transform", transform);
  }
  if (fields & 2) {
    vkr_bakery_json_set(arena, entity, "name",
                        vkr_project_edit_value(job, edit, "name"));
  }
  if (fields & 8) {
    const VkrBakeryJson *params = vkr_bakery_json_get(edit, "point_params");
    VkrBakeryJson *values[7];
    for (uint32_t i = 0u; i < ArrayCount(values); ++i) {
      const VkrBakeryJson *value = vkr_bakery_json_at(params, i);
      values[i] = value ? vkr_bakery_json_clone(arena, value)
                        : vkr_bakery_json_null(arena);
    }
    VkrBakeryJson *attenuation = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, attenuation, "constant", values[1]);
    vkr_bakery_json_set(arena, attenuation, "linear", values[2]);
    vkr_bakery_json_set(arena, attenuation, "quadratic", values[3]);
    VkrBakeryJson *light = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, light, "color",
                        vkr_project_edit_value(job, edit, "point_color"));
    vkr_bakery_json_set(arena, light, "direction_local",
                        vkr_project_edit_value(job, edit, "point_direction"));
    vkr_bakery_json_set(arena, light, "intensity", values[0]);
    vkr_bakery_json_set(arena, light, "attenuation", attenuation);
    vkr_bakery_json_set(arena, light, "range", values[4]);
    vkr_bakery_json_set(arena, light, "inner_cone_angle", values[5]);
    vkr_bakery_json_set(arena, light, "outer_cone_angle", values[6]);
    vkr_bakery_json_set(arena, light, "kind",
                        vkr_project_edit_value(job, edit, "point_kind"));
    vkr_bakery_json_set(arena, light, "enabled",
                        vkr_project_edit_value(job, edit, "point_enabled"));
    const VkrBakeryJson *shadow =
        vkr_bakery_json_get(edit, "point_casts_shadow");
    vkr_bakery_json_set(arena, light, "casts_shadow",
                        shadow ? vkr_bakery_json_clone(arena, shadow)
                               : vkr_bakery_json_bool(arena, false_v));
    vkr_bakery_json_set(arena, entity, "point_light", light);
  }
  if (fields & 16) {
    VkrBakeryJson *light = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, light, "color",
                        vkr_project_edit_value(job, edit, "directional_color"));
    vkr_bakery_json_set(
        arena, light, "direction_local",
        vkr_project_edit_value(job, edit, "directional_direction"));
    vkr_bakery_json_set(arena, light, "intensity",
                        vkr_project_edit_first(job, edit,
                                               "directional_intensity",
                                               vkr_bakery_json_null(arena)));
    vkr_bakery_json_set(
        arena, light, "enabled",
        vkr_project_edit_value(job, edit, "directional_enabled"));
    vkr_bakery_json_set(
        arena, light, "sun_angular_diameter_degrees",
        vkr_project_edit_first(job, edit,
                               "directional_sun_angular_diameter_degrees",
                               vkr_bakery_json_float(arena, 0.53)));
    vkr_bakery_json_set(
        arena, light, "temperature_kelvin",
        vkr_project_edit_first(job, edit, "directional_temperature_kelvin",
                               vkr_bakery_json_float(arena, 0.0)));
    const VkrBakeryJson *sun =
        vkr_bakery_json_get(edit, "directional_atmosphere_sun");
    vkr_bakery_json_set(arena, light, "atmosphere_sun",
                        sun ? vkr_bakery_json_clone(arena, sun)
                            : vkr_bakery_json_bool(arena, true_v));
    const VkrBakeryJson *moon =
        vkr_bakery_json_get(edit, "directional_atmosphere_moon");
    vkr_bakery_json_set(arena, light, "atmosphere_moon",
                        moon ? vkr_bakery_json_clone(arena, moon)
                             : vkr_bakery_json_bool(arena, false_v));
    vkr_bakery_json_set(arena, entity, "directional_light", light);
  }
  if (fields & 32) {
    VkrBakeryJson *light = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, light, "color",
                        vkr_project_edit_value(job, edit, "rectangle_color"));
    vkr_bakery_json_set(arena, light, "radiance",
                        vkr_project_edit_first(job, edit, "rectangle_radiance",
                                               vkr_bakery_json_null(arena)));
    vkr_bakery_json_set(arena, light, "size",
                        vkr_project_edit_value(job, edit, "rectangle_size"));
    vkr_bakery_json_set(arena, light, "enabled",
                        vkr_project_edit_value(job, edit, "rectangle_enabled"));
    vkr_bakery_json_set(arena, entity, "rectangle_light", light);
  }
}

// =============================================================================
// Effective bake runtime
// =============================================================================

/* Visibility state shared by the entity and source-node walks. */
typedef struct VkrProjectBakeEdits {
  VkrProjectJob *job;
  VkrBakeryJson *entities;
  VkrBakeryJson **wrapper_edits; /* Per entity, or NULL. */
  int8_t *visible;               /* -1 unknown, 0, 1. */
  bool8_t *visiting;
  uint32_t count;
} VkrProjectBakeEdits;

vkr_internal bool8_t vkr_project_entity_visible(VkrProjectBakeEdits *edits,
                                                uint32_t index, bool8_t *out) {
  VkrProjectJob *job = edits->job;
  if (edits->visible[index] >= 0) {
    *out = edits->visible[index] != 0;
    return true_v;
  }
  if (edits->visiting[index]) {
    return vkr_project_fail(job, "Entity parent cycle in bake input");
  }
  edits->visiting[index] = true_v;
  const VkrBakeryJson *edit = edits->wrapper_edits[index];
  const VkrBakeryJson *visible_value = vkr_bakery_json_get(edit, "visible");
  bool8_t local = visible_value ? vkr_project_truthy(visible_value) : true_v;
  const VkrBakeryJson *parent =
      vkr_bakery_json_get(vkr_bakery_json_at(edits->entities, index), "parent");
  const VkrBakeryJson *inherit = vkr_bakery_json_get(edit, "inherit");
  if (parent && parent->type != VKR_BAKERY_JSON_NULL &&
      (!inherit || vkr_project_truthy(inherit))) {
    int64_t parent_index = 0;
    if (!vkr_project_integer(parent, &parent_index) || parent_index < 0 ||
        parent_index >= (int64_t)edits->count) {
      return vkr_project_fail(job, "Invalid entity parent in bake input");
    }
    if (local) {
      bool8_t parent_visible = false_v;
      VKR_PROJECT_TRY(vkr_project_entity_visible(edits, (uint32_t)parent_index,
                                                 &parent_visible));
      local = parent_visible;
    }
  }
  edits->visible[index] = local ? 1 : 0;
  edits->visiting[index] = false_v;
  *out = local;
  return true_v;
}

/* Source-node edits and cooked node inventory of one mesh entity. */
typedef struct VkrProjectNodeWalk {
  VkrProjectJob *job;
  VkrBakeryJson *entities;
  uint32_t entity_index;
  bool8_t wrapper_visible;
  const VkrBakeryJson *info_nodes;
  VkrBakeryJson *node_edits;   /* node index text -> edit */
  VkrBakeryJson *node_visible; /* node index text -> bool */
  VkrBakeryJson *anchors;      /* node index text -> entity index */
} VkrProjectNodeWalk;

vkr_internal const VkrBakeryJson *
vkr_project_walk_node(const VkrProjectNodeWalk *walk, int64_t node) {
  for (const VkrBakeryJson *entry = walk->info_nodes ? walk->info_nodes->first
                                                     : NULL;
       entry; entry = entry->next) {
    int64_t index = 0;
    if (vkr_project_integer(vkr_bakery_json_get(entry, "index"), &index) &&
        index == node) {
      return entry;
    }
  }
  return NULL;
}

vkr_internal bool8_t vkr_project_node_visible(VkrProjectNodeWalk *walk,
                                              int64_t node, uint32_t depth,
                                              bool8_t *out) {
  VkrProjectJob *job = walk->job;
  const char *key = vkr_project_printf(job, "%lld", (long long)node);
  const VkrBakeryJson *cached = vkr_bakery_json_get(walk->node_visible, key);
  if (cached) {
    *out = cached->boolean;
    return true_v;
  }
  const VkrBakeryJson *entry = vkr_project_walk_node(walk, node);
  if (!entry || depth > walk->info_nodes->count) {
    return vkr_project_fail(job, "Source-node parent cycle");
  }
  const VkrBakeryJson *edit = vkr_bakery_json_get(walk->node_edits, key);
  int64_t parent = -1;
  (void)vkr_project_integer(vkr_bakery_json_get(entry, "parent"), &parent);
  bool8_t inherited = walk->wrapper_visible;
  if (parent != -1) {
    VKR_PROJECT_TRY(
        vkr_project_node_visible(walk, parent, depth + 1u, &inherited));
  }
  const VkrBakeryJson *visible_value = vkr_bakery_json_get(edit, "visible");
  const VkrBakeryJson *inherit = vkr_bakery_json_get(edit, "inherit");
  const bool8_t visible =
      (visible_value ? vkr_project_truthy(visible_value) : true_v) &&
      ((!inherit || vkr_project_truthy(inherit)) ? inherited : true_v);
  vkr_bakery_json_set(job->arena, walk->node_visible, key,
                      vkr_bakery_json_bool(job->arena, visible));
  *out = visible;
  return true_v;
}

vkr_internal bool8_t vkr_project_light_anchor(VkrProjectNodeWalk *walk,
                                              int64_t node, int64_t *out) {
  VkrProjectJob *job = walk->job;
  Arena *arena = job->arena;
  const char *key = vkr_project_printf(job, "%lld", (long long)node);
  const VkrBakeryJson *cached = vkr_bakery_json_get(walk->anchors, key);
  if (cached) {
    *out = cached->integer;
    return true_v;
  }
  const VkrBakeryJson *entry = vkr_project_walk_node(walk, node);
  if (!entry) {
    return vkr_project_fail(job, "Source-node parent cycle");
  }
  int64_t parent_node = -1;
  (void)vkr_project_integer(vkr_bakery_json_get(entry, "parent"), &parent_node);
  int64_t parent_index = walk->entity_index;
  if (parent_node != -1) {
    VKR_PROJECT_TRY(vkr_project_light_anchor(walk, parent_node, &parent_index));
  }
  VkrBakeryJson *anchor = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, anchor, "parent",
                      vkr_bakery_json_int(arena, parent_index));
  VkrBakeryJson *transform = vkr_bakery_json_object(arena);
  const VkrBakeryJson *matrix = vkr_bakery_json_get(entry, "matrix");
  vkr_bakery_json_set(arena, transform, "matrix",
                      matrix ? vkr_bakery_json_clone(arena, matrix)
                             : vkr_bakery_json_null(arena));
  vkr_bakery_json_set(arena, anchor, "transform", transform);
  const VkrBakeryJson *edit = vkr_bakery_json_get(walk->node_edits, key);
  if (vkr_project_truthy(edit)) {
    vkr_project_apply_wrapper(job, anchor, edit);
  }
  bool8_t visible = false_v;
  VKR_PROJECT_TRY(vkr_project_node_visible(walk, node, 0u, &visible));
  if (!visible) {
    vkr_bakery_json_remove(anchor, "point_light");
    vkr_bakery_json_remove(anchor, "directional_light");
    vkr_bakery_json_remove(anchor, "rectangle_light");
  }
  const int64_t index = walk->entities->count;
  vkr_bakery_json_set(arena, walk->anchors, key,
                      vkr_bakery_json_int(arena, index));
  vkr_bakery_json_append(walk->entities, anchor);
  *out = index;
  return true_v;
}

vkr_internal bool8_t vkr_project_bake_mesh_variant(VkrProjectJob *job,
                                                   VkrProjectNodeWalk *walk,
                                                   VkrBakeryJson *entity,
                                                   const char *bake_root) {
  Arena *arena = job->arena;
  const uint32_t index = walk->entity_index;
  const char *mesh = vkr_project_strdup(
      job, vkr_project_json_text(vkr_bakery_json_get(entity, "mesh"), "path"));
  const char *report =
      vkr_project_printf(job, "%s/%u.inspection.json", bake_root, index);
  const char *inspect[] = {"--inspect", "--input", mesh ? mesh : "", "--output",
                           report};
  VKR_PROJECT_TRY(
      vkr_project_run_tool(job, "mesh", inspect, ArrayCount(inspect),
                           "Resolving authored bake geometry", 0, NULL));
  VkrBakeryJson *info =
      vkr_project_load_json(job, report, VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(info);
  walk->info_nodes = vkr_bakery_json_get(info, "nodes");
  if (!walk->info_nodes) {
    return vkr_project_fail(job, "'nodes'");
  }
  if (!walk->info_nodes->count && !walk->wrapper_visible) {
    vkr_bakery_json_remove(entity, "mesh");
  }
  char seed[17];
  char identity[17];
  const char *fingerprint = vkr_project_json_text(info, "fingerprint");
  vkr_project_fingerprint((const uint8_t *)job->scene_id, strlen(job->scene_id),
                          seed);
  vkr_project_mesh_identity(seed, fingerprint ? fingerprint : "0", identity);
  for (const VkrBakeryJson *edit = walk->node_edits->first; edit;
       edit = edit->next) {
    const int64_t node = strtoll((const char *)edit->key.str, NULL, 10);
    if (!vkr_project_walk_node(walk, node) ||
        !vkr_bakery_json_is_string(
            vkr_bakery_json_get(edit, "source_fingerprint"), identity)) {
      return vkr_project_fail(job, "Source-node overrides conflict with the "
                                   "cooked model; resolve before baking");
    }
  }
  VkrBakeryJson *patches = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *entry = walk->info_nodes->first; entry;
       entry = entry->next) {
    int64_t node = 0;
    (void)vkr_project_integer(vkr_bakery_json_get(entry, "index"), &node);
    const VkrBakeryJson *edit = vkr_bakery_json_get(
        walk->node_edits, vkr_project_printf(job, "%lld", (long long)node));
    bool8_t visible = false_v;
    VKR_PROJECT_TRY(vkr_project_node_visible(walk, node, 0u, &visible));
    VkrBakeryJson *patch = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, patch, "index",
                        vkr_bakery_json_int(arena, node));
    vkr_bakery_json_set(arena, patch, "visible",
                        vkr_bakery_json_bool(arena, visible));
    int64_t fields = 0;
    (void)vkr_project_integer(vkr_bakery_json_get(edit, "fields"), &fields);
    if (fields & 1) {
      vkr_bakery_json_set(arena, patch, "position",
                          vkr_project_edit_value(job, edit, "position"));
      vkr_bakery_json_set(arena, patch, "rotation",
                          vkr_project_edit_value(job, edit, "rotation"));
      vkr_bakery_json_set(arena, patch, "scale",
                          vkr_project_edit_value(job, edit, "scale"));
    }
    if (fields & 56) {
      if (!vkr_project_truthy(vkr_bakery_json_get(entry, "in_scene"))) {
        return vkr_project_fail(
            job, "Light override refers to an inactive source node");
      }
      int64_t anchor = 0;
      VKR_PROJECT_TRY(vkr_project_light_anchor(walk, node, &anchor));
      VkrBakeryJson *punctual = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, punctual, "kind",
                          vkr_bakery_json_int(arena, 0));
      vkr_bakery_json_set(arena, patch, "punctual", punctual);
    }
    vkr_bakery_json_append(patches, patch);
  }
  const char *patch_path =
      vkr_project_printf(job, "%s/%u.patches.json", bake_root, index);
  VkrBakeryJson *patch_document = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, patch_document, "version",
                      vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, patch_document, "source_fingerprint",
                      vkr_project_edit_value(job, info, "fingerprint"));
  vkr_bakery_json_set(arena, patch_document, "nodes", patches);
  VKR_PROJECT_TRY(vkr_project_atomic_json(job, patch_path, patch_document));
  const char *variant = vkr_project_printf(job, "%s/%u.vkb", bake_root, index);
  const char *build[] = {"--input", mesh ? mesh : "",   "--output",
                         variant,   "--source-patches", patch_path};
  VKR_PROJECT_TRY(vkr_project_run_tool(job, "mesh", build, ArrayCount(build),
                                       "Preparing authored bake geometry", 0,
                                       NULL));
  VkrBakeryJson *remap = vkr_project_load_json(
      job, vkr_project_printf(job, "%s.remap.json", mesh ? mesh : ""),
      VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(remap);
  char mesh_directory[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(mesh_directory, sizeof(mesh_directory),
                         mesh ? mesh : "");
  VkrBakeryJson *materials = vkr_bakery_json_object(arena);
  const VkrBakeryJson *listed = vkr_bakery_json_get(remap, "materials");
  for (const VkrBakeryJson *entry = listed ? listed->first : NULL; entry;
       entry = entry->next) {
    char joined[VKR_PROJECT_PATH];
    char resolved[VKR_PROJECT_PATH];
    char relative[VKR_PROJECT_PATH];
    (void)vkr_bakery_path_join(
        joined, sizeof(joined), mesh_directory,
        vkr_project_json_text(listed, (const char *)entry->key.str));
    (void)vkr_project_resolve(joined, false_v, resolved, sizeof(resolved));
    if (!vkr_project_relpath(resolved, bake_root, relative, sizeof(relative))) {
      return vkr_project_fail(job, "Path too long");
    }
    vkr_bakery_json_set(
        arena, materials, (const char *)entry->key.str,
        vkr_bakery_json_cstr(arena, vkr_project_printf(job, "./%s", relative)));
  }
  vkr_bakery_json_set(arena, remap, "materials", materials);
  VKR_PROJECT_TRY(vkr_project_atomic_json(
      job, vkr_project_printf(job, "%s.remap.json", variant), remap));
  VkrBakeryJson *component = vkr_bakery_json_get(entity, "mesh");
  if (vkr_project_truthy(component)) {
    vkr_bakery_json_set(arena, component, "path",
                        vkr_bakery_json_cstr(arena, variant));
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_collect_bake_edits(
    VkrProjectJob *job, const VkrBakeryJson *journal, VkrBakeryJson *runtime,
    const char *root, VkrProjectBakeEdits *edits, VkrBakeryJson **node_edits) {
  Arena *arena = job->arena;
  char seed[17];
  vkr_project_fingerprint((const uint8_t *)job->scene_id, strlen(job->scene_id),
                          seed);
  const VkrBakeryJson *overrides = vkr_bakery_json_get(journal, "overrides");
  for (VkrBakeryJson *edit = overrides ? overrides->first : NULL; edit;
       edit = edit->next) {
    VkrProjectOverlayReference references[20];
    uint32_t count = 0u;
    VKR_PROJECT_TRY(vkr_project_overlay_references(
        job, edit, references, ArrayCount(references), &count));
    for (uint32_t r = 0u; r < count; ++r) {
      const VkrBakeryJson *value =
          vkr_bakery_json_get(references[r].reference, references[r].field);
      if (!references[r].required &&
          vkr_bakery_json_is_string(value, "0000000000000000")) {
        continue;
      }
      char expected[17];
      VKR_PROJECT_TRY(vkr_project_overlay_identity(
          job, references[r].reference, seed, runtime, root, expected));
      if (!vkr_bakery_json_is_string(value, expected)) {
        return vkr_project_fail(
            job, "Authored physics/source identity conflicts with scene");
      }
    }
    int64_t index = 0;
    int64_t node = 0;
    if (!vkr_project_integer(vkr_bakery_json_get(edit, "scene_entity"),
                             &index) ||
        index < 0 || index >= (int64_t)edits->count ||
        !vkr_project_integer(vkr_bakery_json_get(edit, "gltf_node"), &node) ||
        node < -1) {
      return vkr_project_fail(job,
                              "Authored override target is unavailable for "
                              "baking");
    }
    if (node == -1) {
      if (edits->wrapper_edits[index]) {
        return vkr_project_fail(job, "Duplicate authored override target");
      }
      edits->wrapper_edits[index] = edit;
    } else {
      if (!node_edits[index]) {
        node_edits[index] = vkr_bakery_json_object(arena);
      }
      const char *key = vkr_project_printf(job, "%lld", (long long)node);
      if (vkr_bakery_json_get(node_edits[index], key)) {
        return vkr_project_fail(job, "Duplicate authored override target");
      }
      vkr_bakery_json_set(arena, node_edits[index], key, edit);
    }
    char expected[17];
    (void)snprintf(expected, sizeof(expected), "%s", seed);
    const VkrBakeryJson *mesh = vkr_bakery_json_get(
        vkr_bakery_json_at(edits->entities, (uint32_t)index), "mesh");
    if (vkr_project_truthy(mesh)) {
      VkrBakeryJson *info =
          vkr_project_inspect_mesh(job, vkr_project_json_text(mesh, "path"));
      VKR_PROJECT_TRY(info);
      if (vkr_project_truthy(vkr_bakery_json_get(info, "nodes"))) {
        const char *fingerprint = vkr_project_json_text(info, "fingerprint");
        vkr_project_mesh_identity(seed, fingerprint ? fingerprint : "0",
                                  expected);
      }
    }
    if (!vkr_bakery_json_is_string(
            vkr_bakery_json_get(edit, "source_fingerprint"), expected)) {
      return vkr_project_fail(
          job, "Authored override source identity conflicts with scene");
    }
  }
  return true_v;
}

VkrBakeryJson *vkr_project_effective_bake_runtime(VkrProjectJob *job,
                                                  VkrBakeryJson *scene,
                                                  const char *root) {
  Arena *arena = job->arena;
  VkrBakeryJson *result = vkr_project_lower(job, scene, root, true_v);
  if (!result ||
      !vkr_project_truthy(vkr_bakery_json_get(scene, "edit_overlay"))) {
    return result;
  }
  char overlay[VKR_PROJECT_PATH];
  if (!vkr_project_contained(job, root,
                             vkr_project_json_text(scene, "edit_overlay"),
                             true_v, overlay)) {
    return NULL;
  }
  VkrBakeryJson *journal =
      vkr_project_load_json(job, overlay, VKR_PROJECT_MAX_JSON_BYTES);
  if (!journal || !vkr_project_checked_overlay(job, journal)) {
    return NULL;
  }
  VkrBakeryJson *runtime =
      vkr_project_load_json(job, vkr_project_json_text(result, "runtime_path"),
                            VKR_PROJECT_MAX_JSON_BYTES);
  if (!runtime) {
    return NULL;
  }
  VkrBakeryJson *entities = vkr_bakery_json_get(runtime, "entities");
  if (!entities) {
    entities = vkr_bakery_json_array(arena);
  }
  if (!vkr_project_remap_overlay_indices(job, journal, entities)) {
    return NULL;
  }
  const uint32_t count = entities->count;
  VkrProjectBakeEdits edits = {
      .job = job, .entities = entities, .count = count};
  const uint64_t slots = count ? count : 1u;
  edits.wrapper_edits = (VkrBakeryJson **)arena_alloc(
      arena, slots * sizeof(VkrBakeryJson *), ARENA_MEMORY_TAG_ARRAY);
  edits.visible = (int8_t *)arena_alloc(arena, slots, ARENA_MEMORY_TAG_ARRAY);
  edits.visiting = (bool8_t *)arena_alloc(arena, slots, ARENA_MEMORY_TAG_ARRAY);
  VkrBakeryJson **node_edits = (VkrBakeryJson **)arena_alloc(
      arena, slots * sizeof(VkrBakeryJson *), ARENA_MEMORY_TAG_ARRAY);
  if (!edits.wrapper_edits || !edits.visible || !edits.visiting ||
      !node_edits) {
    vkr_project_fail(job, "Out of memory");
    return NULL;
  }
  MemZero(edits.wrapper_edits, slots * sizeof(VkrBakeryJson *));
  MemSet(edits.visible, 0xFF, slots);
  MemZero(edits.visiting, slots);
  MemZero(node_edits, slots * sizeof(VkrBakeryJson *));
  if (!vkr_project_collect_bake_edits(job, journal, runtime, root, &edits,
                                      node_edits)) {
    return NULL;
  }
  char id[37];
  vkr_project_uuid4(id);
  char bake_root[VKR_PROJECT_PATH];
  (void)snprintf(bake_root, sizeof(bake_root), "%s/jobs/effective-%s",
                 job->workspace, id);
  if (!vkr_project_make_dirs(job, bake_root)) {
    return NULL;
  }
  /* Light anchors append entities; only the original entities are walked. */
  VkrBakeryJson *entity = entities->first;
  for (uint32_t index = 0u; index < count; ++index, entity = entity->next) {
    if (edits.wrapper_edits[index]) {
      vkr_project_apply_wrapper(job, entity, edits.wrapper_edits[index]);
    }
    bool8_t wrapper_visible = false_v;
    if (!vkr_project_entity_visible(&edits, index, &wrapper_visible)) {
      return NULL;
    }
    const bool8_t node_edited = node_edits[index] && node_edits[index]->count;
    if (vkr_project_truthy(vkr_bakery_json_get(entity, "mesh")) &&
        (node_edited || !wrapper_visible)) {
      VkrProjectNodeWalk walk = {
          .job = job,
          .entities = entities,
          .entity_index = index,
          .wrapper_visible = wrapper_visible,
          .node_edits = node_edits[index] ? node_edits[index]
                                          : vkr_bakery_json_object(arena),
          .node_visible = vkr_bakery_json_object(arena),
          .anchors = vkr_bakery_json_object(arena),
      };
      if (!vkr_project_bake_mesh_variant(job, &walk, entity, bake_root)) {
        return NULL;
      }
    }
    if (!wrapper_visible) {
      static const char *const hidden[] = {"shape", "text3d", "point_light",
                                           "directional_light",
                                           "rectangle_light"};
      for (uint32_t h = 0u; h < ArrayCount(hidden); ++h) {
        vkr_bakery_json_remove(entity, hidden[h]);
      }
    }
  }
  const char *runtime_path =
      vkr_project_printf(job, "%s/scene.json", bake_root);
  if (!vkr_project_atomic_json(job, runtime_path, runtime)) {
    return NULL;
  }
  vkr_bakery_json_set(arena, result, "runtime_path",
                      vkr_bakery_json_cstr(arena, runtime_path));
  return result;
}

// =============================================================================
// Probe and volume bakes
// =============================================================================

/* str(value) of a JSON scalar as Python prints it. */
vkr_internal const char *vkr_project_argument(VkrProjectJob *job,
                                              const VkrBakeryJson *value) {
  if (!value) {
    return "None";
  }
  switch (value->type) {
  case VKR_BAKERY_JSON_INT:
    return vkr_project_printf(job, "%lld", (long long)value->integer);
  case VKR_BAKERY_JSON_FLOAT: {
    char text[64];
    const uint32_t length =
        vkr_bakery_json_format_float(value->number, text, sizeof(text));
    return vkr_project_printf(job, "%.*s", (int)length, text);
  }
  case VKR_BAKERY_JSON_BOOL:
    return value->boolean ? "True" : "False";
  case VKR_BAKERY_JSON_STRING:
    return vkr_project_strdup(job, (const char *)value->string.str);
  default:
    return "None";
  }
}

typedef struct VkrProjectArguments {
  const char *items[96];
  uint32_t count;
} VkrProjectArguments;

vkr_internal bool8_t vkr_project_argument_push(VkrProjectJob *job,
                                               VkrProjectArguments *arguments,
                                               const char *value) {
  if (arguments->count == ArrayCount(arguments->items)) {
    return vkr_project_fail(job, "Too many bake arguments");
  }
  arguments->items[arguments->count++] = value;
  return true_v;
}

vkr_internal bool8_t vkr_project_bake_probes(VkrProjectJob *job,
                                             VkrBakeryJson *scene,
                                             const char *asset_root) {
  Arena *arena = job->arena;
  const VkrBakeryJson *tools = vkr_bakery_json_get(job->request, "tools");
  const char *harness = vkr_project_json_text(tools, "harness");
  if (!harness || !harness[0]) {
    return vkr_project_fail(job, "Reflection baking requires the installed "
                                 "harness and HDR cubemap packer");
  }
  VkrBakeryJson *capture_scene = vkr_bakery_json_clone(arena, scene);
  vkr_bakery_json_set(arena, capture_scene, "reflection_probes",
                      vkr_bakery_json_array(arena));
  VkrBakeryJson *runtime =
      vkr_project_effective_bake_runtime(job, capture_scene, asset_root);
  VKR_PROJECT_TRY(runtime);
  const char *runtime_path = vkr_project_json_text(runtime, "runtime_path");
  VkrBakeryJson *probes = vkr_bakery_json_get(scene, "reflection_probes");
  for (VkrBakeryJson *probe = probes ? probes->first : NULL; probe;
       probe = probe->next) {
    const VkrBakeryJson *enabled = vkr_bakery_json_get(probe, "enabled");
    if (enabled && !vkr_project_truthy(enabled)) {
      continue;
    }
    const VkrBakeryJson *reference = vkr_bakery_json_get(probe, "asset");
    if (!vkr_project_truthy(reference) ||
        !vkr_bakery_json_is_string(vkr_bakery_json_get(reference, "scope"),
                                   "scene")) {
      return vkr_project_fail(
          job, "Probe capture requires a scene-owned destination");
    }
    VkrBakeryJson *record =
        vkr_project_record_by_id(vkr_bakery_json_get(scene, "assets"),
                                 vkr_project_json_text(reference, "id"));
    if (!record) {
      return vkr_project_fail(job, "Probe capture destination is missing");
    }
    char revision[37];
    vkr_project_uuid4(revision);
    char directory[VKR_PROJECT_PATH];
    (void)snprintf(directory, sizeof(directory), "%s/builds/%s", asset_root,
                   revision);
    if (vkr_project_exists(directory)) {
      return vkr_project_fail(job, "[Errno 17] File exists: '%s'", directory);
    }
    VKR_PROJECT_TRY(vkr_project_make_dirs(job, directory));
    const char *destination =
        vkr_project_printf(job, "%s/probe.vkt", directory);
    VkrProjectArguments arguments = {0};
    const char *fixed[] = {"bake",         "probe",     "--workspace-root",
                           job->workspace, "--scene",   runtime_path,
                           "--output",     destination, "--position"};
    for (uint32_t i = 0u; i < ArrayCount(fixed); ++i) {
      VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments, fixed[i]));
    }
    const VkrBakeryJson *center = vkr_bakery_json_get(probe, "center");
    for (const VkrBakeryJson *value = center ? center->first : NULL; value;
         value = value->next) {
      VKR_PROJECT_TRY(vkr_project_argument_push(
          job, &arguments, vkr_project_argument(job, value)));
    }
    const VkrBakeryJson *size = vkr_bakery_json_get(probe, "resolution");
    const VkrBakeryJson *near_plane = vkr_bakery_json_get(probe, "near_plane");
    const VkrBakeryJson *far_plane = vkr_bakery_json_get(probe, "far_plane");
    const char *tail[] = {
        "--size",
        size ? vkr_project_argument(job, size) : "256",
        "--near-plane",
        near_plane ? vkr_project_argument(job, near_plane) : "0.1",
        "--far-plane",
        far_plane ? vkr_project_argument(job, far_plane) : "1000",
        "--harness",
        harness};
    for (uint32_t i = 0u; i < ArrayCount(tail); ++i) {
      VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments, tail[i]));
    }
    const char *profile = vkr_project_json_text(tools, "profile");
    if (profile && profile[0]) {
      VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments, "--profile"));
      VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments, profile));
    }
    VKR_PROJECT_TRY(
        vkr_project_run_bakery(job, arguments.items, arguments.count,
                               "Baking reflection probe", "probe", 0, NULL));
    if (!vkr_bakery_is_file(destination)) {
      return vkr_project_fail(job,
                              "Reflection baker did not publish a cubemap");
    }
    const char *path_reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    VKR_PROJECT_TRY(vkr_project_managed_reference(job, destination, asset_root,
                                                  &path_reference));
    VKR_PROJECT_TRY(vkr_project_digest(job, destination, hash));
    VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
    VkrBakeryJson *product = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, product, "role",
                        vkr_bakery_json_cstr(arena, "probe-cube"));
    vkr_bakery_json_set(arena, product, "path",
                        vkr_bakery_json_cstr(arena, path_reference));
    vkr_bakery_json_set(arena, product, "version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_append(artifacts, product);
    vkr_bakery_json_set(arena, record, "artifacts", artifacts);
    vkr_bakery_json_set(arena, record, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
    vkr_bakery_json_remove(record, "closure");
  }
  return true_v;
}

vkr_internal bool8_t vkr_project_bake_volume(VkrProjectJob *job,
                                             VkrBakeryJson *scene,
                                             const char *asset_root) {
  Arena *arena = job->arena;
  VkrBakeryJson *bake_scene = vkr_bakery_json_clone(arena, scene);
  vkr_bakery_json_remove(bake_scene, "diffuse_volume");
  VkrBakeryJson *runtime =
      vkr_project_effective_bake_runtime(job, bake_scene, asset_root);
  VKR_PROJECT_TRY(runtime);
  char revision[37];
  vkr_project_uuid4(revision);
  char directory[VKR_PROJECT_PATH];
  (void)snprintf(directory, sizeof(directory), "%s/builds/%s", asset_root,
                 revision);
  if (vkr_project_exists(directory)) {
    return vkr_project_fail(job, "[Errno 17] File exists: '%s'", directory);
  }
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, directory));
  const char *destination =
      vkr_project_printf(job, "%s/volume.vkdv", directory);
  VkrProjectArguments arguments = {0};
  const char *fixed[] = {"bake",
                         "diffuse",
                         "--workspace-root",
                         job->workspace,
                         "--scene",
                         vkr_project_json_text(runtime, "runtime_path"),
                         "--output",
                         destination};
  for (uint32_t i = 0u; i < ArrayCount(fixed); ++i) {
    VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments, fixed[i]));
  }
  const VkrBakeryJson *requested =
      vkr_bakery_json_get(job->request, "diffuse_settings");
  VkrBakeryJson *settings = requested ? vkr_bakery_json_clone(arena, requested)
                                      : vkr_bakery_json_object(arena);
  static const char *const lists[] = {"bounds", "grid"};
  for (uint32_t l = 0u; l < ArrayCount(lists); ++l) {
    const VkrBakeryJson *values = vkr_bakery_json_get(settings, lists[l]);
    if (!vkr_project_truthy(values)) {
      continue;
    }
    VKR_PROJECT_TRY(vkr_project_argument_push(
        job, &arguments, vkr_project_printf(job, "--%s", lists[l])));
    for (const VkrBakeryJson *value = values->first; value;
         value = value->next) {
      VKR_PROJECT_TRY(vkr_project_argument_push(
          job, &arguments, vkr_project_argument(job, value)));
    }
  }
  static const char *const scalars[] = {"voxel_size",   "face_size", "samples",
                                        "max_depth",    "seed",      "photons",
                                        "photon_radius"};
  for (uint32_t s = 0u; s < ArrayCount(scalars); ++s) {
    const VkrBakeryJson *value = vkr_bakery_json_get(settings, scalars[s]);
    if (!value) {
      continue;
    }
    char flag[64];
    (void)snprintf(flag, sizeof(flag), "--%s", scalars[s]);
    for (char *c = flag + 2; *c; ++c) {
      if (*c == '_') {
        *c = '-';
      }
    }
    VKR_PROJECT_TRY(vkr_project_argument_push(job, &arguments,
                                              vkr_project_strdup(job, flag)));
    VKR_PROJECT_TRY(vkr_project_argument_push(
        job, &arguments, vkr_project_argument(job, value)));
  }
  int32_t code = 0;
  VKR_PROJECT_TRY(vkr_project_run_bakery(
      job, arguments.items, arguments.count, "Baking diffuse volume", "diffuse",
      VKR_PROJECT_DIFFUSE_NO_ROOM_CELLS, &code));
  const VkrBakeryJson *previous_reference = vkr_bakery_json_get(
      vkr_bakery_json_get(scene, "diffuse_volume"), "asset");
  VkrBakeryJson *previous = vkr_project_record_by_id(
      job->assets, vkr_project_json_text(previous_reference, "id"));
  if (code == VKR_PROJECT_DIFFUSE_NO_ROOM_CELLS) {
    /* An all-invalid volume renders like no volume (ADR-054). A previous
       volume describes other geometry, so it is dropped. */
    (void)vkr_project_remove_tree(directory);
    if (previous) {
      vkr_project_remove_record(job->assets, previous);
    }
    vkr_bakery_json_remove(scene, "diffuse_volume");
    const char *warning =
        "Diffuse volume skipped: no interpolation cell lies inside a closed "
        "room, so the scene keeps environment and reflection-probe diffuse "
        "lighting";
    vkr_bakery_json_append(job->warnings, vkr_bakery_json_cstr(arena, warning));
    printf("Warning: %s\n", warning);
    fflush(stdout);
  } else {
    if (!vkr_bakery_is_file(destination)) {
      return vkr_project_fail(job,
                              "Diffuse baker did not publish its artifact");
    }
    const char *path_reference = NULL;
    char hash[VKR_BAKERY_SHA256_HEX];
    VKR_PROJECT_TRY(vkr_project_managed_reference(job, destination, asset_root,
                                                  &path_reference));
    VKR_PROJECT_TRY(vkr_project_digest(job, destination, hash));
    char id[37];
    vkr_project_uuid4(id);
    VkrBakeryJson *record = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, record, "id", vkr_bakery_json_cstr(arena, id));
    vkr_bakery_json_set(arena, record, "kind",
                        vkr_bakery_json_cstr(arena, "volume"));
    vkr_bakery_json_set(arena, record, "name",
                        vkr_bakery_json_cstr(arena, "Diffuse volume"));
    vkr_bakery_json_set(arena, record, "import_id",
                        vkr_bakery_json_cstr(arena, revision));
    vkr_bakery_json_set(arena, record, "source", vkr_bakery_json_null(arena));
    VkrBakeryJson *artifacts = vkr_bakery_json_array(arena);
    VkrBakeryJson *product = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, product, "role",
                        vkr_bakery_json_cstr(arena, "volume"));
    vkr_bakery_json_set(arena, product, "path",
                        vkr_bakery_json_cstr(arena, path_reference));
    vkr_bakery_json_set(arena, product, "version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_append(artifacts, product);
    vkr_bakery_json_set(arena, record, "artifacts", artifacts);
    vkr_bakery_json_set(arena, record, "fingerprint",
                        vkr_bakery_json_cstr(
                            arena, vkr_project_printf(job, "sha256:%s", hash)));
    VkrBakeryJson *recipe = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, recipe, "tool",
                        vkr_bakery_json_cstr(arena, "diffuse"));
    vkr_bakery_json_set(arena, recipe, "version",
                        vkr_bakery_json_int(arena, 1));
    vkr_bakery_json_set(arena, recipe, "settings", settings);
    vkr_bakery_json_set(arena, record, "recipe", recipe);
    if (previous) {
      vkr_bakery_json_set(
          arena, record, "id",
          vkr_bakery_json_clone(arena, vkr_bakery_json_get(previous, "id")));
      vkr_project_remove_record(job->assets, previous);
    }
    vkr_bakery_json_append(job->assets, record);
    VkrBakeryJson *volume = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(
        arena, volume, "asset",
        vkr_project_reference(job, "scene", vkr_project_json_text(record, "id"),
                              "volume"));
    vkr_bakery_json_set(arena, scene, "diffuse_volume", volume);
  }
  vkr_bakery_json_set(arena, scene, "assets", job->assets);
  return true_v;
}

bool8_t vkr_project_perform_bakes(VkrProjectJob *job, VkrBakeryJson *scene,
                                  const char *asset_root) {
  Arena *arena = job->arena;
  const VkrBakeryJson *bakes = vkr_bakery_json_get(job->request, "bakes");
  if (!vkr_project_truthy(bakes)) {
    bakes = vkr_bakery_json_get(scene, "bake_recipes");
  }
  VkrBakeryJson *recipes = vkr_bakery_json_get(scene, "bake_recipes");
  if (!recipes || recipes->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_project_fail(job, "'bake_recipes'");
  }
  if (vkr_project_truthy(bakes) && bakes != recipes) {
    for (const VkrBakeryJson *field = bakes->first; field;
         field = field->next) {
      vkr_bakery_json_set(arena, recipes, (const char *)field->key.str,
                          vkr_bakery_json_clone(arena, field));
    }
  }
  const VkrBakeryJson *prepare = vkr_bakery_json_get(bakes, "prepare_assets");
  if (prepare && prepare->type == VKR_BAKERY_JSON_BOOL && !prepare->boolean) {
    return true_v;
  }
  if (!asset_root || !asset_root[0]) {
    asset_root = job->stage;
  }
  if (vkr_project_truthy(vkr_bakery_json_get(bakes, "reflection"))) {
    VKR_PROJECT_TRY(vkr_project_bake_probes(job, scene, asset_root));
  }
  if (vkr_project_truthy(vkr_bakery_json_get(bakes, "diffuse"))) {
    VKR_PROJECT_TRY(vkr_project_bake_volume(job, scene, asset_root));
  }
  return true_v;
}
