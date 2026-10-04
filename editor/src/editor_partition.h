#pragma once

#include "editor_ui.h"

/* World partition in the editor (ADR-086): the World Partition window maps
 * the open scene's cells around the camera, and partition.load and
 * partition.unload pin cells for editing or let them go. */

/* Asks the runtime to load and pin cells x0..x1, z0..z1 (`cells`), or with
   `unload` to unpin and unload them, or every loaded cell with `all`.
   False with `message` when the scene has no partition. */
bool8_t vkr_editor_partition_request(const VkrSampleUiFrame *frame,
                                     bool8_t unload, bool8_t all,
                                     const int32_t cells[4], char *message,
                                     uint32_t capacity);

/* The World Partition window body: settings, counts and the cell map. */
void vkr_editor_partition_window_build(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       VkrUiRect bounds);
