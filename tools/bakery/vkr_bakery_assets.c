#include "filesystem/filesystem.h"
#include "vkr_bakery_internal.h"

#include "vkr_bakery_identity.h"
#include "vkr_vkt_packer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Asset producers wrapping the cooker libraries. Each runs its legacy
 * command line as `vkr_bakery tool <name>` so cooker globals, arenas and
 * crashes stay isolated from the scheduler (spawn cost is negligible next to
 * every cook; see the task note's measurements). */

// =============================================================================
// Shared helpers
// =============================================================================

/* Output declared by the caller, or the producer's default path. */
vkr_internal bool8_t vkr_bakery_default_output(VkrBakeryGraph *graph,
                                               VkrBakeryAction *action,
                                               const char *role,
                                               const char *default_path) {
  return vkr_bakery_action_output(
      graph, action, role,
      action->requested_output ? action->requested_output : default_path);
}

/* Path of `source` relative to the root when inside it, so cookers record
 * the same repository-relative references the shell wrappers produced. */
vkr_internal const char *vkr_bakery_tool_path(const VkrBakeryTask *task,
                                              const char *path) {
  const VkrBakeryAction *action = task->action;
  if (path == action->source && action->display &&
      !vkr_bakery_path_is_absolute(action->display)) {
    return action->display;
  }
  return path;
}

/* Seeds a staged output with the current destination so tools that verify
 * an existing artifact (texture packer, font cooker) skip unchanged work. */
vkr_internal void vkr_bakery_seed(const VkrBakeryTask *task,
                                  const char *staged) {
  const VkrBakeryAction *action = task->action;
  if (action->force) {
    return;
  }
  if (action->output_count && vkr_bakery_is_file(action->outputs[0].path)) {
    (void)vkr_bakery_clone_or_copy(action->outputs[0].path, staged);
  } else if (action->seed_path && vkr_bakery_is_file(action->seed_path)) {
    (void)vkr_bakery_clone_or_copy(action->seed_path, staged);
  }
}

vkr_internal bool8_t vkr_bakery_tool_failed(VkrBakeryTask *task,
                                            VkrBakeryDiag diag,
                                            int32_t exit_code) {
  if (vkr_bakery_task_cancelled(task)) {
    return true_v;
  }
  char message[1024];
  (void)snprintf(message, sizeof(message), "exit %d: %s", exit_code,
                 vkr_bakery_task_output_tail(task, 700u));
  vkr_bakery_task_diag(task, diag, task->action->display, 0u, 0u, message,
                       NULL);
  vkr_bakery_task_fail(task, "%s tool exited with %d",
                       task->action->producer->id, exit_code);
  return true_v;
}

vkr_internal void vkr_bakery_stem_path(char *out, uint32_t capacity,
                                       const char *source,
                                       const char *extension) {
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), source);
  const char *name = vkr_bakery_path_name(source);
  const char *dot = strrchr(name, '.');
  const int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
  (void)snprintf(out, capacity, "%s%s%.*s%s", directory,
                 directory[0] ? "/" : "", stem, name, extension);
}

vkr_internal void vkr_bakery_percent_decode(char *text) {
  char *out = text;
  for (const char *in = text; *in; ++in) {
    if (in[0] == '%' && in[1] && in[2]) {
      char hex[3] = {in[1], in[2], 0};
      char *end = NULL;
      const long value = strtol(hex, &end, 16);
      if (end == hex + 2) {
        *out++ = (char)value;
        in += 2;
        continue;
      }
    }
    *out++ = *in;
  }
  *out = 0;
}

