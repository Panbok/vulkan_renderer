#pragma once

#include "editor_ui.h"

#define VKR_EDITOR_SCENE_TOOLBAR_LAYER 1u
#define VKR_EDITOR_VIEW_TOOLBAR_LAYER 2u
#define VKR_EDITOR_NAVIGATION_HEIGHT_PT VKR_UI_DOCK_TOOLBAR_PT
/* Cmd bar suggestions sit above menus and floating windows. */
#define VKR_EDITOR_CMD_LAYER (VKR_EDITOR_WINDOW_COUNT + 4u)

/* Commands shared by the menu bar, toolbar and Cmd bar. */
typedef enum EditorCommand {
  CMD_LOAD,
  CMD_RELOAD,
  CMD_UNLOAD,
  CMD_SAVE,
  CMD_UNDO,
  CMD_REDO,
  CMD_FRAME,
  CMD_HIERARCHY,
  CMD_INSPECTOR,
  CMD_CONSOLE,
  CMD_BAKERY,
  CMD_CONTENT,
  CMD_ANIMATION,
  CMD_PHYSICS,
  CMD_RESET_LAYOUT,
  CMD_SIM_START,
  CMD_SIM_PAUSE,
  CMD_RENDER_START,
  CMD_RENDER_STOP,
  CMD_SIM_TOGGLE,
  CMD_SIM_STEP,
  CMD_SIM_RESET,
  CMD_RENDER_TOGGLE,
  CMD_CAMERA,
  CMD_GRAPHICS,
  CMD_DRAWS,
  CMD_MEMORY,
  CMD_LABELS,
  CMD_LABELS_DIRECTIONAL,
  CMD_LABELS_SPOT,
  CMD_LABELS_POINT,
  CMD_HELP,
  CMD_COMMANDS,
  CMD_ZOOM_IN,
  CMD_ZOOM_OUT,
  CMD_ZOOM_RESET,
  CMD_REDUCE_MOTION,
  CMD_COUNT
} EditorCommand;

VkrUiStyle vkr_editor_glass_style(void);
VkrUiStyle vkr_editor_overlay_style(void);
VkrUiWidgetConfig vkr_editor_text_config(float32_t size_pt, Vec4 color);
VkrUiWidgetConfig vkr_editor_menu_button_config(uint32_t column, bool8_t active,
                                                VkrFontHandle heading_font);

void vkr_editor_field_style(VkrUiWidgetConfig *config);
void vkr_editor_action_style(VkrUiWidgetConfig *config, VkrFontHandle heading);
void vkr_editor_primary_style(VkrUiWidgetConfig *config, VkrFontHandle heading);
void vkr_editor_ghost_style(VkrUiWidgetConfig *config);
void vkr_editor_toggle_style(VkrUiWidgetConfig *config, bool8_t active);
VkrUiWidgetConfig vkr_editor_icon_button_config(uint32_t column, uint32_t row,
                                                VkrUiIcon icon,
                                                String8 tooltip);
VkrUiWidgetConfig vkr_editor_section_label_config(VkrFontHandle heading);
/** Themed search field with a magnifier, placeholder and clear button.
 * Returns true when the text changed. */
bool8_t vkr_editor_search_field(VkrUiSystem *ui, String8 id,
                                VkrUiTextEditBuffer *buffer,
                                VkrUiPlacement placement, String8 placeholder,
                                String8 tooltip, VkrFontHandle font);

void vkr_editor_dock_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
void vkr_editor_labels_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);
void vkr_editor_labels_project(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_dock_show(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
void vkr_editor_dock_toggle(VkrUiDockTree *dock, VkrUiDockPanelKind kind);
/* Stats readout and render-failure card drawn over the Scene. */
void vkr_editor_scene_overlays_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame);
void vkr_editor_viewport_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_viewport_build(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame);
void vkr_editor_grid_build(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
/* Clickable world-axis indicator in the Scene's lower-left corner. */
void vkr_editor_orientation_gizmo_build(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame);
void vkr_editor_grid_project(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);
void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui);
void vkr_editor_windows_build_navigation(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame);
void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiFrame *frame);
void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui,
                                   const VkrSampleUiFrame *frame);

/* Show a short notification; replaces any visible one. */
void vkr_editor_toast(VkrEditorUi *editor, VkrUiIcon icon, Vec4 color,
                      const char *text);
void vkr_editor_toasts_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* Entity context menu opened from a Hierarchy row. */
VkrUiRect vkr_editor_context_menu_rect(const VkrEditorUi *editor,
                                       const VkrUiSystem *ui);
void vkr_editor_context_menu_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
/* Toggle an entity's own visibility through the undoable edit journal. */
void vkr_editor_toggle_visibility(const VkrSampleUiFrame *frame,
                                  VkrEntityId entity);

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
bool8_t vkr_editor_command_enabled(EditorCommand command,
                                   const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
void vkr_editor_command_execute(EditorCommand command, VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame);
void vkr_editor_window_set_visible(VkrEditorUi *editor,
                                   VkrEditorWindowKind kind, bool8_t visible);

/* Cmd bar: a typed command line with autocomplete, plus a script queue that
 * runs one command per frame (see docs/editor-cmd.md). */
/** Append `;`- or newline-separated commands; false when the queue is full. */
bool8_t vkr_editor_cmd_enqueue(VkrEditorUi *editor, const char *script);
/** Run due queued commands; call once per frame before panels build. */
void vkr_editor_cmd_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame);
/** The top-bar field, in `column` of the navigation bar grid. */
void vkr_editor_cmd_bar_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t column);
/* Vocabulary words shared by commands and the evaluator, NULL-terminated and
 * index-aligned with their value tables. */
extern const char *const vkr_editor_cmd_camera_views[];
extern const char *const vkr_editor_cmd_render_modes[];
extern const VkrRenderMode vkr_editor_cmd_render_mode_values[];
extern const char *const vkr_editor_cmd_tools[];
extern const uint32_t vkr_editor_cmd_tool_modes[];
/** Evaluate one expression statement: `expr`, `name = expr` or
 * `path = expr`. Writes the result or error to `message`; false on error. */
bool8_t vkr_editor_cmd_eval(VkrEditorUi *editor, const VkrSampleUiFrame *frame,
                            String8 line, char *message, uint32_t capacity);
/** Completions for the expression that ends `text`: names at the start of a
 * path, members after a dot. Writes whole-line completions and hints. */
uint32_t vkr_editor_cmd_eval_complete(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame,
                                      String8 text, char (*lines)[96],
                                      char (*hints)[96], uint32_t capacity);
/** Autocomplete suggestions beneath the field; build after other overlays. */
void vkr_editor_cmd_suggestions_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame);
