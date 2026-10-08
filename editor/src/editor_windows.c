#include "editor_brush_grid.h"
#include "editor_internal.h"
#include "editor_level.h"
#include "editor_ops.h"
#include "editor_partition.h"
#include "editor_projects.h"
#include "editor_terrain.h"

#include "editor_graphics.h"
#include "renderer/systems/vkr_gizmo_system.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define EDITOR_MENU_LAYER VKR_EDITOR_POPUP_LAYER
#define EDITOR_MENU_WIDTH_PT 272.0f
#define EDITOR_MENU_ROW_PT 26.0f
#define EDITOR_MENU_SEPARATOR_PT 9.0f
#define EDITOR_MENU_PADDING_PT 5.0f
#define EDITOR_TRANSPORT_BUTTON_PT 27.0f
/* Floating windows resize from borders just past their right and bottom
   edges, both ways this near the corner, down to a minimum size. */
#define EDITOR_WINDOW_EDGE_PT 6.0f
#define EDITOR_WINDOW_CORNER_PT 14.0f
#define EDITOR_WINDOW_MIN_WIDTH_PT 280.0f
#define EDITOR_WINDOW_MIN_HEIGHT_PT 140.0f

#if defined(PLATFORM_APPLE)
#define EDITOR_SHORTCUT(apple, other) apple
#else
#define EDITOR_SHORTCUT(apple, other) other
#endif

/* Controls the keymap does not list: one per line, tabs split the key column
   from its action. The keymap's own lines come first in the Help window. */
static const char s_help_mouse_text[] =
    "Hold RMB\tFly the Scene camera (WASD, Q/E down and up)\n"
    "Drag X / Y / Z\tScrub a value (Shift fast, Alt fine)\n"
    "Tab\tToggle free camera; Esc releases\n"
    "F6\tCycle shadow diagnostics\n"
    "F8 / F9 / F10\tIBL mode and intensity\n"
    "G\tCamera snapshot";

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

/* The View menu's toggle of a window whose body a dock panel can host: the
   tab while the live tree holds the panel, else the window. */
static void editor_window_command(VkrEditorUi *editor,
                                  const VkrSampleUiFrame *frame,
                                  VkrEditorWindowKind kind) {
  const VkrUiDockPanelKind panel = vkr_editor_window_dock_panel(kind);
  if (panel != VKR_UI_DOCK_PANEL_COUNT &&
      vkr_editor_dock_has(frame->dock, panel))
    vkr_editor_dock_toggle(frame->dock, panel);
  else
    editor_window_toggle(editor, kind);
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
  /* Toggles that duplicate a Start/Stop pair; the Cmd bar uses the pair. */
  bool8_t menu_only;
} EditorCommandInfo;

static const EditorCommandInfo s_commands[CMD_COUNT] = {
    [CMD_LOAD] = {"Load scene", VKR_UI_ICON_SCENE_LOAD, false_v},
    [CMD_RELOAD] = {"Reload scene", VKR_UI_ICON_REFRESH, false_v},
    [CMD_UNLOAD] = {"Unload scene", VKR_UI_ICON_SCENE_UNLOAD, false_v},
    [CMD_SAVE] = {"Save scene edits", VKR_UI_ICON_SAVE, false_v},
    [CMD_UNDO] = {"Undo", VKR_UI_ICON_UNDO, false_v},
    [CMD_REDO] = {"Redo", VKR_UI_ICON_REDO, false_v},
    [CMD_FRAME] = {"Frame selected", VKR_UI_ICON_FRAME, false_v},
    [CMD_HIERARCHY] = {"Outliner", VKR_UI_ICON_HIERARCHY, false_v},
    [CMD_INSPECTOR] = {"Details", VKR_UI_ICON_INSPECTOR, false_v},
    [CMD_CONSOLE] = {"Console", VKR_UI_ICON_CONSOLE, false_v},
    [CMD_BAKERY] = {"Bakery", VKR_UI_ICON_BAKERY, false_v},
    [CMD_CONTENT] = {"Content browser", VKR_UI_ICON_CONTENT, false_v},
    [CMD_ANIMATION] = {"Animation editor", VKR_UI_ICON_ANIMATION, false_v},
    [CMD_PHYSICS] = {"Physics settings", VKR_UI_ICON_PHYSICS, false_v},
    [CMD_SCRIPT_EDITOR] = {"Script editor", VKR_UI_ICON_CODE, false_v},
    [CMD_CHANGES] = {"Agent changes", VKR_UI_ICON_TERMINAL, false_v},
    [CMD_BRUSH_DRAW] = {"Draw brushes", VKR_UI_ICON_SHAPES, false_v},
    [CMD_HIDE] = {"Hide selected", VKR_UI_ICON_EYE_SLASH, false_v},
    [CMD_ISOLATE] = {"Hide unselected", VKR_UI_ICON_EYE, false_v},
    [CMD_REVEAL] = {"Show all hidden", VKR_UI_ICON_EYE, false_v},
    [CMD_BRUSH_CLIP] = {"Clip brushes", VKR_UI_ICON_SHAPES, false_v},
    [CMD_LEVEL_CHECKS] = {"Level checks", VKR_UI_ICON_PERSON_WALK, false_v},
    [CMD_TERRAIN] = {"Terrain", VKR_UI_ICON_WAVES, false_v},
    [CMD_PARTITION] = {"World partition", VKR_UI_ICON_GRID, false_v},
    [CMD_RESET_LAYOUT] = {"Reset panel layout", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_SIM_START] = {"Start simulation", VKR_UI_ICON_PLAY, false_v},
    [CMD_SIM_PAUSE] = {"Pause simulation", VKR_UI_ICON_PAUSE, false_v},
    [CMD_RENDER_START] = {"Start scene rendering", VKR_UI_ICON_MONITOR_PLAY,
                          false_v},
    [CMD_RENDER_STOP] = {"Stop scene rendering", VKR_UI_ICON_MONITOR_STOP,
                         false_v},
    [CMD_SIM_TOGGLE] = {"Play / Pause", VKR_UI_ICON_PLAY, true_v},
    [CMD_SIM_STEP] = {"Step one frame", VKR_UI_ICON_STEP, false_v},
    [CMD_SIM_RESET] = {"Stop and reset simulation", VKR_UI_ICON_STOP, false_v},
    [CMD_RENDER_TOGGLE] = {"Live scene rendering", VKR_UI_ICON_MONITOR_PLAY,
                           true_v},
    [CMD_CAMERA] = {"Free camera", VKR_UI_ICON_CAMERA, false_v},
    [CMD_GRAPHICS] = {"Preferences", VKR_UI_ICON_SETTINGS, false_v},
    [CMD_DRAWS] = {"Draws and render graph", VKR_UI_ICON_DRAWS, false_v},
    [CMD_MEMORY] = {"Memory", VKR_UI_ICON_MEMORY, false_v},
    [CMD_LABELS] = {"Light icons", VKR_UI_ICON_LIGHT, false_v},
    [CMD_LABELS_DIRECTIONAL] = {"Directional light icons",
                                VKR_UI_ICON_DIRECTIONAL_LIGHT, false_v},
    [CMD_LABELS_SPOT] = {"Spot light icons", VKR_UI_ICON_SPOT_LIGHT, false_v},
    [CMD_LABELS_POINT] = {"Point light icons", VKR_UI_ICON_POINT_LIGHT,
                          false_v},
    [CMD_HELP] = {"Keyboard and mouse controls", VKR_UI_ICON_KEYBOARD, false_v},
    [CMD_COMMANDS] = {"Cmd bar", VKR_UI_ICON_COMMAND, true_v},
    [CMD_ZOOM_IN] = {"Zoom interface in", VKR_UI_ICON_ZOOM_IN, false_v},
    [CMD_ZOOM_OUT] = {"Zoom interface out", VKR_UI_ICON_ZOOM_OUT, false_v},
    [CMD_ZOOM_RESET] = {"Actual interface size", VKR_UI_ICON_MAXIMIZE, false_v},
    [CMD_REDUCE_MOTION] = {"Reduce motion", VKR_UI_ICON_SPARKLE, false_v},
    [CMD_BUILD] = {"Build", VKR_UI_ICON_EXPORT, false_v},
    [CMD_BUILD_RUN] = {"Build and Run", VKR_UI_ICON_PLAY, false_v},
    [CMD_BUILD_SETTINGS] = {"Build Settings\xe2\x80\xa6", VKR_UI_ICON_SETTINGS,
                            false_v},
    [CMD_BUILD_OPEN] = {"Open Last Build", VKR_UI_ICON_FOLDER, false_v},
    [CMD_BUILD_LOG] = {"Build log", VKR_UI_ICON_LIST, false_v},
    [CMD_SCENE_BAKE] = {"Bake lighting", VKR_UI_ICON_PROBE, false_v},
    [CMD_TOOL_SELECT] = {"Select tool", VKR_UI_ICON_SELECT, false_v},
    [CMD_TOOL_MOVE] = {"Move tool", VKR_UI_ICON_MOVE, false_v},
    [CMD_TOOL_ROTATE] = {"Rotate tool", VKR_UI_ICON_ROTATE, false_v},
    [CMD_TOOL_SCALE] = {"Scale tool", VKR_UI_ICON_SCALE, false_v},
    [CMD_DUPLICATE] = {"Duplicate", VKR_UI_ICON_DUPLICATE, false_v},
    /* Tabs move, so the commands name positions. */
    [CMD_WORKBENCH_1] = {"Workbench 1", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_2] = {"Workbench 2", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_3] = {"Workbench 3", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_4] = {"Workbench 4", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_5] = {"Workbench 5", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_6] = {"Workbench 6", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_7] = {"Workbench 7", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_8] = {"Workbench 8", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_9] = {"Workbench 9", VKR_UI_ICON_LAYOUT, false_v},
    [CMD_WORKBENCH_PREV] = {"Previous workbench", VKR_UI_ICON_CHEVRON_LEFT,
                            false_v},
    [CMD_WORKBENCH_NEXT] = {"Next workbench", VKR_UI_ICON_CHEVRON_RIGHT,
                            false_v},
    [CMD_DELETE] = {"Delete", VKR_UI_ICON_TRASH, false_v},
    [CMD_RENAME] = {"Rename", VKR_UI_ICON_PENCIL_LINE, false_v},
    [CMD_SNAP] = {"Snap to surface or grid", VKR_UI_ICON_SNAP, false_v},
    [CMD_PLAY] = {"Play", VKR_UI_ICON_PLAY, false_v},
};

/* The keymap, after Unity's: a key and the exact modifiers held with it.
   The primary modifier is Cmd on macOS and Ctrl elsewhere. A command may
   have more than one row; the first labels it in menus and tooltips. */
#if defined(PLATFORM_APPLE)
#define EDITOR_MOD_PRIMARY VKR_INPUT_MOD_SUPER
#else
#define EDITOR_MOD_PRIMARY VKR_INPUT_MOD_CONTROL
#endif
#define EDITOR_MODS                                                            \
  (VKR_INPUT_MOD_SHIFT | VKR_INPUT_MOD_CONTROL | VKR_INPUT_MOD_ALT |           \
   VKR_INPUT_MOD_SUPER)

typedef struct EditorKeyBinding {
  EditorCommand command;
  Keys key;
  uint8_t modifiers;
  /* The runtime reads this key itself; the row only labels the command. */
  bool8_t runtime;
} EditorKeyBinding;

static const EditorKeyBinding s_keymap[] = {
    /* File and edit. */
    {CMD_SAVE, KEY_S, EDITOR_MOD_PRIMARY},
    {CMD_UNDO, KEY_Z, EDITOR_MOD_PRIMARY},
    {CMD_REDO, KEY_Y, EDITOR_MOD_PRIMARY},
    {CMD_REDO, KEY_Z, EDITOR_MOD_PRIMARY | VKR_INPUT_MOD_SHIFT},
    {CMD_COMMANDS, KEY_K, EDITOR_MOD_PRIMARY},
    /* Cmd+Space is Spotlight on macOS, so Control everywhere. */
    {CMD_CONTENT, KEY_SPACE, VKR_INPUT_MOD_CONTROL},
    {CMD_ZOOM_IN, KEY_PLUS, EDITOR_MOD_PRIMARY},
    {CMD_ZOOM_IN, KEY_PLUS, EDITOR_MOD_PRIMARY | VKR_INPUT_MOD_SHIFT},
    {CMD_ZOOM_OUT, KEY_MINUS, EDITOR_MOD_PRIMARY},
    {CMD_ZOOM_RESET, KEY_0, EDITOR_MOD_PRIMARY},
    /* Workbenches, as browsers switch tabs. */
    {CMD_WORKBENCH_1, KEY_1, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_2, KEY_2, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_3, KEY_3, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_4, KEY_4, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_5, KEY_5, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_6, KEY_6, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_7, KEY_7, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_8, KEY_8, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_9, KEY_9, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_PREV, KEY_PRIOR, EDITOR_MOD_PRIMARY},
    {CMD_WORKBENCH_NEXT, KEY_NEXT, EDITOR_MOD_PRIMARY},
    {CMD_HELP, KEY_F1, 0u},
    /* The Scene and its selection. */
    {CMD_TOOL_SELECT, KEY_Q, 0u},
    {CMD_TOOL_MOVE, KEY_W, 0u},
    {CMD_TOOL_ROTATE, KEY_E, 0u},
    {CMD_TOOL_SCALE, KEY_R, 0u},
    {CMD_FRAME, KEY_F, VKR_INPUT_MOD_SHIFT},
    {CMD_SNAP, KEY_END, 0u},
    {CMD_DUPLICATE, KEY_D, EDITOR_MOD_PRIMARY},
    {CMD_DELETE, KEY_DELETE, 0u},
#if defined(PLATFORM_APPLE)
    {CMD_DELETE, KEY_BACKSPACE, 0u},
#endif
    {CMD_RENAME, KEY_F2, 0u},
    /* Hide, isolate and show all, as Blender does. */
    {CMD_HIDE, KEY_H, 0u},
    {CMD_ISOLATE, KEY_H, VKR_INPUT_MOD_SHIFT},
    {CMD_REVEAL, KEY_H, VKR_INPUT_MOD_ALT},
    {CMD_CAMERA, KEY_F, 0u, true_v},
    {CMD_CAMERA, KEY_F3, 0u, true_v},
    /* Level design. */
    {CMD_BRUSH_DRAW, KEY_B, 0u},
    {CMD_BRUSH_CLIP, KEY_X, VKR_INPUT_MOD_SHIFT},
    /* Play. */
    {CMD_PLAY, KEY_P, EDITOR_MOD_PRIMARY},
    {CMD_SIM_TOGGLE, KEY_P, EDITOR_MOD_PRIMARY | VKR_INPUT_MOD_SHIFT},
    {CMD_SIM_STEP, KEY_P, EDITOR_MOD_PRIMARY | VKR_INPUT_MOD_ALT},
    /* Build. */
    {CMD_BUILD_RUN, KEY_B, EDITOR_MOD_PRIMARY},
    {CMD_BUILD_SETTINGS, KEY_B, EDITOR_MOD_PRIMARY | VKR_INPUT_MOD_SHIFT},
};

