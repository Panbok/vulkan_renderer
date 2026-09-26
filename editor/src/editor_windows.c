#include "editor_internal.h"
#include "editor_projects.h"

#include "editor_graphics.h"

#include <stdio.h>
#include <string.h>

#define EDITOR_MENU_LAYER (VKR_EDITOR_WINDOW_COUNT + 3u)
#define EDITOR_MENU_WIDTH_PT 272.0f
#define EDITOR_MENU_ROW_PT 26.0f
#define EDITOR_MENU_SEPARATOR_PT 9.0f
#define EDITOR_MENU_PADDING_PT 5.0f
#define EDITOR_TRANSPORT_BUTTON_PT 27.0f

#if defined(PLATFORM_APPLE)
#define EDITOR_SHORTCUT(apple, other) apple
#else
#define EDITOR_SHORTCUT(apple, other) other
#endif

/* One shortcut per line; tabs split the key column from its action. */
// clang-format off
static const char s_help_text[] =
    EDITOR_SHORTCUT("\xe2\x8c\x98P", "Ctrl+P") "\tCmd bar: commands and expressions\n"
    "Q  W  E  R\tSelect, move, rotate, scale tools\n"
    "F\tFrame the selection\n"
    "Hold RMB\tFly the Scene camera (WASD)\n"
    "F3 / Tab\tToggle free camera; Esc releases\n"
    EDITOR_SHORTCUT("\xe2\x8c\x98S", "Ctrl+S") "\tSave scene edits\n"
    EDITOR_SHORTCUT("\xe2\x8c\x98Z / \xe2\x87\xa7\xe2\x8c\x98Z", "Ctrl+Z / Ctrl+Shift+Z") "\tUndo / redo\n"
    EDITOR_SHORTCUT("\xe2\x8c\x83Space", "Ctrl+Space") "\tContent browser\n"
    "Drag X / Y / Z\tScrub a value (Shift fast, Alt fine)\n"
    "F6\tCycle shadow diagnostics\n"
    "F8 / F9 / F10\tIBL mode and intensity\n"
    "G\tCamera snapshot";
// clang-format on

VkrUiWidgetConfig vkr_editor_menu_button_config(uint32_t column, bool8_t active,
                                                VkrFontHandle heading_font) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig button = vkr_ui_widget_config_default();
  button.placement = (VkrUiPlacement){
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_CENTER,
  };
  vkr_editor_ghost_style(&button);
  button.style.padding_pt = (VkrUiEdges){4.0f, 9.0f, 4.0f, 9.0f};
  button.style.min_size_pt.y = 26.0f;
  button.style.font_size_pt = theme->font_body;
  button.text.font = heading_font;
  button.style.text_color = active ? theme->text : theme->text_secondary;
  if (active)
    button.style.background_color = theme->raised_hover;
  return button;
}

static bool8_t editor_point_in_rect(int32_t x, int32_t y, VkrUiRect rect) {
  return (float32_t)x >= rect.x && (float32_t)x < rect.x + rect.width &&
         (float32_t)y >= rect.y && (float32_t)y < rect.y + rect.height;
}

static void editor_window_raise(VkrEditorUi *editor, VkrEditorWindowKind kind) {
  VkrEditorWindowState *window = &editor->windows[kind];
  const uint32_t old_z = window->z_order;
  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    if (editor->windows[i].z_order > old_z)
      editor->windows[i].z_order--;
  }
  window->z_order = VKR_EDITOR_WINDOW_COUNT;
  window->visible = true_v;
}

static void editor_window_toggle(VkrEditorUi *editor,
                                 VkrEditorWindowKind kind) {
  if (editor->windows[kind].visible)
    editor->windows[kind].visible = false_v;
  else
    editor_window_raise(editor, kind);
}

void vkr_editor_window_set_visible(VkrEditorUi *editor,
                                   VkrEditorWindowKind kind, bool8_t visible) {
  if (visible)
    editor_window_raise(editor, kind);
  else
    editor->windows[kind].visible = false_v;
}

/* ---- Commands shared by the menu bar, toolbar and Cmd bar ---- */

typedef struct EditorCommandInfo {
  const char *name;
  VkrUiIcon icon;
  /* Displayed shortcut; NULL when the command has none. */
  const char *shortcut;
  /* Toggles that duplicate a Start/Stop pair; the Cmd bar uses the pair. */
  bool8_t menu_only;
} EditorCommandInfo;

static const EditorCommandInfo s_commands[CMD_COUNT] = {
    [CMD_LOAD] = {"Load scene", VKR_UI_ICON_SCENE_LOAD, NULL, false_v},
    [CMD_RELOAD] = {"Reload scene", VKR_UI_ICON_REFRESH, NULL, false_v},
    [CMD_UNLOAD] = {"Unload scene", VKR_UI_ICON_SCENE_UNLOAD, NULL, false_v},
    [CMD_SAVE] = {"Save scene edits", VKR_UI_ICON_SAVE,
                  EDITOR_SHORTCUT("\xe2\x8c\x98S", "Ctrl+S"), false_v},
    [CMD_UNDO] = {"Undo", VKR_UI_ICON_UNDO,
                  EDITOR_SHORTCUT("\xe2\x8c\x98Z", "Ctrl+Z"), false_v},
    [CMD_REDO] = {"Redo", VKR_UI_ICON_REDO,
                  EDITOR_SHORTCUT("\xe2\x87\xa7\xe2\x8c\x98Z", "Ctrl+Shift+Z"),
                  false_v},
    [CMD_FRAME] = {"Frame selected", VKR_UI_ICON_FRAME, NULL, false_v},
    [CMD_HIERARCHY] = {"Hierarchy", VKR_UI_ICON_HIERARCHY, NULL, false_v},
    [CMD_INSPECTOR] = {"Inspector", VKR_UI_ICON_INSPECTOR, NULL, false_v},
    [CMD_CONSOLE] = {"Console", VKR_UI_ICON_CONSOLE, NULL, false_v},
    [CMD_BAKERY] = {"Bakery", VKR_UI_ICON_BAKERY, NULL, false_v},
    [CMD_CONTENT] = {"Content browser", VKR_UI_ICON_CONTENT,
                     EDITOR_SHORTCUT("\xe2\x8c\x83Space", "Ctrl+Space"),
                     false_v},
    [CMD_ANIMATION] = {"Animation editor", VKR_UI_ICON_ANIMATION, NULL,
                       false_v},
    [CMD_PHYSICS] = {"Physics settings", VKR_UI_ICON_PHYSICS, NULL, false_v},
    [CMD_RESET_LAYOUT] = {"Reset panel layout", VKR_UI_ICON_LAYOUT, NULL,
                          false_v},
    [CMD_SIM_START] = {"Start simulation", VKR_UI_ICON_PLAY, NULL, false_v},
    [CMD_SIM_PAUSE] = {"Pause simulation", VKR_UI_ICON_PAUSE, NULL, false_v},
    [CMD_RENDER_START] = {"Start scene rendering", VKR_UI_ICON_MONITOR_PLAY,
                          NULL, false_v},
    [CMD_RENDER_STOP] = {"Stop scene rendering", VKR_UI_ICON_MONITOR_STOP, NULL,
                         false_v},
    [CMD_SIM_TOGGLE] = {"Play / Pause", VKR_UI_ICON_PLAY, NULL, true_v},
    [CMD_SIM_STEP] = {"Step one frame", VKR_UI_ICON_STEP, NULL, false_v},
    [CMD_SIM_RESET] = {"Stop and reset simulation", VKR_UI_ICON_STOP, NULL,
                       false_v},
    [CMD_RENDER_TOGGLE] = {"Live scene rendering", VKR_UI_ICON_MONITOR_PLAY,
                           NULL, true_v},
    [CMD_CAMERA] = {"Free camera", VKR_UI_ICON_CAMERA, "F3", false_v},
    [CMD_GRAPHICS] = {"Graphics settings", VKR_UI_ICON_GRAPHICS, NULL, false_v},
    [CMD_DRAWS] = {"Draws and render graph", VKR_UI_ICON_DRAWS, NULL, false_v},
    [CMD_MEMORY] = {"Memory", VKR_UI_ICON_MEMORY, NULL, false_v},
    [CMD_LABELS] = {"Light icons", VKR_UI_ICON_LIGHT, NULL, false_v},
    [CMD_LABELS_DIRECTIONAL] = {"Directional light icons",
                                VKR_UI_ICON_DIRECTIONAL_LIGHT, NULL, false_v},
    [CMD_LABELS_SPOT] = {"Spot light icons", VKR_UI_ICON_SPOT_LIGHT, NULL,
                         false_v},
    [CMD_LABELS_POINT] = {"Point light icons", VKR_UI_ICON_POINT_LIGHT, NULL,
                          false_v},
    [CMD_HELP] = {"Keyboard and mouse controls", VKR_UI_ICON_KEYBOARD, NULL,
                  false_v},
    [CMD_COMMANDS] = {"Cmd bar", VKR_UI_ICON_COMMAND,
                      EDITOR_SHORTCUT("\xe2\x8c\x98P", "Ctrl+P"), true_v},
    [CMD_ZOOM_IN] = {"Zoom interface in", VKR_UI_ICON_ZOOM_IN,
                     EDITOR_SHORTCUT("\xe2\x8c\x98=", "Ctrl+="), false_v},
    [CMD_ZOOM_OUT] = {"Zoom interface out", VKR_UI_ICON_ZOOM_OUT,
                      EDITOR_SHORTCUT("\xe2\x8c\x98-", "Ctrl+-"), false_v},
    [CMD_ZOOM_RESET] = {"Actual interface size", VKR_UI_ICON_MAXIMIZE,
                        EDITOR_SHORTCUT("\xe2\x8c\x98"
                                        "0",
                                        "Ctrl+0"),
                        false_v},
    [CMD_REDUCE_MOTION] = {"Reduce motion", VKR_UI_ICON_SPARKLE, NULL, false_v},
};

