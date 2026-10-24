#include "vkr_project_internal.h"

#include "filesystem/filesystem.h"

#include <stdio.h>
#include <string.h>

/* `recook_textures`: cooks again, on this host, every texture a build's
 * VKR_PROJECT_RECIPES_FILE names whose `.vkt` is missing or holds another
 * GPU family's blocks (ASTC on Apple silicon, BC on x86-64), under the name
 * its materials already hold. A project another host shares arrives
 * without cooked textures (`.vkrdepotignore` names `*.vkt`), so its scenes
 * open after this runs.
 *
 * A recipe with a `source` packs that image beside it, as the project
 * texture packer did. Every other recipe came from the mesh cook, whose
 * generated textures can depend on a whole material (spec-gloss
 * conversion, normal and roughness pairs, cutouts), so the build's mesh
 * record is cooked again from its source snapshot into a staging folder
 * and only the textures it lacked move into the build. A build with no mesh
 * record or no snapshot left keeps those textures uncooked and is reported
 * in `missing_source`, so the rest of the scene still cooks. Names are the
 * recipes' (vkr_mesh_cook_source.c), so the materials, the mesh and the
 * inventory stay as they are: no revision is made and the inventory is not
 * written. Every texture publishes by rename, so a cancelled run leaves
 * each texture either absent, as it was, or complete. */

#define VKR_RECOOK_KTX2_HEADER 16u

/* GPU block family of a cooked texture. */
typedef enum VkrRecookFamily {
  VKR_RECOOK_FAMILY_MISSING = 0,
  VKR_RECOOK_FAMILY_ASTC,
  VKR_RECOOK_FAMILY_BC,
  /* Unreadable, or a format every host samples. */
  VKR_RECOOK_FAMILY_OTHER,
} VkrRecookFamily;

/* One build revision whose textures the run checks. */
typedef struct VkrRecookBuild {
  char directory[VKR_PROJECT_PATH];
  const VkrBakeryJson *mesh;  /* The mesh record cooked into it, or NULL. */
  const VkrBakeryJson *first; /* A record of it, for reports. */
} VkrRecookBuild;

typedef struct VkrRecook {
  VkrProjectJob *job;
  const char *owner; /* Scene root, or the project root. */
  VkrRecookBuild *builds;
  uint32_t build_count;
  VkrRecookFamily host;
  /* Scene preparation: a texture present counts as current whatever its
     family, and a read-only job only counts what it would cook. */
  bool8_t missing_only;
  uint32_t textures;
  uint32_t current;
  uint32_t recooked;
  uint32_t mesh_cooks;
  uint32_t skipped;
  VkrBakeryJson *needs_rebuild;
  VkrBakeryJson *missing_source; /* [{asset_id, name, build, textures}] */
} VkrRecook;