/* The printed name of `key` in `out`; false for a key the keymap does not
   use. */
static bool8_t editor_key_name(Keys key, char *out, uint64_t size) {
  if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_0 && key <= KEY_9)) {
    snprintf(out, size, "%c", (char)key);
    return true_v;
  }
  const char *name = NULL;
  switch (key) {
  case KEY_SPACE:
    name = "Space";
    break;
  case KEY_PLUS:
    name = "=";
    break;
  case KEY_MINUS:
    name = "-";
    break;
#if defined(PLATFORM_APPLE)
  case KEY_DELETE:
    name = "\xe2\x8c\xa6";
    break;
  case KEY_BACKSPACE:
    name = "\xe2\x8c\xab";
    break;
  case KEY_END:
    name = "Fn+\xe2\x86\x92";
    break;
#else
  case KEY_DELETE:
    name = "Delete";
    break;
  case KEY_BACKSPACE:
    name = "Backspace";
    break;
  case KEY_END:
    name = "End";
    break;
#endif
  case KEY_PRIOR:
    name = "PgUp";
    break;
  case KEY_NEXT:
    name = "PgDn";
    break;
  case KEY_F1:
    name = "F1";
    break;
  case KEY_F2:
    name = "F2";
    break;
  case KEY_F3:
    name = "F3";
    break;
  default:
    return false_v;
  }
  snprintf(out, size, "%s", name);
  return true_v;
}

/* Writes the first binding of `command` as its platform's shortcut text;
   false when the command has none. */
static bool8_t editor_shortcut_text(EditorCommand command, char *out,
                                    uint64_t size) {
  for (uint32_t i = 0; i < ArrayCount(s_keymap); ++i) {
    const EditorKeyBinding *binding = &s_keymap[i];
    char key[16];
    if (binding->command != command ||
        !editor_key_name(binding->key, key, sizeof(key))) {
      continue;
    }
    const uint8_t mods = binding->modifiers;
#if defined(PLATFORM_APPLE)
    snprintf(out, size, "%s%s%s%s%s",
             mods & VKR_INPUT_MOD_CONTROL ? "\xe2\x8c\x83" : "",
             mods & VKR_INPUT_MOD_ALT ? "\xe2\x8c\xa5" : "",
             mods & VKR_INPUT_MOD_SHIFT ? "\xe2\x87\xa7" : "",
             mods & VKR_INPUT_MOD_SUPER ? "\xe2\x8c\x98" : "", key);
#else
    snprintf(out, size, "%s%s%s%s", mods & VKR_INPUT_MOD_CONTROL ? "Ctrl+" : "",
             mods & VKR_INPUT_MOD_SHIFT ? "Shift+" : "",
             mods & VKR_INPUT_MOD_ALT ? "Alt+" : "", key);
#endif
    return true_v;
  }
  return false_v;
}

bool8_t vkr_editor_command_shortcut(EditorCommand command, char *out,
                                    uint64_t size) {
  return editor_shortcut_text(command, out, size);
}

String8 vkr_editor_command_tooltip(VkrUiSystem *ui, EditorCommand command,
                                   const char *text) {
  char shortcut[48];
  if (!text) {
    text = s_commands[command].name;
  }
  return editor_shortcut_text(command, shortcut, sizeof(shortcut))
             ? string8_create_formatted(ui->frame_allocator, "%s  (%s)", text,
                                        shortcut)
             : string8_create_formatted(ui->frame_allocator, "%s", text);
}

/* The Help window: every bound command, then the mouse and debug controls. */
static String8 editor_help_text(VkrUiSystem *ui) {
  char text[4096];
  uint64_t used = 0u;
  for (uint32_t command = 0; command < CMD_COUNT; ++command) {
    char shortcut[48];
    if (used < sizeof(text) &&
        editor_shortcut_text((EditorCommand)command, shortcut,
                             sizeof(shortcut))) {
      used += (uint64_t)snprintf(text + used, sizeof(text) - used, "%s\t%s\n",
                                 shortcut, s_commands[command].name);
    }
  }
  if (used < sizeof(text)) {
    snprintf(text + used, sizeof(text) - used, "%s", s_help_mouse_text);
  }
  return string8_create_formatted(ui->frame_allocator, "%s", text);
}

static bool8_t editor_panel_visible(const VkrSampleUiFrame *frame,
                                    VkrUiDockPanelKind kind) {
  return vkr_ui_dock_find_panel(frame->dock, kind, NULL, NULL);
}

static bool8_t editor_any_additive(const VkrSampleUiFrame *frame) {
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i])
      return true_v;
  }
  return false_v;
}

/* Undo and redo cover every container's journal (ADR-076). */
static bool8_t editor_can_undo(const VkrSampleUiFrame *frame, bool8_t redo) {
  const VkrSceneEditState *journals[VKR_SCENE_ADDITIVE_MAX + 2u];
  uint32_t count = 0u;
  if (frame->scene)
    journals[count++] = frame->edits;
  if (frame->world)
    journals[count++] = frame->world_edits;
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (frame->additive[i])
      journals[count++] = frame->additive_edits[i];
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (redo ? journals[i]->undo_cursor < journals[i]->undo_count
             : journals[i]->undo_cursor > 0)
      return true_v;
  }
  return false_v;
}