/* Adds external buffer and image URIs of a .gltf or .glb as inputs. */
vkr_internal bool8_t vkr_bakery_gltf_inputs(VkrBakeryGraph *graph,
                                            VkrBakeryAction *action) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(action->source, GB(2), &data, &length)) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_UNREADABLE_SOURCE,
                         "cannot read the model");
    return false_v;
  }
  const uint8_t *json = data;
  uint64_t json_length = length;
  if (length >= 20u && MemCompare(data, "glTF", 4u) == 0) {
    uint32_t chunk_length = 0u;
    MemCopy(&chunk_length, data + 12u, 4u);
    if (20u + (uint64_t)chunk_length > length ||
        MemCompare(data + 16u, "JSON", 4u) != 0) {
      free(data);
      vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_MESH_FAILED,
                           "malformed GLB header");
      return false_v;
    }
    json = data + 20u;
    json_length = chunk_length;
  }
  const uint64_t mark = arena_pos(graph->arena);
  VkrBakeryJsonError error;
  VkrBakeryJson *root =
      vkr_bakery_json_parse(graph->arena, json, json_length, 128u, &error);
  free(data);
  if (!root) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_MESH_FAILED,
                         "invalid glTF JSON at %u:%u: %s", error.line,
                         error.column, error.message);
    return false_v;
  }
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), action->source);
  static const char *const arrays[] = {"buffers", "images"};
  bool8_t ok = true_v;
  for (uint32_t a = 0u; a < ArrayCount(arrays); ++a) {
    const VkrBakeryJson *items = vkr_bakery_json_get(root, arrays[a]);
    for (const VkrBakeryJson *item = items ? items->first : NULL; item;
         item = item->next) {
      String8 uri = {0};
      if (!vkr_bakery_json_get_string(item, "uri", &uri) ||
          (uri.length >= 5u && MemCompare(uri.str, "data:", 5u) == 0)) {
        continue;
      }
      char relative[VKR_BAKERY_PATH_CAPACITY];
      (void)snprintf(relative, sizeof(relative), "%.*s", (int)uri.length,
                     uri.str);
      vkr_bakery_percent_decode(relative);
      char path[VKR_BAKERY_PATH_CAPACITY];
      if (!vkr_bakery_path_join(path, sizeof(path), directory, relative)) {
        continue;
      }
      if (!vkr_bakery_is_file(path)) {
        vkr_bakery_plan_diag(graph, action,
                             VKR_BAKERY_DIAG_MESH_MISSING_REFERENCE,
                             "references missing file '%s'", relative);
        ok = false_v;
        continue;
      }
      ok = vkr_bakery_action_input(graph, action, path, relative) && ok;
    }
  }
  (void)mark;
  return ok;
}

vkr_internal bool8_t vkr_bakery_require_source(VkrBakeryGraph *graph,
                                               VkrBakeryAction *action) {
  if (!action->source || !vkr_bakery_is_file(action->source)) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                         "source does not exist");
    return false_v;
  }
  return vkr_bakery_action_input(graph, action, action->source,
                                 vkr_bakery_path_name(action->source));
}

// =============================================================================
// texture
// =============================================================================

vkr_internal const char *const vkr_bakery_texture_fields[] = {
    "class",        "shape",        "layers",   "tier",
    "alpha_cutoff", "alpha_factor", "encoding", NULL};

/* Reads width and height from PNG, JPEG, BMP or TGA headers. */
vkr_internal bool8_t vkr_bakery_image_extent(const char *path, uint32_t *width,
                                             uint32_t *height) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return false_v;
  }
  uint8_t header[64 * 1024];
  const size_t length = fread(header, 1u, sizeof(header), file);
  fclose(file);
  if (length >= 24u && MemCompare(header, "\x89PNG", 4u) == 0) {
    *width = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
             ((uint32_t)header[18] << 8) | header[19];
    *height = ((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) |
              ((uint32_t)header[22] << 8) | header[23];
    return true_v;
  }
  if (length >= 4u && header[0] == 0xFF && header[1] == 0xD8) {
    size_t i = 2u;
    while (i + 9u < length) {
      if (header[i] != 0xFF) {
        i += 1u;
        continue;
      }
      const uint8_t marker = header[i + 1u];
      const size_t segment = ((size_t)header[i + 2u] << 8) | header[i + 3u];
      if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
          marker != 0xC8 && marker != 0xCC) {
        *height = ((uint32_t)header[i + 5u] << 8) | header[i + 6u];
        *width = ((uint32_t)header[i + 7u] << 8) | header[i + 8u];
        return true_v;
      }
      i += 2u + segment;
    }
    return false_v;
  }
  if (length >= 26u && header[0] == 'B' && header[1] == 'M') {
    int32_t w = 0;
    int32_t h = 0;
    MemCopy(&w, header + 18u, 4u);
    MemCopy(&h, header + 22u, 4u);
    *width = (uint32_t)(w < 0 ? -w : w);
    *height = (uint32_t)(h < 0 ? -h : h);
    return true_v;
  }
  if (length >= 18u) {
    *width = (uint32_t)header[12] | ((uint32_t)header[13] << 8);
    *height = (uint32_t)header[14] | ((uint32_t)header[15] << 8);
    return *width > 0u && *height > 0u;
  }
  return false_v;
}

