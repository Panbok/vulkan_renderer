#include "vkr_project_internal.h"

#include "../vkr_bakery_bundle.h"
#include "filesystem/vkr_vfs.h"

#include <stdio.h>
#include <string.h>

/* `share_project`: copies one project, with only the files its scenes use,
 * into a new self-contained workspace at `destination`:
 *
 *   <dest>/.vkreditor/projects/<id>/...   live project and scene files
 *   <dest>/.vkreditor/assets/terrain/...  terrain heightfields
 *   <dest>/assets/...                      every asset the project reaches
 *   <dest>/.vkrdepotignore                 per-machine state a depot skips
 *
 * A project folder keeps every earlier edit overlay and build revision; only
 * the files the manifests name are live: project.json, game.json, the World
 * documents, Scripts sources, and per scene its scene.json, its current edit
 * overlay and inventory, and the artifacts, closures, sources and import
 * manifests of that inventory's records (project.json's records likewise).
 * Their closure follows the bundle walker in its source mode
 * (vkr_bakery_bundle.h), through surface themes to the materials they bind,
 * resolving `assets/...` as the editor's workspace
 * overlay does (ADR-077): the source workspace's `assets/`, then the content
 * root, then the workspace's `.vkreditor/assets/`, where terrain
 * heightfields live. Cooked `.vkt` textures are per platform, so a texture
 * travels as its source image; a `.vkt` without one is copied and reported.
 * A build's textures travel with its recipes (VKR_PROJECT_RECIPES_FILE) and
 * the images those pack, so the receiving host cooks them again
 * (recook_textures); a build whose cooked textures have no recipes, from a
 * cook before recipes existed, is reported in `needs_rebuild`. A missing
 * reference is a warning, not a failure.
 *
 * The copy is assembled in a sibling staging folder and renamed into place,
 * so a failed or cancelled share leaves no destination. Files are cloned
 * where the volume can and copied otherwise, never hard linked: an in-place
 * write in one workspace must not change the other. */

#define VKR_SHARE_PROGRESS_SECONDS 0.1
#define VKR_SHARE_LIST_LIMIT 256u

/* Mount order of the share closure; identities are `projects/...` (below the
   workspace's `.vkreditor/`) or `assets/...`. */
typedef enum VkrShareMount {
  VKR_SHARE_MOUNT_PROJECTS = 0,
  VKR_SHARE_MOUNT_OVERLAY,
  VKR_SHARE_MOUNT_CONTENT,
  VKR_SHARE_MOUNT_WORKSPACE_ASSETS,
  VKR_SHARE_MOUNT_COUNT,
} VkrShareMount;

/* Files a group reached first: each scene, then the project documents. A
   file several groups use counts once, with the first. Scenes go first:
   project.json names every scene.json, which would credit all of them to the
   project. */
typedef struct VkrShareGroup {
  const char *id;
  const char *name;
  uint32_t first;
  uint32_t files;
  uint64_t bytes;
} VkrShareGroup;

typedef struct VkrShare {
  VkrProjectJob *job;
  VkrBundle bundle;
  VkrBakeryCli cli;
  char source[VKR_PROJECT_PATH]; /* The folder holding `.vkreditor/`. */
  char destination[VKR_PROJECT_PATH];
  char project_identity[VKR_PROJECT_PATH]; /* projects/<id> */
  char mount_directories[VKR_SHARE_MOUNT_COUNT][VKR_PROJECT_PATH];
  VkrShareGroup *groups;
  uint32_t group_count;
  uint32_t group_capacity;
  VkrBakeryJson *needs_rebuild; /* [{scene, asset_id, name}] */
  uint32_t skipped;             /* Files in reserved folders. */
  bool8_t destination_exists;
} VkrShare;

/* The units a depot merge sends whole to Bakery's drivers (vkr_depot_merge.h,
   vkr_project_merge.c): a scene with its live inventory and overlay, and the
   project document. */
vkr_internal const char vkr_share_merge[] =
    "# Written by `vkr_bakery project` share_project. A depot merge sends "
    "each\n"
    "# unit both sides changed whole to Bakery's driver for it.\n"
    "unit scene .vkreditor/projects/*/scenes/*/scene.json inventory/*.json "
    "edits/*.json\n"
    "unit project .vkreditor/projects/*/project.json\n";

vkr_internal const char vkr_share_ignore[] =
    "# Written by `vkr_bakery project` share_project. A depot snapshot leaves\n"
    "# out each machine's own editor state and per-platform build output.\n"
    "\n"
    "# Bakery job requests, results and progress files.\n"
    ".vkreditor/jobs/\n"
    "# Job and editor logs.\n"
    ".vkreditor/logs/\n"
    "# Derived textures, thumbnails and the source index.\n"
    ".vkreditor/cache/\n"
    "# Build output of editor tools.\n"
    ".vkreditor/build/\n"
    "# The editor bundle each machine installs (fonts, default assets).\n"
    ".vkreditor/editor/\n"
    "# Project script libraries each machine compiles from Scripts/.\n"
    ".vkreditor/scripts/\n"
    "# The workspace identity; a clone without one gets a new one.\n"
    ".vkreditor/workspace.json\n"
    "# Per-machine layouts, panels, graphics and camera recall (ADR-069).\n"
    ".vkreditor/local/\n"
    "# Staged revisions of a running project job.\n"
    ".staging/\n"
    "# Runtime documents a scene opening prepares.\n"
    ".runtime/\n"
    "# Cooked textures are per platform (ASTC on Apple silicon, BC on "
    "x86-64);\n"
    "# each machine cooks its own from the source images.\n"
    "*.vkt\n"
    "# Process locks: project-<id>.lock, scene-<id>.lock,\n"
    "# workspace-<id>.lock and VkrEditorBakery.lock.\n"
    "*.lock\n";

