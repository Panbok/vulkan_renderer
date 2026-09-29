#pragma once

#include "../vkr_bakery_internal.h"
#include "vkr_vkt_packer.h"

/* Managed project operations (ADR-069, ADR-076), ported from the former
 * tools/editor_project_jobs.py. Request and result documents, staging rules,
 * inventory revisions, atomic publication and cleanup keep that script's
 * contract; this file set is its only owner.
 *
 * Every function returns false after recording the first failure message on
 * the job; callers propagate with VKR_PROJECT_TRY. Job-lifetime strings and
 * documents live in the job arena and are never freed individually. */

#define VKR_PROJECT_VERSION 1
#define VKR_PROJECT_MAX_JSON_BYTES (16ull * 1024ull * 1024ull)
#define VKR_PROJECT_MAX_DOCUMENT_BYTES (1024ull * 1024ull)
#define VKR_PROJECT_SCENE_VERSION 5
#define VKR_PROJECT_MAX_INVENTORY_BYTES (16ull * 1024ull * 1024ull)
/* The project manifest carries the project's asset inventory, so it takes
   the inventory bound rather than the scene document bound. */
#define VKR_PROJECT_MAX_MANIFEST_BYTES VKR_PROJECT_MAX_INVENTORY_BYTES
#define VKR_PROJECT_MAX_IMPORT_BYTES (8ull * 1024ull * 1024ull * 1024ull)
#define VKR_PROJECT_MAX_IMPORT_FILES 16384u
#define VKR_PROJECT_MAX_MODEL_BYTES (256ull * 1024ull * 1024ull)
#define VKR_PROJECT_MAX_ENTITIES 65536u
/* bake_diffuse_volume status: no closed-room cell, so no volume published. */
#define VKR_PROJECT_DIFFUSE_NO_ROOM_CELLS 3
#define VKR_PROJECT_UNREFERENCED_GRACE_SECONDS (24 * 60 * 60)
#define VKR_PROJECT_ORPHAN_GRACE_SECONDS (60 * 60)
#define VKR_PROJECT_JOB_RETENTION_SECONDS (7 * 24 * 60 * 60)
#define VKR_PROJECT_PATH VKR_BAKERY_PATH_CAPACITY

#define VKR_PROJECT_TRY(expression)                                            \
  do {                                                                         \
    if (!(expression)) {                                                       \
      return false_v;                                                          \
    }                                                                          \
  } while (0)

/* Growable list of arena-owned strings. */
typedef struct VkrProjectStrings {
  const char **items;
  uint32_t count;
  uint32_t capacity;
} VkrProjectStrings;

/* Open-addressing string set/map for large digest sets; malloc-owned. */
typedef struct VkrProjectSet {
  char **keys;
  const char **values;
  uint32_t count;
  uint32_t capacity;
} VkrProjectSet;

/* Growable list of borrowed JSON nodes. */
typedef struct VkrProjectNodes {
  VkrBakeryJson **items;
  uint32_t count;
  uint32_t capacity;
} VkrProjectNodes;