vkr_internal bool8_t vkr_bakery_texture_plan(VkrBakeryGraph *graph,
                                             VkrBakeryAction *action) {
  if (!vkr_bakery_require_source(graph, action)) {
    return false_v;
  }
  Arena *arena = graph->arena;
  VkrBakeryJson *recipe = action->recipe;
  const char *texture_class = vkr_bakery_recipe_string(
      recipe, "class", vkr_vkt_infer_texture_class(action->source));
  static const char *const classes[] = {"color-srgb", "color-linear",
                                        "normal-rg", "data-mask"};
  bool8_t class_ok = false_v;
  for (uint32_t i = 0u; i < ArrayCount(classes); ++i) {
    class_ok = class_ok || strcmp(texture_class, classes[i]) == 0;
  }
  const char *shape = vkr_bakery_recipe_string(recipe, "shape", "2d");
  const char *tier = vkr_bakery_recipe_string(recipe, "tier", "final");
  const bool8_t preview = strcmp(tier, "preview") == 0;
  /* The host's encoding unless the recipe names one; the resolved name joins
     the key, so each host caches its own blocks. */
  const char *encoding = vkr_bakery_recipe_string(
      recipe, "encoding",
      vkr_vkt_host_encoding() == VKR_VKT_ENCODING_BC ? "bc" : "astc");
  VkrVktEncoding encoding_value = vkr_vkt_host_encoding();
  const bool8_t encoding_ok = vkr_vkt_parse_encoding(encoding, &encoding_value);
  if (!class_ok || (!preview && strcmp(tier, "final") != 0) || !encoding_ok ||
      (strcmp(shape, "2d") && strcmp(shape, "2d-array") &&
       strcmp(shape, "cube") && strcmp(shape, "cube-array"))) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                         "class must be color-srgb|color-linear|normal-rg|"
                         "data-mask, shape 2d|2d-array|cube|cube-array, tier "
                         "final|preview, encoding astc|astc-fast|bc|bc-fast "
                         "(astc-fast needs Apple's system encoder, bc an "
                         "x86-64 build)");
    return false_v;
  }
  /* Native block encodes are fast enough that decoding, mips and writing
     outweigh the threaded encode: one Bistro preview import's 185 encodes
     took 83 s of CPU in 70 s of summed wall time. Texture actions share the
     cores, bounded by workers and the memory budget, instead of taking a
     machine slot. */
  action->exclusive_cores = false_v;
  /* Resolved values join the key so class inference is explicit. */
  vkr_bakery_json_set(arena, recipe, "class",
                      vkr_bakery_json_cstr(arena, texture_class));
  vkr_bakery_json_set(arena, recipe, "shape",
                      vkr_bakery_json_cstr(arena, shape));
  vkr_bakery_json_set(arena, recipe, "tier", vkr_bakery_json_cstr(arena, tier));
  vkr_bakery_json_set(arena, recipe, "encoding",
                      vkr_bakery_json_cstr(arena, encoding));
  const VkrBakeryJson *layers = vkr_bakery_json_get(recipe, "layers");
  if (layers) {
    char directory[VKR_BAKERY_PATH_CAPACITY];
    vkr_bakery_path_parent(directory, sizeof(directory), action->source);
    uint32_t index = 0u;
    for (const VkrBakeryJson *layer = layers->first; layer;
         layer = layer->next, ++index) {
      char path[VKR_BAKERY_PATH_CAPACITY];
      if (layer->type != VKR_BAKERY_JSON_STRING ||
          !vkr_bakery_path_join(path, sizeof(path), directory,
                                (const char *)layer->string.str) ||
          !vkr_bakery_is_file(path)) {
        vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                             "layer %u is missing", index);
        return false_v;
      }
      if (!vkr_bakery_action_input(
              graph, action, path,
              vkr_bakery_graph_printf(graph, "layer%03u:%s", index,
                                      vkr_bakery_path_name(path)))) {
        return false_v;
      }
    }
  }
  char default_output[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(default_output, sizeof(default_output), "%s.vkt",
                 action->source);
  vkr_bakery_action_label(graph, action, "%s %s %s", encoding, texture_class,
                          shape);
  return vkr_bakery_default_output(graph, action, "vkt", default_output);
}