bool8_t vkr_editor_command_enabled(EditorCommand command,
                                   const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  switch (command) {
  case CMD_LOAD:
    return frame->scene == NULL && !frame->scene_loading;
  case CMD_RELOAD:
  case CMD_UNLOAD:
    return frame->scene != NULL;
  /* Play simulates the scene, or the World while no scene is open. */
  case CMD_SIM_RESET:
    return frame->scene != NULL || frame->world != NULL;
  /* Save, undo and redo cover the scene and the root World (ADR-076). */
  case CMD_SAVE:
    return frame->scene != NULL || frame->world != NULL ||
           editor_any_additive(frame);
  case CMD_UNDO:
  case CMD_REDO:
    return editor_can_undo(frame, command == CMD_REDO);
  case CMD_REVEAL:
    return editor->hidden_count > 0u;
  case CMD_FRAME:
  case CMD_DUPLICATE:
  case CMD_DELETE:
  case CMD_RENAME:
  case CMD_SNAP:
  case CMD_HIDE:
  case CMD_ISOLATE: {
    const VkrScene *scene =
        vkr_editor_entity_scene(frame, frame->selected_entity);
    return scene && vkr_scene_entity_alive(scene, frame->selected_entity) &&
           ((command != CMD_DELETE && command != CMD_DUPLICATE) ||
            !vkr_editor_scene_panels_cooking(editor->scene_panels,
                                             frame->selected_entity)) &&
           (command != CMD_DUPLICATE ||
            vkr_scene_edit_can_duplicate(scene, frame->selected_entity, NULL));
  }
  case CMD_TOOL_SELECT:
  case CMD_TOOL_MOVE:
  case CMD_TOOL_ROTATE:
  case CMD_TOOL_SCALE:
    return frame->view_request != NULL && frame->mapping_valid &&
           !frame->scene_rendering_stopped;
  case CMD_PLAY:
    return frame->scene != NULL || frame->world != NULL;
  case CMD_SIM_START:
    return !frame->simulation_running;
  case CMD_SIM_PAUSE:
    return frame->simulation_running;
  case CMD_SIM_STEP:
    return (frame->scene || frame->world) && !frame->simulation_running;
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
  case CMD_BUILD:
  case CMD_BUILD_RUN:
    return vkr_editor_build_available(editor->build, editor);
  case CMD_BUILD_SETTINGS:
    return vkr_editor_projects_project(editor->projects) != NULL;
  case CMD_BUILD_OPEN:
    return vkr_editor_build_has_package(editor->build);
  case CMD_SCENE_BAKE:
    return frame->scene != NULL &&
           vkr_editor_bakery_scene_bake_available(editor->bakery);
  case CMD_WORKBENCH_1:
  case CMD_WORKBENCH_2:
  case CMD_WORKBENCH_3:
  case CMD_WORKBENCH_4:
  case CMD_WORKBENCH_5:
  case CMD_WORKBENCH_6:
  case CMD_WORKBENCH_7:
  case CMD_WORKBENCH_8:
  case CMD_WORKBENCH_9:
    return !frame->scene_only &&
           (uint32_t)(command - CMD_WORKBENCH_1) < editor->workbenches.count;
  case CMD_WORKBENCH_PREV:
  case CMD_WORKBENCH_NEXT:
    return !frame->scene_only;
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
  case CMD_SCRIPT_EDITOR:
    return vkr_editor_window_shown(editor, frame, VKR_EDITOR_WINDOW_SCRIPT);
  case CMD_CHANGES:
    return editor->windows[VKR_EDITOR_WINDOW_CHANGES].visible;
  case CMD_BRUSH_DRAW:
    return editor->brush_draw;
  case CMD_BRUSH_CLIP:
    return editor->clip_tool;
  case CMD_LEVEL_CHECKS:
    return vkr_editor_window_shown(editor, frame, VKR_EDITOR_WINDOW_LEVEL);
  case CMD_TERRAIN:
    return vkr_editor_window_shown(editor, frame, VKR_EDITOR_WINDOW_TERRAIN);
  case CMD_PARTITION:
    return editor->windows[VKR_EDITOR_WINDOW_PARTITION].visible;
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
  case CMD_BUILD_SETTINGS:
    return editor->windows[VKR_EDITOR_WINDOW_BUILD].visible;
  case CMD_BUILD_LOG:
    return editor_panel_visible(frame, VKR_UI_DOCK_PANEL_BUILD);
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
  case CMD_SCRIPT_EDITOR:
    editor_window_command(editor, frame, VKR_EDITOR_WINDOW_SCRIPT);
    break;
  case CMD_CHANGES:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_CHANGES);
    break;
  case CMD_HIDE:
  case CMD_ISOLATE:
    vkr_editor_hide_selection(editor, frame, command == CMD_ISOLATE);
    break;
  case CMD_REVEAL:
    vkr_editor_hide_reveal(editor);
    break;
  case CMD_BRUSH_DRAW:
  case CMD_BRUSH_CLIP: {
    const VkrEditorSceneTool tool = command == CMD_BRUSH_DRAW
                                        ? VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW
                                        : VKR_EDITOR_SCENE_TOOL_CLIP;
    vkr_editor_scene_tool_set(editor, vkr_editor_scene_tool(editor) == tool
                                          ? VKR_EDITOR_SCENE_TOOL_NONE
                                          : tool);
    break;
  }
  case CMD_LEVEL_CHECKS:
    editor_window_command(editor, frame, VKR_EDITOR_WINDOW_LEVEL);
    break;
  case CMD_WORKBENCH_1:
  case CMD_WORKBENCH_2:
  case CMD_WORKBENCH_3:
  case CMD_WORKBENCH_4:
  case CMD_WORKBENCH_5:
  case CMD_WORKBENCH_6:
  case CMD_WORKBENCH_7:
  case CMD_WORKBENCH_8:
  case CMD_WORKBENCH_9:
  case CMD_WORKBENCH_PREV:
  case CMD_WORKBENCH_NEXT: {
    const uint32_t count = editor->workbenches.count;
    const uint32_t active = editor->workbenches.active;
    const uint32_t target =
        command == CMD_WORKBENCH_PREV   ? (active + count - 1u) % count
        : command == CMD_WORKBENCH_NEXT ? (active + 1u) % count
                                        : (uint32_t)(command - CMD_WORKBENCH_1);
    char message[96];
    if (!vkr_editor_workbench_request(editor, frame, target, message,
                                      sizeof(message)))
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    break;
  }
  case CMD_TERRAIN:
    editor_window_command(editor, frame, VKR_EDITOR_WINDOW_TERRAIN);
    break;
  case CMD_PARTITION:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_PARTITION);
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
    vkr_editor_workbench_reset(editor, frame);
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
  case CMD_PLAY:
    *frame->transport_action =
        frame->simulation_running || frame->simulation_time > 0.0
            ? VKR_SAMPLE_TRANSPORT_RESET_SIMULATION
            : VKR_SAMPLE_TRANSPORT_START_SIMULATION;
    break;
  case CMD_TOOL_SELECT:
  case CMD_TOOL_MOVE:
  case CMD_TOOL_ROTATE:
  case CMD_TOOL_SCALE: {
    static const VkrGizmoMode modes[] = {
        VKR_GIZMO_MODE_NONE, VKR_GIZMO_MODE_TRANSLATE, VKR_GIZMO_MODE_ROTATE,
        VKR_GIZMO_MODE_SCALE};
    VkrSampleViewState next = frame->view_state;
    next.gizmo_tool = modes[command - CMD_TOOL_SELECT];
    *frame->view_request =
        (VkrSampleViewRequest){.value = next, .apply = true_v};
    break;
  }
  case CMD_DUPLICATE:
  case CMD_DELETE: {
    /* Tiles selected on a brush's grid go before the brush does. */
    if (command == CMD_DELETE && vkr_editor_brush_grid_patch_cut(editor)) {
      break;
    }
    const VkrSceneEditAction action = command == CMD_DUPLICATE
                                          ? VKR_SCENE_EDIT_DUPLICATE
                                          : VKR_SCENE_EDIT_DELETE;
    /* Several selected objects change together, as one undo step. */
    if (editor->selection_extra_count) {
      char message[160];
      if (!vkr_editor_selection_apply(editor, frame, action, message,
                                      sizeof(message)))
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                         vkr_ui_theme()->warning, message);
      break;
    }
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = action, .entity = frame->selected_entity};
    break;
  }
  case CMD_RENAME:
    vkr_editor_scene_panels_request_rename(editor->scene_panels);
    break;
  case CMD_SNAP: {
    char message[160];
    if (!vkr_editor_viewport_snap(editor, frame, frame->selected_entity,
                                  message, sizeof(message))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    }
    break;
  }
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
  case CMD_BUILD:
  case CMD_BUILD_RUN: {
    char message[256];
    const bool8_t started =
        vkr_editor_build_start(editor->build, editor, command == CMD_BUILD_RUN,
                               message, sizeof(message));
    vkr_editor_toast(
        editor, started ? VKR_UI_ICON_EXPORT : VKR_UI_ICON_WARNING_FILL,
        started ? vkr_ui_theme()->accent_hover : vkr_ui_theme()->warning,
        message);
    break;
  }
  case CMD_BUILD_SETTINGS:
    editor_window_toggle(editor, VKR_EDITOR_WINDOW_BUILD);
    break;
  case CMD_BUILD_OPEN: {
    char message[256];
    const bool8_t opened =
        vkr_editor_build_open_last(editor->build, message, sizeof(message));
    vkr_editor_toast(
        editor, opened ? VKR_UI_ICON_FOLDER : VKR_UI_ICON_WARNING_FILL,
        opened ? vkr_ui_theme()->accent_hover : vkr_ui_theme()->warning,
        message);
    break;
  }
  case CMD_BUILD_LOG:
    vkr_editor_dock_toggle(frame->dock, VKR_UI_DOCK_PANEL_BUILD);
    break;
  case CMD_SCENE_BAKE:
    (void)vkr_editor_bakery_request_scene_bake(editor->bakery, true_v, true_v);
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
    {CMD_DUPLICATE, true_v},
    {CMD_RENAME},
    {CMD_DELETE},
    {CMD_FRAME, true_v},
    {CMD_HIDE, true_v},
    {CMD_ISOLATE},
    {CMD_REVEAL},
    {CMD_COMMANDS, true_v},
};
static const EditorMenuEntry s_view_menu[] = {
    {CMD_HIERARCHY},
    {CMD_INSPECTOR},
    {CMD_CONTENT},
    {CMD_CONSOLE},
    {CMD_ANIMATION, true_v},
    {CMD_SCRIPT_EDITOR},
    {CMD_PHYSICS},
    {CMD_CHANGES},
    {CMD_BRUSH_DRAW},
    {CMD_BRUSH_CLIP},
    {CMD_LEVEL_CHECKS},
    {CMD_TERRAIN},
    {CMD_PARTITION},
    {CMD_GRAPHICS},
    {CMD_LABELS, true_v},
    {CMD_LABELS_DIRECTIONAL, false_v, true_v},
    {CMD_LABELS_SPOT, false_v, true_v},
    {CMD_LABELS_POINT, false_v, true_v},
    {CMD_ZOOM_IN, true_v},
    {CMD_ZOOM_OUT},
    {CMD_ZOOM_RESET},
    {CMD_REDUCE_MOTION},
    {CMD_WORKBENCH_PREV, true_v},
    {CMD_WORKBENCH_NEXT},
    {CMD_RESET_LAYOUT},
};
static const EditorMenuEntry s_scene_menu[] = {
    {CMD_PLAY},
    {CMD_SIM_TOGGLE},
    {CMD_SIM_STEP},
    {CMD_SIM_RESET},
    {CMD_RENDER_TOGGLE, true_v},
    {CMD_CAMERA},
    {CMD_SCENE_BAKE, true_v},
};
/* Build replaces Bakery for users; its recipes stay under Develop. */
static const EditorMenuEntry s_build_menu[] = {
    {CMD_BUILD},
    {CMD_BUILD_RUN},
    {CMD_BUILD_SETTINGS, true_v},
    {CMD_BUILD_OPEN, true_v},
    {CMD_BUILD_LOG},
};
static const EditorMenuEntry s_develop_menu[] = {
    {CMD_BAKERY},
    {CMD_DRAWS, true_v},
    {CMD_MEMORY},
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
    [VKR_EDITOR_MENU_BUILD] = {"Build", s_build_menu, ArrayCount(s_build_menu)},
    [VKR_EDITOR_MENU_DEVELOP] = {"Develop", s_develop_menu,
                                 ArrayCount(s_develop_menu)},
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
    /* Play enters and leaves play; the toggle pauses and resumes it. */
    const bool8_t playing =
        frame->simulation_running || frame->simulation_time > 0.0;
    if (entry->command == CMD_PLAY)
      name = playing ? string8_lit("Stop playing") : string8_lit("Play");
    if (entry->command == CMD_SIM_TOGGLE)
      name = frame->simulation_running ? string8_lit("Pause simulation")
             : playing                 ? string8_lit("Resume simulation")
                                       : string8_lit("Pause / Resume");
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
    char shortcut_text[48];
    if (editor_shortcut_text(entry->command, shortcut_text,
                             sizeof(shortcut_text))) {
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
          string8_create((uint8_t *)shortcut_text, strlen(shortcut_text)),
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
  VkrUiWidgetConfig button = vkr_editor_icon_button_config(
      column, 0u, info->icon, vkr_editor_command_tooltip(ui, command, NULL));
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

/* Play, step, stop, live rendering and the free camera, centered in the top
   bar's `column` between flexible gaps, so it never covers the menus or the
   status and command fields when the window narrows. */
static void editor_transport_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   uint32_t column) {
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
  group.placement.column = column;
  group.placement.row = 0u;
  group.placement.justify = group.placement.align = VKR_UI_ALIGN_CENTER;
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
      running
          ? vkr_editor_command_tooltip(ui, CMD_SIM_TOGGLE, "Pause simulation")
      : frame->simulation_time > 0.0
          ? vkr_editor_command_tooltip(ui, CMD_SIM_TOGGLE, "Resume simulation")
          : vkr_editor_command_tooltip(ui, CMD_PLAY, "Play simulation"));
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
      1u, 0u, VKR_UI_ICON_STEP,
      vkr_editor_command_tooltip(ui, CMD_SIM_STEP, NULL));
  step.style.min_size_pt = step.style.max_size_pt = (Vec2){button, button};
  step.disabled = !vkr_editor_command_enabled(CMD_SIM_STEP, editor, frame);
  if (vkr_ui_button(ui, string8_lit("step"), (String8){0}, &step))
    vkr_editor_command_execute(CMD_SIM_STEP, editor, frame);
  VkrUiWidgetConfig stop = vkr_editor_icon_button_config(
      2u, 0u, VKR_UI_ICON_STOP,
      vkr_editor_command_tooltip(ui, CMD_PLAY, "Stop and reset simulation"));
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
          : vkr_editor_command_tooltip(
                ui, CMD_CAMERA, "Free camera (or Tab; hold RMB in the Scene)"));
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
      {.unit = VKR_UI_TRACK_AUTO}, /* Build */
      {.unit = VKR_UI_TRACK_AUTO}, /* Develop */
      {.unit = VKR_UI_TRACK_AUTO}, /* Help */
      {.value = 12.0f, .unit = VKR_UI_TRACK_PX},
      {.unit = VKR_UI_TRACK_AUTO}, /* save */
      {.unit = VKR_UI_TRACK_AUTO}, /* undo */
      {.unit = VKR_UI_TRACK_AUTO}, /* redo */
      {.value = 1.0f, .unit = VKR_UI_TRACK_FR},
      {.unit = VKR_UI_TRACK_AUTO}, /* transport */
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

  editor_top_icon_button(editor, ui, frame, 10u, CMD_SAVE);
  editor_top_icon_button(editor, ui, frame, 11u, CMD_UNDO);
  editor_top_icon_button(editor, ui, frame, 12u, CMD_REDO);

  editor_transport_build(editor, frame, 14u);
  if (projects)
    vkr_editor_projects_navigation(editor->projects, editor, frame, 16u);

  if ((float32_t)ui->target_width / ui->content_scale >= 980.0f) {
    const bool8_t running = frame->simulation_running;
    const bool8_t frozen = frame->scene_rendering_stopped;
    VkrUiWidgetConfig status =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    status.placement = (VkrUiPlacement){
        .column = 18u,
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

  vkr_editor_cmd_bar_build(editor, frame, 19u);
  editor_caption_buttons_build(frame, 20u);
  (void)vkr_ui_panel_end(ui);
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
  const bool8_t gameplay = frame->scripts_running;
  /* The runtime formats performance as newline-separated lines. */
  String8 summary =
      frame->scene_rendering_stopped ? string8_lit("Frozen")
      : !frame->scene && !frame->world
          ? (frame->world_loading ? string8_lit("Loading World")
                                  : string8_lit("No scene loaded"))
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

/* Bottom-left chip while models stream into the World or a scene (ADR-076),
   or a background finalize encodes their full-quality textures (ADR-077):
   which assets, and for cooking how many materials are applied. Their
   Content items and Outliner rows are locked meanwhile. */
static void editor_scene_cooking_build(VkrEditorUi *editor,
                                       const VkrSampleUiFrame *frame) {
  if (!frame->mapping_valid || !editor->content) {
    return;
  }
  bool8_t running = false_v;
  uint32_t applied = 0u;
  vkr_editor_projects_finalize_stats(editor->projects, &running, &applied);
  char label[160];
  char revision[1][37];
  const bool8_t loading =
      vkr_editor_content_loading(editor->content, label, sizeof(label));
  if (!loading &&
      (!running || !vkr_editor_content_cooking(editor->content, revision, 1u,
                                               label, sizeof(label)))) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiSystem *ui = frame->ui;
  const float32_t scale = ui->content_scale;
  const Vec4 viewport = frame->mapping.panel_rect_px;
  if (viewport.z / scale < 260.0f || viewport.w / scale < 120.0f) {
    return;
  }
  VkrUiWidgetConfig chip = vkr_ui_widget_config_default();
  chip.placement = VKR_UI_PLACEMENT_DEFAULT;
  chip.placement.column = chip.placement.row = 0u;
  chip.placement.justify = VKR_UI_ALIGN_START;
  chip.placement.align = VKR_UI_ALIGN_END;
  chip.placement.margin_pt =
      (VkrUiEdges){0.0f, 0.0f,
                   Max(0.0f, (float32_t)ui->target_height / scale -
                                 (viewport.y + viewport.w) / scale + 10.0f),
                   viewport.x / scale + 10.0f};
  chip.style = vkr_editor_overlay_style();
  chip.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, 8.0f};
  chip.style.font_size_pt = theme->font_caption;
  chip.style.text_color = theme->text;
  chip.style.hover_background_color = theme->popup;
  /* The spinner pulses so the work reads as alive. */
  const float32_t pulse =
      0.55f + 0.45f * sinf((float32_t)vkr_platform_get_absolute_time() * 4.0f);
  chip.icon = VKR_UI_ICON_SPINNER;
  chip.icon_size_pt = 12.0f;
  chip.icon_color = vkr_ui_color_alpha(theme->accent_hover, pulse);
  chip.tooltip =
      loading ? string8_lit("The model streams into the viewport; it is "
                            "locked in Content until it shows.")
              : string8_lit("Full-quality textures encode in the background; "
                            "materials update as they finish. The cooking "
                            "assets are locked in Content and the Outliner "
                            "until then.");
  (void)vkr_ui_button(
      ui, string8_lit("editor.scene.cooking"),
      loading
          ? string8_create_formatted(ui->frame_allocator, "Loading %s", label)
          : string8_create_formatted(ui->frame_allocator,
                                     "Cooking %s  \xc2\xb7  %u materials ready",
                                     label, applied),
      &chip);
}

void vkr_editor_scene_overlays_build(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame) {
  (void)vkr_ui_input_layer_set(frame->ui, VKR_EDITOR_SCENE_TOOLBAR_LAYER);
  editor_scene_stats_build(frame);
  editor_scene_cooking_build(editor, frame);
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

/* The window and its resize borders, which its input layer covers. */
static VkrUiRect editor_window_input_rect(const VkrUiSystem *ui,
                                          const VkrEditorWindowState *window) {
  VkrUiRect rect = editor_window_rect(ui, window);
  rect.width += EDITOR_WINDOW_EDGE_PT * ui->content_scale;
  rect.height += EDITOR_WINDOW_EDGE_PT * ui->content_scale;
  return rect;
}

/* The resize borders (VKR_EDITOR_WINDOW_EDGE_*) under a point; zero inside
   the window and away from it. */
static uint8_t editor_window_edges_at(const VkrEditorWindowState *window,
                                      Vec2 point_pt) {
  const float32_t right = window->position_pt.x + window->size_pt.x;
  const float32_t bottom = window->position_pt.y + window->size_pt.y;
  if (point_pt.x < window->position_pt.x ||
      point_pt.y < window->position_pt.y ||
      point_pt.x >= right + EDITOR_WINDOW_EDGE_PT ||
      point_pt.y >= bottom + EDITOR_WINDOW_EDGE_PT) {
    return 0u;
  }

  uint8_t edges = 0u;
  if (point_pt.x >= right ||
      (point_pt.y >= bottom && point_pt.x >= right - EDITOR_WINDOW_CORNER_PT)) {
    edges |= VKR_EDITOR_WINDOW_EDGE_RIGHT;
  }
  if (point_pt.y >= bottom ||
      (point_pt.x >= right && point_pt.y >= bottom - EDITOR_WINDOW_CORNER_PT)) {
    edges |= VKR_EDITOR_WINDOW_EDGE_BOTTOM;
  }
  return edges;
}

static VkrWindowCursor editor_window_edge_cursor(uint8_t edges) {
  return edges == VKR_EDITOR_WINDOW_EDGE_BOTTOM ? VKR_WINDOW_CURSOR_RESIZE_NS
                                                : VKR_WINDOW_CURSOR_RESIZE_EW;
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
                               editor_window_input_rect(ui, window))) {
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
                                        editor_window_input_rect(ui, window));
  }
  if (editor->menu != VKR_EDITOR_MENU_NONE)
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER,
                                      editor_menu_popup_rect(editor, ui));
  if (editor->context_open) {
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER,
                                      vkr_editor_context_menu_rect(editor, ui));
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER,
                                      vkr_editor_context_submenu_rect(editor));
  }
}

