#pragma once

#include "editor_ui.h"

/* Workbenches (ADR-089): a row of tabs under the top bar switches the dock
 * layout, the open floating windows and the Scene's editing mode between
 * tasks. The selection, camera, scenes, journal and
 * simulation stay as they are, and a switch makes no undo entry. */

/* The row's height at zoom 1; the dock's toolbar leaf holds the top bar and
   the row. */
#define VKR_EDITOR_WORKBENCH_ROW_PT 28.0f

/* Built-in layouts and modes, General active. */
void vkr_editor_workbench_init(VkrEditorWorkbenches *workbenches);

/* Applies a requested switch and keeps the dock's chrome height. Runs at the
   start of each UI build, before the dock and the Cmd queue read input. */
void vkr_editor_workbench_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame);

/* Asks to switch to workbench `index` at the start of the next UI build.
   False with a message in `message` for an unknown index, in the Scene-only
   view, or while a pointer gesture holds the mouse. */
bool8_t vkr_editor_workbench_request(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t index, char *message,
                                     uint64_t capacity);

/* The workbench a Cmd word names: its id, its name, or its 1-based
   position; `next` and `prev` count from the active one. UINT32_MAX when
   none matches. */
uint32_t vkr_editor_workbench_find(const VkrEditorWorkbenches *workbenches,
                                   String8 word);

const char *vkr_editor_workbench_id(const VkrEditorWorkbenches *workbenches,
                                    uint32_t index);
const char *vkr_editor_workbench_name(const VkrEditorWorkbenches *workbenches,
                                      uint32_t index);

/* Custom workbenches. Duplicate inserts a copy of workbench `index` (the
   live layout, windows and mode when it is active) after it and switches to
   it. Delete removes a custom workbench, switching away first when it is
   active. Move swaps a tab with its neighbour (`step` -1 or +1). Rename
   gives a workbench a name of 1 to 31 bytes; rename_begin opens the tab's
   inline field. Each returns false with `message` when it cannot. */
bool8_t vkr_editor_workbench_duplicate(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame,
                                       uint32_t index, char *message,
                                       uint64_t capacity);
bool8_t vkr_editor_workbench_delete(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    uint32_t index, char *message,
                                    uint64_t capacity);
bool8_t vkr_editor_workbench_move(VkrEditorUi *editor, uint32_t index,
                                  int32_t step, char *message,
                                  uint64_t capacity);
bool8_t vkr_editor_workbench_rename(VkrEditorUi *editor, uint32_t index,
                                    String8 name, char *message,
                                    uint64_t capacity);
void vkr_editor_workbench_rename_begin(VkrEditorUi *editor, uint32_t index);

/* Workbench `index`'s dock tree, open windows and mode: the live ones for
   the active workbench, else the stored ones. */
const VkrUiDockTree *vkr_editor_workbench_layout(const VkrEditorUi *editor,
                                                 const VkrSampleUiFrame *frame,
                                                 uint32_t index);
uint32_t vkr_editor_workbench_windows(const VkrEditorUi *editor,
                                      uint32_t index);
VkrEditorWorkbenchMode vkr_editor_workbench_mode(const VkrEditorUi *editor,
                                                 const VkrSampleUiFrame *frame,
                                                 uint32_t index);
/* The persisted and reported names of a Scene tool and a Snapping target. */
const char *vkr_editor_scene_tool_name(VkrEditorSceneTool tool);
const char *vkr_editor_snap_name(VkrEditorSnapTarget snap);

/* Restores the active workbench's built-in dock tree. */
void vkr_editor_workbench_reset(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
/* Restores workbench `index`'s built-in dock tree, the live one when it is
   active. */
void vkr_editor_workbench_reset_index(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      uint32_t index);

/* The tab row under the top bar. */
void vkr_editor_workbench_build_row(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);

/* The Tools dock panel: the active workbench's palette. */
void vkr_editor_workbench_tools_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      VkrUiRect bounds);

/* Every workbench as one JSON object. The active one's layout is `live` and
   its windows are the open ones. */
bool8_t vkr_editor_workbench_write_json(const VkrEditorUi *editor,
                                        const VkrUiDockTree *live,
                                        VkrJsonWriter *writer);

/* Reads what vkr_editor_workbench_write_json wrote and puts the active
   workbench's layout into `live` and opens its windows. A workbench without
   a valid layout keeps its built-in one. The active workbench's mode stays
   as the caller's other settings left it. False, changing nothing, for JSON
   that is not a workbench object. */
bool8_t vkr_editor_workbench_read_json(VkrEditorUi *editor, String8 json,
                                       VkrUiDockTree *live);

/* The legacy layout file: the workbench object, or a bare dock tree that
   becomes General's layout. */
bool8_t vkr_editor_workbench_load_file(VkrEditorUi *editor, VkrUiDockTree *live,
                                       String8 path);
bool8_t vkr_editor_workbench_save_file(const VkrEditorUi *editor,
                                       const VkrUiDockTree *live, String8 path);