vkr_internal VkrRecookFamily vkr_recook_family(const char *path) {
  static const uint8_t identifier[12] = {0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
                                         0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
  if (!vkr_bakery_is_file(path)) {
    return VKR_RECOOK_FAMILY_MISSING;
  }
  FILE *file = file_fopen(path, "rb");
  uint8_t header[VKR_RECOOK_KTX2_HEADER] = {0};
  const bool8_t read =
      file && fread(header, 1u, sizeof(header), file) == sizeof(header);
  if (file) {
    (void)fclose(file);
  }
  if (!read || MemCompare(header, identifier, sizeof(identifier)) != 0) {
    return VKR_RECOOK_FAMILY_OTHER;
  }
  const uint32_t format = (uint32_t)header[12] | ((uint32_t)header[13] << 8u) |
                          ((uint32_t)header[14] << 16u) |
                          ((uint32_t)header[15] << 24u);
  /* VkFormat: BC5 141-142, BC7 145-146, ASTC LDR 157-184, ASTC HDR
     (VK_EXT_texture_compression_astc_hdr) 1000066000-1000066013. */
  if ((format >= 141u && format <= 142u) ||
      (format >= 145u && format <= 146u)) {
    return VKR_RECOOK_FAMILY_BC;
  }
  if ((format >= 157u && format <= 184u) ||
      (format >= 1000066000u && format <= 1000066013u)) {
    return VKR_RECOOK_FAMILY_ASTC;
  }
  return VKR_RECOOK_FAMILY_OTHER;
}

/* The build folder below `owner` that a record path names, or false: a
   `builds/<revision>/...` path whose revision is an identifier. */
vkr_internal bool8_t vkr_recook_build_of(const VkrRecook *recook,
                                         const char *path, char *out) {
  if (!path || strncmp(path, "builds/", 7u) != 0) {
    return false_v;
  }
  const char *end = strchr(path + 7, '/');
  char revision[40];
  if (!end || end - (path + 7) != 36) {
    return false_v;
  }
  (void)snprintf(revision, sizeof(revision), "%.36s", path + 7);
  return vkr_project_identifier_valid(revision) &&
         (uint32_t)snprintf(out, VKR_PROJECT_PATH, "%s/builds/%s",
                            recook->owner, revision) < VKR_PROJECT_PATH;
}

/* A build's `textures` folder, refused when it or a folder above it inside
   the owner is a link or resolves outside the owner: recipes come from
   another host and must not reach past their build. */
vkr_internal bool8_t vkr_recook_textures_folder(VkrRecook *recook,
                                                const VkrRecookBuild *build,
                                                char *out) {
  VkrProjectJob *job = recook->job;
  char builds[VKR_PROJECT_PATH];
  (void)snprintf(builds, sizeof(builds), "%s/builds", recook->owner);
  (void)snprintf(out, VKR_PROJECT_PATH, "%s/textures", build->directory);
  const char *const folders[] = {builds, build->directory, out};
  for (uint32_t i = 0u; i < ArrayCount(folders); ++i) {
    if (vkr_project_lstat_is_link(folders[i])) {
      return vkr_project_fail(job, "Re-cook refuses links: %s", folders[i]);
    }
  }
  char resolved[VKR_PROJECT_PATH];
  return vkr_project_contained(
      job, recook->owner,
      vkr_project_printf(job, "builds/%s/textures",
                         vkr_bakery_path_name(build->directory)),
      false_v, resolved);
}

vkr_internal bool8_t vkr_recook_collect(VkrRecook *recook,
                                        const VkrBakeryJson *records) {
  VkrProjectJob *job = recook->job;
  const uint32_t capacity = records ? records->count : 0u;
  recook->builds = (VkrRecookBuild *)arena_alloc(
      job->arena, (capacity ? capacity : 1u) * sizeof(VkrRecookBuild),
      ARENA_MEMORY_TAG_ARRAY);
  if (!recook->builds) {
    return vkr_project_fail(job, "Out of memory");
  }
  for (const VkrBakeryJson *record = records ? records->first : NULL; record;
       record = record->next) {
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    for (const VkrBakeryJson *artifact = artifacts ? artifacts->first : NULL;
         artifact; artifact = artifact->next) {
      char directory[VKR_PROJECT_PATH];
      const char *path = vkr_project_json_text(artifact, "path");
      if (!vkr_recook_build_of(recook, path, directory)) {
        continue;
      }
      VkrRecookBuild *build = NULL;
      for (uint32_t i = 0u; i < recook->build_count && !build; ++i) {
        if (strcmp(recook->builds[i].directory, directory) == 0) {
          build = &recook->builds[i];
        }
      }
      if (!build) {
        build = &recook->builds[recook->build_count++];
        MemZero(build, sizeof(*build));
        (void)snprintf(build->directory, sizeof(build->directory), "%s",
                       directory);
        build->first = record;
      }
      if (vkr_bakery_json_is_string(vkr_bakery_json_get(record, "kind"),
                                    "mesh") &&
          vkr_project_json_text(record, "source")) {
        build->mesh = record;
      }
    }
  }
  return true_v;
}

bool8_t vkr_project_read_recipes(VkrProjectJob *job, const char *textures,
                                 const VkrBakeryJson **out_recipes) {
  *out_recipes = NULL;
  const char *path =
      vkr_project_printf(job, "%s/%s", textures, VKR_PROJECT_RECIPES_FILE);
  if (vkr_project_lstat_is_link(path)) {
    return vkr_project_fail(job, "Texture recipes must not be a link: %s",
                            path);
  }
  if (!vkr_bakery_is_file(path)) {
    return true_v;
  }
  const VkrBakeryJson *document =
      vkr_project_load_json(job, path, VKR_PROJECT_MAX_JSON_BYTES);
  VKR_PROJECT_TRY(document);
  const VkrBakeryJson *recipes = vkr_bakery_json_get(document, "textures");
  if (!recipes || recipes->type != VKR_BAKERY_JSON_OBJECT) {
    return vkr_project_fail(job, "Invalid texture recipes: %s", path);
  }
  *out_recipes = recipes;
  return true_v;
}

/* Whether `text`, a material file of the build, names a texture of the
   build's own `textures` folder (`../textures/<name>.vkt`) no recipe
   covers. */
vkr_internal bool8_t vkr_project_material_unrecorded(
    const uint8_t *text, uint64_t size, const VkrBakeryJson *recipes) {
  static const char prefix[] = "../textures/";
  const uint64_t prefix_length = sizeof(prefix) - 1u;
  for (uint64_t at = 0u; at + prefix_length < size; ++at) {
    if (MemCompare(text + at, prefix, prefix_length) != 0) {
      continue;
    }
    char name[96];
    uint32_t length = 0u;
    for (uint64_t c = at + prefix_length;
         c < size && length + 1u < sizeof(name); ++c) {
      const char ch = (char)text[c];
      const bool8_t plain =
          (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
          (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-' || ch == '.';
      if (!plain) {
        break;
      }
      name[length++] = ch;
    }
    name[length] = '\0';
    if (vkr_project_is_cooked_texture(name) &&
        !vkr_bakery_json_get(recipes, name)) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_project_textures_unrecorded(VkrProjectJob *job,
                                        const char *textures,
                                        const VkrBakeryJson *recipes) {
  VkrProjectStrings names = {0};
  const bool8_t failed_before = job->failed;
  if (vkr_bakery_is_directory(textures) &&
      vkr_project_list(job, textures, &names)) {
    for (uint32_t i = 0u; i < names.count; ++i) {
      if (vkr_project_is_cooked_texture(names.items[i]) &&
          !vkr_bakery_json_get(recipes, names.items[i])) {
        return true_v;
      }
    }
  }
  vkr_project_forgive(job, failed_before);
  /* A copy that never held the cooked textures, as a pull leaves one,
     knows them only from the materials that name them. */
  char build[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(build, sizeof(build), textures);
  static const char *const folders[] = {"dependencies", "materials"};
  for (uint32_t f = 0u; f < ArrayCount(folders); ++f) {
    const char *folder = vkr_project_printf(job, "%s/%s", build, folders[f]);
    if (!vkr_bakery_is_directory(folder) ||
        !vkr_project_list(job, folder, &names)) {
      vkr_project_forgive(job, failed_before);
      continue;
    }
    for (uint32_t i = 0u; i < names.count; ++i) {
      const char *name = names.items[i];
      const uint64_t length = strlen(name);
      if (length < 4u || strcmp(name + length - 3u, ".mt") != 0) {
        continue;
      }
      uint8_t *text = NULL;
      uint64_t size = 0u;
      const bool8_t read = vkr_bakery_read_file(
          vkr_project_printf(job, "%s/%s", folder, name), MB(4), &text, &size);
      const bool8_t unrecorded =
          read && vkr_project_material_unrecorded(text, size, recipes);
      free(text);
      if (unrecorded) {
        return true_v;
      }
    }
  }
  return false_v;
}

vkr_internal void vkr_recook_note_rebuild(VkrRecook *recook,
                                          const VkrRecookBuild *build) {
  Arena *arena = recook->job->arena;
  const VkrBakeryJson *record = build->mesh ? build->mesh : build->first;
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(
      arena, entry, "asset_id",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(record, "id")));
  const VkrBakeryJson *name = vkr_bakery_json_get(record, "name");
  vkr_bakery_json_set(arena, entry, "name",
                      name ? vkr_bakery_json_clone(arena, name)
                           : vkr_bakery_json_null(arena));
  vkr_bakery_json_append(recook->needs_rebuild, entry);
}

/* The source snapshot of the build's mesh record, or false when the build
   has no mesh record with one: the record moved to a newer revision and
   left this build's material and texture records behind, or the snapshot
   file is gone. */
vkr_internal bool8_t vkr_recook_mesh_source(VkrRecook *recook,
                                            const VkrRecookBuild *build,
                                            char *out) {
  VkrProjectJob *job = recook->job;
  const VkrBakeryJson *record = build->mesh;
  if (!record || !vkr_project_json_text(record, "import_id")) {
    return false_v;
  }
  const bool8_t failed_before = job->failed;
  const bool8_t found = vkr_project_contained(
      job, recook->owner, vkr_project_json_text(record, "source"), true_v, out);
  vkr_project_forgive(job, failed_before);
  return found;
}

/* Reports a build whose mesh recipes cannot cook without a source snapshot
   and leaves its `textures` uncooked; the rest of the run goes on. */
vkr_internal void vkr_recook_note_missing_source(VkrRecook *recook,
                                                 const VkrRecookBuild *build,
                                                 uint32_t textures) {
  Arena *arena = recook->job->arena;
  const VkrBakeryJson *record = build->mesh ? build->mesh : build->first;
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(
      arena, entry, "asset_id",
      vkr_bakery_json_clone(arena, vkr_bakery_json_get(record, "id")));
  const VkrBakeryJson *name = vkr_bakery_json_get(record, "name");
  vkr_bakery_json_set(arena, entry, "name",
                      name ? vkr_bakery_json_clone(arena, name)
                           : vkr_bakery_json_null(arena));
  vkr_bakery_json_set(
      arena, entry, "build",
      vkr_bakery_json_cstr(arena, vkr_bakery_path_name(build->directory)));
  vkr_bakery_json_set(arena, entry, "textures",
                      vkr_bakery_json_int(arena, textures));
  vkr_bakery_json_append(recook->missing_source, entry);
  recook->skipped += textures;
}

/* Warns about the builds whose mesh textures did not cook for want of a
   source snapshot (vkr_recook_note_missing_source). */
vkr_internal void vkr_recook_warn_missing_source(const VkrRecook *recook) {
  if (recook->missing_source->count) {
    vkr_project_warn(recook->job,
                     "%u textures of %u builds did not cook: their mesh "
                     "source snapshot is missing; rebuild or reimport the "
                     "model, or delete the records that name them",
                     recook->skipped, recook->missing_source->count);
  }
}

/* Packs the source images of `entries` (name -> image recipe) into their
   names on this host. */
vkr_internal bool8_t vkr_recook_images(VkrRecook *recook, const char *textures,
                                       const VkrBakeryJson *entries) {
  VkrProjectJob *job = recook->job;
  if (!entries->count) {
    return true_v;
  }
  VkrBakeryGraph graph;
  if (!vkr_bakery_graph_init(&graph, job->config)) {
    return vkr_project_fail(job, "Cannot start the texture graph");
  }
  vkr_bakery_graph_use_index(&graph, vkr_project_index(job));
  bool8_t ok = true_v;
  for (const VkrBakeryJson *entry = entries->first; ok && entry;
       entry = entry->next) {
    const char *source_name = vkr_project_json_text(entry, "source");
    const char *texture_class = vkr_project_json_text(entry, "class");
    const bool8_t fast =
        vkr_bakery_json_is_string(vkr_bakery_json_get(entry, "speed"), "fast");
    const bool8_t preview = vkr_bakery_json_is_string(
        vkr_bakery_json_get(entry, "tier"), "preview");
    char source[VKR_PROJECT_PATH];
    if (!vkr_project_plain_name(source_name) || !texture_class ||
        !vkr_bakery_path_join(source, sizeof(source), textures, source_name) ||
        vkr_project_lstat_is_link(source) || !vkr_bakery_is_file(source)) {
      ok = vkr_project_fail(job,
                            "The source image of %s is missing or not "
                            "a plain file in its folder",
                            (const char *)entry->key.str);
      break;
    }
    VkrVktEncoding encoding = vkr_vkt_host_encoding();
    if (fast) {
      encoding = encoding == VKR_VKT_ENCODING_BC ? VKR_VKT_ENCODING_BC_FAST
                                                 : VKR_VKT_ENCODING_ASTC_FAST;
    }
    static const char *const encodings[4] = {"astc", "astc-fast", "bc",
                                             "bc-fast"};
    VkrBakeryJson *recipe = vkr_bakery_json_object(graph.arena);
    vkr_bakery_json_set(graph.arena, recipe, "class",
                        vkr_bakery_json_cstr(graph.arena, texture_class));
    vkr_bakery_json_set(
        graph.arena, recipe, "tier",
        vkr_bakery_json_cstr(graph.arena, preview ? "preview" : "final"));
    vkr_bakery_json_set(graph.arena, recipe, "encoding",
                        vkr_bakery_json_cstr(graph.arena, encodings[encoding]));
    const char *destination = vkr_project_printf(job, "%s/%s", textures,
                                                 (const char *)entry->key.str);
    if (!vkr_bakery_graph_add(&graph, vkr_bakery_producer_find("texture"),
                              source, recipe, destination)) {
      ok = vkr_project_fail(job, "Cannot plan the texture %s",
                            (const char *)entry->key.str);
    }
  }
  if (ok && !vkr_bakery_cache_prepare(job->config)) {
    ok = vkr_project_fail(job, "The bakery cache is not writable");
  }
  if (ok && !vkr_bakery_graph_execute(&graph)) {
    ok = vkr_project_check_cancel(job) &&
         vkr_project_fail(job, "Re-cooking textures failed; see the job log");
  }
  vkr_bakery_graph_shutdown(&graph);
  return ok;
}

/* Cooks the build's mesh record again from its `source` snapshot into a
   staging folder at the tier and speed of `entries` (name -> mesh-cook
   recipe), then moves each named texture into the build. */
vkr_internal bool8_t vkr_recook_mesh(VkrRecook *recook,
                                     const VkrRecookBuild *build,
                                     const char *source, const char *textures,
                                     const VkrBakeryJson *entries,
                                     bool8_t preview, bool8_t fast) {
  VkrProjectJob *job = recook->job;
  const char *import_id = vkr_project_json_text(build->mesh, "import_id");
  char staging[VKR_PROJECT_PATH];
  char bundle[VKR_PROJECT_PATH];
  (void)snprintf(staging, sizeof(staging), "%s/.staging", job->project_root);
  VKR_PROJECT_TRY(vkr_project_make_dirs(job, staging));
  VKR_PROJECT_TRY(vkr_project_mkdtemp(job, staging, "recook-", bundle));
  /* The job's release removes the staging folder. */
  (void)snprintf(job->stage, sizeof(job->stage), "%s", bundle);
  job->texture_preview = preview;
  job->texture_deferred = false_v;
  job->texture_encoding = vkr_vkt_host_encoding();
  if (fast) {
    job->texture_encoding = job->texture_encoding == VKR_VKT_ENCODING_BC
                                ? VKR_VKT_ENCODING_BC_FAST
                                : VKR_VKT_ENCODING_ASTC_FAST;
  }
  const char *output = vkr_project_printf(job, "%s/mesh.vkb", bundle);
  VKR_PROJECT_TRY(vkr_project_cook_mesh(job, source, output, bundle, import_id,
                                        0.0f, "Re-cooking textures"));
  recook->mesh_cooks += 1u;
  for (const VkrBakeryJson *entry = entries->first; entry;
       entry = entry->next) {
    const char *name = (const char *)entry->key.str;
    const char *cooked =
        vkr_project_printf(job, "%s/textures/%s", bundle, name);
    const char *destination = vkr_project_printf(job, "%s/%s", textures, name);
    if (!vkr_bakery_is_file(cooked)) {
      return vkr_project_fail(job,
                              "Cooking %s again did not reproduce %s; "
                              "rebuild the asset",
                              build->directory, name);
    }
    if (!vkr_bakery_rename(cooked, destination, true_v)) {
      return vkr_project_fail(job, "Cannot place %s", destination);
    }
  }
  (void)vkr_project_remove_tree(bundle);
  job->stage[0] = 0;
  return true_v;
}

/* Checks one build's recipes and cooks what this host lacks. */
vkr_internal bool8_t vkr_recook_build(VkrRecook *recook,
                                      const VkrRecookBuild *build) {
  VkrProjectJob *job = recook->job;
  Arena *arena = job->arena;
  char textures[VKR_PROJECT_PATH];
  VKR_PROJECT_TRY(vkr_recook_textures_folder(recook, build, textures));
  const VkrBakeryJson *recipes = NULL;
  VKR_PROJECT_TRY(vkr_project_read_recipes(job, textures, &recipes));
  if (!recook->missing_only &&
      vkr_project_textures_unrecorded(job, textures, recipes)) {
    vkr_recook_note_rebuild(recook, build);
  }
  if (!recipes) {
    return true_v;
  }
  /* Image recipes, then mesh recipes by tier and speed. */
  VkrBakeryJson *images = vkr_bakery_json_object(arena);
  VkrBakeryJson *meshes[4];
  for (uint32_t i = 0u; i < ArrayCount(meshes); ++i) {
    meshes[i] = vkr_bakery_json_object(arena);
  }
  for (const VkrBakeryJson *entry = recipes->first; entry;
       entry = entry->next) {
    /* A recipe names one texture beside it, whatever host wrote it. */
    const char *name = (const char *)entry->key.str;
    if (!vkr_project_plain_name(name) || !vkr_project_is_cooked_texture(name) ||
        entry->type != VKR_BAKERY_JSON_OBJECT) {
      return vkr_project_fail(job, "Invalid texture recipe '%s' in %s", name,
                              textures);
    }
    recook->textures += 1u;
    const char *texture = vkr_project_printf(job, "%s/%s", textures, name);
    if (vkr_project_lstat_is_link(texture)) {
      return vkr_project_fail(job, "Re-cook refuses links: %s", texture);
    }
    VkrRecookFamily family = VKR_RECOOK_FAMILY_MISSING;
    if (!recook->missing_only) {
      family = vkr_recook_family(texture);
    } else if (vkr_bakery_is_file(texture)) {
      family = recook->host;
    }
    if (family == recook->host || family == VKR_RECOOK_FAMILY_OTHER) {
      recook->current += 1u;
      continue;
    }
    if (recook->missing_only && job->read_only) {
      recook->skipped += 1u;
      continue;
    }
    /* Another family's blocks are of no use here and go before the cook. */
    if (family != VKR_RECOOK_FAMILY_MISSING &&
        !vkr_bakery_remove_file(texture)) {
      return vkr_project_fail(job, "Cannot replace %s", texture);
    }
    if (vkr_bakery_json_get(entry, "source")) {
      vkr_bakery_json_set(arena, images, name,
                          vkr_bakery_json_clone(arena, entry));
      continue;
    }
    const uint32_t group =
        (vkr_bakery_json_is_string(vkr_bakery_json_get(entry, "tier"),
                                   "preview")
             ? 1u
             : 0u) +
        (vkr_bakery_json_is_string(vkr_bakery_json_get(entry, "speed"), "fast")
             ? 2u
             : 0u);
    vkr_bakery_json_set(arena, meshes[group], name,
                        vkr_bakery_json_clone(arena, entry));
  }
  uint32_t mesh_textures = 0u;
  for (uint32_t group = 0u; group < ArrayCount(meshes); ++group) {
    mesh_textures += meshes[group]->count;
  }
  char source[VKR_PROJECT_PATH];
  if (mesh_textures && !vkr_recook_mesh_source(recook, build, source)) {
    vkr_recook_note_missing_source(recook, build, mesh_textures);
    mesh_textures = 0u;
  }
  for (uint32_t group = 0u; mesh_textures && group < ArrayCount(meshes);
       ++group) {
    if (meshes[group]->count) {
      VKR_PROJECT_TRY(vkr_project_progress(
          job, "Re-cooking textures", -1.0,
          vkr_project_printf(job, "%u textures of %s", meshes[group]->count,
                             vkr_bakery_path_name(build->directory))));
      VKR_PROJECT_TRY(vkr_recook_mesh(recook, build, source, textures,
                                      meshes[group], (group & 1u) != 0u,
                                      (group & 2u) != 0u));
      recook->recooked += meshes[group]->count;
    }
  }
  if (images->count) {
    VKR_PROJECT_TRY(vkr_project_progress(
        job, "Re-cooking textures", -1.0,
        vkr_project_printf(job, "%u images of %s", images->count,
                           vkr_bakery_path_name(build->directory))));
    VKR_PROJECT_TRY(vkr_recook_images(recook, textures, images));
    recook->recooked += images->count;
  }
  return true_v;
}

VkrBakeryJson *vkr_project_recook_textures(VkrProjectJob *job) {
  Arena *arena = job->arena;
  VkrRecook recook = {
      .job = job,
      .host = vkr_vkt_host_encoding() == VKR_VKT_ENCODING_BC
                  ? VKR_RECOOK_FAMILY_BC
                  : VKR_RECOOK_FAMILY_ASTC,
      .needs_rebuild = vkr_bakery_json_array(arena),
      .missing_source = vkr_bakery_json_array(arena),
  };
  const VkrBakeryJson *records = NULL;
  if (job->scene_id) {
    char expected[VKR_PROJECT_PATH];
    char path[VKR_PROJECT_PATH];
    const char *requested = vkr_project_json_text(job->request, "scene_path");
    (void)snprintf(expected, sizeof(expected), "%s/scene.json",
                   job->final_path);
    if (!requested ||
        !vkr_project_resolve(requested, true_v, path, sizeof(path)) ||
        strcmp(path, expected) != 0) {
      vkr_project_fail(job, "Re-cook scene does not match project membership");
      return NULL;
    }
    const VkrBakeryJson *scene = vkr_project_read_managed_scene(job, path);
    if (!scene) {
      return NULL;
    }
    records = vkr_bakery_json_get(scene, "assets");
    recook.owner = job->final_path;
  } else {
    const VkrBakeryJson *project = vkr_project_document(job);
    if (!project) {
      return NULL;
    }
    records = vkr_bakery_json_get(project, "assets");
    recook.owner = job->project_root;
  }
  if (!vkr_project_progress(job, "Checking textures", -1.0, "") ||
      !vkr_recook_collect(&recook, records)) {
    return NULL;
  }
  for (uint32_t i = 0u; i < recook.build_count; ++i) {
    if (!vkr_recook_build(&recook, &recook.builds[i])) {
      return NULL;
    }
  }
  if (recook.needs_rebuild->count) {
    vkr_project_warn(job,
                     "%u builds hold cooked textures without recipes; "
                     "rebuild them so other hosts can cook them",
                     recook.needs_rebuild->count);
  }
  vkr_recook_warn_missing_source(&recook);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "builds",
                      vkr_bakery_json_int(arena, recook.build_count));
  vkr_bakery_json_set(arena, result, "textures",
                      vkr_bakery_json_int(arena, recook.textures));
  vkr_bakery_json_set(arena, result, "current",
                      vkr_bakery_json_int(arena, recook.current));
  vkr_bakery_json_set(arena, result, "recooked",
                      vkr_bakery_json_int(arena, recook.recooked));
  vkr_bakery_json_set(arena, result, "mesh_cooks",
                      vkr_bakery_json_int(arena, recook.mesh_cooks));
  vkr_bakery_json_set(arena, result, "skipped",
                      vkr_bakery_json_int(arena, recook.skipped));
  vkr_bakery_json_set(arena, result, "needs_rebuild", recook.needs_rebuild);
  vkr_bakery_json_set(arena, result, "missing_source", recook.missing_source);
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  return result;
}

bool8_t vkr_project_recook_missing(VkrProjectJob *job,
                                   const VkrBakeryJson *scene,
                                   const char *owner) {
  VkrRecook recook = {
      .job = job,
      .owner = owner,
      .host = vkr_vkt_host_encoding() == VKR_VKT_ENCODING_BC
                  ? VKR_RECOOK_FAMILY_BC
                  : VKR_RECOOK_FAMILY_ASTC,
      .missing_only = true_v,
      .needs_rebuild = vkr_bakery_json_array(job->arena),
      .missing_source = vkr_bakery_json_array(job->arena),
  };
  VKR_PROJECT_TRY(
      vkr_recook_collect(&recook, vkr_bakery_json_get(scene, "assets")));
  /* A mesh recook sets the job's texture settings; the preparation after
     it cooks with the request's. */
  const bool8_t preview = job->texture_preview;
  const bool8_t deferred = job->texture_deferred;
  const VkrVktEncoding encoding = job->texture_encoding;
  bool8_t ok = true_v;
  for (uint32_t i = 0u; ok && i < recook.build_count; ++i) {
    ok = vkr_recook_build(&recook, &recook.builds[i]);
  }
  job->texture_preview = preview;
  job->texture_deferred = deferred;
  job->texture_encoding = encoding;
  VKR_PROJECT_TRY(ok);
  if (job->read_only && recook.skipped) {
    return vkr_project_fail(job,
                            "%u textures of this scene are not cooked for "
                            "this computer; open it with write access to "
                            "cook them from their recipes",
                            recook.skipped);
  }
  vkr_recook_warn_missing_source(&recook);
  return true_v;
}