/* The body panel below a floating window's header, and its bounds in
 * pixels, for windows whose module lays out its own content. */
static bool8_t editor_window_body_begin(VkrUiSystem *ui,
                                        const VkrEditorWindowState *window,
                                        String8 id, VkrUiRect *out_bounds) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiPanelConfig body = vkr_ui_panel_config_default();
  body.placement.column = 0;
  body.placement.row = 1;
  body.style.padding_pt = (VkrUiEdges){0};
  body.style.background_color = theme->panel;
  body.style.corner_radius_pt =
      (Vec4){0.0f, 0.0f, theme->radius_large, theme->radius_large};
  body.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, id, &body)) {
    return false_v;
  }
  *out_bounds =
      (VkrUiRect){window->position_pt.x * ui->content_scale,
                  (window->position_pt.y + 30.0f) * ui->content_scale,
                  window->size_pt.x * ui->content_scale,
                  Max(1.0f, window->size_pt.y - 30.0f) * ui->content_scale};
  return true_v;
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
  case VKR_EDITOR_WINDOW_SCRIPT:
    title_text = string8_lit("Script editor");
    title_icon = VKR_UI_ICON_CODE;
    break;
  case VKR_EDITOR_WINDOW_CHANGES:
    title_text = string8_lit("Agent changes");
    title_icon = VKR_UI_ICON_TERMINAL;
    break;
  case VKR_EDITOR_WINDOW_LEVEL:
    title_text = string8_lit("Level checks");
    title_icon = VKR_UI_ICON_PERSON_WALK;
    break;
  case VKR_EDITOR_WINDOW_TERRAIN:
    title_text = string8_lit("Terrain");
    title_icon = VKR_UI_ICON_WAVES;
    break;
  case VKR_EDITOR_WINDOW_PARTITION:
    title_text = string8_lit("World partition");
    title_icon = VKR_UI_ICON_GRID;
    break;
  case VKR_EDITOR_WINDOW_CREATE:
    title_text = string8_lit("Create or import");
    title_icon = VKR_UI_ICON_ADD;
    break;
  case VKR_EDITOR_WINDOW_BUILD:
    title_text = string8_lit("Build settings");
    title_icon = VKR_UI_ICON_EXPORT;
    break;
  case VKR_EDITOR_WINDOW_GRAPHICS:
    title_text = string8_lit("Preferences");
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
    body_text = editor_help_text(ui);
    monospace = false_v;
    break;
  default:
    return;
  }

  /* The pointer reaches this window when no window, menu or popup above it
     covers the point; only then do its title and borders take a press. */
  const bool8_t topmost = ui->mouse_input_layer == window->z_order + 2u;
  const Vec2 mouse = {ui->mouse_x / ui->content_scale,
                      ui->mouse_y / ui->content_scale};
  if (ui->mouse_pressed && !frame->mouse_captured && topmost &&
      editor->menu == VKR_EDITOR_MENU_NONE) {
    int32_t press_x = 0;
    int32_t press_y = 0;
    input_get_button_press_position(input, BUTTON_LEFT, &press_x, &press_y);
    const Vec2 press = {press_x / ui->content_scale,
                        press_y / ui->content_scale};
    const uint8_t edges = editor_window_edges_at(window, press);
    if (edges) {
      /* The grab keeps the border's offset from the pointer. */
      window->resize_edges = edges;
      window->drag_grab_pt =
          (Vec2){press.x - window->position_pt.x - window->size_pt.x,
                 press.y - window->position_pt.y - window->size_pt.y};
    } else if (press.x >= window->position_pt.x &&
               press.x < window->position_pt.x + window->size_pt.x - 30 &&
               press.y >= window->position_pt.y &&
               press.y < window->position_pt.y + 28) {
      window->dragging = true_v;
      window->drag_grab_pt = (Vec2){press.x - window->position_pt.x,
                                    press.y - window->position_pt.y};
    }
  }
  const bool8_t down = input_is_button_down(input, BUTTON_LEFT);
  if (window->resize_edges && !frame->mouse_captured &&
      (down || ui->mouse_released)) {
    const float32_t width_pt = (float32_t)ui->target_width / ui->content_scale;
    const float32_t height_pt =
        (float32_t)ui->target_height / ui->content_scale;
    if (window->resize_edges & VKR_EDITOR_WINDOW_EDGE_RIGHT) {
      const float32_t right = Min(mouse.x - window->drag_grab_pt.x, width_pt);
      window->size_pt.x =
          Max(EDITOR_WINDOW_MIN_WIDTH_PT, right - window->position_pt.x);
    }
    if (window->resize_edges & VKR_EDITOR_WINDOW_EDGE_BOTTOM) {
      const float32_t bottom = Min(mouse.y - window->drag_grab_pt.y, height_pt);
      window->size_pt.y =
          Max(EDITOR_WINDOW_MIN_HEIGHT_PT, bottom - window->position_pt.y);
    }
    ui->capture.mouse = true_v;
    ui->cursor = editor_window_edge_cursor(window->resize_edges);
  } else if (!window->dragging && !down && topmost && !frame->mouse_captured) {
    const uint8_t edges = editor_window_edges_at(window, mouse);
    if (edges) {
      ui->cursor = editor_window_edge_cursor(edges);
    }
  }
  if (!down || frame->mouse_captured) {
    window->resize_edges = 0u;
  }
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
    /* The handle draws nothing itself; the title label below carries the
     * text and icon so they appear once. */
    VkrUiWidgetConfig handle = drag;
    handle.icon = VKR_UI_ICON_NONE;
    (void)vkr_ui_button(ui, string8_lit("drag"), (String8){0}, &handle);
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

  if (kind == VKR_EDITOR_WINDOW_GRAPHICS) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("preferences.body"),
                                 &bounds)) {
      vkr_editor_graphics_build(editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_ANIMATION) {
    vkr_editor_animation_build(editor, frame);
  } else if (kind == VKR_EDITOR_WINDOW_SCRIPT) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("script.body"),
                                 &bounds)) {
      vkr_editor_code_build(editor->code, editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_CREATE) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("create.body"),
                                 &bounds)) {
      vkr_editor_projects_build_create_window(editor->projects, editor, frame,
                                              bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_BUILD) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("build.body"),
                                 &bounds)) {
      vkr_editor_build_settings_build(editor->build, editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_TERRAIN) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("terrain.body"),
                                 &bounds)) {
      vkr_editor_terrain_window_build(editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_PARTITION) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("partition.body"),
                                 &bounds)) {
      vkr_editor_partition_window_build(editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_LEVEL) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("level.body"),
                                 &bounds)) {
      vkr_editor_level_window_build(editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_CHANGES) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(ui, window, string8_lit("changes.body"),
                                 &bounds)) {
      vkr_editor_changes_build(editor, frame, bounds);
      (void)vkr_ui_panel_end(ui);
    }
  } else if (kind == VKR_EDITOR_WINDOW_PHYSICS) {
    VkrUiRect bounds = {0};
    if (editor_window_body_begin(
            ui, window, string8_lit("physics.settings.body"), &bounds)) {
      vkr_editor_physics_settings_build(editor->physics_settings, frame, bounds,
                                        editor->heading_font);
      (void)vkr_ui_panel_end(ui);
    }
  } else {
    /* Text longer than the window scrolls. */
    const VkrUiTrack text_row = {.unit = VKR_UI_TRACK_AUTO};
    VkrUiPanelConfig scroll = vkr_ui_panel_config_default();
    scroll.placement.column = 0u;
    scroll.placement.row = 1u;
    scroll.rows = &text_row;
    scroll.row_count = 1u;
    scroll.style.padding_pt = (VkrUiEdges){10.0f, 14.0f, 10.0f, 12.0f};
    if (vkr_ui_scroll_area_begin(ui, string8_lit("body.scroll"), &scroll)) {
      VkrUiWidgetConfig body = vkr_editor_text_config(
          monospace ? theme->font_caption : theme->font_body,
          monospace ? theme->text_secondary : theme->text);
      if (monospace)
        body.text.font = editor->mono_font;
      body.placement = (VkrUiPlacement){
          .column = 0u,
          .row = 0u,
          .column_span = 1u,
          .row_span = 1u,
          .justify = VKR_UI_ALIGN_START,
          .align = VKR_UI_ALIGN_START,
      };
      vkr_ui_label(ui, string8_lit("body"), body_text, &body);
      (void)vkr_ui_scroll_area_end(ui);
    }
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

/* Runs the command bound to a key pressed this frame. A plain key acts while
   the Scene or no widget holds the keyboard, a modified one unless a text
   field does; nothing acts while the camera flies, a menu or the Cmd bar is
   open, or another window claimed the keys. The editor owns save and undo,
   so the runtime's fallback for them stays off. */
static void editor_keymap_update(VkrEditorUi *editor,
                                 const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  if (frame->scene_shortcuts_blocked) {
    if (*frame->scene_shortcuts_blocked) {
      return;
    }
    *frame->scene_shortcuts_blocked = true_v;
  }
  if (frame->mouse_captured || editor->cmd_active ||
      editor->menu != VKR_EDITOR_MENU_NONE || editor->context_open ||
      ui->focused_is_text) {
    return;
  }
  const bool8_t scene_focus =
      frame->scene_keyboard_focus && *frame->scene_keyboard_focus;
  const bool8_t widget_focus = !scene_focus && ui->focused_id != VKR_UI_ID_NONE;
  for (uint32_t i = 0; i < ArrayCount(s_keymap); ++i) {
    const EditorKeyBinding *binding = &s_keymap[i];
    if (binding->runtime ||
        !input_key_just_pressed(frame->input, binding->key)) {
      continue;
    }
    const uint8_t mods =
        input_key_press_modifiers(frame->input, binding->key) & EDITOR_MODS;
    if (mods != binding->modifiers || (!mods && widget_focus) ||
        !vkr_editor_command_enabled(binding->command, editor, frame)) {
      continue;
    }
    vkr_editor_command_execute(binding->command, editor, frame);
    ui->capture.keyboard = true_v;
    return;
  }
}

void vkr_editor_commands_update(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame) {
  if (editor->menu != VKR_EDITOR_MENU_NONE) {
    /* A press anywhere but the popup and its own title closes the menu,
       including the rest of the top bar; the title's click toggles it. */
    VkrUiSystem *ui = frame->ui;
    const bool8_t outside =
        ui->mouse_pressed &&
        !editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                              editor->menu_anchor_px) &&
        !editor_point_in_rect(ui->mouse_x, ui->mouse_y,
                              editor_menu_popup_rect(editor, ui));
    if (outside || input_key_just_pressed(frame->input, KEY_ESCAPE)) {
      editor_menu_close(editor, ui);
    }
  }
  editor_keymap_update(editor, frame);
}

/* ---- Context menus ---- */