// =============================================================================
// Collection
// =============================================================================

vkr_internal bool8_t vkr_share_begin_group(VkrShare *share, const char *id,
                                           const char *name) {
  VkrProjectJob *job = share->job;
  if (share->group_count == share->group_capacity) {
    const uint32_t capacity =
        share->group_capacity ? share->group_capacity * 2u : 8u;
    VkrShareGroup *groups = (VkrShareGroup *)arena_alloc(
        job->arena, capacity * sizeof(*groups), ARENA_MEMORY_TAG_ARRAY);
    if (!groups) {
      return vkr_project_fail(job, "Out of memory");
    }
    if (share->group_count) {
      MemCopy(groups, share->groups, share->group_count * sizeof(*groups));
    }
    share->groups = groups;
    share->group_capacity = capacity;
  }
  share->groups[share->group_count++] = (VkrShareGroup){
      .id = id, .name = name, .first = share->bundle.files.count};
  return true_v;
}

/* Adds `<owner>/<relative>`; a required file that is missing is reported. */
vkr_internal bool8_t vkr_share_add(VkrShare *share, const char *owner,
                                   const char *relative, bool8_t required) {
  char identity[VKR_PROJECT_PATH];
  if (!vkr_bakery_path_join(identity, sizeof(identity), owner, relative)) {
    return false_v;
  }
  return vkr_bundle_add(&share->bundle, identity, strlen(identity), required);
}

/* A record path that must stay below its owner. */
vkr_internal bool8_t vkr_share_managed(VkrShare *share, const char *value) {
  VkrProjectJob *job = share->job;
  const bool8_t failed_before = job->failed;
  if (!value || !value[0]) {
    return false_v;
  }
  if (!vkr_project_validate_managed_path(job, value)) {
    vkr_project_warn(job, "Skipped %s", job->error);
    vkr_project_forgive(job, failed_before);
    return false_v;
  }
  return true_v;
}

/* The live files of an asset inventory owned by `owner` (a project or scene
   identity): each record's artifacts, closure, source and import manifests,
   and the packed-texture marker of each build revision they name. */
vkr_internal void vkr_share_add_records(VkrShare *share, const char *owner,
                                        const VkrBakeryJson *records) {
  VkrProjectJob *job = share->job;
  if (!records || records->type != VKR_BAKERY_JSON_ARRAY) {
    return;
  }
  for (const VkrBakeryJson *record = records->first; record;
       record = record->next) {
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    for (const VkrBakeryJson *artifact = artifacts ? artifacts->first : NULL;
         artifact; artifact = artifact->next) {
      const char *path = vkr_project_json_text(artifact, "path");
      if (!vkr_share_managed(share, path)) {
        continue;
      }
      (void)vkr_share_add(share, owner, path, true_v);
      const char *revision = strncmp(path, "builds/", 7u) == 0 ? path + 7 : "";
      const char *end = strchr(revision, '/');
      if (end) {
        (void)vkr_share_add(share, owner,
                            vkr_project_printf(job, "builds/%.*s/%s",
                                               (int)(end - revision), revision,
                                               VKR_PROJECT_PACKED_MARKER),
                            false_v);
      }
    }
    const VkrBakeryJson *closure = vkr_bakery_json_get(record, "closure");
    for (const VkrBakeryJson *entry = closure ? closure->first : NULL; entry;
         entry = entry->next) {
      const char *path = vkr_project_printf(job, "%.*s", (int)entry->key.length,
                                            (const char *)entry->key.str);
      if (vkr_share_managed(share, path)) {
        (void)vkr_share_add(share, owner, path, true_v);
      }
    }
    const char *source = vkr_project_json_text(record, "source");
    if (source && vkr_share_managed(share, source)) {
      (void)vkr_share_add(share, owner, source, true_v);
    }
    const char *import_id = vkr_project_json_text(record, "import_id");
    if (import_id && vkr_project_identifier_valid(import_id)) {
      (void)vkr_share_add(share, owner,
                          vkr_project_printf(job, "imports/%s.json", import_id),
                          false_v);
      const char *revision = vkr_project_json_text(record, "import_revision");
      if (revision && vkr_project_identifier_valid(revision)) {
        (void)vkr_share_add(
            share, owner,
            vkr_project_printf(job, "imports/%s-%s.json", import_id, revision),
            false_v);
      }
    }
  }
}

/* The build revision folder (`builds/<revision>`) a record path names, or
   NULL unless the revision is an identifier. */
vkr_internal const char *vkr_share_build_of(VkrProjectJob *job,
                                            const char *path) {
  const char *end =
      path && strncmp(path, "builds/", 7u) == 0 ? strchr(path + 7, '/') : NULL;
  if (!end || end - (path + 7) != 36) {
    return NULL;
  }
  const char *revision = vkr_project_printf(job, "%.36s", path + 7);
  return vkr_project_identifier_valid(revision)
             ? vkr_project_printf(job, "builds/%s", revision)
             : NULL;
}

