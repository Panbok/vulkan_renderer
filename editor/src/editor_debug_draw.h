#pragma once

#include "vkr_sample_runtime.h"

struct VkrEditorUi;

/** Builds the debug shapes' widgets over the Scene image: one path for every
 * segment and a label per debug label (core/vkr_debug_draw.h). */
void vkr_editor_debug_draw_build(struct VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame);

/** Projects the shapes with the frame's final camera, before draw
 * preparation, as vkr_editor_recording_project does for the route. */
void vkr_editor_debug_draw_project(struct VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