typedef enum EditorContextAction {
  /* Separators, headings and rows that open a submenu. */
  CONTEXT_NONE = 0,
  CONTEXT_FRAME,
  CONTEXT_SNAP,
  CONTEXT_VISIBILITY,
  CONTEXT_ISOLATE,
  CONTEXT_REVEAL,
  CONTEXT_RENAME,
  CONTEXT_COPY_NAME,
  CONTEXT_TAB_CLOSE,
  CONTEXT_TAB_LAYOUT,
  /* The workbench tab `context_panel` names. */
  CONTEXT_WORKBENCH_SWITCH,
  CONTEXT_WORKBENCH_RESET,
  CONTEXT_WORKBENCH_DUPLICATE,
  CONTEXT_WORKBENCH_RENAME,
  CONTEXT_WORKBENCH_LEFT,
  CONTEXT_WORKBENCH_RIGHT,
  CONTEXT_WORKBENCH_DELETE,
  CONTEXT_CONSOLE_COPY,
  CONTEXT_CONSOLE_CLEAR,
  CONTEXT_DETACH,
  CONTEXT_DUPLICATE,
  CONTEXT_DELETE,
  CONTEXT_CREATE,
  CONTEXT_IMPORT_MODEL,
  CONTEXT_ADD_COMPONENT,
  CONTEXT_ADD_PHYSICS,
  CONTEXT_PRESET_SAVE,
  CONTEXT_PRESET_APPLY,
  /* A Content item's command; `value` is its VkrEditorContentCommand. */
  CONTEXT_CONTENT_COMMAND,
  /* The shown Content folder's command; `value` is its
     VkrEditorContentFolderCommand. */
  CONTEXT_FOLDER_COMMAND,
  /* `value` indexes the loaded script types; UINT32_MAX removes the
     script. */
  CONTEXT_SCRIPT_SET,
  CONTEXT_SCRIPT_NEW,
  CONTEXT_SCRIPT_EDIT,
  /* A text field's command; `value` is its VkrUiTextCommand. */
  CONTEXT_TEXT_COMMAND,
  /* A Details enum dropdown's pick; `value` indexes its names. */
  CONTEXT_CHOICE_SET,
} EditorContextAction;

typedef enum EditorContextRow {
  CONTEXT_ROW_ITEM = 0,
  CONTEXT_ROW_SEPARATOR,
  CONTEXT_ROW_HEADER,
} EditorContextRow;

typedef struct EditorContextItem {
  const char *label;
  VkrUiIcon icon;
  /* Trailing text: a shortcut or the current value. */
  const char *shortcut;
  bool8_t disabled;
  EditorContextAction action;
  /* Object kind or world type index for creation and component items. */
  uint32_t value;
  EditorContextRow row;
  bool8_t checked;
  /* The row opens `submenu` beside the menu instead of acting. */
  bool8_t opens;
  VkrEditorContextKind submenu;
} EditorContextItem;

#define EDITOR_CONTEXT_ITEM_CAPACITY 48u
#define EDITOR_CONTEXT_WIDTH_PT 224.0f
#define EDITOR_CONTEXT_SEPARATOR_PT 9.0f
#define EDITOR_CONTEXT_HEADER_PT 22.0f
#define EDITOR_CONTEXT_SCRIPT_MAX 32u

void vkr_editor_context_open(VkrEditorUi *editor, VkrEditorContextKind kind,
                             Vec2 position_pt) {
  editor->context_open = true_v;
  editor->context_kind = kind;
  editor->context_position_pt = position_pt;
  editor->context_count = 0u;
  editor->context_height_pt = 0.0f;
  editor->context_cursor = -1;
  editor->context_sub_open = false_v;
  editor->context_sub_focused = false_v;
  editor->context_sub_cursor = -1;
  editor->context_sub_rect_px = (VkrUiRect){0};
  editor->context_at_pixel = false_v;
  editor->menu = VKR_EDITOR_MENU_NONE;
}

void vkr_editor_context_open_choice(VkrEditorUi *editor,
                                    VkrEditorDetails *details) {
  if (!details->choice_requested) {
    return;
  }
  details->choice_requested = false_v;
  details->choice_picked = false_v;
  vkr_editor_context_open(editor, VKR_EDITOR_CONTEXT_CHOICE,
                          details->choice_anchor_pt);
  editor->context_details = details;
}

/* Attaches a module made by "New script" to the object that asked for it
   once the module's types load. */
static void editor_script_attach_update(VkrEditorUi *editor,
                                        const VkrSampleUiFrame *frame) {
  const VkrEntityId entity = editor->script_attach_entity;
  if (!entity.u64) {
    return;
  }
  const VkrScene *scene = vkr_editor_entity_scene(frame, entity);
  if (!scene || !vkr_scene_entity_alive(scene, entity)) {
    editor->script_attach_entity = VKR_ENTITY_ID_INVALID;
    return;
  }
  if (!editor->script_attach_module[0] ||
      frame->scene_edit->action != VKR_SCENE_EDIT_NONE) {
    return;
  }
  const VkrScriptModule *module =
      frame->scripts
          ? vkr_script_host_module(frame->scripts, editor->script_attach_module)
          : NULL;
  if (!module || module->retired || !module->type_count) {
    return;
  }
  editor->script_attach_entity = VKR_ENTITY_ID_INVALID;
  vkr_editor_request_script(frame, entity, module->types[0]);
}

static void context_push(EditorContextItem *items, uint32_t *count,
                         EditorContextItem item) {
  if (*count < EDITOR_CONTEXT_ITEM_CAPACITY) {
    items[(*count)++] = item;
  }
}

/* A separator never leads, trails or doubles. */
static void context_separator(EditorContextItem *items, uint32_t *count) {
  if (*count && items[*count - 1u].row != CONTEXT_ROW_SEPARATOR) {
    context_push(items, count,
                 (EditorContextItem){.row = CONTEXT_ROW_SEPARATOR});
  }
}

static void context_header(EditorContextItem *items, uint32_t *count,
                           const char *label) {
  context_push(items, count,
               (EditorContextItem){.label = label, .row = CONTEXT_ROW_HEADER});
}

static float32_t context_row_height(const EditorContextItem *item) {
  return item->row == CONTEXT_ROW_SEPARATOR ? EDITOR_CONTEXT_SEPARATOR_PT
         : item->row == CONTEXT_ROW_HEADER  ? EDITOR_CONTEXT_HEADER_PT
                                            : EDITOR_MENU_ROW_PT;
}

static bool8_t context_row_selectable(const EditorContextItem *item) {
  return item->row == CONTEXT_ROW_ITEM && !item->disabled;
}

static float32_t context_height(const EditorContextItem *items,
                                uint32_t count) {
  float32_t height = EDITOR_MENU_PADDING_PT * 2.0f;
  for (uint32_t i = 0; i < count; ++i) {
    height += context_row_height(&items[i]);
  }
  return height;
}

/* Object creation grouped under headings, then the project's model import
   flow. */
static uint32_t editor_context_create_items(VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            EditorContextItem *items) {
  uint32_t count = 0u;
  const char *group = NULL;
  for (uint32_t kind = 0; kind < vkr_editor_object_kind_count(); ++kind) {
    if (!vkr_editor_object_kind_listed(kind)) {
      continue;
    }
    const char *kind_group = vkr_editor_object_kind_group(kind);
    if (kind_group && (!group || strcmp(group, kind_group))) {
      context_separator(items, &count);
      context_header(items, &count, kind_group);
      group = kind_group;
    }
    context_push(items, &count,
                 (EditorContextItem){vkr_editor_object_kind_label(kind),
                                     vkr_editor_object_kind_icon(kind), NULL,
                                     false_v, CONTEXT_CREATE, kind});
  }
  /* The project flow imports a model into the managed scene document. */
  if (vkr_editor_projects_can_add_entity(editor->projects, editor, frame)) {
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Model or light from project...",
                                     VKR_UI_ICON_IMPORT, NULL, false_v,
                                     CONTEXT_IMPORT_MODEL});
  }
  return count;
}

/* Live world component types the context entity's container may hold and the
   entity does not carry yet: a physics body when it has none, the loaded
   scripts, then the rest grouped by category. */
static uint32_t editor_context_component_items(VkrEditorUi *editor,
                                               const VkrSampleUiFrame *frame,
                                               const VkrScene *scene,
                                               EditorContextItem *items) {
  uint32_t count = 0u;
  VkrSceneEditValues values;
  if (vkr_scene_edit_read(scene, editor->context_entity, &values) &&
      !values.physics.present) {
    context_header(items, &count, "Physics");
    context_push(items, &count,
                 (EditorContextItem){"Physics body", VKR_UI_ICON_PHYSICS, NULL,
                                     false_v, CONTEXT_ADD_PHYSICS});
  }
  uint32_t eligible[EDITOR_CONTEXT_ITEM_CAPACITY];
  uint32_t eligible_count = 0u;
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)) &&
                       eligible_count < ArrayCount(eligible);
       ++i) {
    if (vkr_scene_world_type_live(type) &&
        !vkr_scene_world_type_registered(type) &&
        vkr_scene_type_allowed(scene, type) &&
        !vkr_scene_get_typed(scene, editor->context_entity, type)) {
      eligible[eligible_count++] = i;
    }
  }
  /* Scripts first: the loaded modules' types the entity does not carry, and
     a new module. */
  const VkrTypeDesc *scripts[EDITOR_CONTEXT_SCRIPT_MAX];
  const uint32_t script_count =
      vkr_editor_script_types(frame, scripts, ArrayCount(scripts));
  context_separator(items, &count);
  context_header(items, &count, "Scripts");
  for (uint32_t i = 0; i < script_count; ++i) {
    if (vkr_scene_get_typed(scene, editor->context_entity, scripts[i])) {
      continue;
    }
    for (uint32_t w = 0; (type = vkr_scene_world_type(w)); ++w) {
      if (type == scripts[i]) {
        context_push(items, &count,
                     (EditorContextItem){scripts[i]->label, VKR_UI_ICON_CODE,
                                         NULL,
                                         !vkr_scene_type_allowed(scene, type),
                                         CONTEXT_ADD_COMPONENT, w});
      }
    }
  }
  context_push(
      items, &count,
      (EditorContextItem){"New script...", VKR_UI_ICON_ADD, NULL,
                          !vkr_editor_scripts_project_open(editor->scripts),
                          CONTEXT_SCRIPT_NEW});
  /* Each category once, in the order it first appears. */
  for (uint32_t i = 0; i < eligible_count; ++i) {
    const char *category = vkr_scene_world_type(eligible[i])->category;
    bool8_t seen = false_v;
    for (uint32_t j = 0; j < i && !seen; ++j) {
      const char *other = vkr_scene_world_type(eligible[j])->category;
      seen = category && other && !strcmp(category, other);
    }
    if (seen) {
      continue;
    }
    if (category && strcmp(category, "Physics")) {
      context_separator(items, &count);
      context_header(items, &count, category);
    }
    for (uint32_t j = i; j < eligible_count; ++j) {
      type = vkr_scene_world_type(eligible[j]);
      const bool8_t same =
          category ? type->category && !strcmp(category, type->category)
                   : !type->category;
      if (same) {
        context_push(items, &count,
                     (EditorContextItem){type->label, VKR_UI_ICON_PUZZLE, NULL,
                                         false_v, CONTEXT_ADD_COMPONENT,
                                         eligible[j]});
      }
    }
  }
  return count;
}

/* The script slot: every loaded script type, the current one checked, then
   creation, editing and removal. */
static uint32_t editor_context_script_items(VkrEditorUi *editor,
                                            const VkrSampleUiFrame *frame,
                                            const VkrScene *scene,
                                            EditorContextItem *items) {
  uint32_t count = 0u;
  const VkrTypeDesc *current =
      vkr_editor_entity_script(scene, editor->context_entity);
  const VkrTypeDesc *types[EDITOR_CONTEXT_SCRIPT_MAX];
  const uint32_t type_count =
      vkr_editor_script_types(frame, types, ArrayCount(types));
  context_header(items, &count, "Project scripts");
  if (!type_count) {
    context_push(items, &count,
                 (EditorContextItem){"No scripts yet", VKR_UI_ICON_INFO_FILL,
                                     NULL, true_v, CONTEXT_NONE});
  }
  for (uint32_t i = 0; i < type_count; ++i) {
    context_push(items, &count,
                 (EditorContextItem){
                     .label = types[i]->label,
                     .icon = VKR_UI_ICON_CODE,
                     .disabled = !vkr_scene_type_allowed(scene, types[i]),
                     .action = CONTEXT_SCRIPT_SET,
                     .value = i,
                     .checked = types[i] == current});
  }
  context_separator(items, &count);
  context_push(
      items, &count,
      (EditorContextItem){"New script...", VKR_UI_ICON_ADD, NULL,
                          !vkr_editor_scripts_project_open(editor->scripts),
                          CONTEXT_SCRIPT_NEW});
  if (current) {
    char source[VKR_EDITOR_SCRIPT_PATH];
    context_push(
        items, &count,
        (EditorContextItem){"Edit script", VKR_UI_ICON_PENCIL_LINE, NULL,
                            !vkr_editor_script_source(editor, frame, current,
                                                      source, sizeof(source)),
                            CONTEXT_SCRIPT_EDIT});
    context_push(items, &count,
                 (EditorContextItem){"Remove script", VKR_UI_ICON_TRASH, NULL,
                                     false_v, CONTEXT_SCRIPT_SET, UINT32_MAX});
  }
  return count;
}

/* Save the context component as a preset, then apply any preset of its
   type. */
static uint32_t editor_context_preset_items(const VkrEditorUi *editor,
                                            EditorContextItem *items) {
  uint32_t count = 0u;
  context_push(items, &count,
               (EditorContextItem){"Save as preset", VKR_UI_ICON_SAVE, NULL,
                                   !editor->content, CONTEXT_PRESET_SAVE});
  const uint32_t total = vkr_editor_content_preset_count(editor->content);
  bool8_t headed = false_v;
  for (uint32_t i = 0; i < total; ++i) {
    const VkrEditorPreset *preset =
        vkr_editor_content_preset(editor->content, i);
    if (preset->type != editor->context_type) {
      continue;
    }
    if (!headed) {
      context_separator(items, &count);
      context_header(items, &count, "Apply preset");
      headed = true_v;
    }
    context_push(items, &count,
                 (EditorContextItem){preset->name, VKR_UI_ICON_SPARKLE, NULL,
                                     false_v, CONTEXT_PRESET_APPLY, i});
  }
  return count;
}