typedef struct VkrProjectJob {
  const VkrBakeryConfig *config;
  Arena *arena;
  VkrBakeryJson *request;
  const char *operation;
  char result_path[VKR_PROJECT_PATH];
  char progress_path[VKR_PROJECT_PATH];
  char workspace[VKR_PROJECT_PATH];
  char project_path[VKR_PROJECT_PATH];
  char project_root[VKR_PROJECT_PATH];
  char final_path[VKR_PROJECT_PATH]; /* project/scenes/<scene_id> */
  char legacy_root[VKR_PROJECT_PATH];
  char stage[VKR_PROJECT_PATH]; /* Empty when no staging directory. */
  char runtime_directory[VKR_PROJECT_PATH];
  char generated_root[VKR_PROJECT_PATH];
  const char *scene_id; /* NULL when the operation has no scene. */
  const char *asset_scope;
  bool8_t read_only;
  /* `portable`: a read-only preparation for a package
     (docs/proposals/project-packaging.md). Lowered documents name content
     identities, `project/...` and `editor/...`, instead of absolute paths. */
  bool8_t portable;
  bool8_t final_owned;
  bool8_t cancelled;
  bool8_t failed;
  char error[2048];
  uint64_t bytes_copied;
  uint32_t files_copied;
  VkrBakeryJson *warnings; /* Array of strings. */
  VkrBakeryJson *assets;   /* Array of records being edited. */
  VkrBakeryJson *sources;  /* import_id -> source manifest. */
  VkrBakeryJson *pending_project_assets;
  VkrBakeryJson *pending_default_font;
  VkrProjectStrings published_builds;
  VkrProjectStrings project_builds;
  VkrBakeryJson *inspections;    /* mesh content digest -> inspection. */
  VkrBakeryJson *collision_seen; /* "path|digest" -> true. */
  VkrBakeryJson *texture_seeds;  /* source digest -> seed .vkt path. */
  VkrBakeryJson *asset_names;    /* path -> display name. */
  VkrBakeryJson *source_names;   /* digest -> display name. */
  /* `<cache>/index`: a source whose size, modification time and identity
     are unchanged since it was last hashed is not read again. Opened on
     first use and closed when the job ends. */
  VkrBakeryIndex *index;
  /* Imports may encode textures at the preview tier (`texture_tier`);
     `finalize_textures` rebuilds those assets at the final tier. */
  bool8_t texture_preview;
  /* Block encoding of derived textures (`texture_encoding`,
     `texture_encode_speed`). */
  VkrVktEncoding texture_encoding;
  /* The deferred tier imports meshes whose materials name no textures, so a
     scene opens at once; `finalize_textures` adds them at the final tier. */
  bool8_t texture_deferred;
  /* `ready_log`: the cook and the job append one JSON line per finished
     material, which the editor applies before the job publishes. */
  const char *ready_log;
  /* `material_priority`: material names the cook writes first. */
  const VkrBakeryJson *material_priority;
} VkrProjectJob;

/* Written into a revision by vkr_project_index_bundle once every material
   names packed textures; a revision without it is scanned. */
#define VKR_PROJECT_PACKED_MARKER ".textures-packed"

#if defined(__APPLE__) && defined(__aarch64__)
#define VKR_PROJECT_NATIVE_ASTC true_v
#else
#define VKR_PROJECT_NATIVE_ASTC false_v
#endif

// =============================================================================
// Failure, cancellation and progress
// =============================================================================

bool8_t vkr_project_fail(VkrProjectJob *job, const char *format, ...);
bool8_t vkr_project_check_cancel(VkrProjectJob *job);
bool8_t vkr_project_progress(VkrProjectJob *job, const char *stage,
                             float64_t fraction, const char *detail);
void vkr_project_warn(VkrProjectJob *job, const char *format, ...);

// =============================================================================
// Strings and collections
// =============================================================================

const char *vkr_project_strdup(VkrProjectJob *job, const char *text);
const char *vkr_project_printf(VkrProjectJob *job, const char *format, ...);
const char *vkr_project_json_text(const VkrBakeryJson *object, const char *key);
/** Copy of `text` without leading and trailing ASCII whitespace. */
const char *vkr_project_strip_text(VkrProjectJob *job, const char *text);
bool8_t vkr_project_strings_push(VkrProjectJob *job, VkrProjectStrings *list,
                                 const char *text);
bool8_t vkr_project_nodes_push(VkrProjectJob *job, VkrProjectNodes *list,
                               VkrBakeryJson *node);
/** Python truthiness of a JSON value; a missing value is false. */
bool8_t vkr_project_truthy(const VkrBakeryJson *value);
/** Reads an INT or FLOAT value (never a bool). */
bool8_t vkr_project_number(const VkrBakeryJson *value, float64_t *out);
/** Reads an INT value (never a bool or float). */
bool8_t vkr_project_integer(const VkrBakeryJson *value, int64_t *out);
bool8_t vkr_project_set_add(VkrProjectSet *set, const char *key,
                            const char *value);
const char *vkr_project_set_get(const VkrProjectSet *set, const char *key);
bool8_t vkr_project_set_has(const VkrProjectSet *set, const char *key);
void vkr_project_set_free(VkrProjectSet *set);
/** Collects sorted child names of `directory` into the job arena. */
bool8_t vkr_project_list(VkrProjectJob *job, const char *directory,
                         VkrProjectStrings *out_names);
/** Every regular file below `directory`, sorted, as absolute paths. */
bool8_t vkr_project_walk_files(VkrProjectJob *job, const char *directory,
                               VkrProjectStrings *out_paths,
                               bool8_t reject_links, const char *link_error);

// =============================================================================
// Identity and hashing
// =============================================================================

void vkr_project_uuid4(char out[37]);
bool8_t vkr_project_identifier_valid(const char *value);
bool8_t vkr_project_identifier(VkrProjectJob *job, const VkrBakeryJson *value,
                               const char **out);
