#pragma once

#include "vkr_sample_runtime.h"

/* Brush grid editing (ADR-084): a brush selected with the Select tool shows
   a grid on the faces turned toward the view, its edges and its corners.
   A drag across cells selects a patch, whose arrow pulls it out into a new
   brush or pushes it in as a recess; a drag on a corner, an edge or a grid
   line moves it along the face's normal, or along the face with Shift,
   reshaping the brush through brush.patch and brush.reshape. The Clip
   tool shows the same grid and cuts the brush through brush.clip: a click
   on a grid line cuts along it, and clicks on corners, edges and grid
   crossings set the points a cut passes through. */

typedef struct VkrEditorUi VkrEditorUi;

/* At most this many overlay segments a frame. */
#define VKR_EDITOR_BRUSH_GRID_LINE_MAX 448u

typedef struct VkrEditorBrushGridLine {
  Vec3 from;
  Vec3 to;
  Vec4 color;
} VkrEditorBrushGridLine;

/* What a press on the grid does. */
typedef enum VkrEditorBrushGridMode {
  /* Cells select patches; lines, edges and corners reshape. */
  VKR_EDITOR_BRUSH_GRID_EDIT = 0,
  /* As EDIT without patches, while box drawing takes cell presses. */
  VKR_EDITOR_BRUSH_GRID_DRAW,
  /* Lines, corners, edges and crossings set a cut (the Clip tool). */
  VKR_EDITOR_BRUSH_GRID_CLIP,
} VkrEditorBrushGridMode;

/* Shows the grid on `brush`, or nothing when it is invalid, and runs the
   pointer in `mode`; `blocked` keeps a press for another tool. */
void vkr_editor_brush_grid_update(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  const VkrScene *scene, VkrEntityId brush,
                                  VkrEditorBrushGridMode mode, Vec3 origin,
                                  Vec3 direction, bool8_t has_ray,
                                  bool8_t inside, bool8_t blocked);

/* Whether the pointer is over a grid target or a drag runs. */
bool8_t vkr_editor_brush_grid_busy(const VkrEditorUi *editor);

/* Cuts the selected patch through the brush, the whole way along the face's
   normal, as a doorway or window (brush.patch). False without a patch. */
bool8_t vkr_editor_brush_grid_patch_cut(VkrEditorUi *editor);

/* Whether the Clip tool holds points of a cut, which Escape drops. */
bool8_t vkr_editor_brush_grid_clip_pending(const VkrEditorUi *editor);

/* What the next press or the running drag does, or NULL. */
const char *vkr_editor_brush_grid_hint(const VkrEditorUi *editor);

/* The overlay segments of the last update; `out` may be NULL to count
   them. */
uint32_t vkr_editor_brush_grid_lines(const VkrEditorUi *editor,
                                     const VkrEditorBrushGridLine **out);

void vkr_editor_brush_grid_destroy(VkrEditorUi *editor);