/* Per build revision the records name: its texture recipes and the source
   images its image recipes pack. A build holding cooked textures no recipe
   covers is listed in `needs_rebuild` with its mesh record, or its first
   record, so the editor can offer a Rebuild before sharing. */
vkr_internal void vkr_share_add_builds(VkrShare *share, const char *owner,
                                       const char *scene_id,
                                       const VkrBakeryJson *records) {
  VkrProjectJob *job = share->job;
  Arena *arena = job->arena;
  VkrBakeryJson *builds = vkr_bakery_json_object(arena);
  VkrProjectNodes chosen = {0};
  for (const VkrBakeryJson *record = records ? records->first : NULL; record;
       record = record->next) {
    const VkrBakeryJson *artifacts = vkr_bakery_json_get(record, "artifacts");
    const char *build = vkr_share_build_of(
        job,
        artifacts ? vkr_project_json_text(artifacts->first, "path") : NULL);
    if (!build) {
      continue;
    }
    const bool8_t mesh =
        vkr_bakery_json_is_string(vkr_bakery_json_get(record, "kind"), "mesh");
    const VkrBakeryJson *known = vkr_bakery_json_get(builds, build);
    if (!known) {
      vkr_bakery_json_set(arena, builds, build,
                          vkr_bakery_json_int(arena, chosen.count));
      (void)vkr_project_nodes_push(job, &chosen, (VkrBakeryJson *)record);
    } else if (mesh) {
      chosen.items[known->integer] = (VkrBakeryJson *)record;
    }
  }
  for (const VkrBakeryJson *build = builds->first; build; build = build->next) {
    const char *textures = vkr_project_printf(job, "%s/%s/textures", owner,
                                              (const char *)build->key.str);
    const char *host =
        vkr_project_printf(job, "%s/%s", job->workspace, textures);
    const bool8_t failed_before = job->failed;
    const VkrBakeryJson *recipes = NULL;
    if (!vkr_project_read_recipes(job, host, &recipes)) {
      vkr_project_warn(job, "Skipped %s", job->error);
      vkr_project_forgive(job, failed_before);
    }
    if (recipes) {
      (void)vkr_share_add(share, textures, VKR_PROJECT_RECIPES_FILE, true_v);
    }
    for (const VkrBakeryJson *entry = recipes ? recipes->first : NULL; entry;
         entry = entry->next) {
      const char *source = vkr_project_json_text(entry, "source");
      if (source && vkr_project_plain_name(source)) {
        (void)vkr_share_add(share, textures, source, true_v);
      } else if (source) {
        vkr_project_warn(job, "Skipped the recipe source '%s' in %s", source,
                         textures);
      }
    }
    if (vkr_project_textures_unrecorded(job, host, recipes)) {
      const VkrBakeryJson *record = chosen.items[build->integer];
      VkrBakeryJson *entry = vkr_bakery_json_object(arena);
      vkr_bakery_json_set(arena, entry, "scene",
                          scene_id ? vkr_bakery_json_cstr(arena, scene_id)
                                   : vkr_bakery_json_null(arena));
      vkr_bakery_json_set(
          arena, entry, "asset_id",
          vkr_bakery_json_clone(arena, vkr_bakery_json_get(record, "id")));
      const VkrBakeryJson *name = vkr_bakery_json_get(record, "name");
      vkr_bakery_json_set(arena, entry, "name",
                          name ? vkr_bakery_json_clone(arena, name)
                               : vkr_bakery_json_null(arena));
      vkr_bakery_json_append(share->needs_rebuild, entry);
    }
  }
}

/* Owner-relative `builds/...` and `sources/...` strings of a scene or World
   document, which the bundle walker resolves against the root or the
   document's own folder only. */
vkr_internal void vkr_share_add_owned_strings(VkrShare *share,
                                              const char *owner,
                                              const VkrBakeryJson *value) {
  if (!value) {
    return;
  }
  if (value->type == VKR_BAKERY_JSON_STRING) {
    const char *text = (const char *)value->string.str;
    const uint64_t length = value->string.length;
    const uint64_t skip =
        length > 2u && text[0] == '.' && text[1] == '/' ? 2u : 0u;
    if ((length - skip > 7u && MemCompare(text + skip, "builds/", 7u) == 0) ||
        (length - skip > 8u && MemCompare(text + skip, "sources/", 8u) == 0)) {
      (void)vkr_share_add(share, owner,
                          vkr_project_printf(share->job, "%.*s",
                                             (int)(length - skip), text + skip),
                          false_v);
    }
    return;
  }
  for (const VkrBakeryJson *child = value->first; child; child = child->next) {
    vkr_share_add_owned_strings(share, owner, child);
  }
}

/* Reads an optional document of the source workspace; NULL when it is
   missing or unreadable, which a warning records. */
vkr_internal VkrBakeryJson *
vkr_share_read(VkrShare *share, const char *identity, uint64_t limit) {
  VkrProjectJob *job = share->job;
  char path[VKR_PROJECT_PATH];
  if (!vkr_bakery_path_join(path, sizeof(path), job->workspace, identity) ||
      !vkr_bakery_is_file(path)) {
    return NULL;
  }
  const bool8_t failed_before = job->failed;
  VkrBakeryJson *value = vkr_project_load_json(job, path, limit);
  if (!value) {
    vkr_project_warn(job, "Cannot read %s: %s", identity, job->error);
    vkr_project_forgive(job, failed_before);
  }
  return value;
}

/* Every file below the project's Scripts folder except dot entries; script
   libraries build into the workspace's own scripts folder (ADR-079). */