/** Mesh tool arguments selecting the job's texture tier and encoding (four
 * entries); returns the count appended to `arguments`. */
uint32_t vkr_project_tier_arguments(const VkrProjectJob *job,
                                    const char **arguments);
/** Tool spelling of the job's texture encoding: "uastc", "astc",
 * "astc-fast", "bc" or "bc-fast". */
const char *vkr_project_texture_encoding_name(const VkrProjectJob *job);
/** After a finalize publishes `records`, appends a ready record for every
 * material of theirs the cook did not record, from its published file under
 * `root`. Best effort: a failure costs the editor only a reopen. */
void vkr_project_record_ready_remaining(VkrProjectJob *job,
                                        VkrBakeryJson *const *records,
                                        uint32_t count, const char *root);
/** Name suffix of job-packed textures at the job's tier and encoding: "",
 * "-preview", "-astc", "-astc-preview", "-astc-fast", "-astc-fast-preview",
 * "-bc", "-bc-preview", "-bc-fast" or "-bc-fast-preview". */
const char *vkr_project_texture_suffix(const VkrProjectJob *job);
/** Records of `records` (an asset array) still at the preview or deferred
 * tier. */
int64_t vkr_project_count_preview(const VkrBakeryJson *records);
/** Marks a mesh record imported at the preview tier. */
void vkr_project_mark_tier(VkrProjectJob *job, VkrBakeryJson *record);
/** The job's source index, or NULL when it cannot be opened. */
VkrBakeryIndex *vkr_project_index(VkrProjectJob *job);
bool8_t vkr_project_digest(VkrProjectJob *job, const char *path,
                           char out[VKR_BAKERY_SHA256_HEX]);
void vkr_project_fingerprint(const uint8_t *data, uint64_t length,
                             char out[17]);
/** Job.mesh_identity: FNV-1a continuation of `seed` over a mesh hash. */
void vkr_project_mesh_identity(const char *seed, const char *mesh_hash,
                               char out[17]);

// =============================================================================
// Paths
// =============================================================================

bool8_t vkr_project_is_relative_to(const char *path, const char *root);
/** os.path.relpath of absolute normalized paths. */
bool8_t vkr_project_relpath(const char *path, const char *start, char *out,
                            uint32_t capacity);
/** Path.resolve: strict fails when missing; otherwise resolves the longest
 * existing prefix and appends the rest lexically. */
bool8_t vkr_project_resolve(const char *path, bool8_t strict, char *out,
                            uint32_t capacity);
bool8_t vkr_project_validate_managed_path(VkrProjectJob *job,
                                          const char *value);
bool8_t vkr_project_managed_reference(VkrProjectJob *job, const char *path,
                                      const char *owner, const char **out);
bool8_t vkr_project_contained(VkrProjectJob *job, const char *root,
                              const char *value, bool8_t must_exist, char *out);
/** Package content identity of a resolved path inside the project
 * (`project/<path>`) or the editor bundle (`editor/<path>`). A path anywhere
 * else cannot be packaged and fails the job. */
bool8_t vkr_project_portable_identity(VkrProjectJob *job, const char *path,
                                      char *out);
bool8_t vkr_project_source_file(VkrProjectJob *job, const char *value,
                                char *out);
bool8_t vkr_project_legacy_source(VkrProjectJob *job, const char *value,
                                  const char *origin, const char *legacy_root,
                                  char *out);
void vkr_project_gltf_texture_source(const char *value, const char *origin,
                                     const char *legacy_root, char *out);
bool8_t vkr_project_gltf_uri_path(VkrProjectJob *job, const char *uri,
                                  char *out, uint32_t capacity);
/** OBJ/MTL tokens: whitespace, quotes and comments. */
bool8_t vkr_project_model_tokens(VkrProjectJob *job, const char *line,
                                 VkrProjectStrings *out_tokens);
void vkr_project_suffix_lower(const char *path, char *out, uint32_t capacity);
void vkr_project_stem(const char *path, char *out, uint32_t capacity);

// =============================================================================
// Files
// =============================================================================

/** str.splitlines() for \n, \r\n and \r. */
bool8_t vkr_project_split_lines(VkrProjectJob *job, const char *text,
                                VkrProjectStrings *out);
/** Writes '\n'.join(lines) + '\n'. */
bool8_t vkr_project_write_lines(VkrProjectJob *job, const char *path,
                                const VkrProjectStrings *lines);
