#pragma once

#include "editor_ui.h"

/** Builds the Preferences body inside its window panel; `bounds` is the body
 * rectangle in pixels. */
void vkr_editor_graphics_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame, VkrUiRect bounds);
