#pragma once

#include "core/vkr_json_writer.h"
#include "editor_bakery_service.h"
#include "renderer/systems/vkr_ui_system.h"

typedef struct VkrEditorBakery VkrEditorBakery;

/** UI-thread owner. Allocator must support independent frees and outlive the
 * Bakery. Destroy cancels and joins its sole worker before releasing storage.
 */
VkrEditorBakery *vkr_editor_bakery_create(VkrAllocator *allocator);
void vkr_editor_bakery_destroy(VkrEditorBakery *bakery);
/** Poll every editor frame, including while the Bakery panel is hidden. */
void vkr_editor_bakery_update(VkrEditorBakery *bakery);
/** Build within a caller-owned panel. No renderer or asset mutation occurs. */
void vkr_editor_bakery_build(VkrEditorBakery *bakery, VkrUiSystem *ui,
                             VkrUiRect rect, VkrFontHandle heading);

typedef enum VkrEditorProjectJobStatus {
  VKR_EDITOR_PROJECT_JOB_QUEUED,
  VKR_EDITOR_PROJECT_JOB_RUNNING,
  VKR_EDITOR_PROJECT_JOB_SUCCEEDED,
  VKR_EDITOR_PROJECT_JOB_FAILED,
  VKR_EDITOR_PROJECT_JOB_CANCELLED,
  VKR_EDITOR_PROJECT_JOB_UNKNOWN,
} VkrEditorProjectJobStatus;

/* Project jobs share Bakery's sole cancellable worker. The job owns copied
 * paths; callers keep request/result files alive until a terminal status is
 * observed. */
uint64_t vkr_editor_bakery_project_start(VkrEditorBakery *bakery,
                                         const char *request_path,
                                         const char *result_path);
/* A project package (docs/proposals/project-packaging.md) on the same worker:
 * `vkr_bakery bundle <project_directory> --profile <profile> --json`, whose
 * event lines go to `events_path`. Status and cancellation use the project
 * job calls with the returned id. */
uint64_t vkr_editor_bakery_package_start(VkrEditorBakery *bakery,
                                         const char *project_directory,
                                         const char *profile,
                                         const char *events_path);
VkrEditorProjectJobStatus
vkr_editor_bakery_project_status(VkrEditorBakery *bakery, uint64_t job_id,
                                 String8 *log);
void vkr_editor_bakery_project_cancel(VkrEditorBakery *bakery, uint64_t job_id);

/* Managed panel queues only workspace-owned scene recipes. Compiled renderer
 * tables remain explicit development tools outside project asset inventories.
 */
void vkr_editor_bakery_set_managed(VkrEditorBakery *bakery, bool8_t enabled,
                                   bool8_t writable_scene,
                                   const char *workspace);
/** Asks the open writable scene to bake its lighting, as the panel's Prepare
 * does; the Scene menu offers it once Bakery leaves the main navigation.
 * Lightmaps (ADR-087) follow the panel's setting. */
bool8_t vkr_editor_bakery_request_scene_bake(VkrEditorBakery *bakery,
                                             bool8_t reflection,
                                             bool8_t diffuse);
bool8_t vkr_editor_bakery_scene_bake_available(const VkrEditorBakery *bakery);
bool8_t vkr_editor_bakery_take_scene_bake(VkrEditorBakery *bakery,
                                          bool8_t *reflection, bool8_t *diffuse,
                                          bool8_t *lightmap);
/** Whether scene bakes add lightmaps (ADR-088), as the managed panel's
 * "Scene lightmaps" option sets it. */
bool8_t vkr_editor_bakery_lightmap(const VkrEditorBakery *bakery);
/** Whether model imports and rebuilds cook lightmap UVs (ADR-087), as the
 * managed panel sets it. */
bool8_t vkr_editor_bakery_lightmap_uvs(const VkrEditorBakery *bakery);
/** The editor's Bakery daemon, or NULL where it is unavailable. */
EditorBakeryService *vkr_editor_bakery_service(VkrEditorBakery *bakery);
bool8_t vkr_editor_bakery_busy(const VkrEditorBakery *bakery);
bool8_t vkr_editor_bakery_write_settings(const VkrEditorBakery *bakery,
                                         VkrJsonWriter *writer);
void vkr_editor_bakery_read_settings(VkrEditorBakery *bakery, String8 settings);
