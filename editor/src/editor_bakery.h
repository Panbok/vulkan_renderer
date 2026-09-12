#pragma once

#include "renderer/systems/vkr_ui_system.h"
#include "core/vkr_json_writer.h"

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

/* Project jobs share Bakery's sole cancellable worker. The job owns copied paths;
 * callers keep request/result files alive until a terminal status is observed. */
uint64_t vkr_editor_bakery_project_start(VkrEditorBakery *bakery,
                                       const char *request_path,
                                       const char *result_path);
VkrEditorProjectJobStatus
vkr_editor_bakery_project_status(VkrEditorBakery *bakery, uint64_t job_id,
                                String8 *log);
void vkr_editor_bakery_project_cancel(VkrEditorBakery *bakery, uint64_t job_id);

/* Managed panel queues only workspace-owned scene recipes. Compiled renderer
 * tables remain explicit development tools outside project asset inventories. */
void vkr_editor_bakery_set_managed(VkrEditorBakery *bakery, bool8_t enabled,
                                  bool8_t writable_scene, const char *workspace);
bool8_t vkr_editor_bakery_take_scene_bake(VkrEditorBakery *bakery,
                                         bool8_t *reflection, bool8_t *diffuse);
bool8_t vkr_editor_bakery_busy(const VkrEditorBakery *bakery);
bool8_t vkr_editor_bakery_write_settings(const VkrEditorBakery *bakery,
                                         VkrJsonWriter *writer);
void vkr_editor_bakery_read_settings(VkrEditorBakery *bakery, String8 settings);
