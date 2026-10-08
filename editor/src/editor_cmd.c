#include "editor_agent.h"
#include "editor_internal.h"
#include "editor_partition.h"

#include "core/logger.h"
#include "editor_project_store.h"
#include "editor_projects.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cmd bar: a typed command line in the top bar, modeled on the Unreal
 * Editor's Cmd field. Typed lines and startup scripts share one interpreter
 * and one queue that runs a single command per frame, because the runtime
 * accepts one request of each kind per UI build. ADR-075 lists the
 * vocabulary. */

#define CMD_FIELD_WIDTH_PT 300.0f
#define CMD_TAG_WIDTH_PT 40.0f
#define CMD_POPUP_WIDTH_PT 460.0f
#define CMD_ROW_PT 26.0f
#define CMD_POPUP_PADDING_PT 4.0f
#define CMD_SCENE_WAIT_LIMIT_SECONDS 120.0
#define CMD_HOLD_LIMIT_SECONDS 600.0

#if defined(PLATFORM_APPLE)
#define CMD_SHORTCUT "\xe2\x8c\x98K"
#else
#define CMD_SHORTCUT "Ctrl+K"
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
  /* Object kinds and live world component types (ADR-076). */
  CMD_ARG_OBJECT,
  CMD_ARG_COMPONENT,
} CmdArg;

/* Enumerated argument words, in completion order and matching the value
 * tables below index for index. */
static const char *const cmd_panels[] = {
    "outliner", "details",      "console", "bakery",  "content", "build",
    "tools",    "level_checks", "script",  "terrain", "material", NULL};
static const VkrUiDockPanelKind cmd_panel_kinds[] = {
    VKR_UI_DOCK_PANEL_HIERARCHY, VKR_UI_DOCK_PANEL_INSPECTOR,
    VKR_UI_DOCK_PANEL_CONSOLE,   VKR_UI_DOCK_PANEL_BAKERY,
    VKR_UI_DOCK_PANEL_CONTENT,   VKR_UI_DOCK_PANEL_BUILD,
    VKR_UI_DOCK_PANEL_TOOLS,     VKR_UI_DOCK_PANEL_LEVEL_CHECKS,
    VKR_UI_DOCK_PANEL_SCRIPT,    VKR_UI_DOCK_PANEL_TERRAIN,
    VKR_UI_DOCK_PANEL_MATERIAL};

static const char *const cmd_windows[] = {
    "animation", "physics", "preferences", "draws",  "memory",
    "help",      "create",  "build",       "script", "changes",
    "level",     "terrain", "partition",   NULL};
static const VkrEditorWindowKind cmd_window_kinds[] = {
    VKR_EDITOR_WINDOW_ANIMATION, VKR_EDITOR_WINDOW_PHYSICS,
    VKR_EDITOR_WINDOW_GRAPHICS,  VKR_EDITOR_WINDOW_DRAWS,
    VKR_EDITOR_WINDOW_MEMORY,    VKR_EDITOR_WINDOW_HELP,
    VKR_EDITOR_WINDOW_CREATE,    VKR_EDITOR_WINDOW_BUILD,
    VKR_EDITOR_WINDOW_SCRIPT,    VKR_EDITOR_WINDOW_CHANGES,
    VKR_EDITOR_WINDOW_LEVEL,     VKR_EDITOR_WINDOW_TERRAIN,
    VKR_EDITOR_WINDOW_PARTITION};

const char *vkr_editor_cmd_window_name(VkrEditorWindowKind kind) {
  for (uint32_t i = 0; i < ArrayCount(cmd_window_kinds); ++i) {
    if (cmd_window_kinds[i] == kind)
      return cmd_windows[i];
  }
  return "";
}

/* Indexed by VkrSampleCameraView. */
const char *const vkr_editor_cmd_camera_views[] = {
    "perspective", "top", "left", "right", "bottom", "front", "back", NULL};

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

/* Filled from the object kinds and world types, NULL-terminated; refilled
 * when a script module registers more (ADR-079). */
static const char *cmd_object_words[48];
static const char *cmd_component_words[VKR_SCENE_TYPE_MAX + 1u];
static uint32_t cmd_object_word_kinds;

static void cmd_structure_words(void) {
  const uint32_t kinds = vkr_editor_object_kind_count();
  if (cmd_object_words[0] && cmd_object_word_kinds == kinds) {
    return;
  }
  cmd_object_word_kinds = kinds;
  MemZero(cmd_object_words, sizeof(cmd_object_words));
  MemZero(cmd_component_words, sizeof(cmd_component_words));
  for (uint32_t i = 0; i < kinds && i + 1u < ArrayCount(cmd_object_words);
       ++i) {
    cmd_object_words[i] = vkr_editor_object_kind_word(i);
  }
  uint32_t count = 0u;
  const VkrTypeDesc *type = NULL;
  for (uint32_t i = 0; (type = vkr_scene_world_type(i)) &&
                       count + 1u < ArrayCount(cmd_component_words);
       ++i) {
    if (vkr_scene_world_type_live(type)) {
      cmd_component_words[count++] = type->name;
    }
  }
}

