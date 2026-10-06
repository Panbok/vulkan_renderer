#pragma once

#include "editor_brush_grid.h"
#include "vkr_sample_runtime.h"

/* Editable blockout shapes (ADR-084): a stairs or corridor group carries a
   `blockout` component, and the editor builds its brushes from it. With the
   group selected the Scene shows handles: stairs drag their length, height,
   width, turn and radius, and corridors their points, with a + between two
   points that adds one and Ctrl+click on a point that removes it. A drag
   previews the shape it would build; the release builds it through
   blockout.build as one undo step. Details edits of the component build the
   same way. */

typedef struct VkrEditorUi VkrEditorUi;

/* Runs the handles of `group` when it is a blockout shape, or none. */
void vkr_editor_blockout_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame,
                                VkrEntityId group, Vec3 origin, Vec3 direction,
                                bool8_t has_ray, bool8_t inside);

/* Shows `draft` of `group` as the shape it would build, as while Details
   edits it, or stops with NULL. */
void vkr_editor_blockout_preview(VkrEditorUi *editor, VkrEntityId group,
                                 const SceneBlockout *draft);

/* Builds `group` from `shape` through blockout.build. */
void vkr_editor_blockout_submit(VkrEditorUi *editor, VkrEntityId group,
                                const SceneBlockout *shape);

/* Whether the pointer is over a handle or a drag runs. */
bool8_t vkr_editor_blockout_busy(const VkrEditorUi *editor);

/* What the next press or the running drag does, or NULL. */
const char *vkr_editor_blockout_hint(const VkrEditorUi *editor);

/* The overlay segments of the last update; `out` may be NULL to count
   them. */
uint32_t vkr_editor_blockout_lines(const VkrEditorUi *editor,
                                   const VkrEditorBrushGridLine **out);

void vkr_editor_blockout_destroy(VkrEditorUi *editor);