static uint32_t editor_context_entity_items(VkrEditorUi *editor,
                                            const VkrScene *scene,
                                            EditorContextItem *items) {
  const VkrEntityId entity = editor->context_entity;
  const SceneVisibility *visibility =
      vkr_entity_get_component(scene->world, entity, scene->comp_visibility);
  const SceneTransform *transform =
      vkr_entity_get_component(scene->world, entity, scene->comp_transform);
  const bool8_t hidden = (visibility && !visibility->visible) ||
                         !vkr_editor_hide_shown(editor, scene, entity);
  /* A model whose textures cook stays in place until they finish. */
  const bool8_t cooking =
      vkr_editor_scene_panels_cooking(editor->scene_panels, entity);
  VkrSceneEditValues values;
  const bool8_t renamable = vkr_scene_edit_read(scene, entity, &values) &&
                            (values.fields & VKR_SCENE_EDIT_NAME);
  const VkrTypeDesc *script = vkr_editor_entity_script(scene, entity);
  uint32_t count = 0u;
  context_push(items, &count,
               (EditorContextItem){"Frame", VKR_UI_ICON_FRAME,
                                   EDITOR_SHORTCUT("â§"
                                                   "F",
                                                   "Shift+F"),
                                   false_v, CONTEXT_FRAME});
  /* Snapping rests the object under the Scene's Snapping settings. */
  static const char *const snap_labels[VKR_EDITOR_SNAP_COUNT] = {
      "Snap to ground", "Snap to surface", "Snap to grid"};
  context_push(items, &count,
               (EditorContextItem){snap_labels[editor->placement.target],
                                   VKR_UI_ICON_SNAP,
                                   EDITOR_SHORTCUT("Fn+Right", "End"),
                                   cooking || !transform, CONTEXT_SNAP});
  context_push(
      items, &count,
      (EditorContextItem){hidden ? "Show" : "Hide",
                          hidden ? VKR_UI_ICON_EYE : VKR_UI_ICON_EYE_SLASH,
                          hidden ? NULL : "H", false_v, CONTEXT_VISIBILITY});
  context_push(items, &count,
               (EditorContextItem){"Isolate", VKR_UI_ICON_EYE, NULL, false_v,
                                   CONTEXT_ISOLATE});
  if (editor->hidden_count) {
    context_push(items, &count,
                 (EditorContextItem){"Show all hidden", VKR_UI_ICON_EYE,
                                     EDITOR_SHORTCUT("\xe2\x8c\xa5H", "Alt+H"),
                                     false_v, CONTEXT_REVEAL});
  }
  context_push(items, &count,
               (EditorContextItem){"Rename", VKR_UI_ICON_RENAME, NULL,
                                   !renamable, CONTEXT_RENAME});
  context_push(items, &count,
               (EditorContextItem){"Copy name", VKR_UI_ICON_COPY, NULL, false_v,
                                   CONTEXT_COPY_NAME});
  context_separator(items, &count);
  /* Placed objects carry a script slot; world settings do not. */
  if (transform) {
    context_push(items, &count,
                 (EditorContextItem){.label = "Script",
                                     .icon = VKR_UI_ICON_CODE,
                                     .shortcut = script ? script->label : NULL,
                                     .opens = true_v,
                                     .submenu = VKR_EDITOR_CONTEXT_SCRIPT});
  }
  context_push(
      items, &count,
      (EditorContextItem){.label = "Add component",
                          .icon = VKR_UI_ICON_PUZZLE,
                          .opens = true_v,
                          .submenu = VKR_EDITOR_CONTEXT_ADD_COMPONENT});
  context_separator(items, &count);
  context_push(
      items, &count,
      (EditorContextItem){"Detach from parent", VKR_UI_ICON_TREE, NULL,
                          cooking || !transform || !transform->parent.u64,
                          CONTEXT_DETACH});
  context_push(items, &count,
               (EditorContextItem){"Duplicate", VKR_UI_ICON_DUPLICATE,
                                   EDITOR_SHORTCUT("\xe2\x8c\x98"
                                                   "D",
                                                   "Ctrl+D"),
                                   cooking || !vkr_scene_edit_can_duplicate(
                                                  scene, entity, NULL),
                                   CONTEXT_DUPLICATE});
  context_push(items, &count,
               (EditorContextItem){
                   "Delete", VKR_UI_ICON_TRASH,
                   EDITOR_SHORTCUT("\xe2\x8c\xab", "Del"),
                   cooking || !vkr_scene_edit_can_delete(scene, entity, NULL),
                   CONTEXT_DELETE});
  return count;
}

/* Items for a menu kind; zero when its target no longer exists. */
static uint32_t editor_context_items(VkrEditorUi *editor,
                                     const VkrSampleUiFrame *frame,
                                     VkrEditorContextKind kind,
                                     EditorContextItem *items, char *label,
                                     uint32_t label_capacity) {
  const VkrScene *scene =
      vkr_editor_entity_scene(frame, editor->context_entity);
  const bool8_t entity_alive =
      scene && vkr_scene_entity_alive(scene, editor->context_entity);
  switch (kind) {
  case VKR_EDITOR_CONTEXT_ENTITY:
    return entity_alive ? editor_context_entity_items(editor, scene, items)
                        : 0u;
  case VKR_EDITOR_CONTEXT_CREATE:
    return editor_context_create_items(editor, frame, items);
  case VKR_EDITOR_CONTEXT_ADD_COMPONENT:
    return entity_alive
               ? editor_context_component_items(editor, frame, scene, items)
               : 0u;
  case VKR_EDITOR_CONTEXT_SCRIPT:
    return entity_alive
               ? editor_context_script_items(editor, frame, scene, items)
               : 0u;
  case VKR_EDITOR_CONTEXT_CHOICE: {
    const VkrEditorDetails *details = editor->context_details;
    const VkrPropertyDesc *property = details ? details->choice_property : NULL;
    const uint32_t choices = property ? vkr_property_enum_count(property) : 0u;
    uint32_t count = 0u;
    for (uint32_t i = 0; i < choices && i < EDITOR_CONTEXT_ITEM_CAPACITY; ++i) {
      context_push(
          items, &count,
          (EditorContextItem){.label = property->labels ? property->labels[i]
                                                        : property->names[i],
                              .action = CONTEXT_CHOICE_SET,
                              .value = i,
                              .checked = i == details->choice_current});
    }
    return count;
  }
  case VKR_EDITOR_CONTEXT_PRESET: {
    uint8_t value[VKR_TYPE_VALUE_MAX];
    if (!editor->context_type ||
        !vkr_editor_component_read(frame, editor->context_entity,
                                   editor->context_type, value))
      return 0u;
    return editor_context_preset_items(editor, items);
  }
  case VKR_EDITOR_CONTEXT_WORKBENCH: {
    const VkrEditorWorkbenches *workbenches = &editor->workbenches;
    const uint32_t workbench = editor->context_panel;
    if (workbench >= workbenches->count)
      return 0u;
    snprintf(label, label_capacity, "%s",
             vkr_editor_workbench_name(workbenches, workbench));
    uint32_t count = 0u;
    context_push(items, &count,
                 (EditorContextItem){"Switch to this workbench",
                                     VKR_UI_ICON_LAYOUT, NULL,
                                     workbench == workbenches->active,
                                     CONTEXT_WORKBENCH_SWITCH});
    context_push(items, &count,
                 (EditorContextItem){"Reset panel layout", VKR_UI_ICON_RESET,
                                     NULL, false_v, CONTEXT_WORKBENCH_RESET});
    context_separator(items, &count);
    context_push(
        items, &count,
        (EditorContextItem){"Duplicate", VKR_UI_ICON_DUPLICATE, NULL,
                            workbenches->count == VKR_EDITOR_WORKBENCH_MAX,
                            CONTEXT_WORKBENCH_DUPLICATE});
    context_push(items, &count,
                 (EditorContextItem){"Rename", VKR_UI_ICON_RENAME, NULL,
                                     false_v, CONTEXT_WORKBENCH_RENAME});
    context_push(items, &count,
                 (EditorContextItem){"Move left", VKR_UI_ICON_ARROW_LEFT, NULL,
                                     workbench == 0u, CONTEXT_WORKBENCH_LEFT});
    context_push(items, &count,
                 (EditorContextItem){"Move right", VKR_UI_ICON_ARROW_RIGHT,
                                     NULL, workbench + 1u >= workbenches->count,
                                     CONTEXT_WORKBENCH_RIGHT});
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Delete", VKR_UI_ICON_TRASH, NULL,
                                     !workbenches->items[workbench].custom,
                                     CONTEXT_WORKBENCH_DELETE});
    return count;
  }
  case VKR_EDITOR_CONTEXT_DOCK_TAB: {
    const VkrUiDockPanelKind panel = (VkrUiDockPanelKind)editor->context_panel;
    if (frame && !vkr_ui_dock_find_panel(frame->dock, panel, NULL, NULL))
      return 0u;
    const String8 name = vkr_ui_dock_panel_label(panel);
    snprintf(label, label_capacity, "Close %.*s", (int)name.length, name.str);
    uint32_t count = 0u;
    context_push(items, &count,
                 (EditorContextItem){label, VKR_UI_ICON_CLOSE, NULL,
                                     panel == VKR_UI_DOCK_PANEL_SCENE_VIEWPORT,
                                     CONTEXT_TAB_CLOSE});
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Reset panel layout", VKR_UI_ICON_LAYOUT,
                                     NULL, false_v, CONTEXT_TAB_LAYOUT});
    return count;
  }
  case VKR_EDITOR_CONTEXT_CONTENT: {
    static const VkrUiIcon icons[] = {VKR_UI_ICON_SCENE, VKR_UI_ICON_REVEAL,
                                      VKR_UI_ICON_ADD, VKR_UI_ICON_RENAME,
                                      VKR_UI_ICON_TRASH};
    /* A command without a label, such as Load for a non-scene, is left
       out. */
    uint32_t count = 0u;
    for (uint32_t i = 0; i < VKR_EDITOR_CONTENT_COMMAND_COUNT; ++i) {
      const char *command = vkr_editor_content_menu_label(
          editor->content, (VkrEditorContentCommand)i);
      if (!command) {
        continue;
      }
      if (i == VKR_EDITOR_CONTENT_COMMAND_RENAME) {
        context_separator(items, &count);
      }
      context_push(
          items, &count,
          (EditorContextItem){command, icons[i], NULL,
                              !vkr_editor_content_menu_available(
                                  editor->content, (VkrEditorContentCommand)i),
                              CONTEXT_CONTENT_COMMAND, i});
      /* A loaded object takes scripts and components here as in the
         Outliner. */
      if (i == VKR_EDITOR_CONTENT_COMMAND_PLACE && entity_alive) {
        context_separator(items, &count);
        if (vkr_entity_get_component(scene->world, editor->context_entity,
                                     scene->comp_transform)) {
          const VkrTypeDesc *script =
              vkr_editor_entity_script(scene, editor->context_entity);
          context_push(
              items, &count,
              (EditorContextItem){.label = "Script",
                                  .icon = VKR_UI_ICON_CODE,
                                  .shortcut = script ? script->label : NULL,
                                  .opens = true_v,
                                  .submenu = VKR_EDITOR_CONTEXT_SCRIPT});
        }
        context_push(
            items, &count,
            (EditorContextItem){.label = "Add component",
                                .icon = VKR_UI_ICON_PUZZLE,
                                .opens = true_v,
                                .submenu = VKR_EDITOR_CONTEXT_ADD_COMPONENT});
      }
    }
    return count;
  }
  case VKR_EDITOR_CONTEXT_CONTENT_FOLDER: {
    uint32_t count = 0u;
    static const struct {
      const char *label;
      VkrUiIcon icon;
      VkrEditorContentFolderCommand command;
    } commands[] = {
        {"New folder", VKR_UI_ICON_FOLDER,
         VKR_EDITOR_CONTENT_FOLDER_NEW_FOLDER},
        {"New script...", VKR_UI_ICON_CODE,
         VKR_EDITOR_CONTENT_FOLDER_NEW_SCRIPT},
        {"Import...", VKR_UI_ICON_IMPORT, VKR_EDITOR_CONTENT_FOLDER_IMPORT},
    };
    for (uint32_t i = 0; i < ArrayCount(commands); ++i) {
      context_push(
          items, &count,
          (EditorContextItem){commands[i].label, commands[i].icon, NULL,
                              !vkr_editor_content_folder_available(
                                  editor->content, commands[i].command),
                              CONTEXT_FOLDER_COMMAND, commands[i].command});
    }
    context_separator(items, &count);
    context_push(
        items, &count,
        (EditorContextItem){.label = "Create",
                            .icon = VKR_UI_ICON_PLUS_CIRCLE,
                            .disabled = editor->context_container == UINT16_MAX,
                            .opens = true_v,
                            .submenu = VKR_EDITOR_CONTEXT_CREATE});
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Refresh", VKR_UI_ICON_REFRESH, NULL,
                                     false_v, CONTEXT_FOLDER_COMMAND,
                                     VKR_EDITOR_CONTENT_FOLDER_REFRESH});
    return count;
  }
  case VKR_EDITOR_CONTEXT_TEXT: {
    /* Native text-field menu with platform shortcut hints. */
#if defined(PLATFORM_APPLE)
#define CONTEXT_TEXT_KEY(key) "\xe2\x8c\x98" key
#else
#define CONTEXT_TEXT_KEY(key) "Ctrl+" key
#endif
    const VkrUiTextFieldState *state = &editor->context_text_state;
    const bool8_t editable = !state->read_only;
    uint32_t count = 0u;
    context_push(
        items, &count,
        (EditorContextItem){"Undo", VKR_UI_ICON_UNDO, CONTEXT_TEXT_KEY("Z"),
                            !editable || !state->can_undo, CONTEXT_TEXT_COMMAND,
                            VKR_UI_TEXT_COMMAND_UNDO});
    context_push(items, &count,
                 (EditorContextItem){
                     "Redo", VKR_UI_ICON_REDO, CONTEXT_TEXT_KEY("Shift+Z"),
                     !editable || !state->can_redo, CONTEXT_TEXT_COMMAND,
                     VKR_UI_TEXT_COMMAND_REDO});
    context_separator(items, &count);
    context_push(
        items, &count,
        (EditorContextItem){"Cut", VKR_UI_ICON_COPY, CONTEXT_TEXT_KEY("X"),
                            !editable || !state->has_selection,
                            CONTEXT_TEXT_COMMAND, VKR_UI_TEXT_COMMAND_CUT});
    context_push(
        items, &count,
        (EditorContextItem){"Copy", VKR_UI_ICON_DUPLICATE,
                            CONTEXT_TEXT_KEY("C"), !state->has_selection,
                            CONTEXT_TEXT_COMMAND, VKR_UI_TEXT_COMMAND_COPY});
    context_push(items, &count,
                 (EditorContextItem){"Paste", VKR_UI_ICON_PASTE,
                                     CONTEXT_TEXT_KEY("V"), !editable,
                                     CONTEXT_TEXT_COMMAND,
                                     VKR_UI_TEXT_COMMAND_PASTE});
    context_push(items, &count,
                 (EditorContextItem){"Delete", VKR_UI_ICON_TRASH, NULL,
                                     !editable || !state->has_selection,
                                     CONTEXT_TEXT_COMMAND,
                                     VKR_UI_TEXT_COMMAND_DELETE});
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Select All", VKR_UI_ICON_SELECTION,
                                     CONTEXT_TEXT_KEY("A"), !state->has_text,
                                     CONTEXT_TEXT_COMMAND,
                                     VKR_UI_TEXT_COMMAND_SELECT_ALL});