static bool8_t editor_panel_visible(const VkrSampleUiFrame *frame,
                                    VkrUiDockPanelKind kind) {
  return vkr_ui_dock_find_panel(frame->dock, kind, NULL, NULL);
}

bool8_t vkr_editor_command_enabled(EditorCommand command,
                                   const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  switch (command) {
  case CMD_LOAD:
    return frame->scene == NULL && !frame->scene_loading;
  case CMD_RELOAD:
  case CMD_UNLOAD:
  case CMD_SAVE:
  case CMD_SIM_RESET:
    return frame->scene != NULL;
  case CMD_UNDO:
    return frame->scene && frame->edits->undo_cursor > 0;
  case CMD_REDO:
    return frame->scene && frame->edits->undo_cursor < frame->edits->undo_count;
  case CMD_FRAME:
    return frame->scene &&
           vkr_scene_entity_alive(frame->scene, frame->selected_entity);
  case CMD_SIM_START:
    return !frame->simulation_running;
  case CMD_SIM_PAUSE:
    return frame->simulation_running;
  case CMD_SIM_STEP:
    return frame->scene && !frame->simulation_running;
  case CMD_RENDER_START:
    return frame->scene_rendering_stopped;
  case CMD_RENDER_STOP:
    return !frame->scene_rendering_stopped;
  case CMD_CAMERA:
    return frame->mapping_valid && !frame->scene_rendering_stopped;
  case CMD_LABELS_DIRECTIONAL:
  case CMD_LABELS_SPOT:
  case CMD_LABELS_POINT:
    return editor->labels_enabled;
  case CMD_ZOOM_IN:
    return frame->ui->user_scale < VKR_UI_USER_SCALE_MAX;
  case CMD_ZOOM_OUT:
    return frame->ui->user_scale > VKR_UI_USER_SCALE_MIN;
  case CMD_ZOOM_RESET:
    return frame->ui->user_scale != 1.0f;
  default:
    return true_v;
  }
}

/* 1 checked, 0 unchecked, -1 for commands that are not toggles. */
static int32_t editor_command_checked(EditorCommand command,
                                      const VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  switch (command) {
  case CMD_HIERARCHY:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_HIERARCHY);
  case CMD_INSPECTOR:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_INSPECTOR);
  case CMD_CONSOLE:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_CONSOLE);
  case CMD_BAKERY:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_BAKERY);
  case CMD_CONTENT:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_CONTENT);
  case CMD_ANIMATION:
    return editor->windows[VKR_EDITOR_WINDOW_ANIMATION].visible;
  case CMD_PHYSICS:
    return editor->windows[VKR_EDITOR_WINDOW_PHYSICS].visible;
  case CMD_GRAPHICS:
    return editor->windows[VKR_EDITOR_WINDOW_GRAPHICS].visible;
  case CMD_DRAWS:
    return editor->windows[VKR_EDITOR_WINDOW_DRAWS].visible;
  case CMD_MEMORY:
    return editor->windows[VKR_EDITOR_WINDOW_MEMORY].visible;
  case CMD_HELP:
    return editor->windows[VKR_EDITOR_WINDOW_HELP].visible;
  case CMD_RENDER_TOGGLE:
    return !frame->scene_rendering_stopped;
  case CMD_CAMERA:
    return frame->mouse_captured;
  case CMD_LABELS:
    return editor->labels_enabled;
  case CMD_LABELS_DIRECTIONAL:
    return editor->labels_directional;
  case CMD_LABELS_SPOT:
    return editor->labels_spot;
  case CMD_LABELS_POINT:
    return editor->labels_point;
  case CMD_REDUCE_MOTION:
    return frame->ui->reduce_motion;
  default:
    return -1;
  }
}

void vkr_editor_command_execute(EditorCommand command, VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  static const VkrSceneEditAction actions[] = {
      VKR_SCENE_EDIT_LOAD, VKR_SCENE_EDIT_RELOAD, VKR_SCENE_EDIT_UNLOAD,
      VKR_SCENE_EDIT_SAVE, VKR_SCENE_EDIT_UNDO,   VKR_SCENE_EDIT_REDO,
      VKR_SCENE_EDIT_FRAME};
  if (command <= CMD_FRAME) {
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = actions[command], .entity = frame->selected_entity};
    return;
  }
  switch (command) {
  case CMD_HIERARCHY:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_HIERARCHY);
    break;
  case CMD_INSPECTOR:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_INSPECTOR);
    break;
  case CMD_CONSOLE:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_CONSOLE);
    break;
  case CMD_BAKERY:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_BAKERY);
    break;
  case CMD_CONTENT:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_CONTENT);
    break;
  case CMD_PHYSICS:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_PHYSICS);
    break;
  case CMD_ANIMATION:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_ANIMATION);
    break;
  case CMD_GRAPHICS:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_GRAPHICS);
    break;
  case CMD_DRAWS:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_DRAWS);
    break;
  case CMD_MEMORY:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_MEMORY);
    break;
  case CMD_HELP:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_HELP);
    break;
  case CMD_RESET_LAYOUT:
    vkr_ui_dock_default_editor_layout(frame->dock);
    break;
  case CMD_SIM_START:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_SIMULATION;
    break;
  case CMD_SIM_PAUSE:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION;
    break;
  case CMD_SIM_TOGGLE:
    *frame->transport_action = frame->simulation_running
                                   ? VKR_SAMPLE_TRANSPORT_PAUSE_SIMULATION
                                   : VKR_SAMPLE_TRANSPORT_START_SIMULATION;
    break;
  case CMD_SIM_STEP:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_STEP_SIMULATION;
    break;
  case CMD_SIM_RESET:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_RESET_SIMULATION;
    break;
  case CMD_RENDER_START:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_START_RENDERING;
    break;
  case CMD_RENDER_STOP:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_STOP_RENDERING;
    break;
  case CMD_RENDER_TOGGLE:
    *frame->transport_action = frame->scene_rendering_stopped
                                   ? VKR_SAMPLE_TRANSPORT_START_RENDERING
                                   : VKR_SAMPLE_TRANSPORT_STOP_RENDERING;
    break;
  case CMD_CAMERA:
    *frame->transport_action = VKR_SAMPLE_TRANSPORT_TOGGLE_CAMERA;
    break;
  case CMD_LABELS:
    editor->labels_enabled = !editor->labels_enabled;
    break;
  case CMD_LABELS_DIRECTIONAL:
    editor->labels_directional = !editor->labels_directional;
    break;
  case CMD_LABELS_SPOT:
    editor->labels_spot = !editor->labels_spot;
    break;
  case CMD_LABELS_POINT:
    editor->labels_point = !editor->labels_point;
    break;
  case CMD_ZOOM_IN:
    vkr_ui_system_set_user_scale(frame->ui, frame->ui->user_scale + 0.1f);
    break;
  case CMD_ZOOM_OUT:
    vkr_ui_system_set_user_scale(frame->ui, frame->ui->user_scale - 0.1f);
    break;
  case CMD_ZOOM_RESET:
    vkr_ui_system_set_user_scale(frame->ui, 1.0f);
    break;
  case CMD_REDUCE_MOTION:
    frame->ui->reduce_motion = !frame->ui->reduce_motion;
    break;
  case CMD_COMMANDS:
    editor->cmd_focus_request = true_v;
    break;
  default:
    break;
  }
}

/* ---- Menu bar ---- */

typedef struct EditorMenuEntry {
  EditorCommand command;
  bool8_t separator_before;
  bool8_t indent;
} EditorMenuEntry;

static const EditorMenuEntry s_file_menu[] = {
    {CMD_LOAD},
    {CMD_RELOAD},
    {CMD_UNLOAD},
    {CMD_SAVE, true_v},
};
static const EditorMenuEntry s_edit_menu[] = {
    {CMD_UNDO},
    {CMD_REDO},
    {CMD_FRAME, true_v},
    {CMD_COMMANDS, true_v},
};
static const EditorMenuEntry s_view_menu[] = {
    {CMD_HIERARCHY},
    {CMD_INSPECTOR},
    {CMD_CONTENT},
    {CMD_CONSOLE},
    {CMD_BAKERY},
    {CMD_ANIMATION, true_v},
    {CMD_PHYSICS},
    {CMD_GRAPHICS},
    {CMD_DRAWS, true_v},
    {CMD_MEMORY},
    {CMD_LABELS, true_v},
    {CMD_LABELS_DIRECTIONAL, false_v, true_v},
    {CMD_LABELS_SPOT, false_v, true_v},
    {CMD_LABELS_POINT, false_v, true_v},
    {CMD_ZOOM_IN, true_v},
    {CMD_ZOOM_OUT},
    {CMD_ZOOM_RESET},
    {CMD_REDUCE_MOTION},
    {CMD_RESET_LAYOUT, true_v},
};
static const EditorMenuEntry s_scene_menu[] = {
    {CMD_SIM_TOGGLE}, {CMD_SIM_STEP},
    {CMD_SIM_RESET},  {CMD_RENDER_TOGGLE, true_v},
    {CMD_CAMERA},
};
static const EditorMenuEntry s_help_menu[] = {
    {CMD_HELP},
    {CMD_COMMANDS},
};

typedef struct EditorMenuDefinition {
  const char *title;
  const EditorMenuEntry *entries;
  uint32_t count;
} EditorMenuDefinition;

static const EditorMenuDefinition s_menus[VKR_EDITOR_MENU_COUNT] = {
    [VKR_EDITOR_MENU_FILE] = {"File", s_file_menu, ArrayCount(s_file_menu)},
    [VKR_EDITOR_MENU_EDIT] = {"Edit", s_edit_menu, ArrayCount(s_edit_menu)},
    [VKR_EDITOR_MENU_VIEW] = {"View", s_view_menu, ArrayCount(s_view_menu)},
    [VKR_EDITOR_MENU_SCENE] = {"Scene", s_scene_menu, ArrayCount(s_scene_menu)},
    [VKR_EDITOR_MENU_HELP] = {"Help", s_help_menu, ArrayCount(s_help_menu)},
};

