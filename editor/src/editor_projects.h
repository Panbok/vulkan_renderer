#pragma once

#include "vkr_sample_runtime.h"

typedef struct VkrEditorUi VkrEditorUi;
typedef struct VkrEditorProjects VkrEditorProjects;

/* Editor-lifetime owner, independent of loaded world/scene assets. */
VkrEditorProjects *vkr_editor_projects_create(VkrAllocator *allocator, int argc,
                                              char **argv);
bool8_t vkr_editor_projects_destroy(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrUiDockTree *dock);
bool8_t vkr_editor_projects_modal(const VkrEditorProjects *projects);
/* Whether a floating project dialog covers the pixel. */
bool8_t vkr_editor_projects_dialog_contains(const VkrEditorProjects *projects,
                                            float32_t x_px, float32_t y_px);
/** The chooser or create flow runs before any project is open, so the window
 * shows a compact launcher instead of the editor shell. */
bool8_t vkr_editor_projects_launcher(const VkrEditorProjects *projects);
bool8_t vkr_editor_projects_loading(const VkrEditorProjects *projects);
void vkr_editor_projects_update(VkrEditorProjects *projects,
                                VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_projects_build(VkrEditorProjects *projects, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_projects_build_scene_progress(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame);
/** Projects and Scenes dropdown buttons in top-bar columns `first_column`
 * and `first_column + 1`. */
void vkr_editor_projects_navigation(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t first_column);

bool8_t vkr_editor_projects_save_scene(VkrEditorProjects *projects,
                                       VkrSceneEditState *edits,
                                       const VkrScene *scene,
                                       String8 runtime_scene_path);
void vkr_editor_projects_scene_action(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);

bool8_t vkr_editor_projects_can_add_entity(const VkrEditorProjects *projects,
                                           VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame);
void vkr_editor_projects_add_entity(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

bool8_t vkr_editor_projects_flush(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrUiDockTree *dock);

/* Create or import window (ADR-076), opened by Content's Import button: a
   scene of this project, or an asset copied into the open scene. */
void vkr_editor_projects_build_create_window(VkrEditorProjects *projects,
                                             VkrEditorUi *editor,
                                             const VkrSampleUiFrame *frame,
                                             VkrUiRect bounds);

/* Start the job that creates an empty scene named `name` in the open project
   and opens it; false with a message when no job can start. */
bool8_t vkr_editor_projects_create_scene(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *name);

/* The open project scene's display name, or empty outside a project. */
String8 vkr_editor_projects_scene_name(const VkrEditorProjects *projects);

/* Open the project scene named `name` (exact, else first containing it);
   false when none matches or a job is running. The scene already loading or
   open stays as it is and sets *out_current. */
bool8_t vkr_editor_projects_open_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       String8 name, bool8_t *out_current);

/* Viewport documents (ADR-076): the open project scene's id (empty when only
   the World is open), whether a switch can start now, and switching. */
String8 vkr_editor_projects_scene_id(const VkrEditorProjects *projects);
bool8_t vkr_editor_projects_switch_ready(const VkrEditorProjects *projects);
/* Open the scene with `scene_id`, or with an empty id close the open scene
   so only the World shows. Unsaved edits ask first. */
bool8_t vkr_editor_projects_show_scene(VkrEditorProjects *projects,
                                       VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       const char *scene_id);

/* Added scenes (ADR-076): the project name of the scene an added runtime
   document came from, or empty; adding the project scene named `name`
   beside the open one; and making the added scene in `container` the
   primary scene, adding the previous primary back beside it. Unsaved edits
   in either scene ask first. False when nothing could start. */
String8 vkr_editor_projects_added_name(const VkrEditorProjects *projects,
                                       String8 path);
/* The project id of the scene an added runtime document came from, or "". */
const char *vkr_editor_projects_added_id(const VkrEditorProjects *projects,
                                         String8 path);
bool8_t vkr_editor_projects_add_scene(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 name);
bool8_t vkr_editor_projects_set_primary(VkrEditorProjects *projects,
                                        VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame,
                                        uint16_t container);
/* Prefab instance (ADR-076): copy the project scene named `name` into the
   open scene under one new root at `position`. The copy keeps no link to
   its source and reloads the scene; unsaved edits ask first. */
bool8_t vkr_editor_projects_instantiate_scene(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame,
                                              String8 name, Vec3 position);

/* Import one file (model, texture, font or material) into the project's
   shared assets (ADR-076); false when no job could start. */
/* Open the Create window's scene step importing `source`, as if chosen with
   Browse; the import preflight starts at once. */
bool8_t vkr_editor_projects_import_scene_form(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const char *source);
/* Files dropped from the OS (ADR-076): the Create or import window opens at
   its import step, filing the imports into Content `folder`. */
bool8_t vkr_editor_projects_import_files(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrWindowFileDrop *drop,
                                         const char *folder);
bool8_t vkr_editor_projects_import_asset(VkrEditorProjects *projects,
                                         VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         const char *source);
