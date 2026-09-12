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
bool8_t vkr_editor_projects_loading(const VkrEditorProjects *projects);
void vkr_editor_projects_update(VkrEditorProjects *projects,
                                VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_projects_build(VkrEditorProjects *projects, VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_projects_build_scene_progress(VkrEditorProjects *projects,
                                              VkrEditorUi *editor,
                                              const VkrSampleUiFrame *frame);
void vkr_editor_projects_navigation(VkrEditorProjects *projects,
                                    VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

bool8_t vkr_editor_projects_save_scene(VkrEditorProjects *projects,
                                       VkrSceneEditState *edits,
                                       const VkrScene *scene,
                                       String8 runtime_scene_path);
void vkr_editor_projects_scene_action(VkrEditorProjects *projects,
                                      VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);

bool8_t vkr_editor_projects_flush(VkrEditorProjects *projects,
                                  VkrEditorUi *editor,
                                  const VkrUiDockTree *dock);
