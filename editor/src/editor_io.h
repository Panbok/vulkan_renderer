#pragma once

#include "editor_ui.h"

/* Entity IO in the editor (ADR-084). Details shows an object's outgoing
 * connections under Outputs and the ones that reach it under Inputs; each
 * row selects its connection or source. A selected connection shows whether
 * it routes, the ports its ends offer, and a pick mode: the next object
 * selected in the Scene or Outliner becomes its target. Edits go through
 * the agent operations, so they undo like any other. */

/* The Outputs and Inputs sections for the selection, or the Route section
   for a selected connection, from `*y` down. */
void vkr_editor_io_sections(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            float32_t width, float32_t *y,
                            VkrFontHandle heading);

/* Finishes a target pick once the selection moved to another object, and
   cancels it with Escape. Call once per frame. */
void vkr_editor_io_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
