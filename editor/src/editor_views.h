#pragma once

#include "editor_ui.h"

/* Views (ADR-089): the Views panel, the dock tab that holds the Scene image,
 * switches between the views its workbench offers: the Scene, the selection
 * alone with its own orbit camera (Model), the open material's node graph
 * (Graph) and the Script editor (Script). One view shows at a time, so the
 * one Scene image serves Scene and Model, and nothing renders it while Graph
 * or Script shows. */

/* The views a workbench of `kind` offers until it changes them, a bit per
   VkrEditorView, and the one it shows first. */
uint32_t vkr_editor_views_builtin(VkrEditorWorkbenchKind kind);
VkrEditorView vkr_editor_view_builtin(VkrEditorWorkbenchKind kind);

/* The view's Cmd and settings word ("scene", "model", "graph", "script"),
   its label and icon. */
const char *vkr_editor_view_id(VkrEditorView view);
const char *vkr_editor_view_name(VkrEditorView view);
VkrUiIcon vkr_editor_view_icon(VkrEditorView view);
String8 vkr_editor_view_tooltip(VkrEditorView view);
/* The view a word names by id or name, else VKR_EDITOR_VIEW_COUNT. */
VkrEditorView vkr_editor_view_find(String8 word);

/* The active workbench's views and the one it shows. */
uint32_t vkr_editor_views_offered(const VkrEditorUi *editor);
VkrEditorView vkr_editor_view_active(const VkrEditorUi *editor);
/* Whether `view` shows: the active workbench shows it and the Views tab is
   the shown tab of its stack. */
bool8_t vkr_editor_view_shown(const VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame,
                              VkrEditorView view);

/* Shows `view` in workbench `workbench`, adding it to the views that
   workbench offers; in the active one the Views tab comes to the front. */
void vkr_editor_view_show(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                          uint32_t workbench, VkrEditorView view);
/* Takes a view other than Scene out of the active workbench; the Scene
   shows when it was the shown one. */
void vkr_editor_view_remove(VkrEditorUi *editor, VkrEditorView view);

/* The view whose body is window `kind`'s, else VKR_EDITOR_VIEW_COUNT. While
   the active workbench offers it, the window stays closed and opening it
   shows the view. */
VkrEditorView vkr_editor_window_view(VkrEditorWindowKind kind);

/* Runs at the start of the UI build, after a workbench switch and the
   selection update: moves an open window into its view, marks the dock's
   Scene hidden under Graph and Script, and enters, updates or leaves the
   Model view. */
void vkr_editor_views_update(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* The shown view's content over the Views panel's body `rect` in pixels:
   the graph or Script editor in place of the Scene, or the Model view's
   caption, orbit ring and Frame button over the Scene image. */
void vkr_editor_views_build_body(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame, VkrUiRect rect);