vkr_internal uint64_t
vkr_bakery_texture_estimate(const VkrBakeryAction *action) {
  /* Measured on the former UASTC encoder: a 4096x4096 encode peaked at
     752 MB, 45 bytes/pixel. The native encoders stay below that bound. */
  uint64_t pixels = 0u;
  for (uint32_t i = 0u; i < action->input_count; ++i) {
    uint32_t width = 0u;
    uint32_t height = 0u;
    if (action->inputs[i].path &&
        vkr_bakery_image_extent(action->inputs[i].path, &width, &height)) {
      pixels += (uint64_t)width * height;
    }
  }
  return 24u + pixels * 48u / (1024u * 1024u);
}

vkr_internal bool8_t vkr_bakery_texture_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const VkrBakeryJson *recipe = action->recipe;
  const char *staged = vkr_bakery_task_stage(task, "texture.vkt");
  vkr_bakery_seed(task, staged);
  const char *arguments[48];
  uint32_t count = 0u;
  arguments[count++] = "--output";
  arguments[count++] = staged;
  arguments[count++] = "--type";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "shape", "2d");
  const VkrBakeryJson *layers = vkr_bakery_json_get(recipe, "layers");
  if (layers) {
    for (uint32_t i = 0u; i < action->input_count && count + 12u < 48u; ++i) {
      if (strncmp(action->inputs[i].name, "layer", 5u) == 0) {
        arguments[count++] = "--layer";
        arguments[count++] = action->inputs[i].path;
      }
    }
  } else {
    arguments[count++] = "--layer";
    arguments[count++] = vkr_bakery_tool_path(task, action->source);
  }
  arguments[count++] = "--texture-class";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "class", "color-srgb");
  arguments[count++] = "--encoding";
  arguments[count++] = vkr_bakery_recipe_string(recipe, "encoding", "astc");
  char max_extent[16];
  if (strcmp(vkr_bakery_recipe_string(recipe, "tier", "final"), "preview") ==
      0) {
    /* The preview tier's mip floor: levels above this extent are dropped. */
    (void)snprintf(max_extent, sizeof(max_extent), "%u",
                   VKR_VKT_PREVIEW_MAX_EXTENT);
    arguments[count++] = "--max-extent";
    arguments[count++] = max_extent;
  }
  arguments[count++] = "--strict";
  arguments[count++] = "--no-progress";
  arguments[count++] = "--threads";
  arguments[count++] = "auto";
  char cutoff[32];
  char factor[32];
  if (vkr_bakery_json_get(recipe, "alpha_cutoff")) {
    (void)snprintf(cutoff, sizeof(cutoff), "%.9g",
                   vkr_bakery_recipe_number(recipe, "alpha_cutoff", 0.5));
    arguments[count++] = "--alpha-cutoff";
    arguments[count++] = cutoff;
  }
  if (vkr_bakery_json_get(recipe, "alpha_factor")) {
    (void)snprintf(factor, sizeof(factor), "%.9g",
                   vkr_bakery_recipe_number(recipe, "alpha_factor", 1.0));
    arguments[count++] = "--alpha-factor";
    arguments[count++] = factor;
  }
  if (action->force) {
    arguments[count++] = "--force";
  }
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, "texture", arguments, count, &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_TEX_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "vkt", staged);
}