static float32_t editor_menu_height_pt(VkrEditorMenu menu) {
  const EditorMenuDefinition *definition = &s_menus[menu];
  float32_t height = EDITOR_MENU_PADDING_PT * 2.0f;
  for (uint32_t i = 0u; i < definition->count; ++i)
    height += EDITOR_MENU_ROW_PT + (definition->entries[i].separator_before
                                        ? EDITOR_MENU_SEPARATOR_PT
                                        : 0.0f);
  return height;
}

/* Popups open beneath their menu-bar button and stay inside the window. */
static VkrUiRect editor_menu_popup_rect(const VkrEditorUi *editor,
                                        const VkrUiSystem *ui) {
  if (editor->menu == VKR_EDITOR_MENU_NONE ||
      editor->menu >= VKR_EDITOR_MENU_COUNT)
    return (VkrUiRect){0};
  const float32_t scale = ui->content_scale;
  const float32_t width = EDITOR_MENU_WIDTH_PT * scale;
  const float32_t height = editor_menu_height_pt(editor->menu) * scale;
  const float32_t x = Min(editor->menu_anchor_px.x,
                          Max(0.0f, (float32_t)ui->target_width - width));
  const float32_t y =
      editor->menu_anchor_px.y + editor->menu_anchor_px.height + 3.0f * scale;
  return (VkrUiRect){x, y, width,
                     Min(height, Max(0.0f, (float32_t)ui->target_height - y))};
}

static void editor_menu_close(VkrEditorUi *editor, VkrUiSystem *ui) {
  editor->menu = VKR_EDITOR_MENU_NONE;
  ui->focused_id = ui->active_id = VKR_UI_ID_NONE;
  (void)vkr_ui_keyboard_layer_set(ui, 0u);
}

void vkr_editor_windows_build_menu(VkrEditorUi *editor, VkrUiSystem *ui,
                                   const VkrSampleUiFrame *frame) {
  if (editor->menu == VKR_EDITOR_MENU_NONE ||
      editor->menu >= VKR_EDITOR_MENU_COUNT)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  const EditorMenuDefinition *definition = &s_menus[editor->menu];
  vkr_ui_keyboard_layer_set(ui, EDITOR_MENU_LAYER);
  (void)vkr_ui_input_layer_set(ui, EDITOR_MENU_LAYER);
  const VkrUiRect rect = editor_menu_popup_rect(editor, ui);
  VkrUiTrack rows[32];
  uint32_t row_count = 0u;
  for (uint32_t i = 0u; i < definition->count && row_count + 2u < 32u; ++i) {
    if (definition->entries[i].separator_before)
      rows[row_count++] = (VkrUiTrack){.value = EDITOR_MENU_SEPARATOR_PT,
                                       .unit = VKR_UI_TRACK_PX};
    rows[row_count++] =
        (VkrUiTrack){.value = EDITOR_MENU_ROW_PT, .unit = VKR_UI_TRACK_PX};
  }
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {rect.y / ui->content_scale, 0.0f, 0.0f,
                    rect.x / ui->content_scale},
  };
  popup.columns = &one_track;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = row_count;
  popup.style = vkr_editor_glass_style();
  popup.style.padding_pt =
      (VkrUiEdges){EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT,
                   EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT};
  popup.style.gap_pt = 0.0f;
  popup.style.min_size_pt =
      (Vec2){rect.width / ui->content_scale, rect.height / ui->content_scale};
  popup.style.max_size_pt = popup.style.min_size_pt;
  popup.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.menu.popup"), &popup)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }
  EditorCommand execute = CMD_COUNT;
  uint32_t row = 0u;
  for (uint32_t i = 0u; i < definition->count; ++i) {
    const EditorMenuEntry *entry = &definition->entries[i];
    if (entry->separator_before) {
      VkrUiPanelConfig separator = vkr_ui_panel_config_default();
      separator.placement.column = 0u;
      separator.placement.row = row++;
      separator.placement.align = VKR_UI_ALIGN_CENTER;
      separator.placement.margin_pt = (VkrUiEdges){0, 6, 0, 6};
      separator.style.min_size_pt.y = 1.0f;
      separator.style.max_size_pt.y = 1.0f;
      separator.style.background_color = theme->border;
      (void)vkr_ui_push_id_u64(ui, 1000u + i);
      if (vkr_ui_panel_begin(ui, string8_lit("separator"), &separator))
        (void)vkr_ui_panel_end(ui);
      (void)vkr_ui_pop_id(ui);
    }
    const EditorCommandInfo *info = &s_commands[entry->command];
    const int32_t checked =
        editor_command_checked(entry->command, editor, frame);
    VkrUiWidgetConfig item = vkr_ui_widget_config_default();
    item.placement.column = 0u;
    item.placement.row = row;
    item.placement.justify = VKR_UI_ALIGN_STRETCH;
    item.placement.align = VKR_UI_ALIGN_STRETCH;
    /* The whole row hovers and clicks, not just the button's content. */
    item.fill = true_v;
    vkr_editor_ghost_style(&item);
    item.style.hover_background_color = theme->accent;
    item.style.active_background_color = theme->accent_active;
    item.style.text_color = theme->text;
    item.style.font_size_pt = theme->font_body;
    item.style.padding_pt =
        (VkrUiEdges){3.0f, 8.0f, 3.0f, entry->indent ? 26.0f : 8.0f};
    item.icon = checked > 0    ? VKR_UI_ICON_CHECK
                : checked == 0 ? VKR_UI_ICON_NONE
                               : info->icon;
    item.icon_size_pt = 14.0f;
    item.icon_color = checked > 0 ? theme->accent_hover : theme->text_secondary;
    item.disabled = !vkr_editor_command_enabled(entry->command, editor, frame);
    String8 name = string8_create((uint8_t *)info->name, strlen(info->name));
    if (entry->command == CMD_SIM_TOGGLE)
      name = frame->simulation_running ? string8_lit("Pause simulation")
                                       : string8_lit("Play simulation");
    /* Unchecked toggles keep their label aligned with checked siblings. */
    if (item.icon == VKR_UI_ICON_NONE)
      item.style.padding_pt.left += 20.0f;
    (void)vkr_ui_push_id_u64(ui, entry->command);
    item.placement.justify = VKR_UI_ALIGN_STRETCH;
    const VkrUiId item_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("item"));
    /* The row button is only the hit area; the label draws icon and text. */
    VkrUiWidgetConfig hit = item;
    hit.icon = VKR_UI_ICON_NONE;
    if (vkr_ui_button(ui, string8_lit("item"), (String8){0}, &hit))
      execute = entry->command;
    const bool8_t hot = ui->hot_id == item_id && !item.disabled;
    VkrUiWidgetConfig text = item;
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_CENTER;
    text.style.background_color = (Vec4){0};
    text.style.text_color = hot ? theme->text_on_accent : theme->text;
    if (hot)
      text.icon_color = theme->text_on_accent;
    vkr_ui_label(ui, string8_lit("label"), name, &text);
    if (info->shortcut) {
      VkrUiWidgetConfig shortcut = vkr_editor_text_config(
          theme->font_caption,
          hot ? theme->text_on_accent : theme->text_secondary);
      shortcut.placement.column = 0u;
      shortcut.placement.row = row;
      shortcut.placement.justify = VKR_UI_ALIGN_END;
      shortcut.placement.align = VKR_UI_ALIGN_CENTER;
      shortcut.placement.margin_pt.right = 10.0f;
      shortcut.disabled = item.disabled;
      vkr_ui_label(
          ui, string8_lit("shortcut"),
          string8_create((uint8_t *)info->shortcut, strlen(info->shortcut)),
          &shortcut);
    }
    (void)vkr_ui_pop_id(ui);
    row++;
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
  if (execute != CMD_COUNT) {
    editor_menu_close(editor, ui);
    vkr_editor_command_execute(execute, editor, frame);
  }
}

/* ---- Top bar: menus, quick actions, transport and status ---- */

static void editor_top_icon_button(VkrEditorUi *editor, VkrUiSystem *ui,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t column, EditorCommand command) {
  const EditorCommandInfo *info = &s_commands[command];
  const String8 tooltip =
      info->shortcut
          ? string8_create_formatted(ui->frame_allocator, "%s  (%s)",
                                     info->name, info->shortcut)
          : string8_create((uint8_t *)info->name, strlen(info->name));
  VkrUiWidgetConfig button =
      vkr_editor_icon_button_config(column, 0u, info->icon, tooltip);
  button.disabled = !vkr_editor_command_enabled(command, editor, frame);
  if (command == CMD_SAVE && frame->scene && frame->edits &&
      frame->edits->revision != frame->edits->saved_revision)
    button.icon_color = vkr_ui_theme()->warning;
  (void)vkr_ui_push_id_u64(ui, command);
  if (vkr_ui_button(ui, string8_lit("quick"), (String8){0}, &button))
    vkr_editor_command_execute(command, editor, frame);
  (void)vkr_ui_pop_id(ui);
}

/* Windows draws no caption with a unified title bar, so the top bar ends in
 * minimize, maximize/restore and close. macOS keeps its traffic lights. */
