#include "editor_internal.h"

#include "core/logger.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cmd bar: a typed command line in the top bar, modeled on the Unreal
 * Editor's Cmd field. Typed lines and startup scripts share one interpreter
 * and one queue that runs a single command per frame, because the runtime
 * accepts one request of each kind per UI build. docs/editor-cmd.md lists the
 * vocabulary and explains how to add a command. */

#define CMD_FIELD_WIDTH_PT 300.0f
#define CMD_TAG_WIDTH_PT 40.0f
#define CMD_POPUP_WIDTH_PT 460.0f
#define CMD_ROW_PT 26.0f
#define CMD_POPUP_PADDING_PT 4.0f
#define CMD_SCENE_WAIT_LIMIT_SECONDS 120.0

#if defined(PLATFORM_APPLE)
#define CMD_SHORTCUT "\xe2\x8c\x98P"
#else
#define CMD_SHORTCUT "Ctrl+P"
#endif

typedef enum CmdArg {
  CMD_ARG_NONE,
  CMD_ARG_PANEL,
  CMD_ARG_WINDOW,
  CMD_ARG_CAMERA_VIEW,
  CMD_ARG_RENDER_MODE,
  CMD_ARG_TOOL,
  CMD_ARG_SWITCH,
  CMD_ARG_ZOOM,
  CMD_ARG_NUMBER,
  CMD_ARG_ENTITY,
  CMD_ARG_COMMAND,
  CMD_ARG_TEXT,
} CmdArg;

/* Enumerated argument words, in completion order and matching the value
 * tables below index for index. */
static const char *const cmd_panels[] = {"hierarchy", "inspector", "console",
                                         "bakery",    "content",   NULL};
static const VkrUiDockPanelKind cmd_panel_kinds[] = {
    VKR_UI_DOCK_PANEL_HIERARCHY, VKR_UI_DOCK_PANEL_INSPECTOR,
    VKR_UI_DOCK_PANEL_CONSOLE, VKR_UI_DOCK_PANEL_BAKERY,
    VKR_UI_DOCK_PANEL_CONTENT};

static const char *const cmd_windows[] = {
    "animation", "physics", "graphics", "draws", "memory", "help", NULL};
static const VkrEditorWindowKind cmd_window_kinds[] = {
    VKR_EDITOR_WINDOW_ANIMATION, VKR_EDITOR_WINDOW_PHYSICS,
    VKR_EDITOR_WINDOW_GRAPHICS,  VKR_EDITOR_WINDOW_DRAWS,
    VKR_EDITOR_WINDOW_MEMORY,    VKR_EDITOR_WINDOW_HELP};

/* Indexed by VkrSampleCameraView. */
const char *const vkr_editor_cmd_camera_views[] = {
    "perspective", "top", "left", "right", "bottom", NULL};

const char *const vkr_editor_cmd_render_modes[] = {
    "lit", "unlit", "detail-lighting", "lighting-only", "wireframe", NULL};
const VkrRenderMode vkr_editor_cmd_render_mode_values[] = {
    VKR_RENDER_MODE_DEFAULT, VKR_RENDER_MODE_UNLIT,
    VKR_RENDER_MODE_DETAIL_LIGHTING, VKR_RENDER_MODE_LIGHTING_ONLY,
    VKR_RENDER_MODE_WIREFRAME};

const char *const vkr_editor_cmd_tools[] = {"select", "move", "rotate", "scale",
                                            NULL};
const uint32_t vkr_editor_cmd_tool_modes[] = {
    VKR_GIZMO_MODE_NONE, VKR_GIZMO_MODE_TRANSLATE, VKR_GIZMO_MODE_ROTATE,
    VKR_GIZMO_MODE_SCALE};

static const char *const cmd_switches[] = {"on", "off", "toggle", NULL};
static const char *const cmd_zoom_words[] = {"in", "out", "reset", NULL};

static const char *const *cmd_arg_words(CmdArg arg) {
  switch (arg) {
  case CMD_ARG_PANEL:
    return cmd_panels;
  case CMD_ARG_WINDOW:
    return cmd_windows;
  case CMD_ARG_CAMERA_VIEW:
    return vkr_editor_cmd_camera_views;
  case CMD_ARG_RENDER_MODE:
    return vkr_editor_cmd_render_modes;
  case CMD_ARG_TOOL:
    return vkr_editor_cmd_tools;
  case CMD_ARG_SWITCH:
    return cmd_switches;
  case CMD_ARG_ZOOM:
    return cmd_zoom_words;
  default:
    return NULL;
  }
}

/* Optional arguments accept an empty tail; every other kind requires one. */
static bool8_t cmd_arg_required(CmdArg arg) {
  return arg != CMD_ARG_NONE && arg != CMD_ARG_SWITCH &&
         arg != CMD_ARG_COMMAND && arg != CMD_ARG_TEXT;
}

typedef struct CmdContext {
  VkrEditorUi *editor;
  const VkrSampleUiFrame *frame;
  char message[256];
} CmdContext;

typedef struct CmdDef CmdDef;
typedef bool8_t (*CmdRun)(CmdContext *ctx, const CmdDef *def, String8 arg);

struct CmdDef {
  const char *name;
  CmdArg arg;
  const char *usage;
  const char *help;
  CmdRun run;
  /* Shared editor command for cmd_run_command; CMD_COUNT otherwise. */
  EditorCommand command;
  /* Runner-specific selector, such as which light-icon switch to change. */
  uint32_t value;
};

/* ---- Text helpers (String8 is not null-terminated) ---- */