const VkrBakeryProducer vkr_bakery_producer_texture = {
    .id = "texture",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_TEXTURE,
    .flags = VKR_BAKERY_PRODUCER_EXCLUSIVE_CORES,
    .summary = "PNG/JPEG/BMP/TGA image to a host-native ASTC or BC KTX2 .vkt "
               "(ADR-012).",
    .recipe_fields = vkr_bakery_texture_fields,
    .plan = vkr_bakery_texture_plan,
    .estimate_peak_mib = vkr_bakery_texture_estimate,
    .run = vkr_bakery_texture_run,
};

// =============================================================================
// mesh
// =============================================================================

vkr_internal const char *const vkr_bakery_mesh_fields[] = {
    "light_ranges", "lightmap_texels_per_unit", NULL};

vkr_internal bool8_t vkr_bakery_mesh_plan(VkrBakeryGraph *graph,
                                          VkrBakeryAction *action) {
  if (!vkr_bakery_require_source(graph, action)) {
    return false_v;
  }
  char sidecar[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(sidecar, sizeof(sidecar), "%s.vkr.json", action->source);
  if (vkr_bakery_is_file(sidecar) &&
      !vkr_bakery_action_input(graph, action, sidecar,
                               vkr_bakery_path_name(sidecar))) {
    return false_v;
  }
  const VkrBakeryJson *ranges =
      vkr_bakery_json_get(action->recipe, "light_ranges");
  if (ranges) {
    for (const VkrBakeryJson *range = ranges->first; range;
         range = range->next) {
      if (range->type != VKR_BAKERY_JSON_FLOAT &&
          range->type != VKR_BAKERY_JSON_INT) {
        vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                             "light_ranges values must be meters");
        return false_v;
      }
    }
  }
  /* Opt-in lightmap UV set for static meshes (ADR-087), in texels per mesh
     unit; absent leaves the cooked artifact unchanged. */
  const VkrBakeryJson *lightmap =
      vkr_bakery_json_get(action->recipe, "lightmap_texels_per_unit");
  if (lightmap && ((lightmap->type != VKR_BAKERY_JSON_FLOAT &&
                    lightmap->type != VKR_BAKERY_JSON_INT) ||
                   !(lightmap->number > 0.0) || lightmap->number > 65536.0)) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                         "lightmap_texels_per_unit must be a positive number");
    return false_v;
  }
  char default_output[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_stem_path(default_output, sizeof(default_output), action->source,
                       ".vkb");
  vkr_bakery_action_label(graph, action, "meshoptimizer%s%s",
                          ranges && ranges->count ? " + light ranges" : "",
                          lightmap ? " + lightmap UVs" : "");
  return vkr_bakery_default_output(graph, action, "vkb", default_output);
}

vkr_internal uint64_t vkr_bakery_mesh_estimate(const VkrBakeryAction *action) {
  /* Decoded geometry, optimizer copies and texture decodes scale with the
   * source; the cooker's arenas only commit what they touch. */
  uint64_t bytes = 0u;
  VkrBakeryStat info;
  if (vkr_bakery_stat(action->source, &info)) {
    bytes += info.size;
  }
  char buffer[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_stem_path(buffer, sizeof(buffer), action->source, ".bin");
  if (vkr_bakery_stat(buffer, &info) && info.exists) {
    bytes += info.size;
  }
  return 512u + bytes * 10u / (1024u * 1024u);
}

/* Generated materials and derived textures are side outputs of a repository
 * cook; everything else the cook recorded was read as input. */
vkr_internal bool8_t vkr_bakery_mesh_is_generated(const char *relative) {
  return strncmp(relative, "assets/materials/", 17u) == 0 ||
         strncmp(relative, "assets/textures/generated/", 26u) == 0;
}

vkr_internal bool8_t vkr_bakery_mesh_collect(VkrBakeryTask *task,
                                             const char *list_path) {
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(list_path, MB(64), &data, &length)) {
    vkr_bakery_task_fail(task, "the mesh cooker wrote no dependency list");
    return false_v;
  }
  const char *root = task->config->root;
  uint32_t side_index = 0u;
  bool8_t ok = true_v;
  char *cursor = (char *)data;
  while (ok && cursor && *cursor) {
    char *end = strchr(cursor, '\n');
    if (end) {
      *end = 0;
    }
    if (*cursor) {
      char absolute[VKR_BAKERY_PATH_CAPACITY];
      if (vkr_bakery_path_is_absolute(cursor)) {
        (void)snprintf(absolute, sizeof(absolute), "%s", cursor);
      } else {
        (void)vkr_bakery_path_join(absolute, sizeof(absolute), root, cursor);
      }
      char relative[VKR_BAKERY_PATH_CAPACITY];
      const bool8_t inside =
          root[0] &&
          vkr_bakery_path_relative(root, absolute, relative, sizeof(relative));
      if (inside && vkr_bakery_mesh_is_generated(relative)) {
        char name[64];
        (void)snprintf(name, sizeof(name), "side/%u", side_index++);
        const char *staged = vkr_bakery_task_stage(task, name);
        if (!vkr_bakery_clone_or_copy(absolute, staged)) {
          vkr_bakery_task_fail(task, "could not stage generated output %s",
                               absolute);
          ok = false_v;
        } else {
          ok = vkr_bakery_task_side_product(task, "side", staged, absolute);
        }
      } else {
        vkr_bakery_task_discovered(task, absolute);
      }
    }
    cursor = end ? end + 1 : NULL;
  }
  free(data);
  return ok;
}