/** "key=value" through str.partition with both sides stripped. */
bool8_t vkr_project_partition_line(const char *line, char *key,
                                   uint32_t key_capacity, char *value,
                                   uint32_t capacity);

VkrBakeryJson *vkr_project_load_json(VkrProjectJob *job, const char *path,
                                     uint64_t limit);
bool8_t vkr_project_atomic_json(VkrProjectJob *job, const char *path,
                                const VkrBakeryJson *value);
bool8_t vkr_project_validate_document(VkrProjectJob *job,
                                      const VkrBakeryJson *value,
                                      uint64_t limit);
bool8_t vkr_project_read_text(VkrProjectJob *job, const char *path,
                              uint64_t limit, const char **out,
                              uint64_t *out_length);
bool8_t vkr_project_write_text(VkrProjectJob *job, const char *path,
                               const char *text, uint64_t length);
bool8_t vkr_project_make_dirs(VkrProjectJob *job, const char *path);
bool8_t vkr_project_copy_file(VkrProjectJob *job, const char *source,
                              const char *destination);
bool8_t vkr_project_copy_blob(VkrProjectJob *job, const char *source,
                              const char *directory, char *out);
/** Places the blobs vkr_project_copy_blob would copy from `sources` (resolved
 * paths, already hashed) into `directory` on parallel workers, so the later
 * vkr_project_copy_blob calls find them. Sources it cannot name or place are
 * left to those calls, which report their errors. */
bool8_t vkr_project_prefetch_blobs(VkrProjectJob *job,
                                   const char *const *sources, uint32_t count,
                                   const char *directory);
bool8_t vkr_project_mkdtemp(VkrProjectJob *job, const char *parent,
                            const char *prefix, char *out);
bool8_t vkr_project_publish_directory(VkrProjectJob *job, const char *source,
                                      const char *destination);
/** Removes a file or tree without following links; returns bytes freed. */
uint64_t vkr_project_remove_tree(const char *path);
bool8_t vkr_project_older_than(const char *path, int64_t seconds, int64_t now);
bool8_t vkr_project_lstat_is_link(const char *path);
bool8_t vkr_project_exists(const char *path);

// =============================================================================
// Managed documents
// =============================================================================

bool8_t vkr_project_document_entity_ids(VkrProjectJob *job,
                                        const VkrBakeryJson *entities,
                                        VkrBakeryJson **out_ids);
bool8_t vkr_project_document_to_internal(VkrProjectJob *job,
                                         VkrBakeryJson *scene);
bool8_t vkr_project_document_from_internal(VkrProjectJob *job,
                                           VkrBakeryJson *document);
bool8_t vkr_project_remap_overlay_indices(VkrProjectJob *job,
                                          VkrBakeryJson *overlay,
                                          const VkrBakeryJson *entities);
VkrBakeryJson *vkr_project_read_managed_scene(VkrProjectJob *job,
                                              const char *path);
bool8_t vkr_project_validate_managed_scene(VkrProjectJob *job,
                                           const VkrBakeryJson *scene,
                                           const char *reference,
                                           VkrBakeryJson **out_document,
                                           VkrBakeryJson **out_inventory);
bool8_t vkr_project_write_managed_scene(VkrProjectJob *job, const char *path,
                                        VkrBakeryJson *scene);
void vkr_project_document_references(const VkrBakeryJson *value,
                                     VkrProjectSet *digests,
                                     VkrProjectSet *revisions);
bool8_t vkr_project_migrate_source_references(VkrProjectJob *job,
                                              VkrBakeryJson *document,
                                              const char *label);
/* One fingerprinted reference inside an overlay record. */
typedef struct VkrProjectOverlayReference {
  VkrBakeryJson *reference;
  const char *field;
  bool8_t required;
} VkrProjectOverlayReference;

/** Job.overlay_references: the record's own source plus physics targets. */
bool8_t vkr_project_overlay_references(VkrProjectJob *job,
                                       VkrBakeryJson *record,
                                       VkrProjectOverlayReference *out,
                                       uint32_t capacity, uint32_t *out_count);
VkrBakeryJson *vkr_project_record_by_id(const VkrBakeryJson *records,
                                        const char *id);
/** Unlinks `record` from the `records` array. */
void vkr_project_remove_record(VkrBakeryJson *records,
                               const VkrBakeryJson *record);
/** Asset reference {scope, id, role}. */
VkrBakeryJson *vkr_project_reference(VkrProjectJob *job, const char *scope,
                                     const char *id, const char *role);