static bool8_t cmd_space(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint8_t cmd_lower(uint8_t c) {
  return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}

static String8 cmd_trim(String8 text) {
  while (text.length && cmd_space(text.str[0])) {
    ++text.str;
    --text.length;
  }
  while (text.length && cmd_space(text.str[text.length - 1u]))
    --text.length;
  return text;
}

static String8 cmd_cstr(const char *text) {
  return (String8){.str = (uint8_t *)text, .length = strlen(text)};
}

static bool8_t cmd_equals(String8 a, String8 b) {
  if (a.length != b.length)
    return false_v;
  for (uint64_t i = 0; i < a.length; ++i) {
    if (cmd_lower(a.str[i]) != cmd_lower(b.str[i]))
      return false_v;
  }
  return true_v;
}

static bool8_t cmd_starts_with(String8 text, String8 prefix) {
  return prefix.length <= text.length &&
         cmd_equals((String8){.str = text.str, .length = prefix.length},
                    prefix);
}

static bool8_t cmd_contains(String8 text, String8 needle) {
  if (!needle.length)
    return true_v;
  for (uint64_t i = 0; i + needle.length <= text.length; ++i) {
    if (cmd_equals((String8){.str = text.str + i, .length = needle.length},
                   needle))
      return true_v;
  }
  return false_v;
}

/* Splits the first word from the rest; the rest keeps interior spacing. */
static String8 cmd_split(String8 text, String8 *rest) {
  text = cmd_trim(text);
  uint64_t end = 0;
  while (end < text.length && !cmd_space(text.str[end]))
    ++end;
  if (rest)
    *rest =
        cmd_trim((String8){.str = text.str + end, .length = text.length - end});
  return (String8){.str = text.str, .length = end};
}

static String8 cmd_unquote(String8 text) {
  text = cmd_trim(text);
  if (text.length >= 2u && text.str[0] == '"' &&
      text.str[text.length - 1u] == '"')
    return (String8){.str = text.str + 1, .length = text.length - 2u};
  return text;
}

static int32_t cmd_word_index(const char *const *words, String8 word) {
  for (int32_t i = 0; words && words[i]; ++i) {
    if (cmd_equals(cmd_cstr(words[i]), word))
      return i;
  }
  return -1;
}

static bool8_t cmd_number(String8 word, float64_t *out) {
  char buffer[64];
  if (!word.length || word.length >= sizeof(buffer))
    return false_v;
  MemCopy(buffer, word.str, word.length);
  buffer[word.length] = '\0';
  char *end = NULL;
  const float64_t value = strtod(buffer, &end);
  if (end != buffer + word.length || !isfinite(value))
    return false_v;
  *out = value;
  return true_v;
}

/* on / off / toggle; an empty word toggles. */
static bool8_t cmd_switch(CmdContext *ctx, String8 word, bool8_t current,
                          bool8_t *out) {
  const int32_t index = word.length ? cmd_word_index(cmd_switches, word) : 2;
  if (index < 0) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Expected on, off or toggle, not '%.*s'", (int)word.length,
             word.str);
    return false_v;
  }
  *out = index == 0 ? true_v : index == 1 ? false_v : !current;
  return true_v;
}

/* Every result reaches three places: a `[cmd]` stdout line for scripts and
 * agents (flushed per line), the Console when the build keeps that log level,
 * and, for failures, a toast. Release builds without editor logging compile
 * info and warning logs out, so stdout is the dependable channel. */
static void cmd_report(bool8_t ok, const char *text) {
  fprintf(stdout, "[cmd] %s%s\n", ok ? "" : "error: ", text);
  fflush(stdout);
  if (ok)
    log_info("%s", text);
  else
    log_warn("%s", text);
}

/* ---- Runners ---- */

static bool8_t cmd_run_command(CmdContext *ctx, const CmdDef *def,
                               String8 arg) {
  (void)arg;
  if (!vkr_editor_command_enabled(def->command, ctx->editor, ctx->frame)) {
    snprintf(ctx->message, sizeof(ctx->message), "%s is unavailable right now",
             def->name);
    return false_v;
  }
  vkr_editor_command_execute(def->command, ctx->editor, ctx->frame);
  return true_v;
}

static bool8_t cmd_require_scene(CmdContext *ctx) {
  if (ctx->frame->scene)
    return true_v;
  snprintf(ctx->message, sizeof(ctx->message), "No scene is loaded");
  return false_v;
}

/* An exact name wins; otherwise the first entity whose name contains the
 * text, in entity order. */
static VkrEntityId cmd_find_entity(const VkrScene *scene, String8 name) {
  VkrEntityId partial = VKR_ENTITY_ID_INVALID;
  const uint32_t capacity = scene->world->dir.capacity;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (!scene->world->dir.records[i].chunk)
      continue;
    const VkrEntityId id = vkr_entity_id_from_index(scene->world, i);
    const String8 candidate = vkr_scene_get_name(scene, id);
    if (cmd_equals(candidate, name))
      return id;
    if (!partial.u64 && cmd_contains(candidate, name))
      partial = id;
  }
  return partial;
}

static bool8_t cmd_run_select(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  if (!cmd_require_scene(ctx))
    return false_v;
  const String8 name = cmd_unquote(arg);
  const VkrEntityId entity = cmd_find_entity(ctx->frame->scene, name);
  if (!entity.u64) {
    snprintf(ctx->message, sizeof(ctx->message), "No entity named '%.*s'",
             (int)name.length, name.str);
    return false_v;
  }
  *ctx->frame->scene_edit =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT, .entity = entity};
  const String8 selected = vkr_scene_get_name(ctx->frame->scene, entity);
  snprintf(ctx->message, sizeof(ctx->message), "Selected %.*s",
           (int)selected.length, selected.str);
  return true_v;
}

static bool8_t cmd_run_visibility(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  (void)def;
  (void)arg;
  if (!cmd_require_scene(ctx))
    return false_v;
  if (!vkr_scene_entity_alive(ctx->frame->scene, ctx->frame->selected_entity)) {
    snprintf(ctx->message, sizeof(ctx->message), "Nothing is selected");
    return false_v;
  }
  vkr_editor_toggle_visibility(ctx->frame, ctx->frame->selected_entity);
  return true_v;
}