vkr_internal bool8_t vkr_bakery_mesh_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const char *staged = vkr_bakery_task_stage(task, "mesh.vkb");
  const char *list = vkr_bakery_task_stage(task, "dependencies.txt");
  const char *arguments[2u * 64u + 8u];
  uint32_t count = 0u;
  arguments[count++] = "--input";
  arguments[count++] = vkr_bakery_tool_path(task, action->source);
  arguments[count++] = "--output";
  arguments[count++] = staged;
  arguments[count++] = "--dependency-list";
  arguments[count++] = list;
  const VkrBakeryJson *ranges =
      vkr_bakery_json_get(action->recipe, "light_ranges");
  for (const VkrBakeryJson *range = ranges ? ranges->first : NULL;
       range && count + 2u < 58u; range = range->next) {
    char *value = (char *)arena_alloc(task->arena, range->key.length + 48u,
                                      ARENA_MEMORY_TAG_STRING);
    if (!value) {
      return false_v;
    }
    (void)snprintf(value, range->key.length + 48u, "%.*s=%.9g",
                   (int)range->key.length, range->key.str, range->number);
    arguments[count++] = "--light-range";
    arguments[count++] = value;
  }
  const VkrBakeryJson *lightmap =
      vkr_bakery_json_get(action->recipe, "lightmap_texels_per_unit");
  char lightmap_value[48];
  if (lightmap) {
    (void)snprintf(lightmap_value, sizeof(lightmap_value), "%.9g",
                   lightmap->number);
    arguments[count++] = "--lightmap-texels-per-unit";
    arguments[count++] = lightmap_value;
  }
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, "mesh", arguments, count, &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_MESH_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "vkb", staged) &&
         vkr_bakery_mesh_collect(task, list);
}

const VkrBakeryProducer vkr_bakery_producer_mesh = {
    .id = "mesh",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_MESH,
    .summary = "glTF/GLB/OBJ to meshoptimizer .vkb (ADR-030), with its "
               "generated materials and derived textures.",
    .recipe_fields = vkr_bakery_mesh_fields,
    .plan = vkr_bakery_mesh_plan,
    .estimate_peak_mib = vkr_bakery_mesh_estimate,
    .run = vkr_bakery_mesh_run,
};

// =============================================================================
// animation and collision
// =============================================================================

vkr_internal const char *const vkr_bakery_no_fields[] = {NULL};

vkr_internal bool8_t vkr_bakery_animation_plan(VkrBakeryGraph *graph,
                                               VkrBakeryAction *action) {
  if (!vkr_bakery_require_source(graph, action) ||
      !vkr_bakery_gltf_inputs(graph, action)) {
    return false_v;
  }
  char default_output[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_stem_path(default_output, sizeof(default_output), action->source,
                       ".vka");
  vkr_bakery_action_label(graph, action, "animation bank");
  return vkr_bakery_default_output(graph, action, "vka", default_output);
}

vkr_internal bool8_t vkr_bakery_animation_run(VkrBakeryTask *task) {
  const char *staged = vkr_bakery_task_stage(task, "animation.vka");
  const char *arguments[] = {"--input",
                             vkr_bakery_tool_path(task, task->action->source),
                             "--output", staged};
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, "animation", arguments, ArrayCount(arguments),
                            &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_ANIM_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "vka", staged);
}

