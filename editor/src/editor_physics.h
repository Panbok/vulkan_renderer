#pragma once
#include "vkr_sample_runtime.h"

typedef struct VkrEditorUi VkrEditorUi;
typedef struct VkrEditorPhysicsLine VkrEditorPhysicsLine;

void vkr_editor_physics_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame);
void vkr_editor_physics_project(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
/* Frees the overlay's entity lists. */
void vkr_editor_physics_destroy(VkrEditorUi *editor);