static bool8_t cmd_run_panel(CmdContext *ctx, const CmdDef *def, String8 arg) {
  String8 state = {0};
  const String8 name = cmd_split(arg, &state);
  const int32_t index = cmd_word_index(
      def->arg == CMD_ARG_PANEL ? cmd_panels : cmd_windows, name);
  if (index < 0) {
    snprintf(ctx->message, sizeof(ctx->message), "Unknown %s '%.*s'",
             def->arg == CMD_ARG_PANEL ? "panel" : "window", (int)name.length,
             name.str);
    return false_v;
  }
  if (def->arg == CMD_ARG_PANEL) {
    const VkrUiDockPanelKind kind = cmd_panel_kinds[index];
    const bool8_t visible =
        vkr_ui_dock_find_panel(ctx->frame->dock, kind, NULL, NULL);
    bool8_t want = false_v;
    if (!cmd_switch(ctx, state, visible, &want))
      return false_v;
    if (want != visible)
      vkr_editor_dock_toggle(ctx->frame->dock, kind);
    return true_v;
  }
  const VkrEditorWindowKind kind = cmd_window_kinds[index];
  bool8_t want = false_v;
  if (!cmd_switch(ctx, state, ctx->editor->windows[kind].visible, &want))
    return false_v;
  vkr_editor_window_set_visible(ctx->editor, kind, want);
  return true_v;
}

/* Viewport commands copy the runtime's current view and submit one change. */
static bool8_t cmd_view_request(CmdContext *ctx, VkrSampleViewState next) {
  if (!ctx->frame->view_request) {
    snprintf(ctx->message, sizeof(ctx->message),
             "The Scene view is not available");
    return false_v;
  }
  *ctx->frame->view_request =
      (VkrSampleViewRequest){.value = next, .apply = true_v};
  return true_v;
}

static bool8_t cmd_run_view(CmdContext *ctx, const CmdDef *def, String8 arg) {
  VkrSampleViewState next = ctx->frame->view_state;
  const String8 word = cmd_split(arg, NULL);
  const char *const *words = cmd_arg_words(def->arg);
  float64_t number = 0.0;
  if (words) {
    const int32_t index = cmd_word_index(words, word);
    if (def->arg != CMD_ARG_SWITCH && index < 0) {
      snprintf(ctx->message, sizeof(ctx->message),
               "Unknown value '%.*s'; try Tab for choices", (int)word.length,
               word.str);
      return false_v;
    }
    if (def->arg == CMD_ARG_CAMERA_VIEW)
      next.camera_view = (VkrSampleCameraView)index;
    else if (def->arg == CMD_ARG_RENDER_MODE)
      next.render_mode = vkr_editor_cmd_render_mode_values[index];
    else if (def->arg == CMD_ARG_TOOL)
      next.gizmo_tool = vkr_editor_cmd_tool_modes[index];
    else if (!cmd_switch(ctx, word, next.grid_enabled, &next.grid_enabled))
      return false_v;
  } else {
    if (!cmd_number(word, &number) || number <= 0.0) {
      snprintf(ctx->message, sizeof(ctx->message),
               "Expected a positive number, not '%.*s'", (int)word.length,
               word.str);
      return false_v;
    }
    if (def->value == 0u) {
      next.camera_speed = vkr_clamp_f32((float32_t)number, 0.01f, 1000.0f);
    } else {
      next.grid_spacing = vkr_clamp_f32((float32_t)number, 0.001f, 10000.0f);
      next.grid_enabled = true_v;
    }
  }
  return cmd_view_request(ctx, next);
}

static bool8_t cmd_run_labels(CmdContext *ctx, const CmdDef *def, String8 arg) {
  VkrEditorUi *editor = ctx->editor;
  bool8_t *targets[] = {&editor->labels_enabled, &editor->labels_directional,
                        &editor->labels_spot, &editor->labels_point};
  bool8_t *target = targets[def->value];
  return cmd_switch(ctx, cmd_split(arg, NULL), *target, target);
}

static bool8_t cmd_run_zoom(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  VkrUiSystem *ui = ctx->frame->ui;
  const String8 word = cmd_split(arg, NULL);
  const int32_t index = cmd_word_index(cmd_zoom_words, word);
  float64_t scale = 0.0;
  if (index == 0)
    scale = ui->user_scale + 0.1f;
  else if (index == 1)
    scale = ui->user_scale - 0.1f;
  else if (index == 2)
    scale = 1.0;
  else if (!cmd_number(word, &scale)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Expected a scale, in, out or reset");
    return false_v;
  }
  vkr_ui_system_set_user_scale(ui, (float32_t)scale);
  snprintf(ctx->message, sizeof(ctx->message), "Interface zoom %.0f%%",
           (double)(ui->user_scale * 100.0f));
  return true_v;
}

static bool8_t cmd_run_motion(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  VkrUiSystem *ui = ctx->frame->ui;
  return cmd_switch(ctx, cmd_split(arg, NULL), ui->reduce_motion,
                    &ui->reduce_motion);
}

static bool8_t cmd_run_help(CmdContext *ctx, const CmdDef *def, String8 arg);

static bool8_t cmd_run_echo(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  snprintf(ctx->message, sizeof(ctx->message), "%.*s", (int)arg.length,
           arg.str);
  return true_v;
}

/* Scripts end the editor explicitly; unsaved scene edits need `discard`. */
static bool8_t cmd_run_quit(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  const VkrSampleUiFrame *frame = ctx->frame;
  const String8 word = cmd_split(arg, NULL);
  const bool8_t discard = cmd_equals(word, cmd_cstr("discard"));
  if (word.length && !discard) {
    snprintf(ctx->message, sizeof(ctx->message), "Usage: quit [discard]");
    return false_v;
  }
  if (!discard && frame->scene && frame->edits &&
      frame->edits->revision != frame->edits->saved_revision) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Unsaved scene edits; save them or use quit discard");
    return false_v;
  }
  if (!frame->quit_request) {
    snprintf(ctx->message, sizeof(ctx->message), "Quit is unavailable here");
    return false_v;
  }
  *frame->quit_request = true_v;
  snprintf(ctx->message, sizeof(ctx->message), "Quitting");
  return true_v;
}