static void editor_caption_buttons_build(const VkrSampleUiFrame *frame,
                                         uint32_t first_column) {
  if (!vkr_window_draws_caption_buttons(frame->window))
    return;
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t maximized = vkr_window_is_maximized(frame->window);
  static const char *const tips[] = {"Minimize", "Maximize", "Close"};
  const VkrUiIcon icons[] = {VKR_UI_ICON_MINUS,
                             maximized ? VKR_UI_ICON_COPY : VKR_UI_ICON_SQUARE,
                             VKR_UI_ICON_CLOSE};
  for (uint32_t i = 0; i < ArrayCount(icons); ++i) {
    VkrUiWidgetConfig button = vkr_editor_icon_button_config(
        first_column + i, 0u, icons[i],
        string8_create_from_cstr(
            (const uint8_t *)(i == 1 && maximized ? "Restore" : tips[i]),
            strlen(i == 1 && maximized ? "Restore" : tips[i])));
    button.style.min_size_pt = button.style.max_size_pt =
        (Vec2){40.0f, VKR_EDITOR_NAVIGATION_HEIGHT_PT - 8.0f};
    button.style.corner_radius_pt = (Vec4){0};
    button.icon_size_pt = 14.0f;
    if (i == 2) {
      button.style.hover_background_color = theme->error;
      button.style.active_background_color =
          vkr_ui_color_mix(theme->error, (Vec4){0, 0, 0, 1}, 0.2f);
    }
    (void)vkr_ui_push_id_u64(ui, i);
    if (vkr_ui_button(ui, string8_lit("caption"), (String8){0}, &button)) {
      if (i == 0)
        vkr_window_minimize(frame->window);
      else if (i == 1)
        vkr_window_toggle_maximize(frame->window);
      else
        vkr_window_request_close(frame->window);
    }
    (void)vkr_ui_pop_id(ui);
  }
}

/* Centered transport: play/pause, step, stop, live rendering and camera. */
static void editor_transport_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t button = EDITOR_TRANSPORT_BUTTON_PT;
  const float32_t separator = 9.0f;
  const float32_t padding = 3.0f;
  const float32_t width = button * 5.0f + separator + padding * 2.0f;
  const float32_t height = button + padding * 2.0f;
  const float32_t screen_w = (float32_t)ui->target_width / ui->content_scale;
  if (screen_w < 760.0f)
    return;
  const VkrUiTrack columns[] = {
      {.value = button, .unit = VKR_UI_TRACK_PX},
      {.value = button, .unit = VKR_UI_TRACK_PX},
      {.value = button, .unit = VKR_UI_TRACK_PX},
      {.value = separator, .unit = VKR_UI_TRACK_PX},
      {.value = button, .unit = VKR_UI_TRACK_PX},
      {.value = button, .unit = VKR_UI_TRACK_PX},
  };
  const VkrUiTrack row = {.value = button, .unit = VKR_UI_TRACK_PX};
  VkrUiPanelConfig group = vkr_ui_panel_config_default();
  group.placement = VKR_UI_PLACEMENT_DEFAULT;
  group.placement.column = group.placement.row = 0u;
  group.placement.justify = group.placement.align = VKR_UI_ALIGN_START;
  group.placement.margin_pt =
      (VkrUiEdges){Max(0.0f, (VKR_EDITOR_NAVIGATION_HEIGHT_PT - height) * 0.5f),
                   0, 0, (screen_w - width) * 0.5f};
  group.columns = columns;
  group.column_count = ArrayCount(columns);
  group.rows = &row;
  group.row_count = 1u;
  group.style.padding_pt = (VkrUiEdges){padding, padding, padding, padding};
  group.style.background_color = theme->field;
  group.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
  group.style.border_color = theme->border;
  group.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, theme->radius_large,
             theme->radius_large};
  group.style.min_size_pt = group.style.max_size_pt = (Vec2){width, height};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.transport"), &group))
    return;
  const bool8_t running = frame->simulation_running;
  VkrUiWidgetConfig play = vkr_editor_icon_button_config(
      0u, 0u, running ? VKR_UI_ICON_PAUSE : VKR_UI_ICON_PLAY,
      running ? string8_lit("Pause simulation")
              : string8_lit("Play simulation"));
  play.style.min_size_pt = play.style.max_size_pt = (Vec2){button, button};
  play.icon_size_pt = 16.0f;
  play.icon_color = running ? theme->warning : theme->success;
  if (running) {
    play.style.background_color = vkr_ui_color_alpha(theme->success, 0.2f);
    play.style.hover_background_color =
        vkr_ui_color_alpha(theme->success, 0.3f);
  }
  if (vkr_ui_button(ui, string8_lit("play"), (String8){0}, &play))
    vkr_editor_command_execute(CMD_SIM_TOGGLE, editor, frame);
  VkrUiWidgetConfig step = vkr_editor_icon_button_config(
      1u, 0u, VKR_UI_ICON_STEP, string8_lit("Step one frame"));
  step.style.min_size_pt = step.style.max_size_pt = (Vec2){button, button};
  step.disabled = !vkr_editor_command_enabled(CMD_SIM_STEP, editor, frame);
  if (vkr_ui_button(ui, string8_lit("step"), (String8){0}, &step))
    vkr_editor_command_execute(CMD_SIM_STEP, editor, frame);
  VkrUiWidgetConfig stop = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_STOP, string8_lit("Stop and reset simulation"));
  stop.style.min_size_pt = stop.style.max_size_pt = (Vec2){button, button};
  stop.icon_color = theme->error;
  stop.disabled = !vkr_editor_command_enabled(CMD_SIM_RESET, editor, frame);
  if (vkr_ui_button(ui, string8_lit("stop"), (String8){0}, &stop))
    vkr_editor_command_execute(CMD_SIM_RESET, editor, frame);
  VkrUiPanelConfig divider = vkr_ui_panel_config_default();
  divider.placement.column = 3u;
  divider.placement.row = 0u;
  divider.placement.justify = VKR_UI_ALIGN_CENTER;
  divider.placement.margin_pt = (VkrUiEdges){6, 0, 6, 0};
  divider.style.min_size_pt = divider.style.max_size_pt = (Vec2){1.0f, 18.0f};
  divider.style.background_color = theme->border;
  if (vkr_ui_panel_begin(ui, string8_lit("divider"), &divider))
    (void)vkr_ui_panel_end(ui);
  const bool8_t live = !frame->scene_rendering_stopped;
  VkrUiWidgetConfig render = vkr_editor_icon_button_config(
      4u, 0u, live ? VKR_UI_ICON_MONITOR_PLAY : VKR_UI_ICON_MONITOR_STOP,
      live ? string8_lit("Live rendering (click to freeze the Scene)")
           : string8_lit("Scene frozen (click to resume rendering)"));
  render.style.min_size_pt = render.style.max_size_pt = (Vec2){button, button};
  vkr_editor_toggle_style(&render, live);
  if (vkr_ui_button(ui, string8_lit("render"), (String8){0}, &render))
    vkr_editor_command_execute(CMD_RENDER_TOGGLE, editor, frame);
  VkrUiWidgetConfig camera = vkr_editor_icon_button_config(
      5u, 0u, VKR_UI_ICON_CAMERA,
      frame->mouse_captured
          ? string8_lit("Free camera active; Escape releases")
          : string8_lit("Free camera (F3 or Tab; hold RMB in the Scene)"));
  camera.style.min_size_pt = camera.style.max_size_pt = (Vec2){button, button};
  camera.disabled = !vkr_editor_command_enabled(CMD_CAMERA, editor, frame);
  vkr_editor_toggle_style(&camera, frame->mouse_captured);
  if (vkr_ui_button(ui, string8_lit("camera"), (String8){0}, &camera))
    vkr_editor_command_execute(CMD_CAMERA, editor, frame);
  (void)vkr_ui_panel_end(ui);
}