const VkrBakeryProducer vkr_bakery_producer_animation = {
    .id = "animation",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_ANIMATION,
    .summary = "glTF animations to a .vka bank (ADR-071).",
    .recipe_fields = vkr_bakery_no_fields,
    .plan = vkr_bakery_animation_plan,
    .run = vkr_bakery_animation_run,
};

vkr_internal const char *const vkr_bakery_collision_fields[] = {"kind", "node",
                                                                NULL};

vkr_internal bool8_t vkr_bakery_collision_plan(VkrBakeryGraph *graph,
                                               VkrBakeryAction *action) {
  if (!vkr_bakery_require_source(graph, action) ||
      !vkr_bakery_gltf_inputs(graph, action)) {
    return false_v;
  }
  const char *kind = vkr_bakery_recipe_string(action->recipe, "kind", "hull");
  if (strcmp(kind, "hull") && strcmp(kind, "mesh")) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                         "kind must be hull or mesh");
    return false_v;
  }
  vkr_bakery_json_set(graph->arena, action->recipe, "kind",
                      vkr_bakery_json_cstr(graph->arena, kind));
  char default_output[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_stem_path(default_output, sizeof(default_output), action->source,
                       ".vkc");
  vkr_bakery_action_label(graph, action, "%s collision", kind);
  return vkr_bakery_default_output(graph, action, "vkc", default_output);
}

vkr_internal bool8_t vkr_bakery_collision_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const char *staged = vkr_bakery_task_stage(task, "collision.vkc");
  const char *arguments[8];
  uint32_t count = 0u;
  arguments[count++] = "--input";
  arguments[count++] = vkr_bakery_tool_path(task, action->source);
  arguments[count++] = "--output";
  arguments[count++] = staged;
  arguments[count++] = "--kind";
  arguments[count++] = vkr_bakery_recipe_string(action->recipe, "kind", "hull");
  char node[32];
  int64_t node_index = 0;
  if (vkr_bakery_json_get_int(action->recipe, "node", &node_index)) {
    (void)snprintf(node, sizeof(node), "%lld", (long long)node_index);
    arguments[count++] = "--node";
    arguments[count++] = node;
  }
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, "collision", arguments, count, &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_COLL_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "vkc", staged);
}

const VkrBakeryProducer vkr_bakery_producer_collision = {
    .id = "collision",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_COLLISION,
    .summary = "glTF proxy to a .vkc hull or triangle mesh (ADR-072).",
    .recipe_fields = vkr_bakery_collision_fields,
    .plan = vkr_bakery_collision_plan,
    .run = vkr_bakery_collision_run,
};

// =============================================================================
// font
// =============================================================================

vkr_internal bool8_t vkr_bakery_font_plan(VkrBakeryGraph *graph,
                                          VkrBakeryAction *action) {
  if (!vkr_bakery_require_source(graph, action)) {
    return false_v;
  }
  uint8_t *data = NULL;
  uint64_t length = 0u;
  if (!vkr_bakery_read_file(action->source, MB(1), &data, &length)) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_UNREADABLE_SOURCE,
                         "cannot read the font configuration");
    return false_v;
  }
  char directory[VKR_BAKERY_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), action->source);
  char output[VKR_BAKERY_PATH_CAPACITY] = {0};
  bool8_t ok = true_v;
  char *cursor = (char *)data;
  while (cursor && *cursor) {
    char *end = strchr(cursor, '\n');
    if (end) {
      *end = 0;
    }
    char *line = cursor;
    cursor = end ? end + 1 : NULL;
    const uint64_t line_length = strlen(line);
    if (line_length && line[line_length - 1u] == '\r') {
      line[line_length - 1u] = 0;
    }
    char *equals = strchr(line, '=');
    if (line[0] == '#' || !equals) {
      continue;
    }
    *equals = 0;
    const char *value = equals + 1;
    char path[VKR_BAKERY_PATH_CAPACITY];
    if (strcmp(line, "source") == 0 || strcmp(line, "charset_file") == 0) {
      if (!vkr_bakery_path_join(path, sizeof(path), directory, value) ||
          !vkr_bakery_is_file(path)) {
        vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_IDX_MISSING_SOURCE,
                             "%s '%s' does not exist", line, value);
        ok = false_v;
        continue;
      }
      ok = vkr_bakery_action_input(graph, action, path, value) && ok;
    } else if (strcmp(line, "file") == 0) {
      (void)vkr_bakery_path_join(output, sizeof(output), directory, value);
    }
  }
  free(data);
  if (!ok) {
    return false_v;
  }
  if (!output[0] && !action->requested_output) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                         "the configuration names no output 'file'");
    return false_v;
  }
  vkr_bakery_action_label(graph, action, "MTSDF atlas");
  return vkr_bakery_default_output(graph, action, "vkfa", output);
}