static bool8_t cmd_run_wait(CmdContext *ctx, const CmdDef *def, String8 arg) {
  if (def->value) {
    ctx->editor->cmd_wait_scene_seconds = CMD_SCENE_WAIT_LIMIT_SECONDS;
    return true_v;
  }
  float64_t seconds = 0.0;
  if (!cmd_number(cmd_split(arg, NULL), &seconds) || seconds < 0.0) {
    snprintf(ctx->message, sizeof(ctx->message), "Expected seconds");
    return false_v;
  }
  ctx->editor->cmd_wait_seconds = Min(seconds, 600.0);
  return true_v;
}

/* ---- Vocabulary ---- */

#define CMD_SIMPLE(name, help, command)                                        \
  {name, CMD_ARG_NONE, "", help, cmd_run_command, command, 0u}

static const CmdDef cmd_defs[] = {
    CMD_SIMPLE("scene.load", "Load the scene", CMD_LOAD),
    CMD_SIMPLE("scene.reload", "Reload the scene from disk", CMD_RELOAD),
    CMD_SIMPLE("scene.unload", "Unload the scene", CMD_UNLOAD),
    CMD_SIMPLE("scene.save", "Save scene edits", CMD_SAVE),
    CMD_SIMPLE("undo", "Undo the last edit", CMD_UNDO),
    CMD_SIMPLE("redo", "Redo the last undone edit", CMD_REDO),
    {"select", CMD_ARG_ENTITY, "<name>",
     "Select an entity by name (exact, else first containing)", cmd_run_select,
     CMD_COUNT, 0u},
    CMD_SIMPLE("frame", "Frame the selection in the Scene", CMD_FRAME),
    {"visibility.toggle", CMD_ARG_NONE, "", "Hide or show the selection",
     cmd_run_visibility, CMD_COUNT, 0u},
    {"panel", CMD_ARG_PANEL, "<panel> [on|off|toggle]",
     "Show, hide or toggle a docked panel", cmd_run_panel, CMD_COUNT, 0u},
    {"window", CMD_ARG_WINDOW, "<window> [on|off|toggle]",
     "Show, hide or toggle a floating window", cmd_run_panel, CMD_COUNT, 0u},
    CMD_SIMPLE("layout.reset", "Restore the default panel layout",
               CMD_RESET_LAYOUT),
    CMD_SIMPLE("sim.play", "Start the simulation", CMD_SIM_START),
    CMD_SIMPLE("sim.pause", "Pause the simulation", CMD_SIM_PAUSE),
    CMD_SIMPLE("sim.step", "Advance a paused simulation one frame",
               CMD_SIM_STEP),
    CMD_SIMPLE("sim.stop", "Stop and reset the simulation", CMD_SIM_RESET),
    CMD_SIMPLE("render.start", "Resume Scene rendering", CMD_RENDER_START),
    CMD_SIMPLE("render.stop", "Freeze Scene rendering", CMD_RENDER_STOP),
    CMD_SIMPLE("camera.capture", "Toggle free-camera capture (F3)", CMD_CAMERA),
    {"camera.view", CMD_ARG_CAMERA_VIEW, "<view>",
     "Switch the Scene camera view", cmd_run_view, CMD_COUNT, 0u},
    {"camera.speed", CMD_ARG_NUMBER, "<units/s>",
     "Set free-camera flight speed", cmd_run_view, CMD_COUNT, 0u},
    {"view.mode", CMD_ARG_RENDER_MODE, "<mode>", "Switch the Scene render mode",
     cmd_run_view, CMD_COUNT, 0u},
    {"tool", CMD_ARG_TOOL, "<tool>", "Pick the transform tool", cmd_run_view,
     CMD_COUNT, 0u},
    {"grid", CMD_ARG_SWITCH, "[on|off|toggle]", "Show or hide the world grid",
     cmd_run_view, CMD_COUNT, 0u},
    {"grid.spacing", CMD_ARG_NUMBER, "<units>",
     "Set the grid cell size and show the grid", cmd_run_view, CMD_COUNT, 1u},
    {"labels", CMD_ARG_SWITCH, "[on|off|toggle]", "Show or hide light icons",
     cmd_run_labels, CMD_COUNT, 0u},
    {"labels.directional", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Directional light icons", cmd_run_labels, CMD_COUNT, 1u},
    {"labels.spot", CMD_ARG_SWITCH, "[on|off|toggle]", "Spot light icons",
     cmd_run_labels, CMD_COUNT, 2u},
    {"labels.point", CMD_ARG_SWITCH, "[on|off|toggle]", "Point light icons",
     cmd_run_labels, CMD_COUNT, 3u},
    {"ui.zoom", CMD_ARG_ZOOM, "<scale|in|out|reset>",
     "Scale the whole interface", cmd_run_zoom, CMD_COUNT, 0u},
    {"ui.reduce_motion", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Turn eased interface motion off or on", cmd_run_motion, CMD_COUNT, 0u},
    {"help", CMD_ARG_COMMAND, "[command]", "List commands or describe one",
     cmd_run_help, CMD_COUNT, 0u},
    {"echo", CMD_ARG_TEXT, "<text>", "Print text to the Console", cmd_run_echo,
     CMD_COUNT, 0u},
    {"wait", CMD_ARG_NUMBER, "<seconds>", "Pause the command queue",
     cmd_run_wait, CMD_COUNT, 0u},
    {"quit", CMD_ARG_TEXT, "[discard]",
     "Close the editor; discard drops unsaved scene edits", cmd_run_quit,
     CMD_COUNT, 0u},
    {"wait.scene", CMD_ARG_NONE, "",
     "Pause the command queue until a scene is loaded", cmd_run_wait, CMD_COUNT,
     1u},
};

static const CmdDef *cmd_find(String8 name) {
  for (uint32_t i = 0; i < ArrayCount(cmd_defs); ++i) {
    if (cmd_equals(cmd_cstr(cmd_defs[i].name), name))
      return &cmd_defs[i];
  }
  return NULL;
}

static bool8_t cmd_run_help(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  const String8 name = cmd_split(arg, NULL);
  if (name.length) {
    const CmdDef *found = cmd_find(name);
    if (!found) {
      snprintf(ctx->message, sizeof(ctx->message), "Unknown command '%.*s'",
               (int)name.length, name.str);
      return false_v;
    }
    snprintf(ctx->message, sizeof(ctx->message), "%s %s  -  %s", found->name,
             found->usage, found->help);
    return true_v;
  }
  for (uint32_t i = 0; i < ArrayCount(cmd_defs); ++i) {
    char line[256];
    snprintf(line, sizeof(line), "  %s %s  -  %s", cmd_defs[i].name,
             cmd_defs[i].usage, cmd_defs[i].help);
    cmd_report(true_v, line);
  }
  snprintf(ctx->message, sizeof(ctx->message),
           "%u commands; separate several with ';'",
           (uint32_t)ArrayCount(cmd_defs));
  return true_v;
}

/* ---- Execution and the script queue ---- */

static bool8_t cmd_execute_line(VkrEditorUi *editor,
                                const VkrSampleUiFrame *frame, String8 line) {
  line = cmd_trim(line);
  if (!line.length || line.str[0] == '#')
    return true_v;
  char echo[300];
  snprintf(echo, sizeof(echo), "> %.*s", (int)Min(line.length, 280u), line.str);
  cmd_report(true_v, echo);
  String8 arg = {0};
  const String8 name = cmd_split(line, &arg);
  const CmdDef *def = cmd_find(name);
  CmdContext ctx = {.editor = editor, .frame = frame};
  bool8_t ok = false_v;
  /* Anything that is not a command is an expression statement, and so is a
   * command name used as a property: `ui.zoom = 1.2`, or `ui.zoom` alone
   * when it reads a value; a bare `select` still reports its usage. */
  if (!def || (arg.length && arg.str[0] == '=')) {
    ok = vkr_editor_cmd_eval(editor, frame, line, ctx.message,
                             sizeof(ctx.message));
  } else if (cmd_arg_required(def->arg) && !arg.length) {
    ok = vkr_editor_cmd_eval(editor, frame, line, ctx.message,
                             sizeof(ctx.message));
    if (!ok)
      snprintf(ctx.message, sizeof(ctx.message), "Usage: %s %s", def->name,
               def->usage);
  } else {
    ok = def->run(&ctx, def, arg);
  }
  if (ctx.message[0]) {
    cmd_report(ok, ctx.message);
    vkr_editor_toast(
        editor, ok ? VKR_UI_ICON_TERMINAL : VKR_UI_ICON_WARNING_FILL,
        ok ? vkr_ui_theme()->accent_hover : vkr_ui_theme()->warning,
        ctx.message);
  }
  return ok;
}

bool8_t vkr_editor_cmd_enqueue(VkrEditorUi *editor, const char *script) {
  if (!editor || !script)
    return false_v;
  if (editor->cmd_queue_offset >= editor->cmd_queue_length)
    editor->cmd_queue_offset = editor->cmd_queue_length = 0u;
  const size_t length = strlen(script);
  if (editor->cmd_queue_length + length + 2u > sizeof(editor->cmd_queue)) {
    cmd_report(false_v, "Cmd queue is full; dropped a script");
    return false_v;
  }
  MemCopy(editor->cmd_queue + editor->cmd_queue_length, script, length);
  editor->cmd_queue_length += (uint32_t)length;
  editor->cmd_queue[editor->cmd_queue_length++] = '\n';
  editor->cmd_queue[editor->cmd_queue_length] = '\0';
  return true_v;
}

/* Returns the next `;`/newline-separated command; quotes may hold `;`. */
static String8 cmd_queue_next(VkrEditorUi *editor) {
  uint32_t start = editor->cmd_queue_offset;
  uint32_t end = start;
  bool8_t quoted = false_v;
  while (end < editor->cmd_queue_length) {
    const char c = editor->cmd_queue[end];
    if (c == '"')
      quoted = !quoted;
    else if (!quoted && (c == ';' || c == '\n'))
      break;
    ++end;
  }
  editor->cmd_queue_offset = Min(end + 1u, editor->cmd_queue_length);
  return (String8){.str = (uint8_t *)editor->cmd_queue + start,
                   .length = end - start};
}

void vkr_editor_cmd_update(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  const float64_t dt = frame->ui->delta_time;
  if (editor->cmd_wait_seconds > 0.0) {
    editor->cmd_wait_seconds -= dt;
    return;
  }
  if (editor->cmd_wait_scene_seconds > 0.0) {
    if (frame->scene && !frame->scene_loading) {
      editor->cmd_wait_scene_seconds = 0.0;
    } else {
      editor->cmd_wait_scene_seconds -= dt;
      if (editor->cmd_wait_scene_seconds <= 0.0) {
        cmd_report(false_v,
                   "wait.scene timed out; dropped the remaining commands");
        editor->cmd_queue_offset = editor->cmd_queue_length = 0u;
      }
      return;
    }
  }
  /* One non-empty command per frame keeps each runtime request slot single. */
  while (editor->cmd_queue_offset < editor->cmd_queue_length) {
    const String8 line = cmd_trim(cmd_queue_next(editor));
    if (!line.length)
      continue;
    (void)cmd_execute_line(editor, frame, line);
    break;
  }
}

/* ---- Autocomplete ---- */

static void cmd_add_suggestion(VkrEditorUi *editor, const char *text,
                               const char *hint) {
  if (editor->cmd_suggestion_count >= ArrayCount(editor->cmd_suggestions))
    return;
  /* Entities may share a name; `select` resolves the first either way. */
  for (uint32_t i = 0; i < editor->cmd_suggestion_count; ++i) {
    if (strcmp(editor->cmd_suggestions[i], text) == 0)
      return;
  }
  const uint32_t i = editor->cmd_suggestion_count++;
  snprintf(editor->cmd_suggestions[i], sizeof(editor->cmd_suggestions[i]), "%s",
           text);
  snprintf(editor->cmd_suggestion_hints[i],
           sizeof(editor->cmd_suggestion_hints[i]), "%s", hint);
}

static void cmd_suggest_commands(VkrEditorUi *editor, String8 typed) {
  /* Prefix matches first, then names or descriptions containing the text. */
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < ArrayCount(cmd_defs); ++i) {
      const CmdDef *def = &cmd_defs[i];
      const String8 name = cmd_cstr(def->name);
      const bool8_t prefix = cmd_starts_with(name, typed);
      if (pass == 0 ? !prefix
                    : prefix || !(cmd_contains(name, typed) ||
                                  cmd_contains(cmd_cstr(def->help), typed)))
        continue;
      char text[96];
      snprintf(text, sizeof(text), "%s%s", def->name,
               def->arg == CMD_ARG_NONE ? "" : " ");
      char hint[96];
      snprintf(hint, sizeof(hint), "%s%s%s", def->usage,
               def->usage[0] ? "  " : "", def->help);
      cmd_add_suggestion(editor, text, hint);
    }
  }
}