void vkr_editor_windows_build_navigation(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  const bool8_t projects = editor->projects != NULL;
  const VkrUiTrack nav_columns[] = {
      {.value = editor->title_inset_pt, .unit = VKR_UI_TRACK_PX},
      {.unit = VKR_UI_TRACK_AUTO}, /* brand */
      {.unit = VKR_UI_TRACK_AUTO}, /* File */
      {.unit = VKR_UI_TRACK_AUTO}, /* Edit */
      {.unit = VKR_UI_TRACK_AUTO}, /* View */
      {.unit = VKR_UI_TRACK_AUTO}, /* Scene */
      {.unit = VKR_UI_TRACK_AUTO}, /* Help */
      {.value = 12.0f, .unit = VKR_UI_TRACK_PX},
      {.unit = VKR_UI_TRACK_AUTO}, /* save */
      {.unit = VKR_UI_TRACK_AUTO}, /* undo */
      {.unit = VKR_UI_TRACK_AUTO}, /* redo */
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.unit = VKR_UI_TRACK_AUTO}, /* Projects */
      {.unit = VKR_UI_TRACK_AUTO}, /* Scenes */
      {.unit = VKR_UI_TRACK_AUTO}, /* status */
      {.unit = VKR_UI_TRACK_AUTO}, /* commands */
      {.unit = VKR_UI_TRACK_AUTO}, /* minimize */
      {.unit = VKR_UI_TRACK_AUTO}, /* maximize */
      {.unit = VKR_UI_TRACK_AUTO}, /* close */
  };
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_START,
  };
  bar.columns = nav_columns;
  bar.column_count = ArrayCount(nav_columns);
  bar.rows = &one_track;
  bar.row_count = 1u;
  bar.style.padding_pt = (VkrUiEdges){4.0f, 8.0f, 4.0f, 4.0f};
  bar.style.gap_pt = 2.0f;
  bar.style.min_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.max_size_pt.y = VKR_EDITOR_NAVIGATION_HEIGHT_PT;
  bar.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  bar.style.background_color = theme->header;
  bar.style.border_color = theme->separator;
  (void)vkr_ui_input_layer_set(ui, 0u);
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.navigation"), &bar))
    return;

  VkrUiWidgetConfig brand =
      vkr_editor_text_config(theme->font_emphasis, theme->text);
  brand.text.font = editor->heading_font;
  brand.icon = VKR_UI_ICON_BRAND;
  brand.icon_size_pt = 17.0f;
  brand.icon_color = theme->accent_hover;
  brand.placement = (VkrUiPlacement){
      .column = 1u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 10.0f, 0.0f, 6.0f},
  };
  vkr_ui_label(ui, string8_lit("brand"), string8_lit("VKR"), &brand);

  for (uint32_t menu = VKR_EDITOR_MENU_FILE; menu < VKR_EDITOR_MENU_COUNT;
       ++menu) {
    const bool8_t open = editor->menu == (VkrEditorMenu)menu;
    VkrUiWidgetConfig button = vkr_editor_menu_button_config(
        2u + (menu - VKR_EDITOR_MENU_FILE), open, VKR_FONT_HANDLE_INVALID);
    button.style.text_color = open ? theme->text : theme->text_secondary;
    const String8 title = string8_create((uint8_t *)s_menus[menu].title,
                                         strlen(s_menus[menu].title));
    (void)vkr_ui_push_id_u64(ui, menu);
    const VkrUiId id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("menu"));
    const bool8_t clicked =
        vkr_ui_button(ui, string8_lit("menu"), title, &button);
    (void)vkr_ui_pop_id(ui);
    /* With a menu open, hovering another title switches to it. */
    const bool8_t switch_to =
        editor->menu != VKR_EDITOR_MENU_NONE && !open && ui->hot_id == id;
    if (clicked || switch_to) {
      if (open && clicked) {
        editor_menu_close(editor, ui);
      } else {
        editor->menu = (VkrEditorMenu)menu;
        VkrUiRect anchor = {0};
        if (vkr_ui_widget_rect(ui, id, &anchor))
          editor->menu_anchor_px = anchor;
      }
    } else if (open) {
      VkrUiRect anchor = {0};
      if (vkr_ui_widget_rect(ui, id, &anchor))
        editor->menu_anchor_px = anchor;
    }
  }

  editor_top_icon_button(editor, ui, frame, 8u, CMD_SAVE);
  editor_top_icon_button(editor, ui, frame, 9u, CMD_UNDO);
  editor_top_icon_button(editor, ui, frame, 10u, CMD_REDO);

  if (projects)
    vkr_editor_projects_navigation(editor->projects, editor, frame, 12u);

  if ((float32_t)ui->target_width / ui->content_scale >= 980.0f) {
    const bool8_t running = frame->simulation_running;
    const bool8_t frozen = frame->scene_rendering_stopped;
    VkrUiWidgetConfig status =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    status.placement = (VkrUiPlacement){
        .column = 14u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_END,
        .align = VKR_UI_ALIGN_CENTER,
        .margin_pt = {0.0f, 6.0f, 0.0f, 6.0f},
    };
    status.style.padding_pt = (VkrUiEdges){3.0f, 10.0f, 3.0f, 8.0f};
    status.style.background_color = theme->field;
    status.style.border_pt = (VkrUiEdges){1, 1, 1, 1};
    status.style.border_color = theme->border;
    status.style.corner_radius_pt = (Vec4){11.0f, 11.0f, 11.0f, 11.0f};
    const bool8_t unsaved =
        frame->scene && frame->edits &&
        frame->edits->revision != frame->edits->saved_revision;
    status.icon = VKR_UI_ICON_DOT;
    status.icon_size_pt = 8.0f;
    status.icon_color = unsaved   ? theme->warning
                        : running ? theme->success
                        : frozen  ? theme->warning
                                  : theme->text_disabled;
    status.tooltip = unsaved
                         ? string8_lit("Unsaved scene edits; " EDITOR_SHORTCUT(
                               "\xe2\x8c\x98S", "Ctrl+S") " saves")
                         : (String8){0};
    const String8 content = string8_create_formatted(
        ui->frame_allocator, "%s%s%s  %.1fs",
        unsaved ? "Unsaved \xc2\xb7 " : "", running ? "Playing" : "Paused",
        frozen ? " \xc2\xb7 Frozen" : "", frame->simulation_time);
    vkr_ui_label(ui, string8_lit("status"), content, &status);
  }

  vkr_editor_cmd_bar_build(editor, frame, 15u);
  editor_caption_buttons_build(frame, 16u);
  (void)vkr_ui_panel_end(ui);
  editor_transport_build(editor, frame);
  (void)vkr_ui_input_layer_set(ui, 0u);
}

/* ---- Scene overlays: compact statistics and render failures ---- */

static VkrUiRect editor_scene_error_rect(const VkrSampleUiFrame *frame) {
  if (frame->scene_error == VKR_RENDERER_ERROR_NONE ||
      !frame->scene_rendering_stopped)
    return (VkrUiRect){0};
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  const float32_t top =
      viewport.y / scale +
      (frame->scene_only ? VKR_EDITOR_NAVIGATION_HEIGHT_PT : 0.0f) + 48.0f;
  const float32_t bottom = (viewport.y + viewport.w) / scale - 8.0f;
  const float32_t width = Min(480.0f, Max(0.0f, viewport.z / scale - 16.0f));
  const float32_t height = Min(76.0f, Max(0.0f, bottom - top));
  if (width <= 0 || height <= 0)
    return (VkrUiRect){0};
  return (VkrUiRect){viewport.x / scale + (viewport.z / scale - width) * 0.5f,
                     top, width, height};
}

static void editor_scene_error_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  const VkrUiRect rect = editor_scene_error_rect(frame);
  if (!vkr_ui_rect_has_area(rect))
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = VKR_UI_PLACEMENT_DEFAULT;
  panel.placement.column = panel.placement.row = 0u;
  panel.placement.justify = panel.placement.align = VKR_UI_ALIGN_START;
  panel.placement.margin_pt = (VkrUiEdges){rect.y, 0, 0, rect.x};
  panel.style = vkr_editor_glass_style();
  panel.style.background_color = (Vec4){0.20f, 0.08f, 0.08f, 0.96f};
  panel.style.border_color = vkr_ui_color_alpha(theme->error, 0.8f);
  panel.style.min_size_pt = panel.style.max_size_pt =
      (Vec2){rect.width, rect.height};
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.scene.error"), &panel))
    return;
  const bool8_t narrow = rect.width < 320.0f;
  VkrUiWidgetConfig label = vkr_editor_text_config(
      narrow ? theme->font_caption : theme->font_body, theme->text);
  label.placement.column = label.placement.row = 0u;
  label.placement.justify = VKR_UI_ALIGN_STRETCH;
  label.text.layout.word_wrap = true_v;
  label.icon = VKR_UI_ICON_WARNING_FILL;
  label.icon_size_pt = 18.0f;
  label.icon_color = theme->error;
  vkr_ui_label(
      ui, string8_lit("message"),
      frame->scene_error == VKR_RENDERER_ERROR_OUT_OF_MEMORY
          ? (narrow ? string8_lit("Scene memory limit.\nUnload or retry.")
                    : string8_lit("Scene memory limit reached.\n"
                                  "Unload or retry rendering."))
          : (narrow ? string8_lit("Scene render failed.\nUnload or retry.")
                    : string8_lit("Scene rendering failed.\n"
                                  "Unload or retry rendering.")),
      &label);
  (void)vkr_ui_panel_end(ui);
}

/* One-line frame statistics in the Scene's bottom-right corner; hovering
 * shows resolution, hardware and memory detail. */
static void editor_scene_stats_build(const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid || frame->scene_render_width == 0u)
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  if (viewport.z / scale < 260.0f || viewport.w / scale < 120.0f)
    return;
  const bool8_t fallback = frame->scene_output_scale < 1.0f;
  const bool8_t incomplete =
      frame->texture_pending_count || frame->texture_demanded_missing_count;
  const bool8_t gameplay = frame->scene && frame->scene->player_entity.u64 &&
                           frame->simulation_running;
  /* The runtime formats performance as newline-separated lines. */
  String8 summary = frame->scene_rendering_stopped ? string8_lit("Frozen")
                    : !frame->scene ? string8_lit("No scene loaded")
                                    : frame->text.performance;
  uint8_t *line =
      vkr_allocator_alloc(ui->frame_allocator, summary.length * 3u + 1u,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  uint32_t length = 0u;
  if (line) {
    for (uint64_t i = 0u; i < summary.length; ++i) {
      if (summary.str[i] == '\n') {
        MemCopy(line + length, "  \xc2\xb7  ", 6u);
        length += 6u;
        continue;
      }
      line[length++] = summary.str[i];
    }
    summary = (String8){.str = line, .length = length};
  }
  String8 detail = string8_create_formatted(
      ui->frame_allocator, "Render %u\xc3\x97%u   Output %u\xc3\x97%u",
      frame->scene_render_width, frame->scene_render_height,
      frame->scene_output_width, frame->scene_output_height);
  if (fallback)
    detail = string8_create_formatted(
        ui->frame_allocator, "%.*s\nMemory fallback: %.0f%%",
        (int32_t)detail.length, detail.str,
        (double)(frame->scene_output_scale * 100.0f));
  detail = string8_create_formatted(
      ui->frame_allocator, "%.*s\n%.*s", (int32_t)detail.length, detail.str,
      (int32_t)frame->text.system.length, frame->text.system.str);
  if (gameplay)
    detail = string8_create_formatted(
        ui->frame_allocator, "%.*s\n%.*s", (int32_t)detail.length, detail.str,
        (int32_t)frame->text.camera.length, frame->text.camera.str);
  if (incomplete)
    detail = string8_create_formatted(
        ui->frame_allocator, "%.*s\nTexture wait: %u  Missing: %u",
        (int32_t)detail.length, detail.str, frame->texture_pending_count,
        frame->texture_demanded_missing_count);
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = chip.placement.row = 0u;
  chip.placement.justify = VKR_UI_ALIGN_END;
  chip.placement.align = VKR_UI_ALIGN_END;
  chip.placement.margin_pt =
      (VkrUiEdges){0.0f,
                   Max(0.0f, (float32_t)ui->target_width / scale -
                                 (viewport.x + viewport.z) / scale + 10.0f),
                   Max(0.0f, (float32_t)ui->target_height / scale -
                                 (viewport.y + viewport.w) / scale + 10.0f),
                   0.0f};
  chip.style = vkr_editor_overlay_style();
  chip.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, 8.0f};
  chip.style.font_size_pt = theme->font_caption;
  chip.style.text_color =
      (fallback || incomplete) ? theme->warning : theme->text_secondary;
  chip.style.hover_background_color = theme->popup;
  chip.icon = (fallback || incomplete) ? VKR_UI_ICON_WARNING_FILL
                                       : VKR_UI_ICON_CHART_BAR;
  chip.icon_size_pt = 12.0f;
  chip.icon_color =
      (fallback || incomplete) ? theme->warning : theme->text_secondary;
  chip.tooltip = detail;
  (void)vkr_ui_button(ui, string8_lit("editor.scene.stats"), summary, &chip);
}