VkrBakeryJson *vkr_project_identity_transform(VkrProjectJob *job);

// =============================================================================
// Tools and producers
// =============================================================================

/** Runs `vkr_bakery tool <tool> arguments`; nonzero exits outside `accepted`
 * fail with "<label> failed (exit N); see the job log". */
bool8_t vkr_project_run_tool(VkrProjectJob *job, const char *tool,
                             const char *const *arguments, uint32_t count,
                             const char *label, int32_t accepted,
                             int32_t *out_code);
/** Runs this vkr_bakery with `arguments` (for example `bake diffuse ...`),
 * reporting progress with the named input as detail. */
bool8_t vkr_project_run_bakery(VkrProjectJob *job, const char *const *arguments,
                               uint32_t count, const char *label,
                               const char *fallback_detail, int32_t accepted,
                               int32_t *out_code);
/** Runs an external program (the harness) under the same cancellation. */
bool8_t vkr_project_run_program(VkrProjectJob *job, const char *executable,
                                const char *const *arguments, uint32_t count,
                                const char *label, const char *stdout_path,
                                int32_t *out_code);
/** Records the inspection report a cook wrote for `mesh`, so validating it
 * later starts no inspection process. */
bool8_t vkr_project_adopt_inspection(VkrProjectJob *job, const char *mesh,
                                     const char *report);
VkrBakeryJson *vkr_project_inspect_mesh(VkrProjectJob *job, const char *mesh);

// =============================================================================
// Operations (vkr_project_import.c, vkr_project_scene.c, vkr_project_gc.c)
// =============================================================================

/* Owner and records of one asset scope while lowering a scene. */
typedef struct VkrProjectInventory {
  const char *scope;
  char owner[VKR_PROJECT_PATH];
  VkrBakeryJson *records;
} VkrProjectInventory;

/** Clears a failure recorded by a speculative call when none preceded it. */
void vkr_project_forgive(VkrProjectJob *job, bool8_t failed_before);
bool8_t vkr_project_checked_overlay(VkrProjectJob *job,
                                    const VkrBakeryJson *overlay);
bool8_t vkr_project_overlay_colliders(VkrProjectJob *job,
                                      const VkrBakeryJson *overlay,
                                      VkrProjectNodes *out);
bool8_t vkr_project_overlay_identity(VkrProjectJob *job,
                                     const VkrBakeryJson *reference,
                                     const char *seed,
                                     const VkrBakeryJson *scene,
                                     const char *root, char out[17]);
bool8_t vkr_project_inspect_collision(VkrProjectJob *job, const char *value,
                                      char *out);
bool8_t vkr_project_validate_overlay_dependencies(VkrProjectJob *job,
                                                  const VkrBakeryJson *overlay,
                                                  const char *root);
void vkr_project_bind_model_animations(VkrProjectJob *job, VkrBakeryJson *scene,
                                       const VkrProjectInventory *inventories,
                                       uint32_t inventory_count);
/** bake_recipes.prepare_assets is false while a bake is requested. */
bool8_t vkr_project_pending_bakes(const VkrBakeryJson *scene);
bool8_t vkr_project_has_unbuilt(const VkrBakeryJson *assets);
/** Bundles whose materials still name source images instead of .vkt. */
bool8_t vkr_project_texture_repair_bundles(VkrProjectJob *job,
                                           const VkrBakeryJson *scene,
                                           const char *root,
                                           VkrProjectStrings *out);
bool8_t vkr_project_ensure_bootstrap(VkrProjectJob *job);
bool8_t vkr_project_prepare_font(VkrProjectJob *job);
VkrBakeryJson *vkr_project_document(VkrProjectJob *job);
VkrBakeryJson *vkr_project_artifact(VkrProjectJob *job, const char *kind,
                                    const char *name, const char *path,
                                    const char *role, const char *import_id,
                                    const char *source,
                                    VkrBakeryJson *metadata);
bool8_t vkr_project_snapshot_model(VkrProjectJob *job, const char *source,
                                   const char *import_id, char *out);
VkrBakeryJson *vkr_project_import_model(VkrProjectJob *job, const char *source);
VkrBakeryJson *vkr_project_import_cooked_mesh(VkrProjectJob *job,
                                              const char *source);
VkrBakeryJson *vkr_project_import_font(VkrProjectJob *job, const char *source);
bool8_t vkr_project_import_material(VkrProjectJob *job, const char *source,
                                    const char *bundle, const char *import_id,
                                    uint32_t number, char *out);