static void cmd_suggest_entities(VkrEditorUi *editor, const VkrScene *scene,
                                 String8 head, String8 typed) {
  const uint32_t capacity = scene->world->dir.capacity;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < capacity; ++i) {
      if (editor->cmd_suggestion_count >= ArrayCount(editor->cmd_suggestions))
        return;
      if (!scene->world->dir.records[i].chunk)
        continue;
      const VkrEntityId id = vkr_entity_id_from_index(scene->world, i);
      const String8 name = vkr_scene_get_name(scene, id);
      if (!name.length)
        continue;
      const bool8_t prefix = cmd_starts_with(name, typed);
      if (pass == 0 ? !prefix : prefix || !cmd_contains(name, typed))
        continue;
      bool8_t spaced = false_v;
      for (uint64_t c = 0; c < name.length; ++c)
        spaced |= cmd_space(name.str[c]);
      char text[96];
      snprintf(text, sizeof(text), "%.*s%s%.*s%s", (int)head.length, head.str,
               spaced ? "\"" : "", (int)Min(name.length, 60u), name.str,
               spaced ? "\"" : "");
      cmd_add_suggestion(editor, text, "entity");
    }
  }
}

/* Evaluator names, variables and members for an expression statement. */
static void cmd_suggest_expression(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   String8 typed) {
  char lines[ArrayCount(editor->cmd_suggestions)][96];
  char hints[ArrayCount(editor->cmd_suggestions)][96];
  const uint32_t count = vkr_editor_cmd_eval_complete(
      editor, frame, typed, lines, hints, ArrayCount(lines));
  for (uint32_t i = 0; i < count; ++i)
    cmd_add_suggestion(editor, lines[i], hints[i]);
}