void vkr_editor_scene_overlays_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  (void)vkr_ui_input_layer_set(frame->ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER);
  editor_scene_stats_build(frame);
  editor_scene_error_build(editor, frame);
  (void)vkr_ui_input_layer_set(frame->ui, 0u);
}

static VkrUiRect editor_window_rect(const VkrUiSystem *ui,
                                    const VkrEditorWindowState *window) {
  const float32_t scale = ui->content_scale;
  return (VkrUiRect){
      .x = window->position_pt.x * scale,
      .y = window->position_pt.y * scale,
      .width = window->size_pt.x * scale,
      .height = window->size_pt.y * scale,
  };
}

static void editor_window_clamp(VkrUiSystem *ui, VkrEditorWindowState *window) {
  const float32_t width_pt = (float32_t)ui->target_width / ui->content_scale;
  const float32_t height_pt = (float32_t)ui->target_height / ui->content_scale;
  const float32_t visible_title_pt = 72.0f;
  window->position_pt.x =
      vkr_clamp_f32(window->position_pt.x, 0.0f,
                    vkr_max_f32(0.0f, width_pt - visible_title_pt));
  window->position_pt.y = vkr_clamp_f32(window->position_pt.y, 35.0f,
                                        vkr_max_f32(35.0f, height_pt - 28.0f));
}

void vkr_editor_windows_register_input_layers(VkrEditorUi *editor,
                                              VkrUiSystem *ui) {
  /* Cmd suggestions float above every other overlay. */
  if (vkr_ui_rect_has_area(editor->cmd_popup_px))
    (void)vkr_ui_input_layer_register(ui, VKR_EDITOR_CMD_LAYER,
                                      editor->cmd_popup_px);
  if (ui->keyboard_input_layer == VKR_EDITOR_CMD_LAYER)
    (void)vkr_ui_keyboard_layer_set(ui, 0);

  if (editor->menu != VKR_EDITOR_MENU_NONE)
    vkr_ui_keyboard_layer_set(ui, EDITOR_MENU_LAYER);
  else if (ui->keyboard_input_layer == EDITOR_MENU_LAYER)
    vkr_ui_keyboard_layer_set(ui, 0u);
  const bool8_t popup_contains_pointer =
      editor->menu != VKR_EDITOR_MENU_NONE &&
      editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                           editor_menu_popup_rect(editor, ui));
  if (ui->mouse_pressed && !popup_contains_pointer) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(ui->input, BUTTON_LEFT, &press_x, &press_y);
    uint32_t top_z = 0u;
    VkrEditorWindowKind top_kind = VKR_EDITOR_WINDOW_COUNT;
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      VkrEditorWindowState *window = &editor->windows[i];
      editor_window_clamp(ui, window);
      if (window->visible && window->z_order > top_z &&
          editor_point_in_rect(press_x, press_y,
                               editor_window_rect(ui, window))) {
        top_z = window->z_order;
        top_kind = (VkrEditorWindowKind)i;
      }
    }
    if (top_kind < VKR_EDITOR_WINDOW_COUNT)
      editor_window_raise(editor, top_kind);
  }

  for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
    VkrEditorWindowState *window = &editor->windows[i];
    editor_window_clamp(ui, window);
    if (window->visible)
      (void)vkr_ui_input_layer_register(ui, window->z_order + 2u,
                                        editor_window_rect(ui, window));
  }
  if (editor->menu != VKR_EDITOR_MENU_NONE)
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER,
                                      editor_menu_popup_rect(editor, ui));
  if (editor->context_open)
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER,
                                      vkr_editor_context_menu_rect(editor, ui));
}

static void editor_build_window(VkrEditorUi *editor, VkrUiSystem *ui,
                                InputState *input, VkrEditorWindowKind kind,
                                const VkrSampleUiFrame *frame) {
  VkrEditorWindowState *window = &editor->windows[kind];
  const VkrUiTheme *theme = vkr_ui_theme();
  String8 title_text = {0};
  String8 body_text = {0};
  VkrUiIcon title_icon = VKR_UI_ICON_WINDOW;
  bool8_t monospace = true_v;
  switch (kind) {
  case VKR_EDITOR_WINDOW_PHYSICS:
    title_text = string8_lit("Physics settings");
    title_icon = VKR_UI_ICON_PHYSICS;
    break;
  case VKR_EDITOR_WINDOW_ANIMATION:
    title_text = string8_lit("Animation editor");
    title_icon = VKR_UI_ICON_ANIMATION;
    break;
  case VKR_EDITOR_WINDOW_GRAPHICS:
    title_text = string8_lit("Graphics settings");
    title_icon = VKR_UI_ICON_GRAPHICS;
    break;
  case VKR_EDITOR_WINDOW_DRAWS:
    title_text = string8_lit("Draws and render graph");
    title_icon = VKR_UI_ICON_DRAWS;
    body_text = frame->text.metrics;
    break;
  case VKR_EDITOR_WINDOW_MEMORY:
    title_text = string8_lit("Memory");
    title_icon = VKR_UI_ICON_MEMORY;
    body_text = frame->text.memory;
    break;
  case VKR_EDITOR_WINDOW_HELP:
    title_text = string8_lit("Keyboard and mouse controls");
    title_icon = VKR_UI_ICON_KEYBOARD;
    body_text = string8_lit(s_help_text);
    monospace = false_v;
    break;
  default:
    return;
  }

  if (ui->mouse_pressed && !frame->mouse_captured &&
      editor->menu == VKR_EDITOR_MENU_NONE) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(input, BUTTON_LEFT, &press_x, &press_y);
    const Vec2 press = {press_x / ui->content_scale,
                        press_y / ui->content_scale};
    bool8_t occluded = false_v;
    for (uint32_t i = 0; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      const VkrEditorWindowState *other = &editor->windows[i];
      if (other->visible && other->z_order > window->z_order &&
          editor_point_in_rect(press_x, press_y,
                               editor_window_rect(ui, other))) {
        occluded = true_v;
      }
    }
    if (!occluded && press.x >= window->position_pt.x &&
        press.x < window->position_pt.x + window->size_pt.x - 30 &&
        press.y >= window->position_pt.y &&
        press.y < window->position_pt.y + 28) {
      window->dragging = true_v;
      window->drag_grab_pt = (Vec2){press.x - window->position_pt.x,
                                    press.y - window->position_pt.y};
    }
  }
  const bool8_t down = input_is_button_down(input, BUTTON_LEFT);
  if (window->dragging && !frame->mouse_captured &&
      (down || ui->mouse_released)) {
    /* Include the final endpoint when press and release share one UI frame. */
    window->position_pt =
        (Vec2){ui->mouse_x / ui->content_scale - window->drag_grab_pt.x,
               ui->mouse_y / ui->content_scale - window->drag_grab_pt.y};
    ui->capture.mouse = true_v;
  }
  if (!down || frame->mouse_captured) {
    window->dragging = false_v;
  }
  editor_window_clamp(ui, window);
  (void)vkr_ui_input_layer_set(ui, window->z_order + 2u);
  (void)vkr_ui_push_id_u64(ui, kind);
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  const VkrUiTrack rows[] = {
      {.value = 28.0f, .unit = VKR_UI_TRACK_PX},
      one_track,
  };
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt =
          {
              .top = window->position_pt.y,
              .left = window->position_pt.x,
          },
  };
  panel.columns = &one_track;
  panel.column_count = 1u;
  panel.rows = rows;
  panel.row_count = ArrayCount(rows);
  panel.style = vkr_editor_glass_style();
  panel.style.background_color = theme->panel;
  panel.style.padding_pt = (VkrUiEdges){0};
  panel.style.min_size_pt = window->size_pt;
  panel.style.max_size_pt = window->size_pt;
  panel.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("window"), &panel)) {
    (void)vkr_ui_pop_id(ui);
    return;
  }

  const VkrUiTrack header_columns[] = {
      one_track,
      {.value = 30.0f, .unit = VKR_UI_TRACK_PX},
  };
  VkrUiPanelConfig header = vkr_ui_panel_config_default();
  header.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_STRETCH,
      .align = VKR_UI_ALIGN_STRETCH,
  };
  header.columns = header_columns;
  header.column_count = ArrayCount(header_columns);
  header.rows = &one_track;
  header.row_count = 1u;
  header.style.padding_pt = (VkrUiEdges){0};
  header.style.border_pt = (VkrUiEdges){0.0f, 0.0f, 1.0f, 0.0f};
  header.style.corner_radius_pt =
      (Vec4){theme->radius_large, theme->radius_large, 0.0f, 0.0f};
  header.style.background_color = theme->header;
  header.style.border_color = theme->separator;
  if (vkr_ui_panel_begin(ui, string8_lit("header"), &header)) {
    VkrUiWidgetConfig drag = vkr_ui_widget_config_default();
    drag.placement = (VkrUiPlacement){
        .column = 0u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    drag.fill = true_v;
    drag.style.padding_pt = (VkrUiEdges){5.0f, 10.0f, 5.0f, 12.0f};
    drag.style.corner_radius_pt = (Vec4){0};
    drag.style.background_color = (Vec4){0};
    drag.style.hover_background_color = VKR_UI_COLOR_NONE;
    drag.style.active_background_color = VKR_UI_COLOR_NONE;
    drag.style.text_color = theme->text;
    drag.style.font_size_pt = theme->font_body;
    drag.text.font = editor->heading_font;
    drag.icon = title_icon;
    drag.icon_size_pt = 15.0f;
    drag.icon_color = theme->accent_hover;
    drag.cursor =
        window->dragging ? VKR_WINDOW_CURSOR_GRABBING : VKR_WINDOW_CURSOR_GRAB;
    (void)vkr_ui_button(ui, string8_lit("drag"), title_text, &drag);
    /* Left-aligned title over the full-width drag handle. */
    VkrUiWidgetConfig title = drag;
    title.placement.justify = VKR_UI_ALIGN_START;
    title.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("title"), title_text, &title);

    VkrUiWidgetConfig close = vkr_ui_widget_config_default();
    close.placement = (VkrUiPlacement){
        .column = 1u,
        .row = 0u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_STRETCH,
        .align = VKR_UI_ALIGN_STRETCH,
    };
    vkr_editor_ghost_style(&close);
    close.placement.justify = VKR_UI_ALIGN_CENTER;
    close.placement.align = VKR_UI_ALIGN_CENTER;
    close.style.min_size_pt = close.style.max_size_pt = (Vec2){22.0f, 22.0f};
    close.style.padding_pt = (VkrUiEdges){4.0f, 4.0f, 4.0f, 4.0f};
    close.style.hover_background_color = vkr_ui_color_alpha(theme->error, 0.8f);
    close.icon = VKR_UI_ICON_CLOSE;
    close.icon_size_pt = 13.0f;
    close.tooltip = string8_lit("Close");
    if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &close))
      window->visible = false_v;
    (void)vkr_ui_panel_end(ui);
  }

  if (kind == VKR_EDITOR_WINDOW_GRAPHICS)
    vkr_editor_graphics_build(editor, frame);
  else if (kind == VKR_EDITOR_WINDOW_ANIMATION) {
    vkr_editor_animation_build(editor, frame);
  } else if (kind == VKR_EDITOR_WINDOW_PHYSICS) {
    VkrUiPanelConfig body = vkr_ui_panel_config_default();
    body.placement.column = 0;
    body.placement.row = 1;
    body.style.padding_pt = (VkrUiEdges){0};
    body.style.background_color = theme->panel;
    body.style.corner_radius_pt =
        (Vec4){0.0f, 0.0f, theme->radius_large, theme->radius_large};
    body.clip_children = true_v;
    if (vkr_ui_panel_begin(ui, string8_lit("physics.settings.body"), &body)) {
      const VkrUiRect bounds = {
          window->position_pt.x * ui->content_scale,
          (window->position_pt.y + 30.0f) * ui->content_scale,
          window->size_pt.x * ui->content_scale,
          Max(1.0f, window->size_pt.y - 30.0f) * ui->content_scale};
      vkr_editor_physics_settings_build(editor->physics_settings, frame, bounds,
                                        editor->heading_font);
      (void)vkr_ui_panel_end(ui);
    }
  } else {
    VkrUiWidgetConfig body = vkr_editor_text_config(
        monospace ? theme->font_caption : theme->font_body,
        monospace ? theme->text_secondary : theme->text);
    if (monospace)
      body.text.font = editor->mono_font;
    body.placement = (VkrUiPlacement){
        .column = 0u,
        .row = 1u,
        .column_span = 1u,
        .row_span = 1u,
        .justify = VKR_UI_ALIGN_START,
        .align = VKR_UI_ALIGN_START,
        .margin_pt = {10.0f, 12.0f, 10.0f, 12.0f},
    };
    vkr_ui_label(ui, string8_lit("body"), body_text, &body);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_windows_build_floating(VkrEditorUi *editor, VkrUiSystem *ui,
                                       InputState *input,
                                       const VkrSampleUiFrame *frame) {
  for (uint32_t z = 1u; z <= VKR_EDITOR_WINDOW_COUNT; ++z) {
    for (uint32_t i = 0u; i < VKR_EDITOR_WINDOW_COUNT; ++i) {
      if (editor->windows[i].visible && editor->windows[i].z_order == z) {
        editor_build_window(editor, ui, input, (VkrEditorWindowKind)i, frame);
        break;
      }
    }
  }
}

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  if (editor->menu != VKR_EDITOR_MENU_NONE) {
    VkrUiSystem *ui = frame->ui;
    const bool8_t outside =
        ui->mouse_pressed &&
        (float32_t)ui->mouse_y >=
            VKR_EDITOR_NAVIGATION_HEIGHT_PT * ui->content_scale &&
        !editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                              editor_menu_popup_rect(editor, ui));
    if (outside || input_key_just_pressed(frame->input, KEY_ESCAPE)) {
      editor->menu = VKR_EDITOR_MENU_NONE;
      ui->focused_id = ui->active_id = VKR_UI_ID_NONE;
      (void)vkr_ui_keyboard_layer_set(ui, 0u);
    }
  }
  const uint8_t space_modifiers =
      input_key_press_modifiers(frame->input, KEY_SPACE);
  if (!frame->mouse_captured &&
      input_key_just_pressed(frame->input, KEY_SPACE) &&
      (space_modifiers & (VKR_INPUT_MOD_CONTROL | VKR_INPUT_MOD_ALT)) ==
          VKR_INPUT_MOD_CONTROL) {
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_CONTENT);
    frame->ui->capture.keyboard = true_v;
  }
  /* Interface zoom: Cmd/Ctrl with =, - or 0. */
  if (!frame->mouse_captured) {
    static const struct {
      Keys key;
      EditorCommand command;
    } zoom_keys[] = {{KEY_PLUS, CMD_ZOOM_IN},
                     {KEY_MINUS, CMD_ZOOM_OUT},
                     {KEY_0, CMD_ZOOM_RESET}};
    for (uint32_t i = 0; i < ArrayCount(zoom_keys); ++i) {
      if (input_key_just_pressed(frame->input, zoom_keys[i].key) &&
          input_key_shortcut_modifier(frame->input, zoom_keys[i].key) &&
          vkr_editor_command_enabled(zoom_keys[i].command, editor, frame)) {
        vkr_editor_command_execute(zoom_keys[i].command, editor, frame);
        frame->ui->capture.keyboard = true_v;
      }
    }
  }
  const bool8_t modifier = input_key_shortcut_modifier(frame->input, KEY_P);
  if (!frame->mouse_captured && modifier &&
      input_key_just_pressed(frame->input, KEY_P)) {
    editor->cmd_focus_request = true_v;
    editor->menu = VKR_EDITOR_MENU_NONE;
  }
}

