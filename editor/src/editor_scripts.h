#pragma once

#include "editor_bakery_service.h"
#include "vkr_sample_runtime.h"

/* A project's C script modules (ADR-079): `<project>/Scripts/<Name>/` holds
 * `<Name>.script.json` and its sources. The manager builds each module with
 * Bakery, reports compiler diagnostics, asks the runtime to load or hot
 * reload the library, and rebuilds when a source is saved here or changed
 * on disk. UI-thread owner; one build runs at a time on its worker. */

#define VKR_EDITOR_SCRIPT_MODULE_MAX 8u
#define VKR_EDITOR_SCRIPT_FILE_MAX 64u
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
  char output[VKR_EDITOR_SCRIPT_PATH];
  char library[VKR_EDITOR_SCRIPT_PATH];
  VkrEditorScriptStatus status;
  char message[256];
  /* Bytes of the library the runtime last loaded, to skip identical builds. */
  uint64_t loaded_fingerprint;
  uint64_t pending_fingerprint;
  uint32_t watch;
  /* A build was asked for while another one ran. */
  bool8_t rebuild;
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
 * Scans `scripts_directory`, such as a project's `Scripts`, builds each
 * module that has no library under `output_root` synchronously, and adds its
 * load to the frame's script request, which the runtime applies before the
 * project's World and scenes load. Modules with a library load it now and
 * rebuild in the background.
 */
void vkr_editor_scripts_open(VkrEditorScripts *scripts,
                             const char *scripts_directory,
                             const char *output_root,
                             EditorBakeryService *service,
                             const VkrSampleUiFrame *frame);
/** Retires the project's libraries in the runtime and forgets its modules. */
void vkr_editor_scripts_close_project(VkrEditorScripts *scripts,
                                      EditorBakeryService *service,
                                      const VkrSampleUiFrame *frame);
/** Every frame: finishes builds, requests loads, reads load results and
 * starts rebuilds for changed sources. */
void vkr_editor_scripts_update(VkrEditorScripts *scripts,
                               EditorBakeryService *service,
                               const VkrSampleUiFrame *frame);
/** Queues a rebuild of the module owning `path`, such as after a save. */
void vkr_editor_scripts_rebuild_file(VkrEditorScripts *scripts,
                                     const char *path);
/** Writes a new module from the template: `Scripts/<name>/<name>.script.json`
 * and `<name>.c` with one component type that spins its entity. Returns the
 * source path to open. */
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