vkr_internal bool8_t vkr_share_add_scripts(VkrShare *share) {
  VkrProjectJob *job = share->job;
  char scripts[VKR_PROJECT_PATH];
  (void)snprintf(scripts, sizeof(scripts), "%s/Scripts", job->project_root);
  if (!vkr_bakery_is_directory(scripts)) {
    return true_v;
  }
  VkrProjectStrings paths = {0};
  VKR_PROJECT_TRY(vkr_project_walk_files(job, scripts, &paths, true_v,
                                         "Share refuses links in Scripts"));
  for (uint32_t i = 0u; i < paths.count; ++i) {
    char relative[VKR_PROJECT_PATH];
    if (!vkr_bakery_path_relative(job->project_root, paths.items[i], relative,
                                  sizeof(relative)) ||
        strstr(relative, "/.")) {
      continue;
    }
    (void)vkr_share_add(share, share->project_identity, relative, true_v);
  }
  return true_v;
}

vkr_internal bool8_t vkr_share_collect_project(VkrShare *share,
                                               const VkrBakeryJson *project) {
  VKR_PROJECT_TRY(vkr_share_begin_group(share, NULL, NULL));
  const char *owner = share->project_identity;
  (void)vkr_share_add(share, owner, "project.json", true_v);
  static const char *const documents[] = {"game.json", "world.scene.json",
                                          "world.editor.json"};
  for (uint32_t i = 0u; i < ArrayCount(documents); ++i) {
    if (!vkr_share_add(share, owner, documents[i], false_v)) {
      continue;
    }
    const char *identity =
        vkr_project_printf(share->job, "%s/%s", owner, documents[i]);
    vkr_share_add_owned_strings(
        share, owner,
        vkr_share_read(share, identity, VKR_PROJECT_MAX_OVERLAY_BYTES));
  }
  vkr_share_add_records(share, owner, vkr_bakery_json_get(project, "assets"));
  vkr_share_add_builds(share, owner, NULL,
                       vkr_bakery_json_get(project, "assets"));
  VKR_PROJECT_TRY(vkr_share_add_scripts(share));
  vkr_bundle_close(&share->bundle);
  return true_v;
}

/* One listed scene's live set and its closure. */
vkr_internal bool8_t vkr_share_collect_scene(VkrShare *share, const char *id,
                                             const char *name) {
  VkrProjectJob *job = share->job;
  VKR_PROJECT_TRY(vkr_share_begin_group(share, id, name));
  VKR_PROJECT_TRY(
      vkr_project_progress(job, "Collecting files", -1.0, name ? name : id));
  const char *owner =
      vkr_project_printf(job, "%s/scenes/%s", share->project_identity, id);
  if (!vkr_share_add(share, owner, "scene.json", true_v)) {
    vkr_project_warn(job, "Scene %s has no scene.json", name ? name : id);
    return true_v;
  }
  const VkrBakeryJson *scene =
      vkr_share_read(share, vkr_project_printf(job, "%s/scene.json", owner),
                     VKR_PROJECT_MAX_DOCUMENT_BYTES);
  vkr_share_add_owned_strings(share, owner, scene);
  const char *overlay = vkr_project_json_text(scene, "edit_overlay");
  if (overlay && vkr_share_managed(share, overlay) &&
      vkr_share_add(share, owner, overlay, true_v)) {
    vkr_share_add_owned_strings(
        share, owner,
        vkr_share_read(share, vkr_project_printf(job, "%s/%s", owner, overlay),
                       VKR_PROJECT_MAX_OVERLAY_BYTES));
  }
  const char *inventory = vkr_project_json_text(scene, "inventory");
  if (inventory && vkr_share_managed(share, inventory) &&
      vkr_share_add(share, owner, inventory, true_v)) {
    const VkrBakeryJson *records = vkr_share_read(
        share, vkr_project_printf(job, "%s/%s", owner, inventory),
        VKR_PROJECT_MAX_INVENTORY_BYTES);
    vkr_share_add_records(share, owner, vkr_bakery_json_get(records, "assets"));
    vkr_share_add_builds(share, owner, id,
                         vkr_bakery_json_get(records, "assets"));
  }
  vkr_bundle_close(&share->bundle);
  return true_v;
}

// =============================================================================
// Destination
// =============================================================================

vkr_internal bool8_t vkr_share_count_entry(void *context, const char *name,
                                           bool8_t is_directory) {
  (void)name;
  (void)is_directory;
  *(uint32_t *)context += 1u;
  return true_v;
}

/* The destination: absolute, outside the source workspace and the content
   root, and absent or an empty folder. */