static void cmd_suggest(VkrEditorUi *editor, const VkrSampleUiFrame *frame) {
  editor->cmd_suggestion_count = 0u;
  editor->cmd_selected = -1;
  String8 typed = {.str = editor->cmd_text, .length = editor->cmd_length};
  while (typed.length && cmd_space(typed.str[0])) {
    ++typed.str;
    --typed.length;
  }
  uint64_t space = 0;
  while (space < typed.length && !cmd_space(typed.str[space]))
    ++space;
  if (!typed.length) {
    for (uint32_t i = editor->cmd_history_count; i-- > 0u;)
      cmd_add_suggestion(editor, editor->cmd_history[i], "recent");
    cmd_suggest_commands(editor, typed);
    return;
  }
  if (space == typed.length) {
    cmd_suggest_commands(editor, typed);
    cmd_suggest_expression(editor, frame, typed);
    return;
  }
  const CmdDef *def = cmd_find((String8){.str = typed.str, .length = space});
  if (!def) {
    cmd_suggest_expression(editor, frame, typed);
    return;
  }
  /* The tail after the command; two-word arguments complete the last word. */
  String8 tail = {.str = typed.str + space + 1u,
                  .length = typed.length - space - 1u};
  CmdArg arg = def->arg;
  uint64_t word_start = 0;
  if (arg == CMD_ARG_PANEL || arg == CMD_ARG_WINDOW) {
    for (uint64_t c = 0; c < tail.length; ++c) {
      if (cmd_space(tail.str[c])) {
        word_start = c + 1u;
        arg = CMD_ARG_SWITCH;
      }
    }
  }
  const String8 head = {.str = typed.str, .length = space + 1u + word_start};
  const String8 word = {.str = tail.str + word_start,
                        .length = tail.length - word_start};
  if (arg == CMD_ARG_ENTITY) {
    if (frame->scene)
      cmd_suggest_entities(editor, frame->scene, head, cmd_unquote(word));
    return;
  }
  if (arg == CMD_ARG_COMMAND) {
    for (uint32_t i = 0; i < ArrayCount(cmd_defs); ++i) {
      if (!cmd_starts_with(cmd_cstr(cmd_defs[i].name), word))
        continue;
      char text[96];
      snprintf(text, sizeof(text), "%.*s%s", (int)head.length, head.str,
               cmd_defs[i].name);
      cmd_add_suggestion(editor, text, cmd_defs[i].help);
    }
    return;
  }
  const char *const *words = cmd_arg_words(arg);
  for (uint32_t i = 0; words && words[i]; ++i) {
    if (!cmd_starts_with(cmd_cstr(words[i]), word))
      continue;
    char text[96];
    snprintf(text, sizeof(text), "%.*s%s", (int)head.length, head.str,
             words[i]);
    cmd_add_suggestion(editor, text, def->usage);
  }
}

/* ---- Field and keys ---- */

static void cmd_set_text(VkrEditorUi *editor, VkrUiSystem *ui,
                         const char *text) {
  const size_t length = Min(strlen(text), sizeof(editor->cmd_text) - 1u);
  MemCopy(editor->cmd_text, text, length);
  editor->cmd_text[length] = 0u;
  editor->cmd_length = (uint32_t)length;
  editor->cmd_dirty = true_v;
  vkr_ui_text_field_set_cursor(ui, editor->cmd_field, (uint32_t)length);
}