/* ---- Context menus: entity rows, dock tabs and Console records ---- */

typedef enum EditorContextAction {
  CONTEXT_FRAME,
  CONTEXT_VISIBILITY,
  CONTEXT_COPY_NAME,
  CONTEXT_TAB_CLOSE,
  CONTEXT_TAB_LAYOUT,
  CONTEXT_CONSOLE_COPY,
  CONTEXT_CONSOLE_CLEAR,
} EditorContextAction;

typedef struct EditorContextItem {
  const char *label;
  VkrUiIcon icon;
  const char *shortcut;
  bool8_t disabled;
  EditorContextAction action;
} EditorContextItem;

#define EDITOR_CONTEXT_ITEM_CAPACITY 3u
#define EDITOR_CONTEXT_WIDTH_PT 210.0f

/* Items for the open menu's kind; zero when its target no longer exists. */
static uint32_t editor_context_items(const VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     EditorContextItem *items, char *label,
                                     uint32_t label_capacity) {
  switch (editor->context_kind) {
  case VKR_EDITOR_CONTEXT_ENTITY: {
    if (!frame || !frame->scene ||
        !vkr_scene_entity_alive(frame->scene, editor->context_entity))
      return 0u;
    const SceneVisibility *visibility =
        vkr_entity_get_component(frame->scene->world, editor->context_entity,
                                 frame->scene->comp_visibility);
    const bool8_t hidden = visibility && !visibility->visible;
    items[0] = (EditorContextItem){"Frame", VKR_UI_ICON_FRAME, "F", false_v,
                                   CONTEXT_FRAME};
    items[1] =
        (EditorContextItem){hidden ? "Show" : "Hide",
                            hidden ? VKR_UI_ICON_EYE : VKR_UI_ICON_EYE_SLASH,
                            NULL, !visibility, CONTEXT_VISIBILITY};
    items[2] = (EditorContextItem){"Copy name", VKR_UI_ICON_COPY, NULL, false_v,
                                   CONTEXT_COPY_NAME};
    return 3u;
  }
  case VKR_EDITOR_CONTEXT_DOCK_TAB: {
    const VkrUiDockPanelKind kind = (VkrUiDockPanelKind)editor->context_panel;
    if (frame && !vkr_ui_dock_find_panel(frame->dock, kind, NULL, NULL))
      return 0u;
    const String8 name = vkr_ui_dock_panel_label(kind);
    snprintf(label, label_capacity, "Close %.*s", (int)name.length, name.str);
    items[0] = (EditorContextItem){label, VKR_UI_ICON_CLOSE, NULL,
                                   kind == VKR_UI_DOCK_PANEL_SCENE_VIEWPORT,
                                   CONTEXT_TAB_CLOSE};
    items[1] = (EditorContextItem){"Reset panel layout", VKR_UI_ICON_LAYOUT,
                                   NULL, false_v, CONTEXT_TAB_LAYOUT};
    return 2u;
  }
  case VKR_EDITOR_CONTEXT_CONSOLE:
    items[0] = (EditorContextItem){"Copy selected", VKR_UI_ICON_COPY, NULL,
                                   false_v, CONTEXT_CONSOLE_COPY};
    items[1] = (EditorContextItem){"Clear Console", VKR_UI_ICON_TRASH, NULL,
                                   false_v, CONTEXT_CONSOLE_CLEAR};
    return 2u;
  }
  return 0u;
}

VkrUiRect vkr_editor_context_menu_rect(const VkrEditorUi *editor,
                                       const VkrUiSystem *ui) {
  if (!editor->context_open)
    return (VkrUiRect){0};
  EditorContextItem items[EDITOR_CONTEXT_ITEM_CAPACITY];
  char label[64];
  const uint32_t count =
      editor_context_items(editor, NULL, items, label, sizeof(label));
  const float32_t scale = ui->content_scale;
  const float32_t width = EDITOR_CONTEXT_WIDTH_PT;
  const float32_t height =
      EDITOR_MENU_PADDING_PT * 2.0f + EDITOR_MENU_ROW_PT * (float32_t)count;
  const float32_t screen_w = (float32_t)ui->target_width / scale;
  const float32_t screen_h = (float32_t)ui->target_height / scale;
  const float32_t x =
      Min(editor->context_position_pt.x, Max(0.0f, screen_w - width));
  const float32_t y =
      Min(editor->context_position_pt.y, Max(0.0f, screen_h - height));
  return (VkrUiRect){x * scale, y * scale, width * scale, height * scale};
}