static const char *const *cmd_arg_words(CmdArg arg) {
  cmd_structure_words();
  switch (arg) {
  case CMD_ARG_OBJECT:
    return cmd_object_words;
  case CMD_ARG_COMPONENT:
    return cmd_component_words;
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
  /* It starts a Bakery job or a scene load, so the queue holds until that
   * work settles and the next statement sees its result. */
  bool8_t holds;
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
static void cmd_report(VkrEditorUi *editor, bool8_t ok, const char *text) {
  fprintf(stdout, "[cmd] %s%s\n", ok ? "" : "error: ", text);
  fflush(stdout);
  /* The agent's `cmd` operation collects the lines its statement prints. */
  if (editor->cmd_capture && editor->cmd_capture_capacity) {
    const int written =
        snprintf(editor->cmd_capture + editor->cmd_capture_length,
                 editor->cmd_capture_capacity - editor->cmd_capture_length,
                 "%s%s\n", ok ? "" : "error: ", text);
    if (written > 0) {
      editor->cmd_capture_length =
          Min(editor->cmd_capture_length + (uint32_t)written,
              editor->cmd_capture_capacity - 1u);
    }
  }
  if (ok)
    log_info("%s", text);
  else
    log_warn("%s", text);
}

/* ---- Runners ---- */

static bool8_t cmd_run_op(CmdContext *ctx, const CmdDef *def, String8 arg);

static bool8_t cmd_run_brush_draw(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  /* value 0 switches drawing, 1 the clip tool, 2 terrain sculpting, 3 the
     stairs tool, 4 the corridor tool and 5 the measure tool. One tool holds
     the Scene mouse at a time. */
  static const VkrEditorSceneTool tools[] = {
      VKR_EDITOR_SCENE_TOOL_BRUSH_DRAW, VKR_EDITOR_SCENE_TOOL_CLIP,
      VKR_EDITOR_SCENE_TOOL_TERRAIN,    VKR_EDITOR_SCENE_TOOL_STAIRS,
      VKR_EDITOR_SCENE_TOOL_CORRIDOR,   VKR_EDITOR_SCENE_TOOL_MEASURE};
  static const char *const names[] = {"Brush drawing",     "Brush clipping",
                                      "Terrain sculpting", "Stairs tool",
                                      "Corridor tool",     "Measure tool"};
  const VkrEditorSceneTool tool = tools[def->value];
  bool8_t next = false_v;
  if (!cmd_switch(ctx, cmd_split(arg, NULL),
                  vkr_editor_scene_tool(ctx->editor) == tool, &next)) {
    return false_v;
  }
  vkr_editor_scene_tool_set(ctx->editor,
                            next ? tool : VKR_EDITOR_SCENE_TOOL_NONE);
  snprintf(ctx->message, sizeof(ctx->message), "%s %s", names[def->value],
           next ? "on" : "off");
  return true_v;
}

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

/* `build.game [profile]` and `build.run [profile]`: select a profile by
   name, then build; the queue holds until the package job settles. */
static bool8_t cmd_run_build(CmdContext *ctx, const CmdDef *def, String8 arg) {
  String8 name = cmd_trim(arg);
  /* A profile name with spaces is quoted, as in "Mac Shipping". */
  if (name.length >= 2u && name.str[0] == '"' &&
      name.str[name.length - 1u] == '"') {
    name = (String8){.str = name.str + 1, .length = name.length - 2u};
  }
  if (name.length) {
    char text[64];
    snprintf(text, sizeof(text), "%.*s", (int)Min(name.length, 63u),
             (const char *)name.str);
    if (!vkr_editor_build_select_profile(ctx->editor->build, text)) {
      snprintf(ctx->message, sizeof(ctx->message), "No build profile '%s'",
               text);
      return false_v;
    }
  }
  if (!vkr_editor_build_start(ctx->editor->build, ctx->editor, def->value != 0u,
                              ctx->message, sizeof(ctx->message))) {
    return false_v;
  }
  ctx->editor->cmd_holding_build = true_v;
  return true_v;
}

static bool8_t cmd_require_scene(CmdContext *ctx) {
  if (ctx->frame->scene || ctx->frame->world)
    return true_v;
  snprintf(ctx->message, sizeof(ctx->message), "No scene is loaded");
  return false_v;
}

/* An exact name wins; otherwise the first entity whose name contains the
 * text, in entity order. */
static VkrEntityId cmd_find_entity(const VkrScene *scene, String8 name) {
  VkrEntityId partial = VKR_ENTITY_ID_INVALID;
  if (!scene)
    return partial;
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

/* The scene's entities first, then added scenes', then the root World's. */
static VkrEntityId cmd_find_any(const VkrSampleUiFrame *frame, String8 name) {
  VkrEntityId entity = cmd_find_entity(frame->scene, name);
  for (uint32_t i = 0; !entity.u64 && i < VKR_SCENE_ADDITIVE_MAX; ++i)
    entity = cmd_find_entity(frame->additive[i], name);
  if (!entity.u64)
    entity = cmd_find_entity(frame->world, name);
  return entity;
}

static bool8_t cmd_run_select(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  if (!cmd_require_scene(ctx))
    return false_v;
  const String8 name = cmd_unquote(arg);
  const VkrEntityId entity = cmd_find_any(ctx->frame, name);
  if (!entity.u64) {
    snprintf(ctx->message, sizeof(ctx->message), "No entity named '%.*s'",
             (int)name.length, name.str);
    return false_v;
  }
  *ctx->frame->scene_edit =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_SELECT, .entity = entity};
  const String8 selected =
      vkr_scene_get_name(vkr_editor_entity_scene(ctx->frame, entity), entity);
  snprintf(ctx->message, sizeof(ctx->message), "Selected %.*s",
           (int)selected.length, selected.str);
  return true_v;
}

/* io.trace [on|off|toggle]: the `[io]` line of each delivery (ADR-084). */
static bool8_t cmd_run_io_trace(CmdContext *ctx, const CmdDef *def,
                                String8 arg) {
  (void)def;
  bool8_t next = false_v;
  const bool8_t current = ctx->frame->scripts && ctx->frame->scripts->io.trace;
  if (!ctx->frame->io_request ||
      !cmd_switch(ctx, cmd_split(arg, NULL), current, &next)) {
    return false_v;
  }
  ctx->frame->io_request->set_trace = true_v;
  ctx->frame->io_request->trace = next;
  snprintf(ctx->message, sizeof(ctx->message), "IO trace %s",
           next ? "on" : "off");
  return true_v;
}

/* io.fire <entity> <input> [value]: sends an input during Play; a name with
   spaces is quoted. */
static bool8_t cmd_run_io_fire(CmdContext *ctx, const CmdDef *def,
                               String8 arg) {
  (void)def;
  if (!cmd_require_scene(ctx))
    return false_v;
  arg = cmd_trim(arg);
  String8 name = {0};
  String8 rest = {0};
  if (arg.length && arg.str[0] == '"') {
    uint64_t end = 1u;
    while (end < arg.length && arg.str[end] != '"')
      ++end;
    name = (String8){.str = arg.str + 1, .length = end - 1u};
    rest =
        cmd_trim((String8){.str = arg.str + Min(end + 1u, arg.length),
                           .length = arg.length - Min(end + 1u, arg.length)});
  } else {
    name = cmd_split(arg, &rest);
  }
  String8 value = {0};
  const String8 input = cmd_split(rest, &value);
  const VkrEntityId entity = cmd_find_any(ctx->frame, name);
  VkrSampleIoRequest *request = ctx->frame->io_request;
  if (!entity.u64 || !input.length || input.length >= sizeof(request->input) ||
      value.length >= sizeof(request->value) || !request) {
    snprintf(ctx->message, sizeof(ctx->message),
             "io.fire needs an object, one of its inputs and an optional "
             "value");
    return false_v;
  }
  if (!ctx->frame->io_running) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Inputs reach objects only while the game plays");
    return false_v;
  }
  *request = (VkrSampleIoRequest){.target = entity, .send = true_v};
  MemCopy(request->input, input.str, input.length);
  MemCopy(request->value, value.str, value.length);
  snprintf(ctx->message, sizeof(ctx->message), "Sent %.*s to %.*s",
           (int)input.length, input.str, (int)name.length, name.str);
  return true_v;
}

static bool8_t cmd_run_visibility(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  (void)def;
  (void)arg;
  if (!cmd_require_scene(ctx))
    return false_v;
  if (!vkr_scene_entity_alive(
          vkr_editor_entity_scene(ctx->frame, ctx->frame->selected_entity),
          ctx->frame->selected_entity)) {
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
  /* A docked panel hosts the window's body: show its tab. */
  const VkrUiDockPanelKind docked = vkr_editor_window_dock_panel(kind);
  if (docked != VKR_UI_DOCK_PANEL_COUNT &&
      vkr_editor_dock_has(ctx->frame->dock, docked)) {
    vkr_editor_dock_show(ctx->frame->dock, docked);
    const String8 tab = vkr_ui_dock_panel_label(docked);
    snprintf(ctx->message, sizeof(ctx->message),
             "%.*s is docked; showing its tab", (int)tab.length, tab.str);
    return true_v;
  }
  bool8_t want = false_v;
  if (!cmd_switch(ctx, state, ctx->editor->windows[kind].visible, &want))
    return false_v;
  vkr_editor_window_set_visible(ctx->editor, kind, want);
  return true_v;
}

/* workbench.duplicate, .delete [workbench], .move <left|right> and
   .rename <name>: custom workbenches (ADR-089). */
static bool8_t cmd_run_workbench_edit(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  VkrEditorWorkbenches *workbenches = &ctx->editor->workbenches;
  const String8 word = cmd_unquote(arg);
  if (def->value == 3u) {
    return vkr_editor_workbench_rename(ctx->editor, workbenches->active, word,
                                       ctx->message, sizeof(ctx->message));
  }
  if (def->value == 2u) {
    const bool8_t left = cmd_equals(word, cmd_cstr("left"));
    if (!left && !cmd_equals(word, cmd_cstr("right"))) {
      snprintf(ctx->message, sizeof(ctx->message),
               "Usage: workbench.move "
               "<left|right>");
      return false_v;
    }
    return vkr_editor_workbench_move(ctx->editor, workbenches->active,
                                     left ? -1 : 1, ctx->message,
                                     sizeof(ctx->message));
  }
  const uint32_t index = word.length
                             ? vkr_editor_workbench_find(workbenches, word)
                             : workbenches->active;
  if (index == UINT32_MAX) {
    snprintf(ctx->message, sizeof(ctx->message), "Unknown workbench '%.*s'",
             (int)word.length, word.str);
    return false_v;
  }
  return def->value == 0u
             ? vkr_editor_workbench_duplicate(ctx->editor, ctx->frame, index,
                                              ctx->message,
                                              sizeof(ctx->message))
             : vkr_editor_workbench_delete(ctx->editor, ctx->frame, index,
                                           ctx->message, sizeof(ctx->message));
}

/* workbench [id | name | 1-9 | next | prev]: lists the workbenches, or
   switches at the start of the next frame. */
static bool8_t cmd_run_workbench(CmdContext *ctx, const CmdDef *def,
                                 String8 arg) {
  (void)def;
  VkrEditorWorkbenches *workbenches = &ctx->editor->workbenches;
  const String8 word = cmd_unquote(arg);
  if (!word.length) {
    uint64_t used = 0u;
    for (uint32_t i = 0; i < workbenches->count; ++i) {
      used += (uint64_t)snprintf(
          ctx->message + used, sizeof(ctx->message) - used, "%s%u %s [%s]%s",
          i ? ", " : "", i + 1u, vkr_editor_workbench_name(workbenches, i),
          vkr_editor_workbench_id(workbenches, i),
          i == workbenches->active ? " (active)" : "");
      if (used >= sizeof(ctx->message))
        break;
    }
    return true_v;
  }
  const uint32_t index = vkr_editor_workbench_find(workbenches, word);
  if (index == UINT32_MAX) {
    snprintf(ctx->message, sizeof(ctx->message), "Unknown workbench '%.*s'",
             (int)word.length, word.str);
    return false_v;
  }
  return vkr_editor_workbench_request(ctx->editor, ctx->frame, index,
                                      ctx->message, sizeof(ctx->message));
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
    else if (def->value == 2u) {
      if (!cmd_switch(ctx, word, next.grid_labels, &next.grid_labels))
        return false_v;
    } else if (def->value == 3u) {
      if (!cmd_switch(ctx, word, next.greybox_view, &next.greybox_view)) {
        return false_v;
      }
      snprintf(ctx->message, sizeof(ctx->message), "Greybox view %s",
               next.greybox_view ? "on" : "off");
    } else if (!cmd_switch(ctx, word, next.grid_enabled, &next.grid_enabled))
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

/* grid.fit lifts the grid onto the surface at the Scene's centre; grid.height
   sets its world Y, which may be zero or negative. */
static bool8_t cmd_run_grid_height(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  if (def->value == 1u) {
    if (!ctx->frame->grid_fit_request) {
      snprintf(ctx->message, sizeof(ctx->message),
               "The Scene view is not available");
      return false_v;
    }
    vkr_editor_view_fit_grid(ctx->frame);
    return true_v;
  }
  const String8 word = cmd_split(arg, NULL);
  float64_t number = 0.0;
  if (!cmd_number(word, &number)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Expected a height in world units, not '%.*s'", (int)word.length,
             word.str);
    return false_v;
  }
  VkrSampleViewState next = ctx->frame->view_state;
  next.grid_height = vkr_clamp_f32((float32_t)number, -10000.0f, 10000.0f);
  next.grid_enabled = true_v;
  return cmd_view_request(ctx, next);
}

static bool8_t cmd_run_labels(CmdContext *ctx, const CmdDef *def, String8 arg) {
  VkrEditorUi *editor = ctx->editor;
  bool8_t *targets[] = {&editor->labels_enabled, &editor->labels_directional,
                        &editor->labels_spot, &editor->labels_point,
                        &editor->labels_occlusion};
  bool8_t *target = targets[def->value];
  return cmd_switch(ctx, cmd_split(arg, NULL), *target, target);
}

/* Metres from the camera past which icons fade out; 0 shows them at any
   distance. */
static bool8_t cmd_run_labels_distance(CmdContext *ctx, const CmdDef *def,
                                       String8 arg) {
  (void)def;
  const String8 word = cmd_split(arg, NULL);
  float64_t number = 0.0;
  if (!cmd_number(word, &number) || number < 0.0) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Expected a distance in metres, 0 for no limit, not '%.*s'",
             (int)word.length, word.str);
    return false_v;
  }
  VkrEditorUi *editor = ctx->editor;
  editor->labels_max_distance = vkr_editor_label_distance((float32_t)number);
  if (editor->labels_max_distance > 0.0f) {
    snprintf(ctx->message, sizeof(ctx->message), "Icon distance %.4g m",
             (double)editor->labels_max_distance);
  } else {
    snprintf(ctx->message, sizeof(ctx->message), "Icon distance unlimited");
  }
  return true_v;
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

/* Queues one operation of the agent table (ADR-084) as the editor's own
   request; Cmd and agents then share one implementation. */
static bool8_t cmd_run_op(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  String8 rest = {0};
  const String8 name = cmd_split(arg, &rest);
  rest = cmd_trim(rest);
  if (!name.length || name.length > 64u ||
      (rest.length && rest.str[0] != '{')) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Usage: op <operation> [json object]");
    return false_v;
  }
  char line[1400];
  const int length =
      snprintf(line, sizeof(line),
               "{\"v\":1,\"id\":\"cmd\",\"op\":\"%.*s\",\"args\":%.*s}",
               (int)name.length, name.str, rest.length ? (int)rest.length : 2,
               rest.length ? (const char *)rest.str : "{}");
  if (length <= 0 || (size_t)length >= sizeof(line) ||
      !vkr_editor_agent_submit(ctx->editor->agent, line)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "The operation could not be queued");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Queued %.*s", (int)name.length,
           name.str);
  /* The next statement sees the operation's effect. The `cmd` operation
     itself waits for this queue to go idle, so it cannot hold it. */
  if (!vkr_string8_equals_cstr(&name, "cmd")) {
    ctx->editor->cmd_holding_op = true_v;
    ctx->editor->cmd_hold_seconds = 0.0;
  }
  return true_v;
}

static bool8_t cmd_run_echo(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  snprintf(ctx->message, sizeof(ctx->message), "%.*s", (int)arg.length,
           arg.str);
  return true_v;
}

/* Loads a project scene by name, or a scene document by path, beside the
 * active scene (ADR-076). Legacy scenes keep their edits in
 * `<path>.editor.json`, as the primary scene does. */
static bool8_t cmd_run_scene_add(CmdContext *ctx, const CmdDef *def,
                                 String8 arg) {
  (void)def;
  const String8 path = cmd_unquote(arg);
  VkrEditorUi *editor = ctx->editor;
  if (vkr_editor_projects_add_scene(editor->projects, editor, ctx->frame,
                                    path)) {
    snprintf(ctx->message, sizeof(ctx->message), "Adding %.*s",
             (int)path.length, path.str);
    return true_v;
  }
  if (!path.length || path.length >= sizeof(editor->cmd_scene_path) ||
      path.length + 13u >= sizeof(editor->cmd_scene_sidecar)) {
    snprintf(ctx->message, sizeof(ctx->message), "scene.add needs a path");
    return false_v;
  }
  snprintf(editor->cmd_scene_path, sizeof(editor->cmd_scene_path), "%.*s",
           (int)path.length, path.str);
  snprintf(editor->cmd_scene_sidecar, sizeof(editor->cmd_scene_sidecar),
           "%.*s.editor.json", (int)path.length, path.str);
  *ctx->frame->scene_request = (VkrSampleSceneRequest){
      .add = true_v,
      .path = cmd_cstr(editor->cmd_scene_path),
      .sidecar_path = cmd_cstr(editor->cmd_scene_sidecar)};
  snprintf(ctx->message, sizeof(ctx->message), "Adding %.*s", (int)path.length,
           path.str);
  return true_v;
}

/* The added scene with slot number `word` or whose path or project name
   contains it, as a container id; 0 when none matches. */
static uint16_t cmd_added_container(const CmdContext *ctx, String8 word) {
  const VkrSampleUiFrame *frame = ctx->frame;
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    if (!frame->additive[i])
      continue;
    char slot[8];
    snprintf(slot, sizeof(slot), "%u", i + 1u);
    const String8 name = vkr_editor_projects_added_name(
        ctx->editor->projects, frame->additive_names[i]);
    if (cmd_equals(word, cmd_cstr(slot)) ||
        (word.length && (cmd_contains(frame->additive_names[i], word) ||
                         cmd_contains(name, word))))
      return (uint16_t)(i + 1u);
  }
  return 0u;
}