vkr_internal bool8_t vkr_share_destination(VkrShare *share) {
  VkrProjectJob *job = share->job;
  const char *raw = vkr_project_json_text(job->request, "destination");
  if (!raw || !vkr_bakery_path_is_absolute(raw)) {
    return vkr_project_fail(job, "share_project requires an absolute "
                                 "destination");
  }
  (void)vkr_project_resolve(raw, false_v, share->destination,
                            sizeof(share->destination));
  char repository[VKR_PROJECT_PATH];
  (void)vkr_project_resolve(vkr_content_root(), false_v, repository,
                            sizeof(repository));
  if (vkr_project_is_relative_to(share->destination, share->source) ||
      vkr_project_is_relative_to(share->source, share->destination)) {
    return vkr_project_fail(job,
                            "Share destination must be outside the "
                            "source workspace: %s",
                            share->destination);
  }
  if (vkr_project_is_relative_to(share->destination, job->legacy_root) ||
      vkr_project_is_relative_to(share->destination, repository)) {
    return vkr_project_fail(job,
                            "Share destination must be outside the "
                            "repository and content root: %s",
                            share->destination);
  }
  if (vkr_project_lstat_is_link(share->destination)) {
    return vkr_project_fail(job, "Share destination is a link: %s",
                            share->destination);
  }
  VkrBakeryStat info;
  if (vkr_bakery_stat(share->destination, &info) && info.exists) {
    uint32_t entries = 0u;
    if (!info.is_directory ||
        !vkr_bakery_list_directory(share->destination, vkr_share_count_entry,
                                   &entries) ||
        entries) {
      return vkr_project_fail(job,
                              "Share destination must be empty or "
                              "absent: %s",
                              share->destination);
    }
    share->destination_exists = true_v;
  }
  return true_v;
}

/* Whether an identity has a `.git` or `.vkrdepot` segment, which a working
   copy reserves for its version control; the share never writes one. */
vkr_internal bool8_t vkr_share_reserved(const char *identity) {
  static const char *const reserved[] = {".git", ".vkrdepot"};
  for (const char *segment = identity; segment && *segment;) {
    const char *end = strchr(segment, '/');
    const uint64_t length =
        end ? (uint64_t)(end - segment) : (uint64_t)strlen(segment);
    for (uint32_t i = 0u; i < ArrayCount(reserved); ++i) {
      const uint64_t size = strlen(reserved[i]);
      bool8_t same = length == size;
      for (uint64_t c = 0u; same && c < size; ++c) {
        const char a = segment[c];
        same = (a >= 'A' && a <= 'Z' ? (char)(a - 'A' + 'a') : a) ==
               reserved[i][c];
      }
      if (same) {
        return true_v;
      }
    }
    segment = end ? end + 1 : NULL;
  }
  return false_v;
}

/* Clones every collected file into `staging`; `.vkreditor`-side mounts keep
   their place below `.vkreditor/`. */
vkr_internal bool8_t vkr_share_copy(VkrShare *share, const char *staging,
                                    uint64_t *out_bytes) {
  VkrProjectJob *job = share->job;
  const VkrBundleSet *files = &share->bundle.files;
  uint64_t *sizes = (uint64_t *)arena_alloc(
      job->arena, (files->count ? files->count : 1u) * sizeof(*sizes),
      ARENA_MEMORY_TAG_ARRAY);
  if (!sizes) {
    return vkr_project_fail(job, "Out of memory");
  }
  uint64_t total = 0u;
  for (uint32_t i = 0u; i < files->count; ++i) {
    char path[VKR_PROJECT_PATH];
    VkrBakeryStat info = {0};
    sizes[i] = 0u;
    if (!vkr_share_reserved(files->items[i]) &&
        vkr_bundle_source(&share->bundle, files->items[i], path,
                          sizeof(path)) &&
        vkr_bakery_stat(path, &info)) {
      sizes[i] = info.size;
      total += info.size;
    }
  }
  uint64_t done = 0u;
  float64_t reported = -1.0;
  for (uint32_t i = 0u; i < files->count; ++i) {
    const char *identity = files->items[i];
    const float64_t now = vkr_bakery_monotonic_seconds();
    if (now - reported >= VKR_SHARE_PROGRESS_SECONDS) {
      reported = now;
      VKR_PROJECT_TRY(vkr_project_progress(
          job, "Copying files",
          total ? (float64_t)done / (float64_t)total : 0.0, identity));
    }
    if (vkr_share_reserved(identity)) {
      vkr_project_warn(job, "Skipped %s: a working copy reserves its folder",
                       identity);
      share->skipped += 1u;
      continue;
    }
    char source[VKR_PROJECT_PATH];
    uint32_t mount = 0u;
    if (!vkr_bundle_source_mount(&share->bundle, identity, source,
                                 sizeof(source), &mount)) {
      return vkr_project_fail(job, "Shared file disappeared: %s", identity);
    }
    const bool8_t workspace_side = mount == VKR_SHARE_MOUNT_PROJECTS ||
                                   mount == VKR_SHARE_MOUNT_WORKSPACE_ASSETS;
    char destination[VKR_PROJECT_PATH];
    char parent[VKR_PROJECT_PATH];
    if ((uint32_t)snprintf(destination, sizeof(destination), "%s/%s%s", staging,
                           workspace_side ? ".vkreditor/" : "",
                           identity) >= sizeof(destination)) {
      return vkr_project_fail(job, "Path too long: %s", identity);
    }
    vkr_bakery_path_parent(parent, sizeof(parent), destination);
    VKR_PROJECT_TRY(vkr_project_make_dirs(job, parent));
    if (!vkr_bakery_clone_or_copy(source, destination)) {
      return vkr_project_fail(job, "Cannot copy %s", source);
    }
    done += sizes[i];
    for (uint32_t g = share->group_count; g > 0u; --g) {
      VkrShareGroup *group = &share->groups[g - 1u];
      if (i >= group->first) {
        group->files += 1u;
        group->bytes += sizes[i];
        break;
      }
    }
  }
  *out_bytes = total;
  return true_v;
}

// =============================================================================
// Result
// =============================================================================