bool8_t vkr_project_index_bundle(VkrProjectJob *job, const char *bundle,
                                 const char *import_id);
bool8_t vkr_project_pack_bundle_textures(VkrProjectJob *job,
                                         const VkrProjectStrings *materials);
/** Points a material's source-image texture lines at their packed .vkt. */
bool8_t vkr_project_point_material(VkrProjectJob *job, const char *material);
/** Cooks a model into `bundle` with `tool mesh` at the job's texture tier. */
bool8_t vkr_project_cook_mesh(VkrProjectJob *job, const char *source,
                              const char *output, const char *bundle,
                              const char *import_id, const char *label);
bool8_t vkr_project_cook_model_animation(VkrProjectJob *job,
                                         VkrBakeryJson *record,
                                         const char *source, const char *mesh);
/** Copies the bundle directory holding a record's first artifact into a new
 * staged revision `new_id`, rewrites its products, and names the copy
 * `new_id` with no source. */
bool8_t vkr_project_copy_bundle(VkrProjectJob *job, VkrBakeryJson *record,
                                const char *owner, const char *new_id,
                                const char *link_error,
                                const char *missing_error);
VkrBakeryJson *vkr_project_import_managed_scene(VkrProjectJob *job,
                                                VkrBakeryJson *scene,
                                                const char *origin);
VkrBakeryJson *vkr_project_import_source(VkrProjectJob *job,
                                         const char *source);
VkrBakeryJson *vkr_project_import_scene(VkrProjectJob *job, const char *source);
bool8_t vkr_project_append_entities(VkrProjectJob *job, VkrBakeryJson *scene,
                                    int64_t *out_first);
bool8_t vkr_project_validate_semantics(VkrProjectJob *job,
                                       const VkrBakeryJson *scene);
bool8_t vkr_project_validate_bundle_dependencies(VkrProjectJob *job,
                                                 const char *owner,
                                                 const char *path,
                                                 VkrBakeryJson *visited);
VkrBakeryJson *vkr_project_lower(VkrProjectJob *job, VkrBakeryJson *scene,
                                 const char *root, bool8_t publish_runtime);
/** `package_world`: validates the project's root World for a package and
 * places its document, byte-identical, and its portable overlay in the
 * runtime directory. */
VkrBakeryJson *vkr_project_package_world(VkrProjectJob *job);
bool8_t vkr_project_remap_overlay(VkrProjectJob *job, const char *path,
                                  const char *expected, VkrBakeryJson *scene,
                                  const char *source_root, bool8_t managed);
VkrBakeryJson *vkr_project_create(VkrProjectJob *job);
VkrBakeryJson *vkr_project_prepare(VkrProjectJob *job, const char *scene_path);
VkrBakeryJson *vkr_project_prepare_unbuilt(VkrProjectJob *job, const char *path,
                                           VkrBakeryJson *scene);
VkrBakeryJson *vkr_project_import_project_assets(VkrProjectJob *job);
/** Rebuilds the project assets imported at the preview or deferred tier at
 * the final tier, returning the updated inventory. */
VkrBakeryJson *vkr_project_finalize_project_assets(VkrProjectJob *job);
VkrBakeryJson *vkr_project_edit_assets(VkrProjectJob *job);
VkrBakeryJson *vkr_project_inspect_scene(VkrProjectJob *job);
VkrBakeryJson *vkr_project_delete_scene(VkrProjectJob *job);
VkrBakeryJson *vkr_project_delete_project(VkrProjectJob *job);
/** Removes workspace data no listed scene can reach; reports what it freed. */
bool8_t vkr_project_collect_garbage(VkrProjectJob *job, uint32_t *out_removed,
                                    uint64_t *out_freed);

// =============================================================================
// Bakes (vkr_project_bake.c)
// =============================================================================

/** Lowers `scene`, then applies its authored overlay to a bake-only runtime
 * copy under jobs/effective-<uuid>; returns the lower result pointing at it. */
VkrBakeryJson *vkr_project_effective_bake_runtime(VkrProjectJob *job,
                                                  VkrBakeryJson *scene,
                                                  const char *root);
bool8_t vkr_project_perform_bakes(VkrProjectJob *job, VkrBakeryJson *scene,
                                  const char *asset_root);

int vkr_project_main(const VkrBakeryConfig *config, const char *request_path,
                     const char *result_path);