/* Instantiates a project scene as a prefab in the open scene, at the origin
   (ADR-076). */
static bool8_t cmd_run_scene_instantiate(CmdContext *ctx, const CmdDef *def,
                                         String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  if (!vkr_editor_projects_instantiate_scene(ctx->editor->projects, ctx->editor,
                                             ctx->frame, name, vec3_zero())) {
    snprintf(ctx->message, sizeof(ctx->message),
             "No other project scene '%.*s', or no scene can change now",
             (int)name.length, name.str);
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Instantiating %.*s",
           (int)name.length, name.str);
  return true_v;
}

/* Makes an added scene the primary scene; the previous primary is added
   back beside it (ADR-076). */
static bool8_t cmd_run_scene_primary(CmdContext *ctx, const CmdDef *def,
                                     String8 arg) {
  (void)def;
  const String8 word = cmd_unquote(arg);
  const uint16_t container = cmd_added_container(ctx, word);
  if (!container) {
    snprintf(ctx->message, sizeof(ctx->message),
             "No added scene matches '%.*s'", (int)word.length, word.str);
    return false_v;
  }
  if (!vkr_editor_projects_set_primary(ctx->editor->projects, ctx->editor,
                                       ctx->frame, container)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Only an added project scene can become primary, once no job "
             "is running");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Making scene %u primary",
           container);
  return true_v;
}

/* Removes an additive scene by its slot number or a part of its path. */
static bool8_t cmd_run_scene_remove(CmdContext *ctx, const CmdDef *def,
                                    String8 arg) {
  (void)def;
  const String8 word = cmd_split(cmd_unquote(arg), NULL);
  const bool8_t discard = cmd_contains(arg, cmd_cstr(" discard")) != 0;
  const uint16_t container = cmd_added_container(ctx, word);
  if (!container) {
    snprintf(ctx->message, sizeof(ctx->message),
             "No added scene matches '%.*s'", (int)word.length, word.str);
    return false_v;
  }
  *ctx->frame->scene_request = (VkrSampleSceneRequest){
      .remove = true_v, .container = container, .discard_edits = discard};
  snprintf(ctx->message, sizeof(ctx->message), "Removing scene %u", container);
  return true_v;
}

/* Script modules (ADR-079): create one from the template, open a source in
 * the Script editor, or report each module's build and load state. */
static bool8_t cmd_run_script_new(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  char text[64];
  char path[VKR_EDITOR_SCRIPT_PATH];
  if (!name.length || name.length >= sizeof(text)) {
    snprintf(ctx->message, sizeof(ctx->message), "script.new needs a name");
    return false_v;
  }
  MemCopy(text, name.str, name.length);
  text[name.length] = '\0';
  if (!vkr_editor_scripts_create_module(ctx->editor->scripts, text, path,
                                        sizeof(path), ctx->message,
                                        sizeof(ctx->message))) {
    return false_v;
  }
  /* As the Script editor's Create: an object waiting on "New script" gets
     this module's script once it loads. */
  VkrEditorUi *editor = ctx->editor;
  if (editor->script_attach_entity.u64 && !editor->script_attach_module[0]) {
    snprintf(editor->script_attach_module, sizeof(editor->script_attach_module),
             "%s", text);
  }
  (void)vkr_editor_code_open(ctx->editor->code, ctx->editor, path);
  snprintf(ctx->message, sizeof(ctx->message), "Created %s", path);
  return true_v;
}

static bool8_t cmd_run_script_open(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  const VkrEditorScripts *scripts = ctx->editor->scripts;
  for (uint32_t i = 0; i < vkr_editor_scripts_file_count(scripts); ++i) {
    const VkrEditorScriptFile *file = vkr_editor_scripts_file(scripts, i);
    if (strlen(file->name) == name.length &&
        !MemCompare(file->name, name.str, name.length) &&
        vkr_editor_code_open(ctx->editor->code, ctx->editor, file->path)) {
      snprintf(ctx->message, sizeof(ctx->message), "Opened %s", file->path);
      return true_v;
    }
  }
  snprintf(ctx->message, sizeof(ctx->message), "No script file '%.*s'",
           (int)name.length, name.str);
  return false_v;
}

/* Synthetic pointer input at a window position in points: moves there, then
 * clicks `count` times (two makes a double click) with the left button, or
 * once with the right. `alt` holds Alt and `ctrl` holds Ctrl (Cmd on macOS)
 * through the left clicks. For scripted checks of mouse-only interactions. */
static bool8_t cmd_run_ui_click(CmdContext *ctx, const CmdDef *def,
                                String8 arg) {
  (void)def;
  char text[64] = {0};
  MemCopy(text, arg.str, Min(arg.length, (uint64_t)sizeof(text) - 1u));
  float32_t x = 0.0f;
  float32_t y = 0.0f;
  int32_t count = 1;
  char button[16] = {0};
  const int32_t read = sscanf(text, "%f %f %d %15s", &x, &y, &count, button);
  const bool8_t right = !strcmp(button, "right");
  const bool8_t alt = !strcmp(button, "alt");
  const bool8_t ctrl = !strcmp(button, "ctrl");
  if (read < 2 || count < 1 || count > 3 || !isfinite(x) || !isfinite(y) ||
      (read == 4 && !right && !alt && !ctrl)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "ui.click needs <x> <y> [count] [right|alt|ctrl] in points");
    return false_v;
  }
#if defined(PLATFORM_APPLE)
  const Keys primary = KEY_LWIN;
#else
  const Keys primary = KEY_LCONTROL;
#endif
  const Keys held = alt ? KEY_LMENU : ctrl ? primary : KEY_MAX_KEYS;
  VkrEditorUi *editor = ctx->editor;
  const float32_t scale = ctx->frame->ui->content_scale;
  const int32_t px = (int32_t)(x * scale);
  const int32_t py = (int32_t)(y * scale);
  editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
  int32_t(*steps)[4] = editor->cmd_pointer_steps;
  if (held != KEY_MAX_KEYS) {
    int32_t *step = steps[editor->cmd_pointer_count++];
    step[0] = 3;
    step[1] = px;
    step[2] = py;
    step[3] = held;
  }
  steps[editor->cmd_pointer_count][0] = 0;
  steps[editor->cmd_pointer_count][1] = px;
  steps[editor->cmd_pointer_count++][2] = py;
  steps[editor->cmd_pointer_count][0] = 0;
  steps[editor->cmd_pointer_count][1] = px;
  steps[editor->cmd_pointer_count++][2] = py;
  for (int32_t i = 0; i < (right ? 1 : count); ++i) {
    for (int32_t phase = 1; phase <= 2; ++phase) {
      int32_t *step = steps[editor->cmd_pointer_count++];
      step[0] = phase;
      step[1] = px;
      step[2] = py;
      step[3] = right ? BUTTON_RIGHT : BUTTON_LEFT;
    }
  }
  if (held != KEY_MAX_KEYS) {
    int32_t *step = steps[editor->cmd_pointer_count++];
    step[0] = 4;
    step[1] = px;
    step[2] = py;
    step[3] = held;
  }
  snprintf(ctx->message, sizeof(ctx->message), "%s at (%.0f, %.0f)",
           right        ? "Right click"
           : count == 2 ? "Double click"
                        : "Click",
           x, y);
  return true_v;
}

/* ui.drag <x0> <y0> <x1> <y1>: presses at the start, holds while a pick
 * resolves, moves to the end over several frames and releases, as a
 * left-button drag would. */
static bool8_t cmd_run_ui_drag(CmdContext *ctx, const CmdDef *def,
                               String8 arg) {
  (void)def;
  char text[96] = {0};
  MemCopy(text, arg.str, Min(arg.length, (uint64_t)sizeof(text) - 1u));
  float32_t from_x = 0.0f;
  float32_t from_y = 0.0f;
  float32_t to_x = 0.0f;
  float32_t to_y = 0.0f;
  if (sscanf(text, "%f %f %f %f", &from_x, &from_y, &to_x, &to_y) != 4 ||
      !isfinite(from_x) || !isfinite(from_y) || !isfinite(to_x) ||
      !isfinite(to_y)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "ui.drag needs <x0> <y0> <x1> <y1> in points");
    return false_v;
  }
  VkrEditorUi *editor = ctx->editor;
  const float32_t scale = ctx->frame->ui->content_scale;
  enum { HOLD_FRAMES = 8, MOVE_FRAMES = 16 };
  editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
  int32_t(*steps)[4] = editor->cmd_pointer_steps;
  for (int32_t i = 0; i < 2 + 1 + HOLD_FRAMES + MOVE_FRAMES + 1; ++i) {
    const int32_t moved = Max(0, i - 2 - 1 - HOLD_FRAMES + 1);
    const float32_t t = (float32_t)Min(moved, MOVE_FRAMES) / MOVE_FRAMES;
    int32_t *step = steps[editor->cmd_pointer_count++];
    step[0] = i == 2 ? 1 : i == 2 + 1 + HOLD_FRAMES + MOVE_FRAMES ? 2 : 0;
    step[1] = (int32_t)((from_x + (to_x - from_x) * t) * scale);
    step[2] = (int32_t)((from_y + (to_y - from_y) * t) * scale);
    step[3] = BUTTON_LEFT;
  }
  snprintf(ctx->message, sizeof(ctx->message),
           "Drag from (%.0f, %.0f) to (%.0f, %.0f)", from_x, from_y, to_x,
           to_y);
  return true_v;
}

/* ui.key [cmd+|alt+|ctrl+|shift+]<key> presses the modifiers, then presses
 * and releases the key and releases the modifiers, one edge per frame, for
 * scripted checks of keyboard navigation and editing shortcuts. */