vkr_internal VkrBakeryJson *vkr_share_list(VkrShare *share,
                                           const VkrBundleSet *set) {
  Arena *arena = share->job->arena;
  VkrBakeryJson *list = vkr_bakery_json_array(arena);
  for (uint32_t i = 0u; i < set->count && i < VKR_SHARE_LIST_LIMIT; ++i) {
    vkr_bakery_json_append(list, vkr_bakery_json_cstr(arena, set->items[i]));
  }
  return list;
}

/* Whether a recipe in its folder's VKR_PROJECT_RECIPES_FILE covers the
   cooked texture `path`, so another host cooks it again; `folders` caches
   each folder's recipes (or null). */
vkr_internal bool8_t vkr_share_recipe_covers(VkrShare *share,
                                             VkrBakeryJson *folders,
                                             const char *path) {
  VkrProjectJob *job = share->job;
  Arena *arena = job->arena;
  char folder[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(folder, sizeof(folder), path);
  const VkrBakeryJson *recipes = vkr_bakery_json_get(folders, folder);
  if (!recipes) {
    const bool8_t failed_before = job->failed;
    const VkrBakeryJson *read = NULL;
    if (!vkr_project_read_recipes(job, folder, &read)) {
      vkr_project_forgive(job, failed_before);
    }
    VkrBakeryJson *cached =
        read ? vkr_bakery_json_clone(arena, read) : vkr_bakery_json_null(arena);
    vkr_bakery_json_set(arena, folders, folder, cached);
    recipes = cached;
  }
  return recipes->type == VKR_BAKERY_JSON_OBJECT &&
         vkr_bakery_json_get(recipes, vkr_bakery_path_name(path)) != NULL;
}

vkr_internal VkrBakeryJson *vkr_share_result(VkrShare *share, uint64_t bytes) {
  VkrProjectJob *job = share->job;
  Arena *arena = job->arena;
  const VkrBundle *bundle = &share->bundle;
  /* Cooked textures without a source image and without a recipe; those a
     recipe covers are cooked again where the share arrives. */
  VkrBakeryJson *unrecorded = vkr_bakery_json_array(arena);
  VkrBakeryJson *folders = vkr_bakery_json_object(arena);
  uint32_t unrecorded_count = 0u;
  uint32_t recookable = 0u;
  uint64_t cooked_bytes = 0u;
  for (uint32_t i = 0u; i < bundle->cooked.count; ++i) {
    char path[VKR_PROJECT_PATH];
    VkrBakeryStat info = {0};
    if (!vkr_bundle_source(bundle, bundle->cooked.items[i], path,
                           sizeof(path))) {
      continue;
    }
    if (vkr_share_recipe_covers(share, folders, path)) {
      recookable += 1u;
      continue;
    }
    if (vkr_bakery_stat(path, &info)) {
      cooked_bytes += info.size;
    }
    if (unrecorded_count++ < VKR_SHARE_LIST_LIMIT) {
      vkr_bakery_json_append(
          unrecorded, vkr_bakery_json_cstr(arena, bundle->cooked.items[i]));
    }
  }
  if (bundle->missing_references.count) {
    vkr_project_warn(job, "%u referenced files are missing, first %s",
                     bundle->missing_references.count,
                     bundle->missing_references.items[0]);
  }
  if (unrecorded_count) {
    vkr_project_warn(job,
                     "%u cooked textures (.vkt, %.1f MiB) have neither a "
                     "source image nor a recipe and were copied; "
                     ".vkrdepotignore leaves them out of a depot snapshot",
                     unrecorded_count, (float64_t)cooked_bytes / 1048576.0);
  }
  if (share->needs_rebuild->count) {
    vkr_project_warn(job,
                     "%u builds predate texture recipes; rebuild them so "
                     "another host can cook their textures",
                     share->needs_rebuild->count);
  }
  const char *workspace_root =
      vkr_project_printf(job, "%s/.vkreditor", share->destination);
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "version",
                      vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
  vkr_bakery_json_set(arena, result, "status",
                      vkr_bakery_json_cstr(arena, "complete"));
  vkr_bakery_json_set(arena, result, "destination",
                      vkr_bakery_json_cstr(arena, share->destination));
  vkr_bakery_json_set(arena, result, "workspace_root",
                      vkr_bakery_json_cstr(arena, workspace_root));
  vkr_bakery_json_set(
      arena, result, "project_path",
      vkr_bakery_json_cstr(arena, vkr_project_printf(job, "%s/%s/project.json",
                                                     workspace_root,
                                                     share->project_identity)));
  vkr_bakery_json_set(
      arena, result, "files",
      vkr_bakery_json_int(arena, bundle->files.count - share->skipped));
  vkr_bakery_json_set(arena, result, "bytes",
                      vkr_bakery_json_int(arena, (int64_t)bytes));
  VkrBakeryJson *scenes = vkr_bakery_json_array(arena);
  for (uint32_t g = 0u; g < share->group_count; ++g) {
    const VkrShareGroup *group = &share->groups[g];
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "files",
                        vkr_bakery_json_int(arena, group->files));
    vkr_bakery_json_set(arena, entry, "bytes",
                        vkr_bakery_json_int(arena, (int64_t)group->bytes));
    if (!group->id) {
      vkr_bakery_json_set(arena, result, "project", entry);
      continue;
    }
    vkr_bakery_json_set(arena, entry, "id",
                        vkr_bakery_json_cstr(arena, group->id));
    vkr_bakery_json_set(arena, entry, "name",
                        group->name ? vkr_bakery_json_cstr(arena, group->name)
                                    : vkr_bakery_json_null(arena));
    vkr_bakery_json_append(scenes, entry);
  }
  vkr_bakery_json_set(arena, result, "scenes", scenes);
  vkr_bakery_json_set(
      arena, result, "missing_count",
      vkr_bakery_json_int(arena, bundle->missing_references.count));
  vkr_bakery_json_set(arena, result, "missing",
                      vkr_share_list(share, &bundle->missing_references));
  vkr_bakery_json_set(arena, result, "cooked_without_source_count",
                      vkr_bakery_json_int(arena, unrecorded_count));
  vkr_bakery_json_set(arena, result, "cooked_without_source_bytes",
                      vkr_bakery_json_int(arena, (int64_t)cooked_bytes));
  vkr_bakery_json_set(arena, result, "cooked_without_source", unrecorded);
  vkr_bakery_json_set(arena, result, "recookable_textures",
                      vkr_bakery_json_int(arena, recookable));
  vkr_bakery_json_set(arena, result, "needs_rebuild", share->needs_rebuild);
  vkr_bakery_json_set(arena, result, "warnings", job->warnings);
  return result;
}