#undef CONTEXT_TEXT_KEY
    return count;
  }
  case VKR_EDITOR_CONTEXT_CONSOLE: {
    uint32_t count = 0u;
    context_push(items, &count,
                 (EditorContextItem){"Copy selected", VKR_UI_ICON_COPY, NULL,
                                     false_v, CONTEXT_CONSOLE_COPY});
    context_separator(items, &count);
    context_push(items, &count,
                 (EditorContextItem){"Clear Console", VKR_UI_ICON_TRASH, NULL,
                                     false_v, CONTEXT_CONSOLE_CLEAR});
    return count;
  }
  }
  return 0u;
}

/* Where a menu of `height_pt` opens at `position_pt`, kept on screen. */
static VkrUiRect context_place(const VkrUiSystem *ui, Vec2 position_pt,
                               float32_t height_pt) {
  const float32_t scale = ui->content_scale;
  const float32_t width = EDITOR_CONTEXT_WIDTH_PT;
  const float32_t screen_w = (float32_t)ui->target_width / scale;
  const float32_t screen_h = (float32_t)ui->target_height / scale;
  const float32_t x = Max(0.0f, Min(position_pt.x, screen_w - width));
  const float32_t y = Max(0.0f, Min(position_pt.y, screen_h - height_pt));
  return (VkrUiRect){x * scale, y * scale, width * scale, height_pt * scale};
}

VkrUiRect vkr_editor_context_menu_rect(const VkrEditorUi *editor,
                                       const VkrUiSystem *ui) {
  if (!editor->context_open)
    return (VkrUiRect){0};
  const float32_t height = editor->context_height_pt > 0.0f
                               ? editor->context_height_pt
                               : EDITOR_MENU_PADDING_PT * 2.0f;
  return context_place(ui, editor->context_position_pt, height);
}

VkrUiRect vkr_editor_context_submenu_rect(const VkrEditorUi *editor) {
  return editor->context_open && editor->context_sub_open
             ? editor->context_sub_rect_px
             : (VkrUiRect){0};
}

static void editor_context_run(VkrEditorUi *editor,
                               const VkrSampleUiFrame *frame,
                               VkrEditorContextKind kind,
                               const EditorContextItem *item) {
  switch (item->action) {
  case CONTEXT_NONE:
    break;
  case CONTEXT_FRAME:
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_FRAME, .entity = editor->context_entity};
    break;
  case CONTEXT_SNAP: {
    char message[160];
    if (!vkr_editor_viewport_snap(editor, frame, editor->context_entity,
                                  message, sizeof(message))) {
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    }
    break;
  }
  case CONTEXT_VISIBILITY:
    vkr_editor_hide_eye(editor, frame, editor->context_entity);
    break;
  case CONTEXT_ISOLATE:
    vkr_editor_hide_isolate(editor, editor->context_entity);
    break;
  case CONTEXT_REVEAL:
    vkr_editor_hide_reveal(editor);
    break;
  case CONTEXT_RENAME:
    /* Details edits the name of the selection. */
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_SELECT, .entity = editor->context_entity};
    vkr_editor_scene_panels_request_rename(editor->scene_panels);
    break;
  case CONTEXT_COPY_NAME: {
    const VkrScene *scene =
        vkr_editor_entity_scene(frame, editor->context_entity);
    const String8 name = scene
                             ? vkr_scene_get_name(scene, editor->context_entity)
                             : (String8){0};
    if (name.length)
      (void)vkr_platform_clipboard_write_text(name.str, name.length);
    break;
  }
  case CONTEXT_DETACH:
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = VKR_SCENE_EDIT_REPARENT, .entity = editor->context_entity};
    break;
  case CONTEXT_DUPLICATE:
  case CONTEXT_DELETE: {
    const VkrSceneEditAction action = item->action == CONTEXT_DUPLICATE
                                          ? VKR_SCENE_EDIT_DUPLICATE
                                          : VKR_SCENE_EDIT_DELETE;
    /* On a selected object, the menu acts on the whole selection. */
    if (editor->selection_extra_count &&
        vkr_editor_selection_contains(editor, frame, editor->context_entity)) {
      char message[160];
      if (!vkr_editor_selection_apply(editor, frame, action, message,
                                      sizeof(message)))
        vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                         vkr_ui_theme()->warning, message);
      break;
    }
    *frame->scene_edit = (VkrSceneEditRequest){
        .action = action, .entity = editor->context_entity};
    break;
  }
  case CONTEXT_CREATE:
    /* Content shows what its own menu created. */
    if (vkr_editor_request_create(
            editor, frame, item->value, editor->context_container,
            editor->context_at_pixel ? &editor->context_pixel : NULL) &&
        kind == VKR_EDITOR_CONTEXT_CONTENT_FOLDER)
      vkr_editor_content_reveal_created(editor->content,
                                        frame->selected_entity);
    break;
  case CONTEXT_IMPORT_MODEL:
    vkr_editor_projects_add_entity(editor->projects, editor, frame);
    break;
  case CONTEXT_ADD_COMPONENT: {
    VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_ADD_COMPONENT,
                                   .entity = editor->context_entity};
    request.values.component_type = vkr_scene_world_type(item->value);
    vkr_type_defaults(request.values.component_type, request.values.component);
    *frame->scene_edit = request;
    break;
  }
  case CONTEXT_ADD_PHYSICS:
    (void)vkr_editor_request_physics_body(frame, editor->context_entity,
                                          true_v);
    break;
  case CONTEXT_PRESET_SAVE: {
    _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
    if (vkr_editor_component_read(frame, editor->context_entity,
                                  editor->context_type, value))
      (void)vkr_editor_content_save_preset(editor->content,
                                           editor->context_type, value);
    break;
  }
  case CONTEXT_PRESET_APPLY: {
    const VkrEditorPreset *preset =
        vkr_editor_content_preset(editor->content, item->value);
    if (preset && preset->type == editor->context_type)
      vkr_editor_request_component(frame, editor->context_entity, preset->type,
                                   preset->value);
    break;
  }
  case CONTEXT_TAB_CLOSE:
    vkr_editor_dock_toggle(frame->dock,
                           (VkrUiDockPanelKind)editor->context_panel);
    break;
  case CONTEXT_TAB_LAYOUT:
    vkr_editor_workbench_reset(editor, frame);
    break;
  case CONTEXT_WORKBENCH_SWITCH: {
    char message[96];
    if (!vkr_editor_workbench_request(editor, frame, editor->context_panel,
                                      message, sizeof(message)))
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    break;
  }
  case CONTEXT_WORKBENCH_RESET:
    vkr_editor_workbench_reset_index(editor, frame, editor->context_panel);
    break;
  case CONTEXT_WORKBENCH_RENAME:
    vkr_editor_workbench_rename_begin(editor, editor->context_panel);
    break;
  case CONTEXT_WORKBENCH_DUPLICATE:
  case CONTEXT_WORKBENCH_LEFT:
  case CONTEXT_WORKBENCH_RIGHT:
  case CONTEXT_WORKBENCH_DELETE: {
    char message[96];
    const uint32_t index = editor->context_panel;
    const bool8_t done =
        item->action == CONTEXT_WORKBENCH_DUPLICATE
            ? vkr_editor_workbench_duplicate(editor, frame, index, message,
                                             sizeof(message))
        : item->action == CONTEXT_WORKBENCH_DELETE
            ? vkr_editor_workbench_delete(editor, frame, index, message,
                                          sizeof(message))
            : vkr_editor_workbench_move(
                  editor, index,
                  item->action == CONTEXT_WORKBENCH_LEFT ? -1 : 1, message,
                  sizeof(message));
    if (!done)
      vkr_editor_toast(editor, VKR_UI_ICON_WARNING_FILL,
                       vkr_ui_theme()->warning, message);
    break;
  }
  case CONTEXT_CONSOLE_COPY:
    vkr_editor_console_copy_selection(&editor->console, frame->ui);
    break;
  case CONTEXT_TEXT_COMMAND:
    vkr_ui_text_field_command(frame->ui, editor->context_text_field,
                              (VkrUiTextCommand)item->value);
    break;
  case CONTEXT_CHOICE_SET:
    if (editor->context_details) {
      editor->context_details->choice_pick = item->value;
      editor->context_details->choice_picked = true_v;
    }
    break;
  case CONTEXT_CONSOLE_CLEAR:
    vkr_editor_console_clear(&editor->console);
    break;
  case CONTEXT_CONTENT_COMMAND:
    vkr_editor_content_menu_command(editor->content,
                                    (VkrEditorContentCommand)item->value);
    break;
  case CONTEXT_FOLDER_COMMAND:
    vkr_editor_content_folder_command(
        editor->content, (VkrEditorContentFolderCommand)item->value);
    break;
  case CONTEXT_SCRIPT_SET: {
    const VkrTypeDesc *types[EDITOR_CONTEXT_SCRIPT_MAX];
    const uint32_t count =
        vkr_editor_script_types(frame, types, ArrayCount(types));
    if (item->value == UINT32_MAX || item->value < count)
      vkr_editor_request_script(frame, editor->context_entity,
                                item->value == UINT32_MAX ? NULL
                                                          : types[item->value]);
    break;
  }
  case CONTEXT_SCRIPT_NEW:
    /* The new module's script attaches to the object once it loads. */
    editor->script_attach_entity = editor->context_entity;
    editor->script_attach_module[0] = '\0';
    vkr_editor_code_new_script(editor->code, editor);
    break;
  case CONTEXT_SCRIPT_EDIT:
    (void)vkr_editor_open_entity_script(editor, frame, editor->context_entity);
    break;
  }
}

/* Next selectable row from `from` in `direction`, wrapping; -1 when none. */
static int32_t context_step(const EditorContextItem *items, uint32_t count,
                            int32_t from, int32_t direction) {
  for (uint32_t tried = 0; tried < count; ++tried) {
    from = from < 0 ? (direction > 0 ? 0 : (int32_t)count - 1)
                    : (from + direction + (int32_t)count) % (int32_t)count;
    if (context_row_selectable(&items[from])) {
      return from;
    }
  }
  return -1;
}

typedef struct EditorContextPanel {
  int32_t clicked;
  int32_t hovered;
} EditorContextPanel;

