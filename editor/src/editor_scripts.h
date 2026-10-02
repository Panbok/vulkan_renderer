#pragma once

#include "editor_bakery_service.h"
#include "vkr_sample_runtime.h"

/* A project's C script packages (ADR-079): `<project>/Scripts/<Name>/` holds
 * `<Name>.script.json` and its sources, a module with an entry point or a
 * library other packages depend on. The manager builds the whole folder into
 * one project library with Bakery, reports compiler diagnostics, asks the
 * runtime to load or hot reload that library, and rebuilds when a source is
 * saved here or changed on disk. UI-thread owner; one build runs at a time
 * on its worker. */

#define VKR_EDITOR_SCRIPT_MODULE_MAX 64u
#define VKR_EDITOR_SCRIPT_FILE_MAX 256u
#define VKR_EDITOR_SCRIPT_DEPENDENCY_MAX 8u
#define VKR_EDITOR_SCRIPT_DIAGNOSTIC_MAX 64u
#define VKR_EDITOR_SCRIPT_PATH 1024u
#define VKR_EDITOR_SCRIPT_NAME 48u

typedef enum VkrEditorScriptStatus {
  VKR_EDITOR_SCRIPT_UNBUILT = 0,
  VKR_EDITOR_SCRIPT_BUILDING,
  VKR_EDITOR_SCRIPT_BUILD_FAILED,
  /* Built; the runtime load is requested or pending. */
  VKR_EDITOR_SCRIPT_LOADING,
  VKR_EDITOR_SCRIPT_LOADED,
  VKR_EDITOR_SCRIPT_LOAD_FAILED,
} VkrEditorScriptStatus;

typedef struct VkrEditorScriptModule {
  char name[VKR_EDITOR_SCRIPT_NAME];
  char directory[VKR_EDITOR_SCRIPT_PATH];
  char description[VKR_EDITOR_SCRIPT_PATH];
  /* The project library's state, shown on each package. */
  VkrEditorScriptStatus status;
  char message[256];
  uint32_t watch;
  /* A library package: code other packages use, with no entry point. */
  bool8_t library_kind;
  /* Packages whose headers it includes, from its description. */
  char dependencies[VKR_EDITOR_SCRIPT_DEPENDENCY_MAX][VKR_EDITOR_SCRIPT_NAME];
  uint32_t dependency_count;
} VkrEditorScriptModule;

typedef struct VkrEditorScriptFile {
  char path[VKR_EDITOR_SCRIPT_PATH];
  char name[128];
  uint32_t module;
} VkrEditorScriptFile;

typedef struct VkrEditorScriptDiagnostic {
  char path[VKR_EDITOR_SCRIPT_PATH];
  uint32_t module;
  uint32_t line;
  uint32_t column;
  bool8_t error;
  char message[256];
} VkrEditorScriptDiagnostic;

typedef struct VkrEditorScripts VkrEditorScripts;

/** The allocator must support independent frees and outlive the manager. */
VkrEditorScripts *vkr_editor_scripts_create(VkrAllocator *allocator);
/** Joins a running build first. */
void vkr_editor_scripts_destroy(VkrEditorScripts *scripts);

/**
 * Scans `scripts_directory`, such as a project's `Scripts`. An existing
 * project library under `output_root` joins the frame's script request,
 * which the runtime applies before the project's World and scenes load, and
 * rebuilds in the background. Without one, the first build starts on the
 * worker and its load follows when it finishes; until then
 * vkr_editor_scripts_settling holds the project's documents back.
 */
void vkr_editor_scripts_open(VkrEditorScripts *scripts,
                             const char *scripts_directory,
                             const char *output_root,
                             EditorBakeryService *service,
                             const VkrSampleUiFrame *frame);
/** True while an opened project's first build or its load has not
 * reported, so documents that use its component types should wait. */
bool8_t vkr_editor_scripts_settling(const VkrEditorScripts *scripts);

/** Retires the project's libraries in the runtime and forgets its modules. */
void vkr_editor_scripts_close_project(VkrEditorScripts *scripts,
                                      EditorBakeryService *service,
                                      const VkrSampleUiFrame *frame);
/** Every frame: finishes builds, requests loads, reads load results and
 * starts rebuilds for changed sources. */
void vkr_editor_scripts_update(VkrEditorScripts *scripts,
                               EditorBakeryService *service,
                               const VkrSampleUiFrame *frame);
/** Queues a rebuild of the project when a package owns `path`, such as
 * after a save. */
void vkr_editor_scripts_rebuild_file(VkrEditorScripts *scripts,
                                     const char *path);
/** The folders whose headers a source at `path` may include: its package's,
 * then those of the packages it depends on, directly or not. */
uint32_t vkr_editor_scripts_header_folders(const VkrEditorScripts *scripts,
                                           const char *path, const char **out,
                                           uint32_t capacity);

/** Writes a new module from the template: `Scripts/<name>/<name>.script.json`
 * and `<name>.c` with one component type and a behavior whose start,
 * update, fixed update, destroy and stop hooks are empty. Returns the source
 * path to open. */
bool8_t vkr_editor_scripts_create_module(VkrEditorScripts *scripts,
                                         const char *name, char *out_path,
                                         uint32_t out_capacity, char *error,
                                         uint32_t error_capacity);

bool8_t vkr_editor_scripts_project_open(const VkrEditorScripts *scripts);
/** Increases whenever the module or file list changes; build and load
 * status changes do not count. */
uint64_t vkr_editor_scripts_revision(const VkrEditorScripts *scripts);
uint32_t vkr_editor_scripts_module_count(const VkrEditorScripts *scripts);
const VkrEditorScriptModule *
vkr_editor_scripts_module(const VkrEditorScripts *scripts, uint32_t index);
uint32_t vkr_editor_scripts_file_count(const VkrEditorScripts *scripts);
const VkrEditorScriptFile *
vkr_editor_scripts_file(const VkrEditorScripts *scripts, uint32_t index);
/** The module owning a file path, or NULL. */
const VkrEditorScriptModule *
vkr_editor_scripts_module_of(const VkrEditorScripts *scripts, const char *path);
/** Diagnostics of the latest build of every module. */
uint32_t vkr_editor_scripts_diagnostic_count(const VkrEditorScripts *scripts);
const VkrEditorScriptDiagnostic *
vkr_editor_scripts_diagnostic(const VkrEditorScripts *scripts, uint32_t index);