static bool8_t cmd_run_ui_key(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  static const struct {
    const char *name;
    Keys key;
  } keys[] = {{"up", KEY_UP},         {"down", KEY_DOWN},
              {"left", KEY_LEFT},     {"right", KEY_RIGHT},
              {"enter", KEY_ENTER},   {"escape", KEY_ESCAPE},
              {"tab", KEY_TAB},       {"backspace", KEY_BACKSPACE},
              {"delete", KEY_DELETE}, {"home", KEY_HOME},
              {"pageup", KEY_PRIOR},  {"pagedown", KEY_NEXT},
              {"end", KEY_END},       {"shift", KEY_LSHIFT},
              {"ctrl", KEY_LCONTROL}, {"alt", KEY_LMENU},
              {"a", KEY_A},           {"c", KEY_C},
              {"v", KEY_V},           {"x", KEY_X},
              {"y", KEY_Y},           {"z", KEY_Z}};
  static const struct {
    const char *prefix;
    Keys key;
  } modifiers[] = {{"cmd+", KEY_LWIN},
                   {"alt+", KEY_LMENU},
                   {"ctrl+", KEY_LCONTROL},
                   {"shift+", KEY_LSHIFT}};
  String8 word = cmd_unquote(arg);
  /* A trailing "down" or "up" presses or releases only, so a key can stay
     held across statements, as walking in Play needs. */
  int32_t only = 0;
  static const struct {
    const char *suffix;
    int32_t phase;
  } halves[] = {{" down", 3}, {" up", 4}};
  for (uint32_t h = 0; h < ArrayCount(halves); ++h) {
    const uint64_t length = strlen(halves[h].suffix);
    if (word.length > length &&
        !MemCompare(halves[h].suffix, word.str + word.length - length,
                    length)) {
      only = halves[h].phase;
      word.length -= length;
    }
  }
  Keys held[ArrayCount(modifiers)];
  uint32_t held_count = 0u;
  for (bool8_t found = true_v; found;) {
    found = false_v;
    for (uint32_t m = 0; m < ArrayCount(modifiers); ++m) {
      const uint64_t length = strlen(modifiers[m].prefix);
      if (word.length > length &&
          !MemCompare(modifiers[m].prefix, word.str, length) &&
          held_count < ArrayCount(held)) {
        held[held_count++] = modifiers[m].key;
        word.str += length;
        word.length -= length;
        found = true_v;
      }
    }
  }
  /* A named key, then any letter or digit, space, or f1 to f12. */
  Keys key = KEY_MAX_KEYS;
  for (uint32_t i = 0; i < ArrayCount(keys) && key == KEY_MAX_KEYS; ++i) {
    if (strlen(keys[i].name) == word.length &&
        !MemCompare(keys[i].name, word.str, word.length)) {
      key = keys[i].key;
    }
  }
  if (key == KEY_MAX_KEYS && word.length == 1u) {
    const char c = (char)word.str[0];
    if (c >= 'a' && c <= 'z') {
      key = (Keys)(KEY_A + (c - 'a'));
    } else if (c >= '0' && c <= '9') {
      key = (Keys)(KEY_0 + (c - '0'));
    }
  }
  if (key == KEY_MAX_KEYS && word.length == 5u &&
      !MemCompare("space", word.str, 5u)) {
    key = KEY_SPACE;
  }
  if (key == KEY_MAX_KEYS && word.length >= 2u && word.length <= 3u &&
      word.str[0] == 'f') {
    uint32_t number = 0u;
    for (uint64_t i = 1u; i < word.length; ++i) {
      number = word.str[i] >= '0' && word.str[i] <= '9'
                   ? number * 10u + (uint32_t)(word.str[i] - '0')
                   : 0u;
    }
    if (number >= 1u && number <= 12u) {
      key = (Keys)(KEY_F1 + (number - 1u));
    }
  }
  VkrEditorUi *editor = ctx->editor;
  if (key != KEY_MAX_KEYS) {
    editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
    for (uint32_t m = 0; only != 4 && m < held_count; ++m) {
      int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_count++];
      step[0] = 3;
      step[3] = (int32_t)held[m];
    }
    for (int32_t phase = only ? only : 3; phase <= (only ? only : 4); ++phase) {
      int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_count++];
      step[0] = phase;
      step[3] = (int32_t)key;
    }
    for (uint32_t m = 0; only != 3 && m < held_count; ++m) {
      int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_count++];
      step[0] = 4;
      step[3] = (int32_t)held[m];
    }
    snprintf(ctx->message, sizeof(ctx->message), "Key %.*s",
             (int32_t)arg.length, (const char *)arg.str);
    return true_v;
  }
  snprintf(ctx->message, sizeof(ctx->message),
           "ui.key needs [cmd+|alt+|ctrl+|shift+] and up, down, left, right, "
           "enter, escape, tab, backspace, delete, home, end, space, shift, "
           "ctrl, alt, a letter, a digit or f1 to f12, then [down|up]");
  return false_v;
}

/* ui.look <dx> <dy> moves the pointer by points from where it is, as mouse
 * motion would; captured gameplay reads it as look (ADR-073). */
static bool8_t cmd_run_ui_look(CmdContext *ctx, const CmdDef *def,
                               String8 arg) {
  (void)def;
  char text[64] = {0};
  MemCopy(text, arg.str, Min(arg.length, (uint64_t)sizeof(text) - 1u));
  int32_t dx = 0;
  int32_t dy = 0;
  if (sscanf(text, "%d %d", &dx, &dy) != 2) {
    snprintf(ctx->message, sizeof(ctx->message),
             "ui.look needs <dx> <dy> in points");
    return false_v;
  }
  VkrEditorUi *editor = ctx->editor;
  editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
  int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_count++];
  step[0] = 6;
  step[1] = dx;
  step[2] = dy;
  snprintf(ctx->message, sizeof(ctx->message), "Look %d %d", dx, dy);
  return true_v;
}

/* ui.type <text> commits up to 32 ASCII characters to the focused field, one
 * per frame, as the keyboard's text input would. */
static bool8_t cmd_run_ui_type(CmdContext *ctx, const CmdDef *def,
                               String8 arg) {
  (void)def;
  const String8 text = cmd_unquote(arg);
  VkrEditorUi *editor = ctx->editor;
  if (!text.length || text.length > ArrayCount(editor->cmd_pointer_steps)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "ui.type needs 1 to %u characters",
             (uint32_t)ArrayCount(editor->cmd_pointer_steps));
    return false_v;
  }
  editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
  for (uint64_t i = 0; i < text.length; ++i) {
    int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_count++];
    step[0] = 5;
    step[3] = (int32_t)text.str[i];
  }
  snprintf(ctx->message, sizeof(ctx->message), "Typed %u characters",
           (uint32_t)text.length);
  return true_v;
}

void vkr_editor_cmd_pointer_input(VkrEditorUi *editor, InputState *input) {
  /* One step per frame, after the host advanced input and before the UI
   * reads it, so each press and release is its own edge. */
  if (editor->cmd_pointer_next >= editor->cmd_pointer_count) {
    return;
  }
  const int32_t *step = editor->cmd_pointer_steps[editor->cmd_pointer_next++];
  if (step[0] == 0) {
    input_process_mouse_move(input, step[1], step[2]);
  } else if (step[0] == 6) {
    int32_t x = 0;
    int32_t y = 0;
    input_get_mouse_position(input, &x, &y);
    input_process_mouse_move(input, x + step[1], y + step[2]);
  } else if (step[0] == 5) {
    (void)input_process_char(input, (uint32_t)step[3]);
  } else if (step[0] >= 3) {
    input_process_key(input, (Keys)step[3], step[0] == 3);
  } else {
    input_process_button(input, (Buttons)step[3], step[0] == 1);
  }
  if (editor->cmd_pointer_next == editor->cmd_pointer_count) {
    editor->cmd_pointer_count = editor->cmd_pointer_next = 0u;
  }
}

static bool8_t cmd_run_script_goto(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  (void)def;
  char text[16] = {0};
  MemCopy(text, arg.str, Min(arg.length, (uint64_t)sizeof(text) - 1u));
  const long line = strtol(text, NULL, 10);
  if (line <= 0 || !vkr_editor_code_goto(ctx->editor->code, (uint32_t)line)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "script.goto needs a line and an open script");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Line %ld", line);
  return true_v;
}

static bool8_t cmd_run_script_type(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  (void)def;
  const String8 value = cmd_unquote(arg);
  char text[256];
  if (!value.length || value.length >= sizeof(text)) {
    snprintf(ctx->message, sizeof(ctx->message), "script.type needs text");
    return false_v;
  }
  MemCopy(text, value.str, value.length);
  text[value.length] = '\0';
  if (!vkr_editor_code_type(ctx->editor->code, text)) {
    snprintf(ctx->message, sizeof(ctx->message), "No script tab is open");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Typed %u characters",
           (uint32_t)value.length);
  return true_v;
}

static bool8_t cmd_run_script_save(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  (void)def;
  (void)arg;
  if (!vkr_editor_code_save_active(ctx->editor->code, ctx->editor)) {
    snprintf(ctx->message, sizeof(ctx->message), "No script tab to save");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Saved; rebuilding");
  return true_v;
}

static bool8_t cmd_run_script_status(CmdContext *ctx, const CmdDef *def,
                                     String8 arg) {
  (void)def;
  (void)arg;
  static const char *const statuses[] = {"unbuilt", "building", "build failed",
                                         "loading", "loaded",   "load failed"};
  const VkrEditorScripts *scripts = ctx->editor->scripts;
  uint32_t length = 0u;
  const uint32_t count = vkr_editor_scripts_module_count(scripts);
  for (uint32_t i = 0; i < count && length < sizeof(ctx->message); ++i) {
    const VkrEditorScriptModule *module = vkr_editor_scripts_module(scripts, i);
    length += (uint32_t)snprintf(
        ctx->message + length, sizeof(ctx->message) - length, "%s%s: %s (%s)",
        i ? "; " : "", module->name, statuses[module->status], module->message);
  }
  if (length < sizeof(ctx->message)) {
    snprintf(ctx->message + length, sizeof(ctx->message) - length,
             "%s%u diagnostics", count ? "; " : "no modules; ",
             vkr_editor_scripts_diagnostic_count(scripts));
  }
  return true_v;
}

/* Creates an empty scene in the open project and opens it (ADR-076). */
static bool8_t cmd_run_scene_create(CmdContext *ctx, const CmdDef *def,
                                    String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  char text[VKR_EDITOR_PROJECT_NAME_CAPACITY];
  if (!name.length || name.length >= sizeof(text)) {
    snprintf(ctx->message, sizeof(ctx->message), "scene.create needs a name");
    return false_v;
  }
  MemCopy(text, name.str, name.length);
  text[name.length] = '\0';
  if (!vkr_editor_projects_create_scene(ctx->editor->projects, ctx->editor,
                                        ctx->frame, text)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "No writable project can create a scene now");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Creating scene %s", text);
  return true_v;
}

/* `scene.bake [lightmaps]`: the Bake lighting job for the open project scene
   (ADR-088). Lightmaps follow the Bakery panel's option unless `lightmaps`
   asks for them; the queue holds until the bake and the scene reload after
   it settle, then reports the outcome. */
static bool8_t cmd_run_scene_bake(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  /* "lightmaps" and an optional sample count per texel. */
  const String8 text = cmd_trim(arg);
  uint64_t split = 0u;
  while (split < text.length && text.str[split] != ' ') {
    ++split;
  }
  const String8 word = {.str = text.str, .length = split};
  const String8 rest = cmd_trim(
      (String8){.str = text.str + split, .length = text.length - split});
  uint32_t samples = 0u;
  bool8_t valid = !word.length || vkr_string8_equals_cstr(&word, "lightmaps");
  for (uint64_t i = 0u; valid && i < rest.length; ++i) {
    valid = rest.str[i] >= '0' && rest.str[i] <= '9' && samples < 100000u;
    samples = samples * 10u + (uint32_t)(rest.str[i] - '0');
  }
  if (!valid || (rest.length && (samples < 1u || samples > 4096u))) {
    snprintf(ctx->message, sizeof(ctx->message), "Usage: %s %s", def->name,
             def->usage);
    return false_v;
  }
  const bool8_t lightmap =
      word.length != 0u || vkr_editor_bakery_lightmap(ctx->editor->bakery);
  if (!vkr_editor_projects_bake_lighting(ctx->editor->projects, ctx->editor,
                                         ctx->frame, lightmap, samples)) {
    snprintf(ctx->message, sizeof(ctx->message), "%s",
             vkr_editor_projects_message(ctx->editor->projects));
    return false_v;
  }
  ctx->editor->cmd_holding_bake = true_v;
  snprintf(ctx->message, sizeof(ctx->message),
           "Baking reflection probes, the diffuse volume%s",
           lightmap ? " and lightmaps" : "");
  return true_v;
}

/* Opens a project scene by name, as double-clicking it in Content does. */
static bool8_t cmd_run_scene_open(CmdContext *ctx, const CmdDef *def,
                                  String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  bool8_t current = false_v;
  if (!vkr_editor_projects_open_scene(ctx->editor->projects, ctx->editor,
                                      ctx->frame, name, &current)) {
    snprintf(ctx->message, sizeof(ctx->message), "No project scene '%.*s'",
             (int)name.length, name.str);
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message),
           current ? "%.*s is already open or loading" : "Opening %.*s",
           (int)name.length, name.str);
  return true_v;
}

/* Opens the Create window importing a scene JSON; its preflight runs. */
static bool8_t cmd_run_scene_import(CmdContext *ctx, const CmdDef *def,
                                    String8 arg) {
  (void)def;
  const String8 path = cmd_unquote(arg);
  char text[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!path.length || path.length >= sizeof(text)) {
    snprintf(ctx->message, sizeof(ctx->message), "scene.import needs a path");
    return false_v;
  }
  MemCopy(text, path.str, path.length);
  text[path.length] = '\0';
  if (!vkr_editor_projects_import_scene_form(ctx->editor->projects, ctx->editor,
                                             text)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Importing a scene needs a writable project");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Inspecting %s", text);
  return true_v;
}