// =============================================================================
// Operation
// =============================================================================

/* A share of the job's project, with the closure of its live files
   collected: each scene's, then the project's documents and scripts.
   `destination` checks the request's destination first. NULL, with the
   job failed, otherwise. */
vkr_internal VkrShare *vkr_share_collect(VkrProjectJob *job,
                                         bool8_t destination) {
  VkrShare *share = (VkrShare *)arena_alloc(job->arena, sizeof(VkrShare),
                                            ARENA_MEMORY_TAG_STRUCT);
  if (!share) {
    vkr_project_fail(job, "Out of memory");
    return NULL;
  }
  MemZero(share, sizeof(*share));
  share->job = job;
  share->needs_rebuild = vkr_bakery_json_array(job->arena);
  if (strcmp(vkr_bakery_path_name(job->workspace), ".vkreditor") != 0) {
    vkr_project_fail(job, "%s requires a workspace_root named .vkreditor",
                     job->operation);
    return NULL;
  }
  vkr_bakery_path_parent(share->source, sizeof(share->source), job->workspace);
  if (destination && !vkr_share_destination(share)) {
    return NULL;
  }
  VkrBakeryJson *project =
      vkr_project_load_json(job, job->project_path, VKR_PROJECT_MAX_JSON_BYTES);
  if (!project) {
    return NULL;
  }
  (void)snprintf(share->project_identity, sizeof(share->project_identity),
                 "projects/%s", vkr_bakery_path_name(job->project_root));
  char content[VKR_PROJECT_PATH];
  (void)vkr_project_resolve(job->legacy_root, false_v, content,
                            sizeof(content));
  (void)snprintf(share->mount_directories[VKR_SHARE_MOUNT_PROJECTS],
                 VKR_PROJECT_PATH, "%s/projects", job->workspace);
  (void)snprintf(share->mount_directories[VKR_SHARE_MOUNT_OVERLAY],
                 VKR_PROJECT_PATH, "%s/assets", share->source);
  (void)snprintf(share->mount_directories[VKR_SHARE_MOUNT_CONTENT],
                 VKR_PROJECT_PATH, "%s/assets", content);
  (void)snprintf(share->mount_directories[VKR_SHARE_MOUNT_WORKSPACE_ASSETS],
                 VKR_PROJECT_PATH, "%s/assets", job->workspace);
  static const char *const prefixes[VKR_SHARE_MOUNT_COUNT] = {
      "projects/", "assets/", "assets/", "assets/"};
  share->cli.config = *job->config;
  share->bundle.cli = &share->cli;
  share->bundle.arena = job->arena;
  share->bundle.sources = true_v;
  share->bundle.mount_count = VKR_SHARE_MOUNT_COUNT;
  for (uint32_t i = 0u; i < VKR_SHARE_MOUNT_COUNT; ++i) {
    share->bundle.mounts[i] = (VkrBundleMount){
        .prefix = prefixes[i], .directory = share->mount_directories[i]};
  }

  const VkrBakeryJson *scenes = vkr_bakery_json_get(project, "scenes");
  for (const VkrBakeryJson *scene = scenes ? scenes->first : NULL; scene;
       scene = scene->next) {
    const char *id = vkr_project_json_text(scene, "id");
    if (!id || !vkr_project_identifier_valid(id)) {
      vkr_project_warn(job, "Skipped a scene entry without a valid id");
      continue;
    }
    if (!vkr_share_collect_scene(share, id,
                                 vkr_project_json_text(scene, "name"))) {
      vkr_bundle_free(&share->bundle);
      return NULL;
    }
  }
  if (!vkr_project_progress(job, "Collecting files", -1.0, "Project") ||
      !vkr_share_collect_project(share, project)) {
    vkr_bundle_free(&share->bundle);
    return NULL;
  }
  return share;
}