/* One menu panel at `rect`. `lit` and `open_row` draw highlighted. */
static EditorContextPanel editor_context_panel(VkrUiSystem *ui, String8 id,
                                               const EditorContextItem *items,
                                               uint32_t count, VkrUiRect rect,
                                               int32_t lit, int32_t open_row) {
  EditorContextPanel result = {-1, -1};
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack rows[EDITOR_CONTEXT_ITEM_CAPACITY];
  for (uint32_t i = 0; i < count; ++i)
    rows[i] = (VkrUiTrack){.value = context_row_height(&items[i]),
                           .unit = VKR_UI_TRACK_PX};
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
  /* Rows stay legible over busy panels. */
  popup.style.background_color.w = Max(popup.style.background_color.w, 0.97f);
  popup.style.padding_pt =
      (VkrUiEdges){EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT,
                   EDITOR_MENU_PADDING_PT, EDITOR_MENU_PADDING_PT};
  popup.style.gap_pt = 0.0f;
  popup.style.min_size_pt = popup.style.max_size_pt =
      (Vec2){rect.width / ui->content_scale, rect.height / ui->content_scale};
  if (!vkr_ui_panel_begin(ui, id, &popup))
    return result;
  for (uint32_t i = 0; i < count; ++i) {
    const EditorContextItem *entry = &items[i];
    (void)vkr_ui_push_id_u64(ui, i);
    if (entry->row == CONTEXT_ROW_SEPARATOR) {
      VkrUiWidgetConfig line = vkr_ui_widget_config_default();
      line.placement.column = 0u;
      line.placement.row = i;
      line.placement.justify = VKR_UI_ALIGN_CENTER;
      line.placement.align = VKR_UI_ALIGN_CENTER;
      line.style.min_size_pt = line.style.max_size_pt =
          (Vec2){rect.width / ui->content_scale -
                     EDITOR_MENU_PADDING_PT * 2.0f - 12.0f,
                 1.0f};
      line.style.padding_pt = (VkrUiEdges){0};
      line.style.background_color = vkr_ui_color_alpha(theme->text, 0.14f);
      vkr_ui_label(ui, string8_lit("separator"), (String8){0}, &line);
      (void)vkr_ui_pop_id(ui);
      continue;
    }
    if (entry->row == CONTEXT_ROW_HEADER) {
      VkrUiWidgetConfig heading =
          vkr_editor_text_config(theme->font_caption, theme->text_secondary);
      heading.placement.column = 0u;
      heading.placement.row = i;
      heading.placement.justify = VKR_UI_ALIGN_START;
      heading.placement.align = VKR_UI_ALIGN_END;
      heading.placement.margin_pt = (VkrUiEdges){0, 0, 3, 9};
      vkr_ui_label(
          ui, string8_lit("heading"),
          string8_create((uint8_t *)entry->label, strlen(entry->label)),
          &heading);
      (void)vkr_ui_pop_id(ui);
      continue;
    }
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
    item.icon = entry->icon;
    item.icon_size_pt = 14.0f;
    item.icon_color = theme->text_secondary;
    item.disabled = entry->disabled;
    const VkrUiId item_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("item"));
    const bool8_t hot = ui->hot_id == item_id && !item.disabled;
    const bool8_t highlighted =
        !item.disabled && (hot || (int32_t)i == lit || (int32_t)i == open_row);
    VkrUiWidgetConfig hit = item;
    hit.icon = VKR_UI_ICON_NONE;
    if (highlighted)
      hit.style.background_color = theme->accent;
    if (vkr_ui_button(ui, string8_lit("item"), (String8){0}, &hit))
      result.clicked = (int32_t)i;
    if (hot)
      result.hovered = (int32_t)i;
    const Vec4 text_color = highlighted ? theme->text_on_accent : theme->text;
    VkrUiWidgetConfig text = item;
    text.placement.justify = VKR_UI_ALIGN_START;
    text.placement.align = VKR_UI_ALIGN_CENTER;
    text.style.background_color = (Vec4){0};
    text.style.text_color = text_color;
    if (highlighted)
      text.icon_color = theme->text_on_accent;
    vkr_ui_label(ui, string8_lit("label"),
                 string8_create((uint8_t *)entry->label, strlen(entry->label)),
                 &text);
    /* Trailing marks: a submenu chevron, a check, then the shortcut. */
    float32_t trailing = 10.0f;
    if (entry->opens || entry->checked) {
      VkrUiWidgetConfig mark = vkr_ui_widget_config_default();
      mark.placement.column = 0u;
      mark.placement.row = i;
      mark.placement.justify = VKR_UI_ALIGN_END;
      mark.placement.align = VKR_UI_ALIGN_CENTER;
      mark.placement.margin_pt.right = 8.0f;
      mark.style.background_color = (Vec4){0};
      mark.style.padding_pt = (VkrUiEdges){0};
      mark.icon = entry->opens ? VKR_UI_ICON_CHEVRON_RIGHT : VKR_UI_ICON_CHECK;
      mark.icon_size_pt = entry->opens ? 11.0f : 13.0f;
      mark.icon_color = highlighted    ? theme->text_on_accent
                        : entry->opens ? theme->text_secondary
                                       : theme->accent_hover;
      vkr_ui_label(ui, string8_lit("mark"), (String8){0}, &mark);
      trailing = 28.0f;
    }
    if (entry->shortcut) {
      VkrUiWidgetConfig shortcut = vkr_editor_text_config(
          theme->font_caption,
          highlighted ? theme->text_on_accent : theme->text_secondary);
      shortcut.placement.column = 0u;
      shortcut.placement.row = i;
      shortcut.placement.justify = VKR_UI_ALIGN_END;
      shortcut.placement.align = VKR_UI_ALIGN_CENTER;
      shortcut.placement.margin_pt.right = trailing;
      vkr_ui_label(
          ui, string8_lit("shortcut"),
          string8_create((uint8_t *)entry->shortcut, strlen(entry->shortcut)),
          &shortcut);
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  return result;
}

/* Top of row `row` of a menu at `rect`, in pixels. */
static float32_t context_row_y(const VkrUiSystem *ui, VkrUiRect rect,
                               const EditorContextItem *items, uint32_t row) {
  float32_t y = EDITOR_MENU_PADDING_PT;
  for (uint32_t i = 0; i < row; ++i) {
    y += context_row_height(&items[i]);
  }
  return rect.y + y * ui->content_scale;
}

/* The submenu beside row `row`: to the right, else to the left. */
static VkrUiRect context_sub_place(const VkrUiSystem *ui, VkrUiRect root,
                                   const EditorContextItem *items, uint32_t row,
                                   float32_t height_pt) {
  const float32_t scale = ui->content_scale;
  const float32_t width = EDITOR_CONTEXT_WIDTH_PT * scale;
  const float32_t height = height_pt * scale;
  float32_t x = root.x + root.width - 2.0f * scale;
  if (x + width > (float32_t)ui->target_width) {
    x = Max(0.0f, root.x - width + 2.0f * scale);
  }
  const float32_t y = Max(0.0f, Min(context_row_y(ui, root, items, row) -
                                        EDITOR_MENU_PADDING_PT * scale,
                                    (float32_t)ui->target_height - height));
  return (VkrUiRect){x, y, width, height};
}

void vkr_editor_context_menu_build(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  editor_script_attach_update(editor, frame);
  if (!editor->context_open)
    return;
  EditorContextItem items[EDITOR_CONTEXT_ITEM_CAPACITY];
  char label[64];
  uint32_t count = editor_context_items(editor, frame, editor->context_kind,
                                        items, label, sizeof(label));
  while (count && items[count - 1u].row == CONTEXT_ROW_SEPARATOR)
    --count;
  editor->context_count = count;
  editor->context_height_pt = context_height(items, count);
  const VkrUiRect rect = vkr_editor_context_menu_rect(editor, ui);
  EditorContextItem sub[EDITOR_CONTEXT_ITEM_CAPACITY];
  char sub_label[64];
  uint32_t sub_count = 0u;
  if (editor->context_sub_open && editor->context_sub_row < count &&
      items[editor->context_sub_row].opens &&
      !items[editor->context_sub_row].disabled) {
    sub_count = editor_context_items(editor, frame, editor->context_sub_kind,
                                     sub, sub_label, sizeof(sub_label));
    while (sub_count && sub[sub_count - 1u].row == CONTEXT_ROW_SEPARATOR)
      --sub_count;
  }
  if (!sub_count) {
    editor->context_sub_open = false_v;
    editor->context_sub_focused = false_v;
  }
  editor->context_sub_rect_px =
      sub_count ? context_sub_place(ui, rect, items, editor->context_sub_row,
                                    context_height(sub, sub_count))
                : (VkrUiRect){0};
  const VkrUiRect sub_rect = editor->context_sub_rect_px;
  /* The press that opened a menu is a right click; any later press outside,
   * the Cmd bar or a menu-bar menu closes it. */
  const bool8_t inside =
      editor_point_in_rect(ui->mouse_x, ui->mouse_y, rect) ||
      (sub_count && editor_point_in_rect(ui->mouse_x, ui->mouse_y, sub_rect));
  const bool8_t pressed_outside =
      (ui->mouse_pressed ||
       input_button_just_pressed(frame->input, BUTTON_RIGHT)) &&
      !inside && !input_button_just_pressed(frame->input, BUTTON_RIGHT);
  const bool8_t escape = input_key_just_pressed(frame->input, KEY_ESCAPE);
  if (!count || pressed_outside || editor->cmd_active ||
      editor->menu != VKR_EDITOR_MENU_NONE ||
      (escape && !editor->context_sub_focused)) {
    editor->context_open = false_v;
    (void)vkr_ui_keyboard_layer_set(ui, 0u);
    return;
  }
  /* The menu owns the keyboard while open: arrows move, Right and Left enter
   * and leave a submenu, Enter acts, Escape closes. */
  (void)vkr_ui_keyboard_layer_set(ui, EDITOR_MENU_LAYER);
  vkr_ui_keyboard_navigation_enabled(ui, false_v);
  const int32_t vertical =
      (int32_t)input_key_just_pressed(frame->input, KEY_DOWN) -
      (int32_t)input_key_just_pressed(frame->input, KEY_UP);
  const bool8_t right = input_key_just_pressed(frame->input, KEY_RIGHT);
  const bool8_t left = input_key_just_pressed(frame->input, KEY_LEFT);
  const bool8_t enter = input_key_just_pressed(frame->input, KEY_ENTER);
  const bool8_t pointer_moved = ui->mouse_x != editor->context_mouse_x ||
                                ui->mouse_y != editor->context_mouse_y;
  editor->context_mouse_x = ui->mouse_x;
  editor->context_mouse_y = ui->mouse_y;
  int32_t chosen = -1;
  int32_t sub_chosen = -1;
  if (editor->context_sub_focused) {
    if (vertical)
      editor->context_sub_cursor =
          context_step(sub, sub_count, editor->context_sub_cursor, vertical);
    if (left || escape) {
      editor->context_sub_focused = false_v;
      editor->context_sub_open = !left;
      editor->context_sub_cursor = -1;
    } else if (enter && editor->context_sub_cursor >= 0) {
      sub_chosen = editor->context_sub_cursor;
    }
  } else {
    if (vertical)
      editor->context_cursor =
          context_step(items, count, editor->context_cursor, vertical);
    const int32_t cursor = editor->context_cursor;
    const bool8_t on_submenu = cursor >= 0 && (uint32_t)cursor < count &&
                               items[cursor].opens && !items[cursor].disabled;
    if (on_submenu && (right || enter)) {
      editor->context_sub_open = true_v;
      editor->context_sub_focused = true_v;
      editor->context_sub_kind = items[cursor].submenu;
      editor->context_sub_row = (uint32_t)cursor;
      editor->context_sub_cursor = -1;
    } else if (enter && cursor >= 0 && context_row_selectable(&items[cursor])) {
      chosen = cursor;
    }
  }
  if (vertical || right || left || enter || escape)
    ui->capture.keyboard = true_v;

  (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER, rect);
  if (sub_count)
    (void)vkr_ui_input_layer_register(ui, EDITOR_MENU_LAYER, sub_rect);
  (void)vkr_ui_input_layer_set(ui, EDITOR_MENU_LAYER);
  const EditorContextPanel root = editor_context_panel(
      ui, string8_lit("editor.context.menu"), items, count, rect,
      editor->context_sub_focused ? -1 : editor->context_cursor,
      sub_count ? (int32_t)editor->context_sub_row : -1);
  EditorContextPanel child = {-1, -1};
  if (sub_count)
    child = editor_context_panel(
        ui, string8_lit("editor.context.submenu"), sub, sub_count, sub_rect,
        editor->context_sub_focused ? editor->context_sub_cursor : -1, -1);
  (void)vkr_ui_input_layer_set(ui, 0u);

  /* The pointer takes over from the keyboard once it moves over a row;
     resting on a submenu row opens it. */
  if (root.hovered >= 0 && pointer_moved) {
    editor->context_cursor = root.hovered;
    editor->context_sub_focused = false_v;
    const EditorContextItem *hovered = &items[root.hovered];
    if (hovered->opens) {
      if (!editor->context_sub_open ||
          editor->context_sub_row != (uint32_t)root.hovered) {
        editor->context_sub_open = true_v;
        editor->context_sub_kind = hovered->submenu;
        editor->context_sub_row = (uint32_t)root.hovered;
        editor->context_sub_cursor = -1;
      }
    } else {
      editor->context_sub_open = false_v;
    }
  }
  if (child.hovered >= 0 && pointer_moved) {
    editor->context_sub_focused = true_v;
    editor->context_sub_cursor = child.hovered;
  }
  if (root.clicked >= 0) {
    if (items[root.clicked].opens) {
      editor->context_sub_open = true_v;
      editor->context_sub_kind = items[root.clicked].submenu;
      editor->context_sub_row = (uint32_t)root.clicked;
    } else {
      chosen = root.clicked;
    }
  }
  if (child.clicked >= 0)
    sub_chosen = child.clicked;
  if (sub_chosen >= 0 && context_row_selectable(&sub[sub_chosen])) {
    editor->context_open = false_v;
    (void)vkr_ui_keyboard_layer_set(ui, 0u);
    editor_context_run(editor, frame, editor->context_kind, &sub[sub_chosen]);
    return;
  }
  if (chosen < 0 || !context_row_selectable(&items[chosen]))
    return;
  editor->context_open = false_v;
  (void)vkr_ui_keyboard_layer_set(ui, 0u);
  editor_context_run(editor, frame, editor->context_kind, &items[chosen]);
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
  /* A job's own warning outranks the generic completion notice. */
  const bool8_t warning_shown = editor->toast_seconds > 0.0 &&
                                editor->toast_icon == VKR_UI_ICON_LOG_WARNING;
  if (editor->toast_bakery_busy && !bakery_busy && !warning_shown)
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