/* Imports one file into the project's shared assets (ADR-076), or places a
   model as the import step does: in the World, a new scene or a project
   scene. A path with spaces is quoted. */
static bool8_t cmd_run_content_import(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  (void)def;
  String8 rest = {0};
  String8 path = cmd_trim(arg);
  if (path.length > 1u && path.str[0] == '"') {
    uint64_t end = 1u;
    while (end < path.length && path.str[end] != '"')
      ++end;
    rest =
        cmd_trim((String8){.str = path.str + Min(end + 1u, path.length),
                           .length = path.length - Min(end + 1u, path.length)});
    path = (String8){.str = path.str + 1, .length = end - 1u};
  } else {
    path = cmd_split(path, &rest);
  }
  char text[VKR_EDITOR_PROJECT_PATH_CAPACITY];
  if (!path.length || path.length >= sizeof(text)) {
    snprintf(ctx->message, sizeof(ctx->message), "content.import needs a path");
    return false_v;
  }
  MemCopy(text, path.str, path.length);
  text[path.length] = '\0';
  String8 name = {0};
  const String8 target = cmd_split(rest, &name);
  if (target.length) {
    if (!vkr_editor_projects_import_to(ctx->editor->projects, ctx->editor,
                                       ctx->frame, text, target,
                                       cmd_unquote(name))) {
      snprintf(ctx->message, sizeof(ctx->message), "%s",
               vkr_editor_projects_message(ctx->editor->projects));
      return false_v;
    }
    snprintf(ctx->message, sizeof(ctx->message), "Importing %s to %.*s", text,
             (int)target.length, (const char *)target.str);
    return true_v;
  }
  if (!vkr_editor_projects_import_asset(ctx->editor->projects, ctx->editor,
                                        ctx->frame, text)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "No writable, idle project can import now");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Importing %s", text);
  return true_v;
}

/* Whether the open scene inherits the World's objects (ADR-076). */
static bool8_t cmd_run_scene_inherit(CmdContext *ctx, const CmdDef *def,
                                     String8 arg) {
  (void)def;
  const VkrScene *scene = ctx->frame->scene;
  bool8_t inherit = false_v;
  if (!scene || !ctx->frame->world) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Open a project scene beside its World");
    return false_v;
  }
  if (!cmd_switch(ctx, cmd_split(arg, NULL), scene->settings.inherit_world,
                  &inherit)) {
    return false_v;
  }
  VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS};
  request.scene_settings = scene->settings;
  request.scene_settings.inherit_world = inherit;
  *ctx->frame->scene_edit = request;
  snprintf(ctx->message, sizeof(ctx->message), "Scene %s the World",
           inherit ? "inherits" : "no longer inherits");
  return true_v;
}

/* The texture limit of the open scene, or of the World with no scene open:
   reports it without an argument, else sets `full` or a power of two. */
static bool8_t cmd_run_scene_textures(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  (void)def;
  const VkrScene *scene =
      ctx->frame->scene ? ctx->frame->scene : ctx->frame->world;
  if (!scene) {
    snprintf(ctx->message, sizeof(ctx->message), "No scene or World is open");
    return false_v;
  }
  const char *owner = ctx->frame->scene ? "Scene" : "World";
  const String8 value = cmd_split(arg, NULL);
  if (!value.length) {
    const uint32_t extent = scene->settings.texture_max_extent;
    if (extent) {
      snprintf(ctx->message, sizeof(ctx->message), "%s textures: %u", owner,
               extent);
    } else {
      snprintf(ctx->message, sizeof(ctx->message),
               "%s textures: full resolution", owner);
    }
    return true_v;
  }
  const String8 full = string8_lit("full");
  uint32_t extent = 0u;
  if (!string8_equalsi(&value, &full) &&
      (!string8_to_u32(&value, &extent) || extent == 0u ||
       !vkr_scene_texture_extent_valid(extent))) {
    snprintf(ctx->message, sizeof(ctx->message),
             "Use full or a power of two from %u to %u",
             VKR_SCENE_TEXTURE_EXTENT_MIN, VKR_SCENE_TEXTURE_EXTENT_MAX);
    return false_v;
  }
  VkrSceneEditRequest request = {
      .action = VKR_SCENE_EDIT_APPLY_SCENE_SETTINGS,
      .container = ctx->frame->scene ? 0u : (uint16_t)VKR_SCENE_WORLD_ROOT_ID,
  };
  request.scene_settings = scene->settings;
  request.scene_settings.texture_max_extent = extent;
  *ctx->frame->scene_edit = request;
  if (extent) {
    snprintf(ctx->message, sizeof(ctx->message), "%s textures load at %u",
             owner, extent);
  } else {
    snprintf(ctx->message, sizeof(ctx->message),
             "%s textures load at full resolution", owner);
  }
  return true_v;
}

/* Filters the Content browser; an empty query shows every asset. */
static bool8_t cmd_run_content_search(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  (void)def;
  const String8 query = cmd_unquote(arg);
  if (!ctx->editor->content) {
    snprintf(ctx->message, sizeof(ctx->message), "Content is unavailable");
    return false_v;
  }
  vkr_editor_content_search(ctx->editor->content, query);
  snprintf(ctx->message, sizeof(ctx->message), "Content search '%.*s'",
           (int)query.length, query.str);
  return true_v;
}

/* Content folders (ADR-076): show, create, move into, and list or tiles. */
static bool8_t cmd_run_content_folder(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  VkrEditorContent *content = ctx->editor->content;
  if (!content) {
    snprintf(ctx->message, sizeof(ctx->message), "Content is unavailable");
    return false_v;
  }
  /* The first argument may be quoted to hold spaces, as in "Level One". */
  String8 rest = {0};
  String8 first = cmd_trim(arg);
  if (first.length > 1u && first.str[0] == '"') {
    uint64_t end = 1u;
    while (end < first.length && first.str[end] != '"')
      ++end;
    rest = cmd_trim(
        (String8){.str = first.str + Min(end + 1u, first.length),
                  .length = first.length - Min(end + 1u, first.length)});
    first = (String8){.str = first.str + 1, .length = end - 1u};
  } else {
    first = cmd_split(first, &rest);
  }
  const String8 second = cmd_unquote(rest);
  bool8_t ok = false_v;
  if (!strcmp(def->name, "content.open")) {
    ok = vkr_editor_content_open_folder(content, cmd_unquote(arg));
  } else if (!strcmp(def->name, "content.mkdir")) {
    ok = vkr_editor_content_new_folder(content, cmd_unquote(arg));
  } else if (!strcmp(def->name, "content.move")) {
    ok = vkr_editor_content_move(content, first, second);
  } else if (!strcmp(def->name, "content.drop")) {
    /* As if the file were dropped from the OS on the current folder. */
    VkrWindowFileDrop *drop =
        vkr_allocator_alloc(ctx->frame->ui->frame_allocator, sizeof(*drop),
                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    const String8 path = cmd_unquote(arg);
    ok = drop && path.length && path.length < sizeof(drop->paths[0]);
    if (ok) {
      MemZero(drop, sizeof(*drop));
      MemCopy(drop->paths[0], path.str, path.length);
      drop->count = 1u;
      ok = vkr_editor_projects_import_files(ctx->editor->projects, ctx->editor,
                                            drop,
                                            vkr_editor_content_folder(content));
    }
  } else if (!strcmp(def->name, "content.command")) {
    static const char *const commands[] = {"load", "open", "place", "rename",
                                           "delete"};
    ok = false_v;
    for (uint32_t i = 0; i < ArrayCount(commands); ++i) {
      if (cmd_equals(first, cmd_cstr(commands[i]))) {
        ok = vkr_editor_content_command(content, second,
                                        (VkrEditorContentCommand)i);
      }
    }
  } else if (!strcmp(def->name, "content.reveal")) {
    char path[1024];
    const String8 text = cmd_unquote(arg);
    ok = text.length < sizeof(path);
    if (ok) {
      MemCopy(path, text.str, text.length);
      path[text.length] = '\0';
      ok = vkr_editor_content_reveal_path(content, path);
    }
    if (ok) {
      vkr_editor_dock_show(ctx->frame->dock, VKR_UI_DOCK_PANEL_CONTENT);
      snprintf(ctx->message, sizeof(ctx->message), "Revealed %s in %s",
               vkr_editor_content_selected_name(content),
               vkr_editor_content_folder(content)[0]
                   ? vkr_editor_content_folder(content)
                   : "Content");
      return true_v;
    }
  } else if (!strcmp(def->name, "content.place")) {
    /* The drop lands at the viewport's centre. */
    const Vec4 image = ctx->frame->mapping.image_rect_px;
    ok = vkr_editor_content_place(
        content, cmd_unquote(arg),
        (Vec2){image.x + image.z * 0.5f, image.y + image.w * 0.5f});
  } else {
    const bool8_t list = cmd_equals(cmd_cstr("list"), first);
    ok = list || cmd_equals(cmd_cstr("tiles"), first);
    if (ok) {
      vkr_editor_content_set_list(content, list);
    }
  }
  snprintf(ctx->message, sizeof(ctx->message), "%s %s", def->name,
           ok ? "done" : "failed: check the folder, item and write access");
  return ok;
}

/* The selected entity, or none with a message. */
static bool8_t cmd_selection(CmdContext *ctx, VkrEntityId *out) {
  const VkrEntityId entity = ctx->frame->selected_entity;
  if (!vkr_scene_entity_alive(vkr_editor_entity_scene(ctx->frame, entity),
                              entity)) {
    snprintf(ctx->message, sizeof(ctx->message), "Nothing is selected");
    return false_v;
  }
  *out = entity;
  return true_v;
}

/* Creates an object in the selection's scene, the primary scene or the
 * World, at the viewport's centre under the Snapping settings (ADR-076). */
static bool8_t cmd_run_create(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  const String8 word = cmd_split(arg, NULL);
  const int32_t kind = cmd_word_index(cmd_arg_words(CMD_ARG_OBJECT), word);
  if (kind < 0) {
    snprintf(ctx->message, sizeof(ctx->message), "Unknown object '%.*s'",
             (int)word.length, word.str);
    return false_v;
  }
  if (!vkr_editor_request_create(ctx->editor, ctx->frame, (uint32_t)kind,
                                 vkr_editor_create_container(ctx->frame),
                                 NULL)) {
    snprintf(ctx->message, sizeof(ctx->message), "No %s is loaded",
             ctx->frame->scene || ctx->frame->world ? "World" : "scene");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Creating %s",
           vkr_editor_object_kind_label((uint32_t)kind));
  return true_v;
}

/* Rests the selection on what lies below it under the Snapping settings. */
static bool8_t cmd_run_snap(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  (void)arg;
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  return vkr_editor_viewport_snap(ctx->editor, ctx->frame, entity, ctx->message,
                                  sizeof(ctx->message));
}

/* Deletes the named object, or the selection. */
static bool8_t cmd_run_delete(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!name.length && ctx->editor->selection_extra_count) {
    return vkr_editor_selection_apply(ctx->editor, ctx->frame,
                                      VKR_SCENE_EDIT_DELETE, ctx->message,
                                      sizeof(ctx->message));
  }
  if (name.length) {
    entity = cmd_find_any(ctx->frame, name);
  } else if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  const VkrScene *scene = vkr_editor_entity_scene(ctx->frame, entity);
  const char *reason = "No such object";
  if (!entity.u64 || !vkr_scene_edit_can_delete(scene, entity, &reason)) {
    snprintf(ctx->message, sizeof(ctx->message), "%s", reason);
    return false_v;
  }
  *ctx->frame->scene_edit =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_DELETE, .entity = entity};
  const String8 label = vkr_scene_get_name(scene, entity);
  snprintf(ctx->message, sizeof(ctx->message), "Deleting %.*s",
           (int)label.length, label.str);
  return true_v;
}

