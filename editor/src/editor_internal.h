#pragma once

#include "editor_ui.h"

#define VKR_EDITOR_SCENE_TOOLBAR_LAYER 1u
#define VKR_EDITOR_VIEW_TOOLBAR_LAYER 2u
#define VKR_EDITOR_NAVIGATION_HEIGHT_PT VKR_UI_DOCK_TOOLBAR_PT
/* Context menus and the color picker sit above floating windows. */
#define VKR_EDITOR_POPUP_LAYER (VKR_EDITOR_WINDOW_COUNT + 3u)
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
  /* Build menu (docs/proposals/project-packaging.md). */
  CMD_BUILD,
  CMD_BUILD_RUN,
  CMD_BUILD_SETTINGS,
  CMD_BUILD_OPEN,
  CMD_BUILD_LOG,
  CMD_SCENE_BAKE,
  /* The Script editor window (ADR-079). */
  CMD_SCRIPT_EDITOR,
  /* Agent changes awaiting review (docs/proposals/level-design-toolkit.md). */
  CMD_CHANGES,
  /* Brush drawing in the Scene (B). */
  CMD_BRUSH_DRAW,
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
/* Asks the runtime to lift the ground grid onto the surface at the Scene's
   centre (VkrSampleGridFitRequest). */
void vkr_editor_view_fit_grid(const VkrSampleUiFrame *frame);
/* Add a document showing the World and switch to it; switch documents. */
bool8_t vkr_editor_viewport_tab_new(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame);
bool8_t vkr_editor_viewport_tab_show(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     uint32_t tab);
/* Where an object spawned at a viewport pixel lands under the Snapping
   settings; false without a valid view. `base` is the distance from the
   object's origin down to its lowest point, so it rests on the snap point
   instead of sinking into it. */
bool8_t vkr_editor_viewport_place(const VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame, Vec2 pixel,
                                  float32_t base, VkrEditorDropPose *out);
/* Rests an existing object on what lies below it under the Snapping
   settings: the first collision surface, else the ground plane (Surface), the
   nearest grid point (Grid) or the ground plane (Free). Requests an undoable
   transform edit; false with `message` set when nothing lies below or the
   object cannot move. */
bool8_t vkr_editor_viewport_snap(const VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame,
                                 VkrEntityId entity, char *message,
                                 uint64_t message_size);
/* Viewport document tabs drawn in `strip`, the Scene tab bar's free space. */
void vkr_editor_viewport_tabs_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect strip);
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

/* Containers (ADR-076): the scene and journal that own `entity`, the root
 * World for its id and the active scene otherwise; a frame view with them
 * substituted lets single-scene panels edit either container. */
const VkrScene *vkr_editor_entity_scene(const VkrSampleUiFrame *frame,
                                        VkrEntityId entity);
VkrSampleUiFrame vkr_editor_entity_frame(const VkrSampleUiFrame *frame,
                                         VkrEntityId entity);

/* Show a short notification; replaces any visible one. */
void vkr_editor_toast(VkrEditorUi *editor, VkrUiIcon icon, Vec4 color,
                      const char *text);
void vkr_editor_toasts_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* Right-click menus (VkrEditorContextKind): open one of `kind` at a point;
   the caller then sets the target it acts on, such as `context_entity`. */
void vkr_editor_context_open(VkrEditorUi *editor, VkrEditorContextKind kind,
                             Vec2 position_pt);
/* Open the choice menu a Details enum dropdown asked for during the build
   that just ran, if any; call once after each Details panel. */
void vkr_editor_context_open_choice(VkrEditorUi *editor,
                                    struct VkrEditorDetails *details);
/* Open the color picker a Details swatch asked for during the build that
   just ran, if any; call once after each Details panel. */
void vkr_editor_color_picker_open(VkrEditorUi *editor,
                                  struct VkrEditorDetails *details);
/* The open color picker popup; call after the panels each frame. */
void vkr_editor_color_picker_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame);
VkrUiRect vkr_editor_context_menu_rect(const VkrEditorUi *editor,
                                       const VkrUiSystem *ui);
/* The open submenu's pixels, or an empty rect. */
VkrUiRect vkr_editor_context_submenu_rect(const VkrEditorUi *editor);
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
/* Collects every [cmd] result line, newline-separated, into `buffer` until
   vkr_editor_cmd_capture_end, which returns the collected length. */
void vkr_editor_cmd_capture_begin(VkrEditorUi *editor, char *buffer,
                                  uint32_t capacity);
uint32_t vkr_editor_cmd_capture_end(VkrEditorUi *editor);
/* The queue ran every statement and no wait, hold or input step remains. */
bool8_t vkr_editor_cmd_idle(const VkrEditorUi *editor);
/** Applies the next `ui.click`, `ui.drag` or `ui.key` step to the host's
 * input. */
void vkr_editor_cmd_pointer_input(VkrEditorUi *editor, InputState *input);
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
