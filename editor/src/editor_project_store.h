#pragma once

#include "containers/str.h"
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
bool8_t vkr_editor_project_create(VkrEditorWorkspace *workspace,
                                  const char *name, VkrEditorProject *project,
                                  VkrEditorProjectError *error);
// Synchronously validates and atomically replaces the manifest. Refuses a stale
// fingerprint and concurrent writers; failure preserves the previous manifest.
bool8_t vkr_editor_project_save(VkrEditorProject *project,
                                VkrEditorProjectError *error);
// Validate a manifest byte buffer without filesystem reads. Views borrow bytes.
bool8_t vkr_editor_project_parse(String8 bytes, VkrEditorProject *project,
                                 VkrEditorProjectError *error);
// Resolve an existing managed relative path, rejecting lexical/symlink escapes.
bool8_t
vkr_editor_project_resolve(const char *owner_root, const char *relative,
                           char out_path[VKR_EDITOR_PROJECT_PATH_CAPACITY],
                           VkrEditorProjectError *error);

// Validates a JSON object and returns a raw member view borrowing the input.
bool8_t vkr_editor_project_json_member(String8 object, const char *name,
                                       String8 *value,
                                       VkrEditorProjectError *error);

// Reserve a draft directory and defaults without publishing project.json.
bool8_t vkr_editor_project_begin(VkrEditorWorkspace *workspace,
                                 const char *name, VkrEditorProject *project,
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

/** OS-local jobs directory for read-only workspace runtime projections. */
bool8_t vkr_editor_project_local_jobs_directory(
    char out[VKR_EDITOR_PROJECT_PATH_CAPACITY], VkrEditorProjectError *error);