/* Adds the named object to the selection, or takes it out (ADR-089). */
static bool8_t cmd_run_select_toggle(CmdContext *ctx, const CmdDef *def,
                                     String8 arg) {
  (void)def;
  const VkrEntityId entity = cmd_find_any(ctx->frame, cmd_unquote(arg));
  if (!entity.u64) {
    snprintf(ctx->message, sizeof(ctx->message), "No such object");
    return false_v;
  }
  vkr_editor_selection_toggle(ctx->editor, ctx->frame, entity);
  snprintf(ctx->message, sizeof(ctx->message), "Toggled %.*s", (int)arg.length,
           arg.str);
  return true_v;
}

/* Duplicates the named object, or the selection, beside itself. */
static bool8_t cmd_run_duplicate(CmdContext *ctx, const CmdDef *def,
                                 String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!name.length && ctx->editor->selection_extra_count) {
    return vkr_editor_selection_apply(ctx->editor, ctx->frame,
                                      VKR_SCENE_EDIT_DUPLICATE, ctx->message,
                                      sizeof(ctx->message));
  }
  if (name.length) {
    entity = cmd_find_any(ctx->frame, name);
  } else if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  const VkrScene *scene = vkr_editor_entity_scene(ctx->frame, entity);
  const char *reason = "No such object";
  if (!entity.u64 || !vkr_scene_edit_can_duplicate(scene, entity, &reason)) {
    snprintf(ctx->message, sizeof(ctx->message), "%s", reason);
    return false_v;
  }
  *ctx->frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_DUPLICATE, .entity = entity};
  const String8 label = vkr_scene_get_name(scene, entity);
  snprintf(ctx->message, sizeof(ctx->message), "Duplicating %.*s",
           (int)label.length, label.str);
  return true_v;
}

/* partition.load x0 z0 [x1 z1] and partition.unload [all | x0 z0 [x1 z1]]:
   cells of the open scene's world partition (ADR-086). */
static bool8_t cmd_run_partition(CmdContext *ctx, const CmdDef *def,
                                 String8 arg) {
  const bool8_t unload = strcmp(def->name, "partition.unload") == 0;
  String8 rest = arg;
  String8 word = cmd_split(rest, &rest);
  const int32_t none[4] = {0, 0, 0, 0};
  if (unload && word.length == 3u && MemCompare(word.str, "all", 3u) == 0) {
    return vkr_editor_partition_request(ctx->frame, true_v, true_v, none,
                                        ctx->message, sizeof(ctx->message));
  }
  float64_t values[4];
  uint32_t count = 0u;
  while (word.length && count < 4u && cmd_number(word, &values[count])) {
    count++;
    word = cmd_split(rest, &rest);
  }
  if ((count != 2u && count != 4u) || word.length) {
    snprintf(ctx->message, sizeof(ctx->message), "Usage: %s %s", def->name,
             def->usage);
    return false_v;
  }
  if (count == 2u) {
    values[2] = values[0];
    values[3] = values[1];
  }
  int32_t cells[4];
  for (uint32_t i = 0; i < 4u; ++i) {
    cells[i] = (int32_t)floor(values[i]);
  }
  return vkr_editor_partition_request(ctx->frame, unload, false_v, cells,
                                      ctx->message, sizeof(ctx->message));
}

/* component.add and component.remove on the selection. */
static bool8_t cmd_run_component(CmdContext *ctx, const CmdDef *def,
                                 String8 arg) {
  const bool8_t add = strcmp(def->name, "component.add") == 0;
  const String8 word = cmd_split(arg, NULL);
  const VkrTypeDesc *type = vkr_scene_world_type_named(word);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  /* The physics body lives in the physics draft, not a world component. */
  if (cmd_equals(word, cmd_cstr(vkr_scene_physics_body_type.name))) {
    if (!cmd_selection(ctx, &entity)) {
      return false_v;
    }
    if (!vkr_editor_request_physics_body(ctx->frame, entity, add)) {
      snprintf(ctx->message, sizeof(ctx->message),
               add ? "The selection already has a physics body"
                   : "The selection has no physics body");
      return false_v;
    }
    snprintf(ctx->message, sizeof(ctx->message), "%s physics body",
             add ? "Adding" : "Removing");
    return true_v;
  }
  if (!type || !vkr_scene_world_type_live(type)) {
    snprintf(ctx->message, sizeof(ctx->message), "Unknown component '%.*s'",
             (int)word.length, word.str);
    return false_v;
  }
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  if (add && !vkr_scene_type_allowed(
                 vkr_editor_entity_scene(ctx->frame, entity), type)) {
    snprintf(ctx->message, sizeof(ctx->message), "%s belongs to the World",
             type->label);
    return false_v;
  }
  VkrSceneEditRequest request = {.action =
                                     add ? VKR_SCENE_EDIT_ADD_COMPONENT
                                         : VKR_SCENE_EDIT_REMOVE_COMPONENT,
                                 .entity = entity};
  request.values.component_type = type;
  if (add) {
    vkr_type_defaults(type, request.values.component);
  }
  *ctx->frame->scene_edit = request;
  snprintf(ctx->message, sizeof(ctx->message), "%s %s",
           add ? "Adding" : "Removing", type->label);
  return true_v;
}

/* script.attach <type|none> gives the selection one of the loaded script
   types as its script, the way Details' script slot does (ADR-079). */
static bool8_t cmd_run_script_attach(CmdContext *ctx, const CmdDef *def,
                                     String8 arg) {
  (void)def;
  const String8 word = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  const VkrTypeDesc *type = NULL;
  const String8 none = string8_lit("none");
  if (!string8_equals(&word, &none)) {
    const VkrTypeDesc *types[32];
    const uint32_t count =
        vkr_editor_script_types(ctx->frame, types, ArrayCount(types));
    for (uint32_t i = 0; i < count && !type; ++i) {
      if (strlen(types[i]->name) == word.length &&
          !MemCompare(types[i]->name, word.str, word.length)) {
        type = types[i];
      }
    }
    if (!type) {
      snprintf(ctx->message, sizeof(ctx->message), "No script type '%.*s'",
               (int)word.length, word.str);
      return false_v;
    }
  }
  vkr_editor_request_script(ctx->frame, entity, type);
  snprintf(ctx->message, sizeof(ctx->message), "Script %s",
           type ? type->label : "removed");
  return true_v;
}

/* script.edit opens the selection's script source. */
static bool8_t cmd_run_script_edit(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  (void)def;
  (void)arg;
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  if (!vkr_editor_open_entity_script(ctx->editor, ctx->frame, entity)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "The selection has no script with a project source");
    return false_v;
  }
  snprintf(ctx->message, sizeof(ctx->message), "Opened the script");
  return true_v;
}

/* physics.motion <static|kinematic|dynamic> sets the selection's body
   motion, the way Details' Physics section does (undoable). */
static bool8_t cmd_run_physics_motion(CmdContext *ctx, const CmdDef *def,
                                      String8 arg) {
  (void)def;
  static const struct {
    const char *name;
    VkrPhysicsMotion motion;
  } motions[] = {{"static", VKR_PHYSICS_STATIC},
                 {"kinematic", VKR_PHYSICS_KINEMATIC},
                 {"dynamic", VKR_PHYSICS_DYNAMIC}};
  const String8 word = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  VkrSceneEditValues values;
  if (!vkr_scene_edit_read(vkr_editor_entity_scene(ctx->frame, entity), entity,
                           &values) ||
      !values.physics.present) {
    snprintf(ctx->message, sizeof(ctx->message),
             "The selection has no physics body");
    return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(motions); ++i) {
    if (strlen(motions[i].name) == word.length &&
        !MemCompare(motions[i].name, word.str, word.length)) {
      values.physics.body.motion = motions[i].motion;
      values.fields = VKR_SCENE_EDIT_PHYSICS;
      *ctx->frame->scene_edit = (VkrSceneEditRequest){
          .action = VKR_SCENE_EDIT_APPLY, .entity = entity, .values = values};
      snprintf(ctx->message, sizeof(ctx->message), "Body %s", motions[i].name);
      return true_v;
    }
  }
  snprintf(ctx->message, sizeof(ctx->message),
           "physics.motion needs static, kinematic or dynamic");
  return false_v;
}

/* time.hour <hour> runs the time of day from an hour, and light.group <group>
   <intensity> scales a light group's static lights, both until the
   simulation resets (ADR-090). */
static bool8_t cmd_run_time_of_day(CmdContext *ctx, const CmdDef *def,
                                   String8 arg) {
  VkrSampleTimeOfDayRequest *request = ctx->frame->time_of_day_request;
  if (!request) {
    snprintf(ctx->message, sizeof(ctx->message),
             "The time of day is not available");
    return false_v;
  }
  String8 rest = {0};
  const String8 word = cmd_split(arg, &rest);
  float64_t number = 0.0;
  if (def->value == 0u) {
    if (!cmd_number(word, &number)) {
      snprintf(ctx->message, sizeof(ctx->message),
               "Expected an hour, not '%.*s'", (int)word.length, word.str);
      return false_v;
    }
    request->set_hour = true_v;
    request->hour = number;
    snprintf(ctx->message, sizeof(ctx->message), "Hour %.2f", number);
    return true_v;
  }
  const String8 value = cmd_split(rest, NULL);
  if (word.length >= sizeof(request->group) ||
      !vkr_light_group_name_valid((const char *)word.str, word.length) ||
      !cmd_number(value, &number) || !(number >= 0.0) || !isfinite(number)) {
    snprintf(ctx->message, sizeof(ctx->message),
             "light.group needs a group name and an intensity of 0 or more");
    return false_v;
  }
  request->set_group = true_v;
  MemCopy(request->group, word.str, word.length);
  request->group[word.length] = '\0';
  request->intensity = (float32_t)number;
  snprintf(ctx->message, sizeof(ctx->message), "Light group %s at %.2f",
           request->group[0] ? request->group : VKR_LIGHT_GROUP_DEFAULT,
           number);
  return true_v;
}

/* preset.save <type> saves the selection's component as a preset;
   preset.apply <name> applies a preset to the selection's component of its
   type (ADR-076). */
static bool8_t cmd_run_preset(CmdContext *ctx, const CmdDef *def, String8 arg) {
  const String8 word = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  VkrEditorContent *content = ctx->editor->content;
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  if (!strcmp(def->name, "preset.save")) {
    const VkrTypeDesc *type = vkr_editor_preset_type(word);
    if (!type || !vkr_editor_component_read(ctx->frame, entity, type, value) ||
        !vkr_editor_content_save_preset(content, type, value)) {
      snprintf(ctx->message, sizeof(ctx->message),
               "Cannot save a '%.*s' preset from the selection",
               (int)word.length, word.str);
      return false_v;
    }
    snprintf(ctx->message, sizeof(ctx->message), "Saved a %s preset",
             type->label);
    return true_v;
  }
  for (uint32_t i = 0; i < vkr_editor_content_preset_count(content); ++i) {
    const VkrEditorPreset *preset = vkr_editor_content_preset(content, i);
    if (cmd_equals(word, cmd_cstr(preset->name)) &&
        vkr_editor_component_read(ctx->frame, entity, preset->type, value)) {
      vkr_editor_request_component(ctx->frame, entity, preset->type,
                                   preset->value);
      snprintf(ctx->message, sizeof(ctx->message), "Applying preset %s",
               preset->name);
      return true_v;
    }
  }
  snprintf(ctx->message, sizeof(ctx->message),
           "No preset '%.*s' for a component of the selection",
           (int)word.length, word.str);
  return false_v;
}

/* Moves the selection under the named object, or to the root with `none`,
 * keeping where it is in the world. */