static void cmd_submit(VkrEditorUi *editor, VkrUiSystem *ui) {
  if (!editor->cmd_length)
    return;
  char line[sizeof(editor->cmd_text)];
  MemCopy(line, editor->cmd_text, editor->cmd_length);
  line[editor->cmd_length] = '\0';
  /* History keeps the newest distinct lines, oldest first. */
  const uint32_t capacity = ArrayCount(editor->cmd_history);
  if (!editor->cmd_history_count ||
      strcmp(editor->cmd_history[editor->cmd_history_count - 1u], line) != 0) {
    if (editor->cmd_history_count == capacity) {
      MemCopy(editor->cmd_history[0], editor->cmd_history[1],
              sizeof(editor->cmd_history[0]) * (capacity - 1u));
      --editor->cmd_history_count;
    }
    snprintf(editor->cmd_history[editor->cmd_history_count++],
             sizeof(editor->cmd_history[0]), "%s", line);
  }
  editor->cmd_history_cursor = editor->cmd_history_count;
  (void)vkr_editor_cmd_enqueue(editor, line);
  cmd_set_text(editor, ui, "");
}

/* Accepting a suggestion that needs no further argument runs it. */
static void cmd_accept(VkrEditorUi *editor, VkrUiSystem *ui, uint32_t index,
                       bool8_t run) {
  cmd_set_text(editor, ui, editor->cmd_suggestions[index]);
  const size_t length = strlen(editor->cmd_suggestions[index]);
  if (run && length && editor->cmd_suggestions[index][length - 1u] != ' ')
    cmd_submit(editor, ui);
}

static void cmd_handle_keys(VkrEditorUi *editor,
                            const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  InputState *input = frame->input;
  const uint32_t count = editor->cmd_suggestion_count;
  const int32_t direction = (int32_t)input_key_just_pressed(input, KEY_DOWN) -
                            (int32_t)input_key_just_pressed(input, KEY_UP);
  if (direction && !editor->cmd_length && editor->cmd_history_count &&
      editor->cmd_selected < 0) {
    /* An empty line walks the history instead of the suggestion list. */
    const int32_t cursor = (int32_t)editor->cmd_history_cursor + direction;
    if (cursor >= 0 && cursor < (int32_t)editor->cmd_history_count) {
      editor->cmd_history_cursor = (uint32_t)cursor;
      cmd_set_text(editor, ui, editor->cmd_history[cursor]);
    }
  } else if (direction && count) {
    const int32_t rows = (int32_t)count;
    if (editor->cmd_selected < 0)
      editor->cmd_selected = direction > 0 ? 0 : rows - 1;
    else
      editor->cmd_selected = (editor->cmd_selected + direction + rows) % rows;
  }
  if (input_key_just_pressed(input, KEY_TAB) && count)
    cmd_accept(editor, ui,
               editor->cmd_selected >= 0 ? (uint32_t)editor->cmd_selected : 0u,
               false_v);
  if (input_key_just_pressed(input, KEY_ENTER)) {
    if (editor->cmd_selected >= 0 && (uint32_t)editor->cmd_selected < count)
      cmd_accept(editor, ui, (uint32_t)editor->cmd_selected, true_v);
    else
      cmd_submit(editor, ui);
  }
  if (input_key_just_pressed(input, KEY_ESCAPE)) {
    if (editor->cmd_length) {
      cmd_set_text(editor, ui, "");
    } else {
      ui->focused_id = VKR_UI_ID_NONE;
      ui->focused_is_text = false_v;
    }
  }
  ui->capture.keyboard = true_v;
}

void vkr_editor_cmd_bar_build(VkrEditorUi *editor,
                              const VkrSampleUiFrame *frame, uint32_t column) {
  VkrUiSystem *ui = frame->ui;
  const VkrUiTheme *theme = vkr_ui_theme();
  if (!vkr_ui_push_id_label(ui, string8_lit("cmd")))
    return;
  editor->cmd_field =
      vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("field"));
  if (editor->cmd_focus_request) {
    editor->cmd_focus_request = false_v;
    ui->focused_id = editor->cmd_field;
    ui->focused_is_text = true_v;
    (void)vkr_ui_keyboard_layer_set(ui, 0u);
    editor->cmd_dirty = true_v;
  }
  if (ui->focused_id == editor->cmd_field)
    cmd_handle_keys(editor, frame);

  const VkrUiPlacement placement = {
      .column = column,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_END,
      .align = VKR_UI_ALIGN_CENTER,
      .margin_pt = {0.0f, 2.0f, 0.0f, 6.0f},
  };
  VkrUiWidgetConfig field = vkr_ui_widget_config_default();
  field.placement = placement;
  field.style.font_size_pt = theme->font_body;
  field.style.min_size_pt = field.style.max_size_pt =
      (Vec2){CMD_FIELD_WIDTH_PT, theme->control_height};
  vkr_editor_field_style(&field);
  field.style.padding_pt = (VkrUiEdges){4.0f, 10.0f, 4.0f, CMD_TAG_WIDTH_PT};
  field.text.font = editor->mono_font;
  /* The suggestion list replaces the tooltip while typing. */
  if (ui->focused_id != editor->cmd_field)
    field.tooltip = string8_lit("Type a command: Tab completes, Enter runs, "
                                "Up/Down choose (" CMD_SHORTCUT ")");
  VkrUiTextEditBuffer buffer = {.data = editor->cmd_text,
                                .length = editor->cmd_length,
                                .capacity = sizeof(editor->cmd_text)};
  if (vkr_ui_text_field(ui, string8_lit("field"), &buffer, &field)) {
    editor->cmd_length = buffer.length;
    editor->cmd_dirty = true_v;
    editor->cmd_history_cursor = editor->cmd_history_count;
  }
  const bool8_t focused = ui->focused_id == editor->cmd_field;
  if (focused && !editor->cmd_active)
    editor->cmd_dirty = true_v;
  editor->cmd_active = focused;

  /* "Cmd" tag inside the field's left padding, like Unreal's Cmd box. */
  VkrUiWidgetConfig tag =
      vkr_editor_text_config(theme->font_caption, theme->text_secondary);
  tag.placement = placement;
  tag.placement.justify = VKR_UI_ALIGN_END;
  tag.placement.margin_pt.right =
      placement.margin_pt.right + CMD_FIELD_WIDTH_PT - CMD_TAG_WIDTH_PT;
  tag.style.min_size_pt = tag.style.max_size_pt =
      (Vec2){CMD_TAG_WIDTH_PT, theme->control_height};
  tag.style.padding_pt = (VkrUiEdges){0.0f, 0.0f, 0.0f, 8.0f};
  tag.text.font = editor->heading_font;
  tag.style.text_color = focused ? theme->accent_hover : theme->text_secondary;
  vkr_ui_label(ui, string8_lit("tag"), string8_lit("Cmd"), &tag);
  if (!editor->cmd_length && !focused) {
    VkrUiWidgetConfig hint =
        vkr_editor_text_config(theme->font_body, theme->text_disabled);
    hint.placement = placement;
    hint.style.min_size_pt = hint.style.max_size_pt =
        (Vec2){CMD_FIELD_WIDTH_PT - CMD_TAG_WIDTH_PT, theme->control_height};
    hint.style.padding_pt = (VkrUiEdges){0.0f, 10.0f, 0.0f, 0.0f};
    vkr_ui_label(ui, string8_lit("hint"),
                 string8_lit("Enter a command  " CMD_SHORTCUT), &hint);
  }
  (void)vkr_ui_pop_id(ui);
}