vkr_internal bool8_t vkr_bakery_font_run(VkrBakeryTask *task) {
  const VkrBakeryAction *action = task->action;
  const char *staged = vkr_bakery_task_stage(task, "font.vkfa");
  vkr_bakery_seed(task, staged);
  const char *arguments[6];
  uint32_t count = 0u;
  arguments[count++] = "--config";
  arguments[count++] = vkr_bakery_tool_path(task, action->source);
  arguments[count++] = "--output";
  arguments[count++] = staged;
  if (action->force) {
    arguments[count++] = "--force";
  }
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, "font", arguments, count, &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_FONT_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "vkfa", staged);
}

const VkrBakeryProducer vkr_bakery_producer_font = {
    .id = "font",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_FONT,
    .summary = ".fontcfg to an MTSDF .vkfa atlas (ADR-034).",
    .recipe_fields = vkr_bakery_no_fields,
    .plan = vkr_bakery_font_plan,
    .run = vkr_bakery_font_run,
};

// =============================================================================
// table
// =============================================================================

vkr_internal const char *const vkr_bakery_table_fields[] = {"table", NULL};

vkr_internal bool8_t vkr_bakery_table_plan(VkrBakeryGraph *graph,
                                           VkrBakeryAction *action) {
  const char *table = vkr_bakery_recipe_string(action->recipe, "table", "");
  if (strcmp(table, "dfg") && strcmp(table, "sheen") &&
      strcmp(table, "anisotropy")) {
    vkr_bakery_plan_diag(graph, action, VKR_BAKERY_DIAG_REC_INVALID_VALUE,
                         "table must be dfg, sheen or anisotropy");
    return false_v;
  }
  action->display = vkr_bakery_graph_printf(graph, "table:%s", table);
  char output[VKR_BAKERY_PATH_CAPACITY];
  (void)snprintf(output, sizeof(output), "%s/renderer/src/vkr_%s_lut_data.inc",
                 graph->config->root, table);
  vkr_bakery_action_label(graph, action, "%s lookup table", table);
  return vkr_bakery_default_output(graph, action, "inc", output);
}

vkr_internal bool8_t vkr_bakery_table_run(VkrBakeryTask *task) {
  const char *table =
      vkr_bakery_recipe_string(task->action->recipe, "table", "dfg");
  const char *staged = vkr_bakery_task_stage(task, "table.inc");
  const char *arguments[] = {staged};
  int32_t exit_code = -1;
  if (!vkr_bakery_task_tool(task, table, arguments, 1u, &exit_code)) {
    return false_v;
  }
  if (exit_code != 0) {
    vkr_bakery_tool_failed(task, VKR_BAKERY_DIAG_TABLE_FAILED, exit_code);
    return false_v;
  }
  return vkr_bakery_task_product(task, "inc", staged);
}

const VkrBakeryProducer vkr_bakery_producer_table = {
    .id = "table",
    .version = 1u,
    .identity = VKR_BAKERY_IDENTITY_TABLE,
    .summary = "Checked-in BRDF lookup tables (dfg, sheen, anisotropy); the "
               "renderer must be rebuilt after a change.",
    .recipe_fields = vkr_bakery_table_fields,
    .plan = vkr_bakery_table_plan,
    .run = vkr_bakery_table_run,
};