static bool8_t cmd_run_parent(CmdContext *ctx, const CmdDef *def, String8 arg) {
  (void)def;
  const String8 name = cmd_unquote(arg);
  VkrEntityId entity = VKR_ENTITY_ID_INVALID;
  if (!cmd_selection(ctx, &entity)) {
    return false_v;
  }
  VkrEntityId parent = VKR_ENTITY_ID_INVALID;
  if (!cmd_equals(name, cmd_cstr("none"))) {
    /* Parents come from the selection's own container. */
    parent = cmd_find_entity(vkr_editor_entity_scene(ctx->frame, entity), name);
    if (!parent.u64) {
      snprintf(ctx->message, sizeof(ctx->message),
               "No object named '%.*s' in the selection's scene",
               (int)name.length, name.str);
      return false_v;
    }
  }
  *ctx->frame->scene_edit = (VkrSceneEditRequest){
      .action = VKR_SCENE_EDIT_REPARENT, .entity = entity, .parent = parent};
  snprintf(ctx->message, sizeof(ctx->message), "Parenting to %.*s",
           (int)name.length, name.str);
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
#define CMD_SIMPLE_HOLDS(name, help, command)                                  \
  {name, CMD_ARG_NONE, "", help, cmd_run_command, command, 0u, .holds = true_v}

static const CmdDef cmd_defs[] = {
    CMD_SIMPLE_HOLDS("scene.load", "Load the scene", CMD_LOAD),
    CMD_SIMPLE_HOLDS("scene.reload", "Reload the scene from disk", CMD_RELOAD),
    CMD_SIMPLE("scene.unload", "Unload the scene", CMD_UNLOAD),
    CMD_SIMPLE_HOLDS("scene.save", "Save scene edits", CMD_SAVE),
    CMD_SIMPLE("undo", "Undo the last edit", CMD_UNDO),
    CMD_SIMPLE("redo", "Redo the last undone edit", CMD_REDO),
    {"select", CMD_ARG_ENTITY, "<name>",
     "Select an entity by name (exact, else first containing)", cmd_run_select,
     CMD_COUNT, 0u},
    CMD_SIMPLE("frame", "Frame the selection in the Scene", CMD_FRAME),
    {"visibility.toggle", CMD_ARG_NONE, "",
     "Hide or show the selection in the game too (its saved Visibility)",
     cmd_run_visibility, CMD_COUNT, 0u},
    CMD_SIMPLE("hide", "Hide the selection in the editor only (H)", CMD_HIDE),
    CMD_SIMPLE("isolate", "Show only the selection in the editor (Shift+H)",
               CMD_ISOLATE),
    CMD_SIMPLE("unhide", "Show every object hidden in the editor (Alt+H)",
               CMD_REVEAL),
    {"panel", CMD_ARG_PANEL, "<panel> [on|off|toggle]",
     "Show, hide or toggle a docked panel", cmd_run_panel, CMD_COUNT, 0u},
    {"window", CMD_ARG_WINDOW, "<window> [on|off|toggle]",
     "Show, hide or toggle a floating window", cmd_run_panel, CMD_COUNT, 0u},
    {"build.game", CMD_ARG_TEXT, "[profile]",
     "Package the project with its build profile (the selected one by "
     "default)",
     cmd_run_build, CMD_COUNT, 0u, .holds = true_v},
    {"build.run", CMD_ARG_TEXT, "[profile]",
     "Package the project, then run the game", cmd_run_build, CMD_COUNT, 1u,
     .holds = true_v},
    CMD_SIMPLE("build.settings", "Show or hide Build Settings",
               CMD_BUILD_SETTINGS),
    CMD_SIMPLE("build.open", "Open the last package's folder", CMD_BUILD_OPEN),
    CMD_SIMPLE("layout.reset",
               "Restore the active workbench's default panel layout",
               CMD_RESET_LAYOUT),
    CMD_SIMPLE("layout.maximize",
               "Maximize the Scene over the panels and top bar, or restore it "
               "(G)",
               CMD_SCENE_MAXIMIZE),
    {"workbench.duplicate", CMD_ARG_TEXT, "[workbench]",
     "Copy a workbench, or the active one, after it and switch to the copy",
     cmd_run_workbench_edit, CMD_COUNT, 0u},
    {"workbench.delete", CMD_ARG_TEXT, "[workbench]",
     "Delete a custom workbench, or the active one", cmd_run_workbench_edit,
     CMD_COUNT, 1u},
    {"workbench.move", CMD_ARG_TEXT, "<left|right>",
     "Move the active workbench's tab", cmd_run_workbench_edit, CMD_COUNT, 2u},
    {"workbench.rename", CMD_ARG_TEXT, "<name>", "Rename the active workbench",
     cmd_run_workbench_edit, CMD_COUNT, 3u},
    {"workbench", CMD_ARG_TEXT,
     "[general|level_design|terrain|lighting|scripting|1-9|next|prev]",
     "List the workbenches, or switch to one and to the scene it shows",
     cmd_run_workbench, CMD_COUNT, 0u, .holds = true_v},
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
    {"brush.draw", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Draw box brushes by dragging in the Scene (B)", cmd_run_brush_draw,
     CMD_COUNT, 0u},
    {"terrain.tool", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Sculpt and paint terrain in the Scene with the Terrain window's brush",
     cmd_run_brush_draw, CMD_COUNT, 2u},
    {"brush.clip_tool", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Cut the selected brush on its face grid: along a grid line, or "
     "through clicked corners, edges and grid crossings",
     cmd_run_brush_draw, CMD_COUNT, 1u},
    {"brush.stairs_tool", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Build stairs rising 3 m between two grid clicks", cmd_run_brush_draw,
     CMD_COUNT, 3u},
    {"brush.corridor_tool", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Build a corridor between two grid clicks", cmd_run_brush_draw, CMD_COUNT,
     4u},
    {"measure.tool", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Measure distance, run, rise and slope between two clicks in the Scene; "
     "Shift snaps to the grid",
     cmd_run_brush_draw, CMD_COUNT, 5u},
    {"partition.load", CMD_ARG_TEXT, "<x0> <z0> [<x1> <z1>]",
     "Load and pin world partition cells for editing", cmd_run_partition,
     CMD_COUNT, 0u},
    {"partition.unload", CMD_ARG_TEXT, "all | <x0> <z0> [<x1> <z1>]",
     "Unpin world partition cells; those without unsaved or undoable edits "
     "unload",
     cmd_run_partition, CMD_COUNT, 0u},
    {"io.trace", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Log each entity IO delivery during Play as an [io] line",
     cmd_run_io_trace, CMD_COUNT, 0u},
    {"io.fire", CMD_ARG_TEXT, "<object> <input> [value]",
     "Send an input to an object during Play, as a connection would",
     cmd_run_io_fire, CMD_COUNT, 0u},
    {"grid.labels", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Show or hide the grid's cell numbers and letters", cmd_run_view,
     CMD_COUNT, 2u},
    {"view.greybox", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Show every brush face in its surface's greybox look, over any "
     "material",
     cmd_run_view, CMD_COUNT, 3u},
    {"grid.spacing", CMD_ARG_NUMBER, "<units>",
     "Set the grid cell size and show the grid", cmd_run_view, CMD_COUNT, 1u},
    {"grid.height", CMD_ARG_NUMBER, "<y>",
     "Set the grid's world height and show the grid", cmd_run_grid_height,
     CMD_COUNT, 0u},
    {"grid.fit", CMD_ARG_NONE, "",
     "Lift the grid onto the surface at the Scene's centre",
     cmd_run_grid_height, CMD_COUNT, 1u},
    {"time.hour", CMD_ARG_NUMBER, "<hour>",
     "Run the time of day from an hour until the simulation resets; the "
     "World's Time of Day keeps its authored hour",
     cmd_run_time_of_day, CMD_COUNT, 0u},
    {"light.group", CMD_ARG_TEXT, "<group> <intensity>",
     "Scale a light group's static lights until the simulation resets; 0 "
     "switches them off",
     cmd_run_time_of_day, CMD_COUNT, 1u},
    {"labels", CMD_ARG_SWITCH, "[on|off|toggle]", "Show or hide light icons",
     cmd_run_labels, CMD_COUNT, 0u},
    {"labels.directional", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Directional light icons", cmd_run_labels, CMD_COUNT, 1u},
    {"labels.spot", CMD_ARG_SWITCH, "[on|off|toggle]", "Spot light icons",
     cmd_run_labels, CMD_COUNT, 2u},
    {"labels.point", CMD_ARG_SWITCH, "[on|off|toggle]", "Point light icons",
     cmd_run_labels, CMD_COUNT, 3u},
    {"labels.occlusion", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Hide object icons behind collision", cmd_run_labels, CMD_COUNT, 4u},
    {"labels.distance", CMD_ARG_NUMBER, "<metres>",
     "Fade object icons out past a distance from the camera; 0 for no limit",
     cmd_run_labels_distance, CMD_COUNT, 0u},
    {"ui.zoom", CMD_ARG_ZOOM, "<scale|in|out|reset>",
     "Scale the whole interface", cmd_run_zoom, CMD_COUNT, 0u},
    {"ui.reduce_motion", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Turn eased interface motion off or on", cmd_run_motion, CMD_COUNT, 0u},
    {"scene.add", CMD_ARG_TEXT, "<name|path>",
     "Load a project scene or scene file beside the active one",
     cmd_run_scene_add, CMD_COUNT, 0u, .holds = true_v},
    {"scene.remove", CMD_ARG_TEXT, "<slot|name> [discard]",
     "Remove an added scene", cmd_run_scene_remove, CMD_COUNT, 0u},
    {"scene.instantiate", CMD_ARG_TEXT, "<name>",
     "Copy a project scene into the open one as a prefab instance",
     cmd_run_scene_instantiate, CMD_COUNT, 0u, .holds = true_v},
    {"scene.primary", CMD_ARG_TEXT, "<slot|name>",
     "Make an added scene the primary scene", cmd_run_scene_primary, CMD_COUNT,
     0u, .holds = true_v},
    {"create", CMD_ARG_OBJECT, "<object>",
     "Create an object at the Scene's centre, snapped like a drop",
     cmd_run_create, CMD_COUNT, 0u},
    {"snap", CMD_ARG_NONE, "",
     "Rest the selection on the surface, grid or ground below it (End)",
     cmd_run_snap, CMD_COUNT, 0u},
    {"script.new", CMD_ARG_TEXT, "<Name>",
     "Create a script module in Scripts/ and open it", cmd_run_script_new,
     CMD_COUNT, 0u},
    {"script.open", CMD_ARG_TEXT, "<file>",
     "Open a script source in the Script editor", cmd_run_script_open,
     CMD_COUNT, 0u},
    {"script.attach", CMD_ARG_TEXT, "<type|none>",
     "Set the selection's script to a loaded script type, or remove it",
     cmd_run_script_attach, CMD_COUNT, 0u},
    {"script.edit", CMD_ARG_NONE, "", "Open the selection's script source",
     cmd_run_script_edit, CMD_COUNT, 0u},
    {"ui.click", CMD_ARG_TEXT, "<x> <y> [count] [right|alt|ctrl]",
     "Click the window at a point, as the mouse would", cmd_run_ui_click,
     CMD_COUNT, 0u},
    {"ui.key", CMD_ARG_TEXT, "[cmd+|alt+|ctrl+|shift+]<key> [down|up]",
     "Press and release a key with modifiers, as the keyboard would; down or "
     "up only presses or releases it, and shift, ctrl or alt alone are keys",
     cmd_run_ui_key, CMD_COUNT, 0u},
    {"ui.look", CMD_ARG_TEXT, "<dx> <dy>",
     "Move the pointer by points from where it is, as mouse motion would",
     cmd_run_ui_look, CMD_COUNT, 0u},
    {"ui.type", CMD_ARG_TEXT, "<text>",
     "Type characters into the focused field, as the keyboard would",
     cmd_run_ui_type, CMD_COUNT, 0u},
    {"ui.drag", CMD_ARG_TEXT, "<x0> <y0> <x1> <y1>",
     "Drag with the left button between two window points", cmd_run_ui_drag,
     CMD_COUNT, 0u},
    {"script.goto", CMD_ARG_TEXT, "<line>",
     "Move the Script editor's caret to a line", cmd_run_script_goto, CMD_COUNT,
     0u},
    {"script.type", CMD_ARG_TEXT, "<text>",
     "Type text into the Script editor at its caret", cmd_run_script_type,
     CMD_COUNT, 0u},
    {"script.save", CMD_ARG_NONE, "",
     "Save the Script editor's tab, rebuilding and hot reloading its module",
     cmd_run_script_save, CMD_COUNT, 0u},
    {"script.status", CMD_ARG_NONE, "",
     "Report each script module's build and load state", cmd_run_script_status,
     CMD_COUNT, 0u},
    {"delete", CMD_ARG_TEXT, "[name]", "Delete the selection or a named object",
     cmd_run_delete, CMD_COUNT, 0u},
    {"select.toggle", CMD_ARG_ENTITY, "<name>",
     "Add a named object to the selection, or take it out, as Ctrl+click does",
     cmd_run_select_toggle, CMD_COUNT, 0u},
    {"duplicate", CMD_ARG_TEXT, "[name]",
     "Duplicate the selection or a named object beside itself and select the "
     "copy",
     cmd_run_duplicate, CMD_COUNT, 0u},
    {"physics.motion", CMD_ARG_TEXT, "<static|kinematic|dynamic>",
     "Set the selection's physics body motion (undoable)",
     cmd_run_physics_motion, CMD_COUNT, 0u},
    {"component.add", CMD_ARG_COMPONENT, "<type>",
     "Add a component to the selection", cmd_run_component, CMD_COUNT, 0u},
    {"component.remove", CMD_ARG_COMPONENT, "<type>",
     "Remove a component from the selection", cmd_run_component, CMD_COUNT, 0u},
    {"parent", CMD_ARG_TEXT, "<name|none>",
     "Move the selection under an object, keeping its place", cmd_run_parent,
     CMD_COUNT, 0u},
    {"scene.open", CMD_ARG_TEXT, "<name>", "Open a project scene by name",
     cmd_run_scene_open, CMD_COUNT, 0u, .holds = true_v},
    {"scene.import", CMD_ARG_TEXT, "<path>",
     "Open the Create window importing a scene JSON and inspect it",
     cmd_run_scene_import, CMD_COUNT, 0u, .holds = true_v},
    {"scene.create", CMD_ARG_TEXT, "<name>",
     "Create an empty scene in the project and open it", cmd_run_scene_create,
     CMD_COUNT, 0u, .holds = true_v},
    {"scene.bake", CMD_ARG_TEXT, "[lightmaps [samples]]",
     "Bake the open project scene's lighting as Bake lighting does; "
     "lightmaps adds its lightmaps whatever the Bakery option, traced with "
     "samples per texel (1-4096; the baker's default is 16)",
     cmd_run_scene_bake, CMD_COUNT, 0u, .holds = true_v},
    {"scene.inherit", CMD_ARG_SWITCH, "[on|off|toggle]",
     "Whether the open scene uses the World's objects where it has none",
     cmd_run_scene_inherit, CMD_COUNT, 0u},
    {"scene.textures", CMD_ARG_TEXT, "[full|4096|2048|1024]",
     "Largest texture extent the open scene, or the World, loads (undoable)",
     cmd_run_scene_textures, CMD_COUNT, 0u},
    {"content.import", CMD_ARG_TEXT,
     "<path> [world|new <name>|scene <name>|content]",
     "Import a file into the project's shared assets, or place a model in the "
     "World, a new scene or a project scene",
     cmd_run_content_import, CMD_COUNT, 0u, .holds = true_v},
    {"content.search", CMD_ARG_TEXT, "[text]",
     "Search Content below the current folder by name, type, folder or tag",
     cmd_run_content_search, CMD_COUNT, 0u},
    {"content.command", CMD_ARG_TEXT, "<load|open|place|rename|delete> <item>",
     "Run a Content context menu command on an item or folder",
     cmd_run_content_folder, CMD_COUNT, 0u, .holds = true_v},
    {"content.open", CMD_ARG_TEXT, "[folder]",
     "Show a Content folder, such as Props/Bistro; empty shows the root",
     cmd_run_content_folder, CMD_COUNT, 0u},
    {"content.mkdir", CMD_ARG_TEXT, "<folder>",
     "Create a Content folder and its parents", cmd_run_content_folder,
     CMD_COUNT, 0u},
    {"content.move", CMD_ARG_TEXT, "<item|folder> <folder>",
     "Move an item or folder into a Content folder", cmd_run_content_folder,
     CMD_COUNT, 0u},
    {"preset.save", CMD_ARG_TEXT, "<component>",
     "Save the selection's component, such as post_process, as a preset",
     cmd_run_preset, CMD_COUNT, 0u},
    {"preset.apply", CMD_ARG_TEXT, "<name>",
     "Apply a preset to the selection's component of its type", cmd_run_preset,
     CMD_COUNT, 0u},
    {"content.drop", CMD_ARG_TEXT, "<path>",
     "Drop a file on the current Content folder, opening the import window",
     cmd_run_content_folder, CMD_COUNT, 0u},
    {"content.place", CMD_ARG_TEXT, "<item>",
     "Drop a Content item on the viewport centre: a mesh is placed there",
     cmd_run_content_folder, CMD_COUNT, 0u, .holds = true_v},
    {"content.reveal", CMD_ARG_TEXT, "<path>",
     "Select the asset that owns a workspace file, as a Build diagnostic's "
     "Reveal does",
     cmd_run_content_folder, CMD_COUNT, 0u},
    {"content.view", CMD_ARG_TEXT, "<list|tiles>",
     "Show Content as a list with columns or as tiles", cmd_run_content_folder,
     CMD_COUNT, 0u},
    {"help", CMD_ARG_COMMAND, "[command]", "List commands or describe one",
     cmd_run_help, CMD_COUNT, 0u},
    {"echo", CMD_ARG_TEXT, "<text>", "Print text to the Console", cmd_run_echo,
     CMD_COUNT, 0u},
    {"op", CMD_ARG_TEXT, "<operation> [json arguments]",
     "Run an agent operation, such as op brush.box "
     "{\"min\":[0,0,0],\"max\":[2,2,2]}; its result prints as an [agent] "
     "line",
     cmd_run_op, CMD_COUNT, 0u},
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
    cmd_report(ctx->editor, true_v, line);
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
  cmd_report(editor, true_v, echo);
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
    if (ok && def->holds) {
      editor->cmd_holding = true_v;
      editor->cmd_hold_seconds = 0.0;
    }
  }
  if (ctx.message[0]) {
    cmd_report(editor, ok, ctx.message);
    vkr_editor_toast(
        editor, ok ? VKR_UI_ICON_TERMINAL : VKR_UI_ICON_WARNING_FILL,
        ok ? vkr_ui_theme()->accent_hover : vkr_ui_theme()->warning,
        ctx.message);
  }
  return ok;
}

void vkr_editor_cmd_capture_begin(VkrEditorUi *editor, char *buffer,
                                  uint32_t capacity) {
  editor->cmd_capture = buffer;
  editor->cmd_capture_capacity = capacity;
  editor->cmd_capture_length = 0u;
  if (buffer && capacity) {
    buffer[0] = '\0';
  }
}

uint32_t vkr_editor_cmd_capture_end(VkrEditorUi *editor) {
  const uint32_t length = editor->cmd_capture_length;
  editor->cmd_capture = NULL;
  editor->cmd_capture_capacity = 0u;
  editor->cmd_capture_length = 0u;
  return length;
}

bool8_t vkr_editor_cmd_idle(const VkrEditorUi *editor) {
  return editor->cmd_queue_offset >= editor->cmd_queue_length &&
         !editor->cmd_holding && !editor->cmd_holding_op &&
         editor->cmd_wait_seconds <= 0.0 &&
         editor->cmd_wait_scene_seconds <= 0.0 &&
         editor->cmd_pointer_next >= editor->cmd_pointer_count;
}

bool8_t vkr_editor_cmd_enqueue(VkrEditorUi *editor, const char *script) {
  if (!editor || !script)
    return false_v;
  if (editor->cmd_queue_offset >= editor->cmd_queue_length)
    editor->cmd_queue_offset = editor->cmd_queue_length = 0u;
  const size_t length = strlen(script);
  if (editor->cmd_queue_length + length + 2u > sizeof(editor->cmd_queue)) {
    cmd_report(editor, false_v, "Cmd queue is full; dropped a script");
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
        cmd_report(editor, false_v,
                   "wait.scene timed out; dropped the remaining commands");
        editor->cmd_queue_offset = editor->cmd_queue_length = 0u;
      }
      return;
    }
  }
  /* Pointer steps apply in the input callback; the queue waits for them. */
  if (editor->cmd_pointer_next < editor->cmd_pointer_count) {
    return;
  }
  /* The hold's work shows from the next frame: runners start project jobs at
   * once, and the runtime starts requested loads after this build. */
  if (editor->cmd_holding) {
    const bool8_t busy =
        frame->scene_loading || frame->additive_loading ||
        vkr_editor_projects_busy(editor->projects, editor->bakery) ||
        vkr_editor_build_busy(editor->build);
    editor->cmd_hold_seconds += dt;
    if (busy && editor->cmd_hold_seconds < CMD_HOLD_LIMIT_SECONDS) {
      return;
    }
    editor->cmd_holding = false_v;
    if (busy) {
      editor->cmd_holding_build = false_v;
      editor->cmd_holding_bake = false_v;
      cmd_report(editor, false_v,
                 "Job or scene load timed out; dropped the remaining "
                 "commands");
      editor->cmd_queue_offset = editor->cmd_queue_length = 0u;
      return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Settled after %.2f s",
             editor->cmd_hold_seconds);
    cmd_report(editor, true_v, text);
    if (editor->cmd_holding_build) {
      editor->cmd_holding_build = false_v;
      bool8_t succeeded = false_v;
      const char *result = vkr_editor_build_result(editor->build, &succeeded);
      cmd_report(editor, succeeded, result[0] ? result : "Build did not run");
    }
    if (editor->cmd_holding_bake) {
      editor->cmd_holding_bake = false_v;
      bool8_t succeeded = false_v;
      const char *result =
          vkr_editor_projects_take_bake_result(editor->projects, &succeeded);
      /* A bake that succeeded reopens its scene; a failed reopen only sets
         the Projects status line. */
      if (succeeded && !frame->scene) {
        char reopen[640];
        snprintf(reopen, sizeof(reopen), "%s, but the scene did not reopen: %s",
                 result, vkr_editor_projects_message(editor->projects));
        cmd_report(editor, false_v, reopen);
      } else {
        cmd_report(editor, succeeded, result);
      }
    }
  }
  if (editor->cmd_holding_op) {
    editor->cmd_hold_seconds += dt;
    if (vkr_editor_agent_self_pending(editor->agent) &&
        editor->cmd_hold_seconds < CMD_HOLD_LIMIT_SECONDS) {
      return;
    }
    editor->cmd_holding_op = false_v;
  }
  /* Nobody can close a headless editor, so the end of its script does. The
   * quit request skips the unsaved-edits check that `quit` makes. */
  if (editor->cmd_quit_when_done &&
      editor->cmd_queue_offset >= editor->cmd_queue_length &&
      !vkr_editor_agent_busy(editor->agent) && frame->quit_request) {
    const bool8_t unsaved =
        frame->scene && frame->edits &&
        frame->edits->revision != frame->edits->saved_revision;
    cmd_report(editor, true_v,
               unsaved ? "Script finished; quitting and discarding "
                         "unsaved scene edits"
                       : "Script finished; quitting");
    editor->cmd_quit_when_done = false_v;
    *frame->quit_request = true_v;
    return;
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
