#pragma once

#include "containers/str.h"
#include "core/vkr_type_desc.h"
#include "memory/vkr_allocator.h"
#include "platform/vkr_platform.h"

#define VKR_EDITOR_PROJECT_PATH_CAPACITY 1024u
#define VKR_EDITOR_PROJECT_NAME_CAPACITY 513u
#define VKR_EDITOR_PROJECT_MAX_SCENES 128u
#define VKR_EDITOR_PROJECT_JSON_LIMIT (1024u * 1024u)

typedef struct VkrEditorProjectError {
  char message[512];
} VkrEditorProjectError;

typedef struct VkrEditorWorkspace {
  // Canonical absolute .vkreditor path, also before initial creation.
  char root[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  char id[37];
  bool8_t initialized;
} VkrEditorWorkspace;

typedef struct VkrEditorProjectScene {
  char id[37];
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  char path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
} VkrEditorProjectScene;

typedef struct VkrEditorProject {
  char id[37];
  char name[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  char manifest_path[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  VkrEditorProjectScene scenes[VKR_EDITOR_PROJECT_MAX_SCENES];
  uint32_t scene_count;
  // Validated JSON views. Loaded views borrow document storage from the
  // caller's allocator and expire when it is reset. New projects use immutable
  // literals.
  String8 default_font;
  String8 editor_settings;
  String8 scene_editor_state;
  String8 assets;
  String8 document;
  uint64_t fingerprint;
} VkrEditorProject;

// No mutation for create=false, including a previously unused workspace folder.
bool8_t vkr_editor_workspace_open(const char *directory, bool8_t create,
                                  VkrEditorWorkspace *workspace,
                                  VkrEditorProjectError *error);
// The callback receives a canonical ID; load errors can be shown as broken
// cards.
typedef bool8_t (*VkrEditorProjectVisitor)(const char *id, void *context);
bool8_t vkr_editor_workspace_visit(const VkrEditorWorkspace *workspace,
                                   VkrEditorProjectVisitor visitor,
                                   void *context, VkrEditorProjectError *error);
bool8_t vkr_editor_project_id_generate(char id[37],
                                       VkrEditorProjectError *error);
bool8_t vkr_editor_project_name_valid(const char *name,
                                      VkrEditorProjectError *error);
bool8_t vkr_editor_project_load(const VkrEditorWorkspace *workspace,
                                const char *id, VkrAllocator *allocator,
                                VkrEditorProject *project,
                                VkrEditorProjectError *error);
// Initializes the workspace when needed, then reserves a project directory and
// publishes its default project.json, so later job failures leave it listed.
bool8_t vkr_editor_project_create(VkrEditorWorkspace *workspace,
                                  const char *name, VkrEditorProject *project,
                                  VkrEditorProjectError *error);
// Removes project.json under the project lock, so discovery no longer lists
// the project. Files remain until a delete_project job erases the directory.
// Requires the workspace write lease; callers first stop the project's users.
bool8_t vkr_editor_project_unpublish(const VkrEditorWorkspace *workspace,
                                     const char *id,
                                     VkrEditorProjectError *error);
// Synchronously validates and atomically replaces the manifest. Refuses a stale
// fingerprint and concurrent writers; failure preserves the previous manifest.
bool8_t vkr_editor_project_save(VkrEditorProject *project,
                                VkrEditorProjectError *error);
// Atomically removes membership and recall; failure restores the in-memory
// project. Updated recall is allocated from allocator and must outlive project
// use (or be copied). Files are retained until the editor stops their consumers
// and dispatches the delete_scene job. Requires the workspace write lease.
bool8_t vkr_editor_project_remove_scene(VkrEditorProject *project,
                                        uint32_t index, VkrAllocator *allocator,
                                        VkrEditorProjectError *error);
// Validate a manifest byte buffer without filesystem reads. Views borrow bytes.
bool8_t vkr_editor_project_parse(String8 bytes, VkrEditorProject *project,
                                 VkrEditorProjectError *error);
// Resolve an existing managed relative path, rejecting lexical/symlink escapes.
bool8_t
vkr_editor_project_resolve(const char *owner_root, const char *relative,
                           char out_path[VKR_EDITOR_PROJECT_PATH_CAPACITY],
                           VkrEditorProjectError *error);

// Resolve the selected scene from its loaded manifest owner. Deletion may name
// a missing manifest; its job still verifies physical containment before
// mutation.
bool8_t
vkr_editor_project_scene_path(const VkrEditorProject *project, uint32_t index,
                              bool8_t must_exist,
                              char out_path[VKR_EDITOR_PROJECT_PATH_CAPACITY],
                              VkrEditorProjectError *error);

// Validates a JSON object and returns a raw member view borrowing the input.
bool8_t vkr_editor_project_json_member(String8 object, const char *name,
                                       String8 *value,
                                       VkrEditorProjectError *error);

bool8_t vkr_editor_project_json_string(String8 object, const char *name,
                                       char *out, uint32_t capacity,
                                       VkrEditorProjectError *error);

typedef struct VkrEditorWorkspaceLease {
  VkrPlatformProcessLock lock;
} VkrEditorWorkspaceLease;

// Hold across the entire managed editor session. A failed acquisition means
// read-only workspace access; callers must disable imports, saves and caches.
bool8_t vkr_editor_workspace_lease_acquire(const VkrEditorWorkspace *workspace,
                                           VkrEditorWorkspaceLease *lease,
                                           VkrEditorProjectError *error);
void vkr_editor_workspace_lease_release(VkrEditorWorkspaceLease *lease);
// NULL locator_path selects the OS-local VKR/editor.json. An explicit path is
// useful for isolated tests/portable deployments. Missing file => success and
// empty directory. Returned paths never borrow parser or native storage.
bool8_t vkr_editor_workspace_locator_load(
    const char *locator_path, char directory[VKR_EDITOR_PROJECT_PATH_CAPACITY],
    VkrEditorProjectError *error);
bool8_t vkr_editor_workspace_locator_save(const char *locator_path,
                                          const char *directory,
                                          VkrEditorProjectError *error);

// Returns an owned length+1 STRING allocation, preserving all other members.
bool8_t vkr_editor_project_json_replace_member(VkrAllocator *allocator,
                                               String8 object, const char *key,
                                               String8 value, String8 *out,
                                               VkrEditorProjectError *error);

struct VkrScene;
struct VkrSceneEditState;
bool8_t vkr_editor_project_json_read_file(const char *path,
                                          VkrAllocator *allocator,
                                          String8 *bytes, uint64_t *fingerprint,
                                          VkrEditorProjectError *error);
bool8_t vkr_editor_project_save_scene_overlay(const char *manifest_path,
                                              uint64_t *expected_fingerprint,
                                              struct VkrSceneEditState *edits,
                                              const struct VkrScene *scene,
                                              VkrAllocator *scratch_allocator,
                                              VkrEditorProjectError *error);
uint64_t vkr_editor_project_document_fingerprint(String8 bytes);

// Recursively overlays owned object fields while preserving unknown members.
// Arrays/scalars replace whole values. Output allocation has previous.length +
// owned.length + 1 bytes; intended for a caller-owned scratch allocator scope.
bool8_t vkr_editor_project_json_merge_objects(VkrAllocator *allocator,
                                              String8 previous, String8 owned,
                                              String8 *out,
                                              VkrEditorProjectError *error);

// Content labels (ADR-076): the project's virtual folder tree and each item's
// folder and tags, stored beside project.json as content.labels.json. Files
// never move. Items are named by id: a UUID for managed assets and scenes, a
// short name for editor bundle assets and built-in objects. Folder paths are
// '/'-separated names below the Content root, which is the empty path.
#define VKR_EDITOR_LABELS_VERSION 2u
#define VKR_EDITOR_FOLDER_PATH_CAPACITY 64u

typedef struct VkrEditorAssetLabel {
  char id[37];
  char folder[VKR_EDITOR_FOLDER_PATH_CAPACITY];
  char tags[128]; // Comma-separated.
} VkrEditorAssetLabel;

typedef struct VkrEditorFolder {
  char path[VKR_EDITOR_FOLDER_PATH_CAPACITY];
} VkrEditorFolder;

// Caller-owned arrays of one labels document. Folders list every virtual
// folder, including empty ones and each labelled folder's ancestors.
typedef struct VkrEditorContentLabels {
  VkrEditorAssetLabel *labels;
  uint32_t label_count;
  uint32_t label_capacity;
  VkrEditorFolder *folders;
  uint32_t folder_count;
  uint32_t folder_capacity;
} VkrEditorContentLabels;

// Keeps printable text without JSON delimiters and trims outer spaces.
void vkr_editor_label_clean(char *out, uint32_t capacity, const uint8_t *text,
                            uint64_t length);

// Rewrites `path` as clean '/'-separated names: no empty names or outer
// slashes. Returns false for a path too long to hold.
bool8_t vkr_editor_folder_normalize(char path[VKR_EDITOR_FOLDER_PATH_CAPACITY]);
// Whether `path` equals `ancestor` or lies below it; every path lies within
// the root.
bool8_t vkr_editor_folder_within(const char *path, const char *ancestor);
void vkr_editor_folder_parent(const char *path,
                              char out[VKR_EDITOR_FOLDER_PATH_CAPACITY]);
// The last name of a path; the root has none.
const char *vkr_editor_folder_name(const char *path);

// Adds a folder and its ancestors; false when capacity runs out.
bool8_t vkr_editor_folders_add(VkrEditorContentLabels *doc, const char *path);
// Moves or renames a folder with its subfolders and items. Fails without
// change when `to` is inside `from`, already exists, or is the root.
bool8_t vkr_editor_folders_rename(VkrEditorContentLabels *doc, const char *from,
                                  const char *to);
// Removes an empty folder: no subfolders and no items.
bool8_t vkr_editor_folders_remove(VkrEditorContentLabels *doc,
                                  const char *path);

// Reads version 1 (labels only) or 2. Entries without a usable id are
// skipped and labelled folders join the folder list. Any other version fails
// with an empty document.
bool8_t vkr_editor_labels_parse(String8 bytes, VkrEditorContentLabels *doc,
                                VkrEditorProjectError *error);

struct VkrJsonWriter;
bool8_t vkr_editor_labels_write(struct VkrJsonWriter *writer,
                                const VkrEditorContentLabels *doc);

// Presets (ADR-076): named values of one component type, stored beside
// project.json as presets.json. Applying a preset copies its values through
// the edit journal. Preset types are the live world component types, the
// three light types and the physics body settings.
#define VKR_EDITOR_PRESETS_VERSION 1u
#define VKR_EDITOR_PRESET_MAX 64u

typedef struct VkrEditorPreset {
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  const VkrTypeDesc *type;
  char id[37];
  char name[64];
} VkrEditorPreset;

// Caller-owned array of one presets document.
typedef struct VkrEditorPresets {
  VkrEditorPreset *presets;
  uint32_t count;
  uint32_t capacity;
} VkrEditorPresets;

// The preset type named `name`, or NULL when presets cannot hold it.
const VkrTypeDesc *vkr_editor_preset_type(String8 name);

// Reads version 1. A record without an id or name, of an unknown type, or
// whose values fail the type's validation is skipped. Any other version
// fails with an empty document. `scratch` decodes string properties.
bool8_t vkr_editor_presets_parse(String8 bytes, VkrEditorPresets *doc,
                                 VkrAllocator *scratch,
                                 VkrEditorProjectError *error);
bool8_t vkr_editor_presets_write(struct VkrJsonWriter *writer,
                                 const VkrEditorPresets *doc);

/** OS-local jobs directory for read-only workspace runtime projections. */
bool8_t vkr_editor_project_local_jobs_directory(
    char out[VKR_EDITOR_PROJECT_PATH_CAPACITY], VkrEditorProjectError *error);