VkrBakeryJson *vkr_project_share(VkrProjectJob *job) {
  VkrShare *share = vkr_share_collect(job, true_v);
  if (!share) {
    return NULL;
  }
  VkrBakeryJson *result = NULL;
  uint64_t bytes = 0u;
  char parent[VKR_PROJECT_PATH];
  char staging[VKR_PROJECT_PATH];
  char ignore[VKR_PROJECT_PATH];
  vkr_bakery_path_parent(parent, sizeof(parent), share->destination);
  if (!vkr_project_make_dirs(job, parent) ||
      !vkr_project_mkdtemp(
          job, parent,
          vkr_project_printf(job, ".%s.share-",
                             vkr_bakery_path_name(share->destination)),
          staging)) {
    goto cleanup;
  }
  /* The job's release removes the staging folder unless it is published. */
  (void)snprintf(job->stage, sizeof(job->stage), "%s", staging);
  (void)snprintf(ignore, sizeof(ignore), "%s/.vkrdepotignore", staging);
  if (!vkr_share_copy(share, staging, &bytes) ||
      !vkr_project_write_text(job, ignore, vkr_share_ignore,
                              sizeof(vkr_share_ignore) - 1u) ||
      !vkr_project_write_text(
          job, vkr_project_printf(job, "%s/.vkrdepotmerge", staging),
          vkr_share_merge, sizeof(vkr_share_merge) - 1u) ||
      !vkr_project_progress(job, "Publishing", 1.0, share->destination)) {
    goto cleanup;
  }
  if (share->destination_exists &&
      !vkr_bakery_remove_tree(share->destination)) {
    vkr_project_fail(job, "Cannot replace the empty destination %s",
                     share->destination);
    goto cleanup;
  }
  if (!vkr_bakery_rename(staging, share->destination, false_v)) {
    vkr_project_fail(job, "Cannot publish the share at %s", share->destination);
    goto cleanup;
  }
  job->stage[0] = 0;
  result = vkr_share_result(share, bytes);

cleanup:
  vkr_bundle_free(&share->bundle);
  return result;
}

VkrBakeryJson *vkr_project_dependencies(VkrProjectJob *job) {
  Arena *arena = job->arena;
  /* Engine content stays where it is; the rest of what the content root
     serves this project is the project's to hold. */
  if (!vkr_vfs_engine_content_load()) {
    vkr_project_fail(job, "Engine content is unknown: git ls-files failed in "
                          "the content root");
    return NULL;
  }
  bool8_t adopt = false_v;
  const VkrBakeryJson *requested = vkr_bakery_json_get(job->request, "adopt");
  if (requested && requested->type != VKR_BAKERY_JSON_BOOL) {
    vkr_project_fail(job, "adopt must be boolean");
    return NULL;
  }
  adopt = requested && requested->boolean;
  VkrShare *share = vkr_share_collect(job, false_v);
  if (!share) {
    return NULL;
  }
  VkrBakeryJson *untracked = vkr_bakery_json_array(arena);
  uint64_t untracked_bytes = 0u;
  uint32_t adopted = 0u;
  bool8_t ok = true_v;
  const VkrBundleSet *files = &share->bundle.files;
  for (uint32_t i = 0u; ok && i < files->count; ++i) {
    const char *identity = files->items[i];
    char source[VKR_PROJECT_PATH];
    uint32_t mount = 0u;
    if (!vkr_bundle_source_mount(&share->bundle, identity, source,
                                 sizeof(source), &mount) ||
        mount != VKR_SHARE_MOUNT_CONTENT ||
        vkr_vfs_is_engine_content(identity)) {
      continue;
    }
    VkrBakeryStat info = {0};
    (void)vkr_bakery_stat(source, &info);
    untracked_bytes += info.size;
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "path",
                        vkr_bakery_json_cstr(arena, identity));
    vkr_bakery_json_set(arena, entry, "bytes",
                        vkr_bakery_json_int(arena, (int64_t)info.size));
    vkr_bakery_json_append(untracked, entry);
    if (!adopt) {
      continue;
    }
    /* Into the working copy's own assets/, which the overlay serves first,
       so the project's next commit carries it. */
    char destination[VKR_PROJECT_PATH];
    char parent[VKR_PROJECT_PATH];
    if ((uint32_t)snprintf(destination, sizeof(destination), "%s/%s",
                           share->source, identity) >= sizeof(destination)) {
      ok = vkr_project_fail(job, "Path too long: %s", identity);
      break;
    }
    if (vkr_bakery_is_file(destination)) {
      continue;
    }
    vkr_bakery_path_parent(parent, sizeof(parent), destination);
    ok = vkr_project_make_dirs(job, parent) &&
         (vkr_bakery_clone_or_copy(source, destination) ||
          vkr_project_fail(job, "Cannot copy %s", source));
    adopted += ok ? 1u : 0u;
  }
  VkrBakeryJson *result = NULL;
  if (ok) {
    result = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, result, "version",
                        vkr_bakery_json_int(arena, VKR_PROJECT_VERSION));
    vkr_bakery_json_set(arena, result, "status",
                        vkr_bakery_json_cstr(arena, "complete"));
    vkr_bakery_json_set(arena, result, "untracked", untracked);
    vkr_bakery_json_set(arena, result, "untracked_count",
                        vkr_bakery_json_int(arena, untracked->count));
    vkr_bakery_json_set(arena, result, "untracked_bytes",
                        vkr_bakery_json_int(arena, (int64_t)untracked_bytes));
    vkr_bakery_json_set(arena, result, "adopted",
                        vkr_bakery_json_int(arena, adopted));
    vkr_bakery_json_set(
        arena, result, "missing",
        vkr_share_list(share, &share->bundle.missing_references));
    vkr_bakery_json_set(
        arena, result, "missing_count",
        vkr_bakery_json_int(arena, share->bundle.missing_references.count));
  }
  vkr_bundle_free(&share->bundle);
  return result;
}
