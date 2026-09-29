#pragma once

#include "editor_project_store.h"
#include "vkr_sample_runtime.h"

typedef struct VkrEditorUi VkrEditorUi;
typedef struct VkrEditorBuild VkrEditorBuild;

/* Project builds (docs/proposals/project-packaging.md): the Build Settings
 * window over the project's game.json, the preflight, the package job on
 * Bakery's worker, its progress and report, and Build and Run. UI-thread
 * owner; the running game's output is forwarded to the Console as `[game]`
 * lines. */
VkrEditorBuild *vkr_editor_build_create(VkrAllocator *allocator);
/** Stops a running game and releases storage; a queued or running package
 * job belongs to Bakery, which cancels it on its own destruction. */
void vkr_editor_build_destroy(VkrEditorBuild *build);
/** Poll every frame: follows the open project, drives the preflight and the
 * job, and forwards a running game's output. */
void vkr_editor_build_update(VkrEditorBuild *build, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);
/** Builds the selected profile, then runs the game when `run`. False with a
 * message when no build can start. */
bool8_t vkr_editor_build_start(VkrEditorBuild *build, VkrEditorUi *editor,
                               bool8_t run, char *message, uint32_t capacity);
/** Cancels the preflight or the package job. */
void vkr_editor_build_cancel(VkrEditorBuild *build, VkrEditorUi *editor);
/** Opens the last package's folder in the system file browser. */
bool8_t vkr_editor_build_open_last(VkrEditorBuild *build, char *message,
                                   uint32_t capacity);
/** A project is open and no build is in progress. */
bool8_t vkr_editor_build_available(const VkrEditorBuild *build,
                                   const VkrEditorUi *editor);
bool8_t vkr_editor_build_busy(const VkrEditorBuild *build);
bool8_t vkr_editor_build_has_package(const VkrEditorBuild *build);
/** Selects the profile named `name`; false when none has that name. */
bool8_t vkr_editor_build_select_profile(VkrEditorBuild *build,
                                        const char *name);
/** The last build's outcome line, empty before the first. */
const char *vkr_editor_build_result(const VkrEditorBuild *build,
                                    bool8_t *out_succeeded);
/** Build Settings window body within `bounds`, in pixels. */
void vkr_editor_build_settings_build(VkrEditorBuild *build, VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrUiRect bounds);
/** The Build tab: stage checklist and durations, log and report. */
void vkr_editor_build_panel(VkrEditorBuild *build, VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame, VkrUiRect rect);
/** Status strip with the current stage and Cancel while a build runs. */
void vkr_editor_build_status_build(VkrEditorBuild *build, VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