void vkr_editor_cmd_suggestions_build(VkrEditorUi *editor,
                                      const VkrSampleUiFrame *frame) {
  VkrUiSystem *ui = frame->ui;
  editor->cmd_popup_px = (VkrUiRect){0};
  if (!editor->cmd_active)
    return;
  if (editor->cmd_dirty) {
    cmd_suggest(editor, frame);
    editor->cmd_dirty = false_v;
  }
  VkrUiRect anchor = {0};
  if (!editor->cmd_suggestion_count ||
      !vkr_ui_widget_rect(ui, editor->cmd_field, &anchor))
    return;
  const VkrUiTheme *theme = vkr_ui_theme();
  const float32_t scale = ui->content_scale;
  const float32_t width = Max(anchor.width, CMD_POPUP_WIDTH_PT * scale);
  const float32_t height =
      ((float32_t)editor->cmd_suggestion_count * CMD_ROW_PT +
       CMD_POPUP_PADDING_PT * 2.0f) *
      scale;
  editor->cmd_popup_px =
      (VkrUiRect){Max(4.0f * scale, anchor.x + anchor.width - width),
                  anchor.y + anchor.height + 4.0f * scale, width, height};
  (void)vkr_ui_input_layer_set(ui, VKR_EDITOR_CMD_LAYER);

  VkrUiTrack rows[ArrayCount(editor->cmd_suggestions)];
  for (uint32_t i = 0; i < editor->cmd_suggestion_count; ++i)
    rows[i] = (VkrUiTrack){.value = CMD_ROW_PT, .unit = VKR_UI_TRACK_PX};
  const VkrUiTrack one = {.value = 1.0f, .unit = VKR_UI_TRACK_FR};
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {editor->cmd_popup_px.y / scale, 0.0f, 0.0f,
                    editor->cmd_popup_px.x / scale},
  };
  popup.columns = &one;
  popup.column_count = 1u;
  popup.rows = rows;
  popup.row_count = editor->cmd_suggestion_count;
  popup.style = vkr_editor_glass_style();
  popup.style.background_color.w = 1.0f;
  popup.style.padding_pt =
      (VkrUiEdges){CMD_POPUP_PADDING_PT, CMD_POPUP_PADDING_PT,
                   CMD_POPUP_PADDING_PT, CMD_POPUP_PADDING_PT};
  popup.style.gap_pt = 0.0f;
  popup.style.min_size_pt = popup.style.max_size_pt =
      (Vec2){width / scale, height / scale};
  popup.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("editor.cmd.suggestions"), &popup)) {
    (void)vkr_ui_input_layer_set(ui, 0u);
    return;
  }
  int32_t clicked = -1;
  for (uint32_t i = 0; i < editor->cmd_suggestion_count; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    const bool8_t selected = editor->cmd_selected == (int32_t)i;
    VkrUiWidgetConfig row = vkr_ui_widget_config_default();
    row.placement.column = 0u;
    row.placement.row = i;
    row.fill = true_v;
    vkr_editor_ghost_style(&row);
    row.style.hover_background_color = theme->row_hover;
    if (selected)
      row.style.background_color = theme->accent;
    const VkrUiId row_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("row"));
    if (vkr_ui_button(ui, string8_lit("row"), (String8){0}, &row))
      clicked = (int32_t)i;
    const bool8_t lit = selected || ui->hot_id == row_id;
    VkrUiWidgetConfig name = vkr_editor_text_config(
        theme->font_body, selected ? theme->text_on_accent : theme->text);
    name.placement.column = 0u;
    name.placement.row = i;
    name.placement.justify = VKR_UI_ALIGN_START;
    name.placement.align = VKR_UI_ALIGN_CENTER;
    name.style.padding_pt = (VkrUiEdges){0.0f, 8.0f, 0.0f, 10.0f};
    name.text.font = editor->mono_font;
    vkr_ui_label(ui, string8_lit("name"), cmd_cstr(editor->cmd_suggestions[i]),
                 &name);
    VkrUiWidgetConfig hint = vkr_editor_text_config(
        theme->font_caption, selected ? theme->text_on_accent
                             : lit    ? theme->text
                                      : theme->text_secondary);
    hint.placement.column = 0u;
    hint.placement.row = i;
    hint.placement.justify = VKR_UI_ALIGN_END;
    hint.placement.align = VKR_UI_ALIGN_CENTER;
    hint.placement.margin_pt.right = 10.0f;
    vkr_ui_label(ui, string8_lit("hint"),
                 cmd_cstr(editor->cmd_suggestion_hints[i]), &hint);
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
  (void)vkr_ui_input_layer_set(ui, 0u);
  if (clicked >= 0) {
    cmd_accept(editor, ui, (uint32_t)clicked, true_v);
    editor->cmd_focus_request = true_v;
  }
}