static void editor_context_run(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               EditorContextAction action) {
  switch (action) {
  case CONTEXT_FRAME:
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_FRAME, .entity = editor->context_entity};
    break;
  case CONTEXT_VISIBILITY:
    vkr_editor_toggle_visibility(frame, editor->context_entity);
    break;
  case CONTEXT_COPY_NAME: {
    const String8 name =
        vkr_scene_get_name(frame->scene, editor->context_entity);
    if (name.length)
      (void)vkr_platform_clipboard_write_text(name.str, name.length);
    break;
  }
  case CONTEXT_TAB_CLOSE:
    vkr_editor_dock_toggle(frame->dock,
                           (VkrUiDockPanelKind)editor->context_panel);
    break;
  case CONTEXT_TAB_LAYOUT:
    vkr_ui_dock_default_editor_layout(frame->dock);
    break;
  case CONTEXT_CONSOLE_COPY:
    vkr_editor_console_copy_selection(&editor->console, frame->ui);
    break;
  case CONTEXT_CONSOLE_CLEAR:
    vkr_editor_console_clear(&editor->console);
    break;
  }
}

void vkr_editor_context_menu_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (!editor->context_open)
    return;
  EditorContextItem items[EDITOR_CONTEXT_ITEM_CAPACITY];
  char label[64];
  const uint32_t count =
      editor_context_items(editor, frame, items, label, sizeof(label));
  const VkrUiRect rect = vkr_editor_context_menu_rect(editor, ui);
  /* The press that opened a menu is a right click; any later press outside,
   * Escape, the Cmd bar or a menu-bar menu closes it. */
  const bool8_t pressed_outside =
      (ui->mouse_pressed ||
       input_button_just_pressed(frame->input, BUTTON_RIGHT)) &&
      !editor_point_in_rect(ui->mouse_x, ui->mouse_y, rect) &&
      !input_button_just_pressed(frame->input, BUTTON_RIGHT);
  if (!count || pressed_outside ||
      input_key_just_pressed(frame->input, KEY_ESCAPE) || editor->cmd_active ||
      editor->menu != VKR_EDITOR_MENU_NONE) {
    editor->context_open = false_v;
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER, rect);
  (void)vkr_ui_input_layer_set(ui, EDITOR_MENU_LAYER);
  VkrUiTrack rows[EDITOR_CONTEXT_ITEM_CAPACITY];
  for (uint32_t i = 0; i < count; ++i)
    rows[i] =
        (VkrUiTrack){.value = EDITOR_MENU_ROW_PT, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack one_track = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement.column = popup.placement.row = 0u;
  popup.placement.justify = popup.placement.align = VKR_UI_ALIGN_START;
  popup.placement.margin_pt = (VkrUiEdges){rect.y / ui->content_scale, 0, 0,
                                           rect.x / ui->content_scale};
  popup.columns = &one_track;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = count;
  popup.style = vkr_editor_glass_style();
  popup.style.padding_pt =
      (VkrUiEdges){EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT,
                   EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT};
  popup.style.gap_pt = 0.0f;
  popup.style.min_size_pt = popup.style.max_size_pt =
      (Vec2){rect.width / ui->content_scale, rect.height / ui->content_scale};
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.context.menu"), &popup)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }
  int32_t chosen = -1;
  for (uint32_t i = 0; i < count; ++i) {
    VkrUiWidgetConfig item = vkr_ui_widget_config_default();
    item.placement.column = 0u;
    item.placement.row = i;
    item.placement.justify = VKR_UI_ALIGN_STRETCH;
    item.placement.align = VKR_UI_ALIGN_STRETCH;
    /* The whole row hovers and clicks, not just the button's content. */
    item.fill = true_v;
    vkr_editor_ghost_style(&item);
    item.style.hover_background_color = theme->accent;
    item.style.text_color = theme->text;
    item.style.padding_pt = (VkrUiEdges){3.0f, 8.0f, 3.0f, 8.0f};
    item.icon = items[i].icon;
    item.icon_size_pt = 14.0f;
    item.icon_color = theme->text_secondary;
    item.disabled = items[i].disabled;
    (void)vkr_ui_push_id_u64(ui, i);
    const VkrUiId item_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("item"));
    VkrUiWidgetConfig hit = item;
    hit.icon = VKR_UI_ICON_NONE;
    if (vkr_ui_button(ui, string8_lit("item"), (String8){0}, &hit))
      chosen = (int32_t)i;
    const bool8_t hot = ui->hot_id == item_id && !item.disabled;
    VkrUiWidgetConfig text = item;
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_CENTER;
    text.style.background_color = (Vec4){0};
    text.style.text_color = hot ? theme->text_on_accent : theme->text;
    if (hot)
      text.icon_color = theme->text_on_accent;
    vkr_ui_label(
        ui, string8_lit("label"),
        string8_create((uint8_t *)items[i].label, strlen(items[i].label)),
        &text);
    if (items[i].shortcut) {
      VkrUiWidgetConfig shortcut = vkr_editor_text_config(
          theme->font_caption,
          hot ? theme->text_on_accent : theme->text_secondary);
      shortcut.placement.column = 0u;
      shortcut.placement.row = i;
      shortcut.placement.justify = VKR_UI_ALIGN_END;
      shortcut.placement.align = VKR_UI_ALIGN_CENTER;
      shortcut.placement.margin_pt.right = 10.0f;
      vkr_ui_label(ui, string8_lit("shortcut"),
                   string8_create((uint8_t *)items[i].shortcut,
                                  strlen(items[i].shortcut)),
                   &shortcut);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
  if (chosen < 0)
    return;
  editor->context_open = false_v;
  editor_context_run(editor, frame, items[chosen].action);
}

/* ---- Toast notifications ---- */

#define EDITOR_TOAST_SECONDS 3.0

void vkr_editor_toast(VkrEditorUi *editor, VkrUiIcon icon, Vec4 color,
                      const char *text) {
  snprintf(editor->toast_text, sizeof(editor->toast_text), "%s", text);
  editor->toast_icon = icon;
  editor->toast_color = color;
  editor->toast_seconds = EDITOR_TOAST_SECONDS;
}

void vkr_editor_toasts_build(VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  /* Announce completed saves and finished Bakery work. A reload resets the
   * journal, so a new scene generation restarts save tracking silently. */
  if (frame->scene_generation != editor->toast_scene_generation) {
    editor->toast_scene_generation = frame->scene_generation;
    editor->toast_saved_known = false_v;
  }
  if (frame->edits && frame->scene &&
      (!editor->toast_saved_known ||
       frame->edits->saved_revision != editor->toast_saved_revision)) {
    if (editor->toast_saved_known &&
        frame->edits->saved_revision == frame->edits->revision)
      vkr_editor_toast(editor, VKR_UI_ICON_CHECK_CIRCLE, theme->success,
                       "Scene edits saved");
    editor->toast_saved_revision = frame->edits->saved_revision;
    editor->toast_saved_known = true_v;
  }
  const bool8_t bakery_busy = vkr_editor_bakery_busy(editor->bakery);
  if (editor->toast_bakery_busy && !bakery_busy)
    vkr_editor_toast(editor, VKR_UI_ICON_BAKERY, theme->accent_hover,
                     "Bakery finished; see Jobs for results");
  editor->toast_bakery_busy = bakery_busy;

  if (editor->toast_seconds <= 0.0 || !editor->toast_text[0])
    return;
  editor->toast_seconds -= ui->delta_time;
  ui->animating = true_v;
  const float32_t opacity =
      (float32_t)vkr_clamp_f64(editor->toast_seconds / 0.35, 0.0, 1.0) *
      (float32_t)vkr_clamp_f64(
          (EDITOR_TOAST_SECONDS - editor->toast_seconds) / 0.15, 0.0, 1.0);
  const float32_t screen_w = (float32_t)ui->target_width / ui->content_scale;
  const float32_t screen_h = (float32_t)ui->target_height / ui->content_scale;
  const float32_t width = Min(380.0f, screen_w - 24.0f);
  VkrUiWidgetConfig toast = vkr_ui_widget_config_default();
  toast.placement = VKR_UI_PLACEMENT_DEFAULT;
  toast.placement.column = toast.placement.row = 0u;
  toast.placement.justify = toast.placement.align = VKR_UI_ALIGN_START;
  /* Rise 8pt while fading in. */
  toast.placement.margin_pt =
      (VkrUiEdges){screen_h - 64.0f + 8.0f * (1.0f - opacity), 0, 0,
                   (screen_w - width) * 0.5f};
  toast.style = vkr_editor_glass_style();
  toast.style.min_size_pt = (Vec2){width, 36.0f};
  toast.style.max_size_pt = toast.style.min_size_pt;
  toast.style.padding_pt = (VkrUiEdges){8, 14, 8, 12};
  toast.style.background_color =
      vkr_ui_color_alpha(theme->popup, theme->popup.w * opacity);
  toast.style.border_color = vkr_ui_color_alpha(theme->border_strong, opacity);
  toast.style.shadow_color =
      vkr_ui_color_alpha(theme->shadow, theme->shadow.w * opacity);
  toast.style.text_color = vkr_ui_color_alpha(theme->text, opacity);
  toast.icon = editor->toast_icon;
  toast.icon_size_pt = 16.0f;
  toast.icon_color = vkr_ui_color_alpha(editor->toast_color, opacity);
  vkr_ui_label(
      ui, string8_lit("editor.toast"),
      string8_create((uint8_t *)editor->toast_text, strlen(editor->toast_text)),
      &toast);
}
